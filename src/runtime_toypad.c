/* The USB toy pad, emulated behind the PS4's USB library (sceUsbd, libusb 1.0's API).
 *
 * The portal is a USB HID device (vendor 0x0E6F, product 0x0241) with one interrupt endpoint each
 * way and 32-byte reports. The game writes requests [0x55, length, command, sequence, payload...,
 * checksum] and reads replies [0x55, length, sequence, payload..., checksum]; figures placed on or
 * lifted from the pad come as unsolicited [0x56, ...] reports. The pad proves itself with the
 * seed/challenge commands (TEA with the portal's command key, a small 32-bit generator).
 *
 * Figures are virtual: a text file lists what stands on the pad, one "<zone> <figure id>" per line
 * (zone 1: the centre, 2: left, 3: right; '#' starts a comment), and is read again whenever it
 * changes, so figures can be put on and lifted off while the game runs. Each one is a 45-page
 * NTAG213 image kept in <user>/toypad/<id>-<copy>.bin: made on first use (a character's id
 * encrypted with the key of its tag UID, the tag's password), then written to as the game writes
 * the tag (vehicles, upgrades). The file: BB_TOYPAD_FILE, else <user>/toypad.txt.
 * BB_TOYPAD=0: no device, as before (sceUsbd finds nothing). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#define USBD_ERROR_NO_DEVICE ((int32_t)0x80240004)
#define USBD_ERROR_INVALID_PARAM ((int32_t)0x80240002)

enum { VENDOR = 0x0E6F, PRODUCT = 0x0241, ENDPOINT_IN = 0x81, ENDPOINT_OUT = 0x01, REPORT = 32 };
enum { TRANSFER_COMPLETED = 0, TRANSFER_TIMED_OUT = 2, TRANSFER_CANCELLED = 3 };

/* libusb 1.0's public structures, as the PS4's library lays them out. */
typedef struct UsbTransfer UsbTransfer;
typedef ABI void (*TransferCallback)(UsbTransfer *);
struct UsbTransfer {
    void *handle;
    uint8_t flags, endpoint, type;
    uint32_t timeout;
    int32_t status, length, actual_length;
    TransferCallback callback;
    void *user_data;
    uint8_t *buffer;
    int32_t num_iso_packets;
};
_Static_assert(__builtin_offsetof(UsbTransfer, callback) == 32, "libusb_transfer layout");
_Static_assert(__builtin_offsetof(UsbTransfer, buffer) == 48, "libusb_transfer layout");

typedef struct {
    uint8_t length, type, endpoint, attributes;
    uint16_t max_packet;
    uint8_t interval, refresh, synch_address;
    const uint8_t *extra;
    int32_t extra_length;
} EndpointDescriptor;
typedef struct {
    uint8_t length, type, number, alternate, num_endpoints, class_, subclass, protocol, name;
    const EndpointDescriptor *endpoint;
    const uint8_t *extra;
    int32_t extra_length;
} InterfaceDescriptor;
typedef struct { const InterfaceDescriptor *altsetting; int32_t num_altsetting; } Interface;
typedef struct {
    uint8_t length, type;
    uint16_t total_length;
    uint8_t num_interfaces, value, name, attributes, max_power;
    const Interface *interface;
    const uint8_t *extra;
    int32_t extra_length;
} ConfigDescriptor;
typedef struct {
    uint8_t length, type;
    uint16_t usb;
    uint8_t class_, subclass, protocol, max_packet0;
    uint16_t vendor, product, device;
    uint8_t manufacturer, product_name, serial, num_configurations;
} DeviceDescriptor;
_Static_assert(sizeof(DeviceDescriptor) == 18, "libusb_device_descriptor layout");
_Static_assert(__builtin_offsetof(ConfigDescriptor, interface) == 16, "libusb_config_descriptor layout");
_Static_assert(__builtin_offsetof(InterfaceDescriptor, endpoint) == 16, "libusb_interface_descriptor layout");

