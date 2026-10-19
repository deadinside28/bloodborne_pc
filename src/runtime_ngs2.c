/* NGS2, the voice graph many games mix their sound with (first: another game).
 *
 * The game builds racks of voices and patches them: sampler voices play waveform blocks it queues
 * (PCM, VAG ADPCM or ATRAC9), submixers and reverbs take their inputs, mastering voices write
 * the buffers sceNgs2SystemRender fills; the game sends those to sceAudioOut itself. Rendering
 * runs on the caller's thread, one grain (at most 256 frames) at a time:
 *   samplers -> reverbs (their wet signal is not made yet: silence) -> submixers -> mastering.
 * A sampler resamples its rate times its pitch to the system rate (48 kHz) linearly, and sends
 * each port through the port's matrix (or channel to channel) times the port's volume to the
 * patched voice's input.
 *
 * Streaming games poll the sampler's state (decoded samples and bytes, the block in play) and add
 * blocks as earlier ones finish; a sampler whose blocks run out stops, as on the console.
 * Envelopes, filters, distortion, peak meters and the mastering limiter are not modelled; their
 * parameters are accepted. BB_NGS2_TRACE=1 prints the graph and parameters as they come. */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define NGS2_INVALID_HANDLE ((int32_t)0x804A0230)
#define NGS2_INVALID_ARG ((int32_t)0x804A0001)
#define NGS2_NO_MEMORY ((int32_t)0x804A0050)
#define SYSTEM_RATE 48000u
#define GRAIN 256u
#define MAX_CH 8u
#define MAX_PORTS 4u
#define MAX_MATRICES 4u
#define MAX_BLOCKS 32u
#define CACHE_FRAMES 2048u

enum { MAGIC_SYSTEM = 0x4e475331, MAGIC_RACK = 0x4e475332, MAGIC_VOICE = 0x4e475333 };
enum { KIND_SAMPLER = 1, KIND_SUBMIXER = 2, KIND_MASTERING = 3, KIND_REVERB = 4 };
enum { STATE_INUSE = 0x1, STATE_PLAYING = 0x2, STATE_PAUSED = 0x4, STATE_STOPPED = 0x8,
       STATE_ERROR = 0x10, STATE_EMPTY = 0x20 };
enum { EVENT_PLAY, EVENT_STOP, EVENT_STOP_IMM, EVENT_KILL, EVENT_PAUSE, EVENT_RESUME };
enum { WAVE_I8 = 0x10, WAVE_U8, WAVE_I16L, WAVE_I16B, WAVE_I24L, WAVE_I24B, WAVE_I32L, WAVE_I32B,
       WAVE_F32L, WAVE_F32B, WAVE_F64L, WAVE_F64B, WAVE_VAG, WAVE_ATRAC9 = 0x40 };
enum { PARAM_MATRIX_LEVELS = 1, PARAM_PORT_VOLUME, PARAM_PORT_MATRIX, PARAM_PORT_DELAY,
       PARAM_PATCH, PARAM_EVENT, PARAM_CALLBACK };
enum { SAMPLER_SETUP = 0x10000000, SAMPLER_BLOCKS, SAMPLER_ADDRESS, SAMPLER_FRAME_OFFSET,
       SAMPLER_EXIT_LOOP, SAMPLER_PITCH };

typedef struct { uint16_t size; int16_t next; uint32_t id; } ParamHeader;
typedef struct {
    uint32_t data_offset, data_size, repeats, skip_samples, samples, reserved;
    uint64_t user_data;
} WaveBlock;
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } RenderBuffer;
typedef struct { void *buffer; uint64_t size; uint64_t reserved[5]; uint64_t user_data; } BufferInfo;

typedef struct {
    const uint8_t *data;
    uint32_t size, repeats, skip, samples;
    uint64_t user_data;
} Block;

struct Rack;
typedef struct Voice {
    uint32_t magic, index;
    struct Rack *rack;
    uint32_t state;
    uint32_t channels;   ///< sampler: the waveform's; others: input and output channels
    struct { int32_t matrix; float volume; struct Voice *dest; } port[MAX_PORTS];
    float matrix[MAX_MATRICES][MAX_CH * MAX_CH];
    uint32_t matrix_levels[MAX_MATRICES];
    float *input;        ///< GRAIN x MAX_CH, for voices others send to
    int input_used;      ///< something was sent this grain
    /* Sampler. */
    uint32_t waveform, rate, config;
    Block blocks[MAX_BLOCKS];
    uint32_t head, count;
    uint32_t offset;     ///< bytes of the current block consumed
    uint32_t emitted;    ///< frames of the current block played
    uint32_t skip_left, repeats_left;
    float pitch;
    uint64_t decoded_samples, decoded_bytes;
    float cache[CACHE_FRAMES * MAX_CH];
    uint32_t cache_frames, cache_pos;
    float prev[MAX_CH], next[MAX_CH];
    double phase;        ///< position between prev (0) and next (1)
    int primed;
    int32_t vag_hist[MAX_CH][2];
    void *atrac9;
    Atrac9CodecInfo atrac9_info;
} Voice;

