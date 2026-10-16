# Ray-traced ambient occlusion (`BB_RTAO=1`)

Experimental. Replaces the game's screen-space AO with ray-traced AO: the G-buffer geometry the
game draws is captured each frame, built into Vulkan acceleration structures, and traced with
ray queries at half resolution. The result is written into the game's own AO mask, so the game's
lighting applies it exactly where it applied its SSAO. Everything is behind `BB_RTAO=1`; without
it the port is unchanged.

In the launcher: Settings, "Ray tracing (experimental)": the switch sets `BB_RTAO=1`, the quality
preset (low, medium, high or custom) writes `rays`, `radius`, `strength`, `far`, `snap` and
`foliage` (0.35 for high, 0 otherwise) into `user/rtao.txt` at launch (costs in the measurements table; medium is the default below).

## Requirements

- `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` (RADV on
  RDNA2 and later, NVIDIA, Intel Arc). Without them the feature logs why and stays off. They
  are enabled on the device only with `BB_RTAO=1`.
  `VK_KHR_ray_tracing_pipeline`, where present, runs the trace as a ray-tracing pipeline (faster
  on the RX 9070 XT); otherwise it runs as ray queries in a compute shader.
  `VK_KHR_ray_tracing_position_fetch`, where present, only for the BLAS flag ALLOW_DATA_ACCESS it
  defines (cheaper builds on RADV, see measurements); without it the builds go without the flag.
- Object motion vectors (an upscaler with motion vectors): the geometry comes from that capture.
  With `BB_RTAO=1`, ray queries and object motion on, G-buffer pipelines the motion path leaves
  out get a capture-only variant (pipeline key `motion_vectors = 2`: the vertex shader stores
  positions, the fragment shader and attachments are as without RTAO), and their draws store
  positions without history into a region and parameter block of their own (4M vertices per
  frame). The motion vectors the upscaler gets do not change: the `Object motion:` stats line is
  the same with RTAO on and off. Without `BB_RTAO=1` the device, the capture and every pipeline
  key are as without the feature.
- The game's own SSAO pass must run: the "Disable SSAO" game patch (launcher, Game effects,
  SSAO unchecked) removes the mask RTAO writes into. The log says so once.

## Data flow per frame

