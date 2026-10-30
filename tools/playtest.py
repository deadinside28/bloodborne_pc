#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay playtests: runs the game on a frozen save with a recorded pad route, collects the log,
the per-frame CSV and frame dumps, and evaluates them (tools/playtest_eval.py).

  tools/playtest.py run <test> [--repeat N] [--accept-goldens] [--keep-raw] [--lib DIR]
  tools/playtest.py eval <results-dir>
  tools/playtest.py compare <test> [--last N]
  tools/playtest.py record <route> --save <spot> [--delay-ms 3000]
  tools/playtest.py list

The library (saves, routes, tests, golden images, results) is a separate, private directory:
--lib, $BB_PLAYTESTS, or ../bbport-playtests next to the port. Its layout:
  env/<name>.env  saves/<spot>/savedata/  routes/<r>.pad|.camera|.toml  tests/<t>.toml
  tests/bugs/<ticket>-<slug>.toml  golden/<t>/<dump>.png  results/<t>/<stamp>/
The game runs on a scratch copy (out/playtest_user: the player's user/ without its saves, so the
shader caches stay warm) with the frozen save copied in before every run. A game these tools did
not start (the player's session) is never touched: the run is refused.

Engine support (src/runtime_pad/sample.c):
  BB_PAD_REPLAY_AUTOSTART / BB_PAD_REPLAY_DELAY_MS  the replay starts by itself after the level
  BB_PAD_REPLAY_END=quit:<hold>                     the game ends by itself after the route
  BB_PAD_REPLAY_LOOP=<n>                            the route played n more times
Frame dumps are timed here, from the engine's "pad replay started (mono S s)" line.

Linux (and other systems with bash, pgrep and fcntl) only: the game itself runs on Windows too.
"""
from __future__ import annotations

import argparse
import datetime as dt
try:
    import fcntl
except ImportError:   # Windows: checked in main() with a message
    fcntl = None
import json
import os
import re
import copy
import shutil
import subprocess
import sys
import textwrap
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import playtest_eval as ev  # noqa: E402

# HOME: this checkout, whose user/ and bbport.ini are the player's. PORT: the checkout whose build
# runs (BB_PLAYTEST_PORT, e.g. a worktree of another branch for an A/B between builds).
HOME = Path(__file__).resolve().parent.parent
PORT = Path(os.environ.get("BB_PLAYTEST_PORT") or HOME).expanduser().resolve()
OUT = PORT / "out"
# [settings.<name>] / --setting <name>.<key>=<value>: user/<name>.txt, a live settings file the game
# re-reads while it runs. The names a run changed are noted here, so the next run restores them.
SETTINGS_NOTE = ".playtest_settings"
RE_SETTINGS_NAME = re.compile(r"^[A-Za-z0-9_-]+$")
# BB_HDR is NOT forced here: forcing SDR made the game skip its SSAO pass (the one RTAO hooks,
# pixel shader 0xc45c51cb), so RTAO never engaged. The display config comes from env/<base>.env
# and the test, matching the player's real run. The test game always runs fullscreen on screen 1
# (BB_DISPLAY=1; resolve_repro_env).
# Invisible instrumentation layered on top of the reproduction settings for a measured run: logging
# and automation only, nothing that changes what is rendered. restart.sh settles briefly after the
# level (BB_LEVEL_SETTLE); the engine's scene-load check + BB_PAD_REPLAY_DELAY_MS then govern timing.
MEASURE_ENV = {"BB_FRAME_STATS": "1", "BB_GPU_PROFILE": "1", "BB_CAMERA_LOG": "1",
               "BB_SAVE_LOG": "0", "BB_SKIP_NETWORK_CHOICE": "1", "BB_LEVEL_SETTLE": "3"}
# Env keys naming files or folders the game silently does without (see env_path_problems).
PATH_KEYS = ("BB_GAME_DIR", "BB_MODS_DIR", "BB_MODS_CONFIG", "BB_PATCHES_DIR", "BB_PATCHES_CONFIG", "BB_CONFIG")
# Keys dropped when saving a recording's launcher snapshot (routes/<route>.env): session/path
# specifics and instrumentation, which the runner sets itself. What stays is the visual/effect/perf
# settings — the actual reproduction point (fullscreen, HDR, mods, patches, shader packs, …).
REPRO_SKIP = {"BB_USER_DIR", "BB_PAD_FILE", "BB_PAD_RECORD", "BB_PAD_REPLAY", "BB_DUMP_DIR",
              "BB_FINAL_DUMP_TRIGGER", "BB_PRESENT_DUMP_TRIGGER", "BB_CAPTURE_TRIGGER",
              "BB_CAPTURE_DIR", "BB_FRAME_LOG", "BB_FRAME_STATS", "BB_GPU_PROFILE", "BB_SAVE_LOG",
              "BB_CAMERA_LOG", "BB_LEVEL_SETTLE"}
RE_REPLAY_MONO = re.compile(r"Runtime: pad replay started \(mono ([0-9.]+) s\)")
RE_DUMP_FILE = re.compile(r"f(\d+)_(final|present)_(\d+)x(\d+)_(rgba|bgra)\.raw$")
_SENTINEL = object()   # "no argument given", where None is a meaningful value


LIB_DIRS = ("tests", "tests/bugs", "routes", "saves", "env", "results", "goldens", "baselines")


def library_path(arg: str | None) -> Path:
    return Path(arg or os.environ.get("BB_PLAYTESTS") or HOME.parent / "bbport-playtests").expanduser()


def init_library(lib: Path) -> bool:
    """Creates the library's folders and env/default.env (the launcher's current game environment)
    where they are missing. Returns True when it made anything."""
    made = False
    for d in LIB_DIRS:
        if not (lib / d).is_dir():
            (lib / d).mkdir(parents=True)
            made = True
    gitignore = lib / ".gitignore"
    if not gitignore.exists():
        gitignore.write_text("results/\n")
        made = True
    default_env = lib / "env" / "default.env"
    if not default_env.exists():
        env = launcher_env() or {}
        lines = ["# The game environment for tests without a recorded launcher snapshot",
                 "# (routes/<route>.env), from the launcher's settings when it was made",
                 "# (launcher/bbport_launcher.py --print-env). Edit or regenerate as needed."]
        lines += [f"{k}={v}" for k, v in sorted(env.items())]
        default_env.write_text("\n".join(lines) + "\n")
        made = True
    return made


def library(arg: str | None, create: bool = False) -> Path:
    lib = library_path(arg)
    if create and init_library(lib):
        print(f"playtest library ready at {lib}")
    if not (lib / "tests").is_dir():
        raise SystemExit(f"no playtest library at {lib} yet: record a first test with "
                         f"`tools/playtest.py record` (it creates the library), or run "
                         f"`tools/playtest.py init` (--lib or BB_PLAYTESTS for another place)")
    return lib


def find_test(lib: Path, name: str) -> Path:
    for cand in (Path(name), lib / "tests" / f"{name}.toml", lib / "tests" / "bugs" / f"{name}.toml",
                 lib / "tests" / name, lib / "tests" / "bugs" / name):
        if cand.is_file():
            return cand
    # Fall back to the `name` field inside the TOMLs (what the summary/report show may differ from
    # the file stem, e.g. a bug test filed as tests/bugs/<ticket>-<name>.toml).
    for tp in all_test_paths(lib):
        try:
            if ev.load_test(tp).get("name") == name:
                return tp
        except Exception:  # noqa: BLE001
            continue
    raise SystemExit(f"test {name} not found under {lib / 'tests'}")


def read_env_file(path: Path) -> dict:
    env = {}
    try:
        text = path.read_text()
    except OSError:
        raise SystemExit(f"env file {path} missing")
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:]
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        env[k.strip()] = v
    return env


def resolve_repro_env(lib: Path, test: dict) -> tuple:
    """The base environment for a measured run: the recording's launcher snapshot
    (routes/<route>.env, the reproduction point) when it exists, else env/<base>.env windowed as
    before; then the invisible measurement instrumentation on top. The caller still layers the
    replay/dump variables and, last, the test's [env] and any --env overrides. Returns (env, source)
    where source is the snapshot path when one was used, else None."""
    env = dict(os.environ)
    route = test.get("route")
    repro = (lib / "routes" / f"{route}.env") if route else None
    if repro and repro.is_file():
        env.update(read_env_file(repro))
        source = f"routes/{route}.env"
    else:
        env.update(read_env_file(lib / "env" / f"{test.get('env_base', 'default')}.env"))
        source = None
    # The screen as the launcher has it now (its fullscreen and display settings), whatever the
    # recording or env file says: a window can't be placed on Wayland, and a windowed test game
    # opened on whichever screen the compositor chose.
    live = launcher_env() or {}
    env["BB_FULLSCREEN"] = live.get("BB_FULLSCREEN", "1")
    if live.get("BB_DISPLAY"):
        env["BB_DISPLAY"] = live["BB_DISPLAY"]
    else:
        env.pop("BB_DISPLAY", None)   # the primary display
    # A build from another checkout (BB_PLAYTEST_PORT) takes its own patches/: 0.7 skips the built-in
    # patches/Bloodborne.xml only in its own checkout and applied every default-on patch of this
    # checkout's copy as an external file (a 21:9 resolution patch among them).
    if PORT != HOME and Path(env.get("BB_PATCHES_DIR", "")).resolve() == (HOME / "patches").resolve():
        env["BB_PATCHES_DIR"] = str(PORT / "patches")
    env.update(MEASURE_ENV)
    return env, source


def launcher_env() -> dict | None:
    """The environment the launcher would use right now (its bbport.ini settings), via
    `bbport_launcher.py --print-env`. So a recording matches what the player normally sees —
    fullscreen, HDR, the current mods/patches/shader packs — rather than a stale library snapshot.
    Returns None if the launcher can't be queried (then the caller falls back to env/<base>.env)."""
    launcher = PORT / "launcher" / "bbport_launcher.py"
    if not launcher.is_file():
        return None
    try:
        r = subprocess.run([sys.executable, str(launcher), "--print-env"],
                           capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    env = {}
    for line in r.stdout.splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            env[k.strip()] = v
    return env or None


def write_live_settings(path: Path, values: dict):
    """Updates key=value lines of a live settings file, keeping other keys (as the launcher)."""
    lines = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        pass

    def fmt(v):
        return f"{v:g}" if isinstance(v, float) else str(v)
    seen, out = set(), []
    for line in lines:
        key = line.split("=", 1)[0].strip()
        if key in values:
            out.append(f"{key}={fmt(values[key])}")
            seen.add(key)
        else:
            out.append(line)
    for key, value in values.items():
        if key not in seen:
            out.append(f"{key}={fmt(value)}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def foreign_game_running() -> list[int]:
    """bb-probe pids these tools did not start."""
    try:
        pids = [int(p) for p in subprocess.run(["pgrep", "-x", "bb-probe"], capture_output=True, text=True).stdout.split()]
    except (OSError, ValueError):
        return []
    try:
        own = int((OUT / "tools_game.pid").read_text().strip())
    except (OSError, ValueError):
        own = -1
    return [p for p in pids if p != own]


def prepare_user(lib: Path, save: str, settings: dict, user_dir: Path, reset: bool = False) -> None:
    """out/playtest_user: the player's user/ without saves (once), the frozen save copied in,
    the live settings written. Never the player's user/ itself."""
    src = lib / "saves" / save / "savedata"
    if not src.is_dir():
        raise SystemExit(f"save {save} not found at {src}")
    if reset and user_dir.exists():
        shutil.rmtree(user_dir)
    if not user_dir.exists():
        player = HOME / "user"
        user_dir.mkdir(parents=True)
        if player.is_dir():
            for entry in player.iterdir():
                if entry.name.startswith("savedata"):
                    continue
                if entry.is_dir():
                    shutil.copytree(entry, user_dir / entry.name, symlinks=True)
                else:
                    shutil.copy2(entry, user_dir / entry.name)
    dst = user_dir / "savedata"
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    for name in settings:
        if not RE_SETTINGS_NAME.match(name):
            raise SystemExit(f"bad settings file name [settings.{name}] (user/<name>.txt: letters, digits, _ and -)")
    # Every run starts from the player's live settings again: a run's own values (the test's or
    # --setting) would otherwise stay in the scratch files and carry into the next run.
    note = user_dir / SETTINGS_NOTE
    changed_before = note.read_text().split() if note.is_file() else []
    for name in set(changed_before) | set(settings):
        own, player = user_dir / f"{name}.txt", HOME / "user" / f"{name}.txt"
        if player.is_file():
            shutil.copy2(player, own)
        else:
            own.unlink(missing_ok=True)
    for name, values in settings.items():
        write_live_settings(user_dir / f"{name}.txt", values)
    note.write_text("\n".join(sorted(settings)) + "\n" if settings else "")


def git_info() -> dict:
    def run(*args):
        try:
            return subprocess.run(["git", *args], cwd=PORT, capture_output=True, text=True, timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return ""
    return {"commit": run("rev-parse", "HEAD"), "branch": run("rev-parse", "--abbrev-ref", "HEAD"),
            "dirty": bool(run("status", "--porcelain", "--untracked-files=no"))}


def pid_alive(pid: int) -> bool:
    try:
        return (Path(f"/proc/{pid}/comm").read_text().strip() == "bb-probe")
    except OSError:
        return False


def own_pid() -> int:
    try:
        return int((OUT / "tools_game.pid").read_text().strip())
    except (OSError, ValueError):
        return -1


def press(tokens: str, hold: float = 0.3):
    subprocess.run(["bash", str(PORT / "tools" / "press.sh"), tokens, str(hold)], check=False)


def stop_own():
    subprocess.run(["bash", str(PORT / "tools" / "stop_own.sh")], check=False)


RE_DRAWS_LINE = re.compile(r"^Frame stats: .*?(\d+) draws/frame")
LEVEL_DRAWS = 600   # restart.sh's own threshold for "the level has loaded"
TITLE_DRAWS = 150   # at or below this the game is still in the title menu; above it is loading


def latest_draws(lines) -> int | None:
    for line in reversed(lines):
        m = RE_DRAWS_LINE.match(line)
        if m:
            return int(m.group(1))
    return None


def start_game(env: dict, name: str, watch: "LogWatch", timing: dict):
    """tools/restart.sh in the background, watched from here: since 0.6 the port no longer skips
    the title by itself, so restart.sh's single cross only reaches the main menu; the runner
    presses cross again while the draw count stays at menu level. The game dying ends the wait at
    once. Returns (pid, level_reached, restart.sh's output)."""
    session = OUT / "session.log"
    session.write_text("")
    watch.pos = 0
    if not pid_alive(own_pid()):  # a previous run's pid file: not this game's
        (OUT / "tools_game.pid").unlink(missing_ok=True)
    proc = subprocess.Popen(["bash", str(PORT / "tools" / "restart.sh")], env=env, cwd=PORT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = time.time()
    pid, level, seen_presses, extra, last_press = -1, False, 0, 0, None
    while True:
        watch.poll()
        if pid < 0:
            cand = own_pid()
            if cand > 0 and pid_alive(cand):  # restart.sh wrote this game's pid
                pid = cand
        draws = latest_draws(watch.lines)
        if draws is not None and draws > LEVEL_DRAWS:
            level = True
        presses = watch.count("Runtime: pad file: buttons 0x4000")
        if presses > seen_presses:
            seen_presses, last_press = presses, time.time()
        # Press cross again only while the draw count is still at title-menu level: a higher count
        # means the level is already loading, and a cross there would open the in-game menu.
        if (pid > 0 and not level and last_press and time.time() - last_press > 8 and extra < 3
                and draws is not None and draws <= TITLE_DRAWS):
            extra += 1
            print(f"[{name}] still in the title menu ({draws} draws/frame): cross again ({extra})")
            press("cross")
            last_press = time.time()
        if proc.poll() is not None:
            break
        if pid > 0 and not pid_alive(pid) and time.time() - t0 > 15:
            timing["note"] = "the game ended before the level loaded"
            proc.kill()
            break
        if time.time() - t0 > 900:
            timing["note"] = "restart.sh did not return in 15 minutes"
            proc.kill()
            break
        time.sleep(0.5)
    out = ""
    try:
        out = proc.stdout.read() if proc.stdout else ""
    except OSError:
        pass
    watch.poll()
    draws = latest_draws(watch.lines)
    level = level or (draws is not None and draws > LEVEL_DRAWS)
    return pid, level, out


class LogWatch:
    """Reads out/session.log incrementally and remembers the first line matching each marker."""

    def __init__(self, path: Path):
        self.path, self.pos, self.lines = path, 0, []

    def poll(self):
        try:
            with self.path.open("rb") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except OSError:
            return
        if data:
            self.lines.extend(data.decode(errors="replace").splitlines())

    def has(self, prefix: str) -> bool:
        return any(line.startswith(prefix) for line in self.lines)

    def count(self, prefix: str) -> int:
        return sum(1 for line in self.lines if line.startswith(prefix))


def dump_names(dumps: list) -> dict:
    """{kind: [png names in dump order]} for a test's [[dumps]]. A dump with count = N > 1 (a burst
    of N consecutive frames, kind present only) yields <name>_000 .. <name>_<N-1>."""
    names = {"final": [], "present": []}
    for i, d in enumerate(dumps):
        name, count = d.get("name", f"dump{i}"), int(d.get("count", 1))
        names[d.get("kind", "final")].extend([name] if count <= 1 else [f"{name}_{j:03d}" for j in range(count)])
    return names


def env_path_problems(env: dict) -> list[str]:
    """Configured files and folders that do not exist. A missing mods.json does not fail the game: it
    enables every mod in BB_MODS_DIR (2026-10-25: bb_enhanced, which removes the game's AO pass, so
    RTAO had nothing to write into), so such a run measures another game and must not start."""
    return [f"{k}={env[k]} does not exist" for k in PATH_KEYS if env.get(k) and not Path(env[k]).exists()]


def convert_dumps(raw_dir: Path, results: Path, names, keep_raw: bool):
    """Raw frame dumps (fNNN_<kind>_WxH_fmt.raw) -> results/dumps/<name>.png. The final and present
    dumps count separately (each kind's index starts at 0); names is {kind: [names]} or, for the
    final dumps alone, a list."""
    if isinstance(names, list):
        names = {"final": names}
    dumps = results / "dumps"
    dumps.mkdir(exist_ok=True)
    raws = sorted(p for p in raw_dir.glob("f*_*.raw") if RE_DUMP_FILE.search(p.name))
    produced = []
    seen = {"final": 0, "present": 0}
    for raw in raws:
        m = RE_DUMP_FILE.search(raw.name)
        kind = m.group(2)
        i = seen[kind]
        seen[kind] += 1
        kind_names = names.get(kind, [])
        name = kind_names[i] if i < len(kind_names) else (f"dump{i}" if kind == "final" else f"present{i}")
        png = dumps / f"{name}.png"
        size, fmt = f"{m.group(3)}x{m.group(4)}", m.group(5)
        if kind == "present" and fmt == "rgba":
            # The game's display buffer holds BGRA bytes in an image the engine labels rgba (its
            # format is the guest's, the swizzle is in the view): golden candles came out blue.
            # The final (swapchain) dumps are labelled right. Engine label fix: playtests ticket 03.8.
            fmt = "bgra"
        r = subprocess.run(["magick", "-size", size, "-depth", "8", f"{fmt}:{raw}", "-alpha", "off", str(png)],
                           capture_output=True, text=True)
        if r.returncode == 0:
            produced.append(png)
            if not keep_raw:
                raw.unlink(missing_ok=True)
        else:
            print(f"magick failed for {raw.name}: {r.stderr.strip()}", file=sys.stderr)
            shutil.move(str(raw), dumps / raw.name)
    return produced


def replay_start_wall(lines, now_wall=None, now_mono=None) -> float:
    """Wall time of the replay's start. The engine stamps it on CLOCK_MONOTONIC ("pad replay started
    (mono S s)"), the clock of time.monotonic(); the runner may see the line many seconds late (it
    waits for the level beside the engine), and timed dumps must count from the real start."""
    now_wall = time.time() if now_wall is None else now_wall
    now_mono = time.monotonic() if now_mono is None else now_mono
    for line in lines:
        m = RE_REPLAY_MONO.search(line)
        if m:
            return now_wall - (now_mono - float(m.group(1)))
    return now_wall


def run_once(lib: Path, test_path: Path, args) -> tuple[int, str]:
    test = ev.load_test(test_path)
    name = test["name"]
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    results = lib / "results" / name / stamp
    results.mkdir(parents=True)
    (results / "dumps").mkdir()

    # Environment, outermost last: the reproduction point <- measurement instrumentation <-
    # replay/dump variables <- the test's [env] <- --env overrides. The reproduction point is the
    # launcher snapshot captured at record time (routes/<route>.env) so a replay matches what was
    # recorded; without one (older or stand tests) it falls back to env/<base>.env windowed, as before.
    env, repro_src = resolve_repro_env(lib, test)
    if repro_src:
        print(f"[{name}] reproduction point: {repro_src} (recorded launcher settings)")
    route = test.get("route")
    delay_ms = int(test.get("delay_ms", 3000))
    duration_s = float(test.get("duration_s", 60))
    hold_s = float(test.get("hold_s", 5))
    loop = int(test.get("loop", 0))
    dumps = test.get("dumps", [])
    if route:
        pad = lib / "routes" / f"{route}.pad"
        if not pad.is_file():
            print(f"test {name}: route '{route}' is not recorded yet ({pad} missing).\n"
                  f"record it first:  tools/playtest.py record {route} --save {test['save']}",
                  file=sys.stderr)
            raise SystemExit(ev.EXIT_INVALID)
        env["BB_PAD_REPLAY"] = str(pad)
        env["BB_PAD_REPLAY_AUTOSTART"] = "1"
        env["BB_PAD_REPLAY_DELAY_MS"] = str(delay_ms)
        env["BB_PAD_REPLAY_END"] = f"quit:{hold_s:g}"
        if loop:
            env["BB_PAD_REPLAY_LOOP"] = str(loop)
        burst = max((int(d.get("count", 1)) for d in dumps if d.get("kind") == "present"), default=1)
        if burst > 1:   # one present trigger dumps this many consecutive frames
            env["BB_PRESENT_DUMP_COUNT"] = str(burst)
    env["BB_DUMP_DIR"] = str(results / "dumps_raw")
    env["BB_FINAL_DUMP_TRIGGER"] = str(results / "final_trigger")
    env["BB_PRESENT_DUMP_TRIGGER"] = str(results / "present_trigger")
    env["BB_FRAME_LOG"] = str(results / "frames.csv")
    env["BB_TIMEOUT"] = str(int(delay_ms / 1000 + duration_s * (loop + 1) + hold_s + 240))
    for k, v in (test.get("env") or {}).items():      # persistent per-test selective changes
        env[k] = str(v)
    for kv in (getattr(args, "env", None) or []):      # ad-hoc selective changes for investigation
        k, _, v = kv.partition("=")
        env[k.strip()] = v
    user_dir = OUT / "playtest_user"
    settings = {k: dict(v) for k, v in (test.get("settings") or {}).items()}
    for kv in (getattr(args, "setting", None) or []):   # ad-hoc live-settings changes: <file>.<key>=<value> (user/<file>.txt)
        key, _, v = kv.partition("=")
        file, _, k = key.partition(".")
        settings.setdefault(file.strip(), {})[k.strip()] = v.strip()
    prepare_user(lib, test["save"], settings, user_dir, reset=args.reset_user)
    env["BB_USER_DIR"] = str(user_dir)
    # The test's [effects] (e.g. ssr = 1) are game-effect toggles that live only in bbport.ini — no
    # env var sets them. Copy the real bbport.ini, apply the overrides, and point BB_CONFIG at the
    # copy so the test controls the effects it needs without touching the player's config.
    effects = dict(test.get("effects") or {})
    for kv in (getattr(args, "effect", None) or []):   # ad-hoc effect changes, e.g. motion_blur=0
        k, _, v = kv.partition("=")
        effects[k.strip().removeprefix("effect_")] = v.strip()
    ini = {k: str(v) for k, v in (test.get("ini") or {}).items()}
    for kv in (getattr(args, "ini", None) or []):        # ad-hoc bbport.ini keys, e.g. upscaler=fsr3
        k, _, v = kv.partition("=")
        ini[k.strip()] = v.strip()
    # Every run reads a scratch copy of the player's bbport.ini (never the file itself: the in-game
    # menu writes to it). Without one a build in another checkout (BB_PLAYTEST_PORT) read that
    # checkout's missing bbport.ini and ran on defaults (0.7: output_res=auto -> 2560x1440 on a
    # scaled 4K desktop).
    base_ini = Path(env.get("BB_CONFIG") or (HOME / "bbport.ini"))
    text = base_ini.read_text() if base_ini.is_file() else ""
    for k, v in effects.items():
        text = set_ini(text, f"effect_{k}", "1" if str(v).lower() in ("1", "true", "on") else "0")
    for k, v in ini.items():
        text = set_ini(text, k, v)
    (user_dir / "bbport.ini").write_text(text)
    env["BB_CONFIG"] = str(user_dir / "bbport.ini")
    if effects or ini:
        print(f"[{name}] effects/ini: " + ", ".join(f"{k}={v}" for k, v in {**effects, **ini}.items())
              + " (scratch bbport.ini)")
    problems = env_path_problems(env)
    if problems:
        raise SystemExit(f"test {name}: not started, the environment names missing paths: "
                         + "; ".join(problems) + f" (fix env/{test.get('env_base', 'default')}.env"
                         + (f" or routes/{route}.env" if route else "") + ")")
    (results / "dumps_raw").mkdir()
    # Today's restart.sh hard-sets BB_DUMP_DIR=out/present and the triggers to out/*_trigger,
    # overriding the env above. So the legacy dump path fires those files and collects from
    # out/present; clear it first so only this run's frames are picked up. (Ticket 03.9 lets the
    # results paths win; until then these are the real locations.)
    present = OUT / "present"
    present.mkdir(exist_ok=True)
    for old in present.glob("f*_*.raw"):
        old.unlink(missing_ok=True)
    legacy_final = OUT / "final_trigger"
    legacy_present = OUT / "present_trigger"

    run = {**git_info(), "test": name, "test_file": str(test_path), "stamp": stamp, "lib": str(lib),
           "env": {k: v for k, v in env.items() if k.startswith("BB_")},
           "timing": {"started": time.time(), "end_mode": "engine"}}
    (results / "run.json").write_text(json.dumps(run, indent=1) + "\n")
    print(f"[{name}] {stamp}: save {test['save']}, route {route or '(none)'}, results {results}")

    session = OUT / "session.log"
    t0 = time.time()
    watch = LogWatch(session)
    timing = run["timing"]
    pid, level, out = start_game(env, name, watch, timing)
    if pid < 0 or not level or not pid_alive(pid):
        timing.setdefault("note", "level not reached: " + out.strip()[-300:])
        timing["process_ended"] = True
        timing["stopped_by_runner"] = True
        stop_own()
        return finish(lib, test, test_path, results, run, watch, args, status_hint="level not reached")
    print(f"[{name}] level reached after {time.time() - t0:.0f} s (pid {pid}); {out.strip()[-120:]}")

    # The replay. The engine arms it by itself (Phase 1); otherwise the runner sends the token.
    replay_wall = None
    legacy_dumps_fired: set[int] = set()
    deadline_start = time.time() + delay_ms / 1000 + 10
    while route and replay_wall is None:
        watch.poll()
        if watch.has("Runtime: pad replay of") or watch.has("Runtime: pad replay started"):
            replay_wall = replay_start_wall(watch.lines)
            timing["replay_started_s"] = replay_wall - t0
            break
        if not pid_alive(pid):
            break
        if time.time() > deadline_start and not watch.has("Runtime: pad replay armed"):
            if "autostart_sent" not in timing:
                print(f"[{name}] no autostart in the engine: sending the replay token")
                timing["autostart_sent"] = True
                press("replay")
                deadline_start = time.time() + 15
            elif time.time() > deadline_start:
                break
        time.sleep(0.5)
    if route and replay_wall is None:
        timing["note"] = "the replay did not start"
    if not route:
        replay_wall = time.time()
        timing["replay_started_s"] = replay_wall - t0

    # Wait for the finish, firing legacy dumps if the engine does not report its own.
    total_route_s = duration_s * (loop + 1)
    finished = False
    deadline = replay_wall + total_route_s + 60 if replay_wall else time.time() + 30
    while True:
        watch.poll()
        now = time.time()
        if route and watch.has("Runtime: pad replay finished"):
            if not finished:
                finished = True
                timing["replay_finished_s"] = now - t0
            # timed dumps may fall into the hold after the last input (the engine quits after it)
            if not any("at_ms" in d and i not in legacy_dumps_fired for i, d in enumerate(dumps)):
                break
        if not route and now - replay_wall >= duration_s:
            finished = True
            timing["replay_finished_s"] = now - t0
            break
        if not pid_alive(pid):
            timing["process_ended"] = True
            break
        if replay_wall:
            for i, d in enumerate(dumps):
                if i in legacy_dumps_fired:
                    continue
                trig = legacy_final if d.get("kind", "final") == "final" else legacy_present
                if "at_ms" in d and not watch.has("Runtime: pad replay dump at") \
                        and now - replay_wall >= d["at_ms"] / 1000:
                    legacy_dumps_fired.add(i)
                    trig.touch()
                elif d.get("on"):   # event-triggered: dump the next frame when the marker first appears
                    hit = next((ln for ln in watch.lines if d["on"] in ln), None)
                    if hit is not None:
                        legacy_dumps_fired.add(i)
                        trig.touch()
                        run.setdefault("events", []).append(
                            {"reason": d["on"], "name": d.get("name", f"dump{i}"),
                             "at_s": round(now - t0, 2), "line": hit.strip()[:200]})
                        print(f"[{name}] event '{d['on']}' at {now - t0:.1f} s -> dump {d.get('name', i)}")
        if now > deadline:
            timing["note"] = "the replay did not finish in time"
            break
        time.sleep(0.5)

    # The end: the engine quits by itself when it knows BB_PAD_REPLAY_END; otherwise we stop it.
    if pid_alive(pid):
        engine_end = watch.has("Runtime: pad replay armed") or watch.has("Runtime: pad replay started")
        wait_until = time.time() + (hold_s + 30 if engine_end and finished else 2)
        while pid_alive(pid) and time.time() < wait_until:
            time.sleep(0.5)
            watch.poll()
            if watch.has("Exit: GPU work finished"):
                break
        if pid_alive(pid):
            if engine_end and finished:
                timing["note"] = "no clean end: stopped by the runner"
            else:
                timing["end_mode"] = "legacy"
            timing["stopped_by_runner"] = True
            stop_own()
            for _ in range(20):
                if not pid_alive(pid):
                    break
                time.sleep(0.5)
    timing["process_ended"] = True
    timing["ended_s"] = time.time() - t0
    return finish(lib, test, test_path, results, run, watch, args)


def finish(lib, test, test_path, results: Path, run: dict, watch: LogWatch, args, status_hint=None) -> int:
    name = test["name"]
    time.sleep(1.0)
    session = OUT / "session.log"
    if session.exists():
        shutil.copy2(session, results / "session.log")
    (results / "run.json").write_text(json.dumps(run, indent=1) + "\n")
    names = dump_names(test.get("dumps", []))
    raw_dir = results / "dumps_raw"
    raw_dir.mkdir(exist_ok=True)
    if not any(raw_dir.glob("f*_*.raw")):
        # Today's restart.sh forces BB_DUMP_DIR=out/present: take the dumps made since this run began.
        started = run.get("timing", {}).get("started", 0)
        for raw in (OUT / "present").glob("f*_*.raw"):
            if raw.stat().st_mtime >= started - 1:
                shutil.move(str(raw), raw_dir / raw.name)
    convert_dumps(raw_dir, results, names, args.keep_raw)
    try:
        raw_dir.rmdir()
    except OSError:
        pass
    for trig in ("final_trigger", "present_trigger"):
        (results / trig).unlink(missing_ok=True)
    result = ev.evaluate(results, test, lib)
    if status_hint and result["status"] != "crash":
        result["invalid_reasons"].insert(0, status_hint)
        if result["status"] == "pass":
            result["status"], result["exit_code"] = "invalid", ev.EXIT_INVALID
    ev.write_results(results, result)
    if args.accept_goldens and test.get("dumps"):
        golden = lib / "golden" / name
        golden.mkdir(parents=True, exist_ok=True)
        for png in (results / "dumps").glob("*.png"):
            shutil.copy2(png, golden / png.name)
            print(f"[{name}] golden accepted: {golden / png.name}")
    if getattr(args, "accept_baseline", False) and result["status"] in ("pass", "fail"):
        bdir = lib / "baselines"
        bdir.mkdir(parents=True, exist_ok=True)
        (bdir / f"{name}.json").write_text(json.dumps(ev.baseline_from_result(result), indent=1) + "\n")
        print(f"[{name}] baseline accepted -> baselines/{name}.json")
    print(ev.summary_markdown(result), end="")
    print(f"[{name}] {result['status']} -> {results}")
    return result["exit_code"], result["status"], results


def skip_reason(lib: Path, test: dict) -> str:
    """Why a suite test was skipped (shown in the summary)."""
    route = test.get("route")
    if route and not (lib / "routes" / f"{route}.pad").is_file():
        return f"route '{route}' not recorded — tools/playtest.py record {test['name']}"
    return "could not start"


def cmd_run(args) -> int:
    lib = library(args.lib)
    if args.suite:
        tests = sorted((lib / "tests" / args.suite).glob("*.toml")) if args.suite != "all" else \
            sorted(list((lib / "tests").glob("*.toml")) + list((lib / "tests" / "bugs").glob("*.toml")))
    else:
        tests = [find_test(lib, t) for t in args.tests]
    if not tests:
        raise SystemExit("no tests given")
    OUT.mkdir(exist_ok=True)
    lock = (OUT / "playtest.lock").open("w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print("another playtest runner holds out/playtest.lock", file=sys.stderr)
        return ev.EXIT_TOOL
    worst = ev.EXIT_PASS
    roll: list[tuple[str, str, str]] = []
    for test_path in tests:
        test = ev.load_test(test_path)
        name = test["name"]
        run_results = []
        statuses = []
        reason = ""
        for _ in range(args.repeat):
            foreign = foreign_game_running()
            if foreign:
                print(f"a game these tools did not start is running (pid {foreign}): no playtest", file=sys.stderr)
                return ev.EXIT_TOOL
            try:
                code, status, rdir = run_once(lib, test_path, args)
            except SystemExit as e:   # e.g. a route not recorded yet — skip it, keep the suite going
                code = e.code if isinstance(e.code, int) else ev.EXIT_INVALID
                statuses.append("skipped")
                reason = e.code if isinstance(e.code, str) else skip_reason(lib, test)
                worst = max(worst, code) if code != ev.EXIT_TOOL else ev.EXIT_TOOL
                break
            statuses.append(status)
            worst = max(worst, code) if code != ev.EXIT_TOOL else ev.EXIT_TOOL
            try:
                run_results.append(json.loads((rdir / "result.json").read_text()))
            except (OSError, ValueError):
                pass
        status = next((s for s in ("crash", "fail", "invalid", "skipped") if s in statuses), "pass")
        if not reason and run_results:   # why a non-pass run ended up that way
            r = run_results[-1]
            reason = "; ".join(r.get("reasons") or r.get("invalid_reasons") or [])
        # Regression gate: the median of this invocation's repeats vs the accepted baseline.
        base_file = lib / "baselines" / f"{name}.json"
        if run_results and base_file.is_file() and not getattr(args, "accept_baseline", False):
            pct = float(test.get("criteria", {}).get("regression_pct", 5.0))
            regs = ev.check_baseline(ev.aggregate_metrics(run_results), json.loads(base_file.read_text()), pct)
            if regs:
                status, reason = "regressed", "; ".join(regs)
                worst = max(worst, ev.EXIT_FAIL)
                print(f"[{name}] REGRESSED vs baseline:")
                for r in regs:
                    print(f"    {r}")
        roll.append((name, status, reason))
    if len(roll) > 1:
        passed = sum(1 for _, s, _ in roll if s == "pass")
        print("\n=== playtest summary ===")
        for n, s, why in roll:
            line = f"  {'PASS' if s == 'pass' else s.upper():9} {n}"
            print(f"{line}  — {why}" if why else line)
        verdict = "all passed" if passed == len(roll) else f"{len(roll) - passed} not passing"
        print(f"{passed}/{len(roll)} passed — {verdict}")
    if args.suite:
        md, _ = build_report(lib, tests)
        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        (lib / "results").mkdir(exist_ok=True)
        (lib / "results" / f"report-{stamp}.md").write_text(md)
        print(f"\nreport written to results/report-{stamp}.md")
    return worst


def build_report(lib: Path, test_paths: list) -> tuple:
    """One markdown table across tests — status, busy, p99, deltas vs baseline, notes — from each
    test's latest result. Returns (markdown, any_fail). A pass that regressed past its baseline is
    reported as REGRESSED and counts as a failure."""
    rows = []
    any_fail = False
    for tp in test_paths:
        test = ev.load_test(tp)
        name = test["name"]
        runs = sorted((lib / "results" / name).glob("*/result.json"))
        if not runs:
            rows.append((name, "never run", "", "", "", "", ""))
            continue
        r = json.loads(runs[-1].read_text())
        busy = r["gpu_profile"]["busy_ms_median"]
        p99 = (r.get("frames") or {}).get("p99_ms")
        status, note, db, dp = r["status"], "; ".join(r.get("reasons", [])), "", ""
        base_file = lib / "baselines" / f"{name}.json"
        if base_file.is_file():
            baseline = json.loads(base_file.read_text())
            pct = float(test.get("criteria", {}).get("regression_pct", 5.0))
            regs = ev.check_baseline(ev.baseline_from_result(r), baseline, pct)
            if baseline.get("busy_ms") and busy:
                db = f"{(busy - baseline['busy_ms']) / baseline['busy_ms'] * 100:+.0f}%"
            if baseline.get("p99_ms") and p99:
                dp = f"{(p99 - baseline['p99_ms']) / baseline['p99_ms'] * 100:+.0f}%"
            if regs and status == "pass":
                status, note = "regressed", "; ".join(regs)
        if status in ("fail", "crash", "regressed"):
            any_fail = True
        rows.append((name, status, busy, p99, db, dp, note))
    out = ["# Playtest report", "",
           "| test | status | busy ms | p99 ms | Δbusy | Δp99 | notes |",
           "|---|---|---|---|---|---|---|"]
    for name, status, busy, p99, db, dp, note in rows:
        out.append(f"| {name} | {str(status).upper()} | {busy} | {p99} | {db} | {dp} | {note} |")
    return "\n".join(out) + "\n", any_fail


def cmd_report(args) -> int:
    lib = library(args.lib)
    if args.suite:
        tests = sorted((lib / "tests" / args.suite).glob("*.toml")) if args.suite != "all" else \
            sorted(list((lib / "tests").glob("*.toml")) + list((lib / "tests" / "bugs").glob("*.toml")))
    else:
        tests = all_test_paths(lib)
    md, any_fail = build_report(lib, tests)
    print(md, end="")
    return ev.EXIT_FAIL if any_fail else ev.EXIT_PASS


def format_ab(name: str, args, runs: dict, test: dict) -> tuple:
    """Markdown diff of two runs: metrics side by side, then (if the test dumps) per-image quality
    (MAD/PSNR/SSIM) between the paired dumps. Returns (markdown, degraded)."""
    (ra, da), (rb, db) = runs["A"], runs["B"]

    def busy(r):
        return r["gpu_profile"]["busy_ms_median"]

    def p99(r):
        return (r.get("frames") or {}).get("p99_ms")

    out = [f"# A/B — {name}", "",
           f"A: {' '.join(args.a or []) or 'defaults'}", f"B: {' '.join(args.b or []) or 'defaults'}",
           "", "| metric | A | B | Δ | Δ% |", "|---|---|---|---|---|"]

    def row(label, a, b):
        d = dpct = ""
        if a is not None and b is not None:
            d = round(b - a, 3)
            if a:
                dpct = f"{(b - a) / a * 100:+.0f}%"
        out.append(f"| {label} | {a} | {b} | {d} | {dpct} |")

    row("busy ms", busy(ra), busy(rb))
    row("p99 ms", p99(ra), p99(rb))
    labels = list(ra["gpu_profile"]["labels"])
    labels += [k for k in rb["gpu_profile"]["labels"] if k not in labels]
    for lbl in labels:
        row(f"{lbl} ms", ra["gpu_profile"]["labels"].get(lbl, {}).get("ms_median"),
            rb["gpu_profile"]["labels"].get(lbl, {}).get("ms_median"))

    degraded = False
    img_rows = []
    for i, d in enumerate(test.get("dumps", [])):
        nm = d.get("name", f"dump{i}")
        pa, pb = da / "dumps" / f"{nm}.png", db / "dumps" / f"{nm}.png"
        if pa.exists() and pb.exists():
            q = ev.compare_image(pa, pb, test.get("masks"))
            ssim = q.get("ssim")
            ok = ssim is None or ssim >= args.min_ssim
            degraded = degraded or not ok
            img_rows.append(f"| {nm} | {q.get('mad')} | {q.get('psnr')} | {ssim} | "
                            f"{'ok' if ok else 'DEGRADED'} |")
    if img_rows:
        out += ["", f"Image quality A vs B (flag ssim < {args.min_ssim}):", "",
                "| image | mad | psnr | ssim | ok |", "|---|---|---|---|---|"] + img_rows
    return "\n".join(out) + "\n", degraded


def cmd_ab(args) -> int:
    lib = library(args.lib)
    if foreign_game_running():
        print("a game these tools did not start is running: no A/B", file=sys.stderr)
        return ev.EXIT_TOOL
    test_path = find_test(lib, args.target)
    test = ev.load_test(test_path)
    name = test["name"]
    runs = {}
    for tag, envset in (("A", args.a or []), ("B", args.b or [])):
        if foreign_game_running():
            return ev.EXIT_TOOL
        sub = copy.copy(args)
        sub.env, sub.accept_goldens, sub.accept_baseline = envset, False, False
        print(f"\n=== A/B {name}: side {tag} ({' '.join(envset) or 'defaults'}) ===")
        _, _, rdir = run_once(lib, test_path, sub)
        try:
            runs[tag] = (json.loads((rdir / "result.json").read_text()), rdir)
        except (OSError, ValueError):
            print(f"side {tag} produced no result", file=sys.stderr)
            return ev.EXIT_TOOL
    md, degraded = format_ab(name, args, runs, test)
    outdir = lib / "results" / "ab" / name
    outdir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    (outdir / f"{stamp}.md").write_text(md)
    print("\n" + md, end="")
    print(f"A/B written to results/ab/{name}/{stamp}.md")
    return ev.EXIT_FAIL if degraded else ev.EXIT_PASS


def cmd_lib_sync(args) -> int:
    lib = library(args.lib)
    if not (lib / ".git").exists():
        print(f"{lib} is not a git repository", file=sys.stderr)
        return ev.EXIT_TOOL
    git = ["git", "-C", str(lib)]
    remote = subprocess.run([*git, "remote"], capture_output=True, text=True).stdout.strip()
    subprocess.run([*git, "add", "-A"], check=False)
    msg = args.message or f"playtests snapshot {dt.datetime.now():%Y-%m-%d %H:%M}"
    c = subprocess.run([*git, "commit", "-m", msg], capture_output=True, text=True)
    print((c.stdout + c.stderr).strip())
    if not remote:
        print(f"no remote on {lib}; add a private one, then re-run with --push to back it up",
              file=sys.stderr)
        return ev.EXIT_PASS
    if args.push:
        p = subprocess.run([*git, "push"], capture_output=True, text=True)
        print((p.stdout + p.stderr).strip())
    else:
        print("committed locally; pass --push to push to the remote")
    return ev.EXIT_PASS


def cmd_eval(args) -> int:
    lib = None
    try:
        lib = library(args.lib)
    except SystemExit:
        pass
    results = Path(args.results_dir)
    test_path = Path(args.test) if args.test else None
    if test_path is None:
        run = json.loads((results / "run.json").read_text())
        test_path = Path(run["test_file"])
    test = ev.load_test(test_path)
    result = ev.evaluate(results, test, lib)
    ev.write_results(results, result)
    print(ev.summary_markdown(result), end="")
    return result["exit_code"]


def cmd_compare(args) -> int:
    lib = library(args.lib)
    test = ev.load_test(find_test(lib, args.test))
    pct = float(test.get("criteria", {}).get("regression_pct", 5.0))
    table, flags = ev.compare_runs(lib / "results", test["name"], args.last, pct)
    print(ev.format_compare(table, flags), end="")
    return ev.EXIT_FAIL if flags else ev.EXIT_PASS


def cmd_list(args) -> int:
    lib = library(args.lib)
    width = max(60, shutil.get_terminal_size((100, 24)).columns)

    def route_exists(test) -> bool:
        r = test.get("route")
        return bool(r) and (lib / "routes" / f"{r}.pad").exists()

    rows = []  # (suite, stem, test-or-None, last-status)
    for sub in ("tests", "tests/bugs"):
        for t in sorted((lib / sub).glob("*.toml")):
            try:
                test = ev.load_test(t)
            except Exception as e:  # noqa: BLE001
                rows.append((sub, t.stem, None, f"unreadable ({e})"))
                continue
            runs = sorted((lib / "results" / test["name"]).glob("*/result.json"))
            last = "never run"
            if not route_exists(test):
                last = "no route yet"
            if runs:
                try:
                    r = json.loads(runs[-1].read_text())
                    last = f"{r.get('status')} ({r.get('stamp')})"
                except (OSError, ValueError):
                    pass
            rows.append((sub, t.stem, test, last))

    print("run a test:  tools/playtest.py run <name>        (names below; --repeat N for A/B/A)")
    namew = max((len(stem) for _, stem, _, _ in rows), default=4)
    savew = max((len(str((tst or {}).get("save", "-"))) for _, _, tst, _ in rows), default=4)
    rtew = max((len(str((tst or {}).get("route", "-"))) for _, _, tst, _ in rows), default=5)
    cur = None
    for sub, stem, test, last in rows:
        if sub != cur:
            print(f"\n{sub}/")
            cur = sub
        if test is None:
            print(f"  {stem:<{namew}}  {last}")
            continue
        save = str(test.get("save", "-"))
        route = str(test.get("route", "-"))
        print(f"  {stem:<{namew}}  save {save:<{savew}}  route {route:<{rtew}}  last: {last}")
        desc = " ".join(str(test.get("description", "")).split())
        if desc:
            for line in textwrap.wrap(desc, width - 6):
                print(f"      {line}")

    print(f"\nroutes: " + ", ".join(sorted(p.stem for p in (lib / "routes").glob("*.pad"))))
    print("saves:  " + ", ".join(sorted(p.name for p in (lib / "saves").iterdir() if p.is_dir())))
    print("\nrun all:  tools/playtest.py run --suite all      only the bug suite:  run --suite bugs")
    print("record a test's route:  tools/playtest.py record <name>   (save + route come from the test)")
    print("edit / delete a playtest:  tools/playtest.py edit <name>   |   remove <name>")
    return 0


def save_where(lib: Path, name: str) -> str:
    """The 'Where:' line of a save's NOTES.md (a one-line human description), or ''."""
    try:
        for line in (lib / "saves" / name / "NOTES.md").read_text().splitlines():
            s = line.strip()
            if s.lower().startswith("where:"):
                return s[len("where:"):].strip()
    except OSError:
        pass
    return ""


def list_saves(lib: Path, stream=sys.stderr, header: str = "available saves (pass one as --save):") -> list:
    saves = [d.name for d in sorted((lib / "saves").iterdir()) if (d / "savedata").is_dir()]
    if header:
        print(header, file=stream)
    for s in saves:
        print(f"  {s:<16} {save_where(lib, s)}", file=stream)
    return saves


# --- small interactive helpers (used only when stdin is a TTY) ------------------------------------

def _isatty() -> bool:
    return sys.stdin.isatty()


def prompt(text: str, default: str | None = None) -> str:
    """Ask the player a question. When stdin is not a TTY, return the default (or stop with a hint)."""
    if not _isatty():
        if default is not None:
            return default
        raise SystemExit(f"'{text}' needs a terminal; pass the matching flag instead")
    suffix = f" [{default}]" if default else ""
    try:
        answer = input(f"{text}{suffix}: ").strip()
    except EOFError:
        answer = ""
    return answer or (default or "")


def confirm(text: str, default: bool = False) -> bool:
    answer = prompt(f"{text} ({'Y/n' if default else 'y/N'})", "y" if default else "n").lower()
    return answer.startswith("y")


def choose(text: str, options: list, default=_SENTINEL) -> object:
    """A numbered menu. `options` is a list of (label, value); returns the chosen value. With a
    default, an empty answer returns it (used by edit to keep the current value)."""
    print(text)
    for i, (label, _) in enumerate(options):
        print(f"  {i}) {label}")
    while True:
        answer = prompt("choice")
        if answer == "" and default is not _SENTINEL:
            return default
        if answer.isdigit() and 0 <= int(answer) < len(options):
            return options[int(answer)][1]
        print("  please enter a number from the list")


def freeze_current_save(lib: Path, spot: str, where: str, interactive: bool = True) -> str:
    """Copy the live user/savedata into the library as a frozen save. The game must be closed."""
    if foreign_game_running():
        raise SystemExit("a game is running; save in-game, close it, then freeze")
    src = HOME / "user" / "savedata"
    if not src.is_dir():
        raise SystemExit(f"no live save found at {src}")
    dst = lib / "saves" / spot
    if (dst / "savedata").exists():
        if not (interactive and confirm(f"save '{spot}' already exists; overwrite it?")):
            raise SystemExit(f"save '{spot}' already exists")
        shutil.rmtree(dst / "savedata")
    dst.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst / "savedata")
    info = git_info()
    (dst / "NOTES.md").write_text(
        f"# {spot}\n\n"
        f"Where: {where or '(describe this spot)'}\n"
        f"Source: live user/savedata, frozen {dt.date.today()} (the sound-hack byte is already set).\n"
        f"Engine at freeze: {info['branch']} {info['commit'][:10]}.\n"
        "Never run the game on this folder: the runner copies it into "
        "bbport/out/playtest_user/savedata for every run.\n")
    print(f"froze current game state as save '{spot}' ({dst / 'savedata'})")
    return spot


def wizard_pick_save(lib: Path, default_spot: str, keep=_SENTINEL) -> tuple:
    """Numbered save menu with a 'freeze what I'm in now' option. Returns (save_name, froze): froze is
    True when the save was just (re)frozen from the live game, so the caller re-records even if the
    name is unchanged (a same-named freeze still replaced the save's contents). With `keep` (edit) an
    empty answer keeps that current save and the menu marks it."""
    saves = [d.name for d in sorted((lib / "saves").iterdir()) if (d / "savedata").is_dir()]
    options = [("current game state (freeze what you're in right now)", None)]
    options += [(f"{s}  —  {save_where(lib, s)}" + ("  (current)" if s == keep else ""), s) for s in saves]
    prompt_text = "\nselect a save point to spawn on:"
    if keep is not _SENTINEL:
        prompt_text += f" (Enter keeps '{keep}')"
    choice = choose(prompt_text, options, default=keep)
    if choice is not None:
        return choice, False
    spot = prompt("name for this new save point", default_spot)
    where = prompt("one line: where is this? (e.g. 'Oedon Chapel lamp')")
    print("\nnow, in the game: save (sit at a lamp or quit to the main menu), then FULLY CLOSE it.")
    prompt("press Enter once the game is closed")
    return freeze_current_save(lib, spot, where), True


def scaffold_test(path: Path, f: dict, candidates: list) -> None:
    """Write a minimal, runnable tests/<name>.toml with the measure/checkpoint parts as TODO comments."""
    def clean(s: str) -> str:
        return str(s).replace('"', "'").strip()
    cand = "".join(f"#   {int(ms)} {x:.2f} {y:.2f} {z:.2f}\n" for ms, x, y, z in candidates) or "#   (none)\n"
    ticket_line = f'ticket = "{clean(f["ticket"])}"\n' if f.get("ticket") else ""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f'name = "{clean(f["name"])}"\n'
        f'description = "{clean(f["description"])}"\n'
        f"{ticket_line}"
        f'save = "{clean(f["save"])}"\n'
        f'route = "{clean(f["route"])}"\n'
        f'env_base = "{clean(f["env_base"])}"\n'
        f"duration_s = {f['duration_s']}\n"
        "delay_ms = 3000\n"
        "hold_s = 5\n\n"
        "# Fill these in to measure more — see docs/PLAYTESTS.md:\n"
        "# [measure]\n"
        '# gpu_labels = ["upscaler RunScaled (FSR)", "cs 3d5ebf4e"]\n'
        '# per_call   = ["cs 3d5ebf4e"]\n'
        "# Checkpoint candidates recorded for this route (ms x y z):\n"
        f"{cand}"
        "# [[checkpoints]]\n"
        "# at_ms = 30000\n"
        "# tolerance_m = 3.0\n\n"
        "[criteria]\n"
        "crash_free = true\n"
        "clean_end = true\n"
        "# max_busy_ms = 15.5\n"
        "# max_p99_ms = 25\n")


def record_route(lib: Path, route: str, save: str, args, interactive: bool):
    """Drive one recording: launch on `save`, let the player play and press F9, write
    routes/<route>.{pad,camera,toml}. Returns (cams, length_ms, samples) or an int exit code."""
    if not (lib / "saves" / save / "savedata").is_dir():
        print(f"\nsave '{save}' does not exist.\n", file=sys.stderr)
        list_saves(lib)
        return ev.EXIT_INVALID
    pad = lib / "routes" / f"{route}.pad"
    if pad.exists() and not args.force:
        if interactive:
            if not confirm(f"route '{route}' is already recorded; overwrite it?"):
                return ev.EXIT_INVALID
        else:
            print(f"route '{route}' is already recorded ({pad}); pass --force to overwrite it, "
                  f"or record under a different route name", file=sys.stderr)
            return ev.EXIT_INVALID
    # A recording must look exactly like the player's normal game (fullscreen, HDR, the current
    # mods/patches/shader packs), or they cannot tell whether the route triggers the issue. So the
    # base is the live launcher environment, not the harness constants or a stale library snapshot;
    # only invisible instrumentation (the camera track) and the frozen save are added on top.
    env = dict(os.environ)
    base = launcher_env()
    if base:
        env.update(base)
        print("recording with your current launcher settings (fullscreen/HDR/mods/patches/shaders "
              "as you play) — so you see the issue exactly as normal")
    else:
        base = read_env_file(lib / "env" / f"{args.env_base}.env")
        env.update(base)
        print(f"could not read the launcher settings; using env/{args.env_base}.env, which may "
              f"differ from your launcher", file=sys.stderr)
    repro_snapshot = {k: v for k, v in base.items() if k not in REPRO_SKIP}  # saved as the reproduction point
    env["BB_CAMERA_LOG"] = "1"
    env["BB_LEVEL_SETTLE"] = env.get("BB_LEVEL_SETTLE", "3")
    env["BB_PAD_RECORD"] = str(pad)
    # Recording is F9-started on purpose: you decide when to begin (autostart would arm on a timer
    # a few seconds after the level and miss the inputs you make before it fires). Replay autostarts;
    # recording does not.
    user_dir = OUT / "playtest_user"
    prepare_user(lib, save, {}, user_dir)
    env["BB_USER_DIR"] = str(user_dir)
    info = git_info()
    # Print the how-to BEFORE launching: once the game window is up the player is looking at it,
    # not the console.
    print(f"\nspawning on save '{save}'; the game launches and loads the level by itself.")
    print("how to record (switch to the game; nothing records until you press F9):")
    print("  1) Get set at the spawn, then press F9 to START.")
    print("  2) Play the route, pausing 2 s at each checkpoint; come back and stand 2 s.")
    print("  3) Press F9 again to STOP.\n")
    watch = LogWatch(OUT / "session.log")
    timing: dict = {}
    pid, level, out = start_game(env, route, watch, timing)
    if pid < 0 or not level or not pid_alive(pid):
        print(f"the level was not reached: {timing.get('note', out.strip()[-300:])}", file=sys.stderr)
        stop_own()
        return ev.EXIT_TOOL
    started = None
    while pid_alive(pid):
        watch.poll()
        if started is None and watch.has("Runtime: pad recording started"):
            started = True
            print("recording started")
        if watch.has("Runtime: pad recording stopped"):
            print("recording stopped; closing the game in 5 s (or quit it yourself)")
            time.sleep(5)
            if pid_alive(pid):
                stop_own()
            break
        time.sleep(0.5)
    time.sleep(1)
    watch.poll()
    lines = watch.lines
    markers = ev.parse_markers(lines)
    start = markers.get("recording_started")
    if start is None or not pad.exists():
        print("no recording was made", file=sys.stderr)
        return ev.EXIT_INVALID
    m = re.search(r"mono ([\d.]+)", lines[start])
    mono_start = float(m.group(1)) if m else None
    cams = []
    first_t = None
    for i, line in enumerate(lines):
        if i <= start:
            continue
        cm = ev.RE_CAMERA.match(line)
        if not cm:
            continue
        t, x, y, z = (float(cm.group(k)) for k in range(1, 5))
        mono = float(cm.group(5)) if cm.group(5) else None
        if mono_start is not None and mono is not None:
            ms = (mono - mono_start) * 1000
        else:
            first_t = t if first_t is None else first_t
            ms = (t - first_t) * 1000
        if ms >= 0:
            cams.append((ms, x, y, z))
    samples = sum(1 for l in pad.read_text().splitlines() if l.strip() and not l.startswith("#"))
    length_ms = int(cams[-1][0]) if cams else 0
    (lib / "routes" / f"{route}.camera").write_text(
        "# ms x y z (camera position, ms since the recording started)\n" +
        "".join(f"{int(ms)} {x:.3f} {y:.3f} {z:.3f}\n" for ms, x, y, z in cams))
    (lib / "routes" / f"{route}.toml").write_text(
        f'save = "{save}"\nrecorded_commit = "{info["commit"]}"\nrecorded = "{dt.date.today()}"\n'
        f"length_ms = {length_ms}\nsamples = {samples}\nnotes = \"\"\n")
    (lib / "routes" / f"{route}.env").write_text(
        "# The launcher settings this route was recorded with — the reproduction point a run replays\n"
        "# under. Edit to change what a replay reproduces, or override per run with --env / [env].\n" +
        "".join(f"{k}={v}\n" for k, v in sorted(repro_snapshot.items())))
    print(f"{pad}: {samples} samples, {length_ms / 1000:.1f} s, {len(cams)} camera samples; "
          f"saved {len(repro_snapshot)} launcher settings as the reproduction point")
    return cams, length_ms, samples


def checkpoint_candidates(cams: list) -> list:
    """The camera position near each 10 s mark, as (ms, x, y, z) tuples for the scaffold comments."""
    return [(ms, x, y, z) for ms, x, y, z in cams if int(ms) % 10000 < 60]


def cmd_saves(args) -> int:
    lib = library(args.lib)
    saves = list_saves(lib, sys.stdout, "frozen saves:")
    if not saves:
        print("  (none yet — create one with: tools/playtest.py record)")
    return 0


def all_test_paths(lib: Path) -> list:
    return sorted(list((lib / "tests").glob("*.toml")) + list((lib / "tests" / "bugs").glob("*.toml")))


def set_scalar(text: str, key: str, value) -> str:
    """Replace, insert, or remove a top-level `key = ...` line in a test TOML, leaving the comments,
    [measure]/[criteria]/[checkpoints] and everything else untouched. value None/'' removes the line;
    an int is written bare, a str quoted."""
    line_re = re.compile(rf"(?m)^{re.escape(key)} *=.*\n")
    if value is None or value == "":
        return line_re.sub("", text, count=1)
    rendered = str(value) if isinstance(value, int) else '"' + str(value).replace('"', "'") + '"'
    newline = f"{key} = {rendered}\n"
    if line_re.search(text):
        return line_re.sub(lambda m: newline, text, count=1)
    anchor = re.search(r"(?m)^(description|name) *=.*\n", text)   # insert new keys near the top
    return text[:anchor.end()] + newline + text[anchor.end():] if anchor else newline + text


def set_ini(text: str, key: str, value: str) -> str:
    """Set `key=value` in a bbport.ini (replace the line or append it), leaving the rest intact."""
    line_re = re.compile(rf"(?m)^{re.escape(key)}\s*=.*$")
    if line_re.search(text):
        return line_re.sub(f"{key}={value}", text, count=1)
    return (text if not text or text.endswith("\n") else text + "\n") + f"{key}={value}\n"


def cmd_edit(args) -> int:
    """Change a playtest's metadata in place (save, description, ticket, env, delay). The route, the
    recording and any hand-edited [measure]/[criteria]/[checkpoints] are left untouched. With no flag
    at a terminal it asks each field with the current value as the default; flags set fields directly
    (and are the only way off a terminal). To re-record use `record <name> --force`."""
    lib = library(args.lib)
    test_path = find_test(lib, args.target)
    test = ev.load_test(test_path)
    text = test_path.read_text()
    cur = {"save": str(test.get("save", "")), "description": str(test.get("description", "")),
           "ticket": str(test.get("ticket", "")), "env_base": str(test.get("env_base", "default")),
           "delay_ms": int(test.get("delay_ms", 3000))}
    flags = {"save": args.save, "description": args.description, "ticket": args.ticket,
             "env_base": args.env_base, "delay_ms": args.delay_ms}
    new = dict(cur)
    froze = False
    if any(v is not None for v in flags.values()):
        for k, v in flags.items():
            if v is not None:
                new[k] = v
    elif _isatty():
        new["save"], froze = wizard_pick_save(lib, cur["save"], keep=cur["save"])
        new["description"] = prompt("description", cur["description"])
        new["ticket"] = prompt("bug ticket (blank for none)", cur["ticket"])
        new["env_base"] = prompt("env base", cur["env_base"])
        new["delay_ms"] = int(prompt("replay delay ms", str(cur["delay_ms"])) or cur["delay_ms"])
    else:
        print(f"nothing to change: pass a field flag (e.g. --description) or edit at a terminal",
              file=sys.stderr)
        return ev.EXIT_INVALID

    if new["save"] and not (lib / "saves" / new["save"] / "savedata").is_dir():
        print(f"save '{new['save']}' does not exist.\n", file=sys.stderr)
        list_saves(lib)
        return ev.EXIT_INVALID
    for k in ("save", "description", "env_base"):
        text = set_scalar(text, k, new[k])
    text = set_scalar(text, "delay_ms", int(new["delay_ms"]))
    text = set_scalar(text, "ticket", new["ticket"] or None)
    test_path.write_text(text)
    ev.load_test(test_path)   # it still parses
    changed = [k for k in cur if str(cur[k]) != str(new[k])]
    if not changed and not froze:
        print(f"{test_path}: no change")
        return ev.EXIT_PASS
    if froze:
        print(f"re-froze save '{new['save']}' from the current game state")
    if changed:
        print(f"updated {test_path}:")
    for k in changed:
        print(f"  {k}: {cur[k] or '(none)'} -> {new[k] or '(none)'}")

    # A changed save point — a different save, or the same name just re-frozen from the live game —
    # leaves the recorded route out of sync (it was played from the old spawn), so re-record it now.
    # Stand tests (no route) need nothing.
    route = test.get("route")
    if (froze or new["save"] != cur["save"]) and route:
        if not _isatty():
            print(f"the save changed: re-record the route at a terminal with "
                  f"'tools/playtest.py record {args.target} --force'", file=sys.stderr)
            return ev.EXIT_INVALID
        if foreign_game_running():
            print(f"the save changed, but a game these tools did not start is running; re-record "
                  f"later with 'tools/playtest.py record {args.target} --force'", file=sys.stderr)
            return ev.EXIT_TOOL
        print("\nthe save point changed — re-recording the route on the new save (the old inputs "
              "no longer match it). The game launches now.")
        rr = type("A", (), {"env_base": new["env_base"], "delay_ms": int(new["delay_ms"]), "force": True})()
        result = record_route(lib, route, new["save"], rr, True)
        if isinstance(result, int):
            print("the route was NOT re-recorded; it is out of sync with the new save until you run "
                  f"'tools/playtest.py record {args.target} --force'", file=sys.stderr)
            return result
        _, length_ms, _ = result
        test_path.write_text(set_scalar(test_path.read_text(), "duration_s", max(1, round(length_ms / 1000))))
        print(f"re-recorded route '{route}' on save '{new['save']}'.")
    return ev.EXIT_PASS


def cmd_remove(args) -> int:
    """Delete a playtest: its test TOML, its route (.pad/.camera/.toml) when no other test uses it,
    and its golden/results. The frozen save is kept (other tests may share it)."""
    lib = library(args.lib)
    test_path = find_test(lib, args.target)
    test = ev.load_test(test_path)
    name = test["name"]
    route = test.get("route")
    targets = [test_path]
    route_shared = False
    if route:
        route_shared = any(p != test_path and ev.load_test(p).get("route") == route
                           for p in all_test_paths(lib))
        if not route_shared:
            targets += [p for ext in ("pad", "camera", "toml", "env")
                        if (p := lib / "routes" / f"{route}.{ext}").exists()]
    targets += [d for d in (lib / "golden" / name, lib / "results" / name) if d.is_dir()]

    print(f"remove playtest '{name}' — will delete:")
    for t in targets:
        print(f"  {t.relative_to(lib) if t.is_relative_to(lib) else t}")
    print("  (the frozen save is kept)")
    if route and route_shared:
        print(f"  (route '{route}' is kept: another test uses it)")
    if not (args.yes or (_isatty() and confirm("delete these?"))):
        print("cancelled — pass --yes or confirm at a terminal", file=sys.stderr)
        return ev.EXIT_INVALID
    for t in targets:
        shutil.rmtree(t) if t.is_dir() else t.unlink()
    print(f"removed {name}")
    return ev.EXIT_PASS


def cmd_record(args) -> int:
    """Record a route, or create a whole new playtest interactively.

    With an existing test name, records that test's route (save + route from its TOML). With a new
    name, or no name at a terminal, it runs a wizard: name the playtest, pick or freeze a save,
    record the route, describe it, and write a runnable tests/<name>.toml (measures/checkpoints as
    TODO comments). Flags fill their step and are required when stdin is not a terminal."""
    lib = library(args.lib, create=True)
    if foreign_game_running():
        print("a game these tools did not start is running: no recording", file=sys.stderr)
        return ev.EXIT_TOOL
    interactive = _isatty()
    target = args.target

    # Existing test -> record its route (no new test is scaffolded).
    test_path = next((c for c in (lib / "tests" / f"{target}.toml",
                                  lib / "tests" / "bugs" / f"{target}.toml") if target and c.is_file()),
                     None)
    if test_path:
        test = ev.load_test(test_path)
        route = test.get("route") or target
        save = args.save or test.get("save")
        print(f"recording the route for test '{test['name']}': spawn on save '{save}', "
              f"write route '{route}'.")
        result = record_route(lib, route, save, args, interactive)
        return result if isinstance(result, int) else ev.EXIT_PASS

    # New playtest.
    name = target
    if not name:
        if not interactive:
            print("give a profile name (tools/playtest.py record <name>) or an existing test name",
                  file=sys.stderr)
            return ev.EXIT_INVALID
        name = prompt("profile name for this new playtest")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", name or ""):
        print(f"name '{name}' must be lower-case letters, digits and dashes", file=sys.stderr)
        return ev.EXIT_INVALID

    save = args.save
    if not save:
        if interactive:
            save, _ = wizard_pick_save(lib, name)
        else:
            print("\nchoose a save with --save <spot>.\n", file=sys.stderr)
            list_saves(lib)
            return ev.EXIT_INVALID

    route = name
    description = args.description if args.description is not None else (
        prompt("one-line description of this playtest") if interactive else "")
    ticket = args.ticket if args.ticket is not None else (
        prompt("bug ticket this guards (blank for none)") if interactive else "")

    result = record_route(lib, route, save, args, interactive)
    if isinstance(result, int):
        return result
    cams, length_ms, _ = result

    test_file = (lib / "tests" / "bugs" / f"{ticket}-{name}.toml") if ticket else (lib / "tests" / f"{name}.toml")
    scaffold_test(test_file, {
        "name": name, "description": description, "ticket": ticket, "save": save,
        "route": route, "env_base": args.env_base, "duration_s": max(1, round(length_ms / 1000)),
    }, checkpoint_candidates(cams))
    print(f"\nwrote {test_file}")
    print(f"next:  tools/playtest.py run {name}")
    print(f"       then edit {test_file.name} to measure more (the TODO comments + docs/PLAYTESTS.md)")
    return ev.EXIT_PASS


def missing_tools() -> list[str]:
    """What these tools need from the system and do not find: bash (tools/restart.sh), pgrep (the
    game's processes) and Python's fcntl (the run lock)."""
    missing = [t for t in ("bash", "pgrep") if not shutil.which(t)]
    if fcntl is None:
        missing.append("fcntl (Python on Linux; not available on Windows)")
    return missing


def cmd_init(args) -> int:
    lib = library_path(args.lib)
    made = init_library(lib)
    print(f"playtest library {'created' if made else 'already complete'} at {lib}")
    print("next: tools/playtest.py record   (a wizard: name the test, freeze the save you are in, "
          "record the route with F9)")
    return 0


def main(argv=None) -> int:
    missing = missing_tools()
    if missing:
        print("tools/playtest.py needs: " + ", ".join(missing) + ".\n"
              "Install them (pgrep: the procps / procps-ng package; bash: your distribution's bash). "
              "The playtest tools run on Linux; the game itself also runs on Windows.", file=sys.stderr)
        return 2
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lib", help="the playtest library (default $BB_PLAYTESTS or ../bbport-playtests)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("init", help="create an empty playtest library (record does it too)")
    p.set_defaults(func=cmd_init)
    p = sub.add_parser("run", help="run one or more tests")
    p.add_argument("tests", nargs="*")
    p.add_argument("--suite", help="all tests of tests/<suite>/ (e.g. bugs), or 'all'")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--accept-goldens", action="store_true", help="store this run's dumps as the golden images")
    p.add_argument("--accept-baseline", action="store_true",
                   help="store this run's metrics as the baseline the regression gate compares against")
    p.add_argument("--keep-raw", action="store_true")
    p.add_argument("--reset-user", action="store_true", help="recreate out/playtest_user from user/")
    p.add_argument("--env", action="append", metavar="KEY=VALUE",
                   help="override one setting for this run (repeatable), e.g. --env BB_FPS_LIMIT=30; "
                        "investigate without changing the recorded reproduction point")
    p.add_argument("--setting", action="append", metavar="FILE.KEY=VALUE",
                   help="override one live setting for this run (repeatable), e.g. --setting <file>.<key>=<value> for user/<file>.txt "
                        "(user/<FILE>.txt in the scratch user folder)")
    p.add_argument("--ini", action="append", metavar="KEY=VALUE",
                   help="override one bbport.ini key for this run (repeatable), e.g. --ini upscaler=fsr3 "
                        "(applied through a scratch copy)")
    p.add_argument("--effect", action="append", metavar="NAME=0|1",
                   help="override one game effect for this run (repeatable), e.g. --effect motion_blur=0 "
                        "(the effect_* keys of bbport.ini, applied through a scratch copy)")
    p.set_defaults(func=cmd_run)
    p = sub.add_parser("eval", help="evaluate a results directory again")
    p.add_argument("results_dir")
    p.add_argument("--test")
    p.set_defaults(func=cmd_eval)
    p = sub.add_parser("compare", help="medians of the last runs by commit")
    p.add_argument("test")
    p.add_argument("--last", type=int, default=5)
    p.set_defaults(func=cmd_compare)
    p = sub.add_parser("list", help="tests, routes and saves of the library")
    p.set_defaults(func=cmd_list)
    p = sub.add_parser("record", help="create a playtest or record a route (a guided wizard at a terminal)")
    p.add_argument("target", nargs="?", metavar="name",
                   help="an existing test (records its route), or a new playtest name. "
                        "Omit at a terminal to be asked for one")
    p.add_argument("--save", help="which frozen save to spawn on; omit to pick from a menu "
                                  "(or 'saves' to list them). Inferred when the target is a test")
    p.add_argument("--description", help="the new playtest's one-line description (skips that prompt)")
    p.add_argument("--ticket", help="bug ticket the new playtest guards; puts it under tests/bugs/")
    p.add_argument("--delay-ms", type=int, default=3000,
                   help="recording auto-starts this long after the level loads (default 3000)")
    p.add_argument("--env-base", default="default", help="env/<name>.env to load (default 'default')")
    p.add_argument("--force", action="store_true", help="overwrite the route if it already exists")
    p.set_defaults(func=cmd_record)
    p = sub.add_parser("saves", help="list the frozen saves with their location")
    p.set_defaults(func=cmd_saves)
    p = sub.add_parser("edit", help="change a playtest's save/description/ticket/env/delay (wizard or flags)")
    p.add_argument("target", help="the test to edit (its name)")
    p.add_argument("--save", help="spawn save")
    p.add_argument("--description", help="one-line description")
    p.add_argument("--ticket", help="bug ticket (empty string clears it)")
    p.add_argument("--env-base", help="env/<name>.env")
    p.add_argument("--delay-ms", type=int, help="replay start delay after the level")
    p.set_defaults(func=cmd_edit)
    p = sub.add_parser("remove", aliases=["delete"], help="delete a playtest (its test + route; keeps the save)")
    p.add_argument("target", help="the test to delete")
    p.add_argument("--yes", action="store_true", help="delete without confirming")
    p.set_defaults(func=cmd_remove)
    p = sub.add_parser("report", help="one table across the suite: status + metrics + Δ vs baseline")
    p.add_argument("--suite", help="all tests of tests/<suite>/ (e.g. bugs), or 'all' (default: every test)")
    p.set_defaults(func=cmd_report)
    p = sub.add_parser("ab", help="run a test under two --env settings and diff metrics + image quality")
    p.add_argument("target", help="the test to A/B")
    p.add_argument("--a", action="append", metavar="KEY=VALUE", help="side A setting (repeatable)")
    p.add_argument("--b", action="append", metavar="KEY=VALUE", help="side B setting (repeatable)")
    p.add_argument("--min-ssim", type=float, default=0.98, help="flag paired dumps below this SSIM (default 0.98)")
    p.add_argument("--keep-raw", action="store_true")
    p.add_argument("--reset-user", action="store_true")
    p.set_defaults(func=cmd_ab)
    p = sub.add_parser("lib-sync", help="commit the playtest library (and --push if it has a remote)")
    p.add_argument("-m", "--message", help="commit message")
    p.add_argument("--push", action="store_true", help="push to the library's remote after committing")
    p.set_defaults(func=cmd_lib_sync)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
