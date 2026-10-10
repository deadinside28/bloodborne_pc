// SPDX-License-Identifier: GPL-3.0-or-later
// bbhost's plugin host (host/plugins.cpp, github.com/droogie/bbhost, GPL-3.0-or-later) on
// bbport's loader. The game's code and ours share the System V ABI and the thread: a hook's
// stub calls into the plugin on the game's own stack, and call_guest is a plain call.
#include "plugins/plugins.h"

#include "online/common.h"
#include "online/np_http.h"
#include "plugins/bbhost_plugin.h"
#include "plugins/engine.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>

// A game call with every argument register (bbhost's core/thunk.h GuestCallRegs).
struct GuestCallRegs {
    void* fn;                // +0
    std::uint64_t ints[6];   // +8: rdi rsi rdx rcx r8 r9
    std::uint64_t xmm[8];    // +56
    std::uint64_t stack[4];  // +120: the 7th.. integer arguments, at [rsp+8].. in the callee
    std::uint64_t ret;       // +152: rax
    std::uint64_t ret_xmm0;  // +160
};
static_assert(sizeof(GuestCallRegs) == 168, "bb_call_guest_regs reads these offsets");
extern "C" void bb_call_guest_regs(GuestCallRegs* r);
asm(R"(
    .text
    .globl bb_call_guest_regs
    .hidden bb_call_guest_regs
    .type bb_call_guest_regs,@function
bb_call_guest_regs:
    .cfi_startproc
    push %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    mov %rsp, %rbp
    .cfi_def_cfa_register %rbp
    push %rbx
    .cfi_offset %rbx, -24
    push %r12
    .cfi_offset %r12, -32
    mov %rdi, %rbx
    sub $32, %rsp
    mov 120(%rbx), %r11
    mov %r11, 0(%rsp)
    mov 128(%rbx), %r11
    mov %r11, 8(%rsp)
    mov 136(%rbx), %r11
    mov %r11, 16(%rsp)
    mov 144(%rbx), %r11
    mov %r11, 24(%rsp)
    mov 8(%rbx), %rdi
    mov 16(%rbx), %rsi
    mov 24(%rbx), %rdx
    mov 32(%rbx), %rcx
    mov 40(%rbx), %r8
    mov 48(%rbx), %r9
    movq 56(%rbx), %xmm0
    movq 64(%rbx), %xmm1
    movq 72(%rbx), %xmm2
    movq 80(%rbx), %xmm3
    movq 88(%rbx), %xmm4
    movq 96(%rbx), %xmm5
    movq 104(%rbx), %xmm6
    movq 112(%rbx), %xmm7
    mov $8, %eax
    call *0(%rbx)
    add $32, %rsp
    mov %rax, 152(%rbx)
    movq %xmm0, 160(%rbx)
    pop %r12
    pop %rbx
    pop %rbp
    .cfi_def_cfa %rsp, 8
    ret
    .cfi_endproc
    .size bb_call_guest_regs, .-bb_call_guest_regs
    .previous
)");