typedef struct Rack {
    uint32_t magic, id, kind, num_voices;
    struct System *system;
    Voice *voices;
    struct Rack *next;
} Rack;

typedef struct System {
    uint32_t magic, rate;
    Rack *racks;
} System;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static size_t renders, blocks_added, blocks_played, starved;
/* BB_NGS2_TRACE: every 10 s, what may be heard as clicks: queues restarted by a blocks parameter
 * (and how many blocks that dropped), samplers that ran dry, the output's peak and limited frames. */
static size_t window_restarts, window_dropped, window_starved, window_limited, window_renders;
static float window_peak;

static int trace(void) {
    static int on = -1;
    if (on < 0) { const char *env = getenv("BB_NGS2_TRACE"); on = env && env[0] == '1'; }
    return on;
}

static uint32_t rack_kind(uint32_t id) {
    switch (id >> 12) {
    case 1: return KIND_SAMPLER;
    case 3: return KIND_MASTERING;
    case 4: return KIND_REVERB;
    default: return KIND_SUBMIXER; /* 0x2000 submixer, custom racks: pass their input on */
    }
}
static Voice *voice_of(void *handle) {
    Voice *v = handle;
    return v && v->magic == MAGIC_VOICE ? v : NULL;
}

/* ---- Waveform decoding ----------------------------------------------------------------------- */

