// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/types.h"
#include "bbport_guest_hooks.h"
#include "bbport_settings.h"
#include "bbport_guest_code.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"
#include "bbport_fx_init_clear.h"
#include "bbport_effect_flags.h"
#include "platform/gnm/gnm.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include "bbport_guest_code_hooks.h"
#include <cmath>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include <utility>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);
extern "C" void runtime_re_watch(uint64_t address);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 ImageBase = 0x800000000ull;

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the GPU side is told before (its write traps lift: no fault
/// per page) and after the copy (the data now there), as for file reads.
void* PS4_SYSV_ABI LoaderCopy(void* dst, const void* src, std::size_t size) {
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

/// bbport: native detours (default; BB_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 offset;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 65536;

/// A page within +-2 GiB of the image (below its base: the image grows upwards).
u8* MapStubPage() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = ImageBase - k * (16ull << 20);
        void* p = mmap(reinterpret_cast<void*>(hint), StubPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            return static_cast<u8*>(p);
        }
    }
    return nullptr;
}

using BbGuestCode::Emitter;

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page || e.overflow || stub_used + e.size > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `address` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 address, u8 opcode, std::uint32_t rel) {
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + 5) - reinterpret_cast<u64>(page);
    mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    auto* p = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, opcode, __ATOMIC_SEQ_CST);
    mprotect(page, span, Core::GuestCodeProtection());
}

void PS4_SYSV_ABI AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        on_range_allocated(start, end - start);
    }
}

void PS4_SYSV_ABI HeapHook(u64 address, u64 size) {
    if (address != 0 && size != 0 && size < (u64(1) << 32)) {
        on_range_allocated(address, size);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.offset;
    Emitter e;
    u64 back = 0;
    if (site.offset == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* code = reinterpret_cast<const u8*>(address);
        if (code[3] != 0x48 || code[4] != 0x85 || code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(stub_page + stub_used) + e.size;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 offset) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + offset;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
/// Game effects switched while the game runs (no restart). The community patches only set them
/// at start; here the same switches are written live:
///  - Ambient occlusion (SSAO), motion blur, the game's own anti-aliasing and the shadows of
///    dynamic lights are bytes of the renderer's options (+0x2968, +0x2969, +0x296a, +0x296e;
///    the patches change the immediates that set them in the constructor at 0x22c2542, 0x22c2532).
///    The renderer tests them while it builds each frame. The constructor's tail (0x22c254c)
///    reports the options' address; written there and on every change.
///  - Chromatic aberration (0x229faa8) and depth of field (0x21d7bbc) are code the patches
///    replace: written back and forth here; the game's own bytes are kept as they were found.
/// BB_HOT_EFFECTS=0: the patches at start as before (patches.py).
/// The options objects built so far (the game builds about ten at a load, one per render context).
constexpr std::size_t MaxEffectObjects = 64;
std::array<std::atomic<u64>, MaxEffectObjects> effects_objects{};
std::atomic<std::size_t> effects_object_count{0};
std::atomic<int> effects_applied[BbSettings::EffectCount];
struct HotCode {
    int effect;              ///< BbSettings::Effects index
    u64 offset;
    std::array<u8, 12> patch; /// the community patch's bytes
    std::size_t size;
    bool patch_when_on = false; /// the patch enables the effect (SSR), else disables it
    bool data = false;          /// a constant in the game's data, not code
    std::array<u8, 12> original{};
    bool ready = false;
};
/// SSR requires the complete startup patch, including its resource dimensions.
/// Partial live writes leave a half-size target behind full-size shader accesses.
std::array<HotCode, 3> hot_code = {{
    {0, 0x229faa8, {0xc7, 0x83, 0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90}, 12},
    {1, 0x21d7bbc, {0xe9, 0xdf, 0x00, 0x00, 0x00, 0x90}, 6},
    {7, 0x21d72f0, {0x31, 0xf6, 0x0f, 0x1f, 0x40, 0x00}, 6}, // "Disable Bloom"
}};
struct HotFlag {
    int effect;
    std::uint32_t offset;
};
constexpr std::array<HotFlag, 4> hot_flags = {{{2, 0x2969}, {3, 0x2968}, {4, 0x296a}, {5, 0x296e}}};

/// Writes `size` bytes of code from the last to the first (the first byte, an opcode, is the
/// one that makes the new code reachable).
void WriteHotCode(u64 address, const u8* bytes, std::size_t size, bool data) {
    if (data) {
        // A constant: its page keeps the protection it has (read-only data) after the write.
        int prot = PROT_READ;
        if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), maps)) {
                unsigned long long lo = 0, hi = 0;
                char perms[8] = {};
                if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 && address >= lo && address < hi) {
                    prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                           (perms[2] == 'x' ? PROT_EXEC : 0);
                    break;
                }
            }
            std::fclose(maps);
        }
        const long page_size = sysconf(_SC_PAGESIZE);
        void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
        const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
        mprotect(page, span, prot | PROT_WRITE);
        std::memcpy(reinterpret_cast<void*>(address), bytes, size);
        mprotect(page, span, prot);
        return;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
    mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    auto* p = reinterpret_cast<u8*>(address);
    for (std::size_t i = size; i-- > 1;) {
        __atomic_store_n(p + i, bytes[i], __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, bytes[0], __ATOMIC_SEQ_CST);
    mprotect(page, span, Core::GuestCodeProtection());
}

// (counted below)
extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

void ApplyHotEffects(bool force, u64 fresh = 0) {
    auto& settings = BbSettings::Get();
    for (auto& code : hot_code) {
        if (!code.ready) {
            continue;
        }
        const int want = settings.effects[code.effect].load(std::memory_order_relaxed) ? 1 : 0;
        if (!force && effects_applied[code.effect].load() == want) {
            continue;
        }
        effects_applied[code.effect].store(want);
        const bool use_patch = (want != 0) == code.patch_when_on;
        WriteHotCode(ImageBase + code.offset, use_patch ? code.patch.data() : code.original.data(), code.size,
                     code.data);
    }
    const std::size_t count = std::min(effects_object_count.load(std::memory_order_acquire), MaxEffectObjects);
    std::size_t live_objects = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const u64 object = effects_objects[i].load(std::memory_order_relaxed);
        const bool is_fresh = object == fresh; // just built: certainly an options object
        // An object the game has freed since may hold anything: it is written only while its
        // switches still look like switches (every byte 0 or 1, +0x2968..+0x2970).
        // Still the options object: its switches look like switches, and the block of constants the
        // constructor copies from the game's data (32 bytes at +0x2980) is still whole. The game frees
        // these objects; the memory is reused, and a block of zeros has passed for switches: a write
        // there damaged the game's heap (Havok's job queue stopped, a black screen).
        u8 block[0x40]{};
        // process_vm_readv fails on the game's memory (its pages are protected for the kernel's copy);
        // the runtime's own map says whether the range is still mapped.
        if (!runtime_memory_is_mapped(object + 0x2960, sizeof(block))) {
            continue; // not mapped any more
        }
        std::memcpy(block, reinterpret_cast<const void*>(object + 0x2960), sizeof(block));
        bool plausible = std::memcmp(block + 0x20, reinterpret_cast<const void*>(ImageBase + 0x5128a40), 32) == 0;
        for (int b = 8; b <= 0x10; ++b) {
            plausible &= block[b] <= 1;
        }
        if (!plausible && !is_fresh) {
            continue;
        }
        ++live_objects;
        for (const auto& flag : hot_flags) {
            int want = settings.effects[flag.effect].load(std::memory_order_relaxed) ? 1 : 0;
            if (flag.effect == 3) {
                // Menus and auxiliary contexts can deliberately have no SSAO target.
                // Preserve that restriction instead of overriding the game's safety flag.
                want = BbEffectFlags::SsaoEnabled(
                    {reinterpret_cast<const u8*>(object), 0x29a0}, want != 0);
            }
            auto* byte = reinterpret_cast<volatile u8*>(object + flag.offset);
            if (force || *byte != want) {
                __atomic_store_n(const_cast<u8*>(byte), u8(want), __ATOMIC_RELAXED);
            }
        }
    }
    static std::atomic<int> told{0};
    if (force && told.fetch_add(1) < 8) {
        std::printf("Guest hooks: game effects written to %zu of %zu options objects (the rest are gone)\n",
                    live_objects, count);
    }
}

/// The renderer options object is built (rbx at the constructor's tail): its switches follow the
/// settings from the start.
void PS4_SYSV_ABI EffectsConstructed(u64 object) {
    // Built again at the same place: not counted twice.
    const std::size_t known = std::min(effects_object_count.load(std::memory_order_acquire), MaxEffectObjects);
    bool seen = false;
    for (std::size_t i = 0; i < known; ++i) {
        seen |= effects_objects[i].load(std::memory_order_relaxed) == object;
    }
    if (!seen) {
        const std::size_t at = effects_object_count.fetch_add(1);
        if (at < MaxEffectObjects) {
            effects_objects[at].store(object, std::memory_order_release);
        } else {
            effects_object_count.store(MaxEffectObjects);
        }
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 3) {
            std::printf("Guest hooks: renderer options at %#llx (the game builds several; game effects "
                        "follow the settings in each)\n", static_cast<unsigned long long>(object));
        }
    }
    ApplyHotEffects(true, object);
}

