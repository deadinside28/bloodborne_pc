// SPDX-License-Identifier: MIT
// Every address, byte patch and flag bbcoop uses, for the Bloodborne 1.09
// eboot (US CUSA00900 and EU CUSA03173 share it, SHA-256 d65f0b4f...; the
// decrypted ELF bbhost runs is 941f887a...).
//
// Addresses are ELF virtual addresses (image base 0), as Ghidra and
// Wozzardman's shadp2p notes give them; bbhost's API wants Binary Ninja
// addresses, which are these plus kBnSlide. tools/verify.cpp checks every
// `expect` and `prologue` below against the ELF.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace addrs {

constexpr uint64_t kBnSlide = 0x400000;

struct Patch {
    uint64_t va;
    uint8_t n;
    uint8_t expect[16];
    uint8_t bytes[16];
    const char* what;
};

// --- 2. Summon anywhere ------------------------------------------------------
// Wozzardman's 12 (shadp2p src/core/bloodborne_re.cpp, documents/
// bloodborne-seamless-re.md): the bells' area checks take a negative SOS
// area (the area's boss is dead) as allowed, and the summon candidate and
// request builder accept a partner on another map.
constexpr Patch kSummonAnywhere[] = {
    {0x0157F8C8, 6, {0x0F, 0x87, 0x90, 0x00, 0x00, 0x00}, {0x0F, 0x8F, 0x90, 0x00, 0x00, 0x00},
     "Beckoning Bell area range: unsigned ja -> signed jg"},
    {0x0157F960, 2, {0x34, 0x01}, {0x31, 0xC0}, "Beckoning Bell area flag: blocked -> allowed"},
    {0x0157F6D1, 2, {0x88, 0xC3}, {0xB1, 0x01}, "Small/Sinister Bell area result: cl = 1"},
    {0x0157F6D3, 8, {0x4C, 0x89, 0xF7, 0xE8, 0xF5, 0x9F, 0x34, 0x00}, {0xE9, 0xE9, 0x09, 0x00, 0x00, 0x90, 0x90, 0x90},
     "Small/Sinister Bell availability: jump to the checked epilogue"},
    {0x015068BB, 2, {0x77, 0x4A}, {0x7F, 0x4A}, "active bell area range: ja -> jg"},
    {0x015068FC, 5, {0x88, 0xC3, 0x80, 0xF3, 0x01}, {0xB3, 0x01, 0x90, 0x90, 0x90}, "active bell area flag: normal indicator"},
    {0x0191A8C3, 6, {0x18, 0xC9, 0x20, 0xC1, 0xEB, 0x02}, {0x7D, 0x04, 0x88, 0xC1, 0xEB, 0x02},
     "responder search area range: signed jge"},
    {0x018700D3, 16, {0x84, 0xC0, 0x41, 0xBD, 0xFF, 0xFF, 0xFF, 0xFF, 0x44, 0x0F, 0x44, 0xEB, 0x41, 0xC1, 0xED, 0x1F},
     {0x45, 0x31, 0xED, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "SOS status area restriction -> 0"},
    {0x014B714A, 6, {0x0F, 0x85, 0xC0, 0x03, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "summon candidate: other map allowed"},
    {0x01874B79, 2, {0x74, 0x0D}, {0xEB, 0x0D}, "summon request builder: other map allowed"},
    {0x018749E8, 6, {0x0F, 0x85, 0x48, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "summon request builder: world-state check off"},
    {0x018749F0, 6, {0x0F, 0x88, 0x40, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "summon request builder: negative-area event check off"},
};

// --- 3. No kick on a boss kill -------------------------------------------------
// The boss-clear chain BlockClear2 -> _1 -> _2 -> _3 / _3Leave (the game's
// compiled global_event.lua). _3 sends a white phantom (chr_type 1) home:
// its `cmp [player+0x78], 1; jne epilogue` becomes `jmp epilogue`, so it
// still clears the input block it starts with. _3Leave stops the session and
// tears down the remote players: it returns at once. A phantom's BlockClear2
// also marks the event system "leaving" (+0x8D), which the event update
// (0x01339B50) copies each frame to SprjActionButtonMan+0x52: no action
// button (no pickups, no lamps) until the world reloads. It stores 0 instead.
// _1 shows a phantom "returning to your world" (RecallMenuEvent_MisssionClear_
// forNormalCoop): skipped. _3 clears the boss-clear flags 4047 and 4000 for
// the host (chr_type 0 or 8): its `jne` past that, made to jump nowhere,
// clears them for her too. One byte each, so the switch can flip while the
// game runs.
constexpr Patch kKeepAfterBoss[] = {
    {0x0138599C, 1, {0x75}, {0xEB}, "BlockClear2_3: phantom not sent home"},
    {0x013858A0, 1, {0x55}, {0xC3}, "BlockClear2_3Leave: session kept"},
    {0x01384D0A, 1, {0x01}, {0x00}, "BlockClear2: phantom not marked leaving (action button kept)"},
    {0x013855B7, 1, {0x75}, {0xEB}, "BlockClear2_1: no \"returning to your world\""},
    {0x0138596E, 1, {0x1B}, {0x00}, "BlockClear2_3: boss-clear flags cleared for a phantom too"},
};

// A phantom's BlockClear2 gives her SpEffect 4680 (+1 Insight) for helping;
// with Insight shared she gets the host's boss Insight already, so its block's
// `je` past it (no player) becomes `jmp`.
constexpr Patch kHelperInsight[] = {
    {0x0138527A, 1, {0x74}, {0xEB}, "BlockClear2: no helper's Insight (the host's is shared)"},
};

// --- 3b. Lamps while together ----------------------------------------------------
// A lamp ("chair" inside the game) goes dark while anyone else is in the world, by
// one test in two places, both "session members > 0 || players > 1" ending in
// "or cl, al": the lamp's own update (0x012F5C40) stores it as the lamp's
// multiplayer byte (+0x49), which leaves the lamp registry's usable byte 0, and the
// lamp's talk script (ActionButtonParam 6100 Light Lamp, 6101/6102 Return to the
// Hunter's Dream) asks talk command 0x34 (the talk commands 0x0203AD60, this case at
// 0x0203C505), which otherwise only the Hunter's Dream's own scripts ask. Made
// "xor cl, cl", both answer "alone".
constexpr Patch kLamps[] = {
    {0x012F831E, 5, {0x0F, 0x9F, 0xC1, 0x08, 0xC1}, {0x0F, 0x9F, 0xC1, 0x30, 0xC9}, "lamp update: not multiplayer"},
    {0x0203C561, 5, {0x0F, 0x9F, 0xC1, 0x08, 0xC1}, {0x0F, 0x9F, 0xC1, 0x30, 0xC9}, "talk command 0x34: not multiplayer"},
};

// --- 3c. The guest talks too ------------------------------------------------------
// Every talk script (NPCs, lamps, shops) asks "not a guest" before it answers a press:
// talk function 19 (the talk functions 0x0203AD60, this case at 0x0203BA66) reads the
// talk context's byte +0x25, which 0x0203ABB0 sets when the main player's chr_type is
// neither 0 nor 8 - a phantom in someone's world. So the guest saw a lamp's prompt
// and her press did nothing. Its one reader made "xor eax, eax; nop": never a guest.
constexpr Patch kGuestTalk[] = {
    {0x0203BA78, 5, {0x41, 0x0F, 0xB6, 0x47, 0x25}, {0x31, 0xC0, 0x0F, 0x1F, 0x00}, "talk function 19: never a guest"},
};

// Every lamp, from the maps' own events (each .emevd of the 1.09 dump starts common
// event 7200 once per lamp; the DLC's three maps included): its map as the flags
// write it (241: m24_01), its number there, and the Hunter's Dream return point
// (ReturnPointParam) it goes back to. Its flags follow from those two, alike in all
// 44 (the lamp talk scripts' talk command 11, and their healing-fountain flags in
// event 7000): "Return to the Hunter's Dream" turns on 7<map>0100+n, which event
// 7200 waits on - and which ends at once in a guest's game, so nothing takes her
// home; "Light Lamp" turns on 7<map>0200+n, which event 7100 waits on to light it;
// lit is 1<map>7810+20n (the healing-fountain flag + 10), which the lamp's talk
// script and the Dream's headstones read. Event 7200 goes to the lamp's return
// point once flag 9401 is on, to 2102962 before.
struct Lamp {
    uint16_t map;
    uint8_t n;
    int32_t dream;
};
constexpr Lamp kLampList[] = {
    {211, 0, 2102952}, {220, 0, 2102951}, {220, 1, 2102951}, {230, 0, 2102950}, {230, 1, 2102950},
    {230, 2, 2102950}, {240, 0, 2102950}, {240, 1, 2102950}, {241, 0, 2102950}, {241, 1, 2102950},
    {241, 2, 2102950}, {241, 3, 2102950}, {242, 0, 2102950}, {242, 1, 2102950}, {242, 2, 2102950},
    {250, 0, 2102952}, {250, 1, 2102952}, {250, 2, 2102952}, {260, 0, 2102953}, {260, 1, 2102953},
    {260, 2, 2102953}, {260, 3, 2102953}, {270, 0, 2102951}, {270, 1, 2102951}, {280, 0, 2102952},
    {280, 1, 2102952}, {280, 2, 2102952}, {280, 3, 2102953}, {320, 0, 2102951}, {320, 1, 2102953},
    {320, 2, 2102951}, {320, 3, 2102953}, {330, 0, 2102953}, {330, 1, 2102953}, {340, 0, 2102953},
    {340, 1, 2102953}, {340, 2, 2102953}, {340, 3, 2102953}, {350, 0, 2102953}, {350, 1, 2102953},
    {350, 2, 2102953}, {360, 0, 2102953}, {360, 1, 2102953}, {360, 2, 2102953},
};
constexpr uint32_t lamp_return_flag(const Lamp& l) { return 70000100u + l.map * 10000u + l.n; }
constexpr uint32_t lamp_light_flag(const Lamp& l) { return 70000200u + l.map * 10000u + l.n; }
constexpr uint32_t lamp_lit_flag(const Lamp& l) { return 10007810u + l.map * 10000u + 20u * l.n; }
// The lamp's map as World::map >> 16 packs it (area, block): 241 -> 0x1801.
constexpr uint32_t lamp_map(const Lamp& l) { return (l.map / 10u) << 8 | l.map % 10u; }
constexpr uint32_t kDreamReturnFlag = 9401;
constexpr int32_t kDreamReturnFirst = 2102962;
constexpr uint32_t kDreamMap = 0x1500;  // m21_00, as lamp_map packs it

// --- 4. The guest picks up too ---------------------------------------------------
// A joined guest's game places no map treasure: the code placing a map's
// treasure (0x017DC300) returns while joined (state 6), while connecting
// (GameStateMan+0x16F8 -> +0x98 not -1) and for a player type its table
// (0x047348A0, 12 bytes a type, byte 2; the type is PlayerGameData+0xA4) does
// not let have items - a summoned hunter - and InsertTreasureItem
// (0x017D4FB0) gives an item no id while joined or connecting. On joining,
// the host's game sends item packet 0x2C op 1 with -1 and hers clears every
// map item list (MapItemMan update 0x017D2F30), the treasure of the area she
// joined in with them; op 1 with 3 clears the treasure alone. Without the
// five jumps and the two treasure clears she has her own copy of the
// world's items, picked up as at home (the removal, 0x017D6780, still sets no
// flag and runs no event for a guest; the plugin marks them taken in her own
// world). Only the guest's machine applies it; it counts from the next load.
constexpr Patch kGuestPickups[] = {
    {0x017DC3D6, 6, {0x0F, 0x84, 0xA5, 0x0C, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "map treasure placed while joined"},
    {0x017DC3E3, 6, {0x0F, 0x85, 0x98, 0x0C, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "map treasure placed while connecting"},
    {0x017DC418, 6, {0x0F, 0x84, 0x63, 0x0C, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
     "map treasure placed for a summoned player's type"},
    {0x017D52D9, 2, {0x74, 0x26}, {0x90, 0x90}, "InsertTreasureItem: an id while joined"},
    {0x017D52E6, 2, {0x75, 0x1C}, {0x90, 0x90}, "InsertTreasureItem: an id while connecting"},
    {0x017D34A0, 5, {0xE8, 0x9B, 0x26, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90},
     "item sync from the host: map treasure kept when all is cleared"},
    {0x017D40AB, 5, {0xE8, 0x90, 0x1A, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90},
     "item sync from the host: map treasure not cleared"},
};

// --- 5. True seamless (experimental) -------------------------------------------
// Stage loads end the session through CSMultiPlayMan::Stop (0x01ED07A0).
// Each call becomes a call to the class's "matching exists" predicate
// (0x01ED00A0), which follows Stop's success branch without leaving the room
// (Wozzardman's validated retarget of 0x019471B1, applied to
// RequestStageTransition's own Stop as well).
constexpr Patch kSeamless[] = {
    {0x019471B1, 5, {0xE8, 0xEA, 0x95, 0x58, 0x00}, {0xE8, 0xEA, 0x8E, 0x58, 0x00}, "stage-transition destructor keeps the room"},
    {0x013CDEF6, 5, {0xE8, 0xA5, 0x28, 0xB0, 0x00}, {0xE8, 0xA5, 0x21, 0xB0, 0x00}, "RequestStageTransition keeps the room"},
};

// --- Functions bbcoop calls or hooks, with their first bytes -------------------
struct Function {
    uint64_t va;
    uint8_t n;
    uint8_t prologue[20];
    const char* what;
};

constexpr Function kUseItem = {0x018F9720, 16,
                               {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC},
                               "goods apply (player, goods id, 17)"};
constexpr Function kSetForcedMap = {0x0156CF10, 16,
                                    {0x48, 0x8B, 0x05, 0x61, 0x97, 0xFE, 0x03, 0x8B, 0x0F, 0x89, 0x88, 0xF0, 0x14, 0x00, 0x00, 0xC3},
                                    "forced summon map (const u32*)"};
constexpr Function kSetForcedPosition = {0x0156CF20, 16,
                                         {0x48, 0x8B, 0x05, 0x51, 0x97, 0xFE, 0x03, 0xC5, 0xF8, 0x28, 0x07, 0xC5, 0xF8, 0x29, 0x80, 0x00},
                                         "forced summon position (const vec4*, 16-aligned)"};
constexpr Function kSetForcedOrientation = {0x0156CF40, 16,
                                            {0x48, 0x8B, 0x05, 0x31, 0x97, 0xFE, 0x03, 0xC5, 0xF8, 0x28, 0x07, 0xC5, 0xF8, 0x29, 0x80, 0x10},
                                            "forced summon orientation (const vec4*, 16-aligned)"};
constexpr Function kSetForcedWarp = {0x0156CF60, 15,
                                     {0x48, 0x8B, 0x05, 0x11, 0x97, 0xFE, 0x03, 0xC6, 0x80, 0x20, 0x15, 0x00, 0x00, 0x01, 0xC3},
                                     "forced summon warp flag"};
constexpr Function kSelectPlacement = {0x01332BC0, 13,
                                       {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x18},
                                       "SelectNextMapPlacement (0, -1)"};
constexpr Function kStageTransition = {0x013CDE30, 14,
                                       {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
                                       "RequestStageTransition ()"};
// Hooked (bbhost hook2: 16 whole instructions, none rip-relative).
constexpr Function kOnMatchingCheck = {0x013808B0, 16,
                                       {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xD3, 0x49, 0x89, 0xF6},
                                       "SprjSessionManager OnMatchingCheck"};

// Elden Ring style deaths and shared boss drops.
constexpr Function kForceDead = {0x013320C0, 15,
                                 {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x05, 0xFD, 0x8F, 0x20, 0x04, 0x48, 0x83, 0x38, 0x00},
                                 "Lua ForceDead (unused, entity id): the character dies"};
constexpr Function kChrSetHp = {0x018BCD40, 15,
                                {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x10},
                                "ChrIns set HP (chr, hp), as Lua SetHp"};
// Hooked too: every item lot the game gives goes through it - the event
// scripts' Award Item Lot (2003:4, its one call at 0x017C1806, a boss's drop
// and an NPC's gift once its talk set the event's flag), Lua's GetRateItem
// helpers (0x0132E480/0x0132E4A0) and the role lots below. The third argument
// 1 skips it while joined.
constexpr Function kAwardItemLot = {0x017DDC50, 20,
                                    {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                     0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0xA8, 0x01, 0x00, 0x00},
                                    "ItemLotMan award (man, lot, 0), as Lua GetRateItem_IgnoreMultiPlay"};
// Lots the game gives each player for its part, not for the world: the boss
// clear's for helping or being helped (ev_BlockClear2: 5900-5950) and PvP's
// (ev_DeadInfoMsg_Host / _ForceJoinBlack, ev_TextEffectEnd_PK_Success /
// _NPCPK_Success). Not shared.
constexpr int32_t kRoleLots[] = {5020, 5500, 5510, 5600, 5610, 5700, 5710, 5800,
                                 5810, 5900, 5910, 5920, 5930, 5940, 5950, 16581};

// Hooked: the boss-clear Lua event (ev_BlockClear2, section 3), which each
// machine runs as a boss dies, from its defeat event's Handle Boss Defeat -
// the boss's defeat flag comes on as that event ends, past the setters the
// flag watch hears.
constexpr Function kBlockClear = {0x013849A0, 17,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                   0x83, 0xEC, 0x18},
                                  "boss-clear Lua event BlockClear2 (event, global event, args)"};

// Hooked: fills what an enemy's search sees of a character (module +0x40 of
// ChrIns+0x3B0, record). Record +0x64 nonzero = unseen (Player Hide sets it,
// for the main player only); the caller resets the record for each character.
constexpr Function kSightRecord = {0x01A55390, 20,
                                   {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x89, 0xF3, 0x4C,
                                    0x8B, 0x4F, 0x08, 0x4D, 0x8B, 0x81, 0xC8, 0x01, 0x00, 0x00},
                                   "enemy sight record (chr module, record)"};

// Hooked and called: a player picks up a map item (MapItemMan, const u32* id);
// the pick-up action (0x012FD880) is its only caller. A local item is removed
// and its contents given to the caller's player.
constexpr Function kPickUp = {0x017D8570, 20,
                              {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                               0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00},
                              "MapItemMan pick up (man, const u32* id)"};

// Called: removes a map item (MapItemMan, const u32* id, notify). With notify
// 0 nothing is given and no flag is set; a guest runs none of its events.
constexpr Function kRemoveItem = {0x017D6780, 20,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                   0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x41, 0x89, 0xD7},
                                  "MapItemMan remove (man, const u32* id, notify)"};

// Picking a downed partner up.
// Called each frame the prompt should show (SprjActionButtonMan, owner key,
// ActionButtonParam row, 0, greyed, 0, [stack] 0, text offset; xmm0 the
// distance). The manager keeps one entry per key, refreshed by every call, and
// the call returns true on the frame the button is pressed with that entry
// chosen and not greyed (the row's execButtonCircle set). The game's keys are
// 1..16.
// Hooked too: a lamp's talk script asks for its prompt (rows below) each frame
// the player stands at it.
constexpr Function kActionPrompt = {0x01CD91F0, 17,
                                    {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x58},
                                    "action button prompt (man, key, row, 0, greyed, 0, ...; xmm0 distance)"};
// ActionButtonParam rows of a lamp's prompts (t240000.esd: 6000, 6001, 6100,
// 6101; 6102 the Dream's Return).
constexpr bool lamp_prompt(int32_t row) { return (row >= 6000 && row <= 6002) || (row >= 6100 && row <= 6102); }
// Called: an item's index in the player's inventory list (list, const u32*
// key; key 0x40000000 | id for goods), -1 when it is not there.
constexpr Function kItemIndex = {0x014D9F60, 16,
                                 {0x8B, 0x06, 0x83, 0xF8, 0xFF, 0x0F, 0x84, 0xA6, 0x00, 0x00, 0x00, 0x89, 0xC1, 0x48, 0x69, 0xC9},
                                 "inventory item index (list, const u32* key)"};
// Called: takes an item's whole stack out of the inventory and its quick slot
// (0, category bits, id), what EMEVD 2003[24] runs; the count is not asked.
constexpr Function kItemRemove = {0x0131CE20, 15,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x10},
                                  "inventory remove a stack (0, category bits, id)"};

// The one down kneels with no control. Lua EnableLogic (lua, entity, on) sets
// the bit Lua StopPlayer (0x01335B10) clears for the main player: off, the pad
// moves nothing. ForcePlayAnimation / ForcePlayLoopAnimation (lua, entity,
// anim), what EMEVD 2003[18] runs.
constexpr Function kEnableLogic = {0x0132E4D0, 14,
                                   {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xD6, 0x31, 0xD2, 0x89, 0xF7},
                                   "Lua EnableLogic (lua, entity, on)"};
constexpr Function kForceAnim = {0x0132E290, 10, {0x89, 0xD0, 0x31, 0xD2, 0x31, 0xC9, 0x89, 0xF7, 0x89, 0xC6},
                                 "Lua ForcePlayAnimation (lua, entity, anim)"};
constexpr Function kForceLoopAnim = {0x0132E300, 13, {0x89, 0xD0, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x31, 0xC9, 0x89, 0xF7, 0x89, 0xC6},
                                     "Lua ForcePlayLoopAnimation (lua, entity, anim)"};
// The player's three kneels (c0000 TAE a00): + 10 * 0..2 kneeling down (4.5 s),
// +1 kneeling (looped), +2 standing up - the shape of the lamp script's begin,
// loop and end (Lua_BonfireLoopAnim*: a random one of three, 10 apart).
constexpr int32_t kKneelAnim = 101260;

// --- 8. The game's own menu and text ----------------------------------------------
// The co-op pages of the System menu, after bbhost's PC sections
// (src/engine/option_menu.cpp; its Binary Ninja addresses are these + 0x400000).
// System's command list (0x01BB3AD0) is finalised by one call to 0x01BEA630; it is
// pointed at 16 bytes of alignment padding holding a jump to the plugin, which adds
// its rows first. A row's opener runs 0x01BB4C70 (the option movie) with an opener of
// the plugin's, which names a section sprite through the name table's free slot 6
// (the loop over it checks 6 names: one byte makes it 7) and hands 0x01B20900 a
// handler that builds the rows. The sprites and texts are the mod "Co-op - menu and
// text" (tools/coop_assets.cpp from tools/coop_messages.tsv).
constexpr Function kMsgText = {0x01AE8CC0, 16,
                               {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC},
                               "MsgRepository caption (out 0x40, group, id)"};
constexpr Function kOpenSection = {0x01B20900, 16,
                                   {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                   "option section open (root, params, sprite name, handler, 0, 0)"};
constexpr Function kScratchInit = {0x01C8AF20, 16,
                                   {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00},
                                   "option handler scratch block"};
constexpr Function kAddChoiceRow = {0x01B2A100, 16,
                                    {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                    "option choice row (dialog, captions, u8 value*, list, u8 default*)"};
constexpr Function kBuildOnOff = {0x01B2B3B0, 16,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00, 0x48, 0x89},
                                  "option On/Off list"};
constexpr Function kAddListRow = {0x01B29370, 16,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                  "option pick-list row (dialog, captions, i32 value*, list, i32 default*)"};
constexpr Function kListAppend = {0x01B1C0C0, 16,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xF6, 0x49, 0x89, 0xFF},
                                  "pick-list append (list, entry)"};
constexpr Function kWstrCpy = {0x02967040, 16,
                               {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x10, 0x49},
                               "wstring from UTF-16 (out, text)"};
constexpr Function kAddCommandRow = {0x01B4E2A0, 16,
                                     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                     "command list row (builder, captions, row functor, flag*)"};
constexpr Function kAddExitRow = {0x01B4C9A0, 16,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                  "command list command row (builder, captions, popup functor)"};
constexpr Function kSystemFinalize = {0x01BEA630, 16,
                                      {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                      "command list finalise (out, builder, arg)"};
constexpr Function kOpenSectionFlow = {0x01BB4C70, 16,
                                       {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC},
                                       "option movie flow (out, ctx, opener functor)"};
constexpr Function kBoxFromCaption = {0x01C1ED10, 16,
                                      {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0x40, 0x12},
                                      "message box (out, factory, caption)"};
constexpr Function kCommandFromAction = {0x0195DB60, 16,
                                         {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00},
                                         "YES command (out, action functor, 2)"};
constexpr Function kPopupFromBox = {0x01C1A070, 16,
                                    {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x89, 0xFB, 0x4C, 0x8B},
                                    "YES/NO popup (out, box, command)"};
// The message the game shows for a session event ("Cooperator ... returned to their
// own world"): an event text id (group 30), 0.
constexpr Function kShowEventText = {0x01BFFE80, 16,
                                     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                     "on-screen event text (id, 0)"};
constexpr Function kLayoutRows = {0x01B28DE0, 16,
                                  {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC},
                                  "a section's rows laid out again (dialog, 0)"};
constexpr const Function* kMenuFunctions[] = {&kLayoutRows, &kMsgText,       &kOpenSection,   &kScratchInit,     &kAddChoiceRow,
                                              &kBuildOnOff,    &kAddListRow,    &kListAppend,      &kWstrCpy,
                                              &kAddCommandRow, &kAddExitRow,    &kSystemFinalize,  &kOpenSectionFlow,
                                              &kBoxFromCaption, &kCommandFromAction, &kPopupFromBox, &kShowEventText};
constexpr uint64_t kExitGameOpener = 0x01C0C9F0;     // Exit Game's popup opener: a System row that holds it is Exit Game
constexpr uint64_t kSystemFinalizeCall = 0x01BB4978;  // e8 -> kSystemFinalize
constexpr uint8_t kSystemFinalizeCallBytes[5] = {0xE8, 0xB3, 0x5C, 0x03, 0x00};
constexpr uint64_t kTrampolinePad = 0x01BB4C60;       // 16 bytes of padding before 0x01BB4C70
constexpr uint8_t kPadBytes[16] = {0x90, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint64_t kSectionNameTable = 0x053398B0;    // Top .. NetworkSetting, then two zero slots
constexpr uint64_t kSectionCountImm = 0x01B20E21;     // the 6 of `cmp rdx, 6` (48 83 fa 06)
constexpr uint64_t kRowFunctorVTable = 0x0533D120;    // std::function<Dlg*(Dlg*, Ctx)>'s impl vtable
constexpr uint64_t kOpenerFunctorVTable = 0x05343880; // std::function<int64(a, b)>'s impl vtable
constexpr uint32_t kGroupMenuText = 200, kGroupLineHelp = 201, kGroupDialog = 204, kGroupEventText = 30;
// The message repository both read through (asserting when it is not made yet), and
// the on-screen text's owner, which kShowEventText asserts on too.
constexpr uint64_t kMsgRepositoryPtr = 0x05540300, kEventTextOwnerPtr = 0x05540308;
constexpr uint32_t kExitGameRowId = 0x1ADB4;
// The System builder's rows: begin at +0x68, end at +0x70, 0x180 bytes each. A section
// dialog's rows: begin at +0xE58, end at +0xE60, 0x90 bytes each, the widget at +0x80.
constexpr size_t kBuilderBegin = 0x68, kBuilderEnd = 0x70, kBuilderRowStride = 0x180;
constexpr size_t kItemsBegin = 0xE58, kItemsEnd = 0xE60, kItemStride = 0x90, kItemWidget = 0x80;
constexpr uint32_t kDefaultsRowId = 0x9D8A;
// ActionButtonParam: kPromptPickUp is "Pick Up Item"; 10000000 an empty row nothing uses,
// made a copy of 4000 with the text "Revive" (event text 9950000) for the pick-up.
constexpr uint32_t kReviveRow = 10000000;
constexpr size_t kButtonTextOff = 48;  // ActionButtonParam textId

constexpr const Function* kFunctions[] = {&kUseItem,        &kSetForcedMap,       &kSetForcedPosition,
                                          &kSetForcedOrientation, &kSetForcedWarp, &kSelectPlacement,
                                          &kStageTransition, &kOnMatchingCheck, &kForceDead,
                                          &kChrSetHp,       &kAwardItemLot,       &kSightRecord,
                                          &kPickUp,         &kRemoveItem,         &kActionPrompt,
                                          &kItemIndex,      &kItemRemove,         &kEnableLogic,
                                          &kForceAnim,      &kForceLoopAnim,      &kBlockClear};

// --- Singletons (pointers in the image's data) ---------------------------------
constexpr uint64_t kSessionManager = 0x05540290;  // +0x124 state, +0x18 matching controller
constexpr uint64_t kWorldChrMan = 0x0553E878;     // +0x60 the main PlayerIns (+0x78 chr_type: 0 own world, 1 white phantom)
// SprjEventFlagMan: +0x1C flags to a block, +0x20 pool stride, +0x28 pool,
// +0x38 std::map of blocks (node: left +0, right +0x10, is-nil +0x19, block
// +0x20, storage kind +0x28 (2 a pointer, 1 a pool index), storage +0x30); a
// flag is a bit, the block's first flag the first byte's top bit. A guest's
// game swaps all of it for the host's as she joins (packet 0x10, loaded at
// 0x013BECA0): the host's general flags and his map's, no item flags.
constexpr uint64_t kEventFlagMan = 0x0553B100;
// Its pool (0x013BC710 picks a block's slot): 1000 flags a block, a flag id cAABgfff (category
// c, area AA and its part B, group g, flag fff). A general block (AA and B 0) of category
// kFlagGeneral[k], group g, is slot k + 5g; a map's, of category kFlagMapKinds[k] and area
// kFlagAreas[a], slot 50 + 115g + 23k + a (the tables at kFlag*Table). +0x80: 0 while the
// blocks are the pool's (0x013BB590 sets 1 for a chalice dungeon, its blocks elsewhere). The
// game writes flags through 0x013CFCC0/0x013D0060 (bbport's on_event_flag) and ~35 inlined
// copies of them (talk 0x02047CB0, the stage update, map events, the boss clear).
constexpr uint64_t kFlagGeneralTable = 0x04730BF0, kFlagMapTable = 0x04730C10, kFlagAreaTable = 0x047314D0;
constexpr uint32_t kFlagGeneral[5] = {0, 1, 5, 6, 7};
constexpr uint32_t kFlagMapKinds[5] = {1, 5, 6, 7, 9};
constexpr uint32_t kFlagAreas[17][2] = {{21, 0}, {21, 1}, {22, 0}, {23, 0}, {24, 0}, {24, 1}, {24, 2}, {25, 0}, {26, 0},
                                        {27, 0}, {28, 0}, {29, 0}, {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, 0}};
constexpr uint32_t kFlagsPerBlock = 1000;
constexpr uint64_t kFlagMode = 0x80;
constexpr uint32_t kItemFlags = 5;                                  // the item lots' category
constexpr uint32_t kChaliceFirst = 9020, kChaliceLast = 9026;      // which chalice dungeon the store holds
constexpr uint32_t kCoopFlags = 4000;                              // 4000-4999: the boss clear's co-op flags (4000, 4047, 4092)
constexpr uint64_t kFrpgNetMan = 0x0553B120;      // +0xA80 the current SOS area
constexpr uint64_t kWorldRes = 0x0553B148;        // map list: +0x20 index, +0x10 array (+0x18 count, +0x20 entries of 0xA0, map at +8)
constexpr uint64_t kGameStateMan = 0x05556678;    // +0x08 transition pending, +0x0C selected map, +0x1520 forced warp
constexpr uint64_t kGameDataMan = 0x0553B130;     // +0x08 PlayerGameData (+0xA4 the player's type)
constexpr uint64_t kItemLotMan = 0x0553D6E0;      // MapItemMan: what the Lua item-lot helpers pass to kAwardItemLot
// Its map items: lists (head, then next at +0) of kind 1 at +0x18, kind 3 (map
// treasure) at +0x28, kind 2 (enemy drops) at +0x38. An item: +0x20 id (the
// kind in the low 4 bits), +0x28 its ItemLotParam row's getItemFlagId, +0x48
// bits (0x40 can be picked up, 0x80 the host's, shared over the network),
// +0xB8 another flag a pickup sets (-1: none). 0x017D81A0 finds one by id.
// Items at one spot are linked by +0x10/+0x18, one shown at a time; a lot of
// several rows may lie as several of them, each with the lot's flag.
constexpr uint64_t kTreasureList = 0x28;
constexpr uint64_t kItemId = 0x20, kItemFlag = 0x28, kItemBits = 0x48, kItemFlag2 = 0xB8;
constexpr uint16_t kItemActive = 0x40, kItemShared = 0x80;
constexpr uint32_t kKindTreasure = 3;
// +0x108: the character the camera follows; null is the main player (the
// camera's own choice at 0x0193BDEE).
constexpr uint64_t kWorldChrManDbg = 0x0553E880;

// --- 1. Elden Ring style deaths ------------------------------------------------
// The game's debug switches, static bytes next to WorldChrManDbg (its debug
// menu, built at 0x01921171, writes them). Each one counts for the main player
// only: the code asks ChrIns vtable+0x1B8, which is "this == WorldChrMan+0x60"
// (0x018C2D00).
constexpr uint64_t kPlayerNoDead = 0x0553E888;   // a killing blow leaves 1 HP (no-death check 0x01A577D0)
constexpr uint64_t kPlayerHide = 0x0553E88E;     // enemies' sight record says hidden (0x01A55390)
constexpr uint64_t kPlayerSilence = 0x0553E88F;  // the player's sounds reach no enemy (0x01A55230)

// ChrIns fields (bbhost's engine headers, matched against the Lua helpers
// that write them).
constexpr uint64_t kChrModules = 0x3B0;         // -> +0x20 the data module
constexpr uint64_t kDataHp = 0xF8, kDataMaxHp = 0xFC;  // max HP: a Lua helper (0x132FEF0) scales it in place
constexpr uint64_t kDataFlags = 0x200;          // bit 1: DisableDamage (Lua 0x01332E80)
constexpr uint8_t kDataNoDamage = 0x02;
// The 40-bit flag field at ChrIns+0x1E1, by byte: draw enable is bit 12 (Lua
// SetDrawEnable), invincible bit 14 (EnableInvincible), marked for deletion
// bit 15 (WorldChrMan_RemoveChrDelayed), completely no move bit 23
// (SetCompletelyNoMove).
constexpr uint64_t kChrFlags1 = 0x1E2, kChrFlags2 = 0x1E3;
constexpr uint8_t kChrDraw = 0x10, kChrInvincible = 0x40, kChrDeleting = 0x80;  // in kChrFlags1
constexpr uint8_t kChrNoMove = 0x80;                                              // in kChrFlags2
constexpr int32_t kLocalPlayerEntity = 10000;   // what the Lua helpers take for the main player

// Picking a downed partner up: the prompt (kActionPrompt) and the Blood Vial
// it costs.
constexpr uint64_t kActionButtonMan = 0x05540048;  // SprjActionButtonMan
constexpr uint32_t kPromptKey = 0x6262;            // our owner key, clear of the game's 1..16
constexpr int32_t kPromptPickUp = 4000;            // ActionButtonParam row: "Pick Up Item", 1.2 m, button circle
// PlayerGameData (kGameDataMan +0x08): the inventory list kItemIndex searches,
// and its records (16 bytes, +0 nonzero when used, +8 the count): below index
// [+0x34C] at [+0x380] + index * 16, from it on at [+0x370] + (index - split)
// * 16; indexes up to [+0x3B0] are valid.
constexpr uint64_t kInventoryList = 0x328, kInventorySplit = 0x34C, kInventoryLast = 0x3B0;
constexpr uint64_t kInventoryLow = 0x380, kInventoryHigh = 0x370, kItemCount = 8;
constexpr uint32_t kGoodsBits = 0x40000000;
constexpr int32_t kBloodVial = 1000;

// Session states at SessionManager+0x124 (Wozzardman): 0 idle, 1/2/3 create
// starting/failed/established (host), 4/5/6 join starting/failed/established
// (guest), 7 leaving.
constexpr int kStateIdle = 0, kStateHosting = 3, kStateJoined = 6;

// Goods and the SpEffects their bells put on.
constexpr int32_t kBeckoningBell = 200, kOldHunterBell = 201, kSmallResonantBell = 205;
constexpr int32_t kBellUseArg = 17;
// Bells for a new character. The game gives them in the Hunter's Dream only: a messenger
// awards lot 10010 (the Beckoning Bell; chained 10011, the Silencing Blank; both mark 6622)
// once 12100105 is set (back from the first Dream trip with Insight > 0), and 6622 puts the
// Small Resonant Bell in the Bath Messengers' shop (ShopLineupParam 200000-240000: one, its
// count held in flag 6610). 9180 is on while a story cutscene plays, the one after character
// creation among them (m24_01 event 12410000 clears it at the first wake). No goods lot holds the Small Resonant Bell: the empty lot 10020
// lends its row (ItemLotParam: lotItemId01 +0, lotItemCategory01 +0x20 (4 goods), lotItemNum01
// +0x8A; getItemFlagId -1) for one award.
constexpr int32_t kBellLot = 10010, kSpareLot = 10020, kLotGoods = 4;
constexpr uint32_t kBellsGiven = 6622, kSmallBellSold = 6610, kCutscene = 9180;
constexpr uint64_t kLotItemId = 0x00, kLotCategory = 0x20, kLotNum = 0x8A, kLotRowBytes = 0x94;
constexpr int32_t kBeckoningEffect = 9000, kSmallResonantEffect = 9005;

// Map ids as player_block / the map list pack them: area in the top byte.
constexpr uint32_t kAreaDream = 0x15;    // m21: the Hunter's Dream
constexpr uint32_t kAreaChalice = 0x1D;  // m29: the chalice dungeons

// --- 4. Boss progress ----------------------------------------------------------
// Each boss's defeat flag: the id of the map event that runs the game's
// HandleBossDefeat (EMEVD 2003[12]), read from every map's .emevd of the
// 1.09 dump (tools/bossflags.py). The Hunter's Dream's two (12101800,
// 12101850: the ending) and the chalice dungeons' (shared slot numbers) are
// left out.
struct BossFlag {
    uint32_t flag;
    const char* name;
};
constexpr BossFlag kBossFlags[] = {
    {12411700, "Cleric Beast"},
    {12411800, "Father Gascoigne"},
    {12301800, "Blood-starved Beast"},
    {12401800, "Vicar Amelia"},
    {12201800, "Witch of Hemwick"},
    {12301700, "Darkbeast Paarl"},
    {12701800, "Shadow of Yharnam"},
    {13201800, "Rom, the Vacuous Spider"},
    {12801800, "The One Reborn"},
    {12501800, "Martyr Logarius"},
    {13301800, "Amygdala"},
    {12421700, "Celestial Emissary"},
    {12421800, "Ebrietas"},
    {12601850, "Micolash"},
    {12601800, "Mergo's Wet Nurse"},
    {13401800, "Ludwig"},
    {13501850, "Living Failures"},
    {13501800, "Lady Maria"},
    {13401850, "Laurence"},
    {13601800, "Orphan of Kos"},
};

// What each boss's defeat event gives besides its flag (the same .emevd files;
// Logarius' and Maria's drops are in their events 12501803 / 13501803): the
// item lot it awards (AwardItemLot 2003[4]) - or, when the event's
// SkipIfEventFlag before it is on (the unique drop was had already), the
// repeat lot (0: nothing) - and the flags it turns on (SetEventFlag 2003[2]):
// the lamp, the defeat counters, story steps. A guest is given the lot as the
// boss dies; the flags go into her own world with the boss.
struct BossDrop {
    uint32_t boss;
    int32_t lot, repeat_lot;
    uint32_t had_flag;
    uint32_t flags[6];  // 0-ended
};
constexpr BossDrop kBossDrops[] = {
    {12411700, 50000010, 0, 6645, {2411, 9456}},
    {12411800, 31000, 0, 0, {2412, 9457, 5910}},
    {12301800, 80000000, 0, 0, {72400512, 2300, 9453}},
    {12401800, 50000001, 0, 0, {2400, 2401, 9455, 2402, 72400512}},
    {12201800, 21002950, 0, 0, {72400512, 2200, 9452, 5911}},
    {12301700, 50800000, 50800005, 6644, {2301, 9454}},
    {12701800, 2700990, 2700995, 6321, {2700, 2701, 9463}},
    {13201800, 51001900, 0, 0, {3200, 9465}},
    {12801800, 50700000, 0, 0, {2800, 9464}},
    {12501800, 2502000, 0, 0, {2500, 9460}},
    {13301800, 80000200, 0, 0, {3300, 9466}},
    {12421700, 25700000, 25700005, 6332, {2420, 9458}},
    {12421800, 80000300, 0, 0, {2421, 9459}},
    {12601850, 21000, 0, 0, {12604858, 12604857, 2600, 9461, 1082}},
    {12601800, 55100000, 0, 0, {12604808, 2601, 9462}},
    {13401800, 3401800, 3401802, 6674, {3400, 3401, 3402, 3403, 9471}},
    {13501850, 3501850, 0, 0, {3510, 3511, 3512, 3517}},
    {13501800, 3501800, 0, 0, {6675, 3510, 3511, 3512, 3513}},
    {13401850, 3401850, 3401852, 6673, {3400, 3401, 3402, 3403}},
    {13601800, 3601800, 0, 0, {3600}},
};

// --- 4b. Insight -----------------------------------------------------------------
// Every Insight the world gives - a boss first seen or killed, a place found - goes
// through common event 9350 (its argument the amount: SpEffect 4680, heroPointDamage -1,
// once per point), which ends at once for a client; the map events calling it also want
// a human player. A visitor gets none. The goods whose SpEffect raises Insight
// (EquipParamGoods refId -> SpEffectParam heroPointDamage < 0): 704 and Madman's
// Knowledge (2160, +1), Great One's Wisdom (2161, +2), the four One Third of Umbilical
// Cord (4685-4688, +3). PlayerGameData +0x84 holds Insight (bbhost's "insight").
constexpr int32_t kInsightGoods[] = {704, 1500, 1501, 4320, 4321, 4322, 4323};
constexpr int64_t kInsightMax = 999;
// The Beckoning Bell's refund: common event 9198 sets 9185 as the bell rings (SpEffect
// 9000 on a human), 9181 clears it when someone comes, and 9183 gives 1 Insight back
// (4680; "No insight was consumed.") if the bell stops with it still on.
constexpr uint32_t kBellRefund = 9185;

// --- 4c. Gestures ------------------------------------------------------------------
// PlayerGameData +0x5B8 -> 30 gesture slots from +0x10, 4 bytes each: bit 0 had, bits
// 1-15 a row of the gesture table (0x55526D0, 16-byte rows filled by 0x14EC980: row 0
// none, row g + 1 gesture g, its name goods text 800 + g). Talk command 66 (case
// 0x2041691 of the talk command switch 0x203FFD0) gives gesture g by setting bit 0 of
// the slot whose row's gesture (+8) is g; Eileen's first talks give 4 (flag 6744).
constexpr uint64_t kGestures = 0x5B8, kGestureSlots = 0x10;
constexpr int kGestureCount = 30;

}  // namespace addrs