static const EndpointDescriptor endpoints[2] = {
    {7, 5, ENDPOINT_IN, 3, REPORT, 1, 0, 0, NULL, 0},
    {7, 5, ENDPOINT_OUT, 3, REPORT, 1, 0, 0, NULL, 0},
};
static const InterfaceDescriptor interface_descriptor = {9, 4, 0, 0, 2, 3, 0, 0, 0, endpoints, NULL, 0};
static const Interface interfaces[1] = {{&interface_descriptor, 1}};
static const ConfigDescriptor config = {9, 2, 0x29, 1, 1, 0, 0x80, 0xFA, interfaces, NULL, 0};

/* The device, its handle and the list holding it: objects of our own. */
typedef struct { uint32_t magic; } UsbObject;
static UsbObject device = {0x544f5950}, handle = {0x48414e44};
static void *device_list[2] = {&device, NULL};

/* ---- The pad's protocol ---------------------------------------------------------------------- */

static const uint32_t command_key[4] = {0xB0F6FE55, 0x410BBF62, 0xB47CB3C9, 0x7B293E97};

static void tea_encrypt(uint8_t data[8]) {
    uint32_t a, b, sum = 0;
    memcpy(&a, data, 4);
    memcpy(&b, data + 4, 4);
    for (int i = 0; i < 32; ++i) {
        sum += 0x9E3779B9;
        a += ((b << 4) + command_key[0]) ^ (b + sum) ^ ((b >> 5) + command_key[1]);
        b += ((a << 4) + command_key[2]) ^ (a + sum) ^ ((a >> 5) + command_key[3]);
    }
    memcpy(data, &a, 4);
    memcpy(data + 4, &b, 4);
}

static void tea_decrypt(uint8_t data[8]) {
    uint32_t a, b, sum = 0xC6EF3720;
    memcpy(&a, data, 4);
    memcpy(&b, data + 4, 4);
    for (int i = 0; i < 32; ++i) {
        b -= ((a << 4) + command_key[2]) ^ (a + sum) ^ ((a >> 5) + command_key[3]);
        a -= ((b << 4) + command_key[0]) ^ (b + sum) ^ ((b >> 5) + command_key[1]);
        sum -= 0x9E3779B9;
    }
    memcpy(data, &a, 4);
    memcpy(data + 4, &b, 4);
}

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

/* The pad's random generator (seeded by the game through command 0xB1). */
static uint32_t rng[4];
static uint32_t rng_next(void) {
    const uint32_t e = rng[0] - rotl32(rng[1], 21);
    rng[0] = rng[1] ^ rotl32(rng[2], 19);
    rng[1] = rng[2] + rotl32(rng[3], 6);
    rng[2] = rng[3] + e;
    rng[3] = e + rng[0];
    return rng[3];
}
static void rng_seed(uint32_t seed) {
    rng[0] = 0xF1EA5EED;
    rng[1] = rng[2] = rng[3] = seed;
    for (int i = 0; i < 42; ++i) rng_next();
}

static uint8_t checksum(const uint8_t *data, int count) {
    unsigned sum = 0;
    for (int i = 0; i < count; ++i) sum += data[i];
    return (uint8_t)sum;
}

/* ---- Figures ----------------------------------------------------------------------------- */

enum { PAGES = 45, SLOTS = 7 };
typedef struct {
    int present;
    uint8_t zone, index; ///< zone 1..3, the pad's reader index 1..7
    uint32_t id, copy;
    uint8_t data[PAGES * 4];
} Figure;
static Figure figures[SLOTS];

