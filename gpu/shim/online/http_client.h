// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: a small blocking HTTP/1.1 client for plain http:// (the private server
// speaks nothing else), used for the game's sceHttp requests and the session's own calls. One
// request per connection; any host thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace net {

struct HttpReply {
    long status = 0;                  // 0 when the transfer failed
    std::vector<std::uint8_t> body;
    std::string error;                // empty on success
    bool timed_out = false;
};

// `headers` are "Name: value" lines; Host, Connection and Content-Length are added here.
HttpReply http_request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                       const std::uint8_t* body, std::size_t body_len, int connect_timeout_ms, int total_timeout_ms);

}  // namespace net
