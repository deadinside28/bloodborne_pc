// SPDX-License-Identifier: GPL-3.0-or-later
// From bbhost's engine/event_flags, decomp/events/flag_store, engine/lua_events,
// engine/player_data, engine/world_chr, engine/params, engine/paramdef and gcn/container
// (github.com/droogie/bbhost, GPL-3.0-or-later), over bbport's flat image: the game's code
// runs on its own threads with the host's ABI, so a call into it is a plain call.
#include "plugins/engine.h"

#include "online/common.h"

#include <miniz.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

extern "C" int runtime_file_translate(const char* guest, char* out, size_t size);  // bb-probe

namespace engine {

namespace {

unsigned char* g_image = nullptr;
std::uint64_t g_size = 0;
bool g_image_phase = true;
bool g_109 = false;

// The first bytes of what this layer calls and replaces, as the 1.09 eboot has them.
struct Signature {
    std::uint64_t bn;
    std::uint8_t bytes[16];
};
constexpr Signature kSignatures[] = {
    {0x2434770, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec}},  // SprjFlipper update
    {0x17cfc00, {0x53, 0x49, 0x89, 0xd0, 0x8b, 0x4f, 0x1c, 0x31, 0xd2, 0x89, 0xf0, 0xf7, 0xf1, 0x41, 0x89, 0xc2}},  // IsEventFlag
    {0x17cfcc0, {0x41, 0x89, 0xd0, 0x8b, 0x4f, 0x1c, 0x31, 0xd2, 0x89, 0xf0, 0xf7, 0xf1, 0x41, 0x89, 0xc1, 0x44}},  // SetEventFlag
    {0x17cfd80, {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x41, 0x89, 0xd0, 0x44, 0x8b, 0x4f, 0x1c, 0x31, 0xd2, 0x89}},  // GetEventFlagValue
    {0x17d0060, {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x41, 0x89, 0xc8, 0x41, 0x89, 0xd1, 0x44, 0x8b, 0x57, 0x1c}},  // SetEventFlagValue
    {0x1739870, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec}},  // LuaEvent_DispatchByName
    {0x194b110, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec}},  // PreparePlayerWarp
};

}  // namespace

void set_image(unsigned char* image, std::uint64_t size) {
    g_image = image;
    g_size = size;
    g_109 = image != nullptr;
    for (const Signature& s : kSignatures) {
        const std::uint64_t p = at(s.bn, sizeof(s.bytes));
        if (!p || std::memcmp(reinterpret_cast<const void*>(p), s.bytes, sizeof(s.bytes)) != 0) {
            host_log("plugins: 0x%llx is not the 1.09 code: not the 1.09 eboot", static_cast<unsigned long long>(s.bn));
            g_109 = false;
            break;
        }
    }
}

std::uint64_t at(std::uint64_t bn, std::size_t n) {
    if (!g_image || bn < kBnSlide || bn - kBnSlide + n > g_size) return 0;
    return reinterpret_cast<std::uint64_t>(g_image) + (bn - kBnSlide);
}

bool is_109() { return g_109; }

void image_phase_end() { g_image_phase = false; }

bool code_write(std::uint64_t runtime, const void* bytes, std::size_t n) {
    if (g_image_phase) {
        std::memcpy(reinterpret_cast<void*>(runtime), bytes, n);
        return true;
    }
    const std::uint64_t page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    const std::uint64_t lo = runtime & ~(page - 1), hi = (runtime + n + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    std::memcpy(reinterpret_cast<void*>(runtime), bytes, n);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC) != 0) {
        host_log("plugins: cannot restore the protection of 0x%llx", static_cast<unsigned long long>(runtime));
        std::abort();
    }
    return true;
}

bool read(std::uint64_t runtime, void* out, std::size_t n) {
    return runtime && host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(runtime)), out, n);
}

// =====================================================================================
// Event flags. The store: the manager splits flag ids into blocks of `block size` (+0x1c)
// flags; a block lives in an ordered map at +0x38 (std::map nodes: left +0, right +0x10,
// is-nil +0x19, block number +0x20, storage kind +0x28, storage +0x30). Kind 2 stores a
// pointer; kind 1 an index into a pool (+0x28) of `stride` (+0x20) bytes each. A flag is a
// bit of its block, the first flag of each byte its most significant bit. A value
// (Get/SetEventFlagValue) is `count` consecutive flags with the last one as bit 0, running on
// into the next block's head when it reaches the end of its own.

