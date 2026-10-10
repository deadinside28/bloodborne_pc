// SPDX-License-Identifier: GPL-3.0-or-later
#include "online/common.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csetjmp>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" __thread sigjmp_buf* runtime_fault_recover;  // bb-probe: its fault handler jumps there

std::uint64_t g_image_base = 0;

void host_log(const char* fmt, ...) {
    char line[1536];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::printf("Online: %s\n", line);
}

namespace {

const char* ini_path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

std::string trim(std::string s) {
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    s.erase(0, s.find_first_not_of(" \t"));
    return s;
}

// A value as the file holds it, without TOML-style quotes ("host" -> host).
std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

std::mutex g_ini_mu;
bool g_ini_loaded = false;
std::map<std::string, std::string> g_ini;  // under g_ini_mu

void ini_load_locked() {
    if (g_ini_loaded) return;
    g_ini_loaded = true;
    FILE* f = std::fopen(ini_path(), "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        const std::string text = trim(line);
        const auto eq = text.find('=');
        if (text.empty() || text[0] == '#' || eq == std::string::npos) continue;
        g_ini[trim(text.substr(0, eq))] = unquote(text.substr(eq + 1));
    }
    std::fclose(f);
}

}  // namespace

std::string config_value(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_ini_mu);
    ini_load_locked();
    auto it = g_ini.find(key);
    return it == g_ini.end() ? std::string() : it->second;
}

bool config_true(const std::string& key, bool fallback) {
    const std::string v = config_value(key);
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

void config_value_set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_ini_mu);
    ini_load_locked();
    g_ini[key] = value;
    // The file's other lines stay (the menu's and the launcher's keys, comments), as
    // BbSettings::Save keeps them.
    std::string out;
    bool written = false;
    if (FILE* f = std::fopen(ini_path(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            std::string text = line;
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos && trim(text.substr(0, eq)) == key) {
                if (!written) out += key + "=" + value + "\n";
                written = true;
                continue;
            }
            out += text + "\n";
        }
        std::fclose(f);
    }
    if (!written) out += key + "=" + value + "\n";
    const std::string tmp = std::string(ini_path()) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    bool ok = f && std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = f && std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), ini_path()) != 0) host_log("cannot write %s", ini_path());
}

namespace {

// The character last played on this machine (character_note), kept next to bbport.ini.
std::string character_file() {
    const std::string ini = ini_path();
    const auto slash = ini.rfind('/');
    return ini.substr(0, slash == std::string::npos ? 0 : slash + 1) + "online-character";
}

// This machine's address on the way to `host` (the interface the kernel routes it through):
// what the other player reaches us at on a LAN. Empty when there is no route.
std::string local_address_toward(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (host.empty() || getaddrinfo(host.c_str(), "18671", &hints, &res) != 0 || !res) return "";
    std::string out;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof(me);
        char text[INET_ADDRSTRLEN] = {};
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&me), &len) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, text, sizeof(text)))
            out = text;
    }
    if (fd >= 0) ::close(fd);
    freeaddrinfo(res);
    return out;
}

}  // namespace

const OnlineConfig& config() {
    static const OnlineConfig c = [] {
        OnlineConfig o;
        o.enabled = config_true("online", false);
        o.online_host = config_value("online.host");
        o.online_scheme = config_value("online.scheme");
        if (o.online_scheme.empty()) o.online_scheme = "http";
        o.online_id = config_value("online.name");
        if (o.online_id.empty()) o.online_id = config_value("online.online_id");
        if (FILE* f = o.online_id.empty() ? std::fopen(character_file().c_str(), "r") : nullptr) {
            char name[64] = {};
            if (std::fgets(name, sizeof(name), f)) o.online_id = trim(name);
            std::fclose(f);
        }
        if (o.online_id.size() > 16) o.online_id.resize(16);
        o.np_server = config_value("online.np_server");
        o.auth_server = config_value("online.auth_server");
        o.stun_server = config_value("online.stun_server");
        o.p2p_addr = config_value("online.p2p_addr");
        if (o.p2p_addr.empty() || o.p2p_addr == "auto") o.p2p_addr = local_address_toward(o.online_host);
        if (const int p = std::atoi(config_value("online.p2p_port").c_str()); p > 0 && p < 65536) o.p2p_port = p;
        o.online_require_account = false;
        if (o.enabled)
            host_log("online play with %s as %s (this machine %s, game port %d)", o.online_host.c_str(),
                     o.online_id.empty() ? "Player" : o.online_id.c_str(), o.p2p_addr.c_str(), o.p2p_port);
        return o;
    }();
    return c;
}

