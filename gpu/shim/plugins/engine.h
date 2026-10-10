// SPDX-License-Identifier: GPL-3.0-or-later
// bbport plugins: the game's own things by name, for the plugin API (plugins/host.cpp). Ported
// from bbhost's engine/ (github.com/droogie/bbhost, GPL-3.0-or-later) for the 1.09 eboot:
// event flags, named Lua events, the player's numbers, where the player is and the game's warp,
// and param rows by table and field name. Addresses are Binary Ninja's (vaddr + 0x400000).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// The loaded image (bbgpu_patch_image): where Binary Ninja address `bn` is, 0 outside it.
void set_image(unsigned char* image, std::uint64_t size);
std::uint64_t at(std::uint64_t bn, std::size_t n = 1);
// The image is the 1.09 build every address here means: its code is checked where this
// layer and the plugins use it (bbport loads the PS4 SELF, so no file hash would match bbhost's).
bool is_109();
// Writes code bytes: directly while the image is still being patched, else through
// mprotect (RWX, then back to RX). False when the page cannot be written.
bool code_write(std::uint64_t runtime, const void* bytes, std::size_t n);
void image_phase_end();

// A safe read of the game's memory (false where nothing is mapped).
bool read(std::uint64_t runtime, void* out, std::size_t n);
template <typename T>
bool rd(std::uint64_t at, T* out) {
    return at && read(at, out, sizeof(T));
}
inline std::uint64_t rp(std::uint64_t at) {
    std::uint64_t v = 0;
    return rd(at, &v) ? v : 0;
}

// ---- event flags (SprjEventFlagMan at 0x593b100)
bool event_flag_get(std::uint32_t id, bool* value);
bool event_flag_set(std::uint32_t id, bool value);
// Changes as they happen: the game's SetEventFlag / SetEventFlagValue are replaced by ours
// (bbhost's decomp of them), which note each flipped flag in a ring the main thread drains.
bool event_flags_watch();
struct FlagChange {
    std::uint32_t id, value;
};
std::size_t event_flags_take(FlagChange* out, std::size_t n, std::uint64_t* lost);

// ---- named Lua events, raised on the main thread at the next frame
bool lua_event_queue(const char* name);
void lua_events_tick();

// ---- the player's numbers (GameDataMan at 0x593b130)
bool player_stat_get(const char* name, std::int64_t* value);
bool player_stat_set(const char* name, std::int64_t value);

// ---- the world (WorldChrMan at 0x593e878)
std::uint64_t world_chr_man();
bool player_position(float xyz[3], float* yaw);
bool camera(float pos[3], float focus[3]);
bool player_block(std::uint32_t* block);
// The game's own warp (PreparePlayerWarp), queued for world_tick on the main thread.
bool player_warp_queue(std::uint32_t block, const float xyz[3], float yaw);
bool lamp_warp_queue(std::int32_t return_point);
void world_tick();
// Warps are off until a plugin the player turned on says it changes the game (as on bbhost).
void warp_allow();

// ---- params (SoloParamRepository at 0x5940340) and their definitions (paramdef.paramdefbnd.dcx)
void params_tick();
void* param_row(const char* table, std::uint32_t id, std::size_t* bytes);
bool param_ids(const char* table, std::uint32_t* ids, std::size_t max, std::size_t* count);
bool param_get(const char* table, std::uint32_t id, const char* field, std::string* out);
bool param_set(const char* table, std::uint32_t id, const char* field, const char* value);

}  // namespace engine
