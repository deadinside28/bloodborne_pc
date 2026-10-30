# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The replay playtests' runner and evaluator (tools/playtest.py, tools/playtest_eval.py) on
fixtures: no game, no Vulkan. Run: python3 -m unittest discover -s tests -p test_playtest.py"""
from paths import ROOT
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT / "tools"))
import playtest_eval as ev  # noqa: E402
import playtest as pt  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "playtest"
LINES = (FIXTURES / "session.log").read_text().splitlines()

TEST = {
    "name": "fixture-test", "save": "stakes", "route": "stand-60", "duration_s": 60,
    "measure": {"gpu_labels": ["rtao trace", "rt sun shadow"], "per_call": ["cs 3d5ebf4e"], "split_ms": 5000},
    "checkpoints": [{"at_ms": 20000, "tolerance_m": 1.0}],
    "dumps": [{"at_ms": 30000, "kind": "final", "name": "cp1"}],
    "criteria": {"crash_free": True, "clean_end": True, "min_reflection_passes": 1, "max_compiles": 50},
}


class ParserTests(unittest.TestCase):
    def test_gpu_profile_blocks(self):
        blocks = ev.parse_gpu_profile(LINES)
        self.assertEqual([b.total_ms for b in blocks], [16.65, 18.03, 19.13])
        self.assertEqual(blocks[0].frames, 301)
        self.assertAlmostEqual(blocks[1].gaps_ms, 6.476, places=3)
        self.assertAlmostEqual(blocks[1].busy_ms, 18.03 - 6.476, places=3)
        self.assertEqual(blocks[0].labels["rtao trace"], (0.732, 1.0))
        self.assertEqual(ev.lookup(blocks[1], "cs 3d5ebf4e"), (3.224, 4.6))
        self.assertIsNone(ev.lookup(blocks[1], "no such label"))

    def test_markers(self):
        mk = ev.parse_markers(LINES)
        self.assertEqual(LINES[mk["replay_start"]], "Runtime: pad replay of 2 samples from /lib/routes/stand-60.pad")
        self.assertTrue(LINES[mk["replay_finished"]].startswith("Runtime: pad replay finished"))
        self.assertIsNotNone(mk["window_closed"])
        self.assertIsNotNone(mk["exit_done"])
        self.assertEqual(mk["replay_dumps"], [{"line": mk["replay_dumps"][0]["line"], "at_ms": 30000, "index": 0}])
        self.assertEqual(mk["final_dumps"][0]["gpu_frame"], 3000)
        self.assertEqual(mk["reflections_passes"], [0, 482])
        self.assertTrue(mk["reflections_first_pass"])
        self.assertEqual(mk["governor_levels"], [0])
        self.assertEqual(mk["errors"], [])

    def test_errors_detected(self):
        mk = ev.parse_markers(LINES + ["Fault (signal 11) in thread GPU"])
        self.assertEqual(len(mk["errors"]), 1)

    def test_continued_asserts_counted(self):
        extra = ["Runtime: game assert continued (YEBIS camera motion blur, nBaseSamples < 3), 1 so far",
                 "Runtime: game assert continued (YEBIS camera motion blur, nBaseSamples < 3), 2 so far",
                 "Runtime: game assert continued (YEBIS camera motion blur, nBaseSamples < 3), 11 so far"]
        mk = ev.parse_markers(LINES + extra)
        self.assertEqual(mk["asserts"], {"YEBIS camera motion blur": 11})
        self.assertEqual(mk["errors"], [])  # continued asserts are not fatal errors

    def test_pacing_and_stats(self):
        pacing = ev.parse_frame_pacing(LINES)
        self.assertEqual([p["p99_ms"] for p in pacing], [21.09, 27.68, 21.56])
        stats = ev.parse_frame_stats(LINES)
        self.assertEqual(stats[1]["compiles"], 12)
        self.assertEqual(stats[1]["draws"], 1400.0)
        self.assertEqual(stats[0]["fps"], 60.0)

    def test_camera(self):
        cam = ev.parse_camera(LINES)
        self.assertEqual(len(cam), 7)
        self.assertEqual(cam[0], (20.0, 10.0, 2.0, -5.0, 100.0))
        mk = ev.parse_markers(LINES)
        track = ev.run_camera_track(cam, mk, LINES)
        # Phase 0 anchor: the "pad replay of" line has no mono stamp -> the first Camera line is 0 ms.
        self.assertEqual(track[0][0], 0.0)
        self.assertEqual(track[2][0], 20000.0)

    def test_camera_mono_anchor(self):
        lines = [l.replace("Runtime: pad replay of 2 samples from /lib/routes/stand-60.pad",
                           "Runtime: pad replay started (mono 105.000 s)") for l in LINES]
        cam = ev.parse_camera(lines)
        mk = ev.parse_markers(lines)
        track = ev.run_camera_track(cam, mk, lines)
        self.assertEqual([round(t[0]) for t in track[:3]], [5000, 15000, 25000])

    def test_frames_csv(self):
        f = ev.parse_frames_csv(FIXTURES / "frames.csv")
        self.assertGreater(f["count"], 100)
        self.assertGreaterEqual(f["p99_ms"], f["median_ms"])
        self.assertIsNone(ev.parse_frames_csv(FIXTURES / "missing.csv"))
        with tempfile.TemporaryDirectory() as d:  # a run killed mid-row leaves a short last line
            p = Path(d) / "frames.csv"
            p.write_text("t_s,frame_ms,compiles\n1.0,16.6,0\n2.0,17.0,1\n3.0\n")
            cut = ev.parse_frames_csv(p)
            self.assertEqual((cut["count"], cut["compiles"]), (2, 1))
        self.assertEqual(ev.percentile([1, 2, 3, 4], 50), 2.5)

    def test_window_and_summary(self):
        blocks = ev.parse_gpu_profile(LINES)
        mk = ev.parse_markers(LINES)
        window = ev.window_blocks(blocks, mk)
        self.assertEqual([b.total_ms for b in window], [18.03, 19.13])  # the first block settles
        prof = ev.summarise_profile(window, ["rtao trace"], ["cs 3d5ebf4e"], split_ms=5000, markers=mk, all_blocks=blocks)
        self.assertAlmostEqual(prof["busy_ms_median"], (18.03 - 6.476 + 19.13 - 0.313) / 2, places=2)
        self.assertEqual(prof["labels"]["rtao trace"]["ms_median"], round((0.174 + 0.677) / 2, 3))
        pc = prof["labels"]["cs 3d5ebf4e"]
        self.assertAlmostEqual(pc["per_call_us_median"], (3224 / 4.6 + 3416 / 7.0) / 2, delta=1.0)
        # split at 5 s of replay time: the window's first block ends at 10 s -> everything is "after"
        self.assertIsNone(pc["per_call_us_before"])
        self.assertIsNotNone(pc["per_call_us_after"])


class CheckpointAndImageTests(unittest.TestCase):
    def test_checkpoints(self):
        route = [(0, 10.0, 2.0, -5.0), (20000, 10.2, 2.0, -5.0), (40000, 12.0, 2.0, -5.0)]
        track = [(0, 10.0, 2.0, -5.0), (19900, 10.6, 2.0, -5.0), (40100, 20.0, 2.0, -5.0)]
        cps = ev.check_checkpoints(track, route, [{"at_ms": 20000, "tolerance_m": 1.0},
                                                   {"at_ms": 40000, "tolerance_m": 3.0},
                                                   {"at_ms": 60000, "tolerance_m": 3.0}])
        self.assertTrue(cps[0]["ok"])
        self.assertAlmostEqual(cps[0]["distance_m"], 0.4, places=3)
        self.assertFalse(cps[1]["ok"])
        self.assertIsNone(cps[2]["distance_m"])  # no sample within 300 ms

    def test_compare_image(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            a = Image.new("RGB", (100, 50), (10, 20, 30))
            a.save(Path(d) / "a.png")
            b = a.copy()
            for x in range(100):
                for y in range(40, 50):
                    b.putpixel((x, y), (200, 200, 200))  # a "HUD" strip at the bottom
            b.save(Path(d) / "b.png")
            same = ev.compare_image(Path(d) / "a.png", Path(d) / "a.png")
            self.assertEqual(same["mad"], 0.0)
            diff = ev.compare_image(Path(d) / "a.png", Path(d) / "b.png")
            self.assertAlmostEqual(diff["diff_frac"], 0.2, places=3)
            masked = ev.compare_image(Path(d) / "a.png", Path(d) / "b.png", {"hud": [0.0, 0.8, 1.0, 0.2]})
            self.assertEqual(masked["diff_frac"], 0.0)
            self.assertEqual(masked["pixels"], 4000)
            Image.new("RGB", (10, 10)).save(Path(d) / "c.png")
            self.assertIn("error", ev.compare_image(Path(d) / "a.png", Path(d) / "c.png"))


class EvaluateTests(unittest.TestCase):
    def make_results(self, d: Path, lines=None, run=None, with_route=True):
        results = d / "results"
        results.mkdir()
        (results / "session.log").write_text("\n".join(lines or LINES) + "\n")
        shutil.copy(FIXTURES / "frames.csv", results / "frames.csv")
        (results / "run.json").write_text(json.dumps(run or {"commit": "abc123", "stamp": "s", "timing": {"end_mode": "engine", "process_ended": True}}))
        lib = d / "lib"
        (lib / "routes").mkdir(parents=True)
        (lib / "golden" / TEST["name"]).mkdir(parents=True)
        if with_route:
            # The route's camera: the fixture camera is at 10.0 .. 10.6 along x, 0 ms = first sample.
            (lib / "routes" / "stand-60.camera").write_text("0 10.0 2.0 -5.0\n20000 10.2 2.0 -5.0\n60000 10.6 2.0 -5.0\n")
        return results, lib

    def test_pass(self):
        with tempfile.TemporaryDirectory() as d:
            results, lib = self.make_results(Path(d))
            from PIL import Image
            (results / "dumps").mkdir()
            Image.new("RGB", (64, 32), (1, 2, 3)).save(results / "dumps" / "cp1.png")
            Image.new("RGB", (64, 32), (1, 2, 3)).save(lib / "golden" / TEST["name"] / "cp1.png")
            r = ev.evaluate(results, TEST, lib)
            self.assertEqual(r["status"], "pass", (r["reasons"], r["invalid_reasons"]))
            self.assertEqual(r["exit_code"], ev.EXIT_PASS)
            self.assertTrue(r["timing"]["clean_end"])
            self.assertEqual(r["camera"]["checkpoints"][0]["ok"], True)
            self.assertEqual(r["images"][0]["ok"], True)
            self.assertEqual(r["summaries"]["rt_reflections_passes"], [0, 482])
            md = ev.summary_markdown(r)
            self.assertIn("PASS", md)
            self.assertIn("rtao trace", md)

    def test_fail_on_criteria(self):
        with tempfile.TemporaryDirectory() as d:
            results, lib = self.make_results(Path(d))
            (results / "dumps").mkdir()
            test = dict(TEST, criteria={"max_busy_ms": 10.0, "min_reflection_passes": 500,
                                        "max_label_ms": {"rtao trace": 0.1}, "per_call_ratio_max": {"cs 3d5ebf4e": 2.0}})
            r = ev.evaluate(results, test, lib)
            self.assertEqual(r["status"], "fail")
            joined = " ".join(r["reasons"])
            self.assertIn("GPU busy", joined)
            self.assertIn("reflections", joined)
            self.assertIn("rtao trace", joined)
            self.assertIn("per-call ratio", joined)  # split leaves no "before" blocks -> not measurable
            self.assertIn("dump cp1 missing", " ".join(r["invalid_reasons"]))

    def test_max_asserts_criterion(self):
        with tempfile.TemporaryDirectory() as d:
            lines = LINES + ["Runtime: game assert continued (YEBIS camera motion blur, nBaseSamples < 3), 8 so far"]
            results, lib = self.make_results(Path(d), lines=lines)
            r = ev.evaluate(results, dict(TEST, dumps=[], checkpoints=[], criteria={"max_asserts": 0}), lib)
            self.assertEqual(r["status"], "fail")
            self.assertIn("8 continued asserts", " ".join(r["reasons"]))
            self.assertEqual(r["summaries"]["asserts"], {"YEBIS camera motion blur": 8})

    def test_ignored_assert_is_not_a_crash(self):
        with tempfile.TemporaryDirectory() as d:
            lines = LINES + ["G:\\workspace\\yebis\\GPUMath.cpp(3199) : Assertion Failed (nBaseSamples >= 3) in function X",
                             "Runtime: the game's blur-kernel assertion at +0xbbc429 ignored (BB_GAME_ASSERT_STOP=1: stop)"]
            results, lib = self.make_results(Path(d), lines=lines)
            r = ev.evaluate(results, dict(TEST, dumps=[], checkpoints=[], criteria={}), lib)
            self.assertNotEqual(r["status"], "crash")
            self.assertEqual(r["summaries"]["asserts"], {"blur-kernel": 1})

    def test_off_route_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            results, lib = self.make_results(Path(d))
            (lib / "routes" / "stand-60.camera").write_text("0 10.0 2.0 -5.0\n20000 50.0 2.0 -5.0\n")
            test = dict(TEST, dumps=[], criteria={})
            r = ev.evaluate(results, test, lib)
            self.assertEqual(r["status"], "invalid")
            self.assertIn("off-route", r["invalid_reasons"][0])

    def test_crash_and_no_clean_end(self):
        with tempfile.TemporaryDirectory() as d:
            lines = [l for l in LINES if not l.startswith(("Window: closed", "Exit:", "Runtime: pad replay finished"))]
            lines.append("Device lost after submission 12")
            results, lib = self.make_results(Path(d), lines=lines)
            r = ev.evaluate(results, dict(TEST, dumps=[], checkpoints=[]), lib)
            self.assertEqual(r["status"], "crash")
            self.assertFalse(r["timing"]["clean_end"])

    def test_legacy_end_is_not_a_failure(self):
        with tempfile.TemporaryDirectory() as d:
            lines = [l for l in LINES if not l.startswith(("Window: closed", "Exit:"))]
            run = {"commit": "abc", "stamp": "s", "timing": {"end_mode": "legacy", "process_ended": True, "stopped_by_runner": True}}
            results, lib = self.make_results(Path(d), lines=lines, run=run)
            r = ev.evaluate(results, dict(TEST, dumps=[], checkpoints=[]), lib)
            self.assertEqual(r["status"], "pass", r["reasons"])

    def test_no_replay_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            lines = [l for l in LINES if "pad replay" not in l]
            results, lib = self.make_results(Path(d), lines=lines)
            r = ev.evaluate(results, dict(TEST, dumps=[], checkpoints=[]), lib)
            self.assertEqual(r["status"], "invalid")
            self.assertIn("never started", r["invalid_reasons"][0])

    def test_cold_cache_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            results, lib = self.make_results(Path(d))
            r = ev.evaluate(results, dict(TEST, dumps=[], checkpoints=[], criteria={"max_compiles": 0}), lib)
            self.assertEqual(r["status"], "invalid")

    def test_compare_runs(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for stamp, commit, busy in (("1", "aaaa", 14.0), ("2", "aaaa", 14.2), ("3", "bbbb", 15.5)):
                rd = root / "t" / stamp
                rd.mkdir(parents=True)
                (rd / "result.json").write_text(json.dumps({
                    "status": "pass", "commit": commit, "stamp": stamp, "frames": {"p99_ms": 20.0},
                    "gpu_profile": {"busy_ms_median": busy, "labels": {"rtao trace": {"ms_median": 0.7 if commit == "aaaa" else 0.9}}}}))
            table, flags = ev.compare_runs(root, "t", 5, 5.0)
            self.assertEqual([row["commit"] for row in table], ["aaaa", "bbbb"])
            self.assertEqual(table[0]["busy_ms"], 14.1)
            self.assertTrue(any(f.startswith("busy_ms") for f in flags))
            self.assertTrue(any(f.startswith("rtao trace") for f in flags))
            self.assertIn("| commit |", ev.format_compare(table, flags))


class RunnerTests(unittest.TestCase):
    def test_env_file_and_settings(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.env"
            p.write_text('# c\nexport BB_A="1"\nBB_B=two words\n\nnot a line\n')
            self.assertEqual(pt.read_env_file(p), {"BB_A": "1", "BB_B": "two words"})
            s = Path(d) / "rtao.txt"
            s.write_text("rays=2\nkeep=1\n")
            pt.write_live_settings(s, {"rays": 1, "snap": 25.0})
            self.assertEqual(s.read_text(), "rays=1\nkeep=1\nsnap=25\n")

    def test_prepare_user_copies_save_not_player_saves(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d) / "lib"
            save = lib / "saves" / "spot" / "savedata" / "1" / "T" / "S"
            save.mkdir(parents=True)
            (save / "userdata0000").write_bytes(b"frozen")
            user = Path(d) / "user"
            (user / "shader").mkdir(parents=True)
            (user / "shader" / "cache.bin").write_bytes(b"warm")
            (user / "savedata" / "1").mkdir(parents=True)
            (user / "savedata" / "1" / "player").write_bytes(b"mine")
            (user / "rtao.txt").write_text("rays=2\n")
            old_port, old_home = pt.PORT, pt.HOME
            pt.PORT = pt.HOME = Path(d)
            try:
                scratch = Path(d) / "out" / "playtest_user"
                pt.prepare_user(lib, "spot", {"rtao": {"rays": 1}}, scratch)
                self.assertEqual((scratch / "savedata" / "1" / "T" / "S" / "userdata0000").read_bytes(), b"frozen")
                self.assertFalse((scratch / "savedata" / "1" / "player").exists())
                self.assertEqual((scratch / "shader" / "cache.bin").read_bytes(), b"warm")
                self.assertIn("rays=1", (scratch / "rtao.txt").read_text())
                # a second run replaces the save but keeps the caches
                (scratch / "savedata" / "1" / "T" / "S" / "userdata0000").write_bytes(b"moved by the game")
                pt.prepare_user(lib, "spot", {}, scratch)
                self.assertEqual((scratch / "savedata" / "1" / "T" / "S" / "userdata0000").read_bytes(), b"frozen")
                # ... and starts again from the player's settings, without the last run's values
                self.assertEqual((scratch / "rtao.txt").read_text(), "rays=2\n")
                with self.assertRaises(SystemExit):
                    pt.prepare_user(lib, "spot", {"../escape": {}}, scratch)
                # any live settings file; the next run puts back what this one changed
                pt.prepare_user(lib, "spot", {"other": {"k": 1}}, scratch)
                self.assertIn("k=1", (scratch / "other.txt").read_text())
                pt.prepare_user(lib, "spot", {}, scratch)
                self.assertFalse((scratch / "other.txt").exists())    # the player has none
                with self.assertRaises(SystemExit):
                    pt.prepare_user(lib, "nowhere", {}, scratch)
            finally:
                pt.PORT, pt.HOME = old_port, old_home

    def test_latest_draws(self):
        lines = ["Frame stats: 60.0 FPS, worst frame 18 ms (vblank 60 Hz); 0 shader/pipeline compiles, 0.0 ms; GPU thread 5 us/draw, 24 draws/frame",
                 "Runtime: pad file: buttons 0x4000 sticks -1 -1 -1 -1",
                 "Frame stats: 59.0 FPS, worst frame 40 ms (vblank 60 Hz); 1 shader/pipeline compiles, 0.1 ms; GPU thread 5 us/draw, 1400 draws/frame"]
        self.assertEqual(pt.latest_draws(lines[:1]), 24)
        self.assertEqual(pt.latest_draws(lines), 1400)
        self.assertIsNone(pt.latest_draws(["nothing"]))
        # The re-press threshold must sit below a loading scene (466 draws) and above the title menu.
        self.assertLess(pt.TITLE_DRAWS, 466)
        self.assertGreater(pt.TITLE_DRAWS, 111)
        self.assertGreater(pt.LEVEL_DRAWS, pt.TITLE_DRAWS)

    def test_find_test_and_library(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d)
            (lib / "tests" / "bugs").mkdir(parents=True)
            (lib / "tests" / "a.toml").write_text('save = "s"\n')
            (lib / "tests" / "bugs" / "60-x.toml").write_text('save = "s"\n')
            self.assertEqual(pt.find_test(lib, "a").name, "a.toml")
            self.assertEqual(pt.find_test(lib, "60-x").name, "60-x.toml")
            with self.assertRaises(SystemExit):
                pt.find_test(lib, "zzz")
            self.assertEqual(pt.library(str(lib)), lib)
            self.assertEqual(ev.load_test(lib / "tests" / "a.toml")["name"], "a")

    def test_convert_dumps_names_in_order(self):
        if shutil.which("magick") is None:
            self.skipTest("magick not installed")
        with tempfile.TemporaryDirectory() as d:
            raw_dir = Path(d) / "raw"
            raw_dir.mkdir()
            (raw_dir / "f000_final_4x2_rgba.raw").write_bytes(bytes([10, 20, 30, 255] * 8))
            (raw_dir / "f001_final_4x2_bgra.raw").write_bytes(bytes([30, 20, 10, 255] * 8))
            results = Path(d) / "results"
            results.mkdir()
            produced = pt.convert_dumps(raw_dir, results, ["first"], keep_raw=False)
            self.assertEqual([p.name for p in produced], ["first.png", "dump1.png"])
            self.assertEqual(list(raw_dir.glob("*.raw")), [])
            from PIL import Image
            self.assertEqual(Image.open(results / "dumps" / "first.png").convert("RGB").getpixel((0, 0)), (10, 20, 30))
            self.assertEqual(Image.open(results / "dumps" / "dump1.png").convert("RGB").getpixel((0, 0)), (10, 20, 30))

    def test_cli_eval_on_fixture(self):
        with tempfile.TemporaryDirectory() as d:
            results = Path(d) / "r"
            results.mkdir()
            shutil.copy(FIXTURES / "session.log", results / "session.log")
            (results / "run.json").write_text(json.dumps({"commit": "c", "stamp": "s", "timing": {"end_mode": "engine"}}))
            test = Path(d) / "t.toml"
            test.write_text('name = "t"\nsave = "stakes"\n[measure]\ngpu_labels = ["rtao trace"]\n[criteria]\nmin_reflection_passes = 1\n')
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "playtest_eval.py"), str(results), "--test", str(test)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((results / "result.json").exists())
            self.assertIn("PASS", r.stdout)


import contextlib
import io as _io


class WizardTests(unittest.TestCase):
    """The interactive record wizard and its helpers (no game launched)."""

    def _tty(self, answers):
        """Make pt think it is at a terminal and feed `answers` to input(); restore on teardown."""
        import builtins
        it = iter(answers)
        orig_isatty, orig_input = pt._isatty, builtins.input
        pt._isatty = lambda: True
        builtins.input = lambda prompt="": next(it)
        self.addCleanup(lambda: (setattr(pt, "_isatty", orig_isatty),
                                 setattr(builtins, "input", orig_input)))

    def test_prompt_non_tty_uses_default_or_stops(self):
        orig = pt._isatty
        pt._isatty = lambda: False
        self.addCleanup(lambda: setattr(pt, "_isatty", orig))
        self.assertEqual(pt.prompt("q", "fallback"), "fallback")
        with self.assertRaises(SystemExit):
            pt.prompt("q-without-default")

    def test_prompt_and_confirm_and_choose(self):
        self._tty(["", "typed", "n", "y", "9", "1"])
        self.assertEqual(pt.prompt("a", "def"), "def")      # empty -> default
        self.assertEqual(pt.prompt("b"), "typed")
        self.assertFalse(pt.confirm("c"))                   # "n"
        self.assertTrue(pt.confirm("d"))                    # "y"
        # "9" is out of range, re-asks, then "1" picks the second option's value
        with contextlib.redirect_stdout(_io.StringIO()):
            self.assertEqual(pt.choose("pick", [("zero", "z"), ("one", "o")]), "o")

    def test_freeze_current_save(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d) / "lib"
            (lib / "saves").mkdir(parents=True)
            port = Path(d) / "port"
            (port / "user" / "savedata").mkdir(parents=True)
            (port / "user" / "savedata" / "userdata0010").write_text("x")
            orig_port, orig_home, orig_fgr, orig_git = pt.PORT, pt.HOME, pt.foreign_game_running, pt.git_info
            pt.PORT = pt.HOME = port
            pt.foreign_game_running = lambda: None
            pt.git_info = lambda: {"commit": "abc1234567", "branch": "rt", "dirty": False}
            self.addCleanup(lambda: (setattr(pt, "PORT", orig_port), setattr(pt, "HOME", orig_home),
                                     setattr(pt, "foreign_game_running", orig_fgr),
                                     setattr(pt, "git_info", orig_git)))
            pt.freeze_current_save(lib, "spot", "Oedon Chapel lamp", interactive=False)
            self.assertTrue((lib / "saves" / "spot" / "savedata" / "userdata0010").is_file())
            notes = (lib / "saves" / "spot" / "NOTES.md").read_text()
            self.assertIn("Where: Oedon Chapel lamp", notes)
            self.assertEqual(pt.save_where(lib, "spot"), "Oedon Chapel lamp")
            with self.assertRaises(SystemExit):         # an existing save is not overwritten silently
                pt.freeze_current_save(lib, "spot", "again", interactive=False)

    def test_scaffold_test_is_runnable(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "t.toml"
            pt.scaffold_test(path, {"name": "t", "description": 'has "quotes"', "ticket": "",
                                    "save": "lamp", "route": "t", "env_base": "default",
                                    "duration_s": 20}, [(0, 1.0, 2.0, 3.0), (10000, 4.0, 5.0, 6.0)])
            test = ev.load_test(path)
            self.assertEqual(test["save"], "lamp")
            self.assertEqual(test["duration_s"], 20)
            self.assertTrue(test["criteria"]["crash_free"] and test["criteria"]["clean_end"])
            self.assertNotIn("ticket", test)           # blank ticket -> no field
            self.assertIn("#   0 1.00 2.00 3.00", path.read_text())

    def test_scaffold_bug_test_has_ticket(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "b.toml"
            pt.scaffold_test(path, {"name": "b", "description": "d", "ticket": "99", "save": "s",
                                    "route": "b", "env_base": "default", "duration_s": 5}, [])
            self.assertEqual(ev.load_test(path)["ticket"], "99")

    def test_wizard_pick_existing_save(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d)
            for s in ("lamp", "stakes"):
                (lib / "saves" / s / "savedata").mkdir(parents=True)
                (lib / "saves" / s / "NOTES.md").write_text(f"# {s}\n\nWhere: at {s}.\n")
            self._tty(["2"])                            # 0=freeze, 1=lamp, 2=stakes
            with contextlib.redirect_stdout(_io.StringIO()):
                self.assertEqual(pt.wizard_pick_save(lib, "default"), ("stakes", False))

    def test_record_new_playtest_non_tty_without_save_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d)
            (lib / "saves").mkdir()
            (lib / "tests" / "bugs").mkdir(parents=True)
            orig_isatty, orig_fgr, orig_lib = pt._isatty, pt.foreign_game_running, pt.library
            pt._isatty = lambda: False
            pt.foreign_game_running = lambda: None
            pt.library = lambda x, create=False: lib
            self.addCleanup(lambda: (setattr(pt, "_isatty", orig_isatty),
                                     setattr(pt, "foreign_game_running", orig_fgr),
                                     setattr(pt, "library", orig_lib)))
            args = type("A", (), dict(lib=None, target="new", save=None, description=None,
                                      ticket=None, delay_ms=10000, env_base="default", force=False))()
            self.assertEqual(pt.cmd_record(args), ev.EXIT_INVALID)


class GateAndQualityTests(unittest.TestCase):
    """Baseline regression gate, repeat aggregation, image-quality metrics, report, A/B."""

    def _result(self, busy, p99, labels=None, status="pass"):
        return {"commit": "c", "status": status, "reasons": [],
                "gpu_profile": {"busy_ms_median": busy, "labels": labels or {}},
                "frames": {"p99_ms": p99}}

    def test_check_baseline(self):
        base = {"busy_ms": 10.0, "p99_ms": 20.0, "labels": {"ssgi": {"ms_median": 1.0, "per_call_us_median": None}}}
        self.assertEqual(ev.check_baseline({"busy_ms": 10.2, "p99_ms": 20.0,
                                            "labels": {"ssgi": {"ms_median": 1.02}}}, base, 5), [])
        bad = ev.check_baseline({"busy_ms": 12.0, "p99_ms": 20.0,
                                 "labels": {"ssgi": {"ms_median": 1.3}}}, base, 5)
        self.assertTrue(any("GPU busy" in r for r in bad))
        self.assertTrue(any("ssgi" in r for r in bad))
        # an improvement (faster) never fails
        self.assertEqual(ev.check_baseline({"busy_ms": 8.0, "p99_ms": 18.0, "labels": {}}, base, 5), [])

    def test_aggregate_metrics_median(self):
        rs = [self._result(10, 20, {"ssgi": {"ms_median": 1.0, "per_call_us_median": 40}}),
              self._result(12, 22, {"ssgi": {"ms_median": 2.0, "per_call_us_median": 60}}),
              self._result(11, 21, {"ssgi": {"ms_median": 1.5, "per_call_us_median": 50}})]
        m = ev.aggregate_metrics(rs)
        self.assertEqual((m["busy_ms"], m["p99_ms"]), (11, 21))
        self.assertEqual(m["labels"]["ssgi"]["ms_median"], 1.5)

    def test_image_quality_psnr_ssim(self):
        from PIL import Image
        import numpy as np
        with tempfile.TemporaryDirectory() as d:
            arr = np.random.default_rng(0).integers(0, 255, (40, 40, 3)).astype("uint8")
            a, b, c = (Path(d) / n for n in ("a.png", "b.png", "c.png"))
            Image.fromarray(arr).save(a)
            Image.fromarray(arr).save(b)
            Image.fromarray(np.clip(arr.astype(int) + 40, 0, 255).astype("uint8")).save(c)
            same = ev.compare_image(a, b)
            self.assertGreaterEqual(same["ssim"], 0.99)
            self.assertGreaterEqual(same["psnr"], 98)
            worse = ev.compare_image(a, c)
            self.assertLess(worse["ssim"], same["ssim"])
            self.assertLess(worse["psnr"], 98)

    def test_build_report_marks_regression(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d)
            (lib / "tests").mkdir(); (lib / "baselines").mkdir()
            (lib / "tests" / "t.toml").write_text('name = "t"\nsave = "s"\n')
            rd = lib / "results" / "t" / "20260101_000000"
            rd.mkdir(parents=True)
            (rd / "result.json").write_text(json.dumps(self._result(13.0, 25.0)))
            (lib / "baselines" / "t.json").write_text(json.dumps({"busy_ms": 10.0, "p99_ms": 20.0, "labels": {}}))
            md, any_fail = pt.build_report(lib, [lib / "tests" / "t.toml"])
            self.assertTrue(any_fail)
            self.assertIn("REGRESSED", md)
            self.assertIn("+30%", md)   # busy 10 -> 13

    def test_run_suite_skips_routeless_and_continues(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d)
            (lib / "tests" / "bugs").mkdir(parents=True)
            (lib / "env").mkdir()
            (lib / "env" / "default.env").write_text("BB_HDR=1\n")
            (lib / "tests" / "a.toml").write_text('name = "a"\nsave = "s"\nroute = "ra"\n')
            (lib / "tests" / "b.toml").write_text('name = "b"\nsave = "s"\nroute = "rb"\n')
            orig_lib, orig_fg = pt.library, pt.foreign_game_running
            pt.library = lambda x: lib
            pt.foreign_game_running = lambda: None
            self.addCleanup(lambda: (setattr(pt, "library", orig_lib),
                                     setattr(pt, "foreign_game_running", orig_fg)))
            args = type("A", (), {"lib": None, "suite": "all", "tests": [], "repeat": 1,
                                  "accept_goldens": False, "accept_baseline": False,
                                  "keep_raw": False, "reset_user": False, "env": None})()
            buf = _io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = pt.cmd_run(args)                # no game launches: both raise 'route not recorded'
            self.assertEqual(rc, ev.EXIT_INVALID)    # skipped, not aborted
            self.assertEqual(buf.getvalue().upper().count("SKIPPED"), 2)

    def test_set_ini_override(self):
        base = "effect_ssao=1\neffect_ssr=0\nupscaler=fsr3\n"
        out = pt.set_ini(base, "effect_ssr", "1")
        self.assertIn("effect_ssr=1", out)
        self.assertNotIn("effect_ssr=0", out)
        self.assertIn("effect_ssao=1", out)        # other settings untouched
        self.assertIn("upscaler=fsr3", out)
        self.assertEqual(pt.set_ini("", "effect_ssr", "1"), "effect_ssr=1\n")   # append to empty

    def test_format_ab_metrics_diff(self):
        a = self._result(10.0, 20.0, {"ssgi": {"ms_median": 1.0}})
        b = self._result(12.0, 20.0, {"ssgi": {"ms_median": 1.5}})
        args = type("A", (), {"a": ["BB_RTAO=1"], "b": ["BB_RTAO=0"], "min_ssim": 0.98})()
        md, degraded = pt.format_ab("t", args, {"A": (a, Path("/x")), "B": (b, Path("/y"))}, {"dumps": []})
        self.assertFalse(degraded)                 # no dumps -> no image check
        self.assertIn("| busy ms | 10.0 | 12.0 | 2.0 | +20% |", md)


class LauncherEnvTests(unittest.TestCase):
    """launcher_env() reads the launcher's live settings (or returns None to fall back)."""

    def test_reads_print_env(self):
        with tempfile.TemporaryDirectory() as d:
            port = Path(d)
            (port / "launcher").mkdir()
            (port / "launcher" / "bbport_launcher.py").write_text(
                "import sys\n"
                "if '--print-env' in sys.argv:\n"
                "    print('BB_FULLSCREEN=1'); print('BB_HDR=1'); print('# a comment'); print('junk')\n"
                "    sys.exit(0)\n"
                "sys.exit(3)\n")
            orig = pt.PORT; pt.PORT = port; self.addCleanup(lambda: setattr(pt, "PORT", orig))
            env = pt.launcher_env()
            self.assertEqual(env, {"BB_FULLSCREEN": "1", "BB_HDR": "1"})   # comment + junk ignored

    def test_missing_launcher_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            orig = pt.PORT; pt.PORT = Path(d); self.addCleanup(lambda: setattr(pt, "PORT", orig))
            self.assertIsNone(pt.launcher_env())

    def test_failing_launcher_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            port = Path(d); (port / "launcher").mkdir()
            (port / "launcher" / "bbport_launcher.py").write_text("import sys; sys.exit(1)\n")
            orig = pt.PORT; pt.PORT = port; self.addCleanup(lambda: setattr(pt, "PORT", orig))
            self.assertIsNone(pt.launcher_env())


