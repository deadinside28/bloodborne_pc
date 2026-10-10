// sim <host.so> <guest.so> <eboot.elf> <workdir>: two copies of the built
// plugin (two files: dlopen loads one path once) in one process, each with a stand-in bbhost API and a pretend game
// (a few objects at the offsets the plugin reads), talking over loopback
// UDP, on a virtual clock (this file's clock_gettime stands in for libc's,
// so 3 minutes of play take a moment). No game runs.
//
// The script: host on one map, guest on another; the guest travels to the
// host and rings, the host rings; the pretend server matches them; the guest
// takes a killing blow and is down; a boss dies, which brings her back up and
// gives her its drop; each picks up a few things (the other gets them too);
// each lights a lamp (the other gets it too: the host at once, the guest in
// her own world); the host's world gives him 3 Insight (she gets them too),
// he uses a Madman's Knowledge (his alone), and a ring of his bell that found
// nobody gives no Insight back; a gesture he gets together is hers too, one
// he gets apart is not, and one she gets at their second meeting is his too; the boss chain sends the guest home; her plugin marks
// the boss dead in her world and brings her back; they meet again; the host
// goes down, then the guest: both die for real; they meet a third time, she
// goes down, the host tries to pick her up without a Blood Vial (greyed),
// then with two (she is up, one is used); she goes down again and the host
// lights a lamp (she is up, free); then she uses a lamp's Return to the
// Hunter's Dream (out of the session, then to her own Dream). The checks at the end say what the plugin did. Byte patches are checked against the ELF, not
// written; reads of the image come from the ELF. <workdir> is made new each run,
// with what each side's plugin folder holds before it (host/, guest/).
#include "../addrs.hpp"
#include "../../../gpu/shim/plugins/bbhost_plugin.h"

#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

double g_clock = 1000.0;
unsigned char* g_elf;
long g_elf_size;
struct Segment {
    uint64_t va, offset, size;
};
Segment g_seg[64];
int g_nseg;

const unsigned char* elf_at(uint64_t va, size_t n) {
    for (int i = 0; i < g_nseg; ++i)
        if (va >= g_seg[i].va && va + n <= g_seg[i].va + g_seg[i].size) return g_elf + g_seg[i].offset + (va - g_seg[i].va);
    return nullptr;
}

constexpr uint64_t kFakeVtable = 0x123456789ull;

struct Game {
    const char* name;
    const char* role;
    char partner[32];
    char port[8];
    char dir[512];
    // the objects the plugin reads
    alignas(16) unsigned char wcm[0x100], player[0x400], other[0x400], session[0x200], netman[0x1000], wres[0x40], arr[0x40],
        entries[0xA0], gs[0x2000], modules[0x100], data[0x300], dbg[0x200], lotman[0x40], sight_module[0x10], sight_rec[0x80];
    alignas(16) unsigned char enemy[0x400], enemy_modules[0x40], enemy_data[0x200];  // one enemy, 1000 HP of its own
    unsigned char debug_flags[0x20];  // the game's switches at kWorldChrManDbg + 8..
    // GameDataMan -> PlayerGameData: an inventory of five records (+8 the
    // count): the Blood Vials, the Beckoning Bell, the Small Resonant Bell, the
    // Silencing Blank, Madman's Knowledge; the action button manager.
    alignas(16) unsigned char gdm[0x20], pgd[0x600], inv[0x50], abm[0x40];
    int64_t insight;
    alignas(16) unsigned char gest[0x90];  // PlayerGameData +0x5B8: slot i holds gesture i
    int bell_lots;  // awards of the bells' lots (not in lots[])
    float other_pos[3];  // the partner's character, as this screen's chr_list has it
    bool press;          // the action button is pressed this frame
    int prompts, greyed_prompts, stack_removes;
    int force_deads, lots[16], lot_count, hp_sets;
    int fail_awards;  // the next n item-lot awards fail
    // Map treasure (MapItemMan, the lotman buffer: list head at +0x28) and
    // what the game's pick-up and removal did with it.
    alignas(16) unsigned char items[8][0x100];
    int picked[16], pick_count, removes, removed[16];
    // The event flags (SprjEventFlagMan): its pool of 1200 blocks, and a map of four blocks: three
    // in the pool, and one of item flags elsewhere. Hers while she visits him: a copy of his, her
    // own back as she goes home (home_pool).
    alignas(16) unsigned char flagman[0x100], flag_head[0x40], flag_node[0x40], flag_bits[128];
    alignas(16) unsigned char flag_nodes[3][0x40];
    unsigned char pool[1200 * 125], home_pool[1200 * 125];
    int warps;  // the plugin's player_warp calls, each moving the player there
    bool no_control;           // the player's logic off (Lua EnableLogic)
    int32_t anim, loop_anim;   // the last animation forced on the player, and the last looped
    bool in_world;
    int load_frames;
    uint32_t map, pending_map, home_map, forced_map;
    float pos[3], forced_pos[4];
    int bell_frames;
    int effect;  // 9000 / 9005 while a bell is on, 0 otherwise
    int flags_set[64];
    int flag_count;
    uint32_t flag_ids[64];
    int travels, item_rings, event_rings, leaves, aligned_errors;
    int lamp_warps;
    int32_t lamp_to;
    // the plugin's callbacks
    void (*frame)(void*);
    void* frame_user;
    void (*on_flag)(uint32_t, int, void*);
    void* on_flag_user;
    void (*on_world)(uint32_t, void*);
    void* on_world_user;
    struct Hook {
        BbHookFn2 fn;
        void* user;
    } sight, pick, prompt, award, clear;  // enemy sight, pick-up, action button prompt, item-lot award, boss clear
    BbHostApi api;
};
Game G[2];
Game* g;  // the instance being called

template <typename T>
void put(unsigned char* base, size_t off, T v) {
    memcpy(base + off, &v, sizeof(v));
}
template <typename T>
T get(const unsigned char* base, size_t off) {
    T v;
    memcpy(&v, base + off, sizeof(v));
    return v;
}

// The inventory's records, by the goods each holds.
constexpr int32_t kSilencingBlank = 111;
constexpr int32_t kMadmansKnowledge = 1500;
constexpr int32_t kInventoryGoods[] = {addrs::kBloodVial, addrs::kBeckoningBell, addrs::kSmallResonantBell, kSilencingBlank,
                                       kMadmansKnowledge};
constexpr int kInventorySlots = sizeof(kInventoryGoods) / sizeof(kInventoryGoods[0]);
int goods_slot(int32_t id) {
    for (int i = 0; i < kInventorySlots; ++i)
        if (kInventoryGoods[i] == id) return i;
    return -1;
}
void set_goods(Game& x, int32_t id, int32_t n) {
    const size_t at = static_cast<size_t>(goods_slot(id)) * 16;
    put<uint32_t>(x.inv, at, n ? 0x1234 : 0);
    put<int32_t>(x.inv, at + addrs::kItemCount, n);
}
int32_t goods(const Game& x, int32_t id) {
    const size_t at = static_cast<size_t>(goods_slot(id)) * 16;
    return get<uint32_t>(x.inv, at) ? get<int32_t>(x.inv, at + addrs::kItemCount) : 0;
}

void set_state(Game& x, int chr_type, int state, bool other) {
    put<int32_t>(x.player, 0x78, chr_type);
    put<int32_t>(x.session, 0x124, state);
    put<uint64_t>(x.other, 0, other ? kFakeVtable : 0);
    put<uint32_t>(x.entries, 8, x.map);
}

void load_into(Game& x, uint32_t map, int frames) {
    x.in_world = false;
    x.load_frames = frames;
    x.pending_map = map;
}

bool inside(const unsigned char* buf, size_t size, uint64_t at, size_t n) {
    const uint64_t b = reinterpret_cast<uint64_t>(buf);
    return at >= b && at + n <= b + size;
}