namespace {

constexpr const char kEboot109Sha256[] = "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7";

struct Plugin {
    std::string name, path;
    void* handle = nullptr;
    int (*init)(const BbHostApi*) = nullptr;
    int (*image)(const BbHostApi*) = nullptr;
    const BbPluginInfo* info = nullptr;
    const BbOption* options = nullptr;
};
std::vector<Plugin> g_plugins;  // written while the image is patched, read after

// ---- hooks: a stub per hook (bbhost's thunk_emit_prologue_stub), calling hook_dispatch(id,
// saved) with saved -> r9 r8 rcx rdx rsi rdi, saved[-1] the return slot, xmm0-7 16 bytes apart
// 136 bytes below saved.

struct Hook {
    BbHookFn fn;
    BbHookFn2 fn2;
    void* user;
};
constexpr std::size_t kMaxHooks = 256;
Hook g_hooks[kMaxHooks];  // by id from 1; a slot is written before its stub is reachable
std::atomic<std::size_t> g_hook_count{0};
std::mutex g_hook_mu;

std::int64_t hook_dispatch(std::uint64_t id, std::uint64_t* saved);

std::uint8_t* emit_stub(std::uint8_t* c, std::uint64_t id, void* host, std::uint64_t resume, const std::uint8_t* displaced,
                        std::size_t n) {
    // 48 + 136 bytes over the entry's 8 keep the host call 16-byte aligned.
    static constexpr std::uint8_t kSave[] = {
        0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,              // push rdi rsi rdx rcx r8 r9
        0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00,                    // sub rsp, 136
        0xf3, 0x0f, 0x7f, 0x44, 0x24, 0x00,                          // movdqu [rsp], xmm0
        0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10,                          // movdqu [rsp+16], xmm1
        0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20,                          // movdqu [rsp+32], xmm2
        0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30,                          // movdqu [rsp+48], xmm3
        0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40,                          // movdqu [rsp+64], xmm4
        0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,                          // movdqu [rsp+80], xmm5
        0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x60,                          // movdqu [rsp+96], xmm6
        0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70,                          // movdqu [rsp+112], xmm7
        0x48, 0xc7, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0, 0, 0, 0,  // mov qword [rsp+128], 0
        0x48, 0x8d, 0xb4, 0x24, 0x88, 0x00, 0x00, 0x00,              // lea rsi, [rsp+136]
    };
    static constexpr std::uint8_t kRestore[] = {
        0x4c, 0x8b, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00,  // mov r10, [rsp+128]
        0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x00,              // movdqu xmm0, [rsp]
        0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10,              // movdqu xmm1, [rsp+16]
        0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20,              // movdqu xmm2, [rsp+32]
        0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30,              // movdqu xmm3, [rsp+48]
        0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40,              // movdqu xmm4, [rsp+64]
        0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,              // movdqu xmm5, [rsp+80]
        0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x60,              // movdqu xmm6, [rsp+96]
        0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70,              // movdqu xmm7, [rsp+112]
        0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00,        // add rsp, 136
        0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f,  // pop r9 r8 rcx rdx rsi rdi
    };
    static constexpr std::uint8_t kReturn[] = {
        0x4d, 0x85, 0xdb,  // test r11, r11
        0x74, 0x04,        // jz +4: run the function
        0x4c, 0x89, 0xd0,  // mov rax, r10: the return slot
        0xc3,              // ret to the function's caller
    };
    std::memcpy(c, kSave, sizeof(kSave));
    c += sizeof(kSave);
    *c++ = 0x48;
    *c++ = 0xbf;  // movabs rdi, id
    std::memcpy(c, &id, 8);
    c += 8;
    const std::uint64_t fn = reinterpret_cast<std::uint64_t>(host);
    *c++ = 0x48;
    *c++ = 0xb8;  // movabs rax, host
    std::memcpy(c, &fn, 8);
    c += 8;
    *c++ = 0xff;
    *c++ = 0xd0;  // call rax
    *c++ = 0x49;
    *c++ = 0x89;
    *c++ = 0xc3;  // mov r11, rax (the pops leave r11 alone)
    std::memcpy(c, kRestore, sizeof(kRestore));
    c += sizeof(kRestore);
    std::memcpy(c, kReturn, sizeof(kReturn));
    c += sizeof(kReturn);
    std::memcpy(c, displaced, n);
    c += n;
    static constexpr std::uint8_t kJmp[6] = {0xff, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
    std::memcpy(c, kJmp, 6);
    c += 6;
    std::memcpy(c, &resume, 8);
    return c + 8;
}

// The function's first n bytes (whole instructions, none rip-relative) move into a stub
// that calls hook_dispatch; the function's entry jumps there. 0, 1 (bytes differ), 2.
int place_hook(std::uint64_t bn, const std::uint8_t* prologue, std::size_t n, BbHookFn fn, BbHookFn2 fn2, void* user) {
    const std::uint64_t at = engine::at(bn, n);
    if (!at || n < 14 || n > 64) return 1;
    if (prologue && std::memcmp(reinterpret_cast<const void*>(at), prologue, n) != 0) return 1;
    std::uint8_t displaced[64];
    std::memcpy(displaced, reinterpret_cast<const void*>(at), n);
    std::lock_guard<std::mutex> lk(g_hook_mu);
    const std::size_t index = g_hook_count.load();
    if (index >= kMaxHooks) return 2;
    void* page = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 2;
    g_hooks[index] = Hook{fn, fn2, user};
    emit_stub(static_cast<std::uint8_t*>(page), index + 1, reinterpret_cast<void*>(&hook_dispatch), at + n, displaced, n);
    if (mprotect(page, 0x1000, PROT_READ | PROT_EXEC) != 0) {
        munmap(page, 0x1000);
        return 2;
    }
    std::uint8_t entry[64];
    const std::uint64_t dest = reinterpret_cast<std::uint64_t>(page);
    entry[0] = 0xff;
    entry[1] = 0x25;  // jmp [rip+0]
    std::memset(entry + 2, 0, 4);
    std::memcpy(entry + 6, &dest, 8);
    std::memset(entry + 14, 0xcc, n - 14);
    g_hook_count.store(index + 1);
    if (!engine::code_write(at, entry, n)) return 2;
    return 0;
}

std::int64_t hook_dispatch(std::uint64_t id, std::uint64_t* saved) {
    if (id == 0 || id > g_hook_count.load(std::memory_order_acquire)) return 0;
    const Hook& h = g_hooks[id - 1];
    if (h.fn2) {
        auto* xmm = reinterpret_cast<std::uint8_t*>(saved) - 136;
        BbHookCtx c{};
        for (int i = 0; i < 6; ++i) c.arg[i] = saved[5 - i];
        for (int i = 0; i < 8; ++i) std::memcpy(&c.xmm[i], xmm + 16 * i, 8);
        const int skip = h.fn2(&c, h.user);
        for (int i = 0; i < 6; ++i) saved[5 - i] = c.arg[i];
        for (int i = 0; i < 8; ++i) std::memcpy(xmm + 16 * i, &c.xmm[i], 8);
        if (skip) saved[-1] = c.ret;
        return skip ? 1 : 0;
    }
    const BbHookFrame f{saved[5], saved[4], saved[3], saved[2], saved[1], saved[0]};
    return h.fn(&f, h.user) ? 1 : 0;
}

// ---- the API

void api_log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    host_log("plugin: %s", line);
}

const char* api_config(const char* key) {
    thread_local std::string value;
    value = key ? config_value(key) : std::string();
    return value.c_str();
}

std::string data_dir() {
    const char* d = std::getenv("BB_DATA_DIR");
    return d && d[0] ? std::string(d) : std::string();
}

const char* api_data_dir() {
    static const std::string d = data_dir();
    return d.c_str();
}
const char* api_mods_dir() { return ""; }

int api_register_hle(const char*, void*) {
    host_log("plugin: register_hle is not offered by bbport");
    return 1;
}

int api_eboot_is_109() { return engine::is_109() ? 1 : 0; }
// bbport loads the PS4 SELF, whose file hash is not bbhost's decrypted eboot's: the build is
// told by its code instead (engine::is_109), and named by the hash bbhost knows it by.
const char* api_eboot_sha256() { return engine::is_109() ? kEboot109Sha256 : "not-1.09"; }
std::uint64_t api_guest_addr(std::uint64_t bn) { return engine::is_109() ? engine::at(bn) : 0; }

// A Binary Ninja address inside the image, else a runtime address as given.
std::uint64_t runtime_of(std::uint64_t addr, std::size_t n) {
    const std::uint64_t in = engine::at(addr, n);
    return in ? in : addr;
}

int api_read(std::uint64_t bn, void* out, std::size_t n) { return out && engine::read(runtime_of(bn, n), out, n) ? 0 : 1; }

// A data write: it keeps the page's protection (a code page refuses it).
int api_write(std::uint64_t bn, const void* data, std::size_t n) {
    const std::uint64_t at = engine::at(bn, n);
    if (!at || !data || !engine::is_109()) return 1;
    return host_write_safe(reinterpret_cast<void*>(at), data, n) ? 0 : 2;
}

int api_patch(std::uint64_t bn, const void* expect, const void* bytes, std::size_t n) {
    const std::uint64_t at = engine::at(bn, n);
    if (!at || !expect || !bytes || !engine::is_109()) return 1;
    const void* p = reinterpret_cast<const void*>(at);
    if (std::memcmp(p, expect, n) != 0) return std::memcmp(p, bytes, n) == 0 ? 0 : 1;
    return engine::code_write(at, bytes, n) ? 0 : 2;
}

int api_hook(std::uint64_t bn, const void* prologue, std::size_t n, BbHookFn fn, void* user) {
    if (!fn || !prologue || !engine::is_109()) return 1;
    return place_hook(bn, static_cast<const std::uint8_t*>(prologue), n, fn, nullptr, user);
}

int api_hook2(std::uint64_t bn, const void* prologue, std::size_t n, BbHookFn2 fn, void* user) {
    if (!fn || !engine::is_109()) return 1;
    return place_hook(bn, static_cast<const std::uint8_t*>(prologue), n, nullptr, fn, user);
}

struct FrameCallback {
    void (*fn)(void*);
    void* user;
};
struct FlagCallback {
    void (*fn)(std::uint32_t, int, void*);
    void* user;
};
struct WorldLoadCallback {
    void (*fn)(std::uint32_t, void*);
    void* user;
};
struct OptionCallback {
    std::string name;
    void (*fn)(const char*, const char*, void*);
    void* user;
};
struct ActionCallback {
    std::string name;
    void (*fn)(const char*, void*);
    void* user;
};
struct MenuRequest {
    bool action;
    std::string name, key, value;
};
std::mutex g_frame_mu;
std::vector<FrameCallback> g_frame_callbacks;      // under g_frame_mu
std::vector<FlagCallback> g_flag_callbacks;        // under g_frame_mu
std::vector<WorldLoadCallback> g_world_callbacks;  // under g_frame_mu
std::vector<OptionCallback> g_option_callbacks;    // under g_frame_mu
std::vector<ActionCallback> g_action_callbacks;    // under g_frame_mu
std::vector<MenuRequest> g_menu_requests;          // under g_frame_mu
std::uint64_t g_last_player = 0;
std::uint32_t g_last_block = 0;

int api_on_frame(void (*fn)(void*), void* user) {
    if (!fn) return 1;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_frame_callbacks.push_back({fn, user});
    return 0;
}

void* api_param_row(const char* table, std::uint32_t id, std::size_t* bytes) { return engine::param_row(table, id, bytes); }

int api_param_get(const char* table, std::uint32_t id, const char* field, char* out, std::size_t n) {
    std::string v;
    if (!out || !n || !engine::param_get(table, id, field, &v)) return 1;
    std::snprintf(out, n, "%s", v.c_str());
    return 0;
}

int api_param_set(const char* table, std::uint32_t id, const char* field, const char* value) {
    return engine::param_set(table, id, field, value) ? 0 : 1;
}

int api_lua_event(const char* name) { return engine::lua_event_queue(name) ? 0 : 1; }

int api_event_flag_get(std::uint32_t id, int* value) {
    bool v = false;
    if (!engine::event_flag_get(id, &v)) return 1;
    if (value) *value = v ? 1 : 0;
    return 0;
}
int api_event_flag_set(std::uint32_t id, int value) { return engine::event_flag_set(id, value != 0) ? 0 : 1; }
int api_player_stat_get(const char* name, std::int64_t* value) { return engine::player_stat_get(name, value) ? 0 : 1; }
int api_player_stat_set(const char* name, std::int64_t value) { return engine::player_stat_set(name, value) ? 0 : 1; }

int api_on_event_flag(void (*fn)(std::uint32_t, int, void*), void* user) {
    if (!fn || !engine::event_flags_watch()) return 1;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_flag_callbacks.push_back({fn, user});
    return 0;
}

int api_player_position(float xyz[3], float* yaw) { return engine::player_position(xyz, yaw) ? 0 : 1; }
int api_camera_get(float pos[3], float focus[3]) { return engine::camera(pos, focus) ? 0 : 1; }
int api_player_block(std::uint32_t* block) { return engine::player_block(block) ? 0 : 1; }
int api_player_warp(std::uint32_t block, const float xyz[3], float yaw) { return engine::player_warp_queue(block, xyz, yaw) ? 0 : 1; }
int api_lamp_warp(std::int32_t return_point) { return engine::lamp_warp_queue(return_point) ? 0 : 1; }

std::uint64_t api_symbol(const char*) { return 0; }  // bbhost's named engine symbols are not carried over

int api_call_guest(std::uint64_t bn, BbCallRegs* regs) {
    const std::uint64_t at = engine::is_109() ? engine::at(bn) : 0;
    if (!at || !regs) return 1;
    GuestCallRegs r{};
    r.fn = reinterpret_cast<void*>(static_cast<std::uintptr_t>(at));
    std::memcpy(r.ints, regs->arg, sizeof(r.ints));
    std::memcpy(r.xmm, regs->xmm, sizeof(r.xmm));
    std::memcpy(r.stack, regs->stack, sizeof(r.stack));
    bb_call_guest_regs(&r);
    regs->ret = r.ret;
    regs->ret_xmm0 = r.ret_xmm0;
    return 0;
}

int api_param_ids(const char* table, std::uint32_t* ids, std::size_t max, std::size_t* count) {
    return engine::param_ids(table, ids, max, count) ? 0 : 1;
}

int api_on_world_load(void (*fn)(std::uint32_t, void*), void* user) {
    if (!fn) return 1;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_world_callbacks.push_back({fn, user});
    return 0;
}

template <typename T>
T field(const unsigned char* block, std::size_t off) {
    T v;
    std::memcpy(&v, block + off, sizeof(T));
    return v;
}

// The world's characters, by offset, every read checked (a set can change under a load), each
// object's fields in one read.
bool fill_chr(std::uint64_t ins, std::uint64_t player_vtable, BbChr* c) {
    unsigned char head[0x90];  // ChrIns: vtable +0, handle +8, chr type +0x78, team +0x88
    if (!engine::read(ins, head, sizeof(head)) || !field<std::uint64_t>(head, 0)) return false;
    *c = BbChr{};
    c->ins = reinterpret_cast<void*>(static_cast<std::uintptr_t>(ins));
    c->handle = field<std::uint32_t>(head, 0x08);
    c->chr_type = field<std::int32_t>(head, 0x78);
    c->team_type = field<std::int32_t>(head, 0x88);
    c->npc_param = -1;
    unsigned char tail[0x20];  // the modules +0x3b0; NpcIns: the NpcParam reference +0x3c8 (an id, then the row)
    if (!engine::read(ins + 0x3b0, tail, sizeof(tail))) return true;
    std::int32_t id = -1;
    if (field<std::uint64_t>(head, 0) != player_vtable && engine::rd(field<std::uint64_t>(tail, 0x18), &id)) c->npc_param = id;
    unsigned char m[0x50];  // modules [0x20, 0x70): data +0x20, physics +0x68
    const std::uint64_t modules = field<std::uint64_t>(tail, 0);
    if (!modules || !engine::read(modules + 0x20, m, sizeof(m))) return true;
    unsigned char d[0x80];  // data [0xf8, 0x178): hp +0xf8, max +0xfc, the character id +0x158
    if (const std::uint64_t data = field<std::uint64_t>(m, 0); data && engine::read(data + 0xf8, d, sizeof(d))) {
        c->hp = field<std::int32_t>(d, 0);
        c->max_hp = field<std::int32_t>(d, 4);
        // The character id ("c2560"): a small-string wide string (length +0x168, capacity +0x170).
        const std::uint64_t len = field<std::uint64_t>(d, 0x70), cap = field<std::uint64_t>(d, 0x78);
        if (len && len <= 16) {
            std::uint16_t w[16] = {};
            bool ok = true;
            if (cap <= 7) std::memcpy(w, d + 0x60, 16);
            else ok = engine::read(field<std::uint64_t>(d, 0x60), w, len * 2);
            if (ok && w[0] == 'c') {
                for (std::uint64_t i = 1; i < len && w[i] >= '0' && w[i] <= '9'; ++i) c->model = c->model * 10 + (w[i] - '0');
            }
        }
    }
    if (const std::uint64_t phys = field<std::uint64_t>(m, 0x48)) engine::rd(phys + 0x1e0, &c->pos);
    return true;
}

int api_chr_list(BbChr* out, std::size_t max, std::size_t* count) {
    const std::uint64_t man = engine::world_chr_man();
    const std::uint64_t main = man ? engine::rp(man + 0x60) : 0;
    if (!main) return 1;
    const std::uint64_t player_vt = engine::rp(main);
    // The main player, then the handle-indexed sets (WorldChrMan+0x850, 0x3e ChrSet pointers; a
    // set: its capacity +0, its entries +8, 0x38 bytes each with the ChrIns first).
    thread_local std::vector<std::uint64_t> found, sorted;
    thread_local std::vector<unsigned char> entries, taken;
    found.assign(1, main);
    std::uint64_t sets[0x3e];
    if (engine::read(man + 0x850, sets, sizeof(sets))) {
        for (const std::uint64_t set : sets) {
            struct {
                std::uint32_t cap, pad;
                std::uint64_t entries;
            } h;
            if (!set || !engine::read(set, &h, sizeof(h)) || !h.entries || h.cap > 4096) continue;
            entries.resize(h.cap * 0x38);
            if (!engine::read(h.entries, entries.data(), entries.size())) continue;
            for (std::size_t k = 0; k < h.cap; ++k) {
                if (const auto ins = field<std::uint64_t>(entries.data(), k * 0x38)) found.push_back(ins);
            }
        }
    }
    // Each character once, in the order found.
    sorted = found;
    std::sort(sorted.begin(), sorted.end());
    taken.assign(sorted.size(), 0);
    std::size_t n = 0;
    for (std::size_t i = 0; i < found.size(); ++i) {
        const std::size_t at = std::lower_bound(sorted.begin(), sorted.end(), found[i]) - sorted.begin();
        if (taken[at]) continue;
        taken[at] = 1;
        if (out && n < max) {
            BbChr c;
            if (!fill_chr(found[i], player_vt, &c)) continue;
            c.is_player = i == 0 ? 1 : 0;
            out[n] = c;
        } else if (!engine::rp(found[i])) {
            continue;  // past `max`, only the count: a live ChrIns has its vtable
        }
        ++n;
    }
    if (count) *count = n;
    return 0;
}

std::uint64_t chr_or_player(void* chr) {
    if (chr) return reinterpret_cast<std::uint64_t>(chr);
    const std::uint64_t man = engine::world_chr_man();
    return man ? engine::rp(man + 0x60) : 0;
}

int api_sp_effect_apply(void* chr, std::int32_t id) {
    const std::uint64_t ins = chr_or_player(chr);
    const std::uint64_t vt = ins ? engine::rp(ins) : 0;
    const std::uint64_t fn = vt ? engine::rp(vt + 0x3f0) : 0;
    if (!fn) return 1;
    // As Lua_MultiDoping (0x178bc70) calls it: target, effect, source = the target, then five
    // 1.0 multipliers in xmm0-4.
    GuestCallRegs r{};
    r.fn = reinterpret_cast<void*>(static_cast<std::uintptr_t>(fn));
    r.ints[0] = ins;
    r.ints[1] = static_cast<std::uint32_t>(id);
    r.ints[2] = ins;
    for (int i = 0; i < 5; ++i) r.xmm[i] = 0x3f800000u;
    bb_call_guest_regs(&r);
    return 0;
}

int api_sp_effect_has(void* chr, std::int32_t id) {
    const std::uint64_t ins = chr_or_player(chr);
    const std::uint64_t list = ins ? engine::rp(ins + 0x1c8) : 0;
    std::uint64_t node = list ? engine::rp(list + 0x08) : 0;
    for (int guard = 0; node && guard < 1024; ++guard) {
        std::int32_t have = 0;
        if (engine::rd(node + 0x40, &have) && have == id) return 1;
        node = engine::rp(node + 0x58);
    }
    return 0;
}

void make_dirs(const std::string& path) {
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
}

const char* api_plugin_dir(const char* name) {
    thread_local std::string dir;
    const std::string data = data_dir();
    if (!name || !name[0] || data.empty() || std::strpbrk(name, "/\\.:")) return "";
    dir = data + "/plugins/" + name;
    make_dirs(dir);
    return dir.c_str();
}

std::mutex g_banner_mu;
std::string g_banner;  // under g_banner_mu
std::chrono::steady_clock::time_point g_banner_from, g_banner_until;

int api_show_message(const char* text, float seconds) {
    if (!text) return 1;
    host_log("plugin: on screen: %s", text);
    std::lock_guard<std::mutex> lk(g_banner_mu);
    g_banner = text;
    g_banner_from = std::chrono::steady_clock::now();
    g_banner_until = g_banner_from + std::chrono::milliseconds(static_cast<long>(std::clamp(seconds, 0.5f, 60.0f) * 1000.0f));
    return 0;
}

int api_set_time_scale(float) { return 1; }  // not in bbport

int api_on_option(const char* name, void (*fn)(const char*, const char*, void*), void* user) {
    if (!name || !fn) return 1;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_option_callbacks.push_back({name, fn, user});
    return 0;
}

int api_on_action(const char* name, void (*fn)(const char*, void*), void* user) {
    if (!name || !fn) return 1;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_action_callbacks.push_back({name, fn, user});
    return 0;
}

// Plugin file overlays and other players' rules are bbhost's (a randomizer's): not in bbport.
int api_game_file(const char*, const void**, std::size_t*) { return 1; }
int api_overlay_file(const char*, const char*, const void*, std::size_t) { return 1; }
std::mutex g_rules_mu;
std::map<std::string, std::string> g_rules;  // set_rules, under g_rules_mu
int api_set_rules(const char* name, const char* rules) {
    if (!name || !rules) return 1;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    g_rules[name] = rules;
    return 0;
}
int api_on_world_rules(const char*, void (*)(const char*, void*), void*) { return 0; }
int api_visitor(const char*) { return 0; }
int api_replace_text(const BbText*, std::size_t) { return -1; }

const BbHostApi g_api = {
    BB_PLUGIN_API_VERSION, sizeof(BbHostApi), &api_log, &api_config, &api_data_dir, &api_mods_dir, &api_register_hle,
    &api_eboot_sha256, &api_eboot_is_109, &api_guest_addr, &api_read, &api_write, &api_patch, &api_hook,
    &api_on_frame, &api_param_row, &api_param_get, &api_param_set, &api_lua_event, &api_event_flag_get, &api_event_flag_set,
    &api_player_stat_get, &api_player_stat_set, &api_on_event_flag, &api_player_position, &api_camera_get,
    &api_player_block, &api_player_warp, &api_lamp_warp,
    &api_symbol, &api_call_guest, &api_hook2, &api_param_ids, &api_on_world_load, &api_chr_list,
    &api_sp_effect_apply, &api_sp_effect_has, &api_plugin_dir, &api_show_message, &api_set_time_scale,
    &api_on_option, &api_on_action, &api_game_file, &api_overlay_file,
    &api_set_rules, &api_on_world_rules, &api_visitor,
    &api_replace_text,
};

// ---- once a game frame, on the main thread: the start of SprjFlipper's update (0x2434770)

void plugins_frame() {
    std::vector<FrameCallback> now;
    std::vector<WorldLoadCallback> loads;
    std::vector<MenuRequest> requests;
    std::vector<OptionCallback> options;
    std::vector<ActionCallback> actions;
    {
        std::lock_guard<std::mutex> lk(g_frame_mu);
        requests.swap(g_menu_requests);
        options = g_option_callbacks;
        actions = g_action_callbacks;
        now = g_frame_callbacks;
        loads = g_world_callbacks;
    }
    for (const MenuRequest& r : requests) {
        if (r.action) {
            host_log("plugin: %s: action %s", r.name.c_str(), r.key.c_str());
            for (const ActionCallback& c : actions) {
                if (c.name == r.name) c.fn(r.key.c_str(), c.user);
            }
            continue;
        }
        for (const OptionCallback& c : options) {
            if (c.name == r.name) c.fn(r.key.c_str(), r.value.c_str(), c.user);
        }
    }
    if (!loads.empty()) {
        // A world load is a new main player (every load builds one) or the player in another block.
        const std::uint64_t man = engine::world_chr_man();
        const std::uint64_t player = man ? engine::rp(man + 0x60) : 0;
        std::uint32_t block = 0;
        if (!player || !engine::player_block(&block)) {
            g_last_player = 0;
        } else if (player != g_last_player || block != g_last_block) {
            g_last_player = player;
            g_last_block = block;
            for (const WorldLoadCallback& c : loads) c.fn(block, c.user);
        }
    }
    for (const FrameCallback& c : now) c.fn(c.user);
}

void flag_changes() {
    std::vector<FlagCallback> calls;
    {
        std::lock_guard<std::mutex> lk(g_frame_mu);
        if (g_flag_callbacks.empty()) return;
        calls = g_flag_callbacks;
    }
    static std::uint64_t lost_said = 0;
    engine::FlagChange changes[256];
    std::uint64_t lost = 0;
    std::size_t n = 0;
    while ((n = engine::event_flags_take(changes, 256, &lost)) != 0) {
        for (std::size_t i = 0; i < n; ++i) {
            for (const FlagCallback& c : calls) c.fn(changes[i].id, changes[i].value ? 1 : 0, c.user);
        }
    }
    if (lost != lost_said) {
        host_log("plugins: %llu event flag changes dropped (more than the ring holds between two frames)",
                 static_cast<unsigned long long>(lost - lost_said));
        lost_said = lost;
    }
}

int frame_hook(const BbHookFrame*, void*) {
    engine::params_tick();
    engine::lua_events_tick();
    flag_changes();
    plugins_frame();
    engine::world_tick();
    return 0;  // the game's update runs
}

// ---- loading

std::string exe_dir() {
    char path[4096];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return {};
    path[n] = 0;
    std::string p = path;
    return p.substr(0, p.rfind('/'));
}

std::vector<std::string> plugin_dirs() {
    std::vector<std::string> dirs = {exe_dir() + "/plugins"};
    if (const std::string d = data_dir(); !d.empty()) dirs.push_back(d + "/plugins");
    return dirs;
}

// Plugin files by file name; a later directory's file of the same name wins.
std::map<std::string, std::string> plugin_files() {
    std::map<std::string, std::string> files;
    for (const std::string& dir : plugin_dirs()) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) files[n] = dir + "/" + n;
        }
        closedir(d);
    }
    return files;
}

