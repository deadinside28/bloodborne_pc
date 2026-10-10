// SPDX-License-Identifier: GPL-2.0-or-later
#include "breakpoint.h"
#include "mem_scanner.h"
#include "common/decoder.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

#ifndef TRAP_BRKPT
#define TRAP_BRKPT 1
#endif
#ifndef TRAP_TRACE
#define TRAP_TRACE 2
#endif
#ifndef TRAP_BRANCH
#define TRAP_BRANCH 3
#endif
#ifndef TRAP_HWBKPT
#define TRAP_HWBKPT 4
#endif

namespace Debugger {

static struct sigaction old_segv_action;
static struct sigaction old_trap_action;

static void SegvHandler(int sig, siginfo_t* info, void* uctx) {
    auto& mgr = BreakpointManager::Get();
    const uintptr_t fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
    auto* ctx = static_cast<ucontext_t*>(uctx);
#if defined(__x86_64__)
    const uintptr_t rip = ctx->uc_mcontext.gregs[REG_RIP];
#else
    const uintptr_t rip = 0;
#endif
    const pid_t tid = gettid();

    if (mgr.HasActiveWatchpoint()) {
        const uintptr_t watched = mgr.GetWatchedAddress();
        constexpr uintptr_t PAGE_MASK = ~static_cast<uintptr_t>(4095);
        if ((fault_addr & PAGE_MASK) == (watched & PAGE_MASK)) {
            mgr.OnSignalSegv(fault_addr, rip, tid, uctx);
            return;
        }
    }

    if (old_segv_action.sa_flags & SA_SIGINFO) {
        if (old_segv_action.sa_sigaction) {
            old_segv_action.sa_sigaction(sig, info, uctx);
            return;
        }
    } else if (old_segv_action.sa_handler && old_segv_action.sa_handler != SIG_DFL &&
               old_segv_action.sa_handler != SIG_IGN) {
        old_segv_action.sa_handler(sig);
        return;
    }
    // Default action if unhandled
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

static void TrapHandler(int sig, siginfo_t* info, void* uctx) {
    auto& mgr = BreakpointManager::Get();
    auto* ctx = static_cast<ucontext_t*>(uctx);
#if defined(__x86_64__)
    const uintptr_t rip = ctx ? ctx->uc_mcontext.gregs[REG_RIP] : 0;
#else
    const uintptr_t rip = 0;
#endif
    const pid_t tid = gettid();

    if (mgr.OnSignalTrap(rip, tid, info, uctx)) {
        return; // Consumed our watchpoint single-step or breakpoint! Do not forward to guest hooks!
    }

    if (old_trap_action.sa_flags & SA_SIGINFO) {
        if (old_trap_action.sa_sigaction) {
            old_trap_action.sa_sigaction(sig, info, uctx);
            return;
        }
    } else if (old_trap_action.sa_handler && old_trap_action.sa_handler != SIG_DFL &&
               old_trap_action.sa_handler != SIG_IGN) {
        old_trap_action.sa_handler(sig);
        return;
    }
}

BreakpointManager& BreakpointManager::Get() {
    static BreakpointManager instance;
    return instance;
}

BreakpointManager::~BreakpointManager() {
    ClearWriteWatchpoint();
    StopWorker();
}

void BreakpointManager::Init() {
    if (initialized.exchange(true)) {
        return;
    }

    StartWorker();

    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sa.sa_sigaction = SegvHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv_action);

    struct sigaction sa_trap{};
    sa_trap.sa_flags = SA_SIGINFO | SA_NODEFER;
    sa_trap.sa_sigaction = TrapHandler;
    sigemptyset(&sa_trap.sa_mask);
    sigaction(SIGTRAP, &sa_trap, &old_trap_action);
}

void BreakpointManager::StartWorker() {
    if (worker_running.exchange(true)) {
        return;
    }
    worker_thread = std::thread(&BreakpointManager::WorkerLoop, this);
}

void BreakpointManager::StopWorker() {
    if (!worker_running.exchange(false)) {
        return;
    }
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void BreakpointManager::PushRawHit(const RawHitEvent& ev) {
    const size_t cur_w = ring_write_idx.load(std::memory_order_relaxed);
    const size_t next_w = (cur_w + 1) % RING_BUFFER_CAPACITY;
    if (next_w != ring_read_idx.load(std::memory_order_acquire)) {
        ring_buffer[cur_w] = ev;
        ring_write_idx.store(next_w, std::memory_order_release);
    }
}

void BreakpointManager::WorkerLoop() {
    while (worker_running.load(std::memory_order_relaxed)) {
        bool had_work = false;

        while (ring_read_idx.load(std::memory_order_relaxed) != ring_write_idx.load(std::memory_order_acquire)) {
            had_work = true;
            const size_t cur_r = ring_read_idx.load(std::memory_order_relaxed);
            RawHitEvent ev = ring_buffer[cur_r];
            ring_read_idx.store((cur_r + 1) % RING_BUFFER_CAPACITY, std::memory_order_release);

            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = std::find_if(hits.begin(), hits.end(), [&](const WatchpointHit& h) {
                    return h.rip == ev.rip;
                });

                if (it != hits.end()) {
                    it->count++;
                    it->timestamp = ev.timestamp;
                    it->address = ev.fault_addr;
                    it->thread_id = ev.thread_id;
                    if (ev.registers.valid) {
                        it->registers = ev.registers;
                    }
                } else {
                    // Disassemble the instruction safely in the worker thread (outside signal handler)
                    std::string disasm_str;
                    uint8_t code_buf[16];
                    if (ev.rip && MemoryScanner::ReadMemory(ev.rip, code_buf, sizeof(code_buf))) {
                        ZydisDecodedInstruction inst;
                        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                        if (ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(inst, operands, code_buf, sizeof(code_buf)))) {
                            disasm_str = Common::Decoder::Instance()->disassembleInst(inst, operands, ev.rip);
                        }
                    }
                    if (disasm_str.empty()) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "RIP: 0x%016llx", static_cast<unsigned long long>(ev.rip));
                        disasm_str = buf;
                    }

                    hits.push_back({ev.rip, ev.fault_addr, ev.thread_id, ev.timestamp, disasm_str, ev.registers, 1});
                    if (hits.size() > 200) {
                        hits.erase(hits.begin());
                    }
                }
                last_registers = ev.registers;
                last_hit_rip.store(ev.rip, std::memory_order_relaxed);
            }
        }

        // Live value tracker & watchpoint protection watchdog in worker thread
        const uintptr_t target_addr = watched_address.load(std::memory_order_relaxed);
        if (target_addr != 0) {
            uint64_t cur_val = 0;
            if (MemoryScanner::ReadMemory(target_addr, &cur_val, sizeof(cur_val))) {
                if (has_last_watched_value.load(std::memory_order_relaxed)) {
                    if (cur_val != last_watched_value.load(std::memory_order_relaxed)) {
                        value_change_count.fetch_add(1, std::memory_order_relaxed);
                        last_watched_value.store(cur_val, std::memory_order_relaxed);
                    }
                } else {
                    last_watched_value.store(cur_val, std::memory_order_relaxed);
                    has_last_watched_value.store(true, std::memory_order_relaxed);
                }
            }

            // Timeout watchdog: If any thread has been stepping for > 250ms, clean it up
            const auto loop_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
            bool any_timed_out = false;
            for (auto& slot : step_slots) {
                pid_t st_tid = slot.tid.load(std::memory_order_relaxed);
                if (st_tid != 0) {
                    uint64_t arm_t = slot.arm_time_ms.load(std::memory_order_relaxed);
                    if (arm_t != 0 && (static_cast<uint64_t>(loop_now) - arm_t > 250)) {
                        slot.tid.store(0, std::memory_order_release);
                        slot.page.store(0, std::memory_order_release);
                        slot.arm_time_ms.store(0, std::memory_order_release);
                        any_timed_out = true;
                    }
                }
            }
            if (any_timed_out) {
                bool any_stepping = false;
                for (const auto& slot : step_slots) {
                    if (slot.tid.load(std::memory_order_relaxed) != 0) {
                        any_stepping = true;
                        break;
                    }
                }
                if (!any_stepping) {
                    waiting_single_step.store(false, std::memory_order_release);
                        if (mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                                     PROT_READ) != 0) {
                            mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                                     PROT_READ | PROT_EXEC);
                        }
                }
            }
        }

        if (!had_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

bool BreakpointManager::SetWriteWatchpoint(uintptr_t address) {
    Init();
    std::lock_guard<std::mutex> lock(mutex);
    ClearWriteWatchpoint();

    if (!address) return false;

    constexpr uintptr_t PAGE_MASK = ~static_cast<uintptr_t>(4095);
    watched_page_start = address & PAGE_MASK;
    watched_page_size = 4096;
    watched_address.store(address, std::memory_order_release);
    has_last_watched_value.store(false, std::memory_order_release);
    value_change_count.store(0, std::memory_order_release);
    hits.clear();

    for (auto& slot : step_slots) {
        slot.tid.store(0, std::memory_order_release);
        slot.page.store(0, std::memory_order_release);
        slot.arm_time_ms.store(0, std::memory_order_release);
    }
    waiting_single_step.store(false, std::memory_order_release);

    // Make page read-only so any write faults into SegvHandler.
    // Try PROT_READ first (for data/heap pages which do not permit PROT_EXEC),
    // then PROT_READ | PROT_EXEC (for code pages).
    int res = mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size, PROT_READ);
    if (res != 0) {
        res = mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size, PROT_READ | PROT_EXEC);
    }
    if (res != 0) {
        // Fallback: try 16KB alignment (PS4 direct memory page size)
        const uintptr_t page16 = address & ~static_cast<uintptr_t>(16383);
        res = mprotect(reinterpret_cast<void*>(page16), 16384, PROT_READ);
        if (res == 0) {
            watched_page_start = page16;
            watched_page_size = 16384;
        }
    }
    if (res != 0) {
        fprintf(stderr, "BreakpointManager::SetWriteWatchpoint failed for 0x%lx: %s (errno=%d)\n",
                static_cast<unsigned long>(address), strerror(errno), errno);
        watched_address.store(0, std::memory_order_release);
        return false;
    }

    watchpoint_active.store(true, std::memory_order_release);
    return true;
}