bool InstallHotEffects() {
    if (const char* env = std::getenv("BB_HOT_EFFECTS"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0x22c254c;
    static constexpr std::array<u8, 11> tail = {0x48, 0xc7, 0x83, 0x77, 0x29, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), tail.data(), tail.size())) {
        return false; // another game version, or the patches changed it
    }
    // The code effects: the game's own bytes, taken before anything is written.
    for (auto& code : hot_code) {
        const u8* at = reinterpret_cast<const u8*>(ImageBase + code.offset);
        if (std::memcmp(at, code.patch.data(), code.size) == 0) {
            continue; // the startup patch is in (or the bytes are the same): left as it is
        }
        std::memcpy(code.original.data(), at, code.size);
        code.ready = true;
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf});                           // mov rdi, rbx
    e.CallAbs(reinterpret_cast<const void*>(&EffectsConstructed));
    e.RestoreVolatile();
    for (const u8 b : tail) {
        e.Bytes({b});                                      // the instruction the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});         // jmp [rip]
    e.U64(address + tail.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    for (auto& code : hot_code) {
        if (code.ready) {
            effects_applied[code.effect].store(-1);
        }
    }
    ApplyHotEffects(true); // the code effects before the game runs
    return true;
}

/// The render job pool (found in a hang of Laurence's cinematic, 2026-10-16). Six slots, a queue at
/// 0x1056920 (take a free slot, sleeping until one is free) and 0x1056990 (give it back); the one
/// caller of both is the job runner's step at 0x1067f60: take a slot, check the job's resource
/// (0x2165150), run it, give the slot back. Its two error exits (the resource is not in the state
/// asked for; the run fails) leave through 0x1068092 returning 1 with the slot still held in
/// [runner + 0x120], and the next call takes another one: each failed job loses a slot for good.
/// After six, every thread that needs a slot sleeps for ever (the main thread and Havok's five
/// workers polled the same empty pool in the dump; six runners held its six slots). The exit now
/// gives the held slot back before it returns. BB_JOB_SLOT_FIX=0: as the game has it.
std::atomic<u64> job_slots_released{0};

u64 JobProbeWord(u64 address) {
    u64 value = 0;
    if (address && runtime_memory_is_mapped(address, sizeof(value))) {
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    } else if (address) {
        // Stack locals and image vtables are outside the direct-memory map. Read them safely
        // through the kernel; returning zero for those hid the failed request's size/function.
        iovec local{&value, sizeof(value)}, remote{reinterpret_cast<void*>(address), sizeof(value)};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(sizeof(value))) {
            value = 0;
        }
    }
    return value;
}

void JobProbeObject(const char* name, u64 address) {
    std::printf("Job allocation: %s=%#llx", name, static_cast<unsigned long long>(address));
    if (address) {
        for (u64 off = 0; off < 0xe0; off += 8) {
            std::printf(" +%02llx=%#llx", static_cast<unsigned long long>(off),
                        static_cast<unsigned long long>(JobProbeWord(address + off)));
        }
    }
    std::printf("\n");
}

void JobProbeRangeHeap(u64 heap) {
    if (!heap || !runtime_memory_is_mapped(heap, 0xe0)) return;
    const u64 sentinel = heap + 0x68;
    u64 node = JobProbeWord(heap + 0x88), free_bytes = 0, largest = 0;
    unsigned blocks = 0;
    for (; node && node != sentinel && blocks < 8192; ++blocks) {
        if (!runtime_memory_is_mapped(node, 0x30)) return;
        const u64 begin = JobProbeWord(node + 8);
        const u64 end = JobProbeWord(JobProbeWord(node + 0x18) + 8);
        if (end < begin || end - begin > (1ull << 36)) return;
        free_bytes += end - begin;
        largest = std::max(largest, end - begin);
        node = JobProbeWord(node + 0x20);
    }
    if (node == sentinel) {
        std::printf("Job allocation: range heap=%#llx live=%llu reported_free=%llu free=%llu "
                    "largest=%llu free_blocks=%u\n", static_cast<unsigned long long>(heap),
                    static_cast<unsigned long long>(JobProbeWord(heap + 0xc8)),
                    static_cast<unsigned long long>(JobProbeWord(heap + 0xd0)),
                    static_cast<unsigned long long>(free_bytes),
                    static_cast<unsigned long long>(largest), blocks);
    }
}

// Error 9 in 0x2169c30 comes from allocation, not its preceding state check (error 8).
// Observe only failed retries/growth, preserving every guest register and flag.
void PS4_SYSV_ABI JobAllocationFailure(u64 tag, const Core::GuestRegisterSnapshot* r) {
    if (tag == 0 ? r->rcx != 0 : r->r14 != 0) return;
    static std::atomic<u64> failures{0};
    const u64 n = failures.fetch_add(1) + 1;
    if (n > 3 && (n & (n - 1)) != 0) return;
    const u64 allocator = tag == 0 ? r->r12 : JobProbeWord(r->rbx);
    const u64 bytes = tag == 0 ? (JobProbeWord(r->rbp - 0x54) & 0xffffffff)
                              : ((tag == 1 ? r->r15 : JobProbeWord(r->rbx + 0x18) & 0xffffffff) * 24 + 8);
    std::printf("Job allocation: %llu failures, %s bytes=%llu allocator=%#llx "
                "allocate_fn=+%#llx\n", static_cast<unsigned long long>(n),
                tag == 0 ? "resource buffer" : "tracking block",
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(allocator),
                static_cast<unsigned long long>(JobProbeWord(JobProbeWord(allocator) +
                                                             (tag == 0 ? 0x18 : 0x58)) - ImageBase));
    JobProbeObject("allocator", allocator);
    JobProbeObject("allocator+8 target", JobProbeWord(allocator + 8));
    JobProbeRangeHeap(JobProbeWord(allocator + 8));
    JobProbeObject("tracking", r->rbx);
}

bool InstallJobAllocationProbe() {
    constexpr std::array<u8, 8> buffer_fail{0xb8, 9, 0, 0, 0, 0x48, 0x85, 0xc9};
    constexpr std::array<u8, 8> tracking_fail{0xb8, 9, 0, 0, 0, 0x4d, 0x85, 0xf6};
    bool ok = Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + 0x26d0dac),
                                         buffer_fail, 0, JobAllocationFailure);
    ok &= Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + 0x26d0e82),
                                     tracking_fail, 1, JobAllocationFailure);
    ok &= Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + 0x26d0ebb),
                                     tracking_fail, 2, JobAllocationFailure);
    return ok;
}

// The resource cache used by 0x26d0d30 has its own backing allocation, independent of the
// graphics-heap budget patches. At high frame rates its 160 MiB range heap fills before the
// cinematic's geometry is prepared; failed 20-40 KiB allocations then leave missing SRTs.
// 320 MiB still failed on 4 MiB requests in the same load; 512 MiB survived that peak and
// returned to a few hundred live ranges with most of the cache free after streaming finished.
// Taken from the game's own allocator (2026-10-17) the larger backing starved the memory areas
// load into: after a trip to the Hunter's Dream and back, map pieces (walls, the city below,
// distant buildings) were missing. Now the game allocates its 160 MiB as always and the cache
// gets the port's direct memory past the game's size instead (runtime_extra_direct_map,
// BB_DMEM_EXTRA_MB): right after the allocation, its result and the size the range heap is
// set up with are replaced. BB_RENDER_RESOURCE_CACHE_MB=160: the game's cache as it is.
extern "C" int runtime_extra_direct_map(uint64_t size, uint64_t alignment, void** out);
void PS4_SYSV_ABI ResourceCacheBacking(u64, const Core::GuestRegisterSnapshot* r) {
    constexpr u64 native_bytes = 160ull << 20;
    if ((JobProbeWord(r->r14 + 0x54) & 0xffffffffu) != native_bytes || !r->rax) return;
    static const std::uint32_t bytes = [] {
        const char* env = std::getenv("BB_RENDER_RESOURCE_CACHE_MB");
        const long mb = env ? std::strtol(env, nullptr, 10) : 512;
        return std::uint32_t(std::clamp(mb, 160l, 512l)) << 20;
    }();
    if (bytes == native_bytes) return;
    void* block = nullptr;
    if (const int error = runtime_extra_direct_map(bytes, 64ull << 10, &block); error || !block) {
        std::printf("Render resource cache: no port memory for %u MiB (error %#x): the game's "
                    "160 MiB\n", bytes >> 20, unsigned(error));
        return;
    }
    // The creation record can be a stack object: not necessarily in the runtime's map.
    std::memcpy(reinterpret_cast<void*>(r->r14 + 0x54), &bytes, sizeof(bytes));
    const_cast<Core::GuestRegisterSnapshot*>(r)->rax = reinterpret_cast<u64>(block);
    std::printf("Render resource cache: %u MiB in the port's memory at %p (the game's 160 MiB "
                "stays allocated)\n", bytes >> 20, block);
}

bool InstallResourceCacheSize() {
    // mov rcx, rbx; mov [rcx + 0x10], rax: right after the allocator's Allocate(size, 16) returns.
    constexpr std::array<u8, 7> code{0x48, 0x89, 0xd9, 0x48, 0x89, 0x41, 0x10};
    return Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + 0x216b21d), code,
                                      0, ResourceCacheBacking);
}

void PS4_SYSV_ABI JobSlotFix(u64 runner, u64 queue, std::uint32_t result,
                            std::uint32_t error) {
    if (result != 1) {
        return;
    }
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 item = held;
    if (!item) {
        return;
    }
    using GiveBack = void(PS4_SYSV_ABI*)(u64 queue, u64 item);
    reinterpret_cast<GiveBack>(ImageBase + 0x1056990)(queue, item);
    held = 0;
    const u64 n = job_slots_released.fetch_add(1) + 1;
    if (n <= 5 || (n & (n - 1)) == 0) {
        const u64 job = *reinterpret_cast<const u64*>(runner + 0x118);
        const u64 resource = job ? *reinterpret_cast<const u64*>(job + 0x40) : 0;
        std::uint32_t state = 0;
        if (resource && runtime_memory_is_mapped(resource + 0x38, sizeof(state))) {
            std::memcpy(&state, reinterpret_cast<const void*>(resource + 0x38), sizeof(state));
        }
        std::printf("Guest hooks: a failed render job gave its slot back (%llu so far); "
                    "error=%u runner=%#llx resource=%#llx state[38..3b]=%08x\n",
                    static_cast<unsigned long long>(n), error,
                    static_cast<unsigned long long>(runner),
                    static_cast<unsigned long long>(resource), state);
    }
}

/// The runner is destroyed or reset (0x106798a, 0x1067a5c: `mov qword [rbx + 0x120], 0`) with a slot
/// still held (a job cancelled in the middle): the slot goes back first. The pool is the global the
/// game's own 0x1057350 returns.
void PS4_SYSV_ABI JobSlotDrop(u64 runner) {
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 item = held;
    if (!item) {
        return;
    }
    const u64 queue = *reinterpret_cast<const u64*>(ImageBase + 0x54b8788);
    if (!queue) {
        return;
    }
    using GiveBack = void(PS4_SYSV_ABI*)(u64 queue, u64 item);
    reinterpret_cast<GiveBack>(ImageBase + 0x1056990)(queue, item);
    held = 0;
    const u64 n = job_slots_released.fetch_add(1) + 1;
    if (n <= 5 || (n & (n - 1)) == 0) {
        std::printf("Guest hooks: a render job destroyed with its slot gave it back (%llu so far)\n",
                    static_cast<unsigned long long>(n));
    }
}