static uint32_t rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
/* The tags' hash over little-endian words of a key. */
static uint32_t tag_hash(const uint8_t *key, int words) {
    uint32_t h = 0;
    for (int i = 0; i < words; ++i) {
        uint32_t w;
        memcpy(&w, key + i * 4, 4);
        h = w + rotr32(h, 25) + rotr32(h, 10) - h;
    }
    return h;
}
static void tag_uid(const uint8_t *data, uint8_t uid[7]) {
    uid[0] = data[0]; uid[1] = data[1]; uid[2] = data[2];
    memcpy(uid + 3, data + 4, 4);
}
/* A character's key: four hashes of the UID followed by the tags' constant. */
static void figure_key(const uint8_t *data, uint32_t key[4]) {
    static const uint8_t constant[17] = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8,
                                         0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};
    for (int n = 3; n <= 6; ++n) {
        uint8_t buffer[24];
        tag_uid(data, buffer);
        memcpy(buffer + 7, constant, sizeof(constant));
        buffer[n * 4 - 1] = 0xAA;
        key[n - 3] = tag_hash(buffer, n);
    }
}
/* Builds a tag: a UID made from the figure id and copy, the id encrypted into pages 36 and 37
 * for characters (written in plain for vehicles and gadgets, ids from 1000), the password. */