void BreakpointManager::ClearWriteWatchpoint() {
    if (!watchpoint_active.load(std::memory_order_relaxed)) return;
    if (watched_page_start) {
        if (mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                     PROT_READ | PROT_WRITE) != 0) {
            mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                     PROT_READ | PROT_WRITE | PROT_EXEC);
        }
    }
    watchpoint_active.store(false, std::memory_order_release);
    watched_address.store(0, std::memory_order_release);
    has_last_watched_value.store(false, std::memory_order_release);
    watched_page_start = 0;
    waiting_single_step.store(false, std::memory_order_release);
    for (auto& slot : step_slots) {
        slot.tid.store(0, std::memory_order_release);
        slot.page.store(0, std::memory_order_release);
        slot.arm_time_ms.store(0, std::memory_order_release);
    }
}

void BreakpointManager::OnSignalSegv(uintptr_t fault_addr, uintptr_t rip, pid_t tid, void* uctx) {
    auto* ctx = static_cast<ucontext_t*>(uctx);
    if (!ctx) return;

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    CpuRegisters regs{};
#if defined(__x86_64__)
    regs.rax = ctx->uc_mcontext.gregs[REG_RAX];
    regs.rbx = ctx->uc_mcontext.gregs[REG_RBX];
    regs.rcx = ctx->uc_mcontext.gregs[REG_RCX];
    regs.rdx = ctx->uc_mcontext.gregs[REG_RDX];
    regs.rsi = ctx->uc_mcontext.gregs[REG_RSI];
    regs.rdi = ctx->uc_mcontext.gregs[REG_RDI];
    regs.rbp = ctx->uc_mcontext.gregs[REG_RBP];
    regs.rsp = ctx->uc_mcontext.gregs[REG_RSP];
    regs.r8  = ctx->uc_mcontext.gregs[REG_R8];
    regs.r9  = ctx->uc_mcontext.gregs[REG_R9];
    regs.r10 = ctx->uc_mcontext.gregs[REG_R10];
    regs.r11 = ctx->uc_mcontext.gregs[REG_R11];
    regs.r12 = ctx->uc_mcontext.gregs[REG_R12];
    regs.r13 = ctx->uc_mcontext.gregs[REG_R13];
    regs.r14 = ctx->uc_mcontext.gregs[REG_R14];
    regs.r15 = ctx->uc_mcontext.gregs[REG_R15];
    regs.rip = ctx->uc_mcontext.gregs[REG_RIP];
    regs.rflags = ctx->uc_mcontext.gregs[REG_EFL];
    regs.valid = true;
#endif

    // Record the hit to lock-free ring buffer
    RawHitEvent ev{};
    ev.rip = rip;
    ev.fault_addr = fault_addr;
    ev.thread_id = tid;
    ev.timestamp = static_cast<uint64_t>(now);
    ev.registers = regs;
    PushRawHit(ev);

    if (auto_pause_on_hit.load(std::memory_order_relaxed)) {
        game_paused.store(true, std::memory_order_relaxed);
    }

    // Unprotect page with PROT_READ | PROT_WRITE so the instruction can complete
    if (watched_page_start) {
        if (mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                     PROT_READ | PROT_WRITE) != 0) {
            mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                     PROT_READ | PROT_WRITE | PROT_EXEC);
        }
    }

    // Arm per-thread step slot
    for (auto& slot : step_slots) {
        pid_t expected = 0;
        if (slot.tid.load(std::memory_order_relaxed) == tid ||
            slot.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            slot.page.store(watched_page_start, std::memory_order_release);
            slot.arm_time_ms.store(static_cast<uint64_t>(now), std::memory_order_release);
            break;
        }
    }

