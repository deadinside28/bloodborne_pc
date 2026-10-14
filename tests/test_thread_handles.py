from paths import ROOT
import subprocess
import unittest

EXE = ROOT / 'out/thread-handles-test'


@unittest.skipUnless(EXE.exists(), 'run bash build.sh --test first')
class ThreadHandleTests(unittest.TestCase):
    def test_worker_allocations_fit_guest_lock_owner(self):
        r = subprocess.run([str(EXE)], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('PASS: 256 guest thread handles', r.stdout)