namespace {

constexpr std::uint64_t kEventFlagManSlot = 0x593b100;
constexpr std::uint64_t kSetEventFlag = 0x17cfcc0;
constexpr std::uint64_t kSetEventFlagValue = 0x17d0060;
constexpr std::uint64_t kBlockSize = 0x1c, kPoolStride = 0x20, kPool = 0x28, kMap = 0x38;

inline std::uint8_t mask_of(std::uint32_t bit) { return static_cast<std::uint8_t>(0x80u >> (bit & 7)); }

struct Safe {
    template <typename T>
    bool operator()(std::uint64_t a, T* out) const {
        return read(a, out, sizeof(T));
    }
};
// The game's memory read as the game reads it, inside the game's own setter.
struct Direct {
    template <typename T>
    bool operator()(std::uint64_t a, T* out) const {
        *out = *reinterpret_cast<const T*>(static_cast<std::uintptr_t>(a));
        return true;
    }
};

template <class Load>
std::uint64_t block_data(std::uint64_t man, std::uint32_t block, const Load& load) {
    std::uint64_t head = 0, node = 0;
    if (!load(man + kMap, &head) || !load(head + 8, &node)) return 0;
    std::uint64_t found = head;
    for (int depth = 0; depth < 64; ++depth) {
        std::uint8_t nil = 1;
        std::uint32_t key = 0;
        if (!load(node + 0x19, &nil) || nil || !load(node + 0x20, &key)) break;
        if (key < block) {
            if (!load(node + 0x10, &node)) return 0;
        } else {
            found = node;
            if (!load(node, &node)) return 0;
        }
    }
    std::uint32_t key = 0, kind = 0;
    if (found == head || !load(found + 0x20, &key) || key > block || !load(found + 0x28, &kind)) return 0;
    if (kind == 2) {
        std::uint64_t data = 0;
        return load(found + 0x30, &data) ? data : 0;
    }
    if (kind == 1) {
        std::uint32_t stride = 0, index = 0;
        std::uint64_t pool = 0;
        if (!load(man + kPoolStride, &stride) || !load(found + 0x30, &index) || !load(man + kPool, &pool)) return 0;
        return pool + static_cast<std::uint32_t>(stride * index);
    }
    return 0;
}

template <class Load>
bool locate(std::uint64_t man, std::uint32_t id, const Load& load, std::uint64_t* byte, std::uint8_t* mask) {
    std::uint32_t size = 0;
    if (!load(man + kBlockSize, &size) || !size) return false;
    const std::uint32_t block = id / size, bit = id - block * size;
    const std::uint64_t data = block_data(man, block, load);
    if (!data) return false;
    *byte = data + (bit >> 3);
    *mask = mask_of(bit);
    return true;
}

// SetEventFlagValue's writes, in its order: fn(byte, mask, flag id, bit).
template <class Fn>
void walk_write(std::uint64_t man, std::uint32_t first, std::uint32_t count, std::uint32_t value, Fn&& fn) {
    const Direct load;
    std::uint32_t size = 0;
    if (!load(man + kBlockSize, &size) || !size) return;
    const std::uint32_t block = first / size, bit = first - block * size;
    if (bit + count < size) {
        const std::uint64_t data = block_data(man, block, load);
        if (!data || !count) return;
        for (std::uint32_t i = 0; i != count; ++i) {
            const std::uint32_t f = bit + count - 1 - i;
            fn(data + (f >> 3), mask_of(f), block * size + f, (value >> (i & 31)) & 1u);
        }
        return;
    }
    const std::uint64_t d0 = block_data(man, block, load), d1 = block_data(man, block + 1, load);
    const std::uint32_t n1 = size - bit;
    for (std::uint32_t i = 0; i != n1; ++i) {
        const std::uint32_t f = size - 1 - i;
        if (d0) fn(d0 + (f >> 3), mask_of(f), block * size + f, (value >> (i & 31)) & 1u);
    }
    if (n1 == count) return;
    const std::uint32_t rest = value >> (n1 & 31), n2 = count - n1;
    for (std::uint32_t i = 0; i != n2; ++i) {
        const std::uint32_t f = n2 - 1 - i;
        if (d1) fn(d1 + (f >> 3), mask_of(f), (block + 1) * size + f, (rest >> (i & 31)) & 1u);
    }
}

// The change ring: many writers (any game thread), the main thread reads. The title flips
// ~4,700 flags in one frame, so it holds a few such bursts between two frames.
struct Slot {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint32_t> id{0}, value{0};
};
constexpr std::uint32_t kRing = 1u << 16;
Slot g_ring[kRing];
std::atomic<std::uint32_t> g_head{0};
std::uint32_t g_tail = 0;
std::atomic<std::uint64_t> g_lost{0};
std::atomic<bool> g_watch{false};

// Leaf-safe: no lock, no allocation.
void changed(std::uint32_t id, bool value) {
    if (!g_watch.load(std::memory_order_relaxed)) return;
    const std::uint32_t pos = g_head.fetch_add(1, std::memory_order_relaxed);
    Slot& s = g_ring[pos % kRing];
    s.seq.store(0, std::memory_order_relaxed);
    s.id.store(id, std::memory_order_relaxed);
    s.value.store(value ? 1u : 0u, std::memory_order_relaxed);
    s.seq.store(pos + 1, std::memory_order_release);
}

inline void put(std::uint64_t a, std::uint8_t mask, std::uint32_t id, bool on) {
    auto* byte = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(a));
    const std::uint8_t was = *byte, now = on ? static_cast<std::uint8_t>(was | mask) : static_cast<std::uint8_t>(was & ~mask);
    *byte = now;
    if (now != was) changed(id, on);
}

