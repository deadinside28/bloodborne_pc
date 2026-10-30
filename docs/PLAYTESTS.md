# Replay playtests

Reproducible test runs of the game: a frozen save, a recorded pad route, the port's own logging,
frame dumps, and an evaluator that turns them into pass / fail / invalid. The runner and the
engine switches live here; the saves, routes, test definitions and golden images live in a
separate library of your own (`--lib`, `$BB_PLAYTESTS`, default `../bbport-playtests` next to
this checkout), because they are your own game data: nothing of it is in the repository.

The tools run on Linux: they need `bash`, `pgrep` (procps) and Python's `fcntl`, and say so when
one is missing. The game itself also runs on Windows; these tools do not.

## First steps

You need a working build (`./build.sh`) and the launcher set up as you play (game folder, mods,
patches, screen): the tools take the game environment from the launcher
(`launcher/bbport_launcher.py --print-env`).

1. Record a first test. `tools/playtest.py record` is a wizard: it names the test, freezes the
   save you are in (quit the game where the test should start first: Bloodborne saves your
   position when you quit), starts the game there and records your route: press **F9** to start
   recording, play the route, **F9** again to stop, then close the game. The first `record` also
   creates the library (`tools/playtest.py init` does only that).
2. Run it: `tools/playtest.py run <name>`. The game starts on a scratch copy of the save, replays
   your route by itself, quits, and the evaluator prints pass / fail / invalid with the reasons
   (`results/<name>/<time>/summary.md` in the library).
3. Add what the test should check to `tests/<name>.toml` (dumps, criteria: see below) and run it
   again; `--repeat N` shows how stable it is.

A route is pad input with times, not positions: it replays the same way on the same build, but a
change to how the game moves (frame timing, physics, the camera) can make it end elsewhere. The
evaluator marks such a run **invalid** (it never passes a run that went off its route); record the
route again for the new version then.

```
tools/playtest.py run <test> [--repeat N] [--accept-goldens] [--accept-baseline] [--suite bugs|all] [--env KEY=VALUE]
tools/playtest.py eval <results-dir>        evaluate a run again
tools/playtest.py compare <test> [--last N] medians of the last runs by commit
tools/playtest.py report [--suite bugs|all] one table across tests: status + metrics + Δ vs baseline
tools/playtest.py ab <test> --a K=V --b K=V run two settings and diff metrics + image quality
tools/playtest.py record [<name>]           create a playtest, or record a test's route (wizard)
tools/playtest.py edit <name>               change a playtest's save/description/ticket/env/delay
tools/playtest.py remove <name>             delete a playtest (keeps the frozen save)
tools/playtest.py saves                      list the frozen saves and where they are
tools/playtest.py lib-sync [--push]          commit the private library (push if it has a remote)
tools/playtest.py list
tools/playtest_eval.py <results-dir> --test <toml> [--lib <dir>]
```

## How a run works

1. The runner refuses when a `bb-probe` it did not start is running (the player's session), and
   takes `out/playtest.lock`.
2. `out/playtest_user/` is the player's `user/` without its saves (made once, so the shader and
   pipeline caches stay warm); the test's frozen save is copied into it before every run and the
   test's `[settings.*]` tables are written into its live settings files (`user/<name>.txt`;
   the next run restores them). The player's `user/` is never touched.
3. The environment, outermost last: the **reproduction point** — `routes/<route>.env`, the launcher
   settings captured when the route was recorded (fullscreen, HDR, mods, patches, shader packs), so a
   replay matches what was recorded; a test with no such snapshot (older or stand tests) falls back to
   `env/<env_base>.env` windowed as before. Then the measurement instrumentation (`BB_FRAME_STATS=1
   BB_GPU_PROFILE=1 BB_CAMERA_LOG=1 BB_FAULT_WAIT=0 BB_SAVE_LOG=0`, `BB_FRAME_LOG` into the results
   folder, `BB_TIMEOUT` as a hard cap) — logging only, nothing that changes rendering — then the
   replay variables, then the test's `[env]`, then any `--env KEY=VALUE` overrides. So the default is
   an exact reproduction, and `[env]`/`--env` are for investigating with settings changed on purpose.