static void figure_create(Figure *f) {
    memset(f->data, 0, sizeof(f->data));
    uint32_t h = f->id * 2654435761u ^ (f->copy + 1) * 40503u;
    uint8_t uid[7] = {0x04, (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24),
                      (uint8_t)(f->copy * 37 + 11), 0x80};
    f->data[0] = uid[0]; f->data[1] = uid[1]; f->data[2] = uid[2];
    f->data[3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    memcpy(f->data + 4, uid + 3, 4);
    f->data[8] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
    if (f->id < 1000) {
        uint32_t key[4];
        figure_key(f->data, key);
        uint8_t value[8];
        memcpy(value, &f->id, 4);
        memcpy(value + 4, &f->id, 4);
        uint32_t a, b, sum = 0;
        memcpy(&a, value, 4); memcpy(&b, value + 4, 4);
        for (int i = 0; i < 32; ++i) {
            sum += 0x9E3779B9;
            a += ((b << 4) + key[0]) ^ (b + sum) ^ ((b >> 5) + key[1]);
            b += ((a << 4) + key[2]) ^ (a + sum) ^ ((a >> 5) + key[3]);
        }
        memcpy(f->data + 36 * 4, &a, 4);
        memcpy(f->data + 37 * 4, &b, 4);
    } else {
        memcpy(f->data + 36 * 4, &f->id, 4);
        f->data[38 * 4 + 1] = 1;
    }
    static const char copyright[] = "(c) Copyright LEGO 2014";
    uint8_t buffer[32];
    memcpy(buffer, uid, 7);
    memcpy(buffer + 7, copyright, 23);
    buffer[30] = buffer[31] = 0xAA;
    const uint32_t password = tag_hash(buffer, 8);
    memcpy(f->data + 43 * 4, &password, 4);
}
static void figure_path(const Figure *f, char *path, size_t size) {
    snprintf(path, size, "%s/toypad/%u-%u.bin", runtime_file_user_dir(), f->id, f->copy);
}
static void figure_save(const Figure *f) {
    char path[512];
    snprintf(path, sizeof(path), "%s/toypad", runtime_file_user_dir());
    mkdir(path, 0755);
    figure_path(f, path, sizeof(path));
    FILE *file = fopen(path, "wb");
    if (!file) { printf("Toy Pad: cannot write %s\n", path); return; }
    fwrite(f->data, 1, sizeof(f->data), file);
    fclose(file);
}
static void figure_load(Figure *f) {
    char path[512];
    figure_path(f, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    const int loaded = file && fread(f->data, 1, sizeof(f->data), file) == sizeof(f->data);
    if (file) fclose(file);
    if (!loaded) { figure_create(f); figure_save(f); }
}

/* Replies and figure reports waiting for an IN transfer. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
enum { QUEUE = 64 };
static uint8_t replies[QUEUE][REPORT];
static unsigned reply_head, reply_count;
static size_t commands, unknown_commands;
/* Figure reports, sent once the game has woken the pad (after its first challenge). */
static uint8_t events[QUEUE][REPORT];
static unsigned event_head, event_count;
static int awake;

static void queue_event(const Figure *f, int removed) {
    if (event_count == QUEUE) return;
    uint8_t *e = events[(event_head + event_count++) % QUEUE];
    memset(e, 0, REPORT);
    const uint8_t report[13] = {0x56, 0x0b, f->zone, 0, f->index, (uint8_t)removed, f->data[0], f->data[1],
                                f->data[2], f->data[4], f->data[5], f->data[6], f->data[7]};
    memcpy(e, report, sizeof(report));
    e[13] = checksum(e, 13);
}
static Figure *figure_at(uint8_t index) {
    return index >= 1 && index <= SLOTS && figures[index - 1].present ? &figures[index - 1] : NULL;
}

static void queue_reply(const uint8_t reply[REPORT]) {
    if (reply_count == QUEUE) return; /* the game stopped reading: dropped */
    memcpy(replies[(reply_head + reply_count++) % QUEUE], reply, REPORT);
}

/* The three zones' lights as the game last set them (0xRRGGBB): the target colour of a colour,
 * fade or flash. Payloads: C0 [zone, r, g, b]; C2 [zone, tick, count, r, g, b]; C3 [zone, on,
 * off, count, r, g, b]; C6 [enable, tick, count, r, g, b] x 3; C7 [enable, on, off, count, r, g,
 * b] x 3; C8 [enable, r, g, b] x 3. Zone 0 is all three. */
static uint32_t lights[3];
static uint32_t rgb(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static void set_zone(unsigned zone, uint32_t colour) {
    if (zone == 0) lights[0] = lights[1] = lights[2] = colour;
    else if (zone <= 3) lights[zone - 1] = colour;
}
static void set_lights(uint8_t command, const uint8_t *p) {
    switch (command) {
    case 0xC0: set_zone(p[0], rgb(p + 1)); break;
    case 0xC2: set_zone(p[0], rgb(p + 3)); break;
    case 0xC3: set_zone(p[0], rgb(p + 4)); break;
    case 0xC6: for (int z = 0; z < 3; ++z) if (p[z * 6]) lights[z] = rgb(p + z * 6 + 3); break;
    case 0xC7: for (int z = 0; z < 3; ++z) if (p[z * 7]) lights[z] = rgb(p + z * 7 + 4); break;
    case 0xC8: for (int z = 0; z < 3; ++z) if (p[z * 4]) lights[z] = rgb(p + z * 4 + 1); break;
    default: break;
    }
}
/* For the menu (libbbgpu): the zones' colours, centre, left, right. */
void runtime_toypad_lights(uint32_t out[3]) {
    pthread_mutex_lock(&lock);
    memcpy(out, lights, sizeof(lights));
    pthread_mutex_unlock(&lock);
}

/* One request from the game (an OUT report), answered into the reply queue. */
static void handle_request(const uint8_t *request) {
    if (request[0] != 0x55) return;
    const uint8_t command = request[2], sequence = request[3];
    uint8_t reply[REPORT] = {0x55};
    uint8_t value[8];
    ++commands;
    switch (command) {
    case 0xB0: { /* wake: the pad's banner, "(c) LEGO 2014" */
        static const uint8_t banner[] = {0x55, 0x0e, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4c, 0x45,
                                         0x47, 0x4f, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
        memcpy(reply, banner, sizeof(banner));
        break;
    }
    case 0xB1: { /* seed: the generator's seed and a confirmation, echoed back encrypted */
        memcpy(value, request + 4, 8);
        tea_decrypt(value);
        uint32_t seed;
        memcpy(&seed, value, 4);
        rng_seed(seed);
        uint8_t answer[8] = {value[4], value[5], value[6], value[7], 0, 0, 0, 0};
        tea_encrypt(answer);
        reply[1] = 0x09; reply[2] = sequence;
        memcpy(reply + 3, answer, 8);
        reply[11] = checksum(reply, 11);
        break;
    }
    case 0xB3: { /* challenge: the generator's next number and the confirmation */
        memcpy(value, request + 4, 8);
        tea_decrypt(value);
        const uint32_t next = rng_next();
        uint8_t answer[8];
        memcpy(answer, &next, 4);
        memcpy(answer + 4, value, 4);
        tea_encrypt(answer);
        reply[1] = 0x09; reply[2] = sequence;
        memcpy(reply + 3, answer, 8);
        reply[11] = checksum(reply, 11);
        awake = 1;
        break;
    }
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC6: case 0xC7: case 0xC8:
        /* lights: their colours kept for the menu (the game lights the zone it wants a figure on) */
        set_lights(command, request + 4);
        reply[1] = 0x01; reply[2] = sequence;
        reply[3] = checksum(reply, 3);
        break;
    case 0xD2: { /* read 4 pages of the figure at index [4] from page [5] (zeros past the end) */
        const Figure *f = figure_at(request[4]);
        reply[1] = 0x12; reply[2] = sequence; reply[3] = 0;
        for (int i = 0; f && i < 16; ++i)
            if (request[5] * 4 + i < PAGES * 4) reply[4 + i] = f->data[request[5] * 4 + i];
        reply[20] = checksum(reply, 20);
        break;
    }
    case 0xD3: { /* write page [5] of the figure at index [4] */
        Figure *f = figure_at(request[4]);
        if (f && request[5] < PAGES) {
            memcpy(f->data + request[5] * 4, request + 6, 4);
            figure_save(f);
        }
        reply[1] = 0x02; reply[2] = sequence; reply[3] = 0;
        reply[4] = checksum(reply, 4);
        break;
    }
    case 0xD4: { /* the model (id) of the figure at the encrypted index, and the confirmation */
        memcpy(value, request + 4, 8);
        tea_decrypt(value);
        const Figure *f = figure_at(value[0]);
        uint32_t id = 0;
        if (f && f->id < 1000) id = f->id;
        else if (f) memcpy(&id, f->data + 36 * 4, 4);
        uint8_t answer[8] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24),
                             value[4], value[5], value[6], value[7]};
        if (!f) memset(answer, 0, 4);
        tea_encrypt(answer);
        reply[1] = 0x0a; reply[2] = sequence; reply[3] = 0;
        memcpy(reply + 4, answer, 8);
        reply[12] = checksum(reply, 12);
        break;
    }
    default: /* tag list, passwords, activity, LED queries: an empty acknowledgement */
        ++unknown_commands;
        if (unknown_commands <= 8) printf("Toy Pad: command %#x answered empty\n", command);
        reply[1] = 0x01; reply[2] = sequence;
        reply[3] = checksum(reply, 3);
        break;
    }
    queue_reply(reply);
}

static void trace(const char *what, long a, long b);

/* ---- Transfers: OUT completes at once, IN when a report is there --------------------------- */

typedef struct Pending { UsbTransfer *transfer; uint64_t deadline_ns; } Pending;
enum { MAX_PENDING = 32 };
static Pending pending[MAX_PENDING];
static unsigned num_pending;
static UsbTransfer *completed[MAX_PENDING * 2];
static unsigned num_completed;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

/* Under the lock: IN transfers that can complete, and those whose time is up. */
static void settle_locked(void) {
    const uint64_t now = now_ns();
    for (unsigned i = 0; i < num_pending;) {
        UsbTransfer *t = pending[i].transfer;
        int done = 0;
        if (reply_count || (awake && event_count)) {
            const int32_t n = t->length < REPORT ? t->length : REPORT;
            if (reply_count) {
                memcpy(t->buffer, replies[reply_head], (size_t)n);
                reply_head = (reply_head + 1) % QUEUE;
                --reply_count;
            } else {
                memcpy(t->buffer, events[event_head], (size_t)n);
                event_head = (event_head + 1) % QUEUE;
                --event_count;
            }
            t->actual_length = n;
            t->status = TRANSFER_COMPLETED;
            done = 1;
        } else if (pending[i].deadline_ns && now >= pending[i].deadline_ns) {
            t->actual_length = 0;
            t->status = TRANSFER_TIMED_OUT;
            done = 1;
        }
        if (done) {
            if (num_completed < sizeof(completed) / sizeof(*completed)) completed[num_completed++] = t;
            pending[i] = pending[--num_pending];
        } else {
            ++i;
        }
    }
}

static ABI int32_t submit_transfer(UsbTransfer *t) {
    trace(t && t->endpoint == ENDPOINT_OUT ? "submit OUT" : "submit IN", t ? (long)t->endpoint : 0,
          t && t->buffer && t->endpoint == ENDPOINT_OUT ? (long)t->buffer[2] : -1);
    if (!t || t->handle != &handle) return USBD_ERROR_NO_DEVICE;
    pthread_mutex_lock(&lock);
    if (t->endpoint == ENDPOINT_OUT) {
        if (t->buffer && t->length >= 4) handle_request(t->buffer);
        t->actual_length = t->length;
        t->status = TRANSFER_COMPLETED;
        if (num_completed < sizeof(completed) / sizeof(*completed)) completed[num_completed++] = t;
    } else {
        if (num_pending == MAX_PENDING) { pthread_mutex_unlock(&lock); return USBD_ERROR_INVALID_PARAM; }
        pending[num_pending++] = (Pending){t, t->timeout ? now_ns() + (uint64_t)t->timeout * 1000000 : 0};
    }
    settle_locked();
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

/* ---- The figures file --------------------------------------------------------------------- */

static const char *figures_file(void) {
    static char path[512];
    if (!path[0]) {
        const char *env = getenv("BB_TOYPAD_FILE");
        if (env && *env) snprintf(path, sizeof(path), "%s", env);
        else snprintf(path, sizeof(path), "%s/toypad.txt", runtime_file_user_dir());
    }
    return path;
}
/* Under the lock: the file's figures against the pad's. A figure stays while its line does (the
 * n-th line with an id in a zone is copy n of that id); lifted ones go first, then new ones take
 * the lowest free reader index. */
static void sync_figures_locked(void) {
    static uint64_t last_check;
    static struct timespec last_mtime;
    static int had_file = -1;
    const uint64_t now = now_ns();
    if (now - last_check < 300000000ull) return;
    last_check = now;
    struct stat st;
    const int has_file = stat(figures_file(), &st) == 0;
    if (has_file == had_file && (!has_file || (st.st_mtim.tv_sec == last_mtime.tv_sec &&
                                               st.st_mtim.tv_nsec == last_mtime.tv_nsec)))
        return;
    if (had_file < 0 && !has_file) printf("Toy Pad: no figures file (%s)\n", figures_file());
    had_file = has_file;
    if (has_file) last_mtime = st.st_mtim;
    typedef struct { uint8_t zone; uint32_t id, copy; } Wanted;
    Wanted wanted[SLOTS];
    unsigned num_wanted = 0, per_zone[4] = {0};
    FILE *file = has_file ? fopen(figures_file(), "r") : NULL;
    char line[256];
    while (file && fgets(line, sizeof(line), file)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned zone, id;
        if (sscanf(line, "%u %u", &zone, &id) != 2) continue;
        const unsigned capacity = zone == 1 ? 1 : 3;
        if (zone < 1 || zone > 3 || !id || per_zone[zone] == capacity || num_wanted == SLOTS) {
            printf("Toy Pad: figure %u in zone %u does not fit (zone 1 holds 1, zones 2 and 3 hold 3)\n", id, zone);
            continue;
        }
        ++per_zone[zone];
        uint32_t copy = 0;
        for (unsigned i = 0; i < num_wanted; ++i) copy += wanted[i].id == id;
        wanted[num_wanted++] = (Wanted){(uint8_t)zone, id, copy};
    }
    if (file) fclose(file);
    int kept[SLOTS] = {0};
    for (unsigned s = 0; s < SLOTS; ++s) {
        Figure *f = &figures[s];
        if (!f->present) continue;
        int found = 0;
        for (unsigned i = 0; i < num_wanted && !found; ++i)
            if (!kept[i] && wanted[i].zone == f->zone && wanted[i].id == f->id && wanted[i].copy == f->copy)
                kept[i] = found = 1;
        if (!found) {
            queue_event(f, 1);
            printf("Toy Pad: figure %u lifted from zone %u\n", f->id, f->zone);
            f->present = 0;
        }
    }
    for (unsigned i = 0; i < num_wanted; ++i) {
        if (kept[i]) continue;
        for (unsigned s = 0; s < SLOTS; ++s) {
            Figure *f = &figures[s];
            if (f->present) continue;
            f->present = 1; f->zone = wanted[i].zone; f->index = (uint8_t)(s + 1);
            f->id = wanted[i].id; f->copy = wanted[i].copy;
            figure_load(f);
            queue_event(f, 0);
            printf("Toy Pad: figure %u placed on zone %u (reader index %u)\n", f->id, f->zone, f->index);
            break;
        }
    }
}

/* The game's event loop: up to 10 ms for something to complete, then its callbacks. */
static ABI int32_t handle_events(void) {
    UsbTransfer *done[MAX_PENDING * 2];
    unsigned count;
    pthread_mutex_lock(&lock);
    sync_figures_locked();
    settle_locked();
    if (!num_completed) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 10000000;
        if (deadline.tv_nsec >= 1000000000) { deadline.tv_nsec -= 1000000000; ++deadline.tv_sec; }
        pthread_cond_timedwait(&changed, &lock, &deadline);
        settle_locked();
    }
    count = num_completed;
    memcpy(done, completed, count * sizeof(*done));
    num_completed = 0;
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < count; ++i) {
        if (done[i]->callback) done[i]->callback(done[i]);
    }
    return 0;
}

/* ---- sceUsbd -------------------------------------------------------------------------------- */

/* BB_TOYPAD_TRACE=1: the game's USB calls, the first ones of each kind. */
static void trace(const char *what, long a, long b) {
    static int on = -1;
    static int counts[64];
    if (on < 0) { const char *env = getenv("BB_TOYPAD_TRACE"); on = env && env[0] == '1'; }
    if (!on) return;
    unsigned h = 0;
    for (const char *c = what; *c; ++c) h = h * 31 + (unsigned char)*c;
    if (counts[h % 64]++ < 20) printf("Toy Pad trace: %s(%#lx, %#lx)\n", what, a, b);
}

static int enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *env = getenv("BB_TOYPAD");
        on = !(env && env[0] == '0');
    }
    return on;
}

