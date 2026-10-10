// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <array>
#include <signal.h>
#include <sys/types.h>

namespace Debugger {

struct CpuRegisters {
    uint64_t rax{0};
    uint64_t rbx{0};
    uint64_t rcx{0};
    uint64_t rdx{0};
    uint64_t rsi{0};
    uint64_t rdi{0};
    uint64_t rbp{0};
    uint64_t rsp{0};
    uint64_t r8{0};
    uint64_t r9{0};
    uint64_t r10{0};
    uint64_t r11{0};
    uint64_t r12{0};
    uint64_t r13{0};
    uint64_t r14{0};
    uint64_t r15{0};
    uint64_t rip{0};
    uint64_t rflags{0};
    bool valid{false};
};

struct RawHitEvent {
    uintptr_t rip{0};
    uintptr_t fault_addr{0};
    pid_t thread_id{0};
    uint64_t timestamp{0};
    CpuRegisters registers{};
};

constexpr size_t RING_BUFFER_CAPACITY = 256;

struct StepSlot {
    std::atomic<pid_t> tid{0};
    std::atomic<uintptr_t> page{0};
    std::atomic<uint64_t> arm_time_ms{0};
};
static constexpr size_t MAX_STEP_SLOTS = 64;

struct WatchpointHit {
    uintptr_t rip{0};
    uintptr_t address{0};
    pid_t thread_id{0};
    uint64_t timestamp{0};
    std::string disassembly;
    CpuRegisters registers{};
    uint64_t count{1};
};

struct SoftwareBreakpoint {
    uintptr_t address{0};
    uint8_t original_byte{0};
    bool enabled{false};
    std::string label;
};

class BreakpointManager {
public:
    static BreakpointManager& Get();
    ~BreakpointManager();

    void Init();

    // Memory Write Watchpoint ("Find what writes to this address")
    bool SetWriteWatchpoint(uintptr_t address);
    void ClearWriteWatchpoint();
    bool HasActiveWatchpoint() const { return watchpoint_active.load(std::memory_order_relaxed); }
    uintptr_t GetWatchedAddress() const { return watched_address.load(std::memory_order_relaxed); }

    // Auto-pause when hit occurs
    bool GetAutoPauseOnHit() const { return auto_pause_on_hit.load(std::memory_order_relaxed); }
    void SetAutoPauseOnHit(bool enable) { auto_pause_on_hit.store(enable, std::memory_order_relaxed); }

    // Software Breakpoint (Code execution break)
    bool AddBreakpoint(uintptr_t address, const std::string& label = "");
    bool RemoveBreakpoint(uintptr_t address);
    bool HasBreakpoint(uintptr_t address) const;
    std::vector<SoftwareBreakpoint> GetBreakpoints() const;

    // Hits history & CPU State
    std::vector<WatchpointHit> GetHits() const;
    void ClearHits();
    CpuRegisters GetLastRegisters() const;
    uintptr_t GetLastHitRip() const;
    uint64_t GetLastWatchedValue() const { return last_watched_value.load(std::memory_order_relaxed); }
    uint64_t GetValueChangeCount() const { return value_change_count.load(std::memory_order_relaxed); }

    // Game execution flow control
    bool IsPaused() const { return game_paused.load(std::memory_order_relaxed); }
    void SetPaused(bool paused) { game_paused.store(paused, std::memory_order_relaxed); }
    void StepFrame() { step_requested.store(true, std::memory_order_relaxed); }
    bool CheckAndClearStep() { return step_requested.exchange(false, std::memory_order_relaxed); }

    // Internal signal handlers callbacks (lock-free & async-signal-safe)
    void OnSignalSegv(uintptr_t fault_addr, uintptr_t rip, pid_t tid, void* ucontext);
    bool OnSignalTrap(uintptr_t rip, pid_t tid, siginfo_t* info, void* ucontext);

    // Dedicated worker thread functions
    void StartWorker();
    void StopWorker();
    void WorkerLoop();
    void PushRawHit(const RawHitEvent& ev);

private:
    BreakpointManager() = default;

    mutable std::mutex mutex;
    std::atomic<bool> initialized{false};

    // Watchpoint state
    std::atomic<bool> watchpoint_active{false};
    std::atomic<uintptr_t> watched_address{0};
    uintptr_t watched_page_start{0};
    size_t watched_page_size{4096};
    std::atomic<bool> waiting_single_step{false};
    std::array<StepSlot, MAX_STEP_SLOTS> step_slots{};

    // Live memory monitor in worker thread
    std::atomic<uint64_t> last_watched_value{0};
    std::atomic<uint64_t> value_change_count{0};
    std::atomic<bool> has_last_watched_value{false};

    // Breakpoint state
    std::vector<SoftwareBreakpoint> breakpoints;
    std::vector<WatchpointHit> hits;
    CpuRegisters last_registers{};
    std::atomic<uintptr_t> last_hit_rip{0};

    // Execution control
    std::atomic<bool> game_paused{false};
    std::atomic<bool> step_requested{false};
    std::atomic<bool> auto_pause_on_hit{false};

    // Lock-free ring buffer between signal handler and worker thread
    RawHitEvent ring_buffer[RING_BUFFER_CAPACITY];
    std::atomic<size_t> ring_write_idx{0};
    std::atomic<size_t> ring_read_idx{0};

    // Dedicated background thread
    std::thread worker_thread;
    std::atomic<bool> worker_running{false};
};

} // namespace Debugger
