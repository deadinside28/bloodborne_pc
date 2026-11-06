# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""A queued submission must not be visible to the decoder before its count is published.

Compiles the actual SubmitGfx/SubmitAsc publication blocks and the consumer's pop
block from gpu/cp/command_processor.cpp. The producer is paused at its publication
point (after the queue push, before the counter); the queue is then inspected the
way the decoder does it, under the queue mutex. If the task is visible there, the
count must already cover it.

Why the pause is injected instead of taking submit_mutex: the decoder takes the
same mutex inside its pop block, so parking the producer on it would park the
consumer too -- which is exactly why the window is short in production. The pause
sits on the boundary under test; the queue, both mutexes and the counter are real.

No Vulkan device and no command decoding: only the queue, the mutexes and the
counter take part, which is where the ordering lives.

Also executes the production SignalGpuIdle and WaitGpuIdle bodies. Rasterizer
and GPU execution are omitted: idle callbacks are explicitly invoked by the test.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

from paths import ROOT

SOURCE = ROOT / 'gpu/cp/command_processor.cpp'
PUSH = 'queue.works.push_back(std::move(work));'


def _block_start(text, pos):
    """Index of the line holding the lone '{' that opens the block around `pos`."""
    opener = None
    for match in re.finditer(r'\n([ \t]*)\{\n', text[:pos]):
        opener = match
    if opener is None:
        raise ValueError('no block opener before offset %d' % pos)
    return opener.start() + 1


def _match_brace(text, start):
    depth = 0
    for index in range(start, len(text)):
        if text[index] == '{':
            depth += 1
        elif text[index] == '}':
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError('unbalanced block at offset %d' % start)


def _producer_block(text, function):
    """One producer: the queue push under the queue mutex, then the count publication."""
    entry = text.index('void CommandProcessor::%s' % function)
    push = text.index(PUSH, entry)
    start = _block_start(text, push)
    end = text.index('submit_cv.notify_one();', push) + len('submit_cv.notify_one();')
    block = text[start:end]
    block = block.replace(PUSH, PUSH + '\n        NotePushed();')
    # Pause on the boundary under test: before the count's lock, wherever it is.
    return re.sub(r'([ \t]*)(std::scoped_lock \w+\{submit_mutex\};)',
                  r'\1HoldPublication();\n\1\2', block)


def _pop_block(text):
    entry = text.index('void CommandProcessor::Run(')
    pop = text.index('queue.works.pop_front();', entry)
    start = _block_start(text, pop)
    return text[start:_match_brace(text, start)]


def _statement(text, start_anchor, statement):
    entry = text.index(start_anchor)
    at = text.index(statement, entry)
    return text[at:at + len(statement)]


def _body(text, anchor):
    entry = text.index(anchor)
    start = text.index('{', entry)
    return text[start + 1:_match_brace(text, start) - 1]


def _idle_snapshot(text):
    entry = text.index('void CommandProcessor::AfterBatch(')
    at = text.index('decoded = num_submits == 0;', entry)
    start = _block_start(text, at)
    return text[start:_match_brace(text, start)]


