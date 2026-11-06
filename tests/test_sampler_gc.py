# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU sampler-GC regression with production lookup, Touch and natural age advancement.

Compile the Sampler declaration, lookup/GC bodies, LRU, defer/pop and timeline
accessors. Only Vulkan allocation, image GC and completion progress are adapters.
The 4000-object allocation limit is modeled; no device or game is used.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from paths import ROOT


def body(text, anchor):
    start = text.index('{', text.index(anchor))
    depth = 0
    for at in range(start, len(text)):
        if text[at] == '{':
            depth += 1
        elif text[at] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:at]
    raise ValueError('unbalanced source: ' + anchor)


VK_STUB = r'''#pragma once
#include <cstdint>
#include <memory>
#include <vector>
namespace vk {
using Sampler=std::uint64_t;
inline unsigned destroyed=0;
inline Sampler next_id=0;
inline std::vector<Sampler> destroyed_ids;
struct UniqueSampler {
    std::unique_ptr<Sampler> value;
    UniqueSampler()=default;
    explicit UniqueSampler(Sampler id):value(std::make_unique<Sampler>(id)){}
    UniqueSampler(UniqueSampler&&) noexcept=default;
    UniqueSampler& operator=(UniqueSampler&&) noexcept=default;
    ~UniqueSampler(){if(value){++destroyed;destroyed_ids.push_back(*value);}}
    Sampler operator*() const{return *value;}
};
}
'''


HARNESS = r'''#include "common/hash.h"
#include "common/lru_cache.h"
#include "common/scope_exit.h"
#include "common/unique_function.h"
#include "textures/sampler.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tsl/robin_map.h>
#include <xxhash.h>
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
namespace Vulkan {
class Instance {};
} // namespace Vulkan
namespace BbToggle {
constexpr int PendingPollLimit = 0;
bool Disabled(int) { return true; }
} // namespace BbToggle
VideoCore::Sampler::Sampler(const Vulkan::Instance &, const Gcn::Sampler &, Gcn::BorderColorBuffer,
                            bool, float) {
    auto created = vk::UniqueSampler(++vk::next_id);
    <<OWNER>>
}
VideoCore::Sampler::~Sampler() = default;
struct Timeline {
    std::atomic<u64> current_tick{1}, gpu_tick{0};
    u64 CurrentTick() const noexcept { <<CURRENT>> }
    u64 KnownGpuTick() const noexcept { <<KNOWN>> }
    bool IsFree(u64 tick) const noexcept { <<FREE>> }
    u64 NextTick() noexcept { <<NEXT>> }
    void Refresh() {}
};
struct Scheduler {
    Timeline work_semaphore;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::deque<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    std::atomic<u32> num_pending_ops{0};
    u32 pending_polls = 0;
    u64 CurrentTick() const { return work_semaphore.CurrentTick(); }
    void DeferOperation(Common::UniqueFunction<void> &&func) { <<DEFER>> }
    void PopPendingOperations() { <<POP>> }
    void Complete(u64 tick) {
        work_semaphore.gpu_tick = tick;
        PopPendingOperations();
    }
};
struct Cache {
    Vulkan::Instance instance;
    Scheduler scheduler;
    std::mutex samplers_mutex;
    tsl::robin_map<u64, VideoCore::Sampler> samplers;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru;
    <<INITIAL_AGE>>
    u64 trigger_gc_samplers, pressure_gc_samplers, critical_gc_samplers;
    explicit Cache(u32 max_samplers) { <<THRESHOLDS>> }
    vk::Sampler GetSampler(const Gcn::Sampler &sampler, Gcn::BorderColorBuffer border_color_base,
                           bool is_depth, float extra_lod_bias) {
        <<GET>>
    }
    void GarbageCollectImages() {} // no images in this sampler-only fixture
    void RunGarbageCollector() { <<RUN_GC>> }
    void GarbageCollectSamplers() { <<GC>> }
    // Only progress/completion is modeled. Age advances exclusively inside production RunGC.
    void CompletedFrame() {
        RunGarbageCollector();
        scheduler.Complete(scheduler.work_semaphore.NextTick());
    }
    vk::Sampler Lookup(u64 key) {
        Gcn::Sampler sampler{};
        sampler.min_lod = u32(key);
        sampler.max_lod = 4095;
        return GetSampler(sampler, Gcn::BorderColorBuffer{}, false, 0.f);
    }
    void Fill() {
        for (u64 key = 0; key < trigger_gc_samplers; ++key)
            Lookup(key);
    }
};
int main(int argc, char **argv) {
    try {
        check(argc == 3, "scenario max-samplers");
        const std::string scenario = argv[1];
        const auto limit = u32(std::stoul(argv[2]));
        check(limit >= 32, "limit too small");
        Cache cache(limit);
        if (scenario != "startup")
            for (int i = 0; i < 20; ++i)
                cache.CompletedFrame();
        const u64 inserted_age = cache.gc_tick;
        cache.Fill();
        const auto target = cache.Lookup(0); // calls production Touch even on repeat lookup
        const auto initial_count = cache.samplers.size();
        if (scenario == "startup" || scenario == "warm-fresh") {
            cache.RunGarbageCollector();
            const auto early = vk::destroyed;
            const bool target_destroyed =
                std::find(vk::destroyed_ids.begin(), vk::destroyed_ids.end(), target) !=
                vk::destroyed_ids.end();
            std::printf("scenario=%s limit=%u trigger=%llu inserted_age=%llu age_after=%llu "
                        "current_tick=%llu completed_tick=%llu early=%u target_destroyed=%d "
                        "remaining=%zu pending=%u\n",
                        argv[1], limit, (unsigned long long)cache.trigger_gc_samplers,
                        (unsigned long long)inserted_age, (unsigned long long)cache.gc_tick,
                        (unsigned long long)cache.scheduler.CurrentTick(),
                        (unsigned long long)cache.scheduler.work_semaphore.KnownGpuTick(), early,
                        target_destroyed, cache.samplers.size(),
                        cache.scheduler.num_pending_ops.load());
            if (scenario == "startup") {
                check(early == 0 && !target_destroyed &&
                          cache.samplers.size() == initial_count - 10 &&
                          cache.scheduler.num_pending_ops == 10,
                      "premature first-GC destruction");
                cache.scheduler.Complete(cache.scheduler.work_semaphore.NextTick());
                check(vk::destroyed == 10 && cache.scheduler.num_pending_ops == 0,
                      "startup final release");
            } else
                check(early == 0 && cache.samplers.size() == initial_count,
                      "fresh Touch did not protect");
        } else {
            // The first reference completes before later frames: the default pacing case.
            cache.scheduler.Complete(cache.scheduler.work_semaphore.NextTick());
            for (int frame = 0; frame < 18; ++frame) {
                if (scenario == "touch-hot")
                    check(cache.Lookup(0) == target, "hot object replaced");
                cache.CompletedFrame();
            }
            bool target_destroyed = std::find(vk::destroyed_ids.begin(), vk::destroyed_ids.end(),
                                              target) != vk::destroyed_ids.end();
            check(vk::destroyed == 10, "cold entries did not age out");
            check(target_destroyed == (scenario == "paced-cold"), "Touch/expiry result wrong");
            std::printf("scenario=%s inserted_age=%llu final_age=%llu destroyed=%u "
                        "target_destroyed=%d completed_before_expiry=1 pending=%u\n",
                        argv[1], (unsigned long long)inserted_age,
                        (unsigned long long)cache.gc_tick, vk::destroyed, target_destroyed,
                        cache.scheduler.num_pending_ops.load());
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
'''

class SamplerGcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('a C++20 compiler is required')
        flags = shlex.split(os.environ.get('CXXFLAGS', ''))
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        folder = Path(cls.tmp.name)
        probe = folder / 'dependencies.cpp'
        probe.write_text('#include <tsl/robin_map.h>\n#include <xxhash.h>\n'
                         '#include <fmt/format.h>\n'
                         'int main() { return int(XXH3_64bits("", 0) == 0); }\n')
        result = subprocess.run(compiler + flags + ['-std=c++20', str(probe),
                                '-lxxhash', '-o', str(folder / 'dependencies')],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise unittest.SkipTest('C++20, robin-map/fmt headers and libxxhash are required')
        vk_header = folder / 'vulkan/vk_common.h'
        vk_header.parent.mkdir()
        vk_header.write_text(VK_STUB)
        read = lambda name: (ROOT / 'gpu' / name).read_text(encoding='utf-8')
        texture = read('textures/texture_cache.cpp')
        memory = read('textures/texture_cache_memory.cpp')
        timeline = read('vulkan/timeline.h')
        replacements = {
            'OWNER': next(line for line in read('textures/sampler.cpp').splitlines()
                          if 'handle =' in line),
            'CURRENT': body(timeline, 'u64 CurrentTick()'),
            'KNOWN': body(timeline, 'u64 KnownGpuTick()'),
            'FREE': body(timeline, 'bool IsFree('),
            'NEXT': body(timeline, 'u64 NextTick()'),
            'DEFER': body(read('render/scheduler.h'), 'void DeferOperation('),
            'POP': body(read('render/scheduler.cpp'), 'void Scheduler::PopPendingOperations('),
            'INITIAL_AGE': next(line.strip() for line in read('textures/texture_cache.h').splitlines()
                                if 'u64 gc_tick =' in line),
            'THRESHOLDS': '\n'.join(line for line in texture.splitlines()
                                    if '_gc_samplers = max_samplers' in line),
            'GET': body(texture, 'vk::Sampler TextureCache::GetSampler('),
            'RUN_GC': body(memory, 'void TextureCache::RunGarbageCollector('),
            'GC': body(memory, 'void TextureCache::GarbageCollectSamplers('),
        }
        code = HARNESS
        for key, value in replacements.items():
            code = code.replace('<<' + key + '>>', value)
        source = folder / 'sampler-gc.cpp'
        source.write_text(code)
        cls.exe = folder / 'sampler-gc'
        result = subprocess.run(compiler + flags + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                                '-I', str(folder), '-I', str(ROOT / 'gpu'), str(source),
                                '-lxxhash', '-pthread', '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, mode):
        result = subprocess.run([str(self.exe), mode, '4000'], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end='')

    def test_first_gc_retains_fresh_samplers_until_render_completion(self):
        self.run_case('startup')

    def test_fresh_lookup_after_warmup_does_not_evict(self):
        self.run_case('warm-fresh')

    def test_completed_cold_samplers_age_out(self):
        self.run_case('paced-cold')

    def test_repeated_lookup_refreshes_the_hot_sampler(self):
        self.run_case('touch-hot')


if __name__ == '__main__':
    unittest.main()