static ABI int32_t usbd_init(void) {
    if (enabled()) {
        setenv("BB_TOYPAD_FILE", figures_file(), 0); /* the menu's Toy Pad section edits it */
        printf("Toy Pad: connected (emulated; figures from %s)\n", figures_file());
    }
    else printf("Toy Pad: not connected (BB_TOYPAD=0)\n");
    return 0;
}
static ABI void nothing(void) {}
static ABI int64_t get_device_list(void ***list) {
    trace("GetDeviceList", 0, 0);
    static void *none[1];
    if (!list) return USBD_ERROR_INVALID_PARAM;
    *list = enabled() ? device_list : none;
    return enabled() ? 1 : 0;
}
static ABI int32_t get_device_descriptor(void *dev, DeviceDescriptor *out) {
    trace("GetDeviceDescriptor", (long)(uintptr_t)dev, 0);
    if (dev != &device || !out) return USBD_ERROR_NO_DEVICE;
    *out = (DeviceDescriptor){18, 1, 0x0200, 0, 0, 0, REPORT, VENDOR, PRODUCT, 0x0200, 1, 2, 0, 1};
    return 0;
}
static ABI int32_t get_config_descriptor(void *dev, uint8_t index, const ConfigDescriptor **out) {
    if (dev != &device || index != 0 || !out) return USBD_ERROR_NO_DEVICE;
    *out = &config;
    return 0;
}
static ABI void *open_vid_pid(uint16_t vendor, uint16_t product) {
    trace("OpenDeviceWithVidPid", vendor, product);
    return enabled() && vendor == VENDOR && product == PRODUCT ? &handle : NULL;
}
static ABI int32_t device_ok(void *h) {
    trace("ResetDevice", (long)(uintptr_t)h, (long)((uintptr_t)__builtin_return_address(0) - 0x800000000ull));
    return h == &handle || h == &device ? 0 : USBD_ERROR_NO_DEVICE;
}
/* 0: still connected (anything else and the game takes the pad as unplugged). */
static ABI int32_t connected(void *h) {
    trace("CheckConnected", (long)(uintptr_t)h, 0);
    return h == &handle ? 0 : USBD_ERROR_NO_DEVICE;
}
/* The console's HID driver holds the pad: 1 (active); the game detaches it, then claims the
 * interface (with 0 it resets the device and starts over). */