// 0x17cfcc0 and 0x17d0060 in the game's place: what they do, and the change noted.
void ours_set_event_flag(std::uint64_t man, std::uint32_t id, std::uint32_t on) {
    std::uint64_t byte = 0;
    std::uint8_t mask = 0;
    if (locate(man, id, Direct{}, &byte, &mask)) put(byte, mask, id, on != 0);
}
void ours_set_event_flag_value(std::uint64_t man, std::uint32_t first, std::uint32_t count, std::uint32_t value) {
    walk_write(man, first, count, value,
               [](std::uint64_t a, std::uint8_t mask, std::uint32_t id, std::uint32_t on) { put(a, mask, id, on != 0); });
}

std::mutex g_watch_mu;
bool g_replaced = false;  // under g_watch_mu

bool replace_entry(std::uint64_t bn, void* fn) {
    const std::uint64_t p = at(bn, 14);
    if (!p) return false;
    std::uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
    const std::uint64_t dest = reinterpret_cast<std::uint64_t>(fn);
    std::memcpy(jmp + 6, &dest, 8);
    return code_write(p, jmp, sizeof(jmp));
}

std::uint64_t flag_manager() {
    std::uint64_t m = 0;
    return g_109 && rd(at(kEventFlagManSlot, 8), &m) ? m : 0;
}

}  // namespace

bool event_flag_get(std::uint32_t id, bool* value) {
    const std::uint64_t man = flag_manager();
    std::uint64_t byte = 0;
    std::uint8_t mask = 0, v = 0;
    if (!man || !locate(man, id, Safe{}, &byte, &mask) || !rd(byte, &v)) return false;
    if (value) *value = (v & mask) != 0;
    return true;
}

bool event_flag_set(std::uint32_t id, bool value) {
    const std::uint64_t man = flag_manager();
    std::uint64_t byte = 0;
    std::uint8_t mask = 0, was = 0;
    if (!man || !locate(man, id, Safe{}, &byte, &mask) || !rd(byte, &was)) return false;
    auto* p = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(byte));
    *p = value ? static_cast<std::uint8_t>(was | mask) : static_cast<std::uint8_t>(was & ~mask);
    if (((was & mask) != 0) != value) changed(id, value);
    return true;
}

bool event_flags_watch() {
    if (!g_109) return false;
    std::lock_guard<std::mutex> lk(g_watch_mu);
    if (!g_replaced) {
        if (!replace_entry(kSetEventFlag, reinterpret_cast<void*>(&ours_set_event_flag)) ||
            !replace_entry(kSetEventFlagValue, reinterpret_cast<void*>(&ours_set_event_flag_value))) {
            host_log("plugins: the event flag setters could not be replaced: no flag changes");
            return false;
        }
        g_replaced = true;
        host_log("plugins: event flag changes watched (the game's two setters are ours)");
    }
    g_watch.store(true, std::memory_order_relaxed);
    return true;
}

std::size_t event_flags_take(FlagChange* out, std::size_t n, std::uint64_t* lost) {
    const std::uint32_t head = g_head.load(std::memory_order_acquire);
    if (head - g_tail > kRing) {  // lapped: the oldest are gone
        g_lost.fetch_add(head - g_tail - kRing, std::memory_order_relaxed);
        g_tail = head - kRing;
    }
    std::size_t got = 0;
    while (g_tail != head && got < n) {
        Slot& s = g_ring[g_tail % kRing];
        const std::uint32_t seq = s.seq.load(std::memory_order_acquire);
        if (seq != g_tail + 1) {
            if (seq == 0 || static_cast<std::int32_t>(seq - (g_tail + 1)) < 0) break;  // not written yet
            g_lost.fetch_add(1, std::memory_order_relaxed);
            ++g_tail;
            continue;
        }
        const FlagChange c{s.id.load(std::memory_order_relaxed), s.value.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != g_tail + 1) {
            g_lost.fetch_add(1, std::memory_order_relaxed);
            ++g_tail;
            continue;
        }
        out[got++] = c;
        ++g_tail;
    }
    if (lost) *lost = g_lost.load(std::memory_order_relaxed);
    return got;
}

// =====================================================================================
// Named Lua events: LuaEvent_DispatchByName (0x1739870) on the context at
// *(SprjLuaEventMan (slot 0x593b0c8) + 8), as the scripts' own event raisers call it.

namespace {
constexpr std::uint64_t kLuaDispatchByName = 0x1739870;
constexpr std::uint64_t kSprjLuaEventMan = 0x593b0c8;
std::mutex g_lua_mu;
std::deque<std::string> g_lua_queue;  // under g_lua_mu
}  // namespace

bool lua_event_queue(const char* name) {
    if (!g_109 || !name || !name[0]) return false;
    std::lock_guard<std::mutex> lk(g_lua_mu);
    if (g_lua_queue.size() >= 64) return false;
    g_lua_queue.emplace_back(name);
    return true;
}

void lua_events_tick() {
    std::deque<std::string> now;
    {
        std::lock_guard<std::mutex> lk(g_lua_mu);
        if (g_lua_queue.empty()) return;
        now.swap(g_lua_queue);
    }
    const std::uint64_t man = rp(at(kSprjLuaEventMan, 8));
    const std::uint64_t ctx = man ? rp(man + 8) : 0;
    if (!ctx) {
        host_log("plugins: no Lua event manager yet; %zu event(s) dropped", now.size());
        return;
    }
    for (const std::string& name : now) {
        const auto r = hle_call_guest<std::int64_t>(reinterpret_cast<const void*>(at(kLuaDispatchByName)), ctx, name.c_str());
        host_log("plugins: Lua event %s raised -> %lld", name.c_str(), static_cast<long long>(r));
    }
}