4. `tools/restart.sh` starts the game, presses cross at the title and returns when the level has
   loaded (`Frame stats` with more than 600 draws a frame, plus 10 s).
5. The replay. With the engine switches below the engine arms it itself after the level and
   quits itself after the route; with an older engine the runner sends the `replay` token through
   `BB_PAD_FILE`, touches the dump triggers by wall time and stops the game with `tools/stop_own.sh`
   (`end_mode: legacy` in `run.json`; the clean-end criterion is then not applied).
6. `results/<test>/<stamp>/`: `session.log`, `frames.csv`, `run.json` (commit, branch, dirty,
   environment, timing), `dumps/<name>.png` (raw dumps converted with `magick`), `result.json`,
   `summary.md`.

## What the evaluator reads

- `GPU profile:` blocks (every 5 s): the median per label and the busy time (total minus the
  `submission gaps`) over the replay window (the blocks after the replay started, minus the first).
  Labels are matched by text or by a unique substring; shader hashes without their zero padding
  (`cs 3d5ebf4e`). `per_call` labels get µs per dispatch (ms/frame ÷ dispatches/frame), and with
  `split_ms` a before/after median and their ratio (5 s per block from the replay start).
- `Frame pacing`, `Frame stats` (FPS, shader compiles: more than `max_compiles` during the replay
  means cold caches and an invalid run), `frames.csv` (median, p95, p99, max of `frame_ms`).
- `Camera:` lines against the route's `.camera` track at the test's checkpoints: a distance over the
  tolerance makes the run invalid (off-route), never a failure.
- `RT reflections: N passes in 600 frames`, `RT reflections: first pass`, `RT governor: level`,
  `Runtime: pad replay …`, `Final dump`, `Window: closed` + `Exit: GPU work finished` (clean end),
  error lines (`Fault (signal`, `Device lost`, `STOP`, `Assertion`, `Unimplemented PM4`).
- Dumps against `golden/<test>/<name>.png`: mean absolute difference per channel and the fraction
  of pixels with any channel off by more than 8, outside the `[masks]` rectangles (HUD).

Exit codes: 0 pass, 1 fail or crash, 2 invalid, 3 tool error.

## Test definition (`tests/<name>.toml` in the library)

```toml
name = "cathedral-walk"          # default: the file name
description = "..."
ticket = "..."                   # bug tests: the issue this guards
save = "cathedral"               # saves/<save>/savedata
route = "cathedral-there-back"   # routes/<route>.pad (+ .camera, .env); omitted: stand for duration_s
env_base = "default"             # env/<env_base>.env
duration_s = 90                  # the route's length
delay_ms = 10000                 # replay start after the level
hold_s = 5                       # seconds after the route before the quit
loop = 0                         # extra replays of the route (soak)
[env]                            # BB_* overrides
[effects]  ssr = 1  ssao = 1     # game-effect toggles -> a scratch bbport.ini via BB_CONFIG (effect_<name>)
[settings.<name>]                # live settings file user/<name>.txt of the scratch user
[measure]  gpu_labels = [...]  per_call = [...]  split_ms = 25000
[[checkpoints]]  at_ms = 30000  tolerance_m = 1.5
[[dumps]]        at_ms = 30000  kind = "final"  name = "cp1"   # or: on = "YEBIS"  (dump when that log line appears)
[masks]  hud = [x, y, w, h]      # fractions of the output
[criteria]  crash_free  clean_end  max_busy_ms  max_p99_ms  max_label_ms = {label = ms}
            per_call_ratio_max = {label = ratio}  min_reflection_passes  require_reflections_first_pass
            no_error_lines  image_mad_max  image_diff_frac_max  max_compiles  regression_pct
```

## Baselines and the regression gate

Absolute `[criteria]` thresholds catch gross failures; a **baseline** catches creeping regressions.
`run <test> --accept-baseline` stores the run's metrics (busy, p99, measured label medians and
per-call µs) in the library as `baselines/<test>.json`, tagged with the commit. On later runs, the
median across this invocation's `--repeat N` runs is compared to that baseline, and any metric risen
above it by more than `regression_pct` (default 5, per-test overridable) makes the test **REGRESSED**
(a failure, with the offending deltas printed). Improvements never fail. Re-accept the baseline on a
new known-good commit to move it.

