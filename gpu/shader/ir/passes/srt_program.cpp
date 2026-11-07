// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The SRT walker as a program (srt.h SrtOp): the same walk as the x86 code the userdata pass
// generates, for hosts without it (AArch64) and to check that code against (BB_SRT_INTERP).
//
// A load from unmapped memory (a stale pointer in a table) reads 0 for that load, as the x86
// walker's fault handler does: the fault returns here (runtime_fault_recover), the load is
// marked, and the walk runs again from the start (it only writes the flat buffer).
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/arch.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "shader/ir/passes/srt.h"
#include "shim/bb_env.h"
#include "shim/bbport_toggles.h"

namespace Shader {

namespace {

constexpr u64 PointerMask = 0xFFFFFFFFFFFFULL; // the tables' 48-bit pointers
constexpr u32 MaxDepth = 32;
constexpr u32 MaxFaults = 32;

/// The walk in progress on this thread: which loads read 0 (they faulted on an earlier try).
struct Walk {
    u32 load{};
    u32 zeroed[MaxFaults]{};
    u32 zeroed_count{};
};
thread_local Walk walk;

u64 Load(u64 address, u32 size) {
    const u32 index = walk.load++;
    for (u32 i = 0; i < walk.zeroed_count; ++i) {
        if (walk.zeroed[i] == index) {
            return 0;
        }
    }
    if (size == 8) {
        return *reinterpret_cast<const volatile u64*>(address);
    }
    return *reinterpret_cast<const volatile u32*>(address);
}

u64 Disp(u32 word) {
    return static_cast<u64>(static_cast<s64>(static_cast<s32>(word)));
}

void Execute(const u32* p, const u32* user_data, u32* flat) {
    u64 table = reinterpret_cast<u64>(user_data);
    u64 tables[MaxDepth];
    u32 depth = 0;
    u32 values[MaxDepth];
    u32 sp = 0;
    const auto pop = [&] { return values[--sp]; };
    const auto push = [&](u32 v) { values[sp++] = v; };
    for (;;) {
        const u32 op = *p++;
        switch (op) {
        case SrtOp::End:
            return;
        case SrtOp::PushTableImm:
            tables[depth++] = table;
            table = Load(table + Disp(*p++), 8) & PointerMask;
            break;
        case SrtOp::PushTableExpr: {
            const u32 v = pop();
            tables[depth++] = table;
            table = Load(table + static_cast<u32>(v << 2), 8) & PointerMask;
            break;
        }
        case SrtOp::PopTable:
            table = tables[--depth];
            break;
        case SrtOp::CopyImm: {
            const u32 slot = *p++;
            flat[slot] = static_cast<u32>(Load(table + Disp(*p++), 4));
            break;
        }
        case SrtOp::CopyExpr: {
            const u32 slot = *p++;
            flat[slot] = static_cast<u32>(Load(table + static_cast<u32>(pop() << 2), 4));
            break;
        }
        case SrtOp::Flat:
            push(flat[*p++]);
            break;
        case SrtOp::Imm:
            push(*p++);
            break;
        case SrtOp::Not:
            push(~pop());
            break;
        case SrtOp::Bfe: {
            const u32 count = pop(), offset = pop(), base = pop();
            push((base >> (offset & 31)) & ((1u << (count & 31)) - 1));
            break;
        }
        case SrtOp::BfeImmCount: {
            const u32 count = *p++;
            const u32 offset = pop(), base = pop();
            const u32 v = base >> (offset & 31);
            push(count < 32 ? v & ((1u << count) - 1) : v);
            break;
        }
        default: {
            const u32 b = pop(), a = pop();
            switch (op) {
            case SrtOp::Add: push(a + b); break;
            case SrtOp::Sub: push(a - b); break;
            case SrtOp::Mul: push(a * b); break;
            case SrtOp::And: push(a & b); break;
            case SrtOp::Or: push(a | b); break;
            case SrtOp::Xor: push(a ^ b); break;
            case SrtOp::UMin: push(a < b ? a : b); break;
            case SrtOp::UMax: push(a > b ? a : b); break;
            case SrtOp::Shl: push(a << (b & 31)); break;
            case SrtOp::Shr: push(a >> (b & 31)); break;
            default: UNREACHABLE_MSG("SRT program: opcode {}", op);
            }
        }
        }
    }
}

} // namespace

void RunSrtProgram(const u32* program, const u32* user_data, u32* flat_dst) {
    sigjmp_buf* const outer = runtime_fault_recover;
    sigjmp_buf recover;
    walk.zeroed_count = 0;
    for (;;) {
        walk.load = 0;
        if (sigsetjmp(recover, 0)) {
            // runtime_fault_recover was cleared by the fault handler; walk.load is the load
            if (walk.zeroed_count == MaxFaults) {
                runtime_fault_recover = outer;
                return; // tables that broken: what was copied stays
            }
            walk.zeroed[walk.zeroed_count++] = walk.load - 1;
            static std::atomic<u64> faults{0};
            const u64 count = faults.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 5 || (count & (count - 1)) == 0) {
                LOG_WARNING(Render_Recompiler, "SRT walker program read unavailable memory "
                            "({} reads so far); supplied zero for this draw", count);
            }
            continue;
        }
        runtime_fault_recover = &recover;
        Execute(program, user_data, flat_dst);
        runtime_fault_recover = outer;
        return;
    }
}

