#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Evaluates a playtest run (tools/playtest.py): parses the game's log and per-frame CSV, checks
the route's camera checkpoints, compares frame dumps with golden images and decides pass / fail /
invalid. Usage: tools/playtest_eval.py <results-dir> [--test tests/<name>.toml] [--lib <dir>].

The log markers it relies on (all printed by the engine):
  "GPU profile: X ms/frame over N frames, L labels" + "  ms/frame  n/frame  label" rows +
  "submission gaps: ...; Y ms/frame in gaps of 0.25 ms or more"      (BB_GPU_PROFILE=1)
  "Frame pacing: median M ms, stddev S ms, p99 P ms, K frames over 1.5x median"
  "Frame stats: F FPS, worst frame W ms ...; C shader/pipeline compiles ..."
  "Camera: T s at X Y Z (...)[ mono M frame F]"                       (BB_CAMERA_LOG=1)
  "Runtime: pad replay of N samples", "Runtime: pad replay started (mono M s)",
  "Runtime: pad replay finished", "Runtime: pad replay dump at T ms (index I)",
  "Final dump I: GPU frame F", "RT reflections: N passes in 600 frames",
  "Window: closed (...)", "Exit: GPU work finished"
Exit codes: 0 pass, 1 fail or crash, 2 invalid (off-route, no replay, cold caches), 3 tool error.
"""
from __future__ import annotations

import csv
import json
import math
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

RE_PROFILE = re.compile(r"^GPU profile: ([\d.]+) ms/frame over (\d+) frames, (\d+) labels")
RE_LABEL = re.compile(r"^\s+([\d.]+) ms/frame\s+([\d.]+)/frame\s+(.*\S)\s*$")
RE_GAPS = re.compile(r"([\d.]+) ms/frame in gaps of")
RE_PACING = re.compile(r"^Frame pacing: median ([\d.]+) ms, stddev ([\d.]+) ms, p99 ([\d.]+) ms, (\d+) frames over")
RE_STATS = re.compile(r"^Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms.*?(\d+) shader/pipeline compiles")
RE_DRAWS = re.compile(r"([\d.]+) draws/frame")
RE_CAMERA = re.compile(r"^Camera: ([\d.]+) s at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)(?:.*? mono ([\d.]+))?")
RE_REPLAY_START = re.compile(r"^Runtime: pad replay (?:of (\d+) samples|started)")
RE_REPLAY_DUMP = re.compile(r"^Runtime: pad replay dump at (\d+) ms \(index (\d+)\)")
RE_FINAL_DUMP = re.compile(r"^Final dump (\d+): GPU frame (\d+)")
RE_REFLECT = re.compile(r"^RT reflections: (\d+) passes in 600 frames")
RE_GOVERNOR = re.compile(r"^RT governor: level (\d+)")
# 0.6's form of a continued game assert (upstream's raise_exception), and the guest's own assert text.
RE_ASSERT_IGNORED = re.compile(r"Runtime: the game's (.+?) assertion at \S+ ignored|Assertion Failed \((.+?)\)")
RE_ERROR = re.compile(r"Fault \(signal|Device lost|STOP|Assertion|Unimplemented PM4")
RE_ASSERT = re.compile(r"Runtime: game assert continued \(([^,]+)")
RE_ZERO_HEX = re.compile(r"\b0+([0-9a-f]{4,})")
BLOCK_SECONDS = 5.0  # the profiler's and the frame stats' period

EXIT_PASS, EXIT_FAIL, EXIT_INVALID, EXIT_TOOL = 0, 1, 2, 3


@dataclass
class Block:
    line: int
    total_ms: float
    frames: int
    labels: dict = field(default_factory=dict)  # name -> (ms_per_frame, per_frame)
    gaps_ms: float = 0.0

    @property
    def busy_ms(self) -> float:
        return self.total_ms - self.gaps_ms


def parse_gpu_profile(lines) -> list[Block]:
    blocks: list[Block] = []
    for i, line in enumerate(lines):
        m = RE_PROFILE.match(line)
        if m:
            blocks.append(Block(i, float(m.group(1)), int(m.group(2))))
            continue
        if not blocks:
            continue
        b = blocks[-1]
        m = RE_LABEL.match(line)
        if m and b.line < i:
            b.labels[m.group(3)] = (float(m.group(1)), float(m.group(2)))
            continue
        m = RE_GAPS.search(line)
        if m and line.lstrip().startswith("submission gaps"):
            b.gaps_ms = float(m.group(1))
    return blocks


def parse_frame_pacing(lines):
    out = []
    for i, line in enumerate(lines):
        m = RE_PACING.match(line)
        if m:
            out.append({"line": i, "median_ms": float(m.group(1)), "stddev_ms": float(m.group(2)),
                        "p99_ms": float(m.group(3)), "spikes": int(m.group(4))})
    return out


def parse_frame_stats(lines):
    out = []
    for i, line in enumerate(lines):
        m = RE_STATS.match(line)
        if m:
            d = RE_DRAWS.search(line)
            out.append({"line": i, "fps": float(m.group(1)), "worst_ms": float(m.group(2)),
                        "compiles": int(m.group(3)), "draws": float(d.group(1)) if d else None})
    return out


def parse_markers(lines):
    """Replay, dump, end and error markers with their line numbers."""
    mk = {"replay_start": None, "replay_finished": None, "quit_requested": None,
          "window_closed": None, "exit_done": None, "replay_dumps": [], "final_dumps": [],
          "reflections_passes": [], "reflections_first_pass": False, "governor_levels": [],
          "errors": [], "recording_started": None, "recording_stopped": None, "asserts": {}}
    for i, line in enumerate(lines):
        if mk["replay_start"] is None and RE_REPLAY_START.match(line):
            mk["replay_start"] = i
        elif line.startswith("Runtime: pad replay finished"):
            mk["replay_finished"] = i
        elif line.startswith("Runtime: pad replay end: quit requested"):
            mk["quit_requested"] = i
        elif line.startswith("Runtime: pad recording started"):
            mk["recording_started"] = i
        elif line.startswith("Runtime: pad recording stopped"):
            mk["recording_stopped"] = i
        elif line.startswith("Window: closed"):
            mk["window_closed"] = i
        elif line.startswith("Exit: GPU work finished"):
            mk["exit_done"] = i
        elif line.startswith("RT reflections: first pass"):
            mk["reflections_first_pass"] = True
        else:
            m = RE_REPLAY_DUMP.match(line)
            if m:
                mk["replay_dumps"].append({"line": i, "at_ms": int(m.group(1)), "index": int(m.group(2))})
                continue
            m = RE_FINAL_DUMP.match(line)
            if m:
                mk["final_dumps"].append({"line": i, "index": int(m.group(1)), "gpu_frame": int(m.group(2))})
                continue
            m = RE_REFLECT.match(line)
            if m:
                mk["reflections_passes"].append(int(m.group(1)))
                continue
            m = RE_GOVERNOR.match(line)
            if m:
                mk["governor_levels"].append(int(m.group(1)))
                continue
            am = RE_ASSERT.search(line)
            if am:
                # The engine continues these asserts (a short black flicker) instead of ending;
                # the last count per kind is the total (vk ... "N so far").
                kind = am.group(1).strip()
                num = re.search(r"(\d+) so far", line)
                mk["asserts"][kind] = max(mk["asserts"].get(kind, 0), int(num.group(1)) if num else mk["asserts"].get(kind, 0) + 1)
                continue
            im = RE_ASSERT_IGNORED.search(line)
            if im:   # the game goes on: counted as an assert, not a crash
                if im.group(1):
                    kind = im.group(1).strip()
                    mk["asserts"][kind] = mk["asserts"].get(kind, 0) + 1
                continue
            if RE_ERROR.search(line):
                mk["errors"].append(line.rstrip()[:200])
    return mk


def parse_camera(lines):
    """[(t_s, x, y, z, mono_s or None)] in log order."""
    out = []
    for line in lines:
        m = RE_CAMERA.match(line)
        if m:
            out.append((float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)),
                        float(m.group(5)) if m.group(5) else None))
    return out


def parse_frames_csv(path: Path, t_from: float | None = None, t_to: float | None = None):
    """frame_ms statistics of frames.csv (BB_FRAME_LOG) within [t_from, t_to] seconds of the run."""
    ms, compiles = [], 0
    try:
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                try:  # a cut-off last row has None in its missing columns
                    t = float(row["t_s"])
                    v = float(row["frame_ms"])
                except (KeyError, ValueError, TypeError):
                    continue
                if (t_from is not None and t < t_from) or (t_to is not None and t > t_to) or v <= 0:
                    continue
                ms.append(v)
                compiles += int(float(row.get("compiles", 0) or 0))
    except OSError:
        return None
    if not ms:
        return None
    ms.sort()
    med = statistics.median(ms)
    return {"count": len(ms), "median_ms": round(med, 3), "p95_ms": round(percentile(ms, 95), 3),
            "p99_ms": round(percentile(ms, 99), 3), "max_ms": round(ms[-1], 3),
            "over_1_5x": sum(1 for v in ms if v > 1.5 * med), "compiles": compiles}


def percentile(sorted_values, p):
    if not sorted_values:
        return math.nan
    k = (len(sorted_values) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    if lo == hi:
        return sorted_values[lo]
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)


def window_blocks(blocks: list[Block], markers, settle: int = 1):
    """The profile blocks of the replay: those printed after the replay start (and before the
    finish, if printed), minus the first `settle` blocks. Without replay markers: all but the
    first two blocks (the title and the load)."""
    start, end = markers.get("replay_start"), markers.get("replay_finished")
    if start is None:
        chosen = blocks[2:]
    else:
        chosen = [b for b in blocks if b.line > start and (end is None or b.line < end + 1)]
        if end is not None and not chosen:
            chosen = [b for b in blocks if b.line > start]
    return chosen[settle:] if len(chosen) > settle else chosen


def block_time_s(block: Block, blocks: list[Block], markers) -> float:
    """Approximate replay time at the end of a block: 5 s per block after the replay start."""
    start = markers.get("replay_start")
    after = [b for b in blocks if start is None or b.line > start]
    try:
        return (after.index(block) + 1) * BLOCK_SECONDS
    except ValueError:
        return math.nan


def canonical(label: str) -> str:
    """Shader hashes without their zero padding: "cs 000000003d5ebf4e" -> "cs 3d5ebf4e"."""
    return RE_ZERO_HEX.sub(r"\1", label)


def lookup(block: Block, name: str):
    """A label by its exact text, or by a unique substring ("cs 3d5ebf4e" finds
    "dispatch cs 000000003d5ebf4e (58880x1x1 groups)")."""
    if name in block.labels:
        return block.labels[name]
    want = canonical(name)
    hits = [v for k, v in block.labels.items() if want in canonical(k)]
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:  # several dispatch sizes of one shader: add them up
        return (sum(h[0] for h in hits), sum(h[1] for h in hits))
    return None


def summarise_profile(blocks: list[Block], wanted_labels, per_call_labels, split_ms=None, markers=None,
                      all_blocks=None):
    """Medians over `blocks` (the replay window); the before/after split of per-call costs uses
    the replay time of each block, counted over `all_blocks` (the whole log) from the replay start."""
    timeline = all_blocks if all_blocks is not None else blocks

    def med(values):
        values = [v for v in values if v is not None and not math.isnan(v)]
        return round(statistics.median(values), 3) if values else None

    labels = {}
    names = set(wanted_labels) | set(per_call_labels)
    for name in sorted(names):
        rows = [lookup(b, name) for b in blocks]
        present = [r for r in rows if r]
        entry = {"blocks": len(present), "ms_median": med([r[0] for r in present]),
                 "per_frame_median": med([r[1] for r in present])}
        if name in per_call_labels:
            per_call = [1000.0 * r[0] / r[1] for r in present if r[1] > 0]
            entry["per_call_us_median"] = med(per_call)
            if split_ms is not None and markers is not None:
                before, after = [], []
                for b in blocks:
                    r = lookup(b, name)
                    if not r or r[1] <= 0:
                        continue
                    (before if block_time_s(b, timeline, markers) * 1000 <= split_ms else after).append(1000.0 * r[0] / r[1])
                entry["per_call_us_before"] = med(before)
                entry["per_call_us_after"] = med(after)
                if entry["per_call_us_before"] and entry["per_call_us_after"]:
                    entry["per_call_ratio"] = round(entry["per_call_us_after"] / entry["per_call_us_before"], 2)
        labels[name] = entry
    return {"blocks": len(blocks), "total_ms_median": med([b.total_ms for b in blocks]),
            "gaps_ms_median": med([b.gaps_ms for b in blocks]),
            "busy_ms_median": med([b.busy_ms for b in blocks]), "labels": labels}


def load_route_camera(path: Path):
    """routes/<name>.camera: lines "ms x y z" -> [(ms, x, y, z)]."""
    track = []
    try:
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) < 4 or parts[0].startswith("#"):
                continue
            track.append((float(parts[0]), float(parts[1]), float(parts[2]), float(parts[3])))
    except OSError:
        return []
    return track


def nearest(track, ms, tolerance_ms=300.0):
    best = None
    for sample in track:
        d = abs(sample[0] - ms)
        if d <= tolerance_ms and (best is None or d < abs(best[0] - ms)):
            best = sample
    return best


def run_camera_track(camera, markers, lines):
    """The run's camera samples as (ms since replay start, x, y, z). With mono stamps the
    replay-start line's mono time anchors them; otherwise the first Camera line after the replay
    start marker is ms 0 (coarser, Phase 0)."""
    start_line = markers.get("replay_start")
    if start_line is None or not camera:
        return []
    mono_start = None
    m = re.search(r"mono ([\d.]+)", lines[start_line]) if start_line < len(lines) else None
    if m:
        mono_start = float(m.group(1))
    out = []
    first = None
    for t, x, y, z, mono in camera:
        if mono_start is not None and mono is not None:
            ms = (mono - mono_start) * 1000.0
            if ms < 0:
                continue
            out.append((ms, x, y, z))
        else:
            if first is None:
                first = t
            out.append(((t - first) * 1000.0, x, y, z))
    return out


def check_checkpoints(track, route_camera, checkpoints):
    results = []
    for cp in checkpoints:
        at, tol = float(cp["at_ms"]), float(cp.get("tolerance_m", 3.0))
        expected, actual = nearest(route_camera, at), nearest(track, at)
        entry = {"at_ms": at, "tolerance_m": tol, "expected": expected[1:] if expected else None,
                 "actual": actual[1:] if actual else None, "distance_m": None, "ok": False}
        if expected and actual:
            entry["distance_m"] = round(math.dist(expected[1:], actual[1:]), 3)
            entry["ok"] = entry["distance_m"] <= tol
        results.append(entry)
    return results


def compare_image(path: Path, golden: Path, masks=None, channel_threshold=8):
    """Mean absolute difference per channel (0..255) and the fraction of unmasked pixels where any
    channel differs by more than channel_threshold. Masks: {name: [x, y, w, h]} as fractions."""
    try:
        from PIL import Image
        import numpy as np
    except ImportError as e:  # pragma: no cover
        return {"error": f"missing module: {e}"}
    try:
        a = Image.open(path).convert("RGB")
        b = Image.open(golden).convert("RGB")
    except OSError as e:
        return {"error": str(e)}
    if a.size != b.size:
        return {"error": f"size mismatch {a.size} vs {b.size}"}
    aa, bb = np.asarray(a, dtype=np.int16), np.asarray(b, dtype=np.int16)
    keep = np.ones(aa.shape[:2], dtype=bool)
    h, w = keep.shape
    for rect in (masks or {}).values():
        x0, y0 = int(rect[0] * w), int(rect[1] * h)
        x1, y1 = int((rect[0] + rect[2]) * w), int((rect[1] + rect[3]) * h)
        keep[max(0, y0):min(h, y1), max(0, x0):min(w, x1)] = False
    if not keep.any():
        return {"error": "mask covers the whole image"}
    diff = np.abs(aa - bb)[keep]
    mad = float(diff.mean())
    frac = float((diff.max(axis=1) > channel_threshold).mean())
    out = {"mad": round(mad, 3), "diff_frac": round(frac, 5), "pixels": int(keep.sum())}
    mse = float((diff.astype(np.float64) ** 2).mean())
    out["psnr"] = 99.0 if mse == 0 else round(10.0 * math.log10(255.0 * 255.0 / mse), 2)
    ssim = _ssim(aa, bb, keep)
    if ssim is not None:
        out["ssim"] = round(ssim, 4)
    return out


def mean_luma(path: Path, masks=None, reduce: int = 4):
    """Mean luminance (0..255) of an image outside the masks, on a reduced copy (speed)."""
    try:
        from PIL import Image
        import numpy as np
    except ImportError:  # pragma: no cover
        return None
    try:
        img = Image.open(path).convert("RGB")
    except OSError:
        return None
    if reduce > 1 and min(img.size) >= 8 * reduce:
        img = img.reduce(reduce)
    a = np.asarray(img, dtype=np.float64) @ np.array([0.299, 0.587, 0.114])
    keep = np.ones(a.shape, dtype=bool)
    h, w = keep.shape
    for rect in (masks or {}).values():
        x0, y0 = int(rect[0] * w), int(rect[1] * h)
        x1, y1 = int((rect[0] + rect[2]) * w), int((rect[1] + rect[3]) * h)
        keep[max(0, y0):min(h, y1), max(0, x0):min(w, x1)] = False
    return float(a[keep].mean()) if keep.any() else float(a.mean())


def shimmer(paths: list, region, threshold: float = 15.0):
    """Sparkle in a still view: the mean share (%) of the region's pixels whose luma jumps by more
    than threshold between consecutive frames. region = [x, y, w, h] as fractions. None if fewer
    than two frames load."""
    try:
        from PIL import Image
        import numpy as np
    except ImportError:  # pragma: no cover
        return None
    prev, shares = None, []
    for path in paths:
        try:
            img = Image.open(path).convert("RGB")
        except OSError:
            prev = None
            continue
        w, h = img.size
        x0, y0 = int(region[0] * w), int(region[1] * h)
        box = (x0, y0, x0 + max(1, int(region[2] * w)), y0 + max(1, int(region[3] * h)))
        lum = np.asarray(img.crop(box), dtype=np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
        if prev is not None and prev.shape == lum.shape:
            shares.append(float((np.abs(lum - prev) > threshold).mean()) * 100.0)
        prev = lum
    return round(statistics.mean(shares), 4) if shares else None


def black_frames(lumas: list, black_luma: float = 3.0, black_rel: float = 0.25, drop_rel: float = 0.5,
                 drop_span: int = 4) -> list[int]:
    """Indices of the frames of a burst that are (near) black or drop out: below black_luma, below
    black_rel of the burst's median, or inside a run of up to drop_span frames that all fall below
    drop_rel of both frames around the run (ticket 81's flicker: one or two frames whose scene falls
    to about a third of its brightness while the HUD stays)."""
    vals = [v for v in lumas if v is not None]
    if not vals:
        return []
    limit = max(black_luma, black_rel * statistics.median(vals))
    out = {i for i, v in enumerate(lumas) if v is not None and v < limit}
    n = len(lumas)
    for start in range(n):
        for length in range(1, drop_span + 1):
            end = start + length            # the run is [start, end)
            if end > n:
                break
            run = lumas[start:end]
            if any(v is None for v in run):
                break
            around = [lumas[k] for k in (start - 1, end) if 0 <= k < n and lumas[k] is not None]
            if around and max(run) < drop_rel * min(around):
                out.update(range(start, end))
    return sorted(out)


def _ssim(aa, bb, keep, win: int = 7):
    """Mean windowed SSIM over the kept region, on luminance; numpy only (no scipy/skimage).
    Returns None if the image is smaller than the window."""
    import numpy as np
    if aa.shape[0] < win or aa.shape[1] < win:
        return None
    a = (aa @ np.array([0.299, 0.587, 0.114])).astype(np.float64)
    b = (bb @ np.array([0.299, 0.587, 0.114])).astype(np.float64)

    def boxmean(x):
        ii = np.zeros((x.shape[0] + 1, x.shape[1] + 1), dtype=np.float64)
        ii[1:, 1:] = x.cumsum(0).cumsum(1)
        s = ii[win:, win:] - ii[:-win, win:] - ii[win:, :-win] + ii[:-win, :-win]
        return s / (win * win)

    mu_a, mu_b = boxmean(a), boxmean(b)
    va = boxmean(a * a) - mu_a ** 2
    vb = boxmean(b * b) - mu_b ** 2
    cov = boxmean(a * b) - mu_a * mu_b
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    smap = ((2 * mu_a * mu_b + c1) * (2 * cov + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))
    off = (win - 1) // 2
    kv = keep[off:off + smap.shape[0], off:off + smap.shape[1]]
    return float(smap[kv].mean()) if kv.any() else float(smap.mean())


def evaluate(results_dir: Path, test: dict, lib: Path | None) -> dict:
    """Builds result.json's content for a run directory holding session.log, run.json and
    optionally frames.csv and dumps/<name>.png."""
    run = {}
    try:
        run = json.loads((results_dir / "run.json").read_text())
    except (OSError, ValueError):
        pass
    try:
        lines = (results_dir / "session.log").read_text(errors="replace").splitlines()
    except OSError:
        return {"status": "invalid", "reasons": ["no session.log"], "exit_code": EXIT_TOOL}

    blocks = parse_gpu_profile(lines)
    markers = parse_markers(lines)
    pacing = parse_frame_pacing(lines)
    stats = parse_frame_stats(lines)
    camera = parse_camera(lines)
    measure = test.get("measure", {})
    criteria = test.get("criteria", {})
    reasons, invalid = [], []

    window = window_blocks(blocks, markers)
    profile = summarise_profile(window, measure.get("gpu_labels", []), measure.get("per_call", []),
                                measure.get("split_ms"), markers, all_blocks=blocks)
    window_stats = [s for s in stats if markers["replay_start"] is None or s["line"] > markers["replay_start"]]
    compiles = sum(s["compiles"] for s in window_stats[1:]) if len(window_stats) > 1 else sum(s["compiles"] for s in window_stats)
    window_pacing = [p for p in pacing if markers["replay_start"] is None or p["line"] > markers["replay_start"]][1:]
    pacing_summary = None
    if window_pacing:
        pacing_summary = {"median_ms": round(statistics.median(p["median_ms"] for p in window_pacing), 3),
                          "stddev_ms": round(statistics.median(p["stddev_ms"] for p in window_pacing), 3),
                          "p99_ms": round(max(p["p99_ms"] for p in window_pacing), 3),
                          "spikes": sum(p["spikes"] for p in window_pacing)}

    timing = run.get("timing", {})
    frames = parse_frames_csv(results_dir / "frames.csv", timing.get("replay_started_s"), timing.get("replay_finished_s"))

    # The run's end.
    end_mode = timing.get("end_mode", "engine")
    # 0.7 no longer prints the "Exit:" line after a requested quit: a quit the replay asked for that
    # closed the window is a clean end too.
    clean_end = markers["window_closed"] is not None and (
        markers["exit_done"] is not None
        or (markers["quit_requested"] is not None and markers["quit_requested"] < markers["window_closed"]))
    crashed = bool(markers["errors"]) or (timing.get("process_ended") and not clean_end and end_mode == "engine"
                                          and not timing.get("stopped_by_runner"))
    if timing.get("crashed"):
        crashed = True

    # Camera checkpoints.
    checkpoints = []
    route = test.get("route")
    if test.get("checkpoints") and lib is not None and route:
        route_camera = load_route_camera(lib / "routes" / f"{route}.camera")
        track = run_camera_track(camera, markers, lines)
        if not route_camera:
            invalid.append(f"route {route} has no .camera track")
        elif not track:
            invalid.append("no Camera lines in the replay (BB_CAMERA_LOG=1?)")
        else:
            checkpoints = check_checkpoints(track, route_camera, test["checkpoints"])
            for cp in checkpoints:
                if not cp["ok"]:
                    invalid.append(f"off-route at {cp['at_ms']:.0f} ms: {cp['distance_m']} m (tolerance {cp['tolerance_m']})"
                                   if cp["distance_m"] is not None else f"no camera sample at {cp['at_ms']:.0f} ms")

    # Images.
    images = []
    for i, dump in enumerate(test.get("dumps", [])):
        name = dump.get("name", f"dump{i}")
        count = int(dump.get("count", 1))
        if count > 1:   # a burst of consecutive frames: judged by brightness, not by a golden
            pngs = [results_dir / "dumps" / f"{name}_{j:03d}.png" for j in range(count)]
            lumas = [mean_luma(f, test.get("masks")) if f.exists() else None for f in pngs]
            got = sum(v is not None for v in lumas)
            black = black_frames(lumas, float(criteria.get("black_luma", 3.0)), float(criteria.get("black_rel", 0.25)),
                                 float(criteria.get("drop_rel", 0.5)), int(criteria.get("drop_span", 4)))
            entry = {"name": name, "burst": count, "frames": got, "black": black,
                     "luma": [None if v is None else round(v, 2) for v in lumas], "ok": not black}
            if dump.get("region"):
                entry["shimmer_pct"] = shimmer(pngs, dump["region"], float(dump.get("shimmer_threshold", 15)))
            if got < count:
                invalid.append(f"burst {name}: {got} of {count} frames dumped")
            images.append(entry)
            continue
        png = results_dir / "dumps" / f"{name}.png"
        golden = (lib / "golden" / test["name"] / f"{name}.png") if lib else None
        entry = {"name": name, "file": str(png), "golden": str(golden) if golden else None, "ok": None}
        if not png.exists():
            entry["error"] = "no dump produced"
            invalid.append(f"dump {name} missing")
        elif golden and golden.exists():
            cmp = compare_image(png, golden, test.get("masks"))
            entry.update(cmp)
            if "error" in cmp:
                reasons.append(f"image {name}: {cmp['error']}")
                entry["ok"] = False
            else:
                entry["ok"] = (cmp["mad"] <= float(criteria.get("image_mad_max", 4.0)) and
                               cmp["diff_frac"] <= float(criteria.get("image_diff_frac_max", 0.02)))
                if not entry["ok"]:
                    reasons.append(f"image {name} differs from the golden: mad {cmp['mad']}, {cmp['diff_frac']:.2%} pixels")
        else:
            entry["note"] = "no golden yet (run --accept-goldens)"
        images.append(entry)

    # Validity.
    if markers["replay_start"] is None:
        invalid.append("the replay never started")
    elif markers["replay_finished"] is None and not crashed:
        invalid.append("the replay did not finish")
    max_compiles = int(criteria.get("max_compiles", 50))
    if compiles > max_compiles:
        invalid.append(f"{compiles} shader compiles during the replay (cold caches; warm-up run)")
    if not window:
        invalid.append("no GPU profile blocks in the replay window")

    # Criteria.
    if crashed:
        reasons.append("crash: " + ("; ".join(markers["errors"][:3]) or "the process ended without closing its window"))
    if criteria.get("clean_end", True) and end_mode == "engine" and not clean_end and not crashed:
        reasons.append("no clean end (Window: closed + Exit: lines missing)")
    if "max_busy_ms" in criteria and profile["busy_ms_median"] is not None and profile["busy_ms_median"] > float(criteria["max_busy_ms"]):
        reasons.append(f"GPU busy {profile['busy_ms_median']} ms > {criteria['max_busy_ms']}")
    p99 = (frames or {}).get("p99_ms") or (pacing_summary or {}).get("p99_ms")
    if "max_p99_ms" in criteria and p99 is not None and p99 > float(criteria["max_p99_ms"]):
        reasons.append(f"frame p99 {p99} ms > {criteria['max_p99_ms']}")
    for name, limit in (criteria.get("max_label_ms") or {}).items():
        v = profile["labels"].get(name, {}).get("ms_median")
        if v is None:
            reasons.append(f"label '{name}' not in the profile")
        elif v > float(limit):
            reasons.append(f"{name} {v} ms > {limit}")
    for name, limit in (criteria.get("per_call_ratio_max") or {}).items():
        entry = profile["labels"].get(name, {})
        ratio = entry.get("per_call_ratio")
        if ratio is None:
            reasons.append(f"per-call ratio of '{name}' not measurable (label or split missing)")
        elif ratio > float(limit):
            reasons.append(f"{name}: {entry['per_call_us_before']} -> {entry['per_call_us_after']} us a call (x{ratio} > {limit})")
    if "min_reflection_passes" in criteria:
        passes = markers["reflections_passes"]
        if not passes or max(passes) < int(criteria["min_reflection_passes"]):
            reasons.append(f"RT reflections passes {passes or 'none'} < {criteria['min_reflection_passes']}")
    if criteria.get("require_reflections_first_pass") and not markers["reflections_first_pass"]:
        reasons.append("no 'RT reflections: first pass' line")
    if criteria.get("no_error_lines", False) and markers["errors"]:
        reasons.append("error lines: " + "; ".join(markers["errors"][:3]))
    total_asserts = sum(markers["asserts"].values())
    if "max_asserts" in criteria and total_asserts > int(criteria["max_asserts"]):
        detail = ", ".join(f"{k}: {v}" for k, v in markers["asserts"].items())
        reasons.append(f"{total_asserts} continued asserts > {criteria['max_asserts']} ({detail})")

    if "max_shimmer_pct" in criteria:
        for im in images:
            v = im.get("shimmer_pct")
            if v is None and im.get("burst"):
                invalid.append(f"burst {im['name']}: no shimmer measure (region missing or too few frames)")
            elif v is not None and v > float(criteria["max_shimmer_pct"]):
                reasons.append(f"burst {im['name']}: shimmer {v} % of the region's pixels a frame > {criteria['max_shimmer_pct']}")
    if "max_black_frames" in criteria:
        bursts = [im for im in images if im.get("burst")]
        if not bursts:
            invalid.append("max_black_frames set but no burst dump ([[dumps]] with count > 1, kind present)")
        for im in bursts:
            if len(im["black"]) > int(criteria["max_black_frames"]):
                reasons.append(f"burst {im['name']}: {len(im['black'])} dark or dropped frames > {criteria['max_black_frames']} "
                               f"(frames {im['black'][:12]})")

    if crashed:
        status, code = "crash", EXIT_FAIL
    elif reasons:
        status, code = "fail", EXIT_FAIL
    elif invalid:
        status, code = "invalid", EXIT_INVALID
    else:
        status, code = "pass", EXIT_PASS
    # A crash or a failed criterion beats "invalid": both are reported.
    return {
        "test": test.get("name"), "stamp": run.get("stamp"), "commit": run.get("commit"),
        "dirty": run.get("dirty"), "branch": run.get("branch"), "status": status, "exit_code": code,
        "reasons": reasons, "invalid_reasons": invalid,
        "timing": {**timing, "clean_end": clean_end, "replay_start_line": markers["replay_start"],
                   "replay_finished_line": markers["replay_finished"]},
        "gpu_profile": profile, "frames": frames, "pacing": pacing_summary,
        "frame_stats": {"blocks": len(window_stats), "compiles": compiles,
                        "fps_median": round(statistics.median(s["fps"] for s in window_stats), 2) if window_stats else None},
        "summaries": {"rt_reflections_passes": markers["reflections_passes"],
                      "rt_reflections_first_pass": markers["reflections_first_pass"],
                      "governor_levels": markers["governor_levels"],
                      "replay_dumps": markers["replay_dumps"], "final_dumps": markers["final_dumps"],
                      "asserts": markers["asserts"]},
        "camera": {"checkpoints": checkpoints, "samples": len(camera)},
        "images": images, "events": run.get("events", []), "errors": {"log_lines": markers["errors"]},
        "env": run.get("env", {}), "files": {"session_log": str(results_dir / "session.log"),
                                             "frames_csv": str(results_dir / "frames.csv"),
                                             "run_json": str(results_dir / "run.json")},
    }


def summary_markdown(result: dict) -> str:
    out = [f"# {result.get('test')} — {result['status'].upper()}", ""]
    out.append(f"Commit {result.get('commit')}{' (dirty)' if result.get('dirty') else ''} on {result.get('branch')}, run {result.get('stamp')}.")
    for r in result["reasons"]:
        out.append(f"- FAIL: {r}")
    for r in result["invalid_reasons"]:
        out.append(f"- invalid: {r}")
    p = result["gpu_profile"]
    out += ["", f"GPU profile over {p['blocks']} blocks: total {p['total_ms_median']} ms, gaps {p['gaps_ms_median']}, busy {p['busy_ms_median']} ms.", ""]
    if p["labels"]:
        out += ["| label | ms/frame | n/frame | us/call | before -> after |", "|---|---|---|---|---|"]
        for name, e in p["labels"].items():
            ba = ""
            if e.get("per_call_us_before") is not None or e.get("per_call_us_after") is not None:
                ba = f"{e.get('per_call_us_before')} -> {e.get('per_call_us_after')} (x{e.get('per_call_ratio')})"
            out.append(f"| {name} | {e['ms_median']} | {e['per_frame_median']} | {e.get('per_call_us_median', '')} | {ba} |")
        out.append("")
    if result.get("frames"):
        f = result["frames"]
        out.append(f"Frames: {f['count']}, median {f['median_ms']} ms, p95 {f['p95_ms']}, p99 {f['p99_ms']}, max {f['max_ms']}, {f['over_1_5x']} over 1.5x, {f['compiles']} compiles.")
    if result.get("pacing"):
        pc = result["pacing"]
        out.append(f"Pacing: median {pc['median_ms']} ms, p99 {pc['p99_ms']}, {pc['spikes']} spikes.")
    s = result["summaries"]
    if s["rt_reflections_passes"]:
        out.append(f"RT reflections passes per 600 frames: {s['rt_reflections_passes']}.")
    if s.get("asserts"):
        out.append("Continued game asserts: " + ", ".join(f"{k}: {v}" for k, v in s["asserts"].items()) + ".")
    if s["governor_levels"]:
        out.append(f"Governor levels seen: {sorted(set(s['governor_levels']))}.")
    if result["camera"]["checkpoints"]:
        out += ["", "| checkpoint ms | distance m | tolerance | ok |", "|---|---|---|---|"]
        for cp in result["camera"]["checkpoints"]:
            out.append(f"| {cp['at_ms']:.0f} | {cp['distance_m']} | {cp['tolerance_m']} | {'yes' if cp['ok'] else 'NO'} |")
    for im in result["images"]:
        if im.get("burst"):
            lumas = [v for v in im["luma"] if v is not None]
            span = f"luma {min(lumas)} .. {max(lumas)}" if lumas else "no frames"
            out.append(f"Burst {im['name']}: {im['frames']} of {im['burst']} frames, {span}; "
                       f"dark or dropped frames: {im['black'] or 'none'}"
                       + (f"; shimmer {im['shimmer_pct']} % of the region's pixels a frame." if im.get("shimmer_pct") is not None else "."))
    stills = [im for im in result["images"] if not im.get("burst")]
    if stills:
        out += ["", "| image | mad | diff | psnr | ssim | ok |", "|---|---|---|---|---|---|"]
        for im in stills:
            ok = im.get("ok") if im.get("ok") is not None else im.get("note", im.get("error", ""))
            out.append(f"| {im['name']} | {im.get('mad', '')} | {im.get('diff_frac', '')} | "
                       f"{im.get('psnr', '')} | {im.get('ssim', '')} | {ok} |")
    if result.get("events"):
        out += ["", "Event captures:"]
        for e in result["events"]:
            out.append(f"- {e.get('reason')} at {e.get('at_s')} s: {e.get('line', '')}")
    clean = result["timing"].get("clean_end")
    out += ["", f"Clean end: {clean}; end mode: {result['timing'].get('end_mode', 'engine')}."]
    return "\n".join(out) + "\n"


def write_results(results_dir: Path, result: dict):
    (results_dir / "result.json").write_text(json.dumps(result, indent=1) + "\n")
    (results_dir / "summary.md").write_text(summary_markdown(result))


def baseline_from_result(result: dict) -> dict:
    """The accepted-metrics snapshot to store as baselines/<test>.json."""
    labels = {lbl: {"ms_median": e.get("ms_median"), "per_call_us_median": e.get("per_call_us_median")}
              for lbl, e in result["gpu_profile"]["labels"].items()}
    return {"commit": result.get("commit"), "busy_ms": result["gpu_profile"]["busy_ms_median"],
            "p99_ms": (result.get("frames") or {}).get("p99_ms"), "labels": labels}


def aggregate_metrics(results: list) -> dict:
    """Median busy/p99/label metrics across several runs of one test (so a noisy run does not flip
    the gate). Shaped like a baseline for check_baseline."""
    def med(vals):
        vals = [v for v in vals if v is not None]
        return round(statistics.median(vals), 3) if vals else None
    labels: dict = {}
    for r in results:
        for lbl, e in r["gpu_profile"]["labels"].items():
            labels.setdefault(lbl, {"ms": [], "us": []})
            labels[lbl]["ms"].append(e.get("ms_median"))
            labels[lbl]["us"].append(e.get("per_call_us_median"))
    return {"commit": results[-1].get("commit"),
            "busy_ms": med([r["gpu_profile"]["busy_ms_median"] for r in results]),
            "p99_ms": med([(r.get("frames") or {}).get("p99_ms") for r in results]),
            "labels": {lbl: {"ms_median": med(v["ms"]), "per_call_us_median": med(v["us"])}
                       for lbl, v in labels.items()}}


def check_baseline(metrics: dict, baseline: dict, pct: float) -> list:
    """Regression reasons: metrics (median busy/p99/labels) risen above the baseline by more than
    pct per cent. Only increases (slower / more cost) count; improvements never fail."""
    out = []

    def cmp(label, cur, base):
        if cur is None or not base:
            return
        delta = (cur - base) / base * 100
        if delta > pct:
            out.append(f"{label} {base}→{cur} (+{delta:.0f}%) vs baseline")

    cmp("GPU busy ms", metrics.get("busy_ms"), baseline.get("busy_ms"))
    cmp("frame p99 ms", metrics.get("p99_ms"), baseline.get("p99_ms"))
    for lbl, b in (baseline.get("labels") or {}).items():
        e = (metrics.get("labels") or {}).get(lbl)
        if e:
            cmp(f"{lbl} ms", e.get("ms_median"), b.get("ms_median"))
            cmp(f"{lbl} us/call", e.get("per_call_us_median"), b.get("per_call_us_median"))
    return out


def compare_runs(results_root: Path, test_name: str, last: int = 5, regression_pct: float = 5.0):
    """Medians of the last runs grouped by commit, oldest group first; flags deltas above
    regression_pct between the two most recent commits. Returns (groups, flags)."""
    runs = []
    for d in sorted((results_root / test_name).glob("*")):
        try:
            r = json.loads((d / "result.json").read_text())
        except (OSError, ValueError):
            continue
        if r.get("status") in ("pass", "fail"):
            runs.append(r)
    runs = runs[-last * 4:]
    groups: dict[str, list] = {}
    order = []
    for r in runs:
        key = (r.get("commit") or "?")[:9]
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(r)

    def med(vals):
        vals = [v for v in vals if v is not None]
        return round(statistics.median(vals), 3) if vals else None

    table = []
    for key in order:
        rs = groups[key][-last:]
        row = {"commit": key, "runs": len(rs),
               "busy_ms": med([r["gpu_profile"]["busy_ms_median"] for r in rs]),
               "p99_ms": med([(r.get("frames") or {}).get("p99_ms") for r in rs]),
               "labels": {}}
        names = set()
        for r in rs:
            names |= set(r["gpu_profile"]["labels"])
        for name in sorted(names):
            row["labels"][name] = med([r["gpu_profile"]["labels"].get(name, {}).get("ms_median") for r in rs])
        table.append(row)
    flags = []
    if len(table) >= 2:
        a, b = table[-2], table[-1]
        for metric in ("busy_ms", "p99_ms"):
            if a[metric] and b[metric] and abs(b[metric] - a[metric]) / a[metric] * 100 > regression_pct:
                flags.append(f"{metric}: {a[metric]} -> {b[metric]} ({(b[metric] - a[metric]) / a[metric] * 100:+.1f} %)")
        for name in sorted(set(a["labels"]) & set(b["labels"])):
            va, vb = a["labels"][name], b["labels"][name]
            if va and vb and va > 0.05 and abs(vb - va) / va * 100 > regression_pct:
                flags.append(f"{name}: {va} -> {vb} ms ({(vb - va) / va * 100:+.1f} %)")
    return table, flags


def format_compare(table, flags) -> str:
    if not table:
        return "no evaluated runs\n"
    names = sorted({n for row in table for n in row["labels"]})
    head = "| commit | runs | busy ms | p99 ms | " + " | ".join(names) + " |"
    out = [head, "|" + "---|" * (4 + len(names))]
    for row in table:
        out.append(f"| {row['commit']} | {row['runs']} | {row['busy_ms']} | {row['p99_ms']} | " +
                   " | ".join(str(row["labels"].get(n)) for n in names) + " |")
    for f in flags:
        out.append(f"- change over threshold: {f}")
    return "\n".join(out) + "\n"


def load_test(path: Path) -> dict:
    import tomllib
    with path.open("rb") as f:
        test = tomllib.load(f)
    test.setdefault("name", path.stem)
    return test


def main(argv=None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("results_dir", type=Path)
    ap.add_argument("--test", type=Path, help="tests/<name>.toml (default: the test named in run.json)")
    ap.add_argument("--lib", type=Path, help="the playtest library (routes, golden)")
    args = ap.parse_args(argv)
    test_path = args.test
    if test_path is None:
        try:
            run = json.loads((args.results_dir / "run.json").read_text())
            test_path = Path(run["test_file"])
        except (OSError, ValueError, KeyError):
            print("no --test and no test_file in run.json", file=sys.stderr)
            return EXIT_TOOL
    test = load_test(test_path)
    result = evaluate(args.results_dir, test, args.lib)
    write_results(args.results_dir, result)
    print(summary_markdown(result), end="")
    return result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
