// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: the rest of the NP libraries as an online player needs them, from bbhost's
// hle/system.cpp (github.com/droogie/bbhost, GPL-3.0-or-later): signed in as online.name, Plus
// and parental checks passed, auth codes, an empty friend list, no leaderboards (only the
// text filter).
#include "online/common.h"
#include "online/np.h"
#include "online/session.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr int kNpInvalidArg = static_cast<int>(0x80550003);

// ---- NpManager
int hle_np_reg_state(void*, void*) { return 0; }
int hle_np_unreg_state(void*) { return 0; }
int hle_np_reg_presence(void*, void*) { return 0; }
int hle_np_get_state(int, int* state) {
    if (!state) return kNpInvalidArg;
    *state = 2;  // SIGNED_IN
    return 0;
}
int hle_np_online_id(int, void* id) {
    if (!id) return kNpInvalidArg;
    const std::string cur = net::online_id();
    std::memset(id, 0, 20);
    std::memcpy(id, cur.c_str(), std::min<std::size_t>(cur.size(), 16));
    return 0;
}
int hle_np_get_npid(int, void* id) {
    if (!id) return kNpInvalidArg;
    // The same handle as sceNpGetOnlineId: the session service matches peers by it.
    hle_np_fill_npid(id, net::online_id().c_str());
    return 0;
}
int hle_np_presence_status(int, int* st) {
    if (st) *st = 1;
    return 0;
}
int hle_np_cmp_online(const void* a, const void* b) {
    if (!a || !b) return kNpInvalidArg;
    return std::memcmp(a, b, 16) ? static_cast<int>(0x80550609) : 0;
}
int hle_np_cmp_npid(const void* a, const void* b) {
    if (!a || !b) return kNpInvalidArg;
    return std::memcmp(a, b, 16) ? static_cast<int>(0x80550609) : 0;
}
// A no-op, as on bbhost and shadPS4: sceNpGetState already says signed in.
int hle_np_check_cb() { return 0; }

// ---- availability, Plus, parental, auth: passed locally (no PSN behind the private server)
std::atomic<int> g_np_req{1};
int hle_np_create_async_request(const void*) { return g_np_req.fetch_add(1); }
int hle_np_delete_request(int) { return 0; }
int hle_np_abort_request(int) { return 0; }
int hle_np_poll_async(int, int* result) {
    if (result) *result = 0;
    return 0;  // finished
}
int hle_np_check_availability(int, const void*, void*) { return 0; }
int hle_np_parental(int, const void*, std::int8_t* age, std::uint8_t* info) {
    if (age) *age = 25;
    if (info) std::memset(info, 0, 3);
    return 0;
}
int hle_np_check_plus(int, const void*, std::uint8_t* result) {
    if (result) result[0] = 1;
    return 0;
}
int hle_np_plus_cb(const void*, void*) { return 0; }
int hle_np_plus_notify(int, std::uint64_t) { return 0; }
int hle_npauth_create_async_request(const void*) { return g_np_req.fetch_add(1); }
int hle_npauth_delete_request(int) { return 0; }
int hle_npauth_poll_async(int, int* result) {
    if (result) *result = 0;
    return 0;
}
int hle_npauth_get_code(int, const void*, char* code, int* issuer) {
    if (code) {
        std::memset(code, 0, 128);
        std::strncpy(code, "DUMMY", 127);
    }
    if (issuer) *issuer = 10;
    return 0;
}

// ---- NpWebApi: friend and block lists, empty
constexpr int kWebApiInvalid = static_cast<int>(0x80552902);
constexpr int kWebApiNoReq = static_cast<int>(0x80552906);
constexpr char kWebApiJsonType[] = "application/json; charset=utf-8";
struct WebApiReq {
    int ctx = 0;
    int status = 200;
    std::string body;
    std::size_t read_off = 0;
};
std::mutex g_web_mu;
int g_web_lib = 1, g_web_ctx = 1, g_web_filter = 1, g_web_cb = 1;
std::int64_t g_web_req = 1;
std::unordered_map<std::int64_t, WebApiReq> g_web_reqs;