HARNESS = r'''#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <latch>
#include <functional>
#include <string>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr u64 NoSeq = ~u64{0};

namespace BbStats {
std::atomic<u64> guest_submit_stamp{0};
}
namespace BbTimeline {
inline constexpr int GuestSubmit = 0;
inline void Note(int, u64 = 0, u64 = 0) {}
}
namespace BbFreeCheck {
inline void NoteSubmit(u64, const u32*, size_t) {}
}
struct SubmittedCopy {
    const u32* guest_dcb = nullptr;
};
struct Prep {
    void Enqueue(u64, void*) {}
    void* Build(std::span<const u32>) { return nullptr; }
};
struct Work {
    std::span<const u32> dcb, ccb;
    u64 seq = NoSeq;
    u64 stamp = 0;
    std::shared_ptr<SubmittedCopy> copy;
};

namespace BbIrq {
enum class Line { GpuIdle };
std::atomic<unsigned> raises{0};
void Raise(Line) { ++raises; }
}
// A diagnostic wrapper around a real mutex. While the producer is paused, the
// flag lets the observer distinguish a held queue lock without try_lock's
// permitted spurious failure. Lock acquisition/release order is unchanged.
struct TrackedMutex {
    std::mutex mutex;
    std::atomic<bool> held{false};
    void lock() { mutex.lock(); held.store(true); }
    void unlock() { held.store(false); mutex.unlock(); }
};
struct State {
    struct Queue {
        TrackedMutex mutex;
        std::deque<std::unique_ptr<Work>> works;
    } queue;
    std::mutex submit_mutex;
    std::condition_variable submit_cv;
    std::atomic<u32> num_submits{0};
    std::atomic<u32> num_commands{0};
    u64 submissions_total = 0;
    u64 decoded_total = 0;
    u32 num_mapped_queues = 1;
    u32 gnm_vqid = 1;
    u64 gfx_submit_seq = 0;
    std::atomic<bool> work_retired{true};
    Prep* draw_prep = nullptr;
    std::span<const u32> dcb, ccb;
    Work* last_decoded = nullptr;
    std::deque<std::pair<u64, u64>> frame_ends;
    bool frame_end = false;
    std::atomic<bool> armed{false};
    std::latch pushed{1};
    std::latch pub{1};
    std::latch pub_release{1};

    void NotePushed() {
        if (armed.load(std::memory_order_acquire)) {
            pushed.count_down();
        }
    }

    void HoldPublication() {
        if (!armed.load(std::memory_order_acquire)) {
            return;
        }
        pub.count_down();
        pub_release.wait();
    }

    void Submit(bool gfx) {
        auto work = std::make_unique<Work>();
        auto* draw_prep = this->draw_prep;
        [[maybe_unused]] void* prepared = nullptr;
        if (gfx) {
<<GFX>>
        } else {
<<ASC>>
        }
    }

    void SignalGpuIdle(u64 generation) {
<<SIGNAL>>
    }

    std::function<void()> IdleCallback() {
        u64 generation;
        bool decoded;
<<SNAPSHOT>>
        (void)decoded;
        return <<CALLBACK>>;
    }

    std::latch waiting{1};
    std::atomic<bool> wait_noted{false};
    void NoteWaitPredicate() {
        if (num_submits != 0 && !wait_noted.exchange(true)) waiting.count_down();
    }
    void WaitGpuIdle() {
<<WAIT>>
    }

    void PopOne() {
<<POP>>
    }

    bool Empty() {
        std::scoped_lock lock{queue.mutex};
        return queue.works.empty();
    }

    /// AfterBatch's own decision, on the same counter.
    bool WouldBeIdle() {
        bool decoded = false;
        <<IDLE>>
        return decoded;
    }

    /// Run()'s loop condition, on the same counters.
    bool LoopContinues() {
        return (<<LOOP>>);
    }
};

struct Result {
    bool visible = false;
    u32 counted = 0;
    u32 queued = 0;
    bool premature = false;
    u32 left = 0;
    u64 total = 0;
    unsigned early_idle = 0;
    unsigned stale_idle = 0;
    unsigned fresh_idle = 0;
};

Result Run(bool gfx) {
    State s;
    s.Submit(gfx); // the counted one
    s.armed.store(true, std::memory_order_release);
    std::thread producer([&] { s.Submit(gfx); }); // pushed, not yet counted
    s.pushed.wait();
    s.pub.wait();

    Result r;
    {
        r.visible = !s.queue.mutex.held.load();
        if (r.visible) {
            std::scoped_lock q{s.queue.mutex};
            r.counted = s.num_submits.load();
            r.queued = u32(s.queue.works.size());
        }
    }
    if (r.visible) {
        s.PopOne();
        r.premature = s.WouldBeIdle() && !s.LoopContinues();
    }
    auto idle = s.IdleCallback();
    BbIrq::raises = 0;
    idle();
    r.early_idle = BbIrq::raises.exchange(0);
    s.pub_release.count_down();
    producer.join();
    while (!s.Empty()) {
        s.PopOne();
    }
    // The callback captured before the new publication must now be stale even
    // after all work is drained: the generation guard must reject it.
    idle();
    r.stale_idle = BbIrq::raises.exchange(0);
    s.IdleCallback()();
    r.fresh_idle = BbIrq::raises.exchange(0);
    r.left = s.num_submits.load();
    r.total = s.submissions_total;
    return r;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "publication";
    if (mode == "wait") {
        State s;
        s.Submit(true);
        std::atomic<bool> returned{false};
        std::thread waiter([&] { s.WaitGpuIdle(); returned = true; });
        s.waiting.wait(); // actual wait predicate observed outstanding work
        const bool early = returned.load();
        s.PopOne();      // production queue -> submit pop, then notify_all
        waiter.join();
        std::printf("wait: early=%d returned=%d count=%u\n", int(early),
                    int(returned.load()), s.num_submits.load());
        return early || !returned || s.num_submits != 0 ? 1 : 0;
    }
    int bad = 0;
    for (int i = 0; i < 2; ++i) {
        const bool gfx = i == 0;
        const Result r = Run(gfx);
        if (r.visible) {
            std::printf("%s: snapshot=available counted=%u queued=%u premature_idle=%d left=%u total=%llu\n",
                        gfx ? "SubmitGfx" : "SubmitAsc", r.counted, r.queued,
                        int(r.premature), r.left, static_cast<unsigned long long>(r.total));
        } else {
            std::printf("%s: queue_locked snapshot=not_taken left=%u total=%llu\n",
                        gfx ? "SubmitGfx" : "SubmitAsc", r.left,
                        static_cast<unsigned long long>(r.total));
        }
        if (mode == "publication" && r.visible) {
            std::printf("  a queued submission was visible before its count was published\n");
            ++bad;
        }
        if (r.premature) {
            std::printf("  the decoder leaves the loop with a submission still queued\n");
        }
        std::printf("  idle callbacks: before_publication=%u stale=%u fresh=%u\n",
                    r.early_idle, r.stale_idle, r.fresh_idle);
        if (mode == "early-idle" && r.early_idle != 0) ++bad;
        if (mode == "generation" && (r.stale_idle != 0 || r.fresh_idle != 1)) ++bad;
        if (r.left != 0 || r.total != 2) {
            std::printf("  the count and the queue do not agree after the drain\n");
            ++bad;
        }
    }
    return bad == 0 ? 0 : 1;
}
'''


class SubmissionPublishTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('a C++20 compiler is required')
        text = SOURCE.read_text(encoding='utf-8')
        callback = re.search(r'rasterizer->SignalAfterGpu\((\[this, generation\] \{ SignalGpuIdle\(generation\); \})\);', text)
        if callback is None:
            raise ValueError('idle callback anchor changed')
        wait = _body(text, 'void CommandProcessor::WaitGpuIdle(')
        wait = wait.replace('return num_submits == 0;',
                            'NoteWaitPredicate(); return num_submits == 0;')
        code = (HARNESS
                .replace('<<SIGNAL>>', _body(text, 'void CommandProcessor::SignalGpuIdle('))
                .replace('<<SNAPSHOT>>', _idle_snapshot(text))
                .replace('<<CALLBACK>>', callback.group(1))
                .replace('<<WAIT>>', wait)
                .replace('<<GFX>>', _producer_block(text, 'SubmitGfx'))
                .replace('<<ASC>>', _producer_block(text, 'SubmitAsc'))
                .replace('<<POP>>', _pop_block(text))
                .replace('<<IDLE>>', _statement(text, 'void CommandProcessor::AfterBatch(',
                                                'decoded = num_submits == 0;'))
                .replace('<<LOOP>>', _statement(text, 'void CommandProcessor::Run(',
                                                'while (num_submits || num_commands) {')
                         [len('while ('):]
                         [:-len(') {')]))
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        folder = Path(cls.tmp.name)
        source = folder / 'publish.cpp'
        source.write_text(code, encoding='utf-8')
        cls.exe = folder / 'publish-test'
        result = subprocess.run(compiler + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                                            '-pthread', str(source), '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end='')

    def test_count_is_published_before_the_task_is_visible(self):
        self.run_case('publication')

    def test_idle_callback_cannot_accept_unpublished_queued_work(self):
        self.run_case('early-idle')

    def test_generation_rejects_stale_callback_and_accepts_fresh_idle(self):
        self.run_case('generation')

    def test_wait_gpu_idle_blocks_then_wakes_after_the_real_pop(self):
        self.run_case('wait')


if __name__ == '__main__':
    unittest.main()
