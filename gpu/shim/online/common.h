// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play and plugins: what the pieces share. The layer follows bbhost's
// (github.com/droogie/bbhost, GPL-3.0-or-later): the game's Net/Http/Np imports against a
// private server (the bbcoop server), and bbhost's plugin API. Off unless bbport.ini says
// online=1; then every co-op setting lives in bbport.ini too (online.*, plugins.*, <plugin>.*).
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

extern "C" {
void* runtime_low_map(size_t size, int prot);       // bb-probe: memory below 1 TiB
void runtime_thread_attach_host(const char* name);  // bb-probe: a guest TCB for this thread
int32_t runtime_guest_errno(int host_errno);        // bb-probe: Linux errno -> FreeBSD's
}

// One log line, "Online: ..." on stdout like the runtime's own lines.
void host_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// bbport.ini (BB_CONFIG): the key's value, "" when it is not there. Read once, kept in memory.
std::string config_value(const std::string& key);
// Sets the key in memory and in the file (the other lines stay as they are).
void config_value_set(const std::string& key, const std::string& value);
bool config_true(const std::string& key, bool fallback);

struct OnlineConfig {
    bool enabled = false;           // online=1
    std::string online_host;        // online.host: the private server, in place of *.scej-network.jp
    std::string online_scheme;      // online.scheme (default http)
    std::string online_id;          // online.name, else the character last played: the name the other player sees
    std::string np_server;          // online.np_server, default <scheme>://<host>:18671
    std::string auth_server;        // online.auth_server
    std::string stun_server;        // online.stun_server, default <host>:3478; "off" disables
    std::string p2p_addr;           // online.p2p_addr, default: this machine's address toward the server
    int p2p_port = 9307;            // online.p2p_port
    bool online_verify_tls = false; // unused: the private server is plain HTTP
    bool online_require_account = false;
};
const OnlineConfig& config();
// With no online.name, the online name is the character last played here (the file
// online-character next to bbport.ini): this notes the loaded character's name there for the
// next start (the game takes its online ID once, at boot).
void character_note();

// The game's code called from ours (the System V ABI of the PS4 and of Linux: a plain call).
// The thread must hold a guest TCB (a game thread, or one made by start_guest_thread).
std::int64_t guest_call6(const void* fn, std::int64_t a0, std::int64_t a1, std::int64_t a2, std::int64_t a3,
                         std::int64_t a4, std::int64_t a5);
template <typename T>
std::int64_t guest_arg(T v) {
    if constexpr (std::is_pointer_v<T>) return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(v));
    else if constexpr (std::is_enum_v<T>) return static_cast<std::int64_t>(v);
    else return static_cast<std::int64_t>(v);
}
template <typename R = std::int64_t, typename... A>
R hle_call_guest(const void* fn, A... a) {
    static_assert(sizeof...(A) <= 6, "six register arguments at most");
    std::int64_t v[6] = {guest_arg(a)...};
    return static_cast<R>(guest_call6(fn, v[0], v[1], v[2], v[3], v[4], v[5]));
}
inline std::int64_t hle_call_guest6(const void* fn, std::int64_t a0, std::int64_t a1, std::int64_t a2, std::int64_t a3,
                                    std::int64_t a4, std::int64_t a5) {
    return guest_call6(fn, a0, a1, a2, a3, a4, a5);
}

// A detached thread that may call the game: its stack below 1 TiB (the game packs stack
// addresses) and a guest TCB. `name` is the thread's name (15 characters).
void start_guest_thread(const char* name, std::function<void()> fn);

// The game's errno numbers are FreeBSD's.
inline int to_freebsd(int e) { return e ? runtime_guest_errno(e) : 0; }

// A safe read of the game's memory (false where nothing is mapped).
bool host_read_safe(const void* at, void* out, std::size_t n);
// A safe data write to it (false where nothing is mapped or the page is read-only, as code is).
bool host_write_safe(void* at, const void* data, std::size_t n);

// The game image: ELF vaddr V lives at g_image_base + V (set by bbgpu_patch_image). Plugins
// name addresses as Binary Ninja does (vaddr + 0x400000).
extern std::uint64_t g_image_base;
constexpr std::uint64_t kBnSlide = 0x400000ull;
inline std::uint64_t bn_runtime(std::uint64_t bn) { return g_image_base + (bn - kBnSlide); }

// online/np_matching2.cpp etc.
using HleRegister = void (*)(const char* name, void* fn);
void hle_register_net(HleRegister reg);
void hle_register_http(HleRegister reg);
void hle_register_np_matching2(HleRegister reg);
void hle_register_np_signaling(HleRegister reg);
void hle_register_np_misc(HleRegister reg);