static ABI int32_t driver_active(void *h, int32_t iface) {
    trace("KernelDriverActive", (long)(uintptr_t)h, iface);
    return h == &handle ? 1 : USBD_ERROR_NO_DEVICE;
}
static ABI int32_t claim(void *h, int32_t iface) { (void)iface; return h == &handle ? 0 : USBD_ERROR_NO_DEVICE; }
static ABI int32_t bus_number(void *dev) { return dev == &device ? 1 : USBD_ERROR_NO_DEVICE; }
static ABI int32_t max_packet(void *dev, uint8_t endpoint) { (void)endpoint; return dev == &device ? REPORT : USBD_ERROR_NO_DEVICE; }
static ABI UsbTransfer *alloc_transfer(int32_t iso_packets) {
    return calloc(1, sizeof(UsbTransfer) + 16 + (size_t)(iso_packets > 0 ? iso_packets : 0) * 12);
}
static ABI void fill_interrupt(UsbTransfer *t, void *h, uint8_t endpoint, uint8_t *buffer, int32_t length,
                               TransferCallback callback, void *user_data, uint32_t timeout) {
    if (!t) return;
    t->handle = h; t->endpoint = endpoint; t->type = 3; /* interrupt */
    t->timeout = timeout; t->buffer = buffer; t->length = length;
    t->callback = callback; t->user_data = user_data;
}

