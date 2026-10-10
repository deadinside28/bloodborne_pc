// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: the session's own calls to the private server's matchmaking API
// (from bbhost's net/http.h, over online/http_client instead of libcurl). There are no
// accounts here: the private server takes every name.
#pragma once

#include "online/json.h"

#include <string>

namespace net {

// online.np_server, else "<online.scheme>://<online.host>:18671".
std::string np_server_base();

// POST/GET to "<np_server_base><path>" and parse the JSON reply. False (with `error`) on a
// transport error, a non-2xx status or a body that is not JSON.
bool np_post(const std::string& path, const json::Value& body, json::Value& out, std::string& error,
             int timeout_ms = 5000);
bool np_get(const std::string& path, json::Value& out, std::string& error, int timeout_ms = 5000);

// Reads that never throw: the member or a default.
std::string str_of(const json::Value& obj, const char* key, const std::string& dflt = "");
long long int_of(const json::Value& obj, const char* key, long long dflt = 0);

// bbhost's accounts, as a private server without them sees it.
inline std::string account_name() { return {}; }
inline std::string account_token() { return {}; }
inline bool account_logged_in() { return false; }
inline bool account_refused() { return false; }

// The rules this game plays by, as bbhost tells its server (X-BBHost-Ruleset): "vanilla", or
// the loaded plugins that change the game ("bbcoop"), from plugins/host.cpp.
std::string plugins_ruleset();

}  // namespace net