#if defined(__x86_64__)
    // Enable Trap Flag (single-step) so we catch control right after the instruction
    ctx->uc_mcontext.gregs[REG_EFL] |= 0x100;
#endif
    waiting_single_step.store(true, std::memory_order_release);
}

bool BreakpointManager::OnSignalTrap(uintptr_t rip, pid_t tid, siginfo_t* info, void* uctx) {
    auto* ctx = static_cast<ucontext_t*>(uctx);
    if (!ctx) return false;

    // 1. Check if this is a single-step (trace trap) from our watchpoint
    bool is_trace = false;
#if defined(__x86_64__)
    if ((ctx->uc_mcontext.gregs[REG_EFL] & 0x100) != 0) {
        is_trace = true;
    }
#endif
    if (info && (info->si_code == TRAP_TRACE || info->si_code == SI_KERNEL || info->si_code == TRAP_HWBKPT)) {
        is_trace = true;
    }

    bool tid_in_slots = false;
    for (auto& slot : step_slots) {
        if (slot.tid.load(std::memory_order_relaxed) == tid) {
            tid_in_slots = true;
            slot.tid.store(0, std::memory_order_release);
            slot.page.store(0, std::memory_order_release);
            slot.arm_time_ms.store(0, std::memory_order_release);
            break;
        }
    }

    if (tid_in_slots || is_trace || waiting_single_step.load(std::memory_order_relaxed)) {
#if defined(__x86_64__)
        // Unconditionally clear Trap Flag so subsequent instructions do not trap!
        ctx->uc_mcontext.gregs[REG_EFL] &= ~static_cast<greg_t>(0x100);
#endif
        // Check if any other threads are still single-stepping
        bool any_stepping = false;
        for (const auto& slot : step_slots) {
            if (slot.tid.load(std::memory_order_relaxed) != 0) {
                any_stepping = true;
                break;
            }
        }

        if (!any_stepping) {
            waiting_single_step.store(false, std::memory_order_release);
            // Re-apply protection if watchpoint is still active
            if (watchpoint_active.load(std::memory_order_relaxed) && watched_page_start) {
                if (mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                             PROT_READ) != 0) {
                    mprotect(reinterpret_cast<void*>(watched_page_start), watched_page_size,
                             PROT_READ | PROT_EXEC);
                }
            }
        }
        return true; // Consumed! Never forward single-step traps to guest hooks!
    }

    // 2. Check for software breakpoints (INT3 hit)
    const uintptr_t bp_addr = rip ? (rip - 1) : 0;
    if (bp_addr && HasBreakpoint(bp_addr)) {
        if (auto_pause_on_hit.load(std::memory_order_relaxed)) {
            game_paused.store(true, std::memory_order_relaxed);
        }
        return true;
    }

    return false;
}