**Setting all baselines at once** (e.g. when a new stable release lands): `run --suite all
--accept-baseline` runs every test and writes each one's baseline in a single pass (the gate is
skipped while accepting). A suite run skips and continues past tests it cannot run — a route not yet
recorded, say — rather than aborting, so one unrecorded route does not stop the rest; those show as
SKIPPED and the run exits non-zero to flag that not everything baselined.

`report [--suite all|bugs]` prints one table — `test | status | busy | p99 | Δbusy | Δp99 | notes` —
from each test's latest result, and exits non-zero if anything fails or regressed; `run --suite all`
emits the same report and writes it to `results/report-<stamp>.md`. This is the "is this merge
stable?" check.

## A/B and image quality (investigating changes)

`ab <test> --a KEY=VALUE… --b KEY=VALUE…` runs the test under two settings on the current build and
writes `results/ab/<test>/<stamp>.md`: the metrics side by side (busy, p99, each measured label, with
Δ and Δ%), and — if the test takes `[[dumps]]` — the **image quality** between the paired dumps
(MAD, PSNR and SSIM), flagging any below `--min-ssim` (default 0.98) as a quality degradation. Use it
to judge an RT/optimisation change: e.g. `ab cathedral-goldens --a BB_RTAO=1 --b BB_RTAO=0`, or two
commits by building each and running `ab` with the same `--a/--b` on each. `--env` on a plain `run`
makes the same ad-hoc change without touching the recorded reproduction point.

## Event-triggered capture

A `[[dumps]]` entry with `on = "<log substring>"` (instead of `at_ms`) dumps the next frame when that
line first appears — the YEBIS flicker assert, `Device lost`, an error — and records the event (the
line and its time) in `result.json`. The capture is the next frame after the marker (within the
~0.5 s poll), which is right for a multi-frame stall; a single-frame event would need an engine-side
trigger (future).

## Backing up the library

The library holds the saves, routes, goldens and baselines the gate
depends on — back it up. `lib-sync [-m msg]` commits it; with a remote configured it also pushes on
`--push`. It has no remote by default; add a **private** one (never the public fork) before pushing.

## Engine switches (`src/runtime_pad/sample.c`)

Recording and replay: `BB_PAD_RECORD=<file>` (F9 starts and stops; lines `ms buttons lx ly rx ry l2
r2`), `BB_PAD_REPLAY=<file>` (started by the token `replay` in `BB_PAD_FILE`, or by autostart below),
`BB_PAD_FILE` tokens,
`BB_CAMERA_LOG=1` (`Camera:` every 100 ms), `BB_FINAL_DUMP_TRIGGER` / `BB_PRESENT_DUMP_TRIGGER`
(a file the engine removes when it dumps the next frame into `BB_DUMP_DIR`).

Replay autostart (so a run needs no `replay` token and quits by itself) arms once the scene has
loaded — **two consecutive seconds with more than `BB_PAD_SCENE_DRAWS` (default 600) draws a
frame** — plus a short settle:
- `BB_PAD_REPLAY_AUTOSTART=1` + `BB_PAD_REPLAY_DELAY_MS` (default **3000**): arms the replay; prints
  `Runtime: pad replay armed (…)` then `Runtime: pad replay started (mono %.3f s)`.
- `BB_PAD_REPLAY_END=quit[:hold_s]` (default hold 5): after the route (and `hold_s`), one SDL quit,
  so the run ends on `Window: closed (quit)` / `Exit: GPU work finished` (a clean end).

- `BB_PAD_REPLAY_LOOP=N`: the route N more times after its end (a soak test).
- `BB_PAD_REPLAY_LEAD=keep`: start at the file's first sample, the standing lead included (since
  0.7 a replay starts at the first input that is not neutral).