// plugins.<name>: 1/true on, 0/false off, -1 the plugin's own default.
int config_switch(const std::string& name) {
    const std::string v = config_value("plugins." + name);
    if (v == "true" || v == "1") return 1;
    if (v == "false" || v == "0") return 0;
    return -1;
}

PluginInfo info_of(const std::string& file, const BbPluginInfo* info, const BbOption* options) {
    PluginInfo e;
    e.name = file.substr(0, file.rfind('.'));
    if (info) {
        if (info->name && info->name[0]) e.name = info->name;
        e.flags = info->flags;
        e.title = info->title ? info->title : "";
        e.version = info->version ? info->version : "";
        e.description = info->description ? info->description : "";
    }
    if (e.title.empty()) e.title = e.name;
    for (const BbOption* o = options; o && o->type != BB_OPT_END; ++o) {
        PluginOption d;
        d.type = o->type & 0xffu;
        d.live = (o->type & BB_OPT_LIVE) != 0;
        d.key = o->key ? o->key : "";
        d.label = o->label ? o->label : d.key;
        d.help = o->help ? o->help : "";
        d.def = o->def ? o->def : "";
        d.choices = o->choices ? o->choices : "";
        d.min = o->min;
        d.max = o->max;
        e.options.push_back(std::move(d));
    }
    const int sw = config_switch(e.name);
    e.enabled = sw == 1 || (sw < 0 && !(e.flags & BB_PLUGIN_OPT_IN));
    return e;
}

