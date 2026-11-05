# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production preset-file reader without a Vulkan device.

Compile the unchanged OnFrameStart polling/reading block with the real settings
implementation. Only the remaining frame/GPU work is omitted from the harness.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from paths import ROOT


class PresetFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('a C++23 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.folder = Path(cls.tmp.name)
        source = (ROOT / 'gpu/upscale/vk_temporal_upscaler.cpp').read_text(encoding='utf-8')
        start = source.index('static const char* preset_file',
                             source.index('bool TemporalUpscaler::OnFrameStart() {'))
        end = source.index('const auto& settings = BbSettings::Get();', start)
        reader = source[start:end]
        harness = cls.folder / 'preset_file.cpp'
        harness.write_text('''#include <cstdio>
#include <cstdlib>
#include "shim/bb_env.h"
#include "shim/bbport_settings.h"

void Poll() {
    static unsigned preset_file_frames = 0;
''' + reader + '''}

int main(int argc, char** argv) {
    auto& s = BbSettings::Get();
    s.preset = BbSettings::Balanced;
    s.upscaler = BbSettings::UpscalerDlss;
    s.aspect = BbSettings::Aspect16x9;
    s.output_res = BbSettings::PackSize({1920, 1080});
    const int frames = argc > 1 ? std::atoi(argv[1]) : 64;
    for (int i = 0; i < frames; ++i) Poll();
    const auto output = BbSettings::UnpackSize(s.output_res.load());
    std::printf("%d %d %d %d %d\\n", s.preset.load(), s.upscaler.load(),
                s.aspect.load(), output.width, output.height);
    std::fprintf(stderr, "%d\\n", int(BbSettings::UpscalerCount));
}
''', encoding='utf-8')
        cls.exe = cls.folder / 'preset-file-test'
        result = subprocess.run(compiler + ['-std=c++23', '-Wall', '-Wextra', '-Werror',
                                '-I', str(ROOT / 'gpu'), str(harness),
                                str(ROOT / 'gpu/shim/bbport_settings.cpp'),
                                '-o', str(cls.exe)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        # The provider list is the real one, not a copy: a provider added later is covered too.
        probe = subprocess.run([str(cls.exe), '0'], capture_output=True, text=True, check=True)
        cls.providers = int(probe.stderr)

    def read(self, text, frames=64):
        request = self.folder / 'request.txt'
        if text is None:
            request.unlink(missing_ok=True)
        else:
            request.write_text(text, encoding='utf-8')
        env = dict(os.environ, BB_PRESET_FILE=str(request))
        result = subprocess.run([str(self.exe), str(frames)], env=env,
                                capture_output=True, text=True, timeout=5, check=True)
        return tuple(map(int, result.stdout.split()))

    def test_four_fields_apply_provider_and_aspect(self):
        self.assertEqual(self.read('1 0 0 3'), (1, 0, 3, 640, 360))

    def test_three_and_four_fields_accept_every_provider(self):
        for provider in range(self.providers):
            for suffix in ('', ' 3'):
                with self.subTest(provider=provider, suffix=suffix):
                    self.assertEqual(self.read(f'1 0 {provider}{suffix}'),
                                     (1, provider, 3, 640, 360))

    def test_one_and_two_fields_keep_provider(self):
        self.assertEqual(self.read('1'), (1, 5, 3, 1920, 1080))
        self.assertEqual(self.read('1 0'), (1, 5, 3, 640, 360))

    def test_invalid_provider_keeps_previous_provider(self):
        for provider in (-1, self.providers, 999):
            for suffix in ('', ' 2'):
                with self.subTest(provider=provider, suffix=suffix):
                    aspect, size = (2, (640, 400)) if suffix else (3, (640, 360))
                    self.assertEqual(self.read(f'1 0 {provider}{suffix}'), (1, 5, aspect, *size))

    def test_invalid_other_fields_do_not_block_valid_provider(self):
        for text in ('-1 -1 0 -1', '5 999 0 6'):
            with self.subTest(text=text):
                self.assertEqual(self.read(text), (2, 0, 3, 1920, 1080))

    def test_aspect_is_applied_before_output_lookup(self):
        self.assertEqual(self.read('1 0 0 2'), (1, 0, 2, 640, 400))

    def test_missing_empty_and_non_numeric_requests(self):
        for text in (None, '', 'bad', 'bad 0 0 3'):
            with self.subTest(text=text):
                self.assertEqual(self.read(text), (2, 5, 3, 1920, 1080))

    def test_partial_requests_apply_only_scanned_fields(self):
        self.assertEqual(self.read('1 0 bad 2'), (1, 5, 3, 640, 360))
        self.assertEqual(self.read('1 0 0 bad'), (1, 0, 3, 640, 360))

    def test_polling_interval_is_unchanged(self):
        self.assertEqual(self.read('1 0 0 3', frames=63), (2, 5, 3, 1920, 1080))
        self.assertEqual(self.read('1 0 0 3', frames=64), (1, 0, 3, 640, 360))


if __name__ == '__main__':
    unittest.main()