static uint32_t sample_bytes(uint32_t waveform) {
    switch (waveform) {
    case WAVE_I8: case WAVE_U8: return 1;
    case WAVE_I16L: case WAVE_I16B: return 2;
    case WAVE_I24L: case WAVE_I24B: return 3;
    case WAVE_I32L: case WAVE_I32B: case WAVE_F32L: case WAVE_F32B: return 4;
    case WAVE_F64L: case WAVE_F64B: return 8;
    default: return 0;
    }
}
static float pcm_sample(const uint8_t *p, uint32_t waveform) {
    switch (waveform) {
    case WAVE_I8: return (int8_t)p[0] / 128.0f;
    case WAVE_U8: return (p[0] - 128) / 128.0f;
    case WAVE_I16L: return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    case WAVE_I16B: return (int16_t)(p[1] | p[0] << 8) / 32768.0f;
    case WAVE_I24L: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
    case WAVE_I24B: return (int32_t)((uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24) / 2147483648.0f;
    case WAVE_I32L: { int32_t s; memcpy(&s, p, 4); return s / 2147483648.0f; }
    case WAVE_I32B: return (int32_t)((uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24) / 2147483648.0f;
    case WAVE_F32L: { float f; memcpy(&f, p, 4); return f; }
    case WAVE_F32B: { uint32_t u = (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24; float f; memcpy(&f, &u, 4); return f; }
    case WAVE_F64L: { double d; memcpy(&d, p, 8); return (float)d; }
    case WAVE_F64B: { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = p[7 - i]; double d; memcpy(&d, b, 8); return (float)d; }
    default: return 0.0f;
    }
}

/* PlayStation ADPCM: 16-byte frames of 28 samples per channel, channels one frame after another. */
static uint32_t decode_vag(Voice *v, const uint8_t *src, uint32_t avail) {
    const uint32_t ch = v->channels, unit = 16 * ch;
    if (avail < unit) return 0;
    static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
    for (uint32_t c = 0; c < ch; ++c) {
        const uint8_t *frame = src + 16 * c;
        int shift = frame[0] & 15, filter = frame[0] >> 4;
        if (shift > 12) shift = 9;
        if (filter > 4) filter = 0;
        int32_t *h = v->vag_hist[c];
        for (int i = 0; i < 28; ++i) {
            int nibble = (frame[2 + i / 2] >> ((i & 1) * 4)) & 15;
            int32_t s = (int32_t)((uint32_t)(nibble << 12) << 16) >> 16; /* sign-extended, x4096 */
            s = (s >> shift) + ((h[0] * f0[filter] + h[1] * f1[filter] + 32) >> 6);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            h[1] = h[0]; h[0] = s;
            v->cache[i * ch + c] = s / 32768.0f;
        }
    }
    v->cache_frames = 28;
    return unit;
}

static uint32_t decode_atrac9(Voice *v, const uint8_t *src, uint32_t avail) {
    if (!v->atrac9) {
        uint8_t config[4];
        memcpy(config, &v->config, 4);
        v->atrac9 = Atrac9GetHandle();
        if (!v->atrac9 || Atrac9InitDecoder(v->atrac9, config) ||
            Atrac9GetCodecInfo(v->atrac9, &v->atrac9_info)) {
            if (v->atrac9) Atrac9ReleaseHandle(v->atrac9);
            v->atrac9 = NULL;
            return 0;
        }
    }
    const Atrac9CodecInfo *info = &v->atrac9_info;
    const uint32_t ch = (uint32_t)info->channels, frames = (uint32_t)info->frameSamples;
    if (!avail || ch > MAX_CH || frames * ch > CACHE_FRAMES * MAX_CH) return 0;
    short pcm[CACHE_FRAMES * MAX_CH];
    int used = 0;
    if (Atrac9Decode(v->atrac9, src, pcm, &used) || used <= 0 || (uint32_t)used > avail) return 0;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < v->channels; ++c)
            v->cache[i * v->channels + c] = c < ch ? pcm[i * ch + c] / 32768.0f : 0.0f;
    v->cache_frames = frames;
    return (uint32_t)used;
}

static void reset_decoder(Voice *v) {
    memset(v->vag_hist, 0, sizeof(v->vag_hist));
    if (v->atrac9) { Atrac9ReleaseHandle(v->atrac9); v->atrac9 = NULL; }
}

/* Ends the block in play: repeats it while it has repeats left, else moves to the next one. */
static void finish_block(Voice *v) {
    Block *b = &v->blocks[v->head];
    if (v->repeats_left) {
        --v->repeats_left;
        v->offset = 0; v->emitted = 0; v->skip_left = b->skip;
        return;
    }
    ++blocks_played;
    v->head = (v->head + 1) % MAX_BLOCKS;
    --v->count;
    v->offset = 0; v->emitted = 0;
    if (v->count) {
        v->skip_left = v->blocks[v->head].skip;
        v->repeats_left = v->blocks[v->head].repeats;
    }
}

/* Decodes the next piece of the queued blocks into the cache; 0 when nothing is left. */
static int refill(Voice *v) {
    v->cache_frames = v->cache_pos = 0;
    while (v->count) {
        Block *b = &v->blocks[v->head];
        const uint32_t limit = b->samples && b->samples != UINT32_MAX ? b->samples : UINT32_MAX;
        const uint32_t avail = b->size > v->offset ? b->size - v->offset : 0;
        const uint8_t *src = b->data + v->offset;
        uint32_t used = 0;
        if (v->emitted < limit && avail) {
            if (v->waveform == WAVE_VAG) used = decode_vag(v, src, avail);
            else if (v->waveform == WAVE_ATRAC9) used = decode_atrac9(v, src, avail);
            else {
                const uint32_t frame = sample_bytes(v->waveform) * v->channels;
                uint32_t n = frame ? avail / frame : 0;
                if (n > CACHE_FRAMES) n = CACHE_FRAMES;
                for (uint32_t i = 0; i < n * v->channels; ++i)
                    v->cache[i] = pcm_sample(src + i * sample_bytes(v->waveform), v->waveform);
                v->cache_frames = n;
                used = n * frame;
            }
        }
        if (!used) { finish_block(v); continue; }
        v->offset += used;
        v->decoded_bytes += used;
        if (v->skip_left) {
            const uint32_t s = v->skip_left < v->cache_frames ? v->skip_left : v->cache_frames;
            v->skip_left -= s;
            v->cache_pos = s;
        }
        if (v->emitted + (v->cache_frames - v->cache_pos) > limit)
            v->cache_frames = v->cache_pos + (limit - v->emitted);
        v->emitted += v->cache_frames - v->cache_pos;
        if (v->cache_pos < v->cache_frames) return 1;
        v->cache_frames = v->cache_pos = 0;
    }
    return 0;
}
static int next_frame(Voice *v, float *out) {
    if (v->cache_pos >= v->cache_frames && !refill(v)) return 0;
    memcpy(out, &v->cache[v->cache_pos * v->channels], v->channels * sizeof(float));
    ++v->cache_pos;
    ++v->decoded_samples;
    return 1;
}

/* ---- Mixing ---------------------------------------------------------------------------------- */

static float *input_of(Voice *v) {
    if (!v->input) v->input = calloc(GRAIN * MAX_CH, sizeof(float));
    return v->input;
}
/* Sends frames x channels to the voices the ports are patched to. The matrix of a port holds
 * level[output channel * input channels + input channel] (seen: 6 -> 8 channels with the surround
 * pair also sent to the back pair); without one, channel i goes to i and
 * mono goes to both front channels. */
static void send(Voice *v, const float *src, uint32_t frames, uint32_t channels) {
    for (uint32_t p = 0; p < MAX_PORTS; ++p) {
        Voice *d = v->port[p].dest;
        const float volume = v->port[p].volume;
        if (!d || volume == 0.0f) continue;
        float *dst = input_of(d);
        if (!dst) continue;
        const uint32_t out = d->channels ? d->channels : 2;
        const int32_t m = v->port[p].matrix;
        const float *levels = m >= 0 && m < (int32_t)MAX_MATRICES && v->matrix_levels[m] >= channels * out
                                  ? v->matrix[m] : NULL;
        for (uint32_t f = 0; f < frames; ++f) {
            const float *s = src + f * channels;
            float *o = dst + f * MAX_CH;
            if (levels) {
                for (uint32_t i = 0; i < channels; ++i)
                    for (uint32_t c = 0; c < out; ++c) o[c] += s[i] * levels[c * channels + i] * volume;
            } else if (channels == 1) {
                o[0] += s[0] * volume;
                if (out > 1) o[1] += s[0] * volume;
            } else {
                for (uint32_t c = 0; c < channels && c < out; ++c) o[c] += s[c] * volume;
            }
        }
        d->input_used = 1;
    }
}

static void render_sampler(Voice *v, uint32_t frames, uint32_t system_rate) {
    if (!(v->state & STATE_PLAYING) || (v->state & STATE_PAUSED) || !v->channels) return;
    float out[GRAIN * MAX_CH];
    const double step = (double)v->rate / system_rate * (v->pitch > 0.0f ? v->pitch : 1.0f);
    const uint32_t ch = v->channels;
    uint32_t f = 0;
    int ended = 0;
    if (!v->primed) {
        if (!next_frame(v, v->prev) || !next_frame(v, v->next)) ended = 1;
        v->phase = 0.0; v->primed = 1;
    }
    for (; f < frames && !ended; ++f) {
        for (uint32_t c = 0; c < ch; ++c)
            out[f * ch + c] = v->prev[c] + (float)v->phase * (v->next[c] - v->prev[c]);
        v->phase += step;
        while (v->phase >= 1.0) {
            v->phase -= 1.0;
            memcpy(v->prev, v->next, ch * sizeof(float));
            if (!next_frame(v, v->next)) { ended = 1; ++f; break; }
        }
    }
    if (f) send(v, out, f, ch);
    if (ended) {
        /* Out of blocks: the voice stops, as when the last block of a sound ends. */
        if (trace()) printf("NGS2 trace: sampler %u ran out of blocks (%llu samples played)\n", v->index,
                            (unsigned long long)v->decoded_samples);
        ++starved;
        ++window_starved;
        v->state = 0;
        v->primed = 0;
    }
}

static void render_passthrough(Voice *v, uint32_t frames) {
    if (!v->input_used) return;
    if (v->state & STATE_PLAYING) {
        float out[GRAIN * MAX_CH];
        const uint32_t ch = v->channels ? v->channels : MAX_CH;
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t c = 0; c < ch; ++c) out[f * ch + c] = v->input[f * MAX_CH + c];
        send(v, out, frames, ch);
    }
}

/* The mastering voice's limiter: a frame whose loudest channel passes the threshold is scaled
 * down by a gain that drops at once and recovers over ~50 ms, so loud mixes do not clip (hard
 * clipping is heard as crackle). */
static float limiter_gain = 1.0f;
static void write_output(const RenderBuffer *b, uint64_t first, const float *mix, uint32_t frames) {
    const uint32_t ch = b->channels ? b->channels : 2, bytes = sample_bytes(b->waveform);
    uint8_t *dst = (uint8_t *)b->buffer + first * ch * bytes;
    const float threshold = 0.95f, recover = 1.0f / (0.05f * SYSTEM_RATE);
    for (uint32_t f = 0; f < frames; ++f) {
        float peak = 0.0f;
        for (uint32_t c = 0; c < ch && c < MAX_CH; ++c) peak = fmaxf(peak, fabsf(mix[f * MAX_CH + c]));
        if (peak > window_peak) window_peak = peak;
        if (peak * limiter_gain > threshold) { limiter_gain = threshold / peak; ++window_limited; }
        else if (limiter_gain < 1.0f) limiter_gain = fminf(1.0f, limiter_gain + recover);
        for (uint32_t c = 0; c < ch; ++c) {
            float s = c < MAX_CH ? mix[f * MAX_CH + c] * limiter_gain : 0.0f;
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            if (b->waveform == WAVE_F32L) { memcpy(dst, &s, 4); dst += 4; continue; }
            if (b->waveform == WAVE_I16L) { int16_t i = (int16_t)lrintf(s * 32767.0f); memcpy(dst, &i, 2); dst += 2; }
            else { memset(dst, 0, bytes); dst += bytes; }
        }
    }
}

static void render_grain(System *s, const RenderBuffer *buffers, uint32_t count, uint64_t first, uint32_t frames) {
    for (Rack *r = s->racks; r; r = r->next)
        for (uint32_t i = 0; i < r->num_voices; ++i) {
            Voice *v = &r->voices[i];
            if (v->input && v->input_used) memset(v->input, 0, GRAIN * MAX_CH * sizeof(float));
            v->input_used = 0;
        }
    static const uint32_t order[] = {KIND_SAMPLER, KIND_REVERB, KIND_SUBMIXER};
    for (uint32_t k = 0; k < sizeof(order) / sizeof(*order); ++k)
        for (Rack *r = s->racks; r; r = r->next) {
            if (r->kind != order[k]) continue;
            for (uint32_t i = 0; i < r->num_voices; ++i) {
                Voice *v = &r->voices[i];
                if (r->kind == KIND_SAMPLER) render_sampler(v, frames, s->rate);
                else if (r->kind == KIND_SUBMIXER) render_passthrough(v, frames);
                /* Reverb: no wet signal yet. */
            }
        }
    static const float silence[GRAIN * MAX_CH];
    int written[16] = {0};
    for (Rack *r = s->racks; r; r = r->next) {
        if (r->kind != KIND_MASTERING) continue;
        for (uint32_t i = 0; i < r->num_voices; ++i) {
            Voice *v = &r->voices[i];
            const uint32_t target = i < count ? i : 0;
            if (target >= 16 || !buffers[target].buffer) continue;
            const int live = (v->state & STATE_PLAYING) && v->input_used;
            if (live || !written[target]) {
                write_output(&buffers[target], first, live ? v->input : silence, frames);
                written[target] = 1;
            }
        }
    }
    for (uint32_t b = 0; b < count && b < 16; ++b)
        if (!written[b] && buffers[b].buffer) write_output(&buffers[b], first, silence, frames);
}

/* ---- Parameters -------------------------------------------------------------------------- */

static void sampler_setup(Voice *v, const uint8_t *p) {
    uint32_t format[6];
    memcpy(format, p + 8, sizeof(format));
    reset_decoder(v);
    v->waveform = format[0];
    v->channels = format[1] <= MAX_CH ? format[1] : MAX_CH;
    v->rate = format[2] ? format[2] : SYSTEM_RATE;
    v->config = format[3];
    v->head = v->count = 0;
    v->offset = v->emitted = v->skip_left = v->repeats_left = 0;
    v->cache_frames = v->cache_pos = 0;
    v->decoded_samples = v->decoded_bytes = 0;
    v->pitch = 1.0f;
    v->primed = 0;
    v->state = 0;
    if (trace())
        printf("NGS2 trace: sampler %u setup: waveform %#x, %u channels, %u Hz, config %#x\n", v->index,
               v->waveform, format[1], v->rate, v->config);
}

/* Blocks param: data, flags, count, blocks. Flag bit 1 continues the queue; without it the queue
 * starts over (the game's first block of a sound). */
static void sampler_blocks(Voice *v, const uint8_t *p) {
    const uint8_t *data;
    uint32_t flags, n;
    const WaveBlock *blocks;
    memcpy(&data, p + 8, 8); memcpy(&flags, p + 16, 4); memcpy(&n, p + 20, 4); memcpy(&blocks, p + 24, 8);
    if (trace()) {
        static int printed;
        if (printed++ < 200)
            printf("NGS2 trace: sampler %u blocks: flags %#x, %u block(s) of %u bytes, %u queued, %llu samples played\n",
                   v->index, flags, n, blocks && n ? blocks[0].data_size : 0, v->count,
                   (unsigned long long)v->decoded_samples);
    }
    if (!(flags & 2)) {
        if (v->count) { ++window_restarts; window_dropped += v->count; }
        v->head = v->count = 0;
        v->offset = v->emitted = 0;
        v->cache_frames = v->cache_pos = 0;
        reset_decoder(v);
    }
    for (uint32_t i = 0; blocks && data && i < n; ++i) {
        if (v->count == MAX_BLOCKS) { printf("Runtime: NGS2 sampler %u: block queue full\n", v->index); break; }
        const int first = v->count == 0;
        Block *b = &v->blocks[(v->head + v->count++) % MAX_BLOCKS];
        b->data = data + blocks[i].data_offset;
        b->size = blocks[i].data_size;
        b->repeats = blocks[i].repeats;
        b->skip = blocks[i].skip_samples;
        b->samples = blocks[i].samples;
        b->user_data = blocks[i].user_data;
        if (first) { v->skip_left = b->skip; v->repeats_left = b->repeats; v->offset = v->emitted = 0; }
        ++blocks_added;
    }
}

static void voice_event(Voice *v, uint32_t event) {
    switch (event) {
    case EVENT_PLAY:
        v->state = STATE_INUSE | STATE_PLAYING;
        v->primed = 0;
        break;
    case EVENT_STOP: case EVENT_STOP_IMM: case EVENT_KILL:
        v->state = 0;
        v->primed = 0;
        if (event != EVENT_STOP) { v->head = v->count = 0; v->cache_frames = v->cache_pos = 0; }
        break;
    case EVENT_PAUSE: if (v->state & STATE_PLAYING) v->state |= STATE_PAUSED; break;
    case EVENT_RESUME: v->state &= ~(uint32_t)STATE_PAUSED; break;
    default: break;
    }
}

static void voice_param(Voice *v, const ParamHeader *h) {
    const uint8_t *p = (const uint8_t *)h;
    uint32_t a = 0, b = 0;
    if (h->size >= 12) memcpy(&a, p + 8, 4);
    if (h->size >= 16) memcpy(&b, p + 12, 4);
    switch (h->id) {
    case PARAM_MATRIX_LEVELS: {
        const float *levels;
        memcpy(&levels, p + 16, 8);
        if (a < MAX_MATRICES && levels) {
            const uint32_t n = b < MAX_CH * MAX_CH ? b : MAX_CH * MAX_CH;
            memcpy(v->matrix[a], levels, n * sizeof(float));
            v->matrix_levels[a] = n;
            if (trace()) {
                static int printed;
                if (printed++ < 4) {
                    printf("NGS2 trace: voice %u matrix %u:", v->index, a);
                    for (uint32_t i = 0; i < n; ++i) printf(" %.2f", levels[i]);
                    putchar('\n');
                }
            }
        }
        break;
    }
    case PARAM_PORT_VOLUME: if (a < MAX_PORTS) memcpy(&v->port[a].volume, p + 12, 4); break;
    case PARAM_PORT_MATRIX: if (a < MAX_PORTS) v->port[a].matrix = (int32_t)b; break;
    case PARAM_PATCH: {
        void *dest;
        memcpy(&dest, p + 16, 8);
        if (a < MAX_PORTS) v->port[a].dest = voice_of(dest);
        break;
    }
    case PARAM_EVENT: voice_event(v, a); break;
    default:
        if (v->rack->kind == KIND_SAMPLER) {
            if (h->id == SAMPLER_SETUP) sampler_setup(v, p);
            else if (h->id == SAMPLER_BLOCKS) sampler_blocks(v, p);
            else if (h->id == SAMPLER_EXIT_LOOP) v->repeats_left = 0;
            else if (h->id == SAMPLER_PITCH) memcpy(&v->pitch, p + 8, 4);
        } else if ((h->id & 0xffff) == 0 && h->size >= 12) {
            /* Setup of a submixer, mastering or reverb voice: its channel count first. */
            v->channels = a && a <= MAX_CH ? a : MAX_CH;
        }
        break;
    }
}

/* ---- Entry points ------------------------------------------------------------------------ */

static ABI int32_t query_size(const void *option, BufferInfo *info) {
    (void)option;
    if (!info) return NGS2_INVALID_ARG;
    info->size = 4096; /* the game's buffer is not used: state lives here */
    return 0;
}
static ABI int32_t rack_query_size(uint32_t rack_id, const void *option, BufferInfo *info) {
    (void)rack_id;
    return query_size(option, info);
}
static ABI int32_t system_create(const void *option, const void *buffer, System **out) {
    (void)option; (void)buffer;
    if (!out) return NGS2_INVALID_ARG;
    System *s = calloc(1, sizeof(*s));
    if (!s) return NGS2_NO_MEMORY;
    s->magic = MAGIC_SYSTEM;
    s->rate = SYSTEM_RATE;
    *out = s;
    printf("Runtime: NGS2 system created (mixing samplers to %u Hz)\n", s->rate);
    return 0;
}
static ABI int32_t system_destroy(System *s, void *context) {
    (void)context;
    if (!s || s->magic != MAGIC_SYSTEM) return NGS2_INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    s->magic = 0; /* racks stay: their handles may still be used while the game tears down */
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Racks: the option's voice count (offset 32 of the common rack option) when one is given. */
static ABI int32_t rack_create(System *s, uint32_t rack_id, const void *option, const void *buffer, Rack **out) {
    (void)buffer;
    if (!s || s->magic != MAGIC_SYSTEM) return NGS2_INVALID_HANDLE;
    if (!out) return NGS2_INVALID_ARG;
    uint32_t voices = 0;
    if (option) memcpy(&voices, (const uint8_t *)option + 32, 4);
    if (!voices || voices > 1024) voices = 256;
    Rack *r = calloc(1, sizeof(*r));
    Voice *v = r ? calloc(voices, sizeof(*v)) : NULL;
    if (!v) { free(r); return NGS2_NO_MEMORY; }
    r->magic = MAGIC_RACK; r->id = rack_id; r->kind = rack_kind(rack_id); r->num_voices = voices;
    r->system = s; r->voices = v;
    for (uint32_t i = 0; i < voices; ++i) {
        v[i].magic = MAGIC_VOICE; v[i].index = i; v[i].rack = r; v[i].pitch = 1.0f;
        for (uint32_t p = 0; p < MAX_PORTS; ++p) { v[i].port[p].matrix = -1; v[i].port[p].volume = 1.0f; }
    }
    pthread_mutex_lock(&lock);
    Rack **tail = &s->racks;
    while (*tail) tail = &(*tail)->next;
    *tail = r;
    pthread_mutex_unlock(&lock);
    if (trace()) printf("NGS2 trace: rack %#x created with %u voices\n", rack_id, voices);
    *out = r;
    return 0;
}
static ABI int32_t rack_destroy(Rack *r, void *context) {
    (void)context;
    if (!r || r->magic != MAGIC_RACK) return NGS2_INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    Rack **link = &r->system->racks;
    while (*link && *link != r) link = &(*link)->next;
    if (*link) *link = r->next;
    /* Voices that patch into this rack lose their destination. */
    for (Rack *o = r->system->racks; o; o = o->next)
        for (uint32_t i = 0; i < o->num_voices; ++i)
            for (uint32_t p = 0; p < MAX_PORTS; ++p)
                if (o->voices[i].port[p].dest && o->voices[i].port[p].dest->rack == r) o->voices[i].port[p].dest = NULL;
    for (uint32_t i = 0; i < r->num_voices; ++i) {
        reset_decoder(&r->voices[i]);
        free(r->voices[i].input);
        r->voices[i].magic = 0;
    }
    r->magic = 0;
    pthread_mutex_unlock(&lock);
    free(r->voices);
    free(r);
    return 0;
}
static ABI int32_t voice_handle(Rack *r, uint32_t index, Voice **out) {
    if (!r || r->magic != MAGIC_RACK) return NGS2_INVALID_HANDLE;
    if (!out || index >= r->num_voices) return NGS2_INVALID_ARG;
    *out = &r->voices[index];
    if (trace()) printf("NGS2 trace: rack %#x voice %u = %p\n", r->id, index, (void *)*out);
    return 0;
}
static ABI int32_t voice_control(void *handle, const ParamHeader *params) {
    Voice *v = voice_of(handle);
    if (!v) return NGS2_INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    const ParamHeader *h = params;
    for (int n = 0; h && n < 256; ++n) {
        if (trace() && h->id != PARAM_MATRIX_LEVELS && h->id != SAMPLER_PITCH)
            printf("NGS2 trace: rack %#x voice %u param %#x\n", v->rack->id, v->index, h->id);
        voice_param(v, h);
        if (h->next == 0) break;
        h = (const ParamHeader *)((const uint8_t *)h + h->next);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Sampler state: flags, envelope height, peak height, reserved, decoded samples, decoded bytes,
 * the block in play's user data and its data. Other voices: the flags. */
static ABI int32_t voice_state(void *handle, void *state, uint64_t size) {
    Voice *v = voice_of(handle);
    if (!v) return NGS2_INVALID_HANDLE;
    if (!state || size < 4) return NGS2_INVALID_ARG;
    uint8_t out[48] = {0};
    pthread_mutex_lock(&lock);
    uint32_t flags = v->state;
    if (v->rack->kind == KIND_SAMPLER && (flags & STATE_INUSE) && !v->count) flags |= STATE_EMPTY;
    memcpy(out, &flags, 4);
    if (v->rack->kind == KIND_SAMPLER) {
        const float one = 1.0f;
        memcpy(out + 4, &one, 4);
        memcpy(out + 16, &v->decoded_samples, 8);
        memcpy(out + 24, &v->decoded_bytes, 8);
        if (v->count) {
            const Block *b = &v->blocks[v->head];
            memcpy(out + 32, &b->user_data, 8);
            memcpy(out + 40, &b->data, 8);
        }
    }
    pthread_mutex_unlock(&lock);
    memcpy(state, out, size < sizeof(out) ? size : sizeof(out));
    if (size > sizeof(out)) memset((uint8_t *)state + sizeof(out), 0, size - sizeof(out));
    return 0;
}
static ABI int32_t system_render(System *s, const RenderBuffer *buffers, uint32_t count) {
    if (!s || s->magic != MAGIC_SYSTEM) return NGS2_INVALID_HANDLE;
    if (!buffers || !count) return NGS2_INVALID_ARG;
    uint64_t frames = UINT64_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t bytes = sample_bytes(buffers[i].waveform) * (buffers[i].channels ? buffers[i].channels : 2);
        if (!bytes) return NGS2_INVALID_ARG;
        if (buffers[i].size / bytes < frames) frames = buffers[i].size / bytes;
    }
    pthread_mutex_lock(&lock);
    for (uint64_t first = 0; first < frames; first += GRAIN)
        render_grain(s, buffers, count, first, (uint32_t)(frames - first < GRAIN ? frames - first : GRAIN));
    ++renders;
    if (trace() && ++window_renders * frames >= 10ull * s->rate) {
        printf("NGS2 stats: %zu renders; queues restarted %zu (%zu blocks dropped), samplers dry %zu, "
               "peak %.2f, limited frames %zu\n", window_renders, window_restarts, window_dropped,
               window_starved, window_peak, window_limited);
        window_renders = window_restarts = window_dropped = window_starved = window_limited = 0;
        window_peak = 0.0f;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t system_lock(System *s) { return s && s->magic == MAGIC_SYSTEM ? 0 : NGS2_INVALID_HANDLE; }
static ABI int32_t rack_lock(Rack *r) { return r && r->magic == MAGIC_RACK ? 0 : NGS2_INVALID_HANDLE; }
static ABI int32_t system_set_value(System *s, uint32_t value) {
    (void)value;
    return s && s->magic == MAGIC_SYSTEM ? 0 : NGS2_INVALID_HANDLE;
}

static const RuntimeExport exports[] = {
    {"sceNgs2SystemQueryBufferSize", query_size}, {"sceNgs2SystemCreate", system_create},
    {"sceNgs2SystemCreateWithAllocator", system_create}, {"sceNgs2SystemDestroy", system_destroy},
    {"sceNgs2RackQueryBufferSize", rack_query_size}, {"sceNgs2RackCreate", rack_create},
    {"sceNgs2RackCreateWithAllocator", rack_create}, {"sceNgs2RackDestroy", rack_destroy},
    {"sceNgs2RackGetVoiceHandle", voice_handle}, {"sceNgs2VoiceControl", voice_control},
    {"sceNgs2VoiceGetState", voice_state}, {"sceNgs2SystemRender", system_render},
    {"sceNgs2SystemLock", system_lock}, {"sceNgs2SystemUnlock", system_lock},
    {"sceNgs2RackLock", rack_lock}, {"sceNgs2RackUnlock", rack_lock},
    {"sceNgs2SystemSetGrainSamples", system_set_value},
};

uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

void runtime_ngs2_report(void) {
    printf("Runtime: NGS2 renders=%zu, blocks added=%zu, played=%zu, samplers out of blocks=%zu\n",
           renders, blocks_added, blocks_played, starved);
}
