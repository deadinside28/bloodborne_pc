// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: from bbhost (github.com/droogie/bbhost, GPL-3.0-or-later), unchanged.
// The STUN Binding exchange, as bytes: a request the host sends
// from its P2P UDP socket to the private server's STUN listener (or any
// public STUN server), and the response that carries the address and port
// the world sees that socket at - the reflexive address the server records
// for other players to reach us behind NAT. No sockets here; hle/net.cpp
// sends and receives on the P2P port so the mapping is that port's.
//
// The request is RFC 5389's shape (the magic cookie leads the transaction
// id), which an RFC 3489 server such as the private server's reads as a
// 16-byte id. The response is taken from MAPPED-ADDRESS (0x0001)
// when present, else XOR-MAPPED-ADDRESS (0x8020 or 0x0020), un-XORed with
// the cookie when the response carries it and with the transaction id's
// own first bytes otherwise (what the private server's does).
#pragma once

#include <cstddef>
#include <cstdint>

namespace net::stun {

constexpr std::size_t kHeader = 20;
constexpr std::size_t kTxid = 16;
constexpr std::size_t kTokenLen = 8;
// The longest request: the header and the relay HELLO (4 + 12).
constexpr std::size_t kMaxRequest = kHeader + 16;

// The private server's relay: a request with
// BBHOST-HELLO (0x8100: "bbr1" + the token it was given, zeros before the
// first) gets a relay port bound to a token, and BBHOST-RELAY (0x8101:
// "bbr1" + token + port + the address the request came from) in the answer.
// The client then sends its datagrams for other relay ports to the STUN
// port framed [0xfb]['R'][token][port u16] and receives the relay's framed
// [0xfb]['r'][source port u16] (hle/net.cpp). A server without the relay
// ignores the attribute.
struct Relay {
    bool present = false;
    std::uint8_t token[kTokenLen] = {};
    std::uint16_t vport = 0;           // host order
    std::uint32_t observed_addr = 0;   // network order: where the request came from
    std::uint16_t observed_port = 0;   // host order
};

// Writes a Binding Request into `out` (kMaxRequest bytes room) and its
// transaction id (the cookie plus 12 random bytes) into `txid`; returns its
// length. `hello` adds BBHOST-HELLO with `token` (zeros when null).
std::size_t build_binding_request(std::uint8_t* out, std::uint8_t txid[kTxid], bool hello = false,
                                  const std::uint8_t* token = nullptr);

// True when [d, d+n) is a Binding Response to `txid` carrying a mapped IPv4
// address: `addr` in network byte order, `port` in host order. `relay`, when
// given, is filled from BBHOST-RELAY (present = false without it).
bool parse_binding_response(const std::uint8_t* d, std::size_t n, const std::uint8_t txid[kTxid], std::uint32_t* addr,
                            std::uint16_t* port, Relay* relay = nullptr);

}  // namespace net::stun