std::string webapi_body_for_path(const char* path) {
    if (path && std::strstr(path, "friendList")) return "{\"totalResults\":0,\"friendList\":[]}";
    if (path && std::strstr(path, "blockList")) return "{\"totalResults\":0,\"blockList\":[]}";
    return "{\"totalResults\":0}";
}
int hle_webapi_init(int, std::uint64_t) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_lib++;
}
int hle_webapi_term(int) { return 0; }
int hle_webapi_create_ctx(int, const void*) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_ctx++;
}
int hle_webapi_delete_ctx(int) { return 0; }
int hle_webapi_create_filter(int, const void*, std::uint64_t) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_filter++;
}
int hle_webapi_delete_filter(int, int) { return 0; }
int hle_webapi_reg_push(int, int, void*, void*) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_cb++;
}
int hle_webapi_unreg_push(int, int) { return 0; }
int hle_webapi_create_req(int ctx, const char*, const char* path, int, const void*, std::int64_t* req_id) {
    if (!path || !req_id) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    const std::int64_t id = g_web_req++;
    WebApiReq r{};
    r.ctx = ctx;
    r.body = webapi_body_for_path(path);
    g_web_reqs[id] = std::move(r);
    *req_id = id;
    return 0;
}
int hle_webapi_delete_req(std::int64_t req) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    g_web_reqs.erase(req);
    return 0;
}
int hle_webapi_abort_req(std::int64_t req) { return hle_webapi_delete_req(req); }
int hle_webapi_send(std::int64_t req, const void*, std::uint64_t) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_reqs.count(req) ? 0 : kWebApiNoReq;
}
int hle_webapi_status(std::int64_t req, int* code) {
    if (!code) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    *code = it->second.status;
    return 0;
}
int hle_webapi_read(std::int64_t req, void* data, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    WebApiReq& r = it->second;
    if (!data || size == 0 || r.read_off >= r.body.size()) return 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), r.body.size() - r.read_off);
    std::memcpy(data, r.body.data() + r.read_off, n);
    r.read_off += n;
    return static_cast<int>(n);
}
int hle_webapi_hdr_len(std::int64_t req, const char* name, std::uint64_t* len) {
    if (!name || !len) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (std::strcmp(name, "Content-Type") == 0 || std::strcmp(name, "content-type") == 0) {
        *len = sizeof(kWebApiJsonType);
        return 0;
    }
    return kWebApiInvalid;
}
int hle_webapi_hdr_val(std::int64_t req, const char* name, char* buf, std::uint64_t n) {
    if (!name || !buf || n == 0) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (std::strcmp(name, "Content-Type") != 0 && std::strcmp(name, "content-type") != 0) return kWebApiInvalid;
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", kWebApiJsonType);
    return 0;
}
int hle_webapi_parse_npid(const char* json, void* npid) {
    if (!json || !npid) return kWebApiInvalid;
    const char* s = json;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    char online[17]{};
    auto take_quoted = [&](const char* p) -> bool {
        const char* q = std::strchr(p, '"');
        if (!q) return false;
        ++q;
        const char* e = std::strchr(q, '"');
        if (!e) return false;
        const std::size_t n = static_cast<std::size_t>(e - q);
        if (n == 0 || n >= sizeof(online)) return false;
        std::memcpy(online, q, n);
        online[n] = 0;
        return true;
    };
    if (*s == '{') {
        const char* key = std::strstr(s, "onlineId");
        if (!key) key = std::strstr(s, "\"data\"");
        if (!key || !take_quoted(key)) return kWebApiInvalid;
    } else if (*s == '"') {
        if (!take_quoted(s)) return kWebApiInvalid;
    } else {
        std::snprintf(online, sizeof(online), "%s", s);
    }
    hle_np_fill_npid(npid, online);
    return 0;
}

// ---- NpLookup
std::mutex g_lookup_mu;
int g_lookup_ctx = 1, g_lookup_req = 1;
int hle_lookup_title_ctx(const void*) {
    std::lock_guard<std::mutex> lock(g_lookup_mu);
    return g_lookup_ctx++;
}
int hle_lookup_delete_ctx(int) { return 0; }
int hle_lookup_async(int, const void*) {
    std::lock_guard<std::mutex> lock(g_lookup_mu);
    return g_lookup_req++;
}
int hle_lookup_delete_req(int) { return 0; }
int hle_lookup_abort(int) { return 0; }
int hle_lookup_poll(int, int* result) {
    if (result) *result = 0;
    return 0;
}
int hle_lookup_npid(int, const void* online, void* npid, void*) {
    if (!online || !npid) return kNpInvalidArg;
    hle_np_fill_npid(npid, static_cast<const char*>(online));
    return 0;
}