static const RuntimeExport exports[] = {
    {"sceUsbdInit", usbd_init}, {"sceUsbdExit", nothing},
    {"sceUsbdGetDeviceList", get_device_list}, {"sceUsbdFreeDeviceList", nothing},
    {"sceUsbdGetDeviceDescriptor", get_device_descriptor},
    {"sceUsbdGetConfigDescriptor", get_config_descriptor}, {"sceUsbdFreeConfigDescriptor", nothing},
    {"sceUsbdOpenDeviceWithVidPid", open_vid_pid}, {"sceUsbdCheckConnected", connected},
    {"sceUsbdKernelDriverActive", driver_active}, {"sceUsbdDetachKernelDriver", claim},
    {"sceUsbdClaimInterface", claim}, {"sceUsbdResetDevice", device_ok},
    {"sceUsbdGetBusNumber", bus_number}, {"sceUsbdGetDeviceAddress", bus_number},
    {"sceUsbdGetMaxPacketSize", max_packet},
    {"sceUsbdAllocTransfer", alloc_transfer}, {"sceUsbdFillInterruptTransfer", fill_interrupt},
    {"sceUsbdSubmitTransfer", submit_transfer}, {"sceUsbdHandleEvents", handle_events},
};

uintptr_t runtime_toypad_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

void runtime_toypad_report(void) {
    if (commands) printf("Toy Pad: %zu commands (%zu answered empty)\n", commands, unknown_commands);
}
