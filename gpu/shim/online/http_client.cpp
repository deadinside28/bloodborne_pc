// SPDX-License-Identifier: GPL-3.0-or-later
#include "online/http_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace net {

namespace {

using Clock = std::chrono::steady_clock;

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left < 0 ? 0 : static_cast<int>(left);
}

bool lower_equal(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (c != b[i]) return false;
    }
    return true;
}

std::string header_name(const std::string& line) {
    std::string n = line.substr(0, line.find(':'));
    n.erase(n.find_last_not_of(" \t") + 1);
    return n;
}

// Decodes a chunked body in place; false when it is cut short.
bool dechunk(std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    for (;;) {
        std::size_t eol = at;
        while (eol + 1 < body.size() && !(body[eol] == '\r' && body[eol + 1] == '\n')) ++eol;
        if (eol + 1 >= body.size()) return false;
        const std::string size_line(body.begin() + static_cast<long>(at), body.begin() + static_cast<long>(eol));
        const unsigned long size = std::strtoul(size_line.c_str(), nullptr, 16);
        at = eol + 2;
        if (size == 0) break;
        if (at + size > body.size()) return false;
        out.insert(out.end(), body.begin() + static_cast<long>(at), body.begin() + static_cast<long>(at + size));
        at += size + 2;
    }
    body.swap(out);
    return true;
}

}  // namespace

HttpReply http_request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                       const std::uint8_t* body, std::size_t body_len, int connect_timeout_ms, int total_timeout_ms) {
    HttpReply r;
    const auto deadline = Clock::now() + std::chrono::milliseconds(total_timeout_ms);
    // scheme://host[:port]/path
    const std::size_t s = url.find("://");
    if (s == std::string::npos) {
        r.error = "not a URL";
        return r;
    }
    const std::string scheme = url.substr(0, s);
    if (scheme != "http") {
        r.error = scheme + " is not supported (the private server is plain http)";
        return r;
    }
    const std::size_t host_at = s + 3;
    std::size_t path_at = url.find('/', host_at);
    if (path_at == std::string::npos) path_at = url.size();
    const std::string authority = url.substr(host_at, path_at - host_at);
    const std::string path = path_at < url.size() ? url.substr(path_at) : "/";
    std::string host = authority, port = "80";
    if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        r.error = "cannot resolve " + host;
        return r;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        r.error = std::strerror(errno);
        return r;
    }
    int connected = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (connected < 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        const int wait = std::min(connect_timeout_ms, ms_left(deadline));
        if (::poll(&p, 1, wait) == 1) {
            int err = 0;
            socklen_t len = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            connected = err ? -1 : 0;
            errno = err;
        } else {
            r.timed_out = true;
            errno = ETIMEDOUT;
        }
    }
    if (connected < 0) {
        r.error = "connect to " + authority + ": " + std::strerror(errno);
        ::close(fd);
        return r;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::string head = method + " " + path + " HTTP/1.1\r\n";
    bool has_host = false;
    for (const std::string& h : headers) {
        const std::string name = header_name(h);
        if (lower_equal(name, "host")) has_host = true;
        if (lower_equal(name, "connection") || lower_equal(name, "content-length") || lower_equal(name, "expect")) continue;
        head += h + "\r\n";
    }
    if (!has_host) head += "Host: " + authority + "\r\n";
    head += "Connection: close\r\n";
    if (body_len || method == "POST" || method == "PUT") head += "Content-Length: " + std::to_string(body_len) + "\r\n";
    head += "\r\n";
    std::vector<std::uint8_t> out(head.begin(), head.end());
    if (body && body_len) out.insert(out.end(), body, body + body_len);
    for (std::size_t sent = 0; sent < out.size();) {
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            r.error = std::string("send: ") + std::strerror(errno);
            ::close(fd);
            return r;
        }
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, ms_left(deadline)) != 1) {
            r.error = "send timed out";
            r.timed_out = true;
            ::close(fd);
            return r;
        }
    }

    // The answer, to the end (Connection: close) or to its declared length.
    std::vector<std::uint8_t> in;
    std::size_t header_end = std::string::npos;
    long long content_length = -1;
    bool chunked = false;
    std::uint8_t buf[16384];
    for (;;) {
        if (header_end != std::string::npos && content_length >= 0 &&
            in.size() - header_end >= static_cast<std::size_t>(content_length))
            break;
        pollfd p{fd, POLLIN, 0};
        const int ready = ::poll(&p, 1, ms_left(deadline));
        if (ready != 1) {
            r.error = "no answer in time";
            r.timed_out = true;
            ::close(fd);
            return r;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) break;  // closed: the end of the answer
        in.insert(in.end(), buf, buf + n);
        if (header_end == std::string::npos) {
            for (std::size_t i = 3; i < in.size(); ++i) {
                if (in[i - 3] == '\r' && in[i - 2] == '\n' && in[i - 1] == '\r' && in[i] == '\n') {
                    header_end = i + 1;
                    break;
                }
            }
            if (header_end != std::string::npos) {
                const std::string text(in.begin(), in.begin() + static_cast<long>(header_end));
                const std::size_t sp = text.find(' ');
                r.status = sp == std::string::npos ? 0 : std::strtol(text.c_str() + sp + 1, nullptr, 10);
                std::size_t at = text.find("\r\n");
                while (at != std::string::npos && at + 2 < text.size()) {
                    const std::size_t end = text.find("\r\n", at + 2);
                    const std::string line = text.substr(at + 2, end - at - 2);
                    const std::string name = header_name(line);
                    const std::size_t colon = line.find(':');
                    const std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
                    if (lower_equal(name, "content-length")) content_length = std::strtoll(value.c_str(), nullptr, 10);
                    if (lower_equal(name, "transfer-encoding") && value.find("chunked") != std::string::npos) chunked = true;
                    at = end;
                }
                if (method == "HEAD" || r.status == 204 || r.status == 304) content_length = 0;
            }
        }
    }
    ::close(fd);
    if (header_end == std::string::npos) {
        r.status = 0;
        r.error = "no HTTP answer";
        return r;
    }
    r.body.assign(in.begin() + static_cast<long>(header_end), in.end());
    if (chunked && !dechunk(r.body)) {
        r.status = 0;
        r.error = "cut-short chunked answer";
        return r;
    }
    if (content_length >= 0 && r.body.size() > static_cast<std::size_t>(content_length))
        r.body.resize(static_cast<std::size_t>(content_length));
    return r;
}

}  // namespace net