// --- the stand-in API ------------------------------------------------------------------
bool g_guest_took_rules;  // the guest's log said it plays by the host's rules
void h_log(const char* fmt, ...) {
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%7.2f %-5s %s\n", g_clock - 1000.0, g->name, line);
    if (!strcmp(g->role, "guest") && strstr(line, "rules: the host's") && strstr(line, "enemy health 120%")) g_guest_took_rules = true;
}
const char* h_config(const char* key) {
    if (!strcmp(key, "bbcoop.role")) return g->role;
    if (!strcmp(key, "bbcoop.partner")) return g->partner;
    if (!strcmp(key, "bbcoop.port")) return g->port;
    if (!strcmp(key, "bbcoop.banners")) return "true";
    // Hers differs: the host's (120, the default) counts once they hear each other.
    if (!strcmp(key, "bbcoop.party_health") && !strcmp(g->role, "guest")) return "150";
    if (!strcmp(key, "bbcoop.share_pickups")) return "true";  // the copying is what the run checks
    if (!strcmp(key, "bbcoop.spectate")) return "true";       // and the camera following
    return "";
}
const char* h_str() { return ""; }
int h_one() { return 1; }
uint64_t h_guest_addr(uint64_t bn) { return bn; }
int h_read(uint64_t at, void* out, size_t n) {
    const uint64_t singletons[][2] = {{addrs::kWorldChrMan, reinterpret_cast<uint64_t>(g->wcm)},
                                      {addrs::kSessionManager, reinterpret_cast<uint64_t>(g->session)},
                                      {addrs::kFrpgNetMan, reinterpret_cast<uint64_t>(g->netman)},
                                      {addrs::kWorldRes, reinterpret_cast<uint64_t>(g->wres)},
                                      {addrs::kGameStateMan, reinterpret_cast<uint64_t>(g->gs)},
                                      {addrs::kWorldChrManDbg, reinterpret_cast<uint64_t>(g->dbg)},
                                      {addrs::kItemLotMan, reinterpret_cast<uint64_t>(g->lotman)},
                                      {addrs::kEventFlagMan, reinterpret_cast<uint64_t>(g->flagman)},
                                      {addrs::kGameDataMan, reinterpret_cast<uint64_t>(g->gdm)},
                                      {addrs::kActionButtonMan, reinterpret_cast<uint64_t>(g->abm)}};
    for (const auto& s : singletons)
        if (at == s[0] + addrs::kBnSlide && n == 8) {
            memcpy(out, &s[1], 8);
            return 0;
        }
    if (at >= 0x400000 && at < 0x400000 + 0x6000000) {
        const unsigned char* p = elf_at(at - 0x400000, n);
        if (!p) return 1;
        memcpy(out, p, n);
        return 0;
    }
    const struct {
        const unsigned char* b;
        size_t s;
    } bufs[] = {{g->wcm, sizeof(g->wcm)},         {g->player, sizeof(g->player)}, {g->other, sizeof(g->other)},
                {g->session, sizeof(g->session)}, {g->netman, sizeof(g->netman)}, {g->wres, sizeof(g->wres)},
                {g->arr, sizeof(g->arr)},         {g->entries, sizeof(g->entries)}, {g->gs, sizeof(g->gs)},
                {g->modules, sizeof(g->modules)}, {g->data, sizeof(g->data)}, {g->dbg, sizeof(g->dbg)},
                {g->sight_module, sizeof(g->sight_module)}, {g->sight_rec, sizeof(g->sight_rec)},
                {g->lotman, sizeof(g->lotman)},           {&g->items[0][0], sizeof(g->items)},
                {g->gdm, sizeof(g->gdm)},                 {g->pgd, sizeof(g->pgd)},
                {g->inv, sizeof(g->inv)},                 {g->abm, sizeof(g->abm)},
                {g->gest, sizeof(g->gest)},
                {g->flagman, sizeof(g->flagman)},         {g->flag_head, sizeof(g->flag_head)},
                {g->flag_node, sizeof(g->flag_node)},     {g->flag_bits, sizeof(g->flag_bits)},
                {&g->flag_nodes[0][0], sizeof(g->flag_nodes)}, {g->pool, sizeof(g->pool)},
                {g->enemy, sizeof(g->enemy)},             {g->enemy_modules, sizeof(g->enemy_modules)},
                {g->enemy_data, sizeof(g->enemy_data)}};
    for (const auto& b : bufs)
        if (inside(b.b, b.s, at, n)) {
            memcpy(out, reinterpret_cast<const void*>(at), n);
            return 0;
        }
    printf("!! %s read of unknown 0x%llx (%zu)\n", g->name, static_cast<unsigned long long>(at), n);
    return 1;
}
int h_write(uint64_t bn, const void* v, size_t n) {
    const uint64_t base = addrs::kWorldChrManDbg + 8 + addrs::kBnSlide;
    if (bn < base || bn + n > base + sizeof(g->debug_flags)) return 1;
    memcpy(g->debug_flags + (bn - base), v, n);
    return 0;
}
bool debug_flag(const Game& x, uint64_t va) { return x.debug_flags[va - addrs::kWorldChrManDbg - 8] != 0; }
int32_t& hp_of(Game& x) { return *reinterpret_cast<int32_t*>(x.data + addrs::kDataHp); }
int h_patch(uint64_t bn, const void* expect, const void* bytes, size_t n) {
    const unsigned char* p = elf_at(bn - 0x400000, n);
    if (!p) return 2;
    return memcmp(p, expect, n) == 0 || memcmp(p, bytes, n) == 0 ? 0 : 1;
}
int h_on_frame(void (*fn)(void*), void* user) {
    g->frame = fn;
    g->frame_user = user;
    return 0;
}
char g_row[256];
// A few ItemLotParam rows (getItemFlagId at +0x80): Gascoigne's drop, a
// five-row pickup, an NPC set with mixed flags, a one-row pickup, a chalice
// lot.
struct LotRow {
    uint32_t id;
    int32_t flag;
    unsigned char bytes[0x94];
};
LotRow g_lots[] = {{5500, 55000, {}},         {17000, 50000200, {}},    {17001, 50000205, {}},  {17002, 50000210, {}},
                   {31000, 5913, {}},         {2420230, 52420230, {}},  {2420231, 52420230, {}}, {2420232, 52420230, {}},
                   {2420233, 52420230, {}},   {2420234, 52420230, {}},  {101510000, 52901500, {}}, {1710, 17100, {}},
                   {1720, 17200, {}},        {7000, 72401000, {}},     {7001, -1, {}},
                   {addrs::kBellLot, static_cast<int32_t>(addrs::kBellsGiven), {}},
                   {addrs::kSpareLot, -1, {}}};
void* h_param_row(const char* table, uint32_t id, size_t* bytes) {
    if (!strcmp(table, "ItemLotParam")) {
        for (LotRow& r : g_lots)
            if (r.id == id) {
                memcpy(r.bytes + 0x80, &r.flag, 4);
                if (bytes) *bytes = sizeof(r.bytes);
                return r.bytes;
            }
        return nullptr;
    }
    if (bytes) *bytes = sizeof(g_row);
    return g_row;
}
int h_param_get(const char*, uint32_t, const char* field, char* out, size_t n) {
    snprintf(out, n, "%s", strcmp(field, "getItemFlagId") == 0 ? "-1" : "1");
    return 0;
}
int h_param_set(const char*, uint32_t, const char*, const char*) { return 0; }
int h_lua_event(const char* name) {
    if (!strcmp(name, "OnEvent_Call_SOS") || !strcmp(name, "OnEvent_SendSoulSign_NormalCoop")) {
        ++g->event_rings;
        g->bell_frames = 30;
    } else if (!strcmp(name, "OnLeaveMenu_Yes")) {
        ++g->leaves;
    }
    return 0;
}
int find_flag(uint32_t id) {
    for (int i = 0; i < g->flag_count; ++i)
        if (g->flag_ids[i] == id) return i;
    return -1;
}
int h_flag_get(uint32_t id, int* v) {
    const int i = find_flag(id);
    *v = i >= 0 ? g->flags_set[i] : 0;
    return 0;
}
int h_flag_set(uint32_t id, int v) {
    int i = find_flag(id);
    if (i < 0 && g->flag_count < 64) {
        i = g->flag_count++;
        g->flag_ids[i] = id;
    }
    if (i >= 0) g->flags_set[i] = v;
    return 0;
}
int h_stat_get(const char* name, int64_t* v) {
    if (strcmp(name, "insight") != 0 || !g->in_world) return 1;
    *v = g->insight;
    return 0;
}
int h_stat_set(const char* name, int64_t v) {
    if (strcmp(name, "insight") == 0) {
        g->insight = v;
        return 0;
    }
    if (strcmp(name, "hp") != 0) return 1;
    hp_of(*g) = static_cast<int32_t>(v);
    return 0;
}
int h_on_flag(void (*fn)(uint32_t, int, void*), void* user) {
    g->on_flag = fn;
    g->on_flag_user = user;
    return 0;
}
int h_pos(float* xyz, float* yaw) {
    if (!g->in_world) return 1;
    if (xyz) memcpy(xyz, g->pos, sizeof(g->pos));
    if (yaw) *yaw = 1.5f;
    return 0;
}
int h_cam(float*, float*) { return 1; }
int h_block(uint32_t* b) {
    if (!g->in_world) return 1;
    *b = g->map;
    return 0;
}
int h_pwarp(uint32_t, const float* xyz, float) {
    if (!g->in_world) return 1;
    ++g->warps;
    memcpy(g->pos, xyz, sizeof(g->pos));
    return 0;
}
int h_lwarp(int32_t to) {
    if (!g->in_world) return 1;
    ++g->lamp_warps;
    g->lamp_to = to;
    return 0;
}
uint64_t h_symbol(const char*) { return 0; }
// A map treasure lying in x's world: an id (kind 3), its item flag, another flag.
void add_item(Game& x, int slot, uint32_t id, int32_t flag, int32_t flag2) {
    unsigned char* it = x.items[slot];
    memset(it, 0, 0x100);
    put<uint64_t>(it, 0, get<uint64_t>(x.lotman, addrs::kTreasureList));
    put<uint32_t>(it, addrs::kItemId, id);
    put<int32_t>(it, addrs::kItemFlag, flag);
    put<uint16_t>(it, addrs::kItemBits, addrs::kItemActive);
    put<int32_t>(it, addrs::kItemFlag2, flag2);
    put<uint64_t>(x.lotman, addrs::kTreasureList, reinterpret_cast<uint64_t>(it));
}

// Takes the item with this id out of x's list: its flag, or 0 (none).
int32_t unlink_item(Game& x, uint32_t id) {
    unsigned char* prev = nullptr;
    for (uint64_t at = get<uint64_t>(x.lotman, addrs::kTreasureList); at; at = get<uint64_t>(reinterpret_cast<unsigned char*>(at), 0)) {
        unsigned char* it = reinterpret_cast<unsigned char*>(at);
        if (get<uint32_t>(it, addrs::kItemId) == id) {
            const uint64_t next = get<uint64_t>(it, 0);
            if (prev) put<uint64_t>(prev, 0, next);
            else put<uint64_t>(x.lotman, addrs::kTreasureList, next);
            return get<int32_t>(it, addrs::kItemFlag);
        }
        prev = it;
    }
    return 0;
}

// The game's pick-up (the hook first): the item goes, its contents to x; the
// host's world sets its flag inline (no flag callback), a guest's sets none.
void game_pick_up(Game& x, uint32_t id) {
    uint32_t arg = id;
    if (x.pick.fn) {
        BbHookCtx ctx = {};
        ctx.arg[0] = reinterpret_cast<uint64_t>(x.lotman);
        ctx.arg[1] = reinterpret_cast<uint64_t>(&arg);
        Game* was = g;
        g = &x;
        x.pick.fn(&ctx, x.pick.user);
        g = was;
    }
    const int32_t flag = unlink_item(x, id);
    if (!flag) return;
    if (x.pick_count < 16) x.picked[x.pick_count++] = flag;
    if (get<int32_t>(x.session, 0x124) != addrs::kStateJoined) {
        Game* was = g;
        g = &x;
        h_flag_set(static_cast<uint32_t>(flag), 1);
        g = was;
    }
}