bool InstallJobSlotDropHook(u64 offset) {
    // mov qword [rbx + 0x120], 0
    static constexpr std::array<u8, 11> code = {0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf});                   // mov rdi, rbx (the runner)
    e.CallAbs(reinterpret_cast<const void*>(&JobSlotDrop));
    e.RestoreVolatile();
    for (const u8 b : code) {
        e.Bytes({b});
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

bool InstallJobSlotFix() {
    if (const char* env = std::getenv("BB_JOB_SLOT_FIX"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0x1068092;
    // mov rax, [rbx]; cmp rax, [rbp-0x30]: the stack check every exit of that function ends in.
    static constexpr std::array<u8, 7> code = {0x48, 0x8b, 0x03, 0x48, 0x3b, 0x45, 0xd0};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x4c, 0x89, 0xf7});                   // mov rdi, r14 (the runner)
    // SaveVolatile uses r12 for its stack anchor; the game's r12 (the pool) is
    // saved at [r12]. Passing the anchor itself silently loses the slot:
    // GiveBack finds no matching item, then JobSlotFix clears the held pointer.
    e.Bytes({0x49, 0x8b, 0x34, 0x24});             // mov rsi, [r12] (the saved pool)
    e.Bytes({0x44, 0x89, 0xfa});                   // mov edx, r15d (the result)
    e.Bytes({0x89, 0xc1});                         // mov ecx, eax (the original error)
    e.CallAbs(reinterpret_cast<const void*>(&JobSlotFix));
    e.RestoreVolatile();
    for (const u8 b : code) {
        e.Bytes({b});                              // the two instructions the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return InstallJobSlotDropHook(0x106798a) & InstallJobSlotDropHook(0x1067a5c) && true;
}

/// Yebis's blur-kernel sample parameters (GPUTexUtil_GetRecursiveSampleParameters, 0xbbc3e0): its
/// first argument, the base sample count, must be at least 3 (an assertion the retail build has
/// compiled out). Game data now and then gives 2; the function then runs on in a state that does
/// not end (the guest stops submitting: a black screen), so the count is raised to 3 at the entry.
/// BB_KERNEL_CLAMP=0: not installed.
bool InstallKernelSampleClamp() {
    if (const char* env = std::getenv("BB_KERNEL_CLAMP"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0xbbc3e0;
    static constexpr std::array<u8, 6> entry = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), entry.data(), entry.size())) {
        return false;
    }
    Emitter e;
    e.Bytes({0x83, 0xff, 0x03});                   // cmp edi, 3
    e.Bytes({0x7d, 0x05});                         // jge over
    e.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00});       // mov edi, 3
    for (const u8 b : entry) {
        e.Bytes({b});                              // the instructions the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + entry.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The free camera (the community patch "Restore Debug Camera", Lance McDonald's: hold Cross and
/// press L3). Its code (0x1de3329..) keeps the mode in the camera manager (r12): byte +0xc4 is 1
/// while the free camera is on (+0x150 of r13: the player is locked in the first mode, free in
/// the second). The probe copies r12 out at the check that runs every update of that code
/// (0x1de344c: cmp byte [r12 + 0xc4], 1); the mouse then drives the free camera's right stick
/// instead of the follow camera's own turn. Only installed where the patch is in place.
alignas(8) std::atomic<u64> free_camera_manager{0};

bool InstallFreeCameraProbe() {
    constexpr u64 offset = 0x1de344c;
    static constexpr std::array<u8, 9> code = {0x41, 0x80, 0xbc, 0x24, 0xc4, 0x00, 0x00, 0x00, 0x01};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false; // the patch is off (or other code there)
    }
    Emitter e;
    e.Bytes({0x9c, 0x50});                         // pushfq; push rax
    e.Bytes({0x48, 0xb8});                         // movabs rax, &free_camera_manager
    e.U64(reinterpret_cast<u64>(&free_camera_manager));
    e.Bytes({0x4c, 0x89, 0x20});                   // mov [rax], r12
    e.Bytes({0x58, 0x9d});                         // pop rax; popfq
    for (const u8 b : code) {
        e.Bytes({b});                              // the check itself
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// Aspect ratio on the fly. The camera manager (0x1436b..) copies the active camera's projection
/// parameters into the current camera every frame (+0x50 vertical field of view, +0x54 aspect
/// ratio, +0x58 near, +0x5c far: four plain copies, and a blend of two cameras, one of them per
/// update); the projection (0x1d16830), the culling and the HUD's markers read them there. After the
/// copy the output's aspect ratio replaces the game's 1.777 when one is set (SetDisplayAspect): a
/// wider output sees more to the sides at the same height; a taller one keeps the width and sees
/// more above and below (the field of view widened: 2 atan(tan(fov / 2) * 16:9 / aspect), x87).
/// Nothing of the game's own code is changed but the jump.
alignas(8) std::atomic<std::uint32_t> camera_aspect_bits{0}; ///< float bits; 0: the game's
alignas(8) std::atomic<u64> camera_state{0};                   ///< the current camera (rbx there)
struct alignas(8) FovScale {
    std::atomic<std::uint32_t> bits{0}; ///< float: the tangent's factor; 0: the game's field of view
    float half = 0.5f;
};
FovScale camera_fov_scale;
struct ParamStore {
    u64 offset;
    std::array<u8, 5> code; ///< the store the jump covers (no RIP-relative operand)
};
constexpr std::array<ParamStore, 5> CameraParamStores = {{
    {0x1436c2c, {0xc5, 0xfa, 0x11, 0x43, 0x5c}}, // vmovss %xmm0, 0x5c(%rbx): the follow camera
    {0x1436d8a, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436e68, {0xc5, 0xf8, 0x11, 0x4b, 0x50}}, // vmovups %xmm1, 0x50(%rbx): a blend of two
    {0x1436eec, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436f4b, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
}};

int InstallCameraAspectHooks() {
    int installed = 0;
    for (const auto& site : CameraParamStores) {
        const u64 address = ImageBase + site.offset;
        if (std::memcmp(reinterpret_cast<const void*>(address), site.code.data(), site.code.size())) {
            continue;
        }
        Emitter e;
        for (const u8 b : site.code) {
            e.Bytes({b});
        }
        e.Bytes({0x9c, 0x50});                         // pushfq; push rax
        e.Bytes({0x48, 0xb8});                         // movabs rax, &camera_state
        e.U64(reinterpret_cast<u64>(&camera_state));
        e.Bytes({0x48, 0x89, 0x18});                   // mov [rax], rbx
        e.Bytes({0x48, 0xb8});                         // movabs rax, &camera_aspect_bits
        e.U64(reinterpret_cast<u64>(&camera_aspect_bits));
        e.Bytes({0x8b, 0x00});                         // mov eax, [rax]
        e.Bytes({0x85, 0xc0, 0x74, 0x03});             // test eax, eax; je over the store
        e.Bytes({0x89, 0x43, 0x54});                   // mov [rbx + 0x54], eax
        e.Bytes({0x48, 0xb8});                         // movabs rax, &camera_fov_scale
        e.U64(reinterpret_cast<u64>(&camera_fov_scale));
        e.Bytes({0x83, 0x38, 0x00, 0x74, 0x15});       // cmp dword [rax], 0; je over the x87 part
        e.Bytes({0xd9, 0x43, 0x50});                   // fld dword [rbx + 0x50]: fov
        e.Bytes({0xd8, 0x48, 0x04});                   // fmul dword [rax + 4]: fov / 2
        e.Bytes({0xd9, 0xf2, 0xdd, 0xd8});             // fptan; fstp st0 (its 1.0)
        e.Bytes({0xd8, 0x08});                         // fmul dword [rax]: tan * factor
        e.Bytes({0xd9, 0xe8, 0xd9, 0xf3});             // fld1; fpatan: atan(tan * factor)
        e.Bytes({0xd8, 0xc0});                         // fadd st0, st0
        e.Bytes({0xd9, 0x5b, 0x50});                   // fstp dword [rbx + 0x50]
        e.Bytes({0x58, 0x9d});                         // pop rax; popfq
        e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
        e.U64(address + site.code.size());
        const u64 stub = PlaceStub(e);
        std::uint32_t rel = 0;
        if (!stub || !Rel32To(address + 5, stub, rel)) {
            continue;
        }
        WriteJumpOverTrap(address, 0xe9, rel);
        ++installed;
    }
    return installed;
}

/// The HUD's markers over characters (lock-on, enemies' health bars, damage numbers): projected
/// with the current camera, then placed in the game's 1920x1080 UI by `* (0.5, -0.5, ..) +
/// (0.5, 0.5, ..)` (three places), and drawn with the UI. The port puts the UI in the middle of a
/// wider (taller) output at its height (width): there a marker's 0.5 becomes 0.5 * output aspect /
/// 16:9 (16:9 / aspect for y), so it lands where its character is. The detour multiplies by the
/// port's vector instead of the game's constant; a marker past the UI's edges is hidden by the
/// game itself, as off screen.
struct MarkerSite {
    u64 offset;
    u64 scale, bias; ///< the constants the two instructions read
};
constexpr std::array<MarkerSite, 3> MarkerSites = {{
    {0x1644379, 0x48f9a00, 0x48f9a10},
    {0x1644c45, 0x48f9a60, 0x48f9a70},
    {0x16452b7, 0x48f9ad0, 0x48f9ae0},
}};
struct alignas(16) MarkerScale {
    float game[4];                     ///< the game's constant
    std::atomic<std::uint32_t> now[4]; ///< what the detour multiplies by
};
std::array<MarkerScale, MarkerSites.size()> marker_scales{};
std::atomic<int> marker_sites{0};
std::atomic<float> marker_kx{1.0f}, marker_ky{1.0f}; ///< SetDisplayAspect's, kept for a later install

void UpdateMarkerScales() {
    const float kx = marker_kx.load(std::memory_order_relaxed), ky = marker_ky.load(std::memory_order_relaxed);
    for (MarkerScale& m : marker_scales) {
        const float now[4] = {m.game[0] * kx, m.game[1] * ky, m.game[2], m.game[3]};
        for (int c = 0; c < 4; ++c) {
            std::uint32_t v;
            std::memcpy(&v, &now[c], 4);
            m.now[c].store(v, std::memory_order_relaxed);
        }
    }
}

/// `vmulps disp(%rip), %xmm0, %xmm0; vaddps disp(%rip), %xmm0, %xmm0` reading `scale`, `bias`.
bool MarkerCodeAt(u64 offset, u64 scale, u64 bias) {
    const auto* p = reinterpret_cast<const u8*>(ImageBase + offset);
    const auto reads = [&](int at, u8 opcode, u64 target) {
        std::int32_t disp;
        std::memcpy(&disp, p + at + 4, 4);
        return p[at] == 0xc5 && p[at + 1] == 0xf8 && p[at + 2] == opcode && p[at + 3] == 0x05 &&
               offset + at + 8 + disp == target;
    };
    return reads(0, 0x59, scale) && reads(8, 0x58, bias);
}

int InstallMarkerHooks() {
    int installed = 0;
    for (std::size_t i = 0; i < MarkerSites.size(); ++i) {
        const MarkerSite& site = MarkerSites[i];
        if (!MarkerCodeAt(site.offset, site.scale, site.bias)) {
            continue;
        }
        MarkerScale& m = marker_scales[i];
        std::memcpy(m.game, reinterpret_cast<const void*>(ImageBase + site.scale), sizeof(m.game));
        UpdateMarkerScales();
        const u64 address = ImageBase + site.offset;
        Emitter e;
        e.Bytes({0x50});                               // push rax
        e.Bytes({0x48, 0xb8});                         // movabs rax, &now
        e.U64(reinterpret_cast<u64>(&m.now));
        e.Bytes({0xc5, 0xf8, 0x59, 0x00});             // vmulps xmm0, xmm0, [rax]
        e.Bytes({0x48, 0xb8});                         // movabs rax, the game's bias
        e.U64(ImageBase + site.bias);
        e.Bytes({0xc5, 0xf8, 0x58, 0x00});             // vaddps xmm0, xmm0, [rax]
        e.Bytes({0x58});                               // pop rax
        e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
        e.U64(address + 16);
        const u64 stub = PlaceStub(e);
        std::uint32_t rel = 0;
        if (!stub || !Rel32To(address + 5, stub, rel)) {
            continue;
        }
        // The rest of the two instructions: traps (the jump returns past them).
        const long page_size = sysconf(_SC_PAGESIZE);
        void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
        mprotect(page, (address + 16) - reinterpret_cast<u64>(page), PROT_READ | PROT_WRITE | PROT_EXEC);
        auto* code = reinterpret_cast<u8*>(address);
        code[0] = 0xcc;
        std::memset(code + 5, 0xcc, 11);
        mprotect(page, (address + 16) - reinterpret_cast<u64>(page), Core::GuestCodeProtection());
        WriteJumpOverTrap(address, 0xe9, rel);
        ++installed;
    }
    marker_sites.store(installed, std::memory_order_relaxed);
    return installed;
}

/// The HUD anchored to the output's edges. The game lays its HUD (menu/fe.gfx, Scaleform) out for
/// 1920x1080: on a wider output the port draws that screen in the middle, and its edges would stay
/// there. The HUD's groups are moved instead, as the game moves its own clips: through the
/// movie's members (GFx::Value, ObjectInterface::SetMember "x"/"y") from the thread the HUD lives
/// on. The bars, items and gauges (PlayerHUD) go to the left edge, blood echoes and insight
/// (PlayerHUD.Soul, .HeroPoint) and the autosave icon to the right; on a taller output the top
/// ones go up and the boss bar down. BB_HUD_ANCHOR=0 keeps them in the 16:9 middle.
/// GFx::Value as this build has it (the game's own copies: 0x1adb830, its path lookup 0x1f68aa5).
/// A managed value is linked into its ObjectInterface's list (AddRef 0x2f15b0, Release 0x2f1640).
struct GfxValue {
    u64 links[2] = {~u64(0), ~u64(0)};
    void* object_interface = nullptr;
    std::uint32_t type = 0, pad = 0;
    union {
        double number;
        void* data;
    } value{};
    u64 aux = 0;
};
static_assert(sizeof(GfxValue) == 0x30);
constexpr std::uint32_t GfxManaged = 0x40, GfxNumber = 5, GfxDisplayObject = 10;
/// ObjectInterface's methods in Scaleform 4's order: AddRef, Release, HasMember, GetMember
/// (the slot the game's path lookup calls), SetMember.
constexpr std::size_t SlotAddRef = 0x10, SlotRelease = 0x18, SlotGetMember = 0x28, SlotSetMember = 0x30;
using GfxRefFn = void (PS4_SYSV_ABI*)(void* oi, GfxValue* value, void* data);
using GfxGetFn = bool (PS4_SYSV_ABI*)(void* oi, void* data, const char* name, GfxValue* out, bool isdobj);
using GfxSetFn = bool (PS4_SYSV_ABI*)(void* oi, void* data, const char* name, const GfxValue* value, bool isdobj);
template <typename F>
F GfxMethod(void* oi, std::size_t slot) {
    return *reinterpret_cast<F*>(*reinterpret_cast<u8**>(oi) + slot);
}
bool GfxIsObject(const GfxValue& v) {
    return v.object_interface && (v.type & 0x8c) == 8;
}
void GfxRelease(GfxValue& v) {
    if (v.object_interface && (v.type & GfxManaged)) {
        GfxMethod<GfxRefFn>(v.object_interface, SlotRelease)(v.object_interface, &v, v.value.data);
    }
    v = GfxValue{};
}
/// `object`'s member `name` into `out` (released by the caller), false if none.
bool GfxGet(const GfxValue& object, const char* name, GfxValue& out) {
    out = GfxValue{};
    if (!GfxIsObject(object)) return false;
    const bool isdobj = (object.type & 0x8f) == GfxDisplayObject;
    return GfxMethod<GfxGetFn>(object.object_interface, SlotGetMember)(object.object_interface,
                                                                       object.value.data, name, &out, isdobj);
}
bool GfxGetNumber(const GfxValue& object, const char* name, double& number) {
    GfxValue v;
    const bool ok = GfxGet(object, name, v) && (v.type & 0x8f) == GfxNumber;
    if (ok) number = v.value.number;
    GfxRelease(v);
    return ok;
}
bool GfxSetNumber(const GfxValue& object, const char* name, double number) {
    if (!GfxIsObject(object)) return false;
    GfxValue v;
    v.type = GfxNumber;
    v.value.number = number;
    const bool isdobj = (object.type & 0x8f) == GfxDisplayObject;
    return GfxMethod<GfxSetFn>(object.object_interface, SlotSetMember)(object.object_interface,
                                                                       object.value.data, name, &v, isdobj);
}

/// The game's in-game screen (vtable 0x539c988) updates once a frame (0x1bf9e10: rdi the screen,
/// xmm0 the frame time) and makes its HUD there on first use: the HUD (a value wrapper, its
/// movie's root GFx::Value at +0x30) at screen + 0xa80. The groups are moved from that update,
/// through values got and released within it: nothing of a movie is kept past its frame (the HUD
/// movie is unloaded with areas, and its ObjectInterface with it).
constexpr u64 HudFrameSite = 0x1bf9e10;
constexpr unsigned char HudFrameCode[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15
constexpr u64 ScreenHudOffset = 0xa80;
constexpr u64 HudWrapperVtable = 0x539a070; ///< the value wrapper's (0x539a060 + 0x10)
std::atomic<float> hud_dx{0.0f}, hud_dy{0.0f}; ///< the shift for an edge, in the HUD's pixels
std::atomic<bool> hud_anchor_on{true};
/// A HUD group: its parent (0 root, 1 PlayerHUD), name and how it follows the edges (x by dx,
/// y by dy), and where the game put it.
struct HudItem {
    int parent;
    const char* name;
    float fx, fy;
    double base_x = 0, base_y = 0;
    double set_x = 0, set_y = 0; ///< what the port set last
    bool known = false;
};
std::array<HudItem, 5> hud_items = {{
    {0, "PlayerHUD", -1.0f, -1.0f},
    {1, "Soul", 2.0f, 0.0f}, // in PlayerHUD: twice, for its move to the left
    {1, "HeroPoint", 2.0f, 0.0f},
    {0, "AutoSaveIcon", 1.0f, -1.0f},
    {0, "BossTop", 0.0f, 1.0f},
}};

/// Each frame, from the game's screen update: the HUD groups where the output's edges are.
void PS4_SYSV_ABI HudFrame(u64 screen) {
    const u64 hud = screen ? *reinterpret_cast<const u64*>(screen + ScreenHudOffset) : 0;
    if (!hud) {
        return;
    }
    if (*reinterpret_cast<const u64*>(hud) != ImageBase + HudWrapperVtable) {
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("HUD: unexpected object at the screen's HUD: not anchored\n");
        }
        return;
    }
    const GfxValue& root = *reinterpret_cast<const GfxValue*>(hud + 0x30);
    static u64 last_hud = 0;
    static unsigned frames = 0;
    static float applied_dx = 0.0f, applied_dy = 0.0f;
    if (hud != last_hud) {
        last_hud = hud; // a new HUD (another area): where the game put its groups is read again
        for (HudItem& item : hud_items) item.known = false;
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("HUD: anchored to the output's edges%s\n", hud_anchor_on ? "" : " off (BB_HUD_ANCHOR=0)");
        }
    }
    const float dx = hud_anchor_on ? hud_dx.load(std::memory_order_relaxed) : 0.0f;
    const float dy = hud_anchor_on ? hud_dy.load(std::memory_order_relaxed) : 0.0f;
    bool all_known = true;
    for (const HudItem& item : hud_items) all_known = all_known && item.known;
    // Rechecked every half second: the game may put a group back itself.
    if (dx == applied_dx && dy == applied_dy && all_known && ++frames % 32 != 0) {
        return;
    }
    if (!GfxIsObject(root)) {
        return;
    }
    GfxValue player;
    GfxGet(root, "PlayerHUD", player);
    for (HudItem& item : hud_items) {
        GfxValue clip;
        if (!GfxGet(item.parent ? player : root, item.name, clip) || !GfxIsObject(clip)) {
            GfxRelease(clip);
            continue;
        }
        double x = 0, y = 0;
        if (GfxGetNumber(clip, "x", x) && GfxGetNumber(clip, "y", y)) {
            // Positions are in twips (1/20 pixel): a read back is that close.
            const auto moved = [](double a, double b) { return std::abs(a - b) > 0.06; };
            if (!item.known || moved(x, item.set_x) || moved(y, item.set_y)) {
                item.base_x = x; // first seen, or moved by the game since
                item.base_y = y;
                item.known = true;
            }
            const double nx = item.base_x + item.fx * dx, ny = item.base_y + item.fy * dy;
            if (moved(nx, x)) GfxSetNumber(clip, "x", nx);
            if (moved(ny, y)) GfxSetNumber(clip, "y", ny);
            double check_x = nx, check_y = ny;
            GfxGetNumber(clip, "x", check_x);
            GfxGetNumber(clip, "y", check_y);
            item.set_x = check_x;
            item.set_y = check_y;
            if (moved(check_x, nx) || moved(check_y, ny)) {
                static bool told = false;
                if (!told) {
                    told = true;
                    std::printf("HUD: %s did not take its place (%.1f, %.1f instead of %.1f, %.1f)\n",
                                item.name, check_x, check_y, nx, ny);
                }
            }
        }
        GfxRelease(clip);
    }
    GfxRelease(player);
    applied_dx = dx;
    applied_dy = dy;
}

/// The game's UI (Scaleform movies) for the output's aspect ratio. The menu system sets each movie's
/// viewport every frame (0x1f59100 -> MovieImpl::SetViewport 0x1f510, a GFx::Viewport on its
/// stack: buffer 1920x1080, the movie's view mode ShowAll, centred). Given the output's pixel
/// aspect ratio (Viewport::AspectRatio: (output width / height) / 16:9), a movie shows its 16:9
/// screen in the middle of a wider (taller) view, squeezed into the 1920x1080 frame, which the
/// output stretches back (UiComposition::Scale): what lies past the game's screen (the HUD's
/// groups at the edges) is drawn, not culled. BB_RE_VIEWPORT=1 logs the calls.
constexpr u64 SetViewportSite = 0x1f510;
std::atomic<float> gfx_pixel_aspect{1.0f};
void PS4_SYSV_ABI ViewportAdjust(u64 movie, u64 viewport, u64 caller) {
    auto* v = reinterpret_cast<std::int32_t*>(viewport);
    static const bool trace = [] {
        const char* env = std::getenv("BB_RE_VIEWPORT");
        return env && env[0] == '1';
    }();
    if (trace) {
        static std::atomic<int> calls{0};
        float scale, aspect;
        std::memcpy(&scale, v + 11, 4);
        std::memcpy(&aspect, v + 12, 4);
        if (++calls <= 24) {
            std::printf("Viewport: movie %#llx from %#llx: buffer %dx%d at %d,%d %dx%d scissor %d,%d %dx%d "
                        "flags %#x scale %.3f aspect %.3f; mode %d align %d\n",
                        (unsigned long long)movie, (unsigned long long)(caller - ImageBase), v[0], v[1], v[2],
                        v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], double(scale), double(aspect),
                        *reinterpret_cast<const int*>(movie + 0xe8), *reinterpret_cast<const int*>(movie + 0xec));
        }
    }
    // The game's full-screen movies (1920x1080 in a 1920x1080 frame) only.
    if (v[0] == 1920 && v[1] == 1080 && v[2] == 0 && v[3] == 0 && v[4] == 1920 && v[5] == 1080) {
        const float aspect = gfx_pixel_aspect.load(std::memory_order_relaxed);
        std::memcpy(v + 12, &aspect, 4);
    }
}
/// The GX swap-chain's per-frame render command (#162): its 16-byte node comes from a per-frame
/// bump allocator that returns null once the frame's heap is used up; the game writes through it
/// unchecked (`mov [r12 + 8], r13` at 0x2199310, then posts it to the render thread): a guest
/// fault at address 8. Without a node the post is skipped and the pending flag (this + 0x3ab)
/// stays set, so the command goes out with the next frame.
constexpr u64 GxNodeSite = 0x2199310;
constexpr u64 GxNodeSkip = 0x219934e; // past the post and the flag's clear
constexpr unsigned char GxNodeCode[9] = {0x4d, 0x89, 0x6c, 0x24, 0x08,  // mov [r12 + 8], r13
                                         0x49, 0x8b, 0x87, 0xb0};        // mov rax, [r15 + 0xb0]
std::atomic<u64> gx_node_skip_trap{0}; ///< the stub's int3 for a null node (OnTrap)

bool InstallGxNodeGuard() {
    const u64 address = ImageBase + GxNodeSite;
    if (std::memcmp(reinterpret_cast<const void*>(address), GxNodeCode, sizeof(GxNodeCode))) {
        return false;
    }
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xe4});                   // test r12, r12
    e.Bytes({0x74, 0x13});                         // jz null (past the next 19 bytes)
    e.Bytes({0x4d, 0x89, 0x6c, 0x24, 0x08});       // mov [r12 + 8], r13
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + 5);
    const std::size_t trap_at = e.size;
    e.Bytes({0xcc});                               // null: OnTrap counts it and skips the post
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) return false;
    gx_node_skip_trap.store(stub + trap_at, std::memory_order_release);
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    mprotect(page, (address + 5) - reinterpret_cast<u64>(page), PROT_READ | PROT_WRITE | PROT_EXEC);
    reinterpret_cast<u8*>(address)[0] = 0xcc; // OnTrap runs the instruction meanwhile
    mprotect(page, (address + 5) - reinterpret_cast<u64>(page), Core::GuestCodeProtection());
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