// ---- NpScore text filter: names and comments pass as they are. The game checks the character
// name with it once in the world, and a failed check reads as a lost connection (back to title).
constexpr std::size_t kScoreCommentMax = 255;
int hle_score_censor(int, const char* comment, void*) { return comment ? 0 : kNpInvalidArg; }
int hle_score_sanitize(int, const char* comment, char* sanitized, void*) {
    if (!comment || !sanitized) return kNpInvalidArg;
    const std::size_t n = strnlen(comment, kScoreCommentMax);
    std::memmove(sanitized, comment, n);
    sanitized[n] = 0;
    return 0;
}

}  // namespace

void hle_register_np_misc(HleRegister reg) {
#define REG(name, fn) reg(name, reinterpret_cast<void*>(fn))
    REG("sceNpRegisterStateCallback", hle_np_reg_state);
    REG("sceNpUnregisterStateCallback", hle_np_unreg_state);
    REG("sceNpRegisterGamePresenceCallback", hle_np_reg_presence);
    REG("sceNpGetState", hle_np_get_state);
    REG("sceNpCheckCallback", hle_np_check_cb);
    REG("sceNpCreateAsyncRequest", hle_np_create_async_request);
    REG("sceNpDeleteRequest", hle_np_delete_request);
    REG("sceNpAbortRequest", hle_np_abort_request);
    REG("sceNpPollAsync", hle_np_poll_async);
    REG("sceNpCheckNpAvailability", hle_np_check_availability);
    REG("sceNpGetParentalControlInfo", hle_np_parental);
    REG("sceNpCheckPlus", hle_np_check_plus);
    REG("sceNpRegisterPlusEventCallback", hle_np_plus_cb);
    REG("sceNpUnregisterPlusEventCallback", hle_np_plus_cb);
    REG("sceNpNotifyPlusFeature", hle_np_plus_notify);
    REG("sceNpAuthCreateAsyncRequest", hle_npauth_create_async_request);
    REG("sceNpAuthDeleteRequest", hle_npauth_delete_request);
    REG("sceNpAuthPollAsync", hle_npauth_poll_async);
    REG("sceNpAuthGetAuthorizationCode", hle_npauth_get_code);
    REG("sceNpGetOnlineId", hle_np_online_id);
    REG("sceNpGetNpId", hle_np_get_npid);
    REG("sceNpGetGamePresenceStatus", hle_np_presence_status);
    REG("sceNpCmpOnlineId", hle_np_cmp_online);
    REG("sceNpCmpNpId", hle_np_cmp_npid);
    REG("sceNpWebApiInitialize", hle_webapi_init);
    REG("sceNpWebApiTerminate", hle_webapi_term);
    REG("sceNpWebApiCreateContext", hle_webapi_create_ctx);
    REG("sceNpWebApiDeleteContext", hle_webapi_delete_ctx);
    REG("sceNpWebApiCreatePushEventFilter", hle_webapi_create_filter);
    REG("sceNpWebApiDeletePushEventFilter", hle_webapi_delete_filter);
    REG("sceNpWebApiRegisterPushEventCallback", hle_webapi_reg_push);
    REG("sceNpWebApiUnregisterPushEventCallback", hle_webapi_unreg_push);
    REG("sceNpWebApiCreateRequest", hle_webapi_create_req);
    REG("sceNpWebApiDeleteRequest", hle_webapi_delete_req);
    REG("sceNpWebApiAbortRequest", hle_webapi_abort_req);
    REG("sceNpWebApiSendRequest", hle_webapi_send);
    REG("sceNpWebApiGetHttpStatusCode", hle_webapi_status);
    REG("sceNpWebApiReadData", hle_webapi_read);
    REG("sceNpWebApiGetHttpResponseHeaderValueLength", hle_webapi_hdr_len);
    REG("sceNpWebApiGetHttpResponseHeaderValue", hle_webapi_hdr_val);
    REG("sceNpWebApiUtilityParseNpId", hle_webapi_parse_npid);
    REG("sceNpLookupCreateTitleCtx", hle_lookup_title_ctx);
    REG("sceNpLookupDeleteTitleCtx", hle_lookup_delete_ctx);
    REG("sceNpLookupCreateAsyncRequest", hle_lookup_async);
    REG("sceNpLookupDeleteRequest", hle_lookup_delete_req);
    REG("sceNpLookupAbortRequest", hle_lookup_abort);
    REG("sceNpLookupPollAsync", hle_lookup_poll);
    REG("sceNpLookupNpId", hle_lookup_npid);
    REG("sceNpScoreCensorComment", hle_score_censor);
    REG("sceNpScoreSanitizeComment", hle_score_sanitize);
#undef REG
}