// =====================================================================================
// The player's numbers: GameDataMan (slot 0x593b130), the local record at +8.

namespace {
constexpr std::uint64_t kGameDataMan = 0x593b130;
struct Stat {
    const char* name;
    bool manager;  // GameDataMan's own field, else the local player's record
    std::uint32_t offset;
    bool is_signed;
};
constexpr Stat kStats[] = {
    {"hp", false, 0x14, true},          {"max_hp", false, 0x18, true},     {"stamina", false, 0x30, true},
    {"max_stamina", false, 0x34, true}, {"vitality", false, 0x40, true},   {"endurance", false, 0x48, true},
    {"strength", false, 0x58, true},    {"skill", false, 0x60, true},      {"bloodtinge", false, 0x68, true},
    {"arcane", false, 0x70, true},      {"insight", false, 0x84, true},    {"level", false, 0x90, true},
    {"echoes", false, 0x94, true},      {"ng_cycle", true, 0x68, true},    {"coop_helps", true, 0x78, false},
    {"invader_kills", true, 0x7c, false}, {"true_deaths", true, 0x84, false}, {"deaths", true, 0x88, false},
    {"play_time_ms", true, 0x94, false},
};

std::uint64_t stat_base(const char* name, const Stat** out) {
    if (!g_109 || !name) return 0;
    for (const Stat& s : kStats) {
        if (std::strcmp(s.name, name) != 0) continue;
        const std::uint64_t man = rp(at(kGameDataMan, 8));
        const std::uint64_t rec = man ? rp(man + 8) : 0;
        *out = &s;
        return rec ? (s.manager ? man : rec) : 0;
    }
    return 0;
}
}  // namespace

bool player_stat_get(const char* name, std::int64_t* value) {
    const Stat* s = nullptr;
    const std::uint64_t base = stat_base(name, &s);
    std::uint32_t v = 0;
    if (!base || !rd(base + s->offset, &v)) return false;
    if (value) *value = s->is_signed ? static_cast<std::int64_t>(static_cast<std::int32_t>(v)) : static_cast<std::int64_t>(v);
    return true;
}

bool player_stat_set(const char* name, std::int64_t value) {
    const Stat* s = nullptr;
    const std::uint64_t base = stat_base(name, &s);
    if (!base) return false;
    const std::uint32_t v = static_cast<std::uint32_t>(value);
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(base + s->offset)), &v, 4);
    return true;
}

// =====================================================================================
// The world: WorldChrMan (slot 0x593e878) keeps the main player's ChrIns at +0x60; its
// module container is at +0x3b0, and module +0x68 holds the transform (yaw +0x1d4, position
// +0x1e0). The block the player is in is at ChrIns +0x3f8. The follow camera is at +0x60 of
// the owner the pointer at +0x2830 of the object in slot 0x593e860 names (position +0x40,
// focus +0xd0).

namespace {
constexpr std::uint64_t kWorldChrMan = 0x593e878;
constexpr std::uint64_t kCameraRoot = 0x593e860;
constexpr std::uint64_t kWorldRes = 0x593b148;
constexpr std::uint64_t kPreparePlayerWarp = 0x194b110;       // (WorldRes*, const int* block, const vec4* pos, const vec4* rot)
constexpr std::uint64_t kWarpNextStageBonfire = 0x172e050;    // (lua state, unused; return point)
constexpr std::uint64_t kSetReturnPointEntity = 0x196d110;    // ReturnPointParam row -> the return point
constexpr std::uint64_t kSetUseReturnPoint = 0x196d0e0;       // the next load places the player there

bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

std::atomic<bool> g_warp_enabled{false};
std::mutex g_warp_mu;
bool g_warp_pending = false;  // under g_warp_mu
float g_warp_xyz[3] = {}, g_warp_yaw = 0;
std::uint32_t g_warp_block = 0;
std::int32_t g_lamp_warp = 0;
}  // namespace

std::uint64_t world_chr_man() { return g_109 ? rp(at(kWorldChrMan, 8)) : 0; }

bool player_position(float xyz[3], float* yaw) {
    const std::uint64_t man = world_chr_man();
    const std::uint64_t chr = man ? rp(man + 0x60) : 0;
    const std::uint64_t modules = chr ? rp(chr + 0x3b0) : 0;
    const std::uint64_t transform = modules ? rp(modules + 0x68) : 0;
    float v[3] = {}, y = 0;
    if (!transform || !rd(transform + 0x1e0, &v) || !rd(transform + 0x1d4, &y) || !finite3(v)) return false;
    if (xyz) std::memcpy(xyz, v, sizeof(v));
    if (yaw) *yaw = y;
    return true;
}