While a replay plays, the game's camera auto-rotation stays on even with mouse look and the window
holding the mouse (#352 turns it off then): a route is stick input and was recorded with it, and
whether a test window had taken the mouse differed from run to run.

Recording is **F9-started** (press F9 to start, F9 again to stop): the player controls when it
begins, so nothing is missed while getting set at the spawn. There is no record autostart: a timer
would fire before the player was ready and drop early inputs. Camera samples are matched to the
route from the first `Camera:` line after `pad replay started`.

The runner also passes `BB_LEVEL_SETTLE` (default 3) to `tools/restart.sh` to shorten its own
post-level settle, since the engine's scene-load check now governs when the replay/recording begins;
together these bring the start to roughly five seconds after the level loads. Frame dumps at route
times (`[[dumps]] at_ms`) are fired by the runner, timed from the engine's `pad replay started
(mono S s)` line.

The screen comes from the launcher's current settings (fullscreen, display), whatever a recording
says: the test game opens where you play. `BB_PLAYTEST_PORT=<checkout>` runs another checkout's
build with the same library and settings (an A/B between builds); it uses that checkout's
`patches/`.

## Creating a playtest (the wizard)

Run `tools/playtest.py record` with no arguments at a terminal and it guides you through the whole
thing:

1. **Name** the playtest (lower-case, digits, dashes). It becomes the test and route name.
2. **Pick a save** from a numbered menu — each frozen save is shown with where it is (`saves` prints
   the same list). Option `0` is **"current game state"**: save in-game (a lamp, or quit to the main
   menu), fully close the game, and it freezes your live `user/savedata` into the library as a new
   save, writing its `NOTES.md` for you.
3. **Record the route**: the game starts on that save and enters the level by itself, **using your
   current launcher settings** (fullscreen, HDR, mods, patches, shader packs — read live from
   `bbport_launcher.py --print-env`), so you see the issue exactly as you normally would. When you
   are oriented and ready, press **F9 to start recording** (nothing is recorded before that — take
   your time getting set at the spawn), play the route with 2 s pauses at the checkpoints, come back
   to the start, stand 2 s, and press **F9 again to stop**. (If the launcher can't be queried, it
   falls back to `env/<base>.env` and says so.)
4. **Describe** it in one line, and optionally give a **bug ticket** (which files the test under
   `tests/bugs/<ticket>-<name>.toml`).

It writes `routes/<name>.{pad,camera,toml,env}` (`.env` = the launcher settings used, the reproduction point) and a runnable `tests/<name>.toml` — `crash_free` and
`clean_end` are on, and the measurement labels and checkpoints are left as commented TODOs (with the
recorded checkpoint candidates) for you to fill in. Then `tools/playtest.py run <name>`.

Keep routes short (60 to 90 s): the simulation runs on real time and positions drift between runs;
the checkpoints catch a drifted run as invalid.

**Shorter forms.** `record <existing-test>` re-records just that test's route (save and route come
from its TOML — no wizard). Every step has a flag (`--save`, `--description`, `--ticket`,
`--env-base`, `--delay-ms`, `--force`) that fills it and skips the prompt; when stdin is not a
terminal (agents, CI) the flags are required and nothing is ever prompted.

## Editing and removing a playtest

`tools/playtest.py edit <name>` changes a playtest's metadata — save, description, ticket, env base,
replay delay — in place. At a terminal with no flags it asks each field with the current value as the
default (press Enter to keep it); the save step is the same menu as `record`, including **"current
game state"** to freeze a new save and point the test at it. Flags (`--save`, `--description`,
`--ticket`, `--env-base`, `--delay-ms`) set fields directly and are the only way off a terminal. Any
hand-edited `[measure]`/`[criteria]`/`[checkpoints]` (and comments) are left untouched.

**Changing the save re-records the route.** A route is played from its save's spawn, so a new save
leaves the old inputs out of sync. When `edit` changes the save (including a freeze), it launches a
re-recording of the route on the new save right away (F9 to start/stop, as in `record`) and syncs the
test's `duration_s` to the new length. Off a terminal, or with a game already running, it refuses and
tells you to run `record <name> --force` when you can. (A stand test — no route — needs no re-record.)
To rename a playtest, remove it and record again.

`tools/playtest.py remove <name>` deletes the test TOML, its route files (only when no other test
uses that route), and its `golden/`/`results/`. The **frozen save is kept** (other tests may share
it). It confirms first; off a terminal pass `--yes`. `delete` is an alias.