std::vector<PluginInfo> g_catalog;  // every plugin file, as found at the start
std::mutex g_catalog_mu;

void load_all() {
    for (const auto& [file, path] : plugin_files()) {
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            host_log("plugin: %s: cannot load: %s", path.c_str(), dlerror());
            continue;
        }
        Plugin p;
        p.path = path;
        p.handle = h;
        p.init = reinterpret_cast<int (*)(const BbHostApi*)>(dlsym(h, "bb_plugin_init"));
        p.image = reinterpret_cast<int (*)(const BbHostApi*)>(dlsym(h, "bb_plugin_image"));
        p.info = static_cast<const BbPluginInfo*>(dlsym(h, "bb_plugin_info"));
        p.options = static_cast<const BbOption*>(dlsym(h, "bb_plugin_options"));
        PluginInfo entry = info_of(file, p.info, p.options);
        p.name = entry.name;
        if (!p.init && !p.image) {
            host_log("plugin: %s: no bb_plugin_init or bb_plugin_image; not a plugin", path.c_str());
            dlclose(h);
            continue;
        }
        const std::uint32_t flags = p.info ? p.info->flags : 0;
        const int sw = config_switch(p.name);
        if (!entry.enabled) {
            if (sw == 0) host_log("plugin: %s: off (plugins.%s=0)", p.name.c_str(), p.name.c_str());
            else host_log("plugin: %s: off until turned on (plugins.%s=1 in bbport.ini)", p.name.c_str(), p.name.c_str());
            {
                std::lock_guard<std::mutex> lk(g_catalog_mu);
                g_catalog.push_back(std::move(entry));
            }
            dlclose(h);
            continue;
        }
        if (p.init && p.init(&g_api) != 0) {
            host_log("plugin: %s: init refused; unloaded", p.name.c_str());
            dlclose(h);
            continue;
        }
        // A plugin the player turned on that says it changes the game may move the player.
        if ((flags & BB_PLUGIN_OPT_IN) && (flags & BB_PLUGIN_GAMEPLAY) && sw == 1) engine::warp_allow();
        host_log("plugin: %s %s loaded%s (%s)", p.name.c_str(), p.info && p.info->version ? p.info->version : "",
                 (flags & BB_PLUGIN_GAMEPLAY) ? ", changes the game" : "", path.c_str());
        g_plugins.push_back(p);
        entry.loaded = true;
        std::lock_guard<std::mutex> lk(g_catalog_mu);
        g_catalog.push_back(std::move(entry));
    }
}

}  // namespace