bool camera(float pos[3], float focus[3]) {
    const std::uint64_t root = g_109 ? rp(at(kCameraRoot, 8)) : 0;
    const std::uint64_t owner = root ? rp(root + 0x2830) : 0;
    const std::uint64_t cam = owner ? rp(owner + 0x60) : 0;
    float p[3] = {}, f[3] = {};
    if (!cam || !rd(cam + 0x40, &p) || !rd(cam + 0xd0, &f) || !finite3(p) || !finite3(f)) return false;
    if (f[0] == 0 && f[1] == 0 && f[2] == 0) return false;  // between areas the camera is reset
    if (pos) std::memcpy(pos, p, sizeof(p));
    if (focus) std::memcpy(focus, f, sizeof(f));
    return true;
}

bool player_block(std::uint32_t* block) {
    const std::uint64_t man = world_chr_man();
    const std::uint64_t chr = man ? rp(man + 0x60) : 0;
    std::uint32_t b = 0;
    if (!chr || !rd(chr + 0x3f8, &b) || b == 0xffffffffu) return false;
    if (block) *block = b;
    return true;
}

void warp_allow() {
    if (!g_warp_enabled.exchange(true)) host_log("plugins: warps on for a gameplay plugin the player turned on");
}

bool player_warp_queue(std::uint32_t block, const float xyz[3], float yaw) {
    if (!g_109 || !g_warp_enabled || !xyz || !std::isfinite(yaw)) return false;
    const float v[3] = {xyz[0], xyz[1], xyz[2]};
    if (!finite3(v) || !player_block(nullptr)) return false;
    std::lock_guard<std::mutex> lk(g_warp_mu);
    g_warp_pending = true;
    std::memcpy(g_warp_xyz, v, sizeof(v));
    g_warp_yaw = yaw;
    g_warp_block = block;
    return true;
}

bool lamp_warp_queue(std::int32_t return_point) {
    if (!g_109 || !g_warp_enabled || return_point <= 0 || !player_block(nullptr)) return false;
    std::lock_guard<std::mutex> lk(g_warp_mu);
    g_lamp_warp = return_point;
    return true;
}

void world_tick() {
    std::int32_t lamp = 0;
    float xyz[3] = {}, yaw = 0;
    std::uint32_t want = 0;
    bool warp = false;
    {
        std::lock_guard<std::mutex> lk(g_warp_mu);
        lamp = g_lamp_warp;
        g_lamp_warp = 0;
        warp = g_warp_pending;
        g_warp_pending = false;
        std::memcpy(xyz, g_warp_xyz, sizeof(xyz));
        yaw = g_warp_yaw;
        want = g_warp_block;
    }
    if (lamp) {
        std::uint32_t from = 0;
        player_block(&from);
        hle_call_guest<std::int64_t>(reinterpret_cast<const void*>(at(kSetReturnPointEntity)), lamp);
        hle_call_guest<std::int64_t>(reinterpret_cast<const void*>(at(kSetUseReturnPoint)), 1);
        hle_call_guest<std::int64_t>(reinterpret_cast<const void*>(at(kWarpNextStageBonfire)), 0, lamp);
        host_log("plugins: lamp travel from block 0x%08x to return point %d", from, lamp);
    }
    if (!warp) return;
    // Block 0 is the player's own; another block must be resident (a neighbour the world streams in).
    std::uint32_t from = 0;
    const std::uint64_t res = rp(at(kWorldRes, 8));
    if (!res || !player_block(&from)) {
        host_log("plugins: warp dropped (no player or no WorldRes)");
        return;
    }
    const std::uint32_t block = want ? want : from;
    alignas(16) std::int32_t blk[4] = {static_cast<std::int32_t>(block), 0, 0, 0};
    alignas(16) float pos[4] = {xyz[0], xyz[1], xyz[2], 0.0f};
    alignas(16) float rot[4] = {0.0f, yaw, 0.0f, 0.0f};
    hle_call_guest<std::int64_t>(reinterpret_cast<const void*>(at(kPreparePlayerWarp)), res, blk, pos, rot);
    std::uint32_t now = 0;
    player_block(&now);
    host_log("plugins: warp from block 0x%08x into 0x%08x at %.3f %.3f %.3f yaw %.3f; now in 0x%08x", from, block, xyz[0], xyz[1],
             xyz[2], yaw, now);
}

// =====================================================================================
// Param definitions: dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx, one 64-bit PARAMDEF
// (format 201, 0xd0-byte fields) per param type, read through a DFLT DCX and a BND4.

namespace {

struct ParamField {
    std::string name, type;
    std::uint32_t offset = 0, bytes = 0, count = 1;
    std::uint8_t bit = 0, bits = 0;
};
struct ParamDefinition {
    std::string type;
    std::uint32_t row_bytes = 0;
    std::vector<ParamField> fields;
    const ParamField* field(const std::string& name) const {
        for (const ParamField& f : fields) {
            if (f.name == name) return &f;
        }
        return nullptr;
    }
};

std::uint32_t rd32be(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

std::size_t find_bytes(const std::vector<std::uint8_t>& b, const char* needle) {
    const std::size_t n = std::strlen(needle);
    for (std::size_t i = 0; i + n <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, needle, n) == 0) return i;
    }
    return static_cast<std::size_t>(-1);
}

