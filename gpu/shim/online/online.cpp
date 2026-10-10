// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: the imports the online layer answers, by name, when online=1 and
// online.host is set. Everything else (and everything when online is off) falls through to
// the offline console in src/runtime_services.c.
#include "online/common.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" const char* runtime_symbol(const char* nid);

namespace {

std::unordered_map<std::string, void*>& table() {
    static std::unordered_map<std::string, void*> t;
    return t;
}

void add(const char* name, void* fn) { table()[name] = fn; }

bool active() {
    static const bool on = [] {
        const OnlineConfig& c = config();
        if (c.enabled && c.online_host.empty()) host_log("online=1 but online.host is empty: staying offline");
        return c.enabled && !c.online_host.empty();
    }();
    return on;
}

}  // namespace

extern "C" uintptr_t bbgpu_online_resolve(const char* scoped_nid) {
    if (!active()) return 0;
    static std::once_flag once;
    std::call_once(once, [] {
        hle_register_net(add);
        hle_register_http(add);
        hle_register_np_matching2(add);
        hle_register_np_signaling(add);
        hle_register_np_misc(add);
    });
    const char* symbol = runtime_symbol(scoped_nid);
    if (!symbol) return 0;
    const auto it = table().find(symbol);
    return it == table().end() ? 0 : reinterpret_cast<uintptr_t>(it->second);
}