bool InstallViewportHook() {
    const u64 address = ImageBase + SetViewportSite;
    if (std::memcmp(reinterpret_cast<const void*>(address), HudFrameCode, sizeof(HudFrameCode))) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.SaveXmm();
    e.Bytes({0x49, 0x8b, 0x54, 0x24, 0x50}); // mov rdx, [r12 + 80]: the return address
    e.CallAbs(reinterpret_cast<const void*>(&ViewportAdjust));
    e.RestoreXmm();
    e.RestoreVolatile();
    for (const unsigned char b : HudFrameCode) e.Bytes({b});
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
    e.U64(address + sizeof(HudFrameCode));
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    mprotect(page, (address + 6) - reinterpret_cast<u64>(page), PROT_READ | PROT_WRITE | PROT_EXEC);
    reinterpret_cast<u8*>(address)[0] = 0xcc;
    reinterpret_cast<u8*>(address)[5] = 0xcc;
    mprotect(page, (address + 6) - reinterpret_cast<u64>(page), Core::GuestCodeProtection());
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The detour at the screen update's entry: the registers (xmm0: its frame time) kept around
/// the call, then its first instructions and a jump back.
bool InstallHudHook() {
    const u64 address = ImageBase + HudFrameSite;
    if (std::memcmp(reinterpret_cast<const void*>(address), HudFrameCode, sizeof(HudFrameCode))) {
        return false;
    }
    Emitter e;
    e.SaveVolatile(); // rdi (the screen) is kept as it is
    e.SaveXmm();
    e.CallAbs(reinterpret_cast<const void*>(&HudFrame));
    e.RestoreXmm();
    e.RestoreVolatile();
    for (const unsigned char b : HudFrameCode) e.Bytes({b});
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + sizeof(HudFrameCode));
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    mprotect(page, (address + 6) - reinterpret_cast<u64>(page), PROT_READ | PROT_WRITE | PROT_EXEC);
    reinterpret_cast<u8*>(address)[0] = 0xcc;
    reinterpret_cast<u8*>(address)[5] = 0xcc;
    mprotect(page, (address + 6) - reinterpret_cast<u64>(page), Core::GuestCodeProtection());
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// Native mouse look. The follow camera's update (0x143ac60, rdi the camera) derives its angles
/// from where the camera is around the player each frame, pitch at +0x140 and yaw at +0x144; in
/// its normal mode 0x143cde0..0x143cdf0 makes xmm1 = camera stick * turn speed * frame time, the
/// increment 0x143cdf0 adds to (pitch, yaw) before the game wraps the yaw and clamps the pitch,
/// and the camera is then placed from them. The detour adds the mouse's turn since the last update
/// (radians, taken and cleared atomically) to that increment: the mouse turns the camera as far as
/// it moved, with none of the stick's dead zone, response curve, acceleration or top speed.
constexpr u64 LookSite = 0x143cde0;
constexpr unsigned char LookSiteCode[16] = {0xc5, 0xa8, 0x59, 0xc9,                         // vmulps xmm1, xmm10, xmm1
                                            0xc5, 0xf8, 0x28, 0x95, 0x10, 0xfc, 0xff, 0xff, // vmovaps xmm2, [rbp - 0x3f0]
                                            0xc5, 0xe8, 0x59, 0xc9};                        // vmulps xmm1, xmm2, xmm1
alignas(8) std::atomic<u64> look_delta{0}; ///< (pitch, yaw) as two floats, taken by the detour
std::atomic<bool> look_installed{false};
std::atomic<long long> look_since_ms{0}; ///< when look_delta last went from empty to not

bool InstallLookHook() {
    const u64 address = ImageBase + LookSite;
    if (std::memcmp(reinterpret_cast<const void*>(address), LookSiteCode, sizeof(LookSiteCode))) {
        return false;
    }
    Emitter e;
    for (const u8 b : LookSiteCode) {
        e.Bytes({b}); // the three instructions the jump covers (no RIP-relative operand)
    }
    e.Bytes({0x50, 0x51});                         // push rax; push rcx
    e.Bytes({0x48, 0xb8});                         // movabs rax, &look_delta
    e.U64(reinterpret_cast<u64>(&look_delta));
    e.Bytes({0x31, 0xc9});                         // xor ecx, ecx
    e.Bytes({0x48, 0x87, 0x08});                   // xchg [rax], rcx: taken and cleared
    // A turn counts as camera input, as the stick does (0x143b8cc): else the update pulls the
    // pitch back to its rest angle (+0x150) at once, and the rest angle follows the new pitch.
    e.Bytes({0x48, 0x85, 0xc9});                   // test rcx, rcx
    e.Bytes({0x74, 0x1d});                         // je past the three stores (29 bytes)
    e.Bytes({0xc7, 0x85, 0x3c, 0xfc, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00}); // mov dword [rbp - 0x3c4], 1
    e.Bytes({0x41, 0xc6, 0x85, 0x5c, 0x01, 0x00, 0x00, 0x01});             // mov byte [r13 + 0x15c], 1
    e.Bytes({0x41, 0xc7, 0x85, 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // mov dword [r13 + 0x154], 0
    e.Bytes({0xc4, 0xe1, 0xf9, 0x6e, 0xd1});       // vmovq xmm2, rcx (xmm2 is dead here)
    e.Bytes({0xc5, 0xf0, 0x58, 0xca});             // vaddps xmm1, xmm1, xmm2
    e.Bytes({0x59, 0x58});                         // pop rcx; pop rax
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + sizeof(LookSiteCode));
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// BB_RE_ANALOG=1 (reverse engineering): the pad device's analog getter (0x26917d0: rdi the
/// device, esi the axis, the value at [rdi + 0x1e4 + 4 * axis]) goes through AnalogProbe, which
/// counts who asks for the right stick (axes 2 and 3) by return address, every 5 s.
constexpr u64 AnalogGetter = 0x26917d0;
constexpr unsigned char AnalogGetterCode[6] = {0xc5, 0xf8, 0x57, 0xc0, 0x83, 0xfe};
struct ProbeSlot {
    std::atomic<u64> key{0}, count{0};
};
std::array<ProbeSlot, 256> probe_slots;
std::atomic<long long> probe_printed{0};

std::atomic<u64> probe_virtual[4]{}; // axis 2/3 at 0x2698deb: r13 (the virtual index), the array
std::atomic<u64> probe_calls{0};

float PS4_SYSV_ABI AnalogProbe(const u8* device, int axis, u64 ret, u64 caller_r13) {
    static std::atomic<u64> targets[2]{}; // bit set of the virtual indices (< 64 + 64) per axis
    static std::atomic<u64> targets_high[2]{};
    if (ret == ImageBase + 0x2698deb && (axis == 2 || axis == 3)) {
        const u64 index = caller_r13 & 0xffffffff;
        if (index < 64) {
            targets[axis - 2].fetch_or(u64(1) << index, std::memory_order_relaxed);
        } else if (index < 128) {
            targets_high[axis - 2].fetch_or(u64(1) << (index - 64), std::memory_order_relaxed);
        }
        probe_virtual[(axis - 2) * 2].store(caller_r13 & 0xffffffff, std::memory_order_relaxed);
        probe_virtual[(axis - 2) * 2 + 1].store(*reinterpret_cast<const u64*>(device + 0x100),
                                                std::memory_order_relaxed);
        // BB_RE_WATCH_VIRTUAL=1: the runtime's watchpoint on the virtual right stick X.
        static const bool watch_virtual = [] {
            const char* env = std::getenv("BB_RE_WATCH_VIRTUAL");
            return env && env[0] == '1';
        }();
        static const u64 watch_index = [] {
            const char* env = std::getenv("BB_RE_WATCH_INDEX");
            return env ? std::strtoull(env, nullptr, 0) : 62ull;
        }();
        if (watch_virtual && axis == 2 && (caller_r13 & 0xffffffff) == watch_index &&
            probe_calls.fetch_add(1) == 3000) {
            runtime_re_watch(*reinterpret_cast<const u64*>(device + 0x100) + 4 * (caller_r13 & 0xffffffff));
        }
    }
    const float value = unsigned(axis) <= 0x15 ? *reinterpret_cast<const float*>(device + 0x1e4 + 4 * axis)
                                                : 0.0f;
    if (axis == 2 || axis == 3) {
        const u64 key = (ret << 2) | u64(axis);
        for (std::size_t i = 0; i < probe_slots.size(); ++i) {
            auto& slot = probe_slots[((key * 0x9E3779B97F4A7C15ull >> 56) + i) & 255];
            u64 expected = 0;
            if (slot.key.load(std::memory_order_relaxed) == key ||
                slot.key.compare_exchange_strong(expected, key)) {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    const long long now = time(nullptr);
    long long printed = probe_printed.load(std::memory_order_relaxed);
    if (now - printed >= 5 && probe_printed.compare_exchange_strong(printed, now)) {
        std::printf("RE analog: right stick readers (return - image, axis: count):");
        for (auto& slot : probe_slots) {
            if (const u64 key = slot.key.load(); key != 0) {
                std::printf(" %#llx/%llu:%llu", (unsigned long long)((key >> 2) - ImageBase),
                            (unsigned long long)(key & 3),
                            (unsigned long long)slot.count.exchange(0));
            }
        }
        std::printf("; targets RX %#llx:%#llx RY %#llx:%#llx",
                    (unsigned long long)targets_high[0].load(), (unsigned long long)targets[0].load(),
                    (unsigned long long)targets_high[1].load(), (unsigned long long)targets[1].load());
        std::printf("; virtual: RX index %llu in %#llx, RY index %llu in %#llx\n",
                    (unsigned long long)probe_virtual[0].load(), (unsigned long long)probe_virtual[1].load(),
                    (unsigned long long)probe_virtual[2].load(), (unsigned long long)probe_virtual[3].load());
    }
    // BB_RE_ZERO_VIRTUAL=1: the virtual inputs fed from the right stick's X at 0x2698deb get 0.
    static const bool zero_virtual = [] {
        const char* env = std::getenv("BB_RE_ZERO_VIRTUAL");
        return env && env[0] == '1';
    }();
    if (zero_virtual && axis == 2 && ret == ImageBase + 0x2698deb) {
        return 0.0f;
    }
    return value;
}

/// The getter's first bytes become a jump to `mov rdx, [rsp]; mov rcx, r13; jmp AnalogProbe`
/// (a tail call).
bool InstallAnalogProbe() {
    const u64 address = ImageBase + AnalogGetter;
    if (std::memcmp(reinterpret_cast<const void*>(address), AnalogGetterCode, sizeof(AnalogGetterCode))) {
        return false;
    }
    Emitter e;
    e.Bytes({0x48, 0x8b, 0x14, 0x24});             // mov rdx, [rsp]
    e.Bytes({0x4c, 0x89, 0xe9});                   // mov rcx, r13 (the caller's)
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&AnalogProbe));
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// BB_RE_CAMSTICK=1 (reverse engineering): who calls the shared stick getter 0x1fdcc90 (vtable
/// slot 0x38 of the input manipulators: two dead zones from 0x49292f0, then 0xf92690), by return
/// address, every 5 s.
constexpr u64 StickThunk = 0x1fdcc90;
constexpr unsigned char StickThunkCode[4] = {0xc5, 0xfa, 0x10, 0x05};
std::array<ProbeSlot, 64> stick_slots;
std::atomic<long long> stick_printed{0};
thread_local u64 stick_ids = 0; // the call's action pair (esi | edx << 32)
std::atomic<u64> stick_seen[4]{}; // pair, last result, pair, last result (the first two pairs)
void PS4_SYSV_ABI StickCaller(u64 ret, u64 first, u64 second, u64 caller_r12) {
    stick_ids = (first & 0xffffffff) | (second << 32);
    // BB_RE_WATCH_CAMIN=1: the runtime's watchpoint on the camera input the manipulator keeps
    // (0x1526750 stores the camera stick at r12 + 0x40 after the call returning to 0x15283e4).
    static const bool watch_camera_input = [] {
        const char* env = std::getenv("BB_RE_WATCH_CAMIN");
        return env && env[0] == '1';
    }();
    static std::atomic<u64> camera_calls{0};
    if (watch_camera_input && ret == ImageBase + 0x15283e4 && camera_calls.fetch_add(1) == 1000) {
        runtime_re_watch(caller_r12 + 0x40);
    }
    for (std::size_t i = 0; i < stick_slots.size(); ++i) {
        auto& slot = stick_slots[((ret * 0x9E3779B97F4A7C15ull >> 58) + i) & 63];
        u64 expected = 0;
        if (slot.key.load(std::memory_order_relaxed) == ret || slot.key.compare_exchange_strong(expected, ret)) {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    const long long now = time(nullptr);
    long long printed = stick_printed.load(std::memory_order_relaxed);
    if (now - printed >= 5 && stick_printed.compare_exchange_strong(printed, now)) {
        for (int i = 0; i < 4; i += 2) {
            const u64 ids = stick_seen[i].load();
            const u64 v = stick_seen[i + 1].load();
            float x, y;
            const std::uint32_t lo = std::uint32_t(v), hi = std::uint32_t(v >> 32);
            std::memcpy(&x, &lo, 4);
            std::memcpy(&y, &hi, 4);
            std::printf("RE stick (%#llx, %#llx): %.3f %.3f\n", (unsigned long long)(ids & 0xffffffff),
                        (unsigned long long)(ids >> 32), x, y);
        }
        std::printf("RE stick getter callers (return - image: count):");
        for (auto& slot : stick_slots) {
            if (const u64 key = slot.key.load(); key != 0) {
                std::printf(" %#llx:%llu", (unsigned long long)(key - ImageBase),
                            (unsigned long long)slot.count.exchange(0));
            }
        }
        std::printf("\n");
    }
}

/// The getter's result (x, y in rax): noted per action pair, returned (to be changed later).
u64 PS4_SYSV_ABI StickResult(u64 result) {
    const u64 ids = stick_ids;
    for (int i = 0; i < 4; i += 2) {
        u64 expected = 0;
        if (stick_seen[i].load() == ids || stick_seen[i].compare_exchange_strong(expected, ids)) {
            if (result != 0 || stick_seen[i + 1].load() == 0) {
                stick_seen[i + 1].store(result);
            }
            break;
        }
    }
    return result;
}

bool InstallStickProbe() {
    const u64 address = ImageBase + StickThunk;
    if (std::memcmp(reinterpret_cast<const void*>(address), StickThunkCode, sizeof(StickThunkCode))) {
        return false;
    }
    std::uint32_t inner, outer;
    std::memcpy(&inner, reinterpret_cast<const void*>(ImageBase + 0x49292f0), 4);
    std::memcpy(&outer, reinterpret_cast<const void*>(ImageBase + 0x49292f4), 4);
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x49, 0x8b, 0x7c, 0x24, 0x50}); // mov rdi, [r12 + 0x50]: the return address (rsi, rdx: the pair)
    e.Bytes({0x49, 0x8b, 0x0c, 0x24});       // mov rcx, [r12]: the caller's r12
    e.CallAbs(reinterpret_cast<const void*>(&StickCaller));
    e.RestoreVolatile();
    e.Bytes({0xb8}); e.U32(inner);              // mov eax, inner
    e.Bytes({0xc5, 0xf9, 0x6e, 0xc0});          // vmovd xmm0, eax
    e.Bytes({0xb8}); e.U32(outer);              // mov eax, outer
    e.Bytes({0xc5, 0xf9, 0x6e, 0xc8});          // vmovd xmm1, eax
    e.Bytes({0x48, 0x83, 0xec, 0x08});          // sub rsp, 8 (the callee's alignment)
    e.CallAbs(reinterpret_cast<const void*>(ImageBase + 0xf92690));
    e.Bytes({0x48, 0x83, 0xc4, 0x08});          // add rsp, 8
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xc7});                // mov rdi, rax
    e.CallAbs(reinterpret_cast<const void*>(&StickResult));
    e.Bytes({0x49, 0x89, 0x44, 0x24, 0x48});    // mov [r12 + 0x48], rax: the saved rax
    e.RestoreVolatile();
    e.Bytes({0xc3});                            // ret
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The follow camera's update (0x143ac60: rdi the camera; its angles at +0x140 and +0x144, from
/// the camera stick's x and y, in radians within [-pi, pi]). BB_RE_CAMERA=1 hooks its entry:
/// BB_RE_WATCH_CAMANGLE=1 watches +0x140 (who writes it), BB_RE_CAM_ADD=<radians> turns the camera
/// by that much each update (does writing the angle turn it?).
constexpr u64 CameraUpdate = 0x143ac60;
constexpr unsigned char CameraUpdateCode[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
void PS4_SYSV_ABI CameraEntry(u8* camera) {
    static const bool watch_angle = [] {
        const char* env = std::getenv("BB_RE_WATCH_CAMANGLE");
        return env && env[0] == '1';
    }();
    static const float add = [] {
        const char* env = std::getenv("BB_RE_CAM_ADD");
        return env ? std::strtof(env, nullptr) : 0.0f;
    }();
    static std::atomic<u64> calls{0};
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed);
    if (watch_angle && n == 1000) {
        runtime_re_watch(reinterpret_cast<u64>(camera) + 0x140);
    }
    if (add != 0.0f) {
        float& yaw = *reinterpret_cast<float*>(camera + 0x140);
        yaw += add;
        if (yaw > 3.14159265f) {
            yaw -= 6.28318531f;
        }
    }
    if (n % 2000 == 0) {
        std::printf("RE camera %p: angles %.3f %.3f\n", static_cast<void*>(camera),
                    *reinterpret_cast<float*>(camera + 0x140), *reinterpret_cast<float*>(camera + 0x144));
    }
}

bool InstallCameraProbe() {
    const u64 address = ImageBase + CameraUpdate;
    if (std::memcmp(reinterpret_cast<const void*>(address), CameraUpdateCode, sizeof(CameraUpdateCode))) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.CallAbs(reinterpret_cast<const void*>(&CameraEntry)); // rdi: the camera, unchanged
    e.RestoreVolatile();
    e.Bytes({0x55, 0x48, 0x89, 0xe5, 0x41, 0x57});         // the instructions the jump covers
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});         // jmp [rip]
    e.U64(address + sizeof(CameraUpdateCode));
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// BB_RE_ENTRY=0x20f00,0x12ab4c0,... (reverse engineering): functions' entries (`push rbp; mov rbp,
/// rsp` then `push r1x` or `push rbx`) counted, with the float argument xmm0 (min, max, mean) and
/// rsi, every 2 s: how often the game calls them and with which frame time.
/// BB_RE_ADVANCE=1: the same for Scaleform's MovieImpl::Advance (0x20f00, xmm0: the time).
struct EntrySlot {
    u64 offset = 0;
    std::atomic<u64> calls{0};
    std::atomic<std::uint32_t> min_bits{0x7f800000}, max_bits{0};
    std::atomic<u64> last_rsi{0};
    double sum = 0;
};
std::array<EntrySlot, 128> entry_slots;
std::size_t entry_count = 0;
std::atomic<long long> entry_printed{0};

template <int I>
void PS4_SYSV_ABI EntryProbe(u64, u64 rsi, float x) {
    EntrySlot& slot = entry_slots[I];
    slot.calls.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(x);
    if (x < std::bit_cast<float>(slot.min_bits.load())) slot.min_bits.store(bits);
    if (x > std::bit_cast<float>(slot.max_bits.load()) || slot.max_bits.load() == 0) slot.max_bits.store(bits);
    slot.sum += x;
    slot.last_rsi.store(rsi, std::memory_order_relaxed);
    std::atomic<long long>& printed = entry_printed;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = printed.load();
    if (now - last < 2000 || !printed.compare_exchange_strong(last, now)) return;
    for (std::size_t k = 0; k < entry_count; ++k) {
        EntrySlot& e = entry_slots[k];
        const u64 n = e.calls.exchange(0);
        if (n == 0) continue;
        std::printf("RE entry %#llx: %llu calls in 2 s, xmm0 %.5f..%.5f mean %.5f, rsi %#llx\n",
                    (unsigned long long)e.offset, (unsigned long long)n,
                    n ? std::bit_cast<float>(e.min_bits.load()) : 0.0f, std::bit_cast<float>(e.max_bits.load()),
                    n ? e.sum / double(n) : 0.0, (unsigned long long)e.last_rsi.load());
        e.sum = 0;
        e.min_bits.store(0x7f800000);
        e.max_bits.store(0);
    }
}
template <std::size_t... I>
constexpr std::array<void (PS4_SYSV_ABI*)(u64, u64, float), sizeof...(I)> EntryProbes(std::index_sequence<I...>) {
    return {&EntryProbe<int(I)>...};
}
constexpr auto entry_probes = EntryProbes(std::make_index_sequence<128>{});

bool InstallEntryProbe(u64 offset) {
    if (entry_count == entry_slots.size()) return false;
    const u64 address = ImageBase + offset;
    const u8* code = reinterpret_cast<const u8*>(address);
    // push rbp; mov rbp, rsp; then push r12..r15 (2 bytes) or push rbx (1 byte)
    if (code[0] != 0x55 || code[1] != 0x48 || code[2] != 0x89 || code[3] != 0xe5) return false;
    std::size_t length = 0;
    if (code[4] == 0x41 && code[5] >= 0x54 && code[5] <= 0x57) length = 6;
    else if (code[4] == 0x53) length = 5;
    else return false;
    Emitter e;
    e.SaveVolatile();
    e.SaveXmm(); // xmm0 reaches the probe as its float argument
    e.CallAbs(reinterpret_cast<const void*>(entry_probes[entry_count]));
    e.RestoreXmm();
    e.RestoreVolatile();
    for (std::size_t i = 0; i < length; ++i) e.Bytes({code[i]});
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + length);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) return false;
    entry_slots[entry_count++].offset = offset;
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    mprotect(page, (address + length) - reinterpret_cast<u64>(page), PROT_READ | PROT_WRITE | PROT_EXEC);
    reinterpret_cast<u8*>(address)[0] = 0xcc;
    for (std::size_t i = 5; i < length; ++i) reinterpret_cast<u8*>(address)[i] = 0xcc;
    mprotect(page, (address + length) - reinterpret_cast<u64>(page), Core::GuestCodeProtection());
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

struct sigaction previous_action {};

void OnTrap(int sig, siginfo_t* info, void* context) {
    auto* g = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(g[REG_RIP]) - 1; // past the int3
    // The GX node guard (#162): its site while the detour goes in, and its null case.
    const bool gx_site = rip == ImageBase + GxNodeSite;
    if (gx_site || (rip != 0 && rip == gx_node_skip_trap.load(std::memory_order_acquire))) {
        if (gx_site && g[REG_R12] != 0) {
            *reinterpret_cast<u64*>(u64(g[REG_R12]) + 8) = u64(g[REG_R13]);
            g[REG_RIP] = greg_t(ImageBase + GxNodeSite + 5);
            return;
        }
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            static const char text[] = "Guest hooks: the GX per-frame command heap was full: a render "
                                       "command waits for the next frame (#162)\n";
            [[maybe_unused]] const auto written = write(STDOUT_FILENO, text, sizeof(text) - 1);
        }
        g[REG_RIP] = greg_t(ImageBase + GxNodeSkip);
        return;
    }
    for (const auto& site : native_sites) {
        if (rip == ImageBase + site.offset) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                g[REG_RIP] = greg_t(stub); // the detour is going in: its stub runs the site
                return;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(g[REG_RAX] & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        g[REG_RIP] = greg_t(ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2));
        return;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = u64(g[REG_RDI]);
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        g[REG_RSP] -= 8; // push rbp
        *reinterpret_cast<u64*>(g[REG_RSP]) = u64(g[REG_RBP]);
        g[REG_RIP] = greg_t(ImageBase + CollectorEntry + 1);
        return;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = u64(g[REG_RAX]);
        const u64 size = *reinterpret_cast<const u64*>(u64(g[REG_RBP]) - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        g[REG_RBX] = g[REG_RAX];
        g[REG_RIP] = greg_t(ImageBase + site + sizeof(HeapAllocReturnCode));
        return;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = u64(g[REG_RAX])) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        g[REG_RSP] += 0x18;
        g[REG_RIP] = greg_t(ImageBase + AllocReturn + sizeof(AllocReturnCode));
        return;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        g[REG_RSP] -= 8;
        *reinterpret_cast<u64*>(g[REG_RSP]) = ret;
        g[REG_RIP] = greg_t(reinterpret_cast<u64>(&LoaderCopy));
        return;
    }
    if (previous_action.sa_flags & SA_SIGINFO) {
        if (previous_action.sa_sigaction) {
            previous_action.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous_action.sa_handler != SIG_DFL && previous_action.sa_handler != SIG_IGN &&
               previous_action.sa_handler) {
        previous_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    iovec local{bytes, size}, remote{reinterpret_cast<void*>(address), size};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(size) ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    mprotect(page, span, Core::GuestCodeProtection());
    return true;
}

void PS4_SYSV_ABI FxInitClear(void* context, void* state, void* texture, const float* color) {
    if (gettid() == getpid() && texture && color &&
        runtime_memory_is_mapped(reinterpret_cast<u64>(texture) + 0x10, 11 * sizeof(u32))) {
        std::array<u32, 11> target;
        std::memcpy(target.data(), static_cast<const u8*>(texture) + 0x10, sizeof(target));
        const auto clear = BbFxInit::Plan(target, std::span{color, 4});
        if (clear && runtime_memory_is_mapped(clear->address, clear->bytes) &&
            BbGnm::SubmitFxInitZero(clear->address, clear->bytes)) {
            static u64 count = 0; // main thread only
            if (++count <= 3 || (count & (count - 1)) == 0)
                std::printf("FX initialization: %llu targets cleared through private commands "
                            "(%#llx +%u bytes)\n", (unsigned long long)count,
                            (unsigned long long)clear->address, clear->bytes);
            return;
        }
    }
    using NativeClear = void(PS4_SYSV_ABI *)(void*, void*, void*, const float*);
    reinterpret_cast<NativeClear>(ImageBase + 0x2168630)(context, state, texture, color);
}

bool InstallFxInitClear() {
    if (const char* env = std::getenv("BB_FX_INIT_CLEAR"); env && env[0] == '0') return false;
    // Only the three initialization calls in 0xebb960; regular clears retain the game's path.
    constexpr std::array<CallSite, 3> sites{{
        {0xebbc86, {0xe8, 0xa5, 0xc9, 0x2a, 0x01}},
        {0xebbca4, {0xe8, 0x87, 0xc9, 0x2a, 0x01}},
        {0xebbcc2, {0xe8, 0x69, 0xc9, 0x2a, 0x01}},
    }};
    for (const auto& site : sites)
        if (std::memcmp(reinterpret_cast<const void*>(ImageBase + site.offset), site.code, 5))
            return false;
    Emitter e;
    e.JmpAbs(reinterpret_cast<u64>(&FxInitClear));
    const u64 stub = PlaceStub(e);
    if (!stub) return false;
    for (const auto& site : sites) {
        std::uint32_t rel = 0;
        if (!Rel32To(ImageBase + site.offset + 5, stub, rel)) return false;
        WriteJumpOverTrap(ImageBase + site.offset, 0xe8, rel);
    }
    return true;
}
} // namespace

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
    struct sigaction action {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_action);
    // Only the PC memory model needs notifications about GPU allocations and resource copies.
    // The remaining hooks (including the loading/cutscene fixes) are shared by both models.
    const bool memory_hooks =
        on_gpu_range_allocated != nullptr && !std::getenv("BB_EXP_NO_MEMORY_HOOKS");
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += memory_hooks && Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator =
        memory_hooks && Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = memory_hooks && (!heap_env || heap_env[0] != '0');
    if (memory_hooks && BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    if (memory_hooks) {
        std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                    patched, MemcpySites.size());
        std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                    HeapAllocReturns.size());
        std::printf("Guest hooks: GPU memory allocator %s\n",
                    allocator ? "tells the GPU side of new ranges" : "not found");
    } else {
        std::printf("Guest hooks: none for the GPU memory (this memory model keeps copies)\n");
    }
    // Native detours over the traps just placed (BB_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* traps_env = std::getenv("BB_GUEST_HOOK_TRAPS");
    if (!(traps_env && traps_env[0] == '1') && (stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallCallDetour(site.offset);
            }
        }
        for (auto& site : native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        const int aspect_sites = InstallCameraAspectHooks();
        std::printf("Guest hooks: render job slot leak fix %s\n",
                    InstallJobSlotFix() ? "on" : "not installed");
        std::printf("Guest hooks: job allocation failure probe %s\n",
                    InstallJobAllocationProbe() ? "on" : "not installed");
        std::printf("Guest hooks: render resource cache size fix %s\n",
                    InstallResourceCacheSize() ? "on" : "not installed");
        std::printf("Guest hooks: FX initialization context race fix %s\n",
                    InstallFxInitClear() ? "on" : "off (BB_FX_INIT_CLEAR=0 or unmatched code)");
        std::printf("Guest hooks: blur-kernel sample count clamp %s\n",
                    InstallKernelSampleClamp() ? "on" : "not installed");
        std::printf("Guest hooks: game effects switched live %s\n",
                    InstallHotEffects() ? "on (no restart)" : "off (startup patches)");
        if (InstallFreeCameraProbe()) {
            std::printf("Guest hooks: free camera probe on (the mouse drives it)\n");
        }
        const int marker_places = InstallMarkerHooks();
        if (const char* env = std::getenv("BB_HUD_ANCHOR"); env && env[0] == '0') hud_anchor_on = false;
        const bool hud_hooks = InstallViewportHook() && InstallHudHook();
        std::printf("Guest hooks: GX command heap guard %s\n",
                    InstallGxNodeGuard() ? "in (#162)" : "not installed (other code there)");
        std::printf("Guest hooks: the output's aspect ratio on the fly: the camera at %d of %zu places, "
                    "the HUD's markers at %d of %zu, its groups %s\n", aspect_sites, CameraParamStores.size(),
                    marker_places, MarkerSites.size(), hud_hooks ? "at the edges" : "in the middle (no hook)");
        // BB_NATIVE_MOUSE=0: the mouse drives the right stick instead (runtime_pad.c).
        if (const char* env = std::getenv("BB_NATIVE_MOUSE"); !(env && env[0] == '0')) {
            look_installed.store(InstallLookHook(), std::memory_order_release);
            std::printf("Guest hooks: native mouse look %s\n",
                        look_installed.load() ? "on (the mouse turns the camera itself)"
                                              : "not available (the mouse drives the right stick)");
        }
        if (const char* env = std::getenv("BB_RE_ADVANCE"); env && env[0] == '1') {
            std::printf("Guest hooks: UI advance probe %s\n", InstallEntryProbe(0x20f00) ? "on" : "not installed");
        }
        if (const char* env = std::getenv("BB_RE_ENTRY"); env && env[0]) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 offset = std::strtoull(p, &end, 16);
                if (end == p) break;
                std::printf("Guest hooks: entry probe at %#llx %s\n", (unsigned long long)offset,
                            InstallEntryProbe(offset) ? "on" : "not installed (other code there)");
                p = *end ? end + 1 : end;
            }
        }
        if (const char* env = std::getenv("BB_RE_CAMERA"); env && env[0] == '1') {
            std::printf("Guest hooks: camera probe %s\n", InstallCameraProbe() ? "on" : "not installed");
        }
        if (const char* env = std::getenv("BB_RE_CAMSTICK"); env && env[0] == '1') {
            std::printf("Guest hooks: stick getter probe %s\n", InstallStickProbe() ? "on" : "not installed");
        }
        if (const char* env = std::getenv("BB_RE_ANALOG"); env && env[0] == '1') {
            std::printf("Guest hooks: analog getter probe %s\n",
                        InstallAnalogProbe() ? "on" : "not installed (other code there)");
        }
        mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
        std::printf("Guest hooks: %d native detours (no traps; BB_GUEST_HOOK_TRAPS=1: int3), %zu of %zu "
                    "stub bytes\n", detours, stub_used, StubPageSize);
    }
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
void ApplyGameEffects() {
    ApplyHotEffects(false);
}