std::vector<std::uint8_t> dcx_decompress(const std::vector<std::uint8_t>& data) {
    if (data.size() < 0x2c || std::memcmp(data.data(), "DCX\0", 4) != 0) return data;
    if (std::memcmp(data.data() + 0x28, "DFLT", 4) != 0) return {};
    const std::size_t dca = find_bytes(data, "DCA");
    if (dca == static_cast<std::size_t>(-1) || dca + 8 > data.size()) return {};
    const std::size_t comp = dca + rd32be(data.data() + dca + 4);
    const std::size_t dcs = find_bytes(data, "DCS");
    std::size_t size = dcs != static_cast<std::size_t>(-1) ? rd32be(data.data() + dcs + 4) : 0;
    if (!size) size = data.size() * 8;
    if (comp >= data.size()) return {};
    std::vector<std::uint8_t> out(size);
    mz_ulong len = size;
    if (mz_uncompress(out.data(), &len, data.data() + comp, static_cast<mz_ulong>(data.size() - comp)) != MZ_OK) return {};
    out.resize(len);
    return out;
}

template <typename T>
bool get(const std::vector<std::uint8_t>& b, std::size_t a, T* out) {
    if (a + sizeof(T) > b.size()) return false;
    std::memcpy(out, b.data() + a, sizeof(T));
    return true;
}

std::string fixed(const std::vector<std::uint8_t>& b, std::size_t a, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && a + i < b.size() && b[a + i]; ++i) s.push_back(static_cast<char>(b[a + i]));
    return s;
}

std::uint32_t element_bytes(const std::string& type) {
    if (type == "s8" || type == "u8" || type == "dummy8" || type == "fixstr") return 1;
    if (type == "s16" || type == "u16" || type == "fixstrW") return 2;
    if (type == "s32" || type == "u32" || type == "f32") return 4;
    return 0;
}

bool parse_definition(const std::vector<std::uint8_t>& b, ParamDefinition& d) {
    std::uint16_t count = 0, field_bytes = 0, format = 0;
    std::uint64_t fields_at = 0;
    if (!get(b, 0x08, &count) || !get(b, 0x0a, &field_bytes) || !get(b, 0x2e, &format) || !get(b, 0x30, &fields_at)) return false;
    if (format != 201 || field_bytes != 0xd0) return false;
    d.type = fixed(b, 0x0c, 0x20);
    if (d.type.empty() || fields_at + static_cast<std::uint64_t>(count) * field_bytes > b.size()) return false;
    struct Unit {
        bool open = false;
        std::uint32_t offset = 0, bytes = 0, used = 0;
    } unit;
    std::uint32_t offset = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t a = static_cast<std::size_t>(fields_at) + static_cast<std::size_t>(i) * field_bytes;
        ParamField f;
        std::uint32_t byte_count = 0;
        get(b, a + 0x64, &byte_count);
        f.type = fixed(b, a + 0x40, 8);
        std::string name = fixed(b, a + 0x90, 0x20);
        std::uint32_t bits = 0;
        if (const std::size_t colon = name.find(':'); colon != std::string::npos) {
            bits = static_cast<std::uint32_t>(std::strtoul(name.c_str() + colon + 1, nullptr, 10));
            name.resize(colon);
        }
        if (const std::size_t bracket = name.find('['); bracket != std::string::npos) {
            f.count = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::strtoul(name.c_str() + bracket + 1, nullptr, 10)));
            name.resize(bracket);
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        f.name = name;
        const std::uint32_t esize = element_bytes(f.type);
        if (bits && esize) {
            if (!unit.open || unit.bytes != esize || unit.used + bits > esize * 8) {
                if (unit.open) offset += unit.bytes;
                unit = Unit{true, offset, esize, 0};
            }
            f.offset = unit.offset;
            f.bytes = esize;
            f.bit = static_cast<std::uint8_t>(unit.used);
            f.bits = static_cast<std::uint8_t>(bits);
            unit.used += bits;
        } else {
            if (unit.open) {
                offset += unit.bytes;
                unit.open = false;
            }
            f.offset = offset;
            f.bytes = byte_count ? byte_count : esize * f.count;
            offset += f.bytes;
        }
        d.fields.push_back(std::move(f));
    }
    if (unit.open) offset += unit.bytes;
    d.row_bytes = offset;
    return true;
}

std::once_flag g_defs_once;
std::map<std::string, ParamDefinition> g_defs;  // by param type, written once

void load_definitions() {
    constexpr const char* kArchive = "/app0/dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx";
    char path[1024];
    if (runtime_file_translate(kArchive, path, sizeof(path)) != 0) {
        host_log("plugins: no path for %s", kArchive);
        return;
    }
    std::vector<std::uint8_t> raw;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        raw.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        if (!raw.empty() && std::fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
        std::fclose(f);
    }
    const std::vector<std::uint8_t> b = dcx_decompress(raw);
    if (b.size() < 0x40 || std::memcmp(b.data(), "BND4", 4) != 0) {
        host_log("plugins: %s is not a readable archive", path);
        return;
    }
    std::uint32_t n = 0;
    std::uint64_t header = 0, entry = 0;
    get(b, 0xc, &n);
    get(b, 0x10, &header);
    get(b, 0x20, &entry);
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint64_t e = header + k * entry;
        std::uint64_t size = 0;
        std::uint32_t data = 0;
        if (entry < 0x20 || e + entry > b.size() || !get(b, e + 8, &size) || !get(b, e + 0x18, &data) || data + size > b.size()) break;
        const std::vector<std::uint8_t> one(b.begin() + data, b.begin() + static_cast<long>(data + size));
        ParamDefinition d;
        if (parse_definition(one, d)) g_defs[d.type] = std::move(d);
    }
    host_log("plugins: %zu param definitions from the game's archive", g_defs.size());
}