std::int64_t guest_call6(const void* fn, std::int64_t a0, std::int64_t a1, std::int64_t a2, std::int64_t a3,
                         std::int64_t a4, std::int64_t a5) {
    using Fn = std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
    return reinterpret_cast<Fn>(const_cast<void*>(fn))(a0, a1, a2, a3, a4, a5);
}

namespace {

struct ThreadStart {
    std::string name;
    std::function<void()> fn;
};

void* guest_thread_main(void* p) {
    std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(p));
    runtime_thread_attach_host(start->name.c_str());
    char host[16] = {};
    std::snprintf(host, sizeof(host), "%s", start->name.c_str());
    pthread_setname_np(pthread_self(), host);
    start->fn();
    return nullptr;
}

}  // namespace

void start_guest_thread(const char* name, std::function<void()> fn) {
    constexpr std::size_t kStack = 1u << 20;
    auto* start = new ThreadStart{name ? name : "bb-online", std::move(fn)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (void* stack = runtime_low_map(kStack, PROT_READ | PROT_WRITE)) pthread_attr_setstack(&attr, stack, kStack);
    else pthread_attr_setstacksize(&attr, kStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, guest_thread_main, start) != 0) {
        host_log("cannot start thread %s", start->name.c_str());
        delete start;
    }
    pthread_attr_destroy(&attr);
}

namespace {

// A copy that a fault cannot crash: the runtime's fault handler (probe.c) jumps back here instead,
// as for the GPU side's speculative reads. Plain loads and stores, so it reads the guest's direct
// memory in a GPU driver mapping (which process_vm_readv refuses) with no syscall, and a write to
// a GPU-tracked page is seen by the tracking as the game's own writes are. It fails on an unmapped
// page or, writing, a read-only one (code).
bool guarded_copy(void* to, const void* from, std::size_t n) {
    sigjmp_buf* const outer = runtime_fault_recover;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = outer;
        return false;
    }
    runtime_fault_recover = &recover;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(to, from, n);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    runtime_fault_recover = outer;
    return true;
}

}  // namespace

bool host_read_safe(const void* at, void* out, std::size_t n) { return at && out && guarded_copy(out, at, n); }

bool host_write_safe(void* at, const void* data, std::size_t n) { return at && data && guarded_copy(at, data, n); }

void character_note() {
    static const bool automatic = config_value("online.name").empty() && config_value("online.online_id").empty();
    static std::string noted = config().online_id;
    const auto read = [](std::uint64_t at, void* out, std::size_t n) {
        return at && host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), out, n);
    };
    // GameDataMan (0x593b130) -> PlayerGameData +8 -> the name +0xa8, 16 UTF-16 characters.
    std::uint64_t man = 0, player = 0;
    std::uint16_t w[16] = {};
    if (!automatic || !read(bn_runtime(0x593b130), &man, 8) || !read(man + 8, &player, 8) || !read(player + 0xa8, w, sizeof(w)))
        return;
    std::string name;  // as an online ID has it: ASCII letters, digits, - and _
    for (const std::uint16_t c : w) {
        if (!c) break;
        name += c < 128 && (std::isalnum(c) || c == '-' || c == '_') ? static_cast<char>(c) : '_';
    }
    if (name.empty() || name == noted) return;
    const std::string path = character_file(), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    bool ok = f && std::fprintf(f, "%s\n", name.c_str()) > 0;
    ok = f && std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        host_log("cannot write %s", path.c_str());
        return;
    }
    noted = name;
    host_log("online name from the next start: %s (the character played here)", name.c_str());
}