int h_call(uint64_t bn, BbCallRegs* r) {
    const uint64_t va = bn - addrs::kBnSlide;
    if (va == addrs::kSetForcedPosition.va || va == addrs::kSetForcedOrientation.va) {
        if (r->arg[0] % 16) ++g->aligned_errors;
        if (va == addrs::kSetForcedPosition.va) memcpy(g->forced_pos, reinterpret_cast<const void*>(r->arg[0]), 16);
        return 0;
    }
    if (va == addrs::kSetForcedMap.va) {
        g->forced_map = *reinterpret_cast<const uint32_t*>(r->arg[0]);
        return 0;
    }
    if (va == addrs::kSetForcedWarp.va) {
        g->gs[0x1520] = 1;
        return 0;
    }
    if (va == addrs::kSelectPlacement.va) {
        r->ret = static_cast<uint32_t>(r->arg[1]) == 0xFFFFFFFFu;
        put<uint32_t>(g->gs, 0x0C, g->forced_map);
        return 0;
    }
    if (va == addrs::kStageTransition.va) {
        r->ret = 1;
        g->gs[0x1520] = 0;
        ++g->travels;
        memcpy(g->pos, g->forced_pos, sizeof(g->pos));
        load_into(*g, g->forced_map, 150);
        return 0;
    }
    if (va == addrs::kEnableLogic.va || va == addrs::kForceAnim.va || va == addrs::kForceLoopAnim.va) {
        if (r->arg[1] != static_cast<uint64_t>(addrs::kLocalPlayerEntity)) return 1;
        if (va == addrs::kEnableLogic.va) g->no_control = !(r->arg[2] & 1);
        if (va == addrs::kForceAnim.va) g->anim = static_cast<int32_t>(r->arg[2]);
        if (va == addrs::kForceLoopAnim.va) g->loop_anim = static_cast<int32_t>(r->arg[2]);
        return 0;
    }
    if (va == addrs::kForceDead.va) {
        if (r->arg[1] == static_cast<uint64_t>(addrs::kLocalPlayerEntity)) {
            ++g->force_deads;
            hp_of(*g) = debug_flag(*g, addrs::kPlayerNoDead) ? 1 : 0;
        }
        return 0;
    }
    if (va == addrs::kChrSetHp.va) {
        if (r->arg[0] == reinterpret_cast<uint64_t>(g->player)) {
            ++g->hp_sets;
            hp_of(*g) = static_cast<int32_t>(r->arg[1]);
        }
        return 0;
    }
    if (va == addrs::kAwardItemLot.va) {
        if (g->award.fn) {  // the plugin's own calls go through its hook too
            BbHookCtx ctx = {};
            memcpy(ctx.arg, r->arg, sizeof(ctx.arg));
            if (g->award.fn(&ctx, g->award.user)) return 0;
        }
        if (g->fail_awards > 0) {
            --g->fail_awards;
            return 1;
        }
        const int lot = static_cast<int>(r->arg[1]);
        if (r->arg[0] != reinterpret_cast<uint64_t>(g->lotman)) return 0;
        if (lot == addrs::kBellLot || lot == addrs::kSpareLot) {  // its goods as the row has them, its flag
            ++g->bell_lots;
            if (lot == addrs::kBellLot) {
                set_goods(*g, addrs::kBeckoningBell, 1);
                set_goods(*g, kSilencingBlank, 1);
                h_flag_set(addrs::kBellsGiven, 1);
            } else {
                size_t n = 0;
                const unsigned char* row = static_cast<const unsigned char*>(h_param_row("ItemLotParam", addrs::kSpareLot, &n));
                if (get<int32_t>(row, addrs::kLotItemId) > 0 && get<int32_t>(row, addrs::kLotCategory) == addrs::kLotGoods && row[addrs::kLotNum])
                    set_goods(*g, get<int32_t>(row, addrs::kLotItemId), row[addrs::kLotNum]);
            }
        } else if (g->lot_count < 16) {
            g->lots[g->lot_count++] = lot;
        }
        return 0;
    }
    if (va == addrs::kPickUp.va) {
        if (r->arg[0] != reinterpret_cast<uint64_t>(g->lotman)) return 1;
        game_pick_up(*g, *reinterpret_cast<const uint32_t*>(r->arg[1]));
        return 0;
    }
    if (va == addrs::kRemoveItem.va) {
        if (r->arg[0] != reinterpret_cast<uint64_t>(g->lotman) || r->arg[2] != 0) return 1;
        const int32_t flag = unlink_item(*g, *reinterpret_cast<const uint32_t*>(r->arg[1]));
        if (flag && g->removes < 16) g->removed[g->removes++] = flag;
        return 0;
    }
    if (va == addrs::kItemIndex.va) {  // a goods' record index while there are any
        const uint32_t key = *reinterpret_cast<const uint32_t*>(r->arg[1]);
        const int i = (key & addrs::kGoodsBits) ? goods_slot(static_cast<int32_t>(key & ~addrs::kGoodsBits)) : -1;
        const bool have = r->arg[0] == reinterpret_cast<uint64_t>(g->pgd) + addrs::kInventoryList && i >= 0 &&
                          get<uint32_t>(g->inv, static_cast<size_t>(i) * 16) != 0;
        r->ret = have ? static_cast<uint64_t>(i) : 0xFFFFFFFFu;
        return 0;
    }
    if (va == addrs::kItemRemove.va) {
        if (r->arg[1] != addrs::kGoodsBits || r->arg[2] != static_cast<uint64_t>(addrs::kBloodVial)) return 1;
        ++g->stack_removes;
        memset(g->inv, 0, 16);
        return 0;
    }
    if (va == addrs::kActionPrompt.va) {  // used when pressed, not greyed
        if (r->arg[0] != reinterpret_cast<uint64_t>(g->abm) || r->arg[1] != addrs::kPromptKey ||
            r->arg[2] != static_cast<uint64_t>(addrs::kPromptPickUp))
            return 1;
        ++g->prompts;
        if (r->arg[4]) ++g->greyed_prompts;
        r->ret = g->press && !r->arg[4];
        g->press = false;
        return 0;
    }
    if (va == addrs::kUseItem.va) {
        ++g->item_rings;
        g->bell_frames = 30;
        r->ret = 1;
        return 0;
    }
    printf("!! %s call of unknown 0x%llx\n", g->name, static_cast<unsigned long long>(bn));
    return 1;
}
// x's game gives itself an item lot (an event's Award Item Lot).
void game_award(Game& x, int lot) {
    g = &x;
    BbCallRegs r = {};
    r.arg[0] = reinterpret_cast<uint64_t>(x.lotman);
    r.arg[1] = static_cast<uint64_t>(lot);
    h_call(addrs::kAwardItemLot.va + addrs::kBnSlide, &r);
}

int h_hook2(uint64_t bn, const void*, size_t, BbHookFn2 fn, void* user) {
    const struct {
        uint64_t va;
        Game::Hook Game::*hook;
    } hooks[] = {{addrs::kSightRecord.va, &Game::sight},
                 {addrs::kPickUp.va, &Game::pick},
                 {addrs::kActionPrompt.va, &Game::prompt},
                 {addrs::kAwardItemLot.va, &Game::award},
                 {addrs::kBlockClear.va, &Game::clear}};
    for (const auto& h : hooks)
        if (bn == h.va + addrs::kBnSlide) g->*h.hook = {fn, user};
    return 0;
}

// Whether an enemy on x's screen would see chr: the game's record, with the
// plugin's hook first (a skipped call leaves what the hook wrote).
bool enemies_see(Game& x, unsigned char* chr) {
    unsigned char* module = x.sight_module;
    unsigned char* rec = x.sight_rec;
    memset(module, 0, sizeof(x.sight_module));
    memset(rec, 0, sizeof(x.sight_rec));
    put<uint64_t>(module, 8, reinterpret_cast<uint64_t>(chr));
    BbHookCtx ctx = {};
    ctx.arg[0] = reinterpret_cast<uint64_t>(module);
    ctx.arg[1] = reinterpret_cast<uint64_t>(rec);
    g = &x;
    const bool skipped = x.sight.fn && x.sight.fn(&ctx, x.sight.user);
    return !(skipped && rec[0x64]);
}
int h_ids(const char* table, uint32_t* ids, size_t max, size_t* count) {
    if (strcmp(table, "ItemLotParam") != 0) return 1;
    const size_t n = sizeof(g_lots) / sizeof(g_lots[0]);
    for (size_t i = 0; i < n && i < max; ++i) ids[i] = g_lots[n - 1 - i].id;  // not in order
    *count = n;
    return 0;
}
int h_on_world(void (*fn)(uint32_t, void*), void* user) {
    g->on_world = fn;
    g->on_world_user = user;
    return 0;
}
int h_chr_list(BbChr* out, size_t max, size_t* count) {
    if (!g->in_world) return 1;
    size_t n = 0;
    if (n < max) out[n] = BbChr{g->player, 1, -1, get<int32_t>(g->player, 0x78), 0, 1, 1, {g->pos[0], g->pos[1], g->pos[2]}, 0, 1};
    ++n;
    if (get<uint64_t>(g->other, 0)) {
        if (n < max) out[n] = BbChr{g->other, 2, -1, 1, 0, 1, 1, {g->other_pos[0], g->other_pos[1], g->other_pos[2]}, 0, 0};
        ++n;
    }
    if (n < max)
        out[n] = BbChr{g->enemy, 3, 100000, 6, 0, get<int32_t>(g->enemy_data, addrs::kDataHp),
                       get<int32_t>(g->enemy_data, addrs::kDataMaxHp), {0, 0, 0}, 0, 0};  // an enemy
    ++n;
    *count = n;
    return 0;
}
int h_sp_apply(void*, int32_t) { return 1; }
int h_sp_has(void* chr, int32_t id) { return !chr && g->effect == id; }
const char* h_plugin_dir(const char*) { return g->dir; }
int h_message(const char* text, float) {
    printf("%7.2f %-5s [screen] %s\n", g_clock - 1000.0, g->name, text);
    return 0;
}
int h_time(float) { return 0; }
int h_on_option(const char*, void (*)(const char*, const char*, void*), void*) { return 0; }
int h_on_action(const char*, void (*)(const char*, void*), void*) { return 0; }
int h_game_file(const char*, const void**, size_t*) { return 1; }
int h_overlay(const char*, const char*, const void*, size_t) { return 1; }
int h_rules(const char*, const char*) { return 0; }
int h_on_rules(const char*, void (*)(const char*, void*), void*) { return 0; }
int h_visitor(const char*) { return 0; }
int h_text(const BbText*, size_t) { return -1; }