void plugins_start(unsigned char* image, std::uint64_t size) {
    g_image_base = reinterpret_cast<std::uint64_t>(image);
    engine::set_image(image, size);
    if (const char* e = std::getenv("BB_PLUGINS"); e && e[0] == '0') {
        host_log("plugin: none loaded (BB_PLUGINS=0)");
        engine::image_phase_end();
        return;
    }
    load_all();
    if (!g_plugins.empty()) {
        if (!engine::is_109()) {
            host_log("plugin: not the 1.09 eboot: the plugins get no game hooks");
        } else {
            static const std::uint8_t kFlipperUpdate[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                            0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
            if (place_hook(0x2434770, kFlipperUpdate, sizeof(kFlipperUpdate), &frame_hook, nullptr, nullptr) != 0)
                host_log("plugin: the frame hook did not go in: no on_frame, flag changes or warps");
        }
    }
    for (std::size_t i = 0; i < g_plugins.size();) {
        Plugin& p = g_plugins[i];
        if (p.image && p.image(&g_api) != 0) {
            host_log("plugin: %s: image phase refused; unloaded", p.name.c_str());
            std::lock_guard<std::mutex> lk(g_catalog_mu);
            for (PluginInfo& e : g_catalog) {
                if (e.name == p.name) e.loaded = false;
            }
            // Its hooks and callbacks may be in place already: the library stays mapped.
            g_plugins.erase(g_plugins.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    engine::image_phase_end();
}

std::vector<PluginInfo> plugins_list() {
    std::lock_guard<std::mutex> lk(g_catalog_mu);
    std::vector<PluginInfo> out = g_catalog;
    for (PluginInfo& e : out) {
        const int sw = config_switch(e.name);
        e.enabled = sw == 1 || (sw < 0 && !(e.flags & BB_PLUGIN_OPT_IN));
    }
    return out;
}

std::string plugins_option_value(const std::string& name, const PluginOption& o) {
    const std::string v = config_value(name + "." + o.key);
    return v.empty() ? o.def : v;
}

namespace {
bool running(const std::string& name) {
    for (const Plugin& p : g_plugins) {
        if (p.name == name) return true;
    }
    return false;
}
}  // namespace

void plugins_set_option(const std::string& name, const std::string& key, const std::string& value) {
    config_value_set(name + "." + key, value);
    if (!running(name)) return;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_menu_requests.push_back({false, name, key, value});
}

void plugins_run_action(const std::string& name, const std::string& action) {
    if (!running(name)) return;
    std::lock_guard<std::mutex> lk(g_frame_mu);
    g_menu_requests.push_back({true, name, action, ""});
}

void plugins_set_enabled(const std::string& name, bool on) { config_value_set("plugins." + name, on ? "1" : "0"); }

bool plugins_banner(std::string* text, float* left) {
    std::lock_guard<std::mutex> lk(g_banner_mu);
    const auto now = std::chrono::steady_clock::now();
    if (g_banner.empty() || now >= g_banner_until) return false;
    if (text) *text = g_banner;
    if (left) {
        const float total = std::chrono::duration<float>(g_banner_until - g_banner_from).count();
        *left = total > 0 ? std::chrono::duration<float>(g_banner_until - now).count() / total : 0.0f;
    }
    return true;
}

// The rules this session plays by, for the server (X-BBHost-Ruleset): the loaded plugins that
// change the game, sorted, or "vanilla" - what bbhost sends for the same plugins.
std::string net::plugins_ruleset() {
    std::map<std::string, std::string> own;
    {
        std::lock_guard<std::mutex> lk(g_rules_mu);
        own = g_rules;
    }
    std::vector<std::string> parts;
    for (const Plugin& p : g_plugins) {
        if (!p.info || !(p.info->flags & BB_PLUGIN_GAMEPLAY)) continue;
        if (auto it = own.find(p.name); it != own.end()) {
            parts.push_back(it->second);
            continue;
        }
        std::string token;
        for (const char ch : p.name) token += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
        parts.push_back(token);
    }
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (const std::string& t : parts) out += (out.empty() ? "" : ",") + t;
    return out.empty() ? "vanilla" : out;
}