bool BreakpointManager::AddBreakpoint(uintptr_t address, const std::string& label) {
    Init();
    std::lock_guard<std::mutex> lock(mutex);
    uint8_t orig = 0;
    if (!MemoryScanner::ReadMemory(address, &orig, 1)) {
        return false;
    }
    constexpr uint8_t INT3 = 0xCC;
    if (!MemoryScanner::WriteMemory(address, &INT3, 1)) {
        return false;
    }
    breakpoints.push_back({address, orig, true, label});
    return true;
}

bool BreakpointManager::RemoveBreakpoint(uintptr_t address) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = breakpoints.begin(); it != breakpoints.end(); ++it) {
        if (it->address == address) {
            MemoryScanner::WriteMemory(address, &it->original_byte, 1);
            breakpoints.erase(it);
            return true;
        }
    }
    return false;
}

bool BreakpointManager::HasBreakpoint(uintptr_t address) const {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& bp : breakpoints) {
        if (bp.address == address && bp.enabled) return true;
    }
    return false;
}

std::vector<SoftwareBreakpoint> BreakpointManager::GetBreakpoints() const {
    std::lock_guard<std::mutex> lock(mutex);
    return breakpoints;
}

std::vector<WatchpointHit> BreakpointManager::GetHits() const {
    std::lock_guard<std::mutex> lock(mutex);
    return hits;
}

void BreakpointManager::ClearHits() {
    std::lock_guard<std::mutex> lock(mutex);
    hits.clear();
}

CpuRegisters BreakpointManager::GetLastRegisters() const {
    std::lock_guard<std::mutex> lock(mutex);
    return last_registers;
}

uintptr_t BreakpointManager::GetLastHitRip() const {
    return last_hit_rip.load(std::memory_order_relaxed);
}

} // namespace Debugger
