// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: from bbhost (github.com/droogie/bbhost, GPL-3.0-or-later).
// Shared between the NP HLE files (system.cpp, np_matching2.cpp, np_signaling.cpp).
#pragma once

#include <cstdint>
#include <string>

// 36-byte SceNpId from an online id: the handle (16 chars + NUL), the rest zero.
void hle_np_fill_npid(void* out, const char* online);
// The handle inside a SceNpId, as text.
std::string hle_np_npid_text(const void* npid);

// One line for the F10 screen: signed in as whom, in which room as which member, how many peers.
std::string hle_np_online_status();
// True once the game has started its Matching2 context (it read its NpId before that,
// so an account logged in afterwards only takes effect on the next launch).
bool hle_np_context_started();

// NpSignaling learns a peer's address: fires ESTABLISHED/ACTIVE for a
// connection that was activated before the peer was known.
void hle_np_signaling_peer_known(std::uint16_t member_id);
// NpMatching2 tells NpSignaling the room is gone.
void hle_np_signaling_reset();
// The peer left or timed out: DEAD (0) on its connection(s), so the game's
// SocketState closes the link instead of waiting on it.
void hle_np_signaling_peer_dead(const std::string& online_id);