bool FreeCameraActive() {
    const u64 manager = free_camera_manager.load(std::memory_order_relaxed);
    return manager != 0 && *reinterpret_cast<const volatile u8*>(manager + 0xc4) == 1;
}
std::uint64_t CameraState() {
    return camera_state.load(std::memory_order_relaxed);
}

void SetDisplayAspect(float aspect) {
    constexpr float game = 16.0f / 9.0f;
    const bool other = aspect > 0.5f && aspect < 8.0f && std::abs(aspect - game) > 0.001f;
    std::uint32_t bits = 0, fov = 0;
    float kx = 1.0f, ky = 1.0f; // the markers' scale: the UI is as high (wide) as the output
    if (other) {
        std::memcpy(&bits, &aspect, 4);
        if (aspect < game) {
            ky = game / aspect;
            std::memcpy(&fov, &ky, 4);
        } else {
            kx = aspect / game;
        }
    }
    camera_aspect_bits.store(bits, std::memory_order_relaxed);
    camera_fov_scale.bits.store(fov, std::memory_order_relaxed);
    // The HUD's groups: as far as the output's edge is from the 16:9 middle (the HUD's pixels).
    hud_dx.store(960.0f * (kx - 1.0f), std::memory_order_relaxed);
    hud_dy.store(540.0f * (ky - 1.0f), std::memory_order_relaxed);
    if (const char* env = std::getenv("BB_HUD_DX")) { // diagnostics: another shift
        hud_dx.store(std::strtof(env, nullptr), std::memory_order_relaxed);
    }
    gfx_pixel_aspect.store(other ? aspect / game : 1.0f, std::memory_order_relaxed);
    marker_kx.store(kx, std::memory_order_relaxed);
    marker_ky.store(ky, std::memory_order_relaxed);
    UpdateMarkerScales();
    std::printf("Display: aspect ratio %.3f (%s): camera %s, HUD markers at %d places\n", double(aspect),
                other ? (aspect > game ? "wider than 16:9" : "taller than 16:9") : "the game's",
                other ? (aspect > game ? "sees more to the sides" : "sees more above and below")
                      : "as the game has it",
                marker_sites.load(std::memory_order_relaxed));
}

