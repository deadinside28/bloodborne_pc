// SPDX-License-Identifier: GPL-3.0-or-later
// From bbhost's net/http.cpp (GPL-3.0-or-later), over online/http_client.
#include "online/np_http.h"

#include "online/common.h"
#include "online/http_client.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace net {

namespace {

bool parse_reply(const std::string& what, const HttpReply& r, json::Value& out, std::string& error) {
    static std::atomic<int> logs{0};
    if (!r.error.empty()) {
        error = r.error;
    } else if (r.status < 200 || r.status >= 300) {
        error = "HTTP " + std::to_string(r.status);
    } else if (!json::parse(std::string(r.body.begin(), r.body.end()), out, error)) {
        error = "bad JSON: " + error;
    } else {
        return true;
    }
    if (logs.fetch_add(1) < 16) host_log("np: %s -> %s", what.c_str(), error.c_str());
    return false;
}

HttpReply perform(const std::string& method, const std::string& url, const std::string* post, int timeout_ms) {
    std::vector<std::string> headers = {"Accept: application/json", "User-Agent: bbport",
                                        "X-BBHost-Ruleset: " + plugins_ruleset()};
    if (post) headers.push_back("Content-Type: application/json");
    return http_request(method, url, headers, post ? reinterpret_cast<const std::uint8_t*>(post->data()) : nullptr,
                        post ? post->size() : 0, timeout_ms, timeout_ms);
}

}  // namespace

std::string np_server_base() {
    const OnlineConfig& c = config();
    if (!c.np_server.empty()) return c.np_server;
    const std::string scheme = c.online_scheme.empty() ? "http" : c.online_scheme;
    return scheme + "://" + c.online_host + ":18671";
}

bool np_post(const std::string& path, const json::Value& body, json::Value& out, std::string& error, int timeout_ms) {
    const std::string text = json::dump(body, 0);
    return parse_reply("POST " + path, perform("POST", np_server_base() + path, &text, timeout_ms), out, error);
}

bool np_get(const std::string& path, json::Value& out, std::string& error, int timeout_ms) {
    return parse_reply("GET " + path, perform("GET", np_server_base() + path, nullptr, timeout_ms), out, error);
}

std::string str_of(const json::Value& obj, const char* key, const std::string& dflt) {
    const json::Value* v = obj.type == json::Value::Type::Object ? obj.find(key) : nullptr;
    if (!v) return dflt;
    if (v->type == json::Value::Type::String) return v->string;
    if (v->type == json::Value::Type::Number) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v->number));
        return buf;
    }
    return dflt;
}

long long int_of(const json::Value& obj, const char* key, long long dflt) {
    const json::Value* v = obj.type == json::Value::Type::Object ? obj.find(key) : nullptr;
    if (!v) return dflt;
    if (v->type == json::Value::Type::Number) return static_cast<long long>(v->number);
    if (v->type == json::Value::Type::String) return std::strtoll(v->string.c_str(), nullptr, 10);
    if (v->type == json::Value::Type::Bool) return v->boolean ? 1 : 0;
    return dflt;
}

}  // namespace net