const ParamDefinition* definition_for(const std::string& type) {
    std::call_once(g_defs_once, load_definitions);
    const auto it = g_defs.find(type);
    return it == g_defs.end() ? nullptr : &it->second;
}

std::string field_read(const ParamField& f, const std::uint8_t* row, std::uint32_t index) {
    const std::uint32_t esize = element_bytes(f.type);
    if (!row || !esize || index >= f.count) return "";
    char buf[64];
    if (f.bits) {
        std::uint32_t unit = 0;
        std::memcpy(&unit, row + f.offset, f.bytes);
        const std::uint32_t mask = f.bits >= 32 ? 0xffffffffu : ((1u << f.bits) - 1u);
        std::snprintf(buf, sizeof(buf), "%u", (unit >> f.bit) & mask);
        return buf;
    }
    if (f.type == "fixstr") return std::string(reinterpret_cast<const char*>(row + f.offset), strnlen(reinterpret_cast<const char*>(row + f.offset), f.bytes));
    if (f.type == "fixstrW") return "";
    const std::uint8_t* p = row + f.offset + index * esize;
    if (f.type == "f32") {
        float v;
        std::memcpy(&v, p, 4);
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    } else if (f.type == "s8") {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(static_cast<std::int8_t>(*p)));
    } else if (f.type == "u8" || f.type == "dummy8") {
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(*p));
    } else if (f.type == "s16" || f.type == "u16") {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        if (f.type == "s16") std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(static_cast<std::int16_t>(v)));
        else std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v));
    } else {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        if (f.type == "s32") std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v));
        else std::snprintf(buf, sizeof(buf), "%u", v);
    }
    return buf;
}

bool field_write(const ParamField& f, std::uint8_t* row, const std::string& text, std::uint32_t index) {
    const std::uint32_t esize = element_bytes(f.type);
    if (!row || !esize || index >= f.count || f.type == "fixstr" || f.type == "fixstrW") return false;
    if (f.bits) {
        const std::uint32_t mask = f.bits >= 32 ? 0xffffffffu : ((1u << f.bits) - 1u);
        const std::uint32_t v = static_cast<std::uint32_t>(std::strtoll(text.c_str(), nullptr, 0)) & mask;
        std::uint32_t unit = 0;
        std::memcpy(&unit, row + f.offset, f.bytes);
        unit = (unit & ~(mask << f.bit)) | (v << f.bit);
        std::memcpy(row + f.offset, &unit, f.bytes);
        return true;
    }
    std::uint8_t* p = row + f.offset + index * esize;
    if (f.type == "f32") {
        const float v = std::strtof(text.c_str(), nullptr);
        std::memcpy(p, &v, 4);
    } else {
        const std::uint32_t u = static_cast<std::uint32_t>(std::strtoll(text.c_str(), nullptr, 0));
        std::memcpy(p, &u, esize);
    }
    return true;
}

// =====================================================================================
// Param tables: SoloParamRepository (slot 0x5940340) keeps 63 slots of resource caps
// (repo + 0x70 + slot * 0x48: a count, then up to eight cap pointers); a cap's name is the
// std::wstring at +0x18 and its file is at *(*(cap + 0x70) + 0x70): row count u16 at +0xa,
// the type name at +0xc, format bytes at +0x2d/+0x2e, then the row records.

constexpr std::uint64_t kRepositorySlot = 0x5940340;
struct Table {
    std::string name, type;
    std::uint64_t file = 0;
    std::uint32_t rows = 0;
    std::uint8_t format = 0, flags = 0;
};
std::mutex g_tables_mu;
std::map<std::string, Table> g_tables;  // by name, under g_tables_mu
int g_param_ticks = 0;