bool CameraLookAvailable() {
    return look_installed.load(std::memory_order_acquire);
}

bool CameraLookAdd(float pitch, float yaw) {
    if (!CameraLookAvailable()) {
        return false;
    }
    // Whether the camera takes the turns: a turn it has not taken for 100 ms (lock-on, menus,
    // cutscenes: its normal update does not run) means not, until it takes one again. Meanwhile
    // the caller drives the stick (lock-on switches targets with the stick) and the turn waiting
    // here is replaced rather than applied all at once later.
    static std::atomic<bool> active{true}, stored{false};
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    u64 old = look_delta.load(std::memory_order_acquire);
    if (stored.load(std::memory_order_relaxed)) {
        if (old == 0) {
            active.store(true, std::memory_order_relaxed);
        } else if (now - look_since_ms.load(std::memory_order_relaxed) > 100) {
            active.store(false, std::memory_order_relaxed);
        }
    }
    const bool taking = active.load(std::memory_order_relaxed);
    if (pitch == 0.0f && yaw == 0.0f) {
        return taking;
    }
    for (;;) {
        float p = 0.0f, y = 0.0f;
        if (old != 0 && taking) {
            const std::uint32_t lo = std::uint32_t(old), hi = std::uint32_t(old >> 32);
            std::memcpy(&p, &lo, 4);
            std::memcpy(&y, &hi, 4);
        }
        p += pitch;
        y += yaw;
        std::uint32_t lo, hi;
        std::memcpy(&lo, &p, 4);
        std::memcpy(&hi, &y, 4);
        const u64 next = u64(lo) | (u64(hi) << 32);
        if (look_delta.compare_exchange_weak(old, next, std::memory_order_acq_rel)) {
            if (old == 0 || !taking) {
                look_since_ms.store(now, std::memory_order_relaxed);
            }
            stored.store(true, std::memory_order_relaxed);
            return taking;
        }
    }
}
} // namespace BbGuestHooks