void fill_api(BbHostApi& a) {
    memset(&a, 0, sizeof(a));
    a.version = BB_PLUGIN_API_VERSION;
    a.size = sizeof(a);
    a.log = h_log;
    a.config = h_config;
    a.data_dir = h_str;
    a.mods_dir = h_str;
    a.eboot_sha256 = h_str;
    a.eboot_is_109 = h_one;
    a.guest_addr = h_guest_addr;
    a.read = h_read;
    a.write = h_write;
    a.patch = h_patch;
    a.on_frame = h_on_frame;
    a.param_row = h_param_row;
    a.param_get = h_param_get;
    a.param_set = h_param_set;
    a.lua_event = h_lua_event;
    a.event_flag_get = h_flag_get;
    a.event_flag_set = h_flag_set;
    a.player_stat_get = h_stat_get;
    a.player_stat_set = h_stat_set;
    a.on_event_flag = h_on_flag;
    a.player_position = h_pos;
    a.camera_get = h_cam;
    a.player_block = h_block;
    a.player_warp = h_pwarp;
    a.lamp_warp = h_lwarp;
    a.symbol = h_symbol;
    a.call_guest = h_call;
    a.hook2 = h_hook2;
    a.param_ids = h_ids;
    a.on_world_load = h_on_world;
    a.chr_list = h_chr_list;
    a.sp_effect_apply = h_sp_apply;
    a.sp_effect_has = h_sp_has;
    a.plugin_dir = h_plugin_dir;
    a.show_message = h_message;
    a.set_time_scale = h_time;
    a.on_option = h_on_option;
    a.on_action = h_on_action;
    a.game_file = h_game_file;
    a.overlay_file = h_overlay;
    a.set_rules = h_rules;
    a.on_world_rules = h_on_rules;
    a.visitor = h_visitor;
    a.replace_text = h_text;
}

bool load_elf(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    g_elf_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_elf = static_cast<unsigned char*>(malloc(static_cast<size_t>(g_elf_size)));
    const bool ok = g_elf && fread(g_elf, 1, static_cast<size_t>(g_elf_size), f) == static_cast<size_t>(g_elf_size);
    fclose(f);
    if (!ok) return false;
    uint64_t phoff;
    uint16_t phentsize, phnum;
    memcpy(&phoff, g_elf + 0x20, 8);
    memcpy(&phentsize, g_elf + 0x36, 2);
    memcpy(&phnum, g_elf + 0x38, 2);
    for (int i = 0; i < phnum && g_nseg < 64; ++i) {
        const unsigned char* p = g_elf + phoff + static_cast<uint64_t>(i) * phentsize;
        uint64_t off, va, filesz;
        memcpy(&off, p + 8, 8);
        memcpy(&va, p + 16, 8);
        memcpy(&filesz, p + 32, 8);
        if (filesz) g_seg[g_nseg++] = {va, off, filesz};
    }
    return true;
}

// The game's slot for a flag's block (0x013BC710, its tables read from the ELF).
int sim_slot(uint32_t id) {
    const uint32_t c = id / 10000000, aa = id / 100000 % 100, b = id / 10000 % 10, g = id / 1000 % 10;
    auto table = [](uint64_t va, int i) {
        uint32_t v = 0;
        memcpy(&v, elf_at(va + 4 * i, 4), 4);
        return v;
    };
    if (!aa && !b) {
        for (int k = 0; k < 5; ++k)
            if (table(addrs::kFlagGeneralTable, k) == c) return static_cast<int>(k + 5 * g);
        return -1;
    }
    int area = -1;
    for (int i = 0; i < 17; ++i)
        if (table(addrs::kFlagAreaTable, 2 * i) == aa && table(addrs::kFlagAreaTable, 2 * i + 1) == b) area = i;
    uint32_t u = g * 0x73 + 0x1B;
    for (int k = 0; k < 5; ++k) {
        u += 0x17;
        if (table(addrs::kFlagMapTable, k) == c) return area < 0 ? -1 : static_cast<int>(u) + area;
    }
    return -1;
}
// A flag in the pool, as the game's inlined setters write it.
void pool_set(Game& x, uint32_t id, bool on) {
    unsigned char& b = x.pool[sim_slot(id) * 125 + id % 1000 / 8];
    const unsigned char m = static_cast<unsigned char>(0x80 >> (id % 8));
    b = on ? static_cast<unsigned char>(b | m) : static_cast<unsigned char>(b & ~m);
}
bool pool_get(const Game& x, uint32_t id) { return x.pool[sim_slot(id) * 125 + id % 1000 / 8] & (0x80 >> (id % 8)); }

void setup(Game& x, const char* name, const char* role, int port, int partner_port, uint32_t map, const char* workdir) {
    x.name = name;
    x.role = role;
    snprintf(x.partner, sizeof(x.partner), "127.0.0.1:%d", partner_port);
    snprintf(x.port, sizeof(x.port), "%d", port);
    snprintf(x.dir, sizeof(x.dir), "%s/%s", workdir, name);
    x.in_world = true;
    x.map = x.home_map = map;
    x.pos[0] = 10.0f;
    put<uint64_t>(x.wcm, 0x60, reinterpret_cast<uint64_t>(x.player));
    put<uint64_t>(x.player, 0, kFakeVtable);
    put<uint64_t>(x.player, addrs::kChrModules, reinterpret_cast<uint64_t>(x.modules));
    put<uint64_t>(x.modules, 0x20, reinterpret_cast<uint64_t>(x.data));
    put<int32_t>(x.data, addrs::kDataHp, 800);
    put<int32_t>(x.data, addrs::kDataMaxHp, 800);
    put<uint64_t>(x.enemy, addrs::kChrModules, reinterpret_cast<uint64_t>(x.enemy_modules));
    put<uint64_t>(x.enemy_modules, 0x20, reinterpret_cast<uint64_t>(x.enemy_data));
    put<int32_t>(x.enemy_data, addrs::kDataHp, 1000);
    put<int32_t>(x.enemy_data, addrs::kDataMaxHp, 1000);
    x.player[addrs::kChrFlags1] = addrs::kChrDraw;
    x.other[addrs::kChrFlags1] = addrs::kChrDraw;
    put<int32_t>(x.wres, 0x20, 0);
    put<uint64_t>(x.wres, 0x10, reinterpret_cast<uint64_t>(x.arr));
    put<int32_t>(x.arr, 0x18, 1);
    put<uint64_t>(x.arr, 0x20, reinterpret_cast<uint64_t>(x.entries));
    put<int32_t>(x.netman, 0xA80, 241100);
    put<uint64_t>(x.gdm, 8, reinterpret_cast<uint64_t>(x.pgd));
    put<uint32_t>(x.pgd, addrs::kInventorySplit, kInventorySlots);
    put<uint32_t>(x.pgd, addrs::kInventoryLast, 10);
    put<uint64_t>(x.pgd, addrs::kInventoryLow, reinterpret_cast<uint64_t>(x.inv));
    put<uint64_t>(x.pgd, addrs::kGestures, reinterpret_cast<uint64_t>(x.gest));
    for (int i = 0; i < addrs::kGestureCount; ++i) put<uint32_t>(x.gest, addrs::kGestureSlots + 4 * i, static_cast<uint32_t>(i + 1) << 1);
    x.other_pos[0] = 200.0f;  // far off until the script brings them close
    set_state(x, 0, 0, false);
    // Event flags: 1000 to a block. The map's root: block 52410 (flags 52410000..52410999),
    // holding a pointer; to its left 12411 (left of it 9), to its right 72400, each a pool slot.
    put<uint32_t>(x.flagman, 0x1C, 1000);
    put<uint32_t>(x.flagman, 0x20, 125);
    put<uint64_t>(x.flagman, 0x28, reinterpret_cast<uint64_t>(x.pool));
    put<uint64_t>(x.flagman, 0x38, reinterpret_cast<uint64_t>(x.flag_head));
    put<uint64_t>(x.flag_head, 8, reinterpret_cast<uint64_t>(x.flag_node));
    x.flag_head[0x19] = 1;
    const uint64_t head = reinterpret_cast<uint64_t>(x.flag_head);
    auto node = [&](unsigned char* n, uint32_t block, int32_t kind, uint64_t storage, uint64_t left, uint64_t right) {
        put<uint64_t>(n, 0, left ? left : head);
        put<uint64_t>(n, 0x10, right ? right : head);
        put<uint32_t>(n, 0x20, block);
        put<int32_t>(n, 0x28, kind);
        put<uint64_t>(n, 0x30, storage);
    };
    const uint64_t n9 = reinterpret_cast<uint64_t>(x.flag_nodes[0]), n12411 = reinterpret_cast<uint64_t>(x.flag_nodes[1]),
                   n72400 = reinterpret_cast<uint64_t>(x.flag_nodes[2]);
    node(x.flag_node, 52410, 2, reinterpret_cast<uint64_t>(x.flag_bits), n12411, n72400);
    node(x.flag_nodes[0], 9, 1, static_cast<uint64_t>(sim_slot(9000)), 0, 0);
    node(x.flag_nodes[1], 12411, 1, static_cast<uint64_t>(sim_slot(12411000)), n9, 0);
    node(x.flag_nodes[2], 72400, 1, static_cast<uint64_t>(sim_slot(72400000)), 0, 0);
    fill_api(x.api);
}