std::string cap_name(std::uint64_t cap) {
    std::uint64_t capacity = 0, a = cap + 0x18;
    if (!rd(cap + 0x30, &capacity) || (capacity >= 8 && !rd(cap + 0x18, &a))) return "";
    std::string s;
    for (int i = 0; i < 64; ++i) {
        std::uint16_t c = 0;
        if (!rd(a + 2u * static_cast<unsigned>(i), &c) || !c) break;
        s.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return s;
}

bool record(const Table& t, std::uint32_t i, std::uint32_t* id, std::uint64_t* data) {
    if (i >= t.rows) return false;
    if (t.format == 2) {
        std::uint32_t off = 0;
        if (!rd(t.file + 0x34 + 0xc * i, id) || !rd(t.file + 0x34 + 0xc * i + 4, &off)) return false;
        *data = t.file + off;
    } else if (t.format <= 3 || !(t.flags & 2)) {
        std::uint32_t off = 0;
        if (!rd(t.file + 0x40 + 0xc * i, id) || !rd(t.file + 0x40 + 0xc * i + 4, &off)) return false;
        *data = t.file + off;
    } else {
        std::uint64_t off = 0;
        if (!rd(t.file + 0x40 + 0x18 * i, id) || !rd(t.file + 0x40 + 0x18 * i + 8, &off)) return false;
        *data = t.file + off;
    }
    return true;
}

std::size_t row_size(const Table& t) {
    std::uint32_t id0 = 0, id1 = 0;
    std::uint64_t d0 = 0, d1 = 0;
    if (t.rows < 2 || !record(t, 0, &id0, &d0) || !record(t, 1, &id1, &d1) || d1 <= d0) return 0;
    return static_cast<std::size_t>(d1 - d0);
}

bool table(const char* name, Table* out) {
    std::lock_guard<std::mutex> lk(g_tables_mu);
    auto it = g_tables.find(name ? name : "");
    if (it == g_tables.end()) return false;
    *out = it->second;
    return true;
}

// The definition when its row size is the table's, else none (named fields refused).
const ParamDefinition* trusted(const Table& t) {
    const ParamDefinition* def = t.type.empty() ? nullptr : definition_for(t.type);
    if (!def) return nullptr;
    const std::size_t size = row_size(t);
    return !size || size == def->row_bytes ? def : nullptr;
}

void walk_params() {
    const std::uint64_t repo = g_109 ? rp(at(kRepositorySlot, 8)) : 0;
    if (!repo) return;
    for (unsigned slot = 0; slot < 63; ++slot) {
        std::int32_t count = 0;
        if (!rd(repo + 0x70 + slot * 0x48, &count) || count <= 0) continue;
        for (unsigned k = 0; k < static_cast<unsigned>(count) && k < 8; ++k) {
            const std::uint64_t cap = rp(repo + 0x70 + slot * 0x48 + 8 + 8 * k);
            const std::uint64_t holder = cap ? rp(cap + 0x70) : 0;
            const std::uint64_t file = holder ? rp(holder + 0x70) : 0;
            std::uint16_t rows = 0;
            std::uint8_t format = 0, flags = 0;
            char type[33] = {};
            if (!file || !rd(file + 0xa, &rows) || !rd(file + 0x2d, &format) || !rd(file + 0x2e, &flags)) continue;
            read(file + 0xc, type, 32);
            const std::string name = cap_name(cap);
            if (name.empty()) continue;
            std::lock_guard<std::mutex> lk(g_tables_mu);
            Table& t = g_tables[name];
            if (t.file == file && t.rows == rows) continue;
            t = Table{name, std::string(type), file, rows, format, flags};
        }
    }
}

const ParamField* named_field(const char* table_name, std::uint32_t id, const char* field, std::uint8_t** row,
                              std::uint32_t* index) {
    Table t;
    if (!g_109 || !field || !table(table_name, &t)) return nullptr;
    const ParamDefinition* def = trusted(t);
    if (!def) return nullptr;
    std::string name = field;
    *index = 0;
    if (const std::size_t br = name.find('['); br != std::string::npos) {
        *index = static_cast<std::uint32_t>(std::strtoul(name.c_str() + br + 1, nullptr, 10));
        name.resize(br);
    }
    const ParamField* f = def->field(name);
    *row = static_cast<std::uint8_t*>(param_row(table_name, id, nullptr));
    return f && *row ? f : nullptr;
}

}  // namespace

void params_tick() {
    // The repository fills during the boot; a walk every 150 frames sees new tables and reloads.
    if (g_param_ticks++ % 150 == 0) walk_params();
}

void* param_row(const char* table_name, std::uint32_t id, std::size_t* bytes) {
    Table t;
    if (!g_109 || !table(table_name, &t)) return nullptr;
    std::uint32_t lo = 0, hi = t.rows;  // the records' ids ascend
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        std::uint32_t rid = 0;
        std::uint64_t data = 0;
        if (!record(t, mid, &rid, &data)) return nullptr;
        if (rid == id) {
            if (bytes) *bytes = row_size(t);
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(data));
        }
        if (rid < id) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

bool param_ids(const char* table_name, std::uint32_t* ids, std::size_t max, std::size_t* count) {
    Table t;
    if (!g_109 || !table(table_name, &t)) return false;
    if (count) *count = t.rows;
    for (std::uint32_t i = 0; ids && i < t.rows && i < max; ++i) {
        std::uint64_t data = 0;
        if (!record(t, i, &ids[i], &data)) return false;
    }
    return true;
}

bool param_get(const char* table_name, std::uint32_t id, const char* field, std::string* out) {
    std::uint8_t* row = nullptr;
    std::uint32_t index = 0;
    const ParamField* f = named_field(table_name, id, field, &row, &index);
    if (!f) return false;
    *out = field_read(*f, row, index);
    return !out->empty();
}

bool param_set(const char* table_name, std::uint32_t id, const char* field, const char* value) {
    std::uint8_t* row = nullptr;
    std::uint32_t index = 0;
    const ParamField* f = named_field(table_name, id, field, &row, &index);
    return f && value && field_write(*f, row, value, index);
}

}  // namespace engine