1. G-buffer draws (five or more colour targets plus depth) store their post-vertex-shader clip
   positions in the object-motion capture (the motion path's draws in its history, the rest in
   the RTAO's own region; only while the pass is on, `enabled=1`). `Rasterizer` hands each to `RayTracedAO::AddGeometry`
   with its index buffer address, counts, topology, alpha-test and depth-write state, an identity
   hash and whether it is skinned. Strips and draws without depth write (decals) are left out;
   alpha-tested draws (foliage) too unless `foliage` > 0: then they go last into the per-frame
   BLAS as a second, non-opaque geometry (never cached, at most a quarter of the per-frame budget),
   and an AO ray hitting one stops with probability `foliage`, drawn per pixel, frame, ray and
   triangle (`rtao_trace_constants.glsl`; the any-hit shader in the pipeline variant, a candidate
   loop with ray queries). Over the temporal accumulation a grass card occludes at about that
   coverage instead of as a solid quad. The surface snap and the primary-ray test skip it.
2. The game's AO upsample pass (pixel shader `0xc45c51cb` into an R8 scene-size target) ends;
   `Record()` (`vk_rtao.cpp`) runs; the draws' intake and classification are `AddGeometry()`,
   `ClassifyDraws()` and `BuildTable()` (`vk_rtao_cache.cpp`), the GPU work `RecordImageSetup()`,
   `FillConstants()` and `RecordPasses()` (`vk_rtao_passes.cpp`); construction, the settings
   re-read and destruction are in `vk_rtao_setup.cpp`, buffers and the static BLAS's choice of
   parts in `vk_rtao_resources.cpp`, the dumps and logs in `vk_rtao_diagnostics.cpp`, the logic
   without Vulkan in `rtao_geometry.{h,cpp}` (with `tests/test_rtao_geometry.cpp`):
   - static cache: the probe results of four passes ago are read (once their scheduler tick is
     done), the static BLAS is rebuilt if due, and the frame's draws are sorted into covered by
     the static BLAS, per-frame geometry and new cache entries (see below);
   - `rtao_expand.comp`: per-frame triangles into a view-space list (and world-space copies of new
     cache entries, copied into the static pool); triangles entirely beyond `far` + `radius`
     become inactive;
   - `rtao_probe.comp`: world-space count, centroid, bounds and first vertex of probed draws;
   - BLAS builds (the static BLAS when rebuilt; the per-frame list, fast build), then a TLAS of
     two instances: the per-frame BLAS and the static BLAS placed with the view matrix;
   - the trace at half resolution (`rtao_trace_body.glsl`, as `rtao_trace.rgen` + `.rmiss` +
     `.rchit` or `rtao_trace.comp`): a geometric normal from four depth taps, (`snap`, default on) a
     short ray that snaps the origin onto the BVH surface (depth-biased draws sit centimetres
     off it), a Waechter-Binder ulp offset, then `rays` cosine-weighted rays over `radius`; temporal blend
     with a bilinear, depth-tested, clamped history (denoiser 1), or raw output (denoiser 2);
   - denoiser 2 (default): `rtao_temporal.comp` blends the raw AO with the reprojected history at
     1/N, N being the per-pixel count of accumulated frames (up to `maxn`, reset where the depth
     test fails), after clipping the history to the current 3x3 mean +- max(`clipk` sigma,
     `clipfloor`);
   - `rtao_blur.comp`: separable 7-tap depth-aware blur (`sigma`; with denoiser 2 twice as wide
     where N is 1, half as wide from N = 16);
   - `rtao_apply.comp`: depth-aware 4x4 upsample, `ao^power`, written as
     `min(game mask, lerp(1, ao, strength))` (or ours alone with `replace=1`).

### Static cache

A draw's identity is its shader, buffers, index range, counts and occurrence in the frame. A draw
whose world-space summary stays the same for `promote` observations is cached: its world-space
triangles are copied into a pool (3M triangles, 102 MiB), and it leaves the per-frame BLAS once
the static BLAS holds it. Cached draws are rechecked every 8th pass and evicted when they change;
three evictions and a draw stays per frame (it is still looked at every 64th pass, and after 16
unchanged looks, about 20 s, it gets another chance). A mesh drawn several times a frame is not
counted against: its slots shift whenever a copy is culled or uncovered (in Central Yharnam this had
blacklisted 475k triangles of crates and coffins for good). Copies of a mesh drawn several times a frame are
matched by content (the game sorts draws by distance) and leave the per-frame BLAS only as a
group. Draws with a character-size skeleton are never cached.

The static BLAS is one tree over the pool's triangles of every cached draw seen in the last 2
seconds, within `far` + `radius` + `margin` of the camera, nearest first up to 2M triangles. It is
rebuilt when the camera has moved more than `margin` (8 m) since the last build, when a draw in it
was evicted, and at most every `rebuild` (30) frames when the set changed. One tree instead of an
instance per cached draw: overlapping instances made every nearby ray traverse several trees
(about 800 pieces of big meshes as instances: trace 0.5 -> 0.75-1.46 ms). A rebuild costs 1.4 ms
on the GPU (956k triangles, fast build with ALLOW_DATA_ACCESS; fast trace 2.75 ms: `staticbuild`).

Draws bigger than `maxsize` (terrain, building blocks) are cached in pieces (`chunk`, metres):
their world-space triangles are copied to the host once, split on a worker thread at the median
of the longest axis until a piece fits in `chunk` metres (box diagonal), and written to a staging
buffer, from which the pieces are copied into the pool. Each piece joins the static BLAS only
within reach; whole, their far triangles would be in every rebuild. (Pieces of 4096-index chunks in
the game's order were tried first: the game's index order is not spatial.) When the pool is full,
the least recently seen draws leave it.

### Index buffers

The pass reads each draw's index buffer by device address when the game's AO pass ends, many draws
later. Small ranges (up to 16 KiB, streamed by the buffer cache through a ring that is reused once
the draw's tick is done) are copied from the game's memory at draw time into the RTAO's own ring,
through the runtime's backing view under its memory lock (`ReadBacking`; never a raw guest pointer).
With layer memory (`BB_LAYER_MEMORY=1`, default off on AMD) the buffer cache holds the mirrors it
retires during a frame over an index range the pass reads by address (the rasterizer registers
each with `HoldRange`; `retire_hold.h`) until the pass is recorded, and only while the pass is on;
every other retirement, the collector's pressure compaction included, is as without RTAO.
`BB_RTAO_TEST_MOVE=1` (with layer memory and `BB_DRAW_PIPE=0`) forces every mirror back in place
between the AO upsample and the pass: without the hold (`BB_RTAO_HOLD=0`) the GPU faults at once,
with it it does not (24 to 30 of 50 retired mirrors held per move at the Doll).

### Host rings

The geometry table, the instance list and the trace constants rotate over three slots, the probe
results over four. Each slot records the scheduler tick of the pass that used it; a slot is
reused only once the GPU has passed that tick, otherwise the pass skips the frame and the game's
AO stays. `ringtest=1` forces every pass onto one slot to exercise that path: at the Doll about
15% of passes skip, no fault.

## Settings

Environment at start (`BB_RTAO_*`), most also live in `<user>/rtao.txt` (`BB_RTAO_FILE`),
re-read within a second of a change. `~/.local/bin/rtao key=value` edits the file.

| Key | Env | Default | Meaning |
|---|---|---|---|
| `enabled` | | 1 | 0: the pass is not recorded and nothing is captured for it (the pipeline variants stay) |
| `strength` | `BB_RTAO_STRENGTH` | 1.0 | how much of the AO enters the mask |
| `power` | `BB_RTAO_POWER` | 2.0 | `ao^power` (1 = physically plain, 2 = the chosen look) |
| `radius` | `BB_RTAO_RADIUS` | 1.5 | ray length in metres |
| `rays` | `BB_RTAO_RAYS` | 2 | AO rays per half-res pixel (denoiser 1 wants 6) |
| `denoise` | `BB_RTAO_DENOISE` | 2 | 2: accumulated frame count + variance clipping; 1: fixed history weight |
| `maxn` | | 32 | denoiser 2: accumulated frames at most |
| `clipk`, `clipfloor` | | 2, 0.1 | denoiser 2: history clip to mean +- max(clipk sigma, clipfloor) |
| `subpixel` | | 0 | denoiser 2: 1 cycles the sampled pixel through the 2x2 block (thin features; less steady) |
| `falloff` | | 0 | 1: binary hits within a random length, linear to 0 at the radius (lighter) |
| `noise` | | 1 | 1: the rays' samples from a 64x64 blue-noise texture (`tools/rtao_bluenoise.py`), shifted per frame by the golden ratio; 0: interleaved gradient noise |
| `period` | | 8 | IGN only: frames before a pixel's ray pattern repeats |
| `history` | `BB_RTAO_HISTORY` | 0.5 | denoiser 1: temporal weight (0.85 lagged visibly in motion) |
| `sigma` | `BB_RTAO_SIGMA` | 1.5 | half-res blur width in texels, 0 off |
| `blur` | | 1 | 4x4 depth-aware upsample, 0: 2x2 |
| `replace` | | 0 | 1: ignore the game's AO (or a replaced AO shader's) in the mask |
| `far` | | 40 | no AO and no per-frame geometry beyond this distance (fades over the last quarter) |
| `snap` | | 40 | the surface snap runs up to this distance; keep it at `far` (0 off; without it depth-biased draws such as the protruding bricks of the Hunter's Dream garden wall flicker in motion) |
| `normals` | | 1 | 1: geometric normal from the depth buffer, 0: the G-buffer's shading normal |
| `offset` | | 0 | metres along the normal on top of the ulp offset |
| `pipeline` | `BB_RTAO_PIPELINE` | 1 | the trace as a ray-tracing pipeline (where supported), 0: ray queries |
| | `BB_RTAO_DATA_ACCESS` | 1 | ALLOW_DATA_ACCESS on the per-frame BLAS (see measurements) |
| `cache` | `BB_RTAO_CACHE` | 1 | static cache on/off |
| `promote` | | 8 | matching observations before a draw is cached |
| `maxsize` | | 20 | bigger draws are cached in pieces, or with `chunk=0` stay per frame (metres, bounding-box diagonal) |
| `chunk` | | 10 | size of the pieces of big draws (metres; 0: big draws stay per frame) |
| `margin` | | 8 | the static BLAS reaches this far past `far` + `radius`; moving this far rebuilds it |
| `rebuild` | | 30 | frames between rebuilds for a changed set of cached draws (0: every pass, to measure a rebuild) |
| `staticbuild` | | 2 | static BLAS build: 0 fast trace, 1 fast build, 2 fast build + ALLOW_DATA_ACCESS |
| `build` | | 0 | 1: per-frame BLAS built for trace speed (no difference on RADV) |
| `alpha`, `decals` | | 0 | 1: keep alpha-tested draws (as solid quads) / draws without depth write |
| `foliage` | 0-1 | 0 | alpha-tested draws occlude with this probability per hit (0: left out); about +0.7 ms |
| | `BB_RTAO_MAX_TRIANGLES` | 1500000 | per-frame triangle budget (buffers are sized for it; 3000000 for `cache=0` or `chunk=0`); set, it also switches off the growth below |
| `stage` | `BB_RTAO_STAGE` | 5 | stop after: 1 expand, 2 BLAS, 3 TLAS, 4 trace, 5 apply |
| `debug` | `BB_RTAO_DEBUG` | 0 | 1 raw AO, 2 camera-ray test, 3 normal facing, 4 AO with t_min 0.1 |
| `dump` | | | change the value: 8 consecutive frames of the AO the apply gets go to `out/rtao_dump_<frame>_<k>.pgm`; the log line gives mean, mean frame-to-frame change and the share of pixels changing > 0.05 |
| `ringtest` | | 0 | 1: every pass on ring slot 0 (tests the busy-slot skip) |
| `recreate` | | | change the value: the pass's images are made again, as on a resize (old ones go deferred) |

## Measurements (RX 9070 XT, RADV, 2560x1440 render, FSR to 4K)

Measure with the camera still, `BB_GPU_PROFILE=1` (per-stage segments `rtao expand`, `rtao blas`,
`rtao tlas`, `rtao trace`, `rtao blur`, `rtao apply`) and the frame cap off (`BB_FPS_LIMIT=0`,
`BB_PRESENT_MODE=Mailbox`). The game paces itself to the 60 Hz vblank even then, so judge "GPU
busy" (frame time minus the profiler's "GPU idle" segment), not FPS. Session-to-session noise is
about 0.15 ms; A/B within one session with the live settings where possible.

| Spot | RT off GPU busy | RT on GPU busy | RT stages (expand + BLAS + trace + rest) |
|---|---|---|---|
| Hunter's Dream, the Doll (2026-10-14) | 13.4-13.7 ms | 16.6-16.8 ms | 3.9-4.1 ms (0.8 + 1.6 + 1.6 + ...) |
| Hunter's Dream start view, cache off / on (maxsize 20) | | | 3.08 / 2.90 ms |
| Looking straight down at cobblestones | | | trace alone 5.6 ms (before the changes below) |

Decisions measured at the Doll (A/B in one session unless noted):

| Change | Before | After |
|---|---|---|
| Trace as ray-tracing pipeline instead of ray queries in compute | trace 1.62-1.64 ms | 1.40-1.44 ms |
| Trace workgroups 8x4 instead of 8x8 (compute variant) | 1.78-1.81 ms | 1.77-1.78 ms (no effect, kept) |
| Per-frame BLAS without ALLOW_DATA_ACCESS (RDNA4 compressed pairs; three sessions A/B/A) | build 1.65, trace 1.79 ms | build 2.41, trace 1.73 ms: kept the flag |
| Cached BLASes compressed (no flag), cache on vs off | RT 4.53 ms | 4.20 ms |
| Depth-derived normals vs shading normals, self-hit share (pixels > 0.15 darker than with rays from 10 cm out; the snap was on in all four runs, a settings bug) | shading 6.4% / 9.1% | depth 4.4% / 6.2% |
| Snap ray on vs off (6 rays / 1 ray) | trace 1.43 / 0.27 ms | 1.55 / 0.50 ms |
| Trace vs ray count (pipeline, snap on) | 1 ray 0.50, 2 rays 0.70 ms | 6 rays 1.55, 12 rays 2.62 ms |
| Snap distance at 2 rays, Doll / looking down at the paving (pixels > 0.15 darker than snap 30 at 16 rays, 8-frame means; noise snap 30 vs 30: 0.85% / 0.55%) | snap 30: trace 0.71-0.75 / 0.70-0.78 ms | snap 0: 0.50 / 0.50 ms, darker 0.89% / 0.33%; 4 m: 0.55 / 0.54 ms, 0.84% / 0.20%: default 0, back on after the garden-wall flicker (still frames could not show it), 40 = `far` since distant geometry between 30 and 40 m flickered too |
| Denoiser 2 at 2 rays vs denoiser 1 at 6 (still camera, 8 frames) | GPU busy 16.63 ms, 4.40% of pixels change > 0.05, mean 0.787 | 15.68 ms, 2.78%, mean 0.786 |
| Denoiser 2 clip floor (2 rays) | 0: mean 0.803 (biased light: all-miss 3x3 neighbourhoods clipped history to 1) | 0.1: 0.787 (raw trace mean 0.791) |
| Static-cache probe in chunks of 4096 indices | probe 0.61 ms | < 0.1 ms |
| Defaults at the end of 2026-10-14 vs RT off | GPU busy 13.37 ms | 15.16 ms (60 FPS; BLAS 1.61, trace 0.69, expand 0.18 ms) |
| Big draws per frame (`chunk=0`) vs whole in the cache (`maxsize=0`) vs in pieces (Doll, snap off) | 14.90-15.03 ms (BLAS 1.60, trace 0.44-0.51) / 14.21-14.36 (BLAS 0.61, trace 0.86-0.89) | pieces of 5 / 10 / 20 / 40 m: 14.30 / 14.20-14.33 / 14.42 / 14.44 (trace 0.75 / 0.75 / 0.90 / 0.90) |
| The same at the workshop door (denser) | `chunk=0`: GPU busy 15.15 ms (BLAS 1.51, trace 0.81) | `chunk=10`: 15.05-15.06 (BLAS 0.71, trace 1.46) |
| One static BLAS instead of an instance per cached draw (lanterns lit, one session; RT off 13.59 ms) | Doll: `chunk=0` 14.67 ms (BLAS 1.44, trace 0.45) | static BLAS 13.70-13.75 (BLAS 0.55, trace 0.47-0.49) |
| The same at the workshop door (RT off 13.59 ms) | `chunk=0` 15.09 ms (BLAS 1.59, trace 0.71) | 14.24 (BLAS 0.82, trace 0.70) |
| Blue noise vs IGN, 2 rays, against a 32-ray reference (still camera, 8 frames each, two A/B pairs) | IGN: per-frame mean error 0.035-0.036, RMS 0.082-0.084, 8-frame mean 0.031-0.033 | blue: 0.023-0.031, RMS 0.054-0.070, 8-frame 0.019-0.027; stability (pixels changing > 0.05) equal, 3.44 vs 3.45%; same cost |
| Refits of the per-frame BLAS (characters; ALLOW_UPDATE, rebuilt every 30 frames; dropped) | still: BLAS 0.40, trace 0.62 ms; walking: trace 0.65-0.69 | still: 0.26 / 0.62; walking: trace 1.18-1.74 (bloated bounds); collapsed triangles at the camera hung the camera-ray test |
| Reusing a character's BLAS while its bone palette is unchanged (Doll, 600-frame windows; not built) | 104 skinned draws, 924k triangles per frame | palette unchanged for 5-9 % of draws, 1.5-1.9 % of triangles: every character animates every frame, idle included |
| Foliage (Hunter's Dream garden, ~370k alpha-tested triangles) | medium, foliage 0: RTAO 1.48 ms (BLAS 0.83, trace 0.66), GPU busy 13.35 | foliage 0.35: 2.16 ms (1.27, expand 0.11, 0.78), 13.85; high: 2.20 -> 2.91 ms. The per-frame BLAS sized for it: +74 MiB (1236 -> 1310 MiB fixed) |
| Launcher presets at the Doll (GPU busy; RTAO off 13.47 ms) | low (1 ray, 1 m, 25 m): 14.09 ms (BLAS 0.53, trace 0.43) | medium (2 rays, 1.5 m, 40 m): 14.47 (0.64, 0.67); high (4 rays, 2 m, 60 m): 15.02 (0.77, 1.12) |
| Static BLAS rebuild (`rebuild=0`: every pass; 956k triangles) | fast trace: 2.75 ms, fast build 2.06 | fast build + data access 1.39 ms, trace +0.03 ms: default |

Known cost drivers: the per-frame BLAS (the characters: 0.8M triangles at the Doll) is rebuilt from
a non-indexed expansion every frame; a static rebuild (1.4 ms) when the camera moved 8 m; every
pixel near the camera is traced, so looking at the ground is the worst case.

## Memory

Fixed buffers at the default budgets (RX 9070 XT, RADV):

| What | MiB |
|---|---|
| Per-frame BLAS (1.5M triangles, ~200 B per triangle with ALLOW_DATA_ACCESS) | 293 |
| Static BLAS (2M triangles, `staticbuild` at creation, plus a tenth) | 430 |
| BLAS build scratch (shared: the static build, then the per-frame one) | ~300 |
| Static pool (3M triangles, 36 B each) | 102 |
| Triangle list (1.5M per-frame + 0.4M cache copies) | 65 |
| TLAS, rings, probe buffers, host staging for splits | ~45 |
| Total reported (`RTAO: fixed buffers`) | 1236 |
| The RTAO's region of the object-motion capture (4M positions x 2 frames) | 128 |

The budgets scale with the device: at creation the pass reads the device-local heap's budget (VMA)
and keeps its buffers plus its capture region within 12 percent of it, shrinking the per-frame,
static and pool budgets alike; below a third of the defaults it stays off with a message
(`RTAO: device heap budget ...` in the log; `BB_RTAO_HEAP_MB=N` pretends a budget). An 8 GB card
(budget 7600 MiB): scale 0.69, 878 MiB, and the Hunter's Dream still fits the static BLAS. Where
everything fits, the per-frame budget grows (up to 2x) into what is left of 16 percent of the
budget (`Rtao::FrameGrowth`): crowded scenes overflowed 1.5M (the Central Yharnam crowd: 2.3M per
frame, 155 draws dropped, AO popping as the dropped set changed). The RX 9070 XT (13043 MiB): 3M
per frame, buffers 2012 MiB instead of 1387. A full budget costs time where it is used: 1.8 ms of
BLAS build in that crowd (57 fps; leaving out distant characters first would be the next step). The
texture collector sees these buffers in the device usage it compares (no separate report needed).

Nothing is allocated per cached draw any more. The buffers are made at the first frame with a
scene camera, not at start: made at start, before the game's own resources, the game's lights
went out (lanterns, candles; reproducibly from about 1.3 GiB, with VRAM far from full; the same
1.7 GiB made later left them lit). The cause in the port is not found yet.

## Feature combinations

One game start per row at the Doll (2026-10-15, `bbworld --measure`, 25 s in the world): RTAO's
log lines, the `Object motion:` stats line (600 frames), device losses, GPU busy, and the lantern
left of the Doll in a screenshot (lit: 49-68, out: ~17). Base: FSR 4.1.1 Quality, SSGI on, the
user's AO shader pack, SSAO on, layer memory off, draw pipe and async submit on. The Hunter's
Dream starts with or without the Doll at the stairs (game state, not the port): 55920 motion draws
with her, 48120 without; rows compare within the same case.

| Row | RTAO | Outcome | Motion draws / with history | GPU busy |
|---|---|---|---|---|
| Base | off | | 55920 / 55560 | 13.39 ms |
| Base | on | on, pass found by shader, static BLAS 957k | 55920 / 55560 | 13.83-13.87 ms |
| TAA | off | | 48120 / 47760 (no Doll) | 13.36 ms |
| TAA | on | on | 55920 / 55560 | 13.40 ms |
| No upscaler | on | off: "needs the object motion vectors" (object motion is off) | - | 12.82 ms |
| SSGI off | off | | 55920 / 55560 | 13.38 ms |
| SSGI off | on | on | 55920 / 55560 | 13.44 ms |
| No shader pack | on | on, pass found by shader | 55920 / 55560 | 13.81 ms |
| SSAO patched out | on | "OFF in practice" once; no capture (0 draws) | 48120 / 47760 (no Doll) | 13.36 ms |
| Layer memory | off / on | on; RTAO live off/on in one session: 13.82 / 15.42-15.57 ms | 55920 / 55560 | see left |
| Draw pipe off | on | on | 55920 / 55560 | 13.79 ms |
| Async submit off | on | on | 48120 / 47760 (no Doll) | 13.64 ms |

No device loss and lit lanterns in every row; the motion stats with RTAO on equal those with it
off in every row with the same scene. GPU busy understates RTAO where the frame has idle gaps
it fills: its own work is ~1.1 ms (segments 1.04 ms plus the AO upsample's difference); with
layer memory the frame is fuller and GPU busy grows by 1.6 ms (idle -1.66 ms). With no
upscaler the RT device extensions are still enabled (BB_RTAO=1 asks for them before object
motion is known to be off).

## Debugging

- **GPU hangs**: breadcrumbs name the stage (`STUCK: pass rtao <stage>`); `stage=N` stops after a
  stage to bisect. With `BB_FAULT_WAIT=1` the process stays alive for `tools/debug_attach.sh`.
- **Geometry**: `debug=2` traces one camera ray per pixel and writes `t/|P|` (white where the BVH
  sits where the depth buffer says); `dump=` writes the image, `1.0 = 128`. The 600-frame log line
  reports draws, triangles, budget drops, skipped draw kinds, passes recorded and skipped; the
  first pass after start reads back expanded triangles (valid share, depth range).
- **Oversized triangles**: every 120th pass the expand records each draw's largest triangle and its
  triangles over 4 m (centre to vertex); `RTAO: frame N: ... triangles over 4 m` names the worst
  per-frame draws when they hold 1000 or more; chains and cloth give ~200. Written after a trace
  blow-up in Central Yharnam (2.6 -> 15 ms, 30 fps, gone with far=15 and after the area reloaded;
  most likely a ragdoll gone wrong, not yet caught by this line).
- **Static cache**: the `RTAO: cache:` line (known draws, pool use, cached and evicted draws,
  static BLAS triangles, parts and rebuilds, probe matches/changes); `RTAO: big meshes in pieces:`
  (split time on the worker). The static build has its own profiler segment, `rtao static`.
- **Validation**: `BB_VK_VALIDATION=1` (core), `BB_VK_VALIDATION_SYNC=1` (synchronization),
  `BB_VK_VALIDATION_GPU=1` (GPU-assisted, about 25 FPS). RTAO objects carry debug names (`rtao
  trace`, `rtao triangles`, `rtao TLAS`, ...), so a message about them names them. To separate
  RTAO from the rest of the port, run once with `enabled=0` in `rtao.txt` from the start and
  compare the sets of message IDs. The layer reports each ID a limited number of times.
  State of 2026-10-14 at the Doll: no message names an RTAO object and GPU-AV found nothing in 453
  passes. Expected noise from the port: the motion capture's fragment input at location 31 (over
  128 components) and its unbound attachment 6, a tessellation interface mismatch, FSR 4 cooperative matrices, a 4 GiB buffer,
  an sRGB storage view, present and render-pass-load hazards.

## Known limitations

- Foliage casts AO only stochastically (`foliage`, at an average coverage; no alpha texture is
  tested) and only from the high preset on; over its budget (375k triangles) draws drop out (4 in
  the Dream garden, 22 at the Yharnam start). Decals are left out.
- RTAO darkens everywhere, also where the game scales its own AO to zero (the Workshop): a
  chosen look (compared in game against honouring the per-area scale), not an oversight.
- Strips and other non-list topologies are skipped (counted in the log).
- The capture's positions are post-vertex-shader: anything the game moves in a later stage is not
  seen.
- With the game's SSAO patched out there is nothing to write into. RTAO writes only into the mask
  of the pass identified by its upsample shader (0xc45c51cb); if a game update or a pack changed
  that hash, the log says so once and nothing is written.