#ifdef ARCH_X86_64
std::atomic<u64> walks{0}, differ{0}, races{0}; // BB_SRT_INTERP=verify
#endif

void PersistentSrtInfo::RunWalker(const u32* user_data, u32* flat_dst) const {
#ifdef ARCH_X86_64
    // 0: the x86 walker; 1: the program; 2: both, compared (BB_SRT_INTERP=verify)
    static const int mode = [] {
        const char* v = bb_env("BB_SRT_INTERP");
        return v && !std::strcmp(v, "verify") ? 2 : bb_env_on("BB_SRT_INTERP") ? 1 : 0;
    }();
    if (walker_func && (mode == 0 || walker_program.empty())) {
        walker_func(user_data, flat_dst);
        return;
    }
    if (walker_func && mode == 2) {
        // Both, compared. The game may change its tables between the two walks: a difference
        // is checked again (both walks, up to 4 times) before it counts; one that goes away is a
        // race, one that stays is the program's.
        const std::vector<u32> start(flat_dst, flat_dst + flattened_bufsize_dw);
        std::vector<u32> check;
        bool same = false;
        for (int attempt = 0; attempt < 4 && !same; ++attempt) {
            std::memcpy(flat_dst, start.data(), start.size() * sizeof(u32));
            check = start;
            walker_func(user_data, flat_dst);
            RunSrtProgram(walker_program.data(), user_data, check.data());
            same = std::memcmp(check.data(), flat_dst, start.size() * sizeof(u32)) == 0;
            if (!same && attempt == 0) {
                races.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const u64 n = walks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (same) {
            if ((n & (n - 1)) == 0 && n >= 1024) {
                std::printf("SRT verify: %llu walks, %llu differ (%llu differed once: tables "
                            "changing between the walks)\n", (unsigned long long)n,
                            (unsigned long long)differ.load(std::memory_order_relaxed),
                            (unsigned long long)races.load(std::memory_order_relaxed));
            }
            return;
        }
        u32 w = 0;
        while (check[w] == flat_dst[w]) {
            ++w;
        }
        const u64 d = differ.fetch_add(1, std::memory_order_relaxed) + 1;
        if (d <= 10 || (d & (d - 1)) == 0) {
            std::printf("SRT verify: program and x86 walker differ at dword %u (%#x, x86 %#x), "
                        "4 times; %llu of %llu walks\n", w, check[w], flat_dst[w],
                        (unsigned long long)d, (unsigned long long)n);
        }
        return;
    }
#endif
    if (!walker_program.empty()) {
        RunSrtProgram(walker_program.data(), user_data, flat_dst);
    }
}

} // namespace Shader