class BurstAndEnvPathTests(unittest.TestCase):
    """Bursts of consecutive present dumps judged by brightness (ticket 81's black flicker) and the
    guard against env paths that no longer exist (a missing mods.json enabled every mod)."""

    def test_dump_names_expand_bursts_per_kind(self):
        names = pt.dump_names([{"at_ms": 100, "kind": "final", "name": "still"},
                               {"at_ms": 7000, "kind": "present", "name": "gate", "count": 3}])
        self.assertEqual(names, {"final": ["still"], "present": ["gate_000", "gate_001", "gate_002"]})

    def test_convert_dumps_counts_each_kind_from_zero(self):
        if shutil.which("magick") is None:
            self.skipTest("magick not installed")
        with tempfile.TemporaryDirectory() as d:
            raw_dir = Path(d) / "raw"
            raw_dir.mkdir()
            (raw_dir / "f000_final_4x2_rgba.raw").write_bytes(bytes([10, 20, 30, 255] * 8))
            (raw_dir / "f000_present_4x2_rgba.raw").write_bytes(bytes([1, 1, 1, 255] * 8))
            (raw_dir / "f001_present_4x2_rgba.raw").write_bytes(bytes([2, 2, 2, 255] * 8))
            results = Path(d) / "results"
            results.mkdir()
            produced = pt.convert_dumps(raw_dir, results, {"final": ["still"], "present": ["gate_000", "gate_001"]}, False)
            self.assertEqual(sorted(p.name for p in produced), ["gate_000.png", "gate_001.png", "still.png"])

    def test_black_frames(self):
        self.assertEqual(ev.black_frames([40, 42, 1.0, 39, 9, 41]), [2, 4])     # 9 < 0.25 * median 39.5
        self.assertEqual(ev.black_frames([20, 20, 6, 18, 19]), [2])             # the 81 dip: a third of its neighbours
        self.assertEqual(ev.black_frames([20, 14, 10, 7, 6]), [])               # a fade is not a drop
        self.assertEqual(ev.black_frames([21, 21, 6.5, 6.3, 18.6, 19]), [2, 3])  # a two-frame dip
        self.assertEqual(ev.black_frames([20, 5, 5, 5, 5, 5, 5, 20]), [])        # longer than drop_span: a scene change
        self.assertEqual(ev.black_frames([6, 5, 6, 5]), [])                     # a dark scene stays dark
        self.assertEqual(ev.black_frames([None, 2.0, 30, 30]), [1])
        self.assertEqual(ev.black_frames([]), [])

    def test_burst_criterion(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            results, lib = EvaluateTests().make_results(Path(d))
            (results / "dumps").mkdir()
            for j, v in enumerate([60, 0, 62]):
                Image.new("RGB", (64, 64), (v, v, v)).save(results / "dumps" / f"gate_{j:03d}.png")
            test = dict(TEST, checkpoints=[], dumps=[{"at_ms": 7000, "kind": "present", "name": "gate", "count": 3}],
                        criteria={"max_black_frames": 0})
            r = ev.evaluate(results, test, lib)
            self.assertEqual(r["status"], "fail")
            self.assertEqual(r["images"][0]["black"], [1])
            self.assertIn("1 dark or dropped frames", " ".join(r["reasons"]))
            self.assertIn("Burst gate: 3 of 3 frames", ev.summary_markdown(r))

    def test_burst_missing_frames_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            results, lib = EvaluateTests().make_results(Path(d))
            test = dict(TEST, checkpoints=[], dumps=[{"at_ms": 7000, "kind": "present", "name": "gate", "count": 3}],
                        criteria={"max_black_frames": 0})
            r = ev.evaluate(results, test, lib)
            self.assertIn("burst gate: 0 of 3 frames dumped", r["invalid_reasons"])

    def test_replay_start_from_mono_stamp(self):
        lines = ["Runtime: pad replay armed (autostart, 5016 ms after scene load)",
                 "Runtime: pad replay started (mono 81317.319 s)"]
        self.assertAlmostEqual(pt.replay_start_wall(lines, now_wall=1000.0, now_mono=81330.319), 987.0, places=3)
        self.assertEqual(pt.replay_start_wall(["no stamp"], now_wall=5.0, now_mono=1.0), 5.0)

    def test_shimmer(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for j in range(4):
                img = Image.new("RGB", (40, 40), (50, 50, 50))
                if j % 2:   # 4 sparkling pixels inside the region's 20x20 = 1 %
                    for x in range(4):
                        img.putpixel((x, 0), (250, 250, 250))
                paths.append(Path(d) / f"s_{j}.png")
                img.save(paths[-1])
            self.assertAlmostEqual(ev.shimmer(paths, [0, 0, 0.5, 0.5]), 1.0, places=3)
            self.assertEqual(ev.shimmer(paths, [0.5, 0.5, 0.5, 0.5]), 0.0)
            self.assertIsNone(ev.shimmer(paths[:1], [0, 0, 1, 1]))

    def test_env_path_problems(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "mods.json").write_text("{}")
            env = {"BB_MODS_CONFIG": str(Path(d) / "mods.json"), "BB_PATCHES_DIR": str(Path(d) / "gone"),
                   "BB_RTAO": "1"}
            problems = pt.env_path_problems(env)
            self.assertEqual(len(problems), 1)
            self.assertIn("BB_PATCHES_DIR", problems[0])


class ReproEnvTests(unittest.TestCase):
    """resolve_repro_env picks the recording's snapshot as the reproduction point, else the fallback."""

    def setUp(self):
        orig = pt.launcher_env          # no real launcher in the tests: the env files decide
        pt.launcher_env = lambda: None
        self.addCleanup(lambda: setattr(pt, "launcher_env", orig))

    def _lib(self, d):
        lib = Path(d)
        (lib / "routes").mkdir(); (lib / "env").mkdir()
        (lib / "env" / "default.env").write_text("BB_HDR=1\nBB_RTAO=1\n")
        return lib

    def test_uses_route_snapshot_when_present(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d)
            (lib / "routes" / "r.env").write_text("# recorded\nBB_FULLSCREEN=1\nBB_HDR=1\nBB_RTAO=1\n")
            env, src = pt.resolve_repro_env(lib, {"route": "r", "env_base": "default"})
            self.assertEqual(src, "routes/r.env")
            self.assertEqual(env["BB_FULLSCREEN"], "1")          # launcher fullscreen, not forced windowed
            self.assertEqual(env["BB_GPU_PROFILE"], "1")         # measurement instrumentation layered on

    def _launcher(self, env):
        orig = pt.launcher_env
        pt.launcher_env = lambda: env
        self.addCleanup(lambda: setattr(pt, "launcher_env", orig))

    def test_falls_back_to_default_env_with_the_launchers_screen(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d)
            (lib / "env" / "default.env").write_text("BB_HDR=1\nBB_FULLSCREEN=0\nBB_DISPLAY=3\n")
            self._launcher({"BB_FULLSCREEN": "1", "BB_DISPLAY": "2"})
            env, src = pt.resolve_repro_env(lib, {"route": "r", "env_base": "default"})
            self.assertIsNone(src)
            self.assertEqual(env["BB_FULLSCREEN"], "1")          # the launcher's screen settings now,
            self.assertEqual(env["BB_DISPLAY"], "2")             # not the env file's
            self.assertEqual(env["BB_HDR"], "1")                 # from env/default.env

    def test_without_a_launcher_fullscreen_on_the_primary_display(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d)
            (lib / "env" / "default.env").write_text("BB_DISPLAY=3\n")
            self._launcher(None)
            env, _ = pt.resolve_repro_env(lib, {"route": "r", "env_base": "default"})
            self.assertEqual(env["BB_FULLSCREEN"], "1")
            self.assertNotIn("BB_DISPLAY", env)


class LibraryTests(unittest.TestCase):
    """A developer's first use: no library yet."""

    def test_init_creates_the_library_once(self):
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d) / "lib"
            orig = pt.launcher_env
            pt.launcher_env = lambda: {"BB_GAME_DIR": "/games/bb", "BB_HDR": "1"}
            self.addCleanup(lambda: setattr(pt, "launcher_env", orig))
            self.assertTrue(pt.init_library(lib))
            for sub in pt.LIB_DIRS:
                self.assertTrue((lib / sub).is_dir(), sub)
            env = pt.read_env_file(lib / "env" / "default.env")
            self.assertEqual(env, {"BB_GAME_DIR": "/games/bb", "BB_HDR": "1"})
            self.assertIn("results/", (lib / ".gitignore").read_text())
            self.assertFalse(pt.init_library(lib))       # nothing left to make
            self.assertEqual(pt.library(str(lib)), lib)

    def test_missing_library_names_the_way_to_make_one(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit) as cm:
                pt.library(str(Path(d) / "none"))
            self.assertIn("record", str(cm.exception))

    def test_missing_system_tools_are_named(self):
        orig_which, orig_fcntl = pt.shutil.which, pt.fcntl
        pt.shutil.which = lambda name: None if name == "pgrep" else "/usr/bin/" + name
        pt.fcntl = None
        try:
            missing = pt.missing_tools()
        finally:
            pt.shutil.which, pt.fcntl = orig_which, orig_fcntl
        self.assertIn("pgrep", missing)
        self.assertTrue(any(m.startswith("fcntl") for m in missing))
        self.assertNotIn("bash", missing)


class EditRemoveTests(unittest.TestCase):
    """The edit and remove subcommands (no game)."""

    def _lib(self, d):
        lib = Path(d)
        (lib / "tests" / "bugs").mkdir(parents=True)
        (lib / "routes").mkdir()
        for s in ("lamp", "stakes"):
            (lib / "saves" / s / "savedata").mkdir(parents=True)
            (lib / "saves" / s / "NOTES.md").write_text(f"# {s}\n\nWhere: at {s}.\n")
        return lib

    def _write_test(self, lib, name, route, save="lamp", extra=""):
        (lib / "tests" / f"{name}.toml").write_text(
            f'name = "{name}"\ndescription = "old"\nsave = "{save}"\nroute = "{route}"\n'
            f'env_base = "default"\nduration_s = 20\ndelay_ms = 3000\nhold_s = 5\n\n'
            f'# keep this comment\n[criteria]\ncrash_free = true\nmax_busy_ms = 15.5\n{extra}')
        for ext in ("pad", "camera", "toml"):
            (lib / "routes" / f"{route}.{ext}").write_text("x")

    def _args(self, lib, **kw):
        return type("A", (), {"lib": None, **kw})()

    def test_set_scalar_replace_insert_remove(self):
        text = 'name = "x"\ndescription = "a"\nsave = "lamp"\n[criteria]\ncrash_free = true\n'
        self.assertIn('description = "b"', pt.set_scalar(text, "description", "b"))
        self.assertIn('ticket = "9"', pt.set_scalar(text, "ticket", "9"))        # inserted near top
        self.assertIn("delay_ms = 1500", pt.set_scalar(text, "delay_ms", 1500))   # int bare
        self.assertNotIn("save =", pt.set_scalar(text, "save", None))             # removed
        self.assertIn("crash_free = true", pt.set_scalar(text, "save", "x"))      # criteria untouched

    def test_edit_flags_preserve_criteria(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            iso = pt._isatty; pt._isatty = lambda: False; self.addCleanup(lambda: setattr(pt, "_isatty", iso))
            rc = pt.cmd_edit(self._args(lib, target="foo", save=None, description="new", ticket="81",
                                        env_base=None, delay_ms=1500))
            self.assertEqual(rc, ev.EXIT_PASS)
            t = ev.load_test(lib / "tests" / "foo.toml")
            self.assertEqual((t["description"], t["ticket"], t["delay_ms"]), ("new", "81", 1500))
            self.assertEqual(t["criteria"]["max_busy_ms"], 15.5)                   # untouched
            self.assertIn("# keep this comment", (lib / "tests" / "foo.toml").read_text())

    def test_edit_interactive_keeps_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            import builtins
            # empty save answer keeps current; new description; blank ticket; empty env+delay keep
            answers = iter(["", "fresh", "", "", ""])
            iso, inp = pt._isatty, builtins.input
            pt._isatty = lambda: True; builtins.input = lambda p="": next(answers)
            self.addCleanup(lambda: (setattr(pt, "_isatty", iso), setattr(builtins, "input", inp)))
            with contextlib.redirect_stdout(_io.StringIO()):
                rc = pt.cmd_edit(self._args(lib, target="foo", save=None, description=None,
                                            ticket=None, env_base=None, delay_ms=None))
            self.assertEqual(rc, ev.EXIT_PASS)
            t = ev.load_test(lib / "tests" / "foo.toml")
            self.assertEqual(t["save"], "lamp")               # kept on empty
            self.assertEqual(t["description"], "fresh")
            self.assertEqual(t["delay_ms"], 3000)             # kept on empty

    def _stub_record(self):
        """Replace record_route with a no-launch stub that records it was called; restore on teardown."""
        calls = []
        orig = pt.record_route
        def fake(lib, route, save, args, interactive):
            calls.append((route, save))
            return ([(0, 1.0, 2.0, 3.0)], 42000, 1)
        pt.record_route = fake
        fg = pt.foreign_game_running; pt.foreign_game_running = lambda: None
        self.addCleanup(lambda: (setattr(pt, "record_route", orig), setattr(pt, "foreign_game_running", fg)))
        return calls

    def test_edit_offers_freeze_current_save(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            frozen = pt.freeze_current_save
            pt.freeze_current_save = lambda lib, spot, where: ((lib / "saves" / spot / "savedata").mkdir(parents=True), spot)[1]
            self.addCleanup(lambda: setattr(pt, "freeze_current_save", frozen))
            calls = self._stub_record()
            import builtins
            # save menu: pick "0" (freeze) -> name "newspot", where, press Enter; then desc/ticket/env/delay kept
            answers = iter(["0", "newspot", "by the gate", "", "", "", "", ""])
            iso, inp = pt._isatty, builtins.input
            pt._isatty = lambda: True; builtins.input = lambda p="": next(answers)
            self.addCleanup(lambda: (setattr(pt, "_isatty", iso), setattr(builtins, "input", inp)))
            with contextlib.redirect_stdout(_io.StringIO()):
                rc = pt.cmd_edit(self._args(lib, target="foo", save=None, description=None,
                                            ticket=None, env_base=None, delay_ms=None))
            self.assertEqual(rc, ev.EXIT_PASS)
            t = ev.load_test(lib / "tests" / "foo.toml")
            self.assertEqual(t["save"], "newspot")
            self.assertEqual(calls, [("foo", "newspot")])   # re-recorded on the new save
            self.assertEqual(t["duration_s"], 42)           # duration synced to the new route length

    def test_edit_refreeze_same_name_rerecords(self):
        # The reported bug: picking "current game state" with the SAME name replaces the save's
        # contents; name is unchanged, but the route must still be re-recorded.
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo", save="lamp")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            frozen = pt.freeze_current_save
            pt.freeze_current_save = lambda lib, spot, where: spot   # pretend the re-freeze happened
            self.addCleanup(lambda: setattr(pt, "freeze_current_save", frozen))
            calls = self._stub_record()
            import builtins
            # "0" freeze, name "" -> keeps "lamp" (same name), where, press Enter; then keep the rest
            answers = iter(["0", "", "same spot", "", "", "", "", ""])
            iso, inp = pt._isatty, builtins.input
            pt._isatty = lambda: True; builtins.input = lambda p="": next(answers)
            self.addCleanup(lambda: (setattr(pt, "_isatty", iso), setattr(builtins, "input", inp)))
            with contextlib.redirect_stdout(_io.StringIO()):
                rc = pt.cmd_edit(self._args(lib, target="foo", save=None, description=None,
                                            ticket=None, env_base=None, delay_ms=None))
            self.assertEqual(rc, ev.EXIT_PASS)
            self.assertEqual(calls, [("foo", "lamp")])   # re-recorded despite the unchanged name

    def test_edit_save_change_rerecords(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            iso = pt._isatty; pt._isatty = lambda: True; self.addCleanup(lambda: setattr(pt, "_isatty", iso))
            calls = self._stub_record()
            with contextlib.redirect_stdout(_io.StringIO()):
                rc = pt.cmd_edit(self._args(lib, target="foo", save="stakes", description=None,
                                            ticket=None, env_base=None, delay_ms=None))
            self.assertEqual(rc, ev.EXIT_PASS)
            self.assertEqual(calls, [("foo", "stakes")])

    def test_edit_save_change_non_tty_refuses(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            iso = pt._isatty; pt._isatty = lambda: False; self.addCleanup(lambda: setattr(pt, "_isatty", iso))
            calls = self._stub_record()
            rc = pt.cmd_edit(self._args(lib, target="foo", save="stakes", description=None,
                                        ticket=None, env_base=None, delay_ms=None))
            self.assertEqual(rc, ev.EXIT_INVALID)   # can't F9-record off a terminal
            self.assertEqual(calls, [])             # nothing launched
            # the save field was still written; the route is flagged out of sync by the message
            self.assertEqual(ev.load_test(lib / "tests" / "foo.toml")["save"], "stakes")

    def test_find_and_remove_by_name_field(self):
        # A bug test filed as tests/bugs/<ticket>-<name>.toml: the summary shows the `name` field,
        # which differs from the file stem; find_test (and so remove) must resolve by that name.
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d)
            (lib / "tests" / "bugs" / "rt-x-shadow-flicker.toml").write_text(
                'name = "shadow-flicker"\nsave = "lamp"\nroute = "shadow-flicker"\n')
            for ext in ("pad", "camera", "toml", "env"):
                (lib / "routes" / f"shadow-flicker.{ext}").write_text("x")
            self.assertEqual(pt.find_test(lib, "shadow-flicker").name, "rt-x-shadow-flicker.toml")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            iso = pt._isatty; pt._isatty = lambda: False; self.addCleanup(lambda: setattr(pt, "_isatty", iso))
            rc = pt.cmd_remove(self._args(lib, target="shadow-flicker", yes=True))
            self.assertEqual(rc, ev.EXIT_PASS)
            self.assertFalse((lib / "tests" / "bugs" / "rt-x-shadow-flicker.toml").exists())

    def test_remove_keeps_shared_route_and_save(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d)
            self._write_test(lib, "foo", "shared")
            self._write_test(lib, "bar", "shared")            # shares the route
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            iso = pt._isatty; pt._isatty = lambda: False; self.addCleanup(lambda: setattr(pt, "_isatty", iso))
            rc = pt.cmd_remove(self._args(lib, target="foo", yes=True))
            self.assertEqual(rc, ev.EXIT_PASS)
            self.assertFalse((lib / "tests" / "foo.toml").exists())
            self.assertTrue((lib / "routes" / "shared.pad").exists())   # bar still uses it
            self.assertTrue((lib / "saves" / "lamp" / "savedata").is_dir())

    def test_remove_without_confirm_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            lib = self._lib(d); self._write_test(lib, "foo", "foo")
            orig = pt.library; pt.library = lambda x: lib; self.addCleanup(lambda: setattr(pt, "library", orig))
            iso = pt._isatty; pt._isatty = lambda: False; self.addCleanup(lambda: setattr(pt, "_isatty", iso))
            rc = pt.cmd_remove(self._args(lib, target="foo", yes=False))
            self.assertEqual(rc, ev.EXIT_INVALID)
            self.assertTrue((lib / "tests" / "foo.toml").exists())       # nothing deleted
            self.assertTrue((lib / "routes" / "foo.pad").exists())


if __name__ == "__main__":
    unittest.main()