void set_vials(Game& x, int32_t n) { set_goods(x, addrs::kBloodVial, n); }
int32_t vials(const Game& x) { return goods(x, addrs::kBloodVial); }

int g_failures;
void expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

}  // namespace

extern "C" int clock_gettime(clockid_t, struct timespec* ts) {
    ts->tv_sec = static_cast<time_t>(g_clock);
    ts->tv_nsec = static_cast<long>((g_clock - floor(g_clock)) * 1e9);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 5 || !load_elf(argv[3])) {
        fprintf(stderr, "usage: sim <host.so> <guest.so> <eboot.elf> <workdir>\n");
        return 2;
    }
    char path[512];
    auto at = [&](const char* name) {
        snprintf(path, sizeof(path), "%s/%s", argv[4], name);
        return path;
    };
    auto put_file = [&](const char* name, const char* text) {
        if (FILE* f = fopen(at(name), "w")) {
            fputs(text, f);
            fclose(f);
        }
    };
    if (mkdir(argv[4], 0755) != 0 || mkdir(at("host"), 0755) != 0 || mkdir(at("guest"), 0755) != 0) {
        fprintf(stderr, "sim: %s: needs a new folder each run\n", argv[4]);
        return 2;
    }
    put_file("host/catchup.txt", "");  // send his whole world once
    put_file("guest/gift.txt", "insight 2\ninsight -1\ngesture 20\nboss 12411700\n");  // her own Insight, a gesture, a boss
    put_file("guest/world.txt", "run 1 5 3\n12411060 1 3\n");  // a change held from a run before
    setup(G[0], "host", "host", 39411, 39412, 0x18010000, argv[4]);
    setup(G[1], "guest", "guest", 39412, 39411, 0x18000000, argv[4]);
    for (int i = 0; i < 2; ++i) {
        g = &G[i];
        void* so = dlopen(argv[1 + i], RTLD_NOW | RTLD_LOCAL);
        if (!so) {
            fprintf(stderr, "dlopen %s: %s\n", argv[1 + i], dlerror());
            return 1;
        }
        auto init = reinterpret_cast<int (*)(const BbHostApi*)>(dlsym(so, "bb_plugin_init"));
        auto image = reinterpret_cast<int (*)(const BbHostApi*)>(dlsym(so, "bb_plugin_image"));
        if (!init || !image || init(&g->api) != 0 || image(&g->api) != 0 || !g->frame) {
            fprintf(stderr, "%s did not start\n", g->name);
            return 1;
        }
    }
    Game& host = G[0];
    Game& guest = G[1];
    const uint32_t kBoss = 12411800;  // Father Gascoigne
    // Central Yharnam's lamps (m24_01, the host's map): 0 is lit in his world
    // already, she lights 1, he lights 2; she returns to the Dream from 0.
    const addrs::Lamp kLamp0 = {241, 0, 2102950}, kLampHers = {241, 1, 2102950}, kLampHis = {241, 2, 2102950};
    const addrs::Lamp kLampRevive = {241, 3, 2102950};  // the host lights it while she is down
    int matches = 0;
    double matched_at = 0, boss_at = 0, sent_home_at = 0, second_at = 0, third_at = 0, left_at = 0;
    bool arrived_lamp = false, lamps_lit = false, respawned = false, returned = false, nobody_left = false;
    int leaves_before = 0, matches_before = 0;
    int warps_at_match = 0, moved_for = 0, join_moves = 0;  // her warps by the join alone (a revive moves her too)
    bool hit1 = false, checked_down = false, checked_up = false, hit_host = false, hit_guest = false;
    bool knelt = false, kneel_checked = false, kneel_held = false, stood_up = false;
    bool was_down = false, camera_followed = false, shown_on_host = false, hidden_from_enemies = false, still = false;
    // The third meeting: picking her up and a lamp.
    bool down3 = false, near3 = false, far_no_prompt = false, greyed_kept_down = false, picked_up = false;
    bool down4 = false, lamp_up = false, down5 = false, down6 = false, prompt_up = false, last_vial = false, held_checked = false, held = false;
    bool tried_greyed = false, tried_vials = false, came_back = false, lamp3 = false;
    // Enemy health: raised on both screens while together, the host's back
    // (in proportion to the damage) once she is gone.
    bool party_checked = false, party_up = false, party_hit = false, party_back_checked = false, party_back = false, stubborn_checked = false, stubborn = false;
    bool revived = false, camera_back = false, shown_again = false, both_released = false;
    bool unseen_on_host = false, seen_again = false, picked = false, guest_picked = false, late_copy = false;
    // Insight: the host has 1 (and two Madman's Knowledge), she none (gift.txt gives her 1).
    host.insight = 1;
    set_goods(host, kMadmansKnowledge, 2);
    int world_insight = 0;
    // Gestures: slot g holds gesture g; bit 0 had.
    auto has_gesture = [](const Game& x, int gesture) { return x.gest[addrs::kGestureSlots + 4 * gesture] & 1; };
    auto get_gesture = [](Game& x, int gesture) { x.gest[addrs::kGestureSlots + 4 * gesture] |= 1; };
    get_gesture(host, 0);
    bool host_gesture = false, guest_gesture = false;
    bool madman = false, npc_gift = false, apart_insight = false, refund_rang = false, refund_kept_off = false;
    // The host's world's treasure: a five-row lot, a one-row lot, an NPC
    // set's piece, and what the guest picks up below.
    add_item(host, 0, 0x13, 52420230, -1);
    add_item(host, 1, 0x23, 55000, -1);
    add_item(host, 2, 0x33, 50000205, -1);
    add_item(host, 3, 0x43, 17100, -1);
    g = &host;
    h_flag_set(17300, 1);  // taken in his world before she came
    h_flag_set(addrs::lamp_lit_flag(kLamp0), 1);
    g = &guest;
    h_flag_set(addrs::kDreamReturnFlag, 1);  // she has been to the Dream
    guest.flag_bits[800 / 8] = static_cast<unsigned char>(0x80 >> (800 % 8));  // 52410800: taken in her own world
    // Bells: the host's character has the Beckoning Bell, not the Small Resonant
    // one; hers is new, its opening cutscene over 5 s in.
    set_goods(host, addrs::kBeckoningBell, 1);
    g = &host;
    h_flag_set(addrs::kBellsGiven, 1);
    g = &guest;
    h_flag_set(addrs::kCutscene, 1);
    bool bells_waited = false;
    // World progress, with the one-time catch-up (the runner's host/catchup.txt): before she comes
    // his world has a door open and an NPC in state 1141; hers that NPC state too, a door of her
    // own, a talk of her own, an item taken and a chalice dungeon picked (her world.txt also holds
    // an older run's door, set as she starts; the catch-up's block puts it back off).
    pool_set(host, 12411030, true);
    pool_set(host, 1141, true);
    pool_set(guest, 1141, true);
    pool_set(guest, 12411040, true);
    pool_set(guest, 72500100, true);
    pool_set(guest, 52410950, true);
    pool_set(guest, 9021, true);
    bool world1 = false, world_parting = false, world_apart = false, world2 = false, early_checked = false, early_world = false;
    auto swap_in = [&]() {  // her game swaps its flags for the host's as she joins
        memcpy(guest.home_pool, guest.pool, sizeof(guest.pool));
        memcpy(guest.pool, host.pool, sizeof(guest.pool));
    };
    auto swap_home = [&]() { memcpy(guest.pool, guest.home_pool, sizeof(guest.pool)); };
    auto flag_on = [](Game& x, uint32_t id) {
        g = &x;
        h_flag_set(id, 1);
        x.on_flag(id, 1, x.on_flag_user);
    };
    auto flag_of = [](Game& x, uint32_t id) {
        g = &x;
        int v = 0;
        h_flag_get(id, &v);
        return v;
    };
    // A blow that would kill: 1 HP left while the game's Player No Dead is on.
    auto lethal_hit = [](Game& x) { hp_of(x) = debug_flag(x, addrs::kPlayerNoDead) ? 1 : 0; };
    auto camera = [](const Game& x) { return get<uint64_t>(x.dbg, 0x108); };
    for (int frame = 0; frame < 60 * 300; ++frame) {
        g_clock += 1.0 / 60.0;
        if (frame == 300) {  // settled for 2 s already: only the cutscene held the bells back
            bells_waited = guest.bell_lots == 0;
            g = &guest;
            h_flag_set(addrs::kCutscene, 0);
        }
        for (Game& x : G) {
            g = &x;
            if (!x.in_world && --x.load_frames <= 0) {  // a load ends
                x.in_world = true;
                x.map = x.pending_map;
                put<uint32_t>(x.entries, 8, x.map);
                if (hp_of(x) <= 1) hp_of(x) = 800;  // the game makes the player whole
                if (x.on_world) x.on_world(x.map, x.on_world_user);
                g_clock += 1e-4;  // the frame comes a moment later, as in bbport
            }
            if (x.bell_frames > 0 && --x.bell_frames == 0) x.effect = &x == &host ? addrs::kBeckoningEffect : addrs::kSmallResonantEffect;
            x.frame(x.frame_user);
        }
        const double t = g_clock;
        if (sent_home_at && !early_checked && guest.in_world) {  // the load home just placed her
            early_checked = true;
            early_world = pool_get(guest, 12411010) && pool_get(guest, 12411090) && pool_get(guest, 1142) && !pool_get(guest, 1141);
            printf("%7.2f ----  her map's events, as the load placed her: a door of hers shuts again\n", t - 1000.0);
            pool_set(guest, 12411090, false);
        }
        if (matches == 1 && matched_at && !world1 && t - matched_at > 8.0) {
            world1 = true;
            printf("%7.2f ----  the host's world, together: a door opens, an NPC moves on (1141 -> 1142), a talk; "
                   "an item taken, a co-op flag, a chalice flag\n", t - 1000.0);
            constexpr uint32_t kOn[] = {12411010, 12411090, 1142, 72400362, 52410900, 4047, 9022};
            for (uint32_t id : kOn) pool_set(host, id, true);
            pool_set(host, 1141, false);
        }
        if (sent_home_at && !world_parting && t - sent_home_at > 2.0) {  // in his compare as they part, after her load pass
            world_parting = true;
            printf("%7.2f ----  the host's world, as they part: four doors open\n", t - 1000.0);
            for (uint32_t id = 12411080; id < 12411084; ++id) pool_set(host, id, true);
        }
        if (sent_home_at && !world_apart && t - sent_home_at > 8.0) {
            world_apart = true;
            printf("%7.2f ----  the host's world, apart: another door opens\n", t - 1000.0);
            pool_set(host, 12411020, true);
        }
        if (second_at && !world2 && t - second_at > 5.0) {
            world2 = true;
            printf("%7.2f ----  the host's world, together again: a door, the NPC moves on (1142 -> 1143), the co-op flag off\n",
                   t - 1000.0);
            pool_set(host, 12411050, true);
            pool_set(host, 1142, false);
            pool_set(host, 1143, true);
            pool_set(host, 4047, false);
        }
        memcpy(host.other_pos, guest.pos, sizeof(host.pos));  // each screen shows the partner where they stand
        memcpy(guest.other_pos, host.pos, sizeof(host.pos));
        // The pretend server and game: two bells on one map make a session;
        // the guest loads into the host's world.
        if (!matched_at && host.in_world && guest.in_world && host.effect && guest.effect && host.map == guest.map) {
            matched_at = t;
            ++matches;
            host.effect = guest.effect = 0;
            set_state(host, 0, addrs::kStateHosting, true);
            set_state(guest, 1, addrs::kStateJoined, true);
            swap_in();
            load_into(guest, host.map, 120);
            guest.pos[0] = host.pos[0] + 50.0f;  // the game puts her where her sign was
            warps_at_match = guest.warps;
            if (matches == 1) {  // her copy of the host's world's items (other ids; one of his is not near her)
                add_item(guest, 0, 0x23, 52420230, -1);
                add_item(guest, 1, 0x13, 17100, 17101);
                add_item(guest, 2, 0x33, 17200, -1);
                add_item(guest, 3, 0x63, 17300, -1);  // the one he took before: still lying in her copy
                add_item(guest, 6, 0x73, 52410800, -1);  // the one she took at home: lying again (the host's flags)
            }
            printf("%7.2f ----  matched (%d)\n", t - 1000.0, matches);
            if (matches == 2) second_at = t;
            if (matches == 3) third_at = t;
        }
        if (matched_at && moved_for < matches && t - matched_at > 4.0) {
            moved_for = matches;
            join_moves += guest.warps - warps_at_match == 1;
        }
        if (matches == 1 && matched_at && !arrived_lamp && guest.in_world && t - matched_at > 3.0) {
            arrived_lamp = true;
            printf("%7.2f ----  the host's world brings its lit lamp along as she arrives\n", t - 1000.0);
            flag_on(guest, addrs::lamp_lit_flag(kLamp0));
        }
        if (matches == 1 && matched_at && !lamps_lit && t - matched_at > 36.0) {  // after the boss: she is up
            lamps_lit = true;
            printf("%7.2f ----  the guest lights a lamp, the host another\n", t - 1000.0);
            flag_on(guest, addrs::lamp_light_flag(kLampHers));
            flag_on(host, addrs::lamp_light_flag(kLampHis));
        }
        if (matches == 1 && matched_at && !hit1 && t - matched_at > 12.0) {
            hit1 = true;
            printf("%7.2f ----  the guest takes a killing blow, 10 m from the host\n", t - 1000.0);
            guest.pos[0] = host.pos[0] + 10.0f;
            lethal_hit(guest);
        }
        if (hit1 && !checked_down && t - matched_at > 14.0) {
            checked_down = true;
            was_down = hp_of(guest) == 1;
            camera_followed = camera(guest) == reinterpret_cast<uint64_t>(guest.other);
            shown_on_host = host.other[addrs::kChrFlags1] & addrs::kChrDraw;
            unseen_on_host = !enemies_see(host, host.other) && enemies_see(host, host.player) && enemies_see(guest, guest.other);
            hidden_from_enemies = debug_flag(guest, addrs::kPlayerHide) && debug_flag(guest, addrs::kPlayerSilence) &&
                                  (guest.player[addrs::kChrFlags1] & addrs::kChrInvincible) &&
                                  (guest.player[addrs::kChrFlags1] & addrs::kChrDraw) &&
                                  (guest.data[addrs::kDataFlags] & addrs::kDataNoDamage) && !enemies_see(guest, guest.player);
            still = guest.player[addrs::kChrFlags2] & addrs::kChrNoMove;
            knelt = guest.no_control && (guest.anim == addrs::kKneelAnim || guest.anim == addrs::kKneelAnim + 10 ||
                                         guest.anim == addrs::kKneelAnim + 20);
        }
        if (hit1 && !kneel_checked && t - matched_at > 17.5) {
            kneel_checked = true;
            kneel_held = guest.no_control && guest.loop_anim == guest.anim + 1 && hp_of(guest) == 1;
        }
        if (matches == 1 && matched_at && !party_checked && t - matched_at > 10.0) {
            party_checked = true;
            party_up = get<int32_t>(host.enemy_data, addrs::kDataMaxHp) == 1200 && get<int32_t>(host.enemy_data, addrs::kDataHp) == 1200 &&
                       get<int32_t>(guest.enemy_data, addrs::kDataMaxHp) == 1200;
        }
        if (matches == 1 && matched_at && !party_hit && t - matched_at > 40.0) {
            party_hit = true;
            put<int32_t>(host.enemy_data, addrs::kDataHp, 600);  // half its health gone
        }
        if (sent_home_at && !apart_insight && t - sent_home_at > 4.0) {
            apart_insight = true;
            get_gesture(host, 15);  // and a gesture
            printf("%7.2f ----  apart, the host's world gives him 1 Insight\n", t - 1000.0);
            ++host.insight;
        }
        if (frame == 400) {  // his bell rings (free): the game marks a refund due if nobody comes
            refund_rang = true;
            flag_on(host, addrs::kBellRefund);
        }
        if (frame == 402) refund_kept_off = refund_rang && flag_of(host, addrs::kBellRefund) == 0;
        if (sent_home_at && !party_back_checked && t - sent_home_at > 8.0) {
            party_back_checked = true;
            party_back = get<int32_t>(host.enemy_data, addrs::kDataMaxHp) == 1000 && get<int32_t>(host.enemy_data, addrs::kDataHp) == 500;
        }
        if (matches == 1 && matched_at && !picked && t - matched_at > 20.0) {
            picked = true;
            printf("%7.2f ----  the host picks up three things (a five-row lot, a one-row lot, an NPC set's piece)\n", t - 1000.0);
            guest.fail_awards = 1;  // her first award fails: tried again, not lost
            game_pick_up(host, 0x13);
            game_pick_up(host, 0x23);
            game_pick_up(host, 0x33);
        }
        // The host's world gives him 3 Insight (event 9350: a point every 10 frames),
        // then he uses a Madman's Knowledge (one fewer, 1 Insight more, at once).
        if (matches == 1 && matched_at && world_insight < 3 && t - matched_at > 22.0 + world_insight / 6.0) {
            if (!world_insight) printf("%7.2f ----  the host's world gives him 3 Insight\n", t - 1000.0);
            ++world_insight;
            ++host.insight;
        }
        if (matches == 1 && matched_at && !host_gesture && t - matched_at > 24.0) {
            host_gesture = true;
            printf("%7.2f ----  Eileen gives the host a gesture (4)\n", t - 1000.0);
            get_gesture(host, 4);
        }
        if (matches == 1 && matched_at && !npc_gift && t - matched_at > 27.0) {
            npc_gift = true;
            printf("%7.2f ----  an NPC's gift to the host (lot 7000); an award to all players (lot 7001) on both\n", t - 1000.0);
            game_award(host, 7000);
            game_award(host, 7001);
            game_award(guest, 7001);
        }
        if (matches == 1 && matched_at && !madman && t - matched_at > 26.0) {
            madman = true;
            printf("%7.2f ----  the host uses a Madman's Knowledge\n", t - 1000.0);
            set_goods(host, kMadmansKnowledge, 1);
            ++host.insight;
        }
        if (matches == 1 && matched_at && !boss_at && t - matched_at > 30.0) {
            boss_at = t;
            printf("%7.2f ----  Father Gascoigne dies\n", t - 1000.0);
            for (Game& x : G) {  // the boss clear, then the flag past the setters on_flag hears
                g = &x;
                BbHookCtx ctx = {};
                if (x.clear.fn) x.clear.fn(&ctx, x.clear.user);
                h_flag_set(kBoss, 1);
            }
            game_award(host, 31000);  // his defeat event's drop (hers skips it: a visitor)
            game_award(host, 5910);   // his role lot, for being helped
        }
        if (boss_at && !guest_picked && t - boss_at > 5.0) {
            guest_picked = true;
            printf("%7.2f ----  the guest picks up two things (one lies near the host too, one not)\n", t - 1000.0);
            game_pick_up(guest, 0x13);
            game_pick_up(guest, 0x33);
        }
        if (boss_at && !late_copy && t - boss_at > 8.0) {
            late_copy = true;
            printf("%7.2f ----  her copy of the one-row lot the host took comes into view\n", t - 1000.0);
            add_item(guest, 5, 0x53, 55000, -1);
        }
        if (boss_at && !checked_up && t - boss_at > 2.0) {
            checked_up = true;
            revived = hp_of(guest) == 800 && !debug_flag(guest, addrs::kPlayerHide) && (guest.player[addrs::kChrFlags1] & addrs::kChrDraw) &&
                      !(guest.player[addrs::kChrFlags1] & addrs::kChrInvincible) && !(guest.player[addrs::kChrFlags2] & addrs::kChrNoMove) &&
                      !(guest.data[addrs::kDataFlags] & addrs::kDataNoDamage);
            camera_back = camera(guest) == 0;
            stood_up = !guest.no_control && guest.anim == guest.loop_anim + 1;
            shown_again = host.other[addrs::kChrFlags1] & addrs::kChrDraw;
            seen_again = enemies_see(host, host.other) && enemies_see(guest, guest.player);
        }
        if (boss_at && !sent_home_at && t - boss_at > 15.0) {  // BlockClear2_3: home
            sent_home_at = t;
            printf("%7.2f ----  the guest is sent home\n", t - 1000.0);
            set_state(host, 0, 0, false);
            set_state(guest, 0, 0, false);
            g = &guest;
            h_flag_set(kBoss, 0);  // her own world comes back without the kill
            h_flag_set(addrs::lamp_lit_flag(kLamp0), 0);  // and without the host's lamps
            h_flag_set(addrs::lamp_light_flag(kLampHers), 0);
            swap_home();
            load_into(guest, guest.home_map, 150);
            matched_at = 0;
        }
        // The second meeting: for 6 s the host's game sets his enemy's max HP
        // back every frame (as it does for some): its HP is not raised again.
        if (second_at && t - second_at > 3.0 && t - second_at < 9.0) {
            if (get<int32_t>(host.enemy_data, addrs::kDataMaxHp) != 1000) put<int32_t>(host.enemy_data, addrs::kDataHp, 500);
            put<int32_t>(host.enemy_data, addrs::kDataMaxHp, 1000);
        }
        if (second_at && !guest_gesture && t - second_at > 6.0) {
            guest_gesture = true;
            printf("%7.2f ----  an NPC gives the guest a gesture (14)\n", t - 1000.0);
            get_gesture(guest, 14);
        }
        if (second_at && !stubborn_checked && t - second_at > 9.0) {
            stubborn_checked = true;
            stubborn = get<int32_t>(host.enemy_data, addrs::kDataHp) == 500;
        }
        if (second_at && !hit_host && t - second_at > 12.0) {
            hit_host = true;
            printf("%7.2f ----  the host takes a killing blow, 10 m from the guest\n", t - 1000.0);
            guest.pos[0] = host.pos[0] + 10.0f;
            lethal_hit(host);
        }
        if (second_at && !hit_guest && t - second_at > 16.0) {
            hit_guest = true;
            printf("%7.2f ----  the guest takes a killing blow too\n", t - 1000.0);
            lethal_hit(guest);
        }
        if (second_at && !respawned && t - second_at > 20.0) {
            both_released = host.force_deads == 1 && guest.force_deads == 1 && !debug_flag(host, addrs::kPlayerNoDead) &&
                            !debug_flag(guest, addrs::kPlayerNoDead) && hp_of(host) == 0 && hp_of(guest) == 0;
            nobody_left = host.leaves == 0 && guest.leaves == 0;
            matches_before = matches;
            respawned = true;
            printf("%7.2f ----  both respawn: the host at his lamp, the guest at home\n", t - 1000.0);
            set_state(host, 0, 0, false);
            set_state(guest, 0, 0, false);
            load_into(host, host.map, 150);
            swap_home();
            load_into(guest, guest.home_map, 150);
            matched_at = 0;
        }
        // The third meeting: she goes down 10 m off; the host comes close
        // without a Blood Vial (greyed: nothing happens), then with two and
        // presses (she is up, one used); down again, he walks off and lights a
        // lamp (up, free); down again, he goes to another lamp (its prompt
        // shows: up, free); down a last time, he stands by her 3 s with his last
        // vial (its stack taken out).
        if (third_at && !down3 && t - third_at > 5.0) {
            down3 = true;
            far_no_prompt = host.prompts == 0;
            printf("%7.2f ----  the guest takes a killing blow; the host comes close, no Blood Vial\n", t - 1000.0);
            guest.pos[0] = host.pos[0] + 10.0f;
            lethal_hit(guest);
            set_vials(host, 0);
        }
        if (down3 && !near3 && t - third_at > 6.0) {
            near3 = true;
            memcpy(host.pos, guest.pos, sizeof(host.pos));
            host.pos[0] += 1.0f;
        }
        if (near3 && !tried_greyed && t - third_at > 6.5) {
            tried_greyed = true;
            host.press = true;
        }
        if (tried_greyed && !tried_vials && t - third_at > 10.0) {  // 3.5 s by her: greyed, nothing
            tried_vials = true;
            greyed_kept_down = hp_of(guest) == 1 && host.greyed_prompts > 0 && !host.press;
            printf("%7.2f ----  the host steps off, has two Blood Vials, comes back and presses Pick Up\n", t - 1000.0);
            set_vials(host, 2);
            host.pos[0] = guest.pos[0] + 20.0f;
        }
        if (tried_vials && !came_back && t - third_at > 10.2) {  // 0.8 s by her: only the press gets her up
            came_back = true;
            memcpy(host.pos, guest.pos, sizeof(host.pos));
            host.pos[0] += 1.0f;
            host.press = true;
        }
        if (tried_vials && !down4 && t - third_at > 11.0) {
            down4 = true;
            picked_up = hp_of(guest) == 800 && vials(host) == 1 && host.stack_removes == 0 && !debug_flag(guest, addrs::kPlayerHide) &&
                        camera(guest) == 0;
            printf("%7.2f ----  the guest goes down again; the host walks off and lights a lamp\n", t - 1000.0);
            host.pos[0] = guest.pos[0] + 20.0f;
            lethal_hit(guest);
        }
        if (down4 && !lamp3 && t - third_at > 12.0) {
            lamp3 = true;
            flag_on(host, addrs::lamp_light_flag(kLampRevive));
        }
        if (lamp3 && !down5 && t - third_at > 14.0) {
            down5 = true;
            lamp_up = hp_of(guest) == 800 && vials(host) == 1 && !debug_flag(guest, addrs::kPlayerHide);
            printf("%7.2f ----  down again; the host goes to a lit lamp\n", t - 1000.0);
            lethal_hit(guest);
            host.pos[0] = guest.pos[0] + 20.0f;
        }
        if (down5 && !down6 && t - third_at > 14.5 && t - third_at < 15.0 && host.prompt.fn) {  // its talk script asks the prompt
            BbHookCtx ctx = {};
            ctx.arg[2] = 6001;
            g = &host;
            host.prompt.fn(&ctx, host.prompt.user);
        }
        if (down5 && !down6 && t - third_at > 16.0) {
            down6 = true;
            prompt_up = hp_of(guest) == 800 && vials(host) == 1 && !debug_flag(guest, addrs::kPlayerHide);
            printf("%7.2f ----  down a last time; the host stands by her with his last Blood Vial, then presses\n", t - 1000.0);
            lethal_hit(guest);
            memcpy(host.pos, guest.pos, sizeof(host.pos));
            host.pos[0] += 1.5f;
        }
        if (down6 && !held_checked && t - third_at > 20.0) {  // 4 s by her, nothing pressed: she stays down
            held_checked = true;
            held = hp_of(guest) == 1 && vials(host) == 1;
            host.press = true;
        }
        if (held && !last_vial && t - third_at > 21.5) {
            last_vial = hp_of(guest) == 800 && vials(host) == 0 && host.stack_removes == 1;
            host.pos[0] = guest.pos[0] + 20.0f;
        }
        if (third_at && !returned && t - third_at > 23.0) {
            returned = true;
            leaves_before = guest.leaves;
            printf("%7.2f ----  the guest picks Return to the Hunter's Dream at a lamp\n", t - 1000.0);
            flag_on(guest, addrs::lamp_return_flag(kLamp0));
        }
        if (returned && !left_at && guest.leaves > leaves_before) {  // the game's Leave: she goes home
            left_at = t;
            printf("%7.2f ----  the guest leaves the session\n", t - 1000.0);
            set_state(host, 0, 0, false);
            set_state(guest, 0, 0, false);
            swap_home();
            load_into(guest, guest.home_map, 150);
        }
        if (left_at && (guest.lamp_warps || t - left_at > 20.0)) break;
        if (returned && !left_at && t - third_at > 47.0) break;
    }
    g = &guest;
    int gascoigne = 0;
    h_flag_get(kBoss, &gascoigne);
    int lamp = 0;
    h_flag_get(2412, &lamp);
    int taken_a = 0, taken_b = 0, taken_npc = 0, taken_d = 0, taken_d2 = 0, taken_e = 0;
    h_flag_get(52420230, &taken_a);
    h_flag_get(55000, &taken_b);
    h_flag_get(50000205, &taken_npc);
    h_flag_get(17100, &taken_d);
    h_flag_get(17101, &taken_d2);
    h_flag_get(17200, &taken_e);
    g = &host;
    int host_e = 0;
    h_flag_get(17200, &host_e);
    const bool her_lamps_home = flag_of(guest, addrs::lamp_lit_flag(kLampHers)) == 1 && flag_of(guest, addrs::lamp_lit_flag(kLampHis)) == 1;
    const bool arrival_kept_out = flag_of(guest, addrs::lamp_lit_flag(kLamp0)) == 0;
    const bool host_got_hers = flag_of(host, addrs::lamp_lit_flag(kLampHers)) == 1;
    const bool return_off = flag_of(guest, addrs::lamp_return_flag(kLamp0)) == 0;
    auto count = [](const int* a, int n, int v) {
        int c = 0;
        for (int i = 0; i < n; ++i) c += a[i] == v;
        return c;
    };
    auto given = [&](int lot) { return count(guest.lots, guest.lot_count, lot); };
    auto host_given = [&](int lot) { return count(host.lots, host.lot_count, lot); };
    printf("\nhost: %d item rings, %d event rings; guest: %d travels, %d item rings, %d event rings, lots given:", host.item_rings,
           host.event_rings, guest.travels, guest.item_rings, guest.event_rings);
    for (int i = 0; i < guest.lot_count; ++i) printf(" %d", guest.lots[i]);
    printf("\n");
    expect(guest.travels >= 2, "the guest travelled to the host before each join");
    expect(join_moves == matches, "each join brought her to the host's side from her sign's spot");
    expect(count(guest.removed, guest.removes, 52410800) == 1, "the item she took in her own world went from her view in his");
    expect(host.item_rings + host.event_rings >= 2 && guest.item_rings + guest.event_rings >= 2, "both rang before each join");
    expect(matches_before == 2, "they met, were split by the boss, and met again");
    expect(gascoigne == 1, "the boss killed together is dead in the guest's own world");
    expect(lamp == 1, "its lamp is lit in the guest's own world");
    expect(guest.aligned_errors == 0, "the warp's vectors were 16-byte aligned");
    expect(nobody_left, "nobody left on their own (before the lamp)");
    expect(was_down, "a killing blow left the guest down at 1 HP");
    expect(knelt && kneel_held, "down: she knelt (down, then held) with no control");
    expect(stood_up, "up again: she stood up and has control back");
    expect(hidden_from_enemies && still, "down: unhurt, invincible, hidden (her enemies' sight too), silent, still drawn, still");
    expect(camera_followed, "down: the camera followed the host");
    expect(shown_on_host, "the host's screen still showed the guest while she was down (to pick her up)");
    expect(revived, "the boss's death brought her back up at full HP with every bit put back");
    expect(camera_back && shown_again, "the camera came back to her; the host sees her again");
    expect(unseen_on_host && seen_again, "the host's enemies ignored her only while she was down");
    expect(given(31000) == 1 && host_given(31000) == 1, "the guest got the boss's drop once, as his game gave him his");
    expect(given(7000) == 1 && flag_of(guest, 72401000) == 1,
           "an NPC's gift to the host while together: hers too, and taken in her own world");
    expect(given(7001) == 1 && host_given(7001) == 1, "an award both games gave: no second one for either");
    expect(given(5910) == 0, "the host's role lot (for being helped): his alone");
    expect(count(guest.picked, guest.pick_count, 52420230) == 1 && given(2420230) == 0,
           "the host's pickup lying near her: she picked up her copy of it");
    expect(given(5500) == 1 && count(guest.removed, guest.removes, 55000) == 1 && guest.lot_count == 5,
           "one not near her: its lot (a failed award tried again); her copy went when it came into view");
    expect(count(guest.removed, guest.removes, 17300) == 1 && guest.removes == 3,
           "what his world had taken before she came went from her view, and nothing else (but hers)");
    expect(count(host.picked, host.pick_count, 17100) == 1 && host_given(1720) == 1 && host_e == 1 && host.lot_count == 5,
           "her pickups: the host picked up his copy, or got its lot and flag");
    expect(given(1710) == 0 && given(1720) == 0 && guest.pick_count == 3, "nothing came back to her twice");
    expect(taken_a && taken_b && taken_d && taken_d2 && taken_e && !taken_npc,
           "every pickup is taken in her own world; the NPC set's piece is left alone");
    expect(both_released, "host down, then guest down: both died for real");
    expect(host_got_hers, "the lamp she lit is lit in the host's world");
    expect(her_lamps_home, "both lamps lit together are lit in her own world");
    expect(arrival_kept_out, "the host's lamps lit before she came stay his");
    expect(matches == 3 && left_at && guest.leaves == leaves_before + 1 && host.leaves == 0, "her Return to the Hunter's Dream took her out of the session");
    expect(guest.lamp_warps == 1 && guest.lamp_to == kLamp0.dream && host.lamp_warps == 0, "and from home to her own Hunter's Dream");
    expect(return_off, "the lamp's Return flag was turned off again");
    expect(far_no_prompt, "no pick-up prompt while far from her");
    expect(greyed_kept_down, "without a Blood Vial the prompt was greyed and she stayed down");
    expect(picked_up, "with two Blood Vials he picked her up: back on her feet, one vial used");
    expect(lamp_up, "a lamp he lit while she was down got her up, no vial used");
    expect(prompt_up, "at another lamp (its prompt showing) while she was down: she was up, no vial used");
    expect(held && last_vial, "standing by her did nothing; pressing Pick Up with his last Blood Vial: she was up and its stack was taken out");
    expect(party_up, "together, the enemy had 120% health on both screens");
    expect(stubborn, "an enemy whose max HP the game keeps setting was left alone (its HP not raised again and again)");
    expect(party_back, "once she was gone, the host's enemy had its own health again (the damage kept in proportion)");
    size_t row_bytes = 0;
    const unsigned char* spare = static_cast<const unsigned char*>(h_param_row("ItemLotParam", addrs::kSpareLot, &row_bytes));
    expect(bells_waited && guest.bell_lots == 2 && goods(guest, addrs::kBeckoningBell) == 1 &&
               goods(guest, addrs::kSmallResonantBell) == 1 && goods(guest, kSilencingBlank) == 1,
           "her new character got both bells (and the Blank) after its first wake, once, through every later load");
    expect(host.bell_lots == 1 && goods(host, addrs::kSmallResonantBell) == 1 && goods(host, addrs::kBeckoningBell) == 1,
           "his character got only the bell it lacked");
    expect(flag_of(guest, addrs::kBellsGiven) && flag_of(guest, addrs::kSmallBellSold) && flag_of(host, addrs::kSmallBellSold),
           "the messenger's gift and the shop's bell are marked given: the game gives no second one");
    expect(get<int32_t>(spare, addrs::kLotItemId) == 0 && spare[addrs::kLotNum] == 0, "the lent lot is empty again");
    expect(g_guest_took_rules, "her enemy health (150) gave way to the host's (120)");
    char insight_txt[64] = "";
    {
        char path[700];
        snprintf(path, sizeof(path), "%s/insight.txt", guest.dir);
        if (FILE* f = fopen(path, "r")) {
            if (!fgets(insight_txt, sizeof(insight_txt), f)) insight_txt[0] = 0;
            fclose(f);
        }
    }
    printf("Insight: host %lld, guest %lld (insight.txt: %s)\n", static_cast<long long>(host.insight),
           static_cast<long long>(guest.insight), insight_txt[0] ? insight_txt : "none\n");
    char gift[700];
    snprintf(gift, sizeof(gift), "%s/gift.txt", guest.dir);
    expect(host.insight == 6 && guest.insight == 4 && strstr(insight_txt, " 3\n"),
           "the 3 Insight his world gave him together: hers too, once; his item and what came while apart: his alone");
    expect(access(gift, F_OK) != 0 && has_gesture(guest, 20) && given(50000010) == 1 && flag_of(guest, 12411700) == 1,
           "gift.txt gave her its 1 Insight, gesture 20 and Cleric Beast (its drop, dead in her world), and went");
    expect(has_gesture(guest, 4) && !has_gesture(guest, 15) && !has_gesture(guest, 0),
           "the gesture he got together is hers too; the one he got apart and the one he had before are not");
    expect(has_gesture(host, 14), "the gesture she got together is his too");
    expect(refund_kept_off, "a free bell gives no Insight back: its refund flag was turned off again");
    expect(early_world, "world: what his world did together was in hers as the load home placed her");
    expect(pool_get(guest, 12411010) && pool_get(guest, 72400362) && pool_get(guest, 12411050) && pool_get(guest, 1143) &&
               !pool_get(guest, 1141) && !pool_get(guest, 1142),
           "world: her world has every change his made together (two doors, a talk, the NPC moved on twice)");
    expect(!pool_get(guest, 52410900) && !pool_get(guest, 4047) && !pool_get(guest, 9022),
           "world: not his item flag, the co-op flag nor the chalice one");
    expect(pool_get(guest, 12411030) && !pool_get(guest, 12411040) && !pool_get(guest, 72500100) && !pool_get(guest, 12411060),
           "world: the catch-up made her world his (his old door open; her own door, talk and older held door gone)");
    expect(pool_get(guest, 52410950) && pool_get(guest, 9021), "world: her item flags and her chalice dungeon stayed hers");
    expect(!pool_get(guest, 12411020), "world: what his world did apart, after the catch-up, is not hers");
    expect(pool_get(guest, 12411080) && pool_get(guest, 12411083),
           "world: what his world did as they parted, come after her load had placed her, went in as it settled");
    expect(!pool_get(guest, 12411090), "world: what her map's events changed after the load placed her stayed (not set again as it settled)");
    {
        char path[700];
        snprintf(path, sizeof(path), "%s/catchup.txt", host.dir);
        expect(access(path, F_OK) != 0, "world: the catch-up went once (catchup.txt gone)");
    }
    auto lines = [](const Game& x, const char* name) {
        char path[700], line[64];
        snprintf(path, sizeof(path), "%s/%s", x.dir, name);
        FILE* f = fopen(path, "r");
        int n = 0;
        while (f && fgets(line, sizeof(line), f)) n += strncmp(line, "run ", 4) != 0;
        if (f) fclose(f);
        return f ? n : -1;
    };
    expect(lines(host, "world-out.txt") == 0 && lines(guest, "world.txt") == 0,
           "world: she had every change (the host holds none back) and none waits any more");
    return g_failures ? 1 : 0;
}
