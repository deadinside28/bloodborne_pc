// bbcoop: seamless-style co-op for two players on a private server, as a
// bbhost (v0.2.17, plugin API 11) plugin. Both machines run it; one is the
// host, whose world the two play in, the other the guest.
//
//   1. Free bells: the Beckoning and Old Hunter's Bells cost no Insight; the
//      plugin rings the right bell by itself while the two are apart. A new
//      character has both bells as soon as it wakes up in the clinic.
//   2. Summon anywhere: bells work where the boss is dead and across maps
//      (Wozzardman's 12 byte patches); before ringing, the guest travels to
//      the host's map and spot through the game's own forced-summon warp.
//   3. Staying together: after any drop (a death, a boss, travel) the guest
//      comes back by itself; optionally a boss kill no longer sends her home.
//      Lamps work while together, for both: one lit is lit in both worlds,
//      and the guest's Return to the Hunter's Dream takes her to her own.
//   4. Boss progress: bosses killed together are marked dead in the guest's
//      own world once she is back in it, with what their defeat unlocks (the
//      lamp); the guest is given the boss's drop as it dies. Both pick up
//      in the host's world, each their own copy of every item, as each
//      one's own world has it. The Insight the host's world gives him while
//      together (a boss seen or killed, a place found) she gets too, and a
//      gesture either one gets while together (an NPC's) the other gets too,
//      as does an item lot the game gives either (a boss's drop, an NPC's gift).
//      Every other lasting change the host's world makes while together (a
//      door or shortcut opened, an NPC's quest moved on, the time of day)
//      goes into the guest's world as she is next home.
//   5. Experimental: loads no longer end the session.
//   6. Elden Ring style deaths: while together nobody dies. The one who would
//      is down: out of the fight, watching the partner, until the boss dies
//      (back up), the partner goes down too (both die for real) or the
//      session ends. The partner's screen shows them where they fell; its
//      enemies ignore them. Near the one down, the partner's "Pick Up Item"
//      prompt gets them up for one Blood Vial; lighting a lamp, for free.
//
// The two plugins talk directly over UDP on the LAN (a heartbeat four times a
// second: where each one is, what it is doing, the bosses killed together);
// nothing goes through any server but the private one the game uses.
// Every patch, hook and decision is logged with a "bbcoop:" prefix.
#include "bbhost_plugin.h"

#include "addrs.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <xmmintrin.h>

extern "C" BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION,
    BB_PLUGIN_OPT_IN | BB_PLUGIN_GAMEPLAY,
    "bbcoop",
    "Seamless co-op",
    "0.10.2",
    "bbcoop",
    "Two-player co-op that keeps itself together: free bells, summons anywhere, re-joining after drops, "
    "boss kills, drops and Insight shared with the guest, gestures shared, pickups for both, Elden Ring style deaths with picking the partner up, lamps while together.",
};

extern "C" BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_HEADING, nullptr, "The two of you", "Set this on both machines."},
    {BB_OPT_CHOICE | BB_OPT_LIVE, "role", "This machine is the", "You play in the host's world.", "host", 0, 0, "host|guest"},
    {BB_OPT_TEXT | BB_OPT_LIVE, "partner", "Partner's address", "The other machine's LAN IP, e.g. 192.168.1.130 (add :port if it listens on another).", ""},
    {BB_OPT_INT, "port", "Link port (UDP)", "The same on both machines (from the next start).", "9311", 1024, 65535},
    {BB_OPT_HEADING, nullptr, "1. Bells"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "free_bells", "Free bells", "The Beckoning and Old Hunter's Bells cost no Insight; the Small Resonant Bell is free in the Insight shop.", "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "give_bells", "Bells from the start",
     "A character has the Beckoning and Small Resonant Bells as soon as it wakes up in the clinic (the game gives them "
     "later, in the Hunter's Dream, with Insight), and the game gives no second one.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "auto_ring", "Ring for me", "Rings the right bell by itself whenever you two are apart.", "true"},
    {BB_OPT_HEADING, nullptr, "2. Summon anywhere"},
    {BB_OPT_BOOL, "summon_anywhere", "Summon anywhere", "Bells work where the boss is dead and across maps (from the next start).",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "follow_host", "Guest travels to the host",
     "Before ringing, the guest is taken to the host's map and spot. Never out of the Hunter's Dream.", "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "join_at_host", "Guest arrives next to the host",
     "As she joins, the guest is moved to the host's side (the game puts her where she rang).", "true"},
    {BB_OPT_HEADING, nullptr, "3. Staying together"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "auto_rejoin", "Re-join after drops",
     "After a death, a boss or travel, the guest comes back by itself. Off: only the first join is automatic.", "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "keep_after_boss", "Stay after a boss",
     "A boss kill no longer sends the guest home. The new lamp can be lit while together (Lamps while together).", "false"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "lamps", "Lamps while together",
     "Both of you can use lamps while together. A lamp lit by either is lit in both worlds (in the guest's once she is "
     "home). Return to the Hunter's Dream ends the session: the host goes as always, the guest to her own Dream. The host's "
     "choice counts.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "guest_talk", "The guest can talk",
     "In the host's world the guest talks to NPCs and uses lamps as at home (the game refuses a visitor).", "true"},
    {BB_OPT_INT | BB_OPT_LIVE, "rejoin_delay", "Seconds at home before re-joining", nullptr, "2", 1, 120},
    {BB_OPT_INT | BB_OPT_LIVE, "pause_seconds", "Leave now pauses re-joining for (s)", nullptr, "45", 10, 900},
    {BB_OPT_ACTION, "leave", "Leave now", "Ends the session and pauses re-joining for a while (light a lamp, shop)."},
    {BB_OPT_ACTION, "join", "Join now", "Ends a pause and rings (and travels) at once."},
    {BB_OPT_INT | BB_OPT_LIVE, "party_health", "Enemy health while together (%)",
     "Every enemy has this much of its health while the two of you are together, and its own again once apart (100: as "
     "the game has it; not in chalice dungeons, which scale their bosses themselves). The host's choice counts.",
     "120", 100, 300},
    {BB_OPT_HEADING, nullptr, "4. Boss progress"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "boss_progress", "Bosses count for the guest",
     "Bosses killed together are marked dead in the guest's own world when she is back in it.", "true"},
    {BB_OPT_TEXT | BB_OPT_LIVE, "extra_flags", "More flags to carry over", "Event flag ids, separated by commas.", ""},
    {BB_OPT_BOOL | BB_OPT_LIVE, "share_drops", "Boss drops for the guest",
     "The guest gets the boss's drop too, as it dies (the game gives it to the host only).", "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "share_rewards", "Rewards for both",
     "An item the game gives one of you while together (a boss's drop, an NPC's gift) the other gets too, as it is "
     "given; not one your own game gave you too, nor one she had in her own world. Set it on both machines.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "share_world", "World progress for both",
     "Every lasting change the host's world makes while you are together (a door or shortcut opened, an NPC's quest "
     "moved on, a boss down, the time of day) goes into the guest's world too, as she is next home. Item pickups stay "
     "each one's own. Set it on both machines.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "share_insight", "Insight for the guest",
     "The Insight the host's world gives him while together (a boss seen or killed, a place found; not his own Insight "
     "items) the guest gets too, in place of the 1 the game gives a visitor for a boss. Set it on both machines.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "guest_pickups", "The guest picks up too",
     "In the host's world the guest has her own copy of every item (from her next load), as her own world has "
     "them: each of you picks up your own, and what she picks up is marked taken in her own world.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "share_pickups", "Pickups copied to the other",
     "Whatever one of you picks up while together, the other gets too, and items the host's world had taken "
     "go from her view. The host's choice counts.",
     "false"},
    {BB_OPT_HEADING, nullptr, "6. Deaths (Elden Ring style)"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "downed", "Down, not dead",
     "While together nobody dies: the one who would kneels where they fell, with no control, until the boss dies, the other picks them up "
     "or lights a lamp (back up), the other goes down too (both die) or the session ends. The host's choice counts.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "spectate", "Watch the partner while down", "Off: the camera stays where you went down.",
     "false"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "pick_up", "Pick up a partner who is down",
     "Next to them (2.5 m), the game's Revive prompt (Pick Up Item without the co-op mod) gets them up for 1 Blood Vial "
     "(not without one). Their character stays in sight while down. The host's choice counts.",
     "true"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "lamp_revive", "A lamp brings them back",
     "Going to a lamp (its prompt showing) or lighting one while the partner is down gets them up, free. The host's choice counts.", "true"},
    {BB_OPT_ACTION, "give_up", "Give up", "While down: die now (the host respawns at the lamp, the guest goes home)."},
    {BB_OPT_HEADING, nullptr, "5. Experimental"},
    {BB_OPT_BOOL, "seamless", "True seamless",
     "Loads no longer end the session; if the partner is not back 45 s after a load, re-join takes over (from the next start).",
     "false"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "banners", "Co-op messages", "Shown the way the game shows its own (with the co-op mod).", "true"},
    {BB_OPT_END}};

namespace {

const BbHostApi* api;

// --- settings ------------------------------------------------------------------

struct Settings {
    bool guest;
    char partner[64];
    int port;
    bool free_bells, give_bells, auto_ring, summon_anywhere, follow_host, join_at_host, auto_rejoin, keep_after_boss, lamps,
        guest_talk;
    int rejoin_delay, pause_seconds, party_health;
    bool boss_progress, seamless, banners, share_drops, guest_pickups, share_pickups, downed, spectate, pick_up, lamp_revive,
        share_insight, share_rewards, share_world;
    uint32_t extra_flags[32];
    int extra_count;
};
Settings S;

// The settings the game's System menu has (the co-op pages, below): each row's
// text id (tools/coop_messages.tsv; id / 1000 is its page), what its Defaults
// restores, the byte the row writes, and what the menu last set (-1: nothing),
// which wins over bbport.ini until the overlay changes that setting again.
struct MenuToggle {
    const char* key;
    uint32_t id;
    bool* setting;
    bool def;  // also bbport.ini's default
    uint8_t value = 0, last = 0;
    int8_t saved = -1;
};
MenuToggle g_toggles[] = {
    {"auto_ring", 125000, &S.auto_ring, true},
    {"auto_rejoin", 125001, &S.auto_rejoin, true},
    {"follow_host", 125002, &S.follow_host, true},
    {"join_at_host", 125003, &S.join_at_host, true},
    {"free_bells", 125004, &S.free_bells, true},
    {"give_bells", 125005, &S.give_bells, true},
    {"downed", 126000, &S.downed, true},
    {"pick_up", 126001, &S.pick_up, true},
    {"lamp_revive", 126002, &S.lamp_revive, true},
    {"spectate", 126003, &S.spectate, false},
    {"banners", 126004, &S.banners, true},
    {"keep_after_boss", 127000, &S.keep_after_boss, false},
    {"lamps", 127001, &S.lamps, true},
    {"guest_talk", 127002, &S.guest_talk, true},
    {"boss_progress", 127003, &S.boss_progress, true},
    {"guest_pickups", 128000, &S.guest_pickups, true},
    {"share_pickups", 128001, &S.share_pickups, false},
    {"share_drops", 128002, &S.share_drops, true},
};
// Enemy health together: a pick list (126005) of these, their texts 126100 on.
constexpr int32_t kHealth[] = {100, 110, 120, 130, 150, 200, 250, 300};
constexpr uint32_t kHealthRow = 126005, kHealthText = 126100;
int32_t g_health, g_health_last, g_health_saved;  // saved 0: nothing

void apply_menu_saved() {
    for (MenuToggle& t : g_toggles)
        if (t.saved >= 0) *t.setting = t.saved != 0;
    if (g_health_saved) S.party_health = g_health_saved;
}

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    api->log("bbcoop: %s", line);
}

const char* config(const char* key) {
    char full[96];
    snprintf(full, sizeof(full), "bbcoop.%s", key);
    const char* v = api->config(full);
    return v ? v : "";
}

bool config_bool(const char* key, bool fallback) {
    const char* v = config(key);
    if (!v[0]) return fallback;
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "on") == 0;
}

// A whole number in a range ("8.0" from a float writer too); anything else is the fallback.
long parse_long(const char* s, long fallback, long lo, long hi) {
    char* end;
    const long v = strtol(s, &end, 10);
    if (end == s) return fallback;
    if (*end == '.')
        while (*++end >= '0' && *end <= '9') {
        }
    while (*end == ' ' || *end == '\n' || *end == '\r') ++end;
    return *end || v < lo || v > hi ? fallback : v;
}

int config_int(const char* key, int fallback, int lo, int hi) {
    return static_cast<int>(parse_long(config(key), fallback, lo, hi));
}

void read_settings() {
    S.guest = strcmp(config("role"), "guest") == 0;
    snprintf(S.partner, sizeof(S.partner), "%s", config("partner"));
    S.port = config_int("port", 9311, 1024, 65535);
    for (const MenuToggle& t : g_toggles) *t.setting = config_bool(t.key, t.def);
    S.summon_anywhere = config_bool("summon_anywhere", true);
    S.seamless = config_bool("seamless", false);
    S.share_insight = config_bool("share_insight", true);
    S.share_rewards = config_bool("share_rewards", true);
    S.share_world = config_bool("share_world", true);
    S.rejoin_delay = config_int("rejoin_delay", 2, 1, 120);
    S.pause_seconds = config_int("pause_seconds", 45, 10, 900);
    S.party_health = config_int("party_health", 120, 100, 300);
    S.extra_count = 0;
    const char* s = config("extra_flags");
    while (*s && S.extra_count < 32) {
        char item[16];
        int n = 0;
        while (*s && *s != ',' && n < 15) item[n++] = *s++;
        item[n] = 0;
        while (*s && *s != ',') ++s;
        if (*s == ',') ++s;
        const long id = parse_long(item, -1, 1, 0xFFFFFFFFL);
        if (id > 0) S.extra_flags[S.extra_count++] = static_cast<uint32_t>(id);
    }
    apply_menu_saved();
}

// --- the game ------------------------------------------------------------------

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

uint64_t bn(uint64_t va) { return va + addrs::kBnSlide; }

// api->read takes a Binary Ninja address inside the image and a runtime one
// above it (the game's heap); both are checked reads.
bool rd(uint64_t at, void* out, size_t n) { return at && api->read(at, out, n) == 0; }
uint64_t rp(uint64_t at) {
    uint64_t v = 0;
    return rd(at, &v, 8) ? v : 0;
}
uint64_t singleton(uint64_t va) { return rp(bn(va)); }
// A pointer field of an object that may be missing (0 stays 0).
uint64_t field(uint64_t obj, uint64_t off) { return obj ? rp(obj + off) : 0; }

// No libm: the CPU's square root.
float root(float x) { return x > 0 ? _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))) : 0; }

bool call(const addrs::Function& f, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t* ret = nullptr) {
    BbCallRegs r = {{a0, a1, a2}};
    if (api->call_guest(bn(f.va), &r) != 0) return false;
    if (ret) *ret = r.ret;
    return true;
}

// call() with all six integer arguments, for what it returns (0 when it was not called).
uint64_t call6(const addrs::Function& f, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0,
               uint64_t a5 = 0) {
    BbCallRegs r = {{a0, a1, a2, a3, a4, a5}};
    return api->call_guest(bn(f.va), &r) == 0 ? r.ret : 0;
}

uint64_t addr(const void* p) { return reinterpret_cast<uint64_t>(p); }

bool g_menu_ok;  // the game's menu and text functions are the 1.09 code (addrs::kMenuFunctions)

// MSVC's wstring (the game's): its buffer or eight inline wchars at +0x00, the
// capacity at +0x18 and the allocator at +0x20, whose slot +0x70 frees.
void free_wstring(uint8_t* ws) {
    uint64_t cap = 0, ptr = 0, alloc = 0, vt = 0, fn = 0;
    memcpy(&cap, ws + 0x18, 8);
    memcpy(&ptr, ws, 8);
    memcpy(&alloc, ws + 0x20, 8);
    if (cap < 8 || !ptr || !alloc || !rd(alloc, &vt, 8) || !rd(vt + 0x70, &fn, 8) || !fn) return;
    reinterpret_cast<void (*)(uint64_t, uint64_t)>(fn)(alloc, ptr);
}

// A message's text from the game's repository (group 200 menu text, 201 line
// help, 204 dialogs, 30 event text), as every menu caption gets it: the
// repository's own UTF-16, which outlives the lookup. Null when the bundle has
// no such message (the co-op mod is not installed) or before the repository exists.
const char16_t* message_text(uint32_t group, uint32_t id) {
    if (!g_menu_ok || !singleton(addrs::kMsgRepositoryPtr)) return nullptr;
    alignas(16) uint8_t caption[0x40] = {};
    call6(addrs::kMsgText, addr(caption), group, id);
    const char16_t* t = nullptr;
    memcpy(&t, caption, 8);
    free_wstring(caption + 0x08);
    return t && t[0] ? t : nullptr;
}

// A co-op message on the screen, the way the game shows its own ("... returned to
// their own world"): the mod's event text `id`; without the mod, `text` in the
// port's banner. Main thread.
void note(uint32_t id, const char* text) {
    say("note: %s", text);
    if (!S.banners) return;
    if (message_text(addrs::kGroupEventText, id) && singleton(addrs::kEventTextOwnerPtr))
        call6(addrs::kShowEventText, id, 0);
    else
        api->show_message(text, 4.0f);
}

uint64_t main_player() {
    const uint64_t man = singleton(addrs::kWorldChrMan);
    return man ? rp(man + 0x60) : 0;
}

// The map the world is on, as the game's own placement code reads it
// (SelectNextMapPlacement): WorldRes's current entry. 0 when there is none.
uint32_t current_map() {
    const uint64_t list = singleton(addrs::kWorldRes);
    int32_t index = -1, count = 0;
    uint64_t array = 0, entries = 0;
    uint32_t map = 0;
    if (!list || !rd(list + 0x20, &index, 4) || !rd(list + 0x10, &array, 8) || index < 0 || !array) return 0;
    if (!rd(array + 0x18, &count, 4) || index >= count || !rd(array + 0x20, &entries, 8) || !entries) return 0;
    if (!rd(entries + static_cast<uint64_t>(index) * 0xA0 + 8, &map, 4) || map == 0xFFFFFFFFu) return 0;
    return map;
}

struct World {
    bool in_world;      // a player in a map (not the title, not a load)
    bool stable;        // ... for 3 s, with no transition pending
    uint32_t map, block;
    int32_t area;       // the SOS area (negative: its boss is dead)
    float pos[3], yaw;
    int32_t chr_type;   // 0 own world, 1 white phantom in someone's
    int32_t state;      // SessionManager+0x124
    uint64_t controller;
    bool in_dream;      // the Hunter's Dream or a chalice dungeon: nobody follows there
    bool transition;    // GameStateMan: a transition or forced warp pending
};

// Time-keeping across frames.
struct Clock {
    double world_since;  // the world (re)appeared or a world_load came
    bool world_seen;
    unsigned frame;
};
Clock C;

World observe(double t) {
    World w = {};
    w.state = -1;
    const uint64_t player = main_player();
    w.in_world = player && api->player_block(&w.block) == 0 && api->player_position(w.pos, &w.yaw) == 0;
    if (!w.in_world) {
        C.world_seen = false;
        return w;
    }
    if (!C.world_seen) {
        C.world_seen = true;
        C.world_since = t;
    }
    w.map = current_map();
    if (!w.map) w.map = w.block;
    rd(player + 0x78, &w.chr_type, 4);
    const uint64_t netman = singleton(addrs::kFrpgNetMan);
    if (netman) rd(netman + 0xA80, &w.area, 4);
    const uint64_t session = singleton(addrs::kSessionManager);
    if (session) {
        rd(session + 0x124, &w.state, 4);
        w.controller = rp(session + 0x18);
    }
    const uint64_t gs = singleton(addrs::kGameStateMan);
    uint8_t pending = 0, forced = 0;
    if (gs) {
        rd(gs + 0x08, &pending, 1);
        rd(gs + 0x1520, &forced, 1);
    }
    w.transition = pending || forced;
    const uint32_t area = w.map >> 24;
    w.in_dream = area == addrs::kAreaDream || area == addrs::kAreaChalice;
    w.stable = !w.transition && t - C.world_since >= 3.0;
    return w;
}

bool idle(const World& w) { return w.state == addrs::kStateIdle && w.controller == 0; }

bool guest_says_here();

// Together: the guest is a phantom in the host's world; the host has a
// session she says she is in (its world always holds 7 player slots, so a
// count of others is true as soon as it opens a room for her).
bool together(const World& w) {
    if (!w.in_world) return false;
    if (S.guest) return w.chr_type == 1 || w.state == addrs::kStateJoined;
    return w.state == addrs::kStateHosting && guest_says_here();
}

// --- byte patches -----------------------------------------------------------------

// Writes a group of patches (or puts the game's bytes back). Each one is
// checked first; a group that does not match is left as the game has it.
template <size_t N>
bool apply_group(const char* group, const addrs::Patch (&p)[N], bool on) {
    for (size_t i = 0; i < N; ++i) {  // all or nothing: check every one first
        uint8_t have[16];
        if (!rd(bn(p[i].va), have, p[i].n) ||
            (memcmp(have, p[i].expect, p[i].n) != 0 && memcmp(have, p[i].bytes, p[i].n) != 0)) {
            say("%s: 0x%llx does not hold the 1.09 bytes; the group is left off", group,
                 static_cast<unsigned long long>(p[i].va));
            return false;
        }
    }
    for (size_t i = 0; i < N; ++i) {
        const uint8_t* from = on ? p[i].expect : p[i].bytes;
        const uint8_t* to = on ? p[i].bytes : p[i].expect;
        const int rc = api->patch(bn(p[i].va), from, to, p[i].n);
        say("%s: %s 0x%llx (%s) -> %s", group, on ? "patched" : "restored", static_cast<unsigned long long>(p[i].va),
             p[i].what, rc == 0 ? "ok" : rc == 1 ? "bytes differ" : "page not writable");
        if (rc != 0) return false;
    }
    return true;
}

// A patch group turned on or off once per change (now: whether it is on).
template <size_t N>
bool set_group(const char* group, const addrs::Patch (&p)[N], bool on, bool* now) {
    if (on == *now || !apply_group(group, p, on)) return false;
    *now = on;
    return true;
}

bool g_functions_ok;  // every function called or hooked has its 1.09 first bytes

// Every function of `list` starts with its 1.09 bytes; else `off` is said.
template <size_t N>
bool check_functions(const addrs::Function* const (&list)[N], const char* off) {
    for (const addrs::Function* f : list) {
        uint8_t have[20];
        if (!rd(bn(f->va), have, f->n) || memcmp(have, f->prologue, f->n) != 0) {
            say("0x%llx (%s) is not the 1.09 code; %s", static_cast<unsigned long long>(f->va), f->what, off);
            return false;
        }
    }
    return true;
}

bool g_pick_hook;  // the pick-up hook is in place
// Whether each switchable patch group is on: a boss kill keeps the guest; lamps
// while together (section 3b); the guest talks as at home (NPCs and lamps);
// the guest's game places the world's items while joined (her pickups are
// only shared while the hook sees them).
bool g_keep_after_boss_on, g_lamps_on, g_guest_talk_on, g_guest_pickups_on, g_helper_insight_on;

void set_groups() {
    set_group("keep_after_boss", addrs::kKeepAfterBoss, S.keep_after_boss, &g_keep_after_boss_on);
    set_group("helper_insight", addrs::kHelperInsight, S.guest && S.share_insight, &g_helper_insight_on);
    set_group("lamps", addrs::kLamps, S.lamps, &g_lamps_on);
    set_group("guest_talk", addrs::kGuestTalk, S.guest && S.guest_talk, &g_guest_talk_on);
    if (set_group("guest_pickups", addrs::kGuestPickups, S.guest && (S.guest_pickups || S.share_pickups) && g_pick_hook, &g_guest_pickups_on))
        say("pickups: from the next load your game %s the items of the host's world", g_guest_pickups_on ? "places" : "no longer places");
}

// --- 1. free bells -----------------------------------------------------------------

// A param row's price, made 0 while free_bells is on: the game's own value ("" until read).
struct Price {
    uint32_t row;
    char original[16];
};
Price g_bell_costs[] = {{addrs::kBeckoningBell, ""}, {addrs::kOldHunterBell, ""}};  // EquipParamGoods consumeHeroPoint
bool g_free_bells_on;

// The Small Resonant Bell is bought in the Hunter's Dream's Insight shop: the
// ShopLineupParam rows selling it (equipId 205, equipType 3 goods; 1.09 has
// five, 200000-240000, price "value" 1) are made free with the others.
Price g_shop_prices[8];
size_t g_shop_count;
bool g_shop_scanned;

bool scan_shop() {
    if (g_shop_scanned) return true;
    static uint32_t ids[4096];
    size_t count = 0;
    if (api->param_ids("ShopLineupParam", ids, sizeof(ids) / sizeof(ids[0]), &count) != 0) return false;
    if (count > sizeof(ids) / sizeof(ids[0])) count = sizeof(ids) / sizeof(ids[0]);
    for (size_t i = 0; i < count && g_shop_count < sizeof(g_shop_prices) / sizeof(g_shop_prices[0]); ++i) {
        char equip[16], type[16];
        if (api->param_get("ShopLineupParam", ids[i], "equipId", equip, sizeof(equip)) != 0 ||
            api->param_get("ShopLineupParam", ids[i], "equipType", type, sizeof(type)) != 0)
            continue;
        if (parse_long(equip, -1, 0, 0x7FFFFFFFL) != addrs::kSmallResonantBell || parse_long(type, -1, 0, 255) != 3) continue;
        Price& p = g_shop_prices[g_shop_count];
        if (api->param_get("ShopLineupParam", ids[i], "value", p.original, sizeof(p.original)) != 0) continue;
        p.row = ids[i];
        ++g_shop_count;
    }
    say("free bells: the Small Resonant Bell is sold in %zu shop row(s)", g_shop_count);
    g_shop_scanned = true;
    return true;
}

void free_bells_tick() {
    if (S.free_bells == g_free_bells_on) return;
    if (C.frame % 30 != 0) return;  // the params load during the boot
    for (Price& b : g_bell_costs) {
        size_t size = 0;
        if (!api->param_row("EquipParamGoods", b.row, &size)) return;
        if (!b.original[0] && api->param_get("EquipParamGoods", b.row, "consumeHeroPoint", b.original, sizeof(b.original)) != 0)
            return;
    }
    if (!scan_shop()) return;
    const auto set = [](const char* param, const Price& p, const char* field) {
        const char* value = S.free_bells ? "0" : p.original;
        const int rc = api->param_set(param, p.row, field, value);
        say("free bells: %s %u %s %s -> %s (%s)", param, p.row, field, S.free_bells ? p.original : "0", value,
            rc == 0 ? "ok" : "refused");
    };
    for (const Price& b : g_bell_costs) set("EquipParamGoods", b, "consumeHeroPoint");
    for (size_t i = 0; i < g_shop_count; ++i) set("ShopLineupParam", g_shop_prices[i], "value");
    g_free_bells_on = S.free_bells;
}

// --- the link to the partner ---------------------------------------------------------

struct Beat {
    char magic[4];       // kMagic
    uint8_t guest;       // the sender's role
    uint8_t in_world;
    uint8_t stable;
    uint8_t in_dream;
    uint8_t together;
    uint8_t bell;        // the sender's bell effect is on
    uint8_t wants_join;  // apart, at home, auto-ring on, not paused
    uint8_t credit_count;
    int32_t chr_type;
    uint32_t map;
    uint32_t block;
    float pos[3];
    float yaw;
    uint32_t pause_ms;   // how much longer the sender's pause lasts
    uint32_t credits[24];  // host: bosses killed together this run
    uint8_t downed;      // 6.: 1 the sender is down, 2 died for real (until the next load)
    uint8_t pickup_count;  // 4.: how many of pickups[] are set
    uint8_t taken_count;   // 4.: how many of taken[] are set
    uint8_t revive_how;  // 6.: what revive_ask is: 1 a pick-up (1 Blood Vial), 2 a lamp
    uint32_t run;        // the sender's run: a new number each start, so credits are told apart
    uint32_t pickups[16];  // host: item flags picked up together this run, the latest last
    uint32_t taken[16];    // guest: item flags she picked up in the host's world this run, the latest last
    uint16_t revive_ask;   // 6.: the sender asks the partner, who is down, to get up (a new number each time; 0: no)
    uint16_t revived_for;  // 6.: the last ask the sender got up for
    uint8_t rules;         // the sender's rules (kRuled), which the host's count for both
    uint8_t spare;
    uint16_t party_health; // the sender's enemy health together, %
    uint16_t insight;      // 4b.: host: the Insight its world gave it while together this run (not its items)
    uint16_t spare2;
    uint32_t gestures;     // 4c.: gestures the sender got while together this run (bit g: gesture g)
    uint16_t award_total;  // 4d.: item lots the sender's game gave it while together this run
    uint8_t award_count;   // 4d.: how many of awards[] are set
    uint8_t spare3;
    int32_t awards[12];    // 4d.: the last of them, the latest last
    uint32_t world_run;    // 4e.: guest: the host's run whose world changes she has, and how many
    uint32_t world_seen;
};
static_assert(sizeof(Beat) == 352, "the heartbeat's layout is the wire format");

// The last letter goes up with every change of what the two send each other ("BBCC": 0.10.2);
// another version's beats are ignored.
constexpr const char kMagic[4] = {'B', 'B', 'C', 'C'};

// 4.: the guest asks whether the items her game placed are taken in the
// host's world ("BBQ4", taken[] unset); the host answers ("BBA4").
struct Ask {
    char magic[4];
    uint32_t count;
    int32_t flags[48];
    uint8_t taken[48];
};
static_assert(sizeof(Ask) == 248 && sizeof(Ask) != sizeof(Beat), "the question's layout is the wire format");
constexpr const char kAskMagic[4] = {'B', 'B', 'Q', '4'}, kAnswerMagic[4] = {'B', 'B', 'A', '4'};

// 4e.: the host's world changes, from the first she lacks (kWorldMagic).
struct WorldMsg {
    char magic[4];
    uint32_t run;    // the host's
    uint32_t first;  // the first change's number in that run
    uint32_t count;
    uint32_t changes[200];  // the host's log (WorldOut: kOn, kClear, kCatchupEnd)
};
static_assert(sizeof(WorldMsg) == 816 && sizeof(WorldMsg) != sizeof(Ask) && sizeof(WorldMsg) != sizeof(Beat),
              "the world message's layout is the wire format");
constexpr const char kWorldMagic[4] = {'B', 'B', 'W', '2'};  // "BBW2": 0.10.2's catch-up end

struct Link {
    int fd = -1;
    sockaddr_in partner;
    bool partner_ok;
    double last_send;
    Beat peer;
    double peer_at;  // when the last heartbeat came; 0: never
    bool peer_here;  // her latest word from a world (not a load): she is in the host's
    double peer_world_at;  // when that word came
    bool warned_role, warned_version;
    uint32_t run;    // this start's number (Beat.run)
};
Link L;

// Her heartbeat (within 10 s) says she is in the host's world, or is loading (up to 30 s) after saying so.
bool guest_says_here() {
    const double t = now_s();
    return L.peer_at > 0 && t - L.peer_at < 10.0 && L.peer.guest && L.peer_here &&
           (L.peer.in_world || t - L.peer_world_at < 30.0);
}

// "192.168.1.130", or "192.168.1.130:9312" when the partner listens on
// another port.
void link_set_partner() {
    memset(&L.partner, 0, sizeof(L.partner));
    L.partner.sin_family = AF_INET;
    char ip[64];
    snprintf(ip, sizeof(ip), "%s", S.partner);
    long port = S.port;
    if (char* colon = strchr(ip, ':')) {
        *colon = 0;
        port = parse_long(colon + 1, -1, 1, 65535);
    }
    L.partner.sin_port = htons(static_cast<uint16_t>(port > 0 ? port : S.port));
    L.partner_ok = ip[0] && port > 0 && inet_pton(AF_INET, ip, &L.partner.sin_addr) == 1;
    if (S.partner[0] && !L.partner_ok) say("link: partner '%s' is not an IPv4 address (with an optional :port)", S.partner);
    if (!S.partner[0]) say("link: no partner set; bells ring without waiting for one and the guest cannot follow");
}

void link_open() {
    L.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (L.fd < 0) {
        say("link: no UDP socket (%s)", strerror(errno));
        return;
    }
    const int yes = 1;
    setsockopt(L.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    fcntl(L.fd, F_SETFL, fcntl(L.fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(static_cast<uint16_t>(S.port));
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(L.fd, reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
        say("link: cannot listen on UDP %d (%s)", S.port, strerror(errno));
        close(L.fd);
        L.fd = -1;
        return;
    }
    say("link: listening on UDP %d, partner %s", S.port, S.partner[0] ? S.partner : "(none)");
}

bool peer_fresh(double t) { return L.peer_at > 0 && t - L.peer_at < 3.0; }

// What both games must do alike is the host's choice: on the guest's machine its
// own settings give way to the host's while it hears from one (bbport.ini or the
// System menu set them again at their next change).
bool* const kRuled[] = {&S.downed, &S.pick_up, &S.lamp_revive, &S.lamps, &S.share_pickups};  // bit i of Beat.rules

uint8_t my_rules() {
    uint8_t r = 0;
    for (int i = 0; i < 5; ++i) r = static_cast<uint8_t>(r | (*kRuled[i] ? 1 << i : 0));
    return r;
}

void host_rules(const Beat& b) {
    const int health = b.party_health >= 100 && b.party_health <= 300 ? b.party_health : S.party_health;
    if (!S.guest || b.guest || (my_rules() == b.rules && S.party_health == health)) return;
    for (int i = 0; i < 5; ++i) *kRuled[i] = (b.rules >> i) & 1;
    S.party_health = health;
    say("rules: the host's: down not dead %d, revive by hand %d, lamps revive %d, lamps %d, pickups shared %d, enemy health %d%%",
        S.downed, S.pick_up, S.lamp_revive, S.lamps, S.share_pickups, health);
    set_groups();
}

void credit_from_peer(const Beat& b);
void lamps_from_guest(const Beat& b);
void pickups_from_peer(const Beat& b);
void awards_from_peer(const Beat& b);
void taken_from_peer(const Beat& b);
void ask_from_peer(const Ask& a);
void world_from_host(const WorldMsg& m);
void world_acked(const Beat& b);

void link_poll(double t) {
    if (L.fd < 0) return;
    for (int i = 0; i < 32; ++i) {
        union {
            Beat beat;
            Ask ask;
            WorldMsg world;
        } u;
        sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const ssize_t n = recvfrom(L.fd, &u, sizeof(u), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 0) break;
        if (L.partner_ok && from.sin_addr.s_addr != L.partner.sin_addr.s_addr) continue;
        if (n == static_cast<ssize_t>(sizeof(Ask)) &&
            (memcmp(u.ask.magic, kAskMagic, 4) == 0 || memcmp(u.ask.magic, kAnswerMagic, 4) == 0)) {
            ask_from_peer(u.ask);
            continue;
        }
        if (n == static_cast<ssize_t>(sizeof(WorldMsg)) && memcmp(u.world.magic, kWorldMagic, 4) == 0) {
            world_from_host(u.world);
            continue;
        }
        const Beat& b = u.beat;
        if (n >= 4 && memcmp(b.magic, "BBC", 3) == 0 && b.magic[3] != kMagic[3] && !L.warned_version) {
            L.warned_version = true;
            say("link: the partner runs another bbcoop version (%c); install the same on both machines", b.magic[3]);
        }
        if (n != static_cast<ssize_t>(sizeof(b)) || memcmp(b.magic, kMagic, 4) != 0) continue;
        const bool first = !peer_fresh(t);
        L.peer = b;
        L.peer_at = t;
        if (b.in_world || b.together) {
            L.peer_here = b.guest && b.together;
            L.peer_world_at = t;
        }
        if (first) say("link: partner %s is here (%s, map 0x%08x)", inet_ntoa(from.sin_addr), b.guest ? "guest" : "host", b.map);
        if (static_cast<bool>(b.guest) == S.guest && !L.warned_role) {
            L.warned_role = true;
            say("link: both machines are set as %s; set one host and one guest", S.guest ? "guest" : "host");
        }
        host_rules(b);
        credit_from_peer(b);
        lamps_from_guest(b);
        pickups_from_peer(b);
        taken_from_peer(b);
        awards_from_peer(b);
        world_acked(b);
    }
}

// --- 4. boss progress and drops ---------------------------------------------------------

struct FlagSet {
    uint32_t ids[2048];  // pickups add a flag each
    int count;
    bool has(uint32_t id) const {
        for (int i = 0; i < count; ++i)
            if (ids[i] == id) return true;
        return false;
    }
    bool add(uint32_t id) {
        if (has(id) || count >= 2048) return false;
        ids[count++] = id;
        return true;
    }
};
FlagSet g_pending;  // guest: flags for her own world (bosses killed together, what they unlock, drops had)
FlagSet g_credits;  // host: bosses killed together this run; both: lamps lit together (3b)
FlagSet g_had;      // guest: "unique drop had" flags (BossDrop.had_flag), from her world and from drops given
char g_dir[512];

// A file of the plugin's folder (until the next call).
const char* in_dir(const char* name) {
    static char path[640];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    return path;
}

// A file written anew: into <name>.tmp, then renamed over it (a crash of the game leaves the
// old or the new).
FILE* open_tmp(const char* name) {
    char tmp[660];
    snprintf(tmp, sizeof(tmp), "%s.tmp", in_dir(name));
    FILE* f = g_dir[0] ? fopen(tmp, "w") : nullptr;
    if (!f && g_dir[0]) say("cannot write %s", tmp);
    return f;
}

void close_tmp(FILE* f, const char* name) {
    char tmp[660];
    const char* path = in_dir(name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (fclose(f) == 0) rename(tmp, path);
}

// What one side does once for each (partner's run, id) the partner's heartbeat repeats; the
// oldest half goes when it is full.
template <int N>
struct RunSet {
    struct {
        uint32_t run, id;
    } v[N];
    int n;
    bool has(uint32_t run, uint32_t id) const {
        for (int i = 0; i < n; ++i)
            if (v[i].run == run && v[i].id == id) return true;
        return false;
    }
    void add(uint32_t run, uint32_t id) {
        if (n == N) {
            memmove(v, v + N / 2, N / 2 * sizeof(v[0]));
            n = N / 2;
        }
        v[n++] = {run, id};
    }
};

// "<run> <id>" lines.
template <int N>
void load_runs(const char* name, RunSet<N>* set) {
    FILE* f = fopen(in_dir(name), "r");
    if (!f) return;
    unsigned run = 0, id = 0;
    while (fscanf(f, "%u %u", &run, &id) == 2)
        if (id) set->add(run, id);
    fclose(f);
}

template <int N>
void save_runs(const char* name, const RunSet<N>& set) {
    FILE* f = open_tmp(name);
    if (!f) return;
    for (int i = 0; i < set.n; ++i) fprintf(f, "%u %u\n", set.v[i].run, set.v[i].id);
    close_tmp(f, name);
}

// Files written at the end of the frame, once however many changes came.
enum : unsigned { kSavePending = 1, kSaveHad = 2, kSaveDrops = 4, kSavePickups = 8 };
unsigned g_dirty;

// guest: the bosses whose drop she was given (drops.txt), so the host's credits, repeated in
// every heartbeat and after every re-join, give it once.
RunSet<256> g_given;

const char* boss_name(uint32_t id) {
    for (const addrs::BossFlag& b : addrs::kBossFlags)
        if (b.flag == id) return b.name;
    return "flag";
}

const addrs::BossDrop* boss_drop(uint32_t boss) {
    for (const addrs::BossDrop& d : addrs::kBossDrops)
        if (d.boss == boss) return &d;
    return nullptr;
}

bool is_boss(uint32_t id) { return boss_drop(id) != nullptr; }

bool carried(uint32_t id) {
    if (is_boss(id)) return true;
    for (int i = 0; i < S.extra_count; ++i)
        if (S.extra_flags[i] == id) return true;
    return false;
}

const addrs::Lamp* lamp_by(uint32_t flag, uint32_t (*which)(const addrs::Lamp&)) {
    for (const addrs::Lamp& l : addrs::kLampList)
        if (which(l) == flag) return &l;
    return nullptr;
}

void load_set(const char* name, FlagSet* set) {
    FILE* f = fopen(in_dir(name), "r");
    if (!f) return;
    unsigned id = 0;
    while (fscanf(f, "%u", &id) == 1)
        if (id) set->add(id);
    fclose(f);
}

void save_set(const char* name, const FlagSet& set) {
    FILE* f = open_tmp(name);
    if (!f) return;
    for (int i = 0; i < set.count; ++i) fprintf(f, "%u\n", set.ids[i]);
    close_tmp(f, name);
}

double g_joined_at;  // when the two came together; 0 while apart
World g_last;        // the last frame's view, for callbacks between frames

// A flag for her own world, once she is home. Each caller asks once per
// event (a kill seen, a (run, boss) credit, a drop or pickup given), so a
// flag her world lost again - a new game cycle - is set again next time.
bool remember_flag(uint32_t id) {
    if (!g_pending.add(id)) return false;
    g_dirty |= kSavePending;
    return true;
}

// The game's item-lot award, which works for a phantom too. False when it
// could not be called; then nothing is tried for 5 s (the caller tries again).
double g_award_retry_at;
bool g_own_award;  // the plugin's own award is running (the award hook leaves it alone)

bool award_lot(int32_t lot) {
    const double t = now_s();
    if (t < g_award_retry_at) return false;
    const uint64_t man = singleton(addrs::kItemLotMan);
    __atomic_store_n(&g_own_award, true, __ATOMIC_RELAXED);
    const bool ok = man && call(addrs::kAwardItemLot, man, static_cast<uint64_t>(static_cast<uint32_t>(lot)), 0);
    __atomic_store_n(&g_own_award, false, __ATOMIC_RELAXED);
    if (ok) return true;
    g_award_retry_at = t + 5.0;
    say("items: item lot %d could not be given (%s); trying again in 5 s", lot, man ? "the call failed" : "no ItemLotMan");
    return false;
}

// In a world, settled (observe: no load or transition pending): when an award is given
// (otherwise the next heartbeat tries again).
bool ready_for_items() { return g_last.stable; }

void remember_for_guest(uint32_t id, const char* how) {
    if (!remember_flag(id)) return;
    say("boss progress: %u (%s) killed together (%s); it goes into your world when you are home", id, boss_name(id), how);
    if (const addrs::BossDrop* d = boss_drop(id))
        for (uint32_t f : d->flags)
            if (f) remember_flag(f);
}

// guest: the boss's drop, as its defeat event would have given it in her own
// world. The game gives a phantom nothing (the award checks for a joined
// session); the Lua helper GetRateItem_IgnoreMultiPlay's call does not.
void share_drop(uint32_t boss, uint32_t run, const char* how) {
    if (!S.guest || !S.share_drops || !g_functions_ok || g_given.has(run, boss) || !ready_for_items()) return;
    const addrs::BossDrop* d = boss_drop(boss);
    if (!d) return;
    const bool had = d->had_flag && g_had.has(d->had_flag);
    const int32_t lot = had ? d->repeat_lot : d->lot;
    if (lot && !award_lot(lot)) return;
    g_given.add(run, boss);
    g_dirty |= kSaveDrops;
    if (!lot) {
        say("drops: %s (%s): its unique drop was had already and it has no other", boss_name(boss), how);
        return;
    }
    char flag_text[16] = "";
    long flag = -1;
    if (api->param_get("ItemLotParam", static_cast<uint32_t>(lot), "getItemFlagId", flag_text, sizeof(flag_text)) == 0)
        flag = parse_long(flag_text, -1, 1, 0x7FFFFFFFL);
    if (flag > 0) remember_flag(static_cast<uint32_t>(flag));  // taken in her own world too
    if (d->had_flag && !had && g_had.add(d->had_flag)) g_dirty |= kSaveHad;
    say("drops: %s (%s): item lot %d%s given (lot flag %ld)", boss_name(boss), how, lot, had ? " (the repeat drop)" : "", flag);
    note(9950014, "Co-op: you got the boss's drop too");
}

// Pickups: what a pickup flag (ItemLotParam getItemFlagId) gives, from the
// param table once it is loaded. The game awards a lot with the rows after it
// (up to the first missing id), so a lot is the first row of its run. Only
// runs whose rows all carry the one flag are copied (a few NPC sets mix
// flags: giving the set would mark the rest taken), and neither chalice
// dungeon lots (ids from 100000000, flags shared by hundreds) nor bosses'
// drops (share_drop gives those).
struct PickupLot {
    uint32_t flag;
    int32_t lot;
};
PickupLot g_pick_lots[4096];
int g_pick_lot_count;
bool g_pick_built;

int cmp_u32(const void* a, const void* b) {
    const uint32_t x = *static_cast<const uint32_t*>(a), y = *static_cast<const uint32_t*>(b);
    return x < y ? -1 : x > y;
}

int cmp_pickup(const void* a, const void* b) {
    const PickupLot& x = *static_cast<const PickupLot*>(a);
    const PickupLot& y = *static_cast<const PickupLot*>(b);
    if (x.flag != y.flag) return x.flag < y.flag ? -1 : 1;
    return x.lot < y.lot ? -1 : x.lot > y.lot;
}

int32_t lot_flag(uint32_t lot) {
    size_t bytes = 0;
    const void* row = api->param_row("ItemLotParam", lot, &bytes);
    int32_t flag = -1;
    if (row && bytes >= 0x84) memcpy(&flag, static_cast<const uint8_t*>(row) + 0x80, 4);
    return flag;
}

void build_pickup_table() {
    if (g_pick_built) return;
    static uint32_t ids[16384];
    size_t count = 0;
    if (api->param_ids("ItemLotParam", ids, 16384, &count) != 0 || count == 0) return;  // not loaded yet
    g_pick_built = true;
    if (count > 16384) count = 16384;
    qsort(ids, count, sizeof(ids[0]), cmp_u32);
    uint32_t boss_flags[64];
    int boss_flag_count = 0;
    for (const addrs::BossDrop& d : addrs::kBossDrops) {
        const int32_t lots[2] = {d.lot, d.repeat_lot};
        for (int32_t lot : lots) {
            const int32_t f = lot > 0 ? lot_flag(static_cast<uint32_t>(lot)) : -1;
            if (f > 0 && boss_flag_count < 64) boss_flags[boss_flag_count++] = static_cast<uint32_t>(f);
        }
    }
    int mixed = 0;
    for (size_t i = 0; i < count && g_pick_lot_count < 4096; ++i) {
        const uint32_t first = ids[i];
        if ((i > 0 && ids[i - 1] == first - 1) || first >= 100000000u) continue;
        const int32_t f = lot_flag(first);
        if (f <= 0) continue;  // (a boss's lot: its flag is one of boss_flags)
        bool pure = true, boss = false;
        for (size_t k = i + 1; k < count && ids[k] == ids[k - 1] + 1 && pure; ++k) pure = lot_flag(ids[k]) == f;
        for (int k = 0; k < boss_flag_count; ++k) boss |= boss_flags[k] == static_cast<uint32_t>(f);
        if (!pure) ++mixed;
        if (!pure || boss) continue;
        g_pick_lots[g_pick_lot_count++] = {static_cast<uint32_t>(f), static_cast<int32_t>(first)};
    }
    qsort(g_pick_lots, static_cast<size_t>(g_pick_lot_count), sizeof(g_pick_lots[0]), cmp_pickup);
    int n = 0;  // the lowest lot of each flag
    for (int i = 0; i < g_pick_lot_count; ++i)
        if (!n || g_pick_lots[n - 1].flag != g_pick_lots[i].flag) g_pick_lots[n++] = g_pick_lots[i];
    g_pick_lot_count = n;
    say("pickups: %d pickup flags from %zu item lots (%d NPC sets with mixed flags left out)", n, count, mixed);
}

int32_t pickup_lot(uint32_t flag) {
    build_pickup_table();
    const auto* p = static_cast<const PickupLot*>(
        bsearch(&flag, g_pick_lots, static_cast<size_t>(g_pick_lot_count), sizeof(PickupLot), cmp_u32));  // flag first
    return p ? p->lot : 0;
}

// Map items. With guest_pickups alone each picks up their own copy: hers is
// marked taken in her own world, nothing goes to the partner. With
// share_pickups both games' pick-up (kPickUp) is hooked: a map
// treasure picked up while the two are together goes to the partner as its
// item flag (a flag stands for its whole item lot).
//   - The host's: the guest picks up her copy of it (kGuestPickups has her
//     game place the world's items too), or - when it is not lying near her -
//     is given its item lot, and her copy goes when it comes into view.
//   - The guest's (her game gives her the items and sets no flag): the host
//     picks up his copy of it, or is given its item lot and its flag when it
//     is not lying near him (nothing when his world had it taken already).
// Her own world marks each one taken once she is home. Not shared: what no
// plain item lot gives (random enemy drops, a few NPC sets, chalice dungeons).

struct MapItem {
    uint64_t at;  // 0: none
    uint32_t id;
    int32_t flag, flag2;
    uint64_t next;  // the next item in its list (0: the end)
};

// A map treasure that can be picked up, or none (next set either way), in one
// read: the lists run to hundreds of items, and each read is a system call.
MapItem read_item(uint64_t at) {
    MapItem it = {0, 0, -1, -1, 0};
    unsigned char b[addrs::kItemFlag2 + 4];
    uint16_t bits = 0;
    if (!rd(at, b, sizeof(b))) return it;
    memcpy(&it.next, b, 8);
    memcpy(&it.id, b + addrs::kItemId, 4);
    memcpy(&it.flag, b + addrs::kItemFlag, 4);
    memcpy(&it.flag2, b + addrs::kItemFlag2, 4);
    memcpy(&bits, b + addrs::kItemBits, 2);
    if ((it.id & 0xF) == addrs::kKindTreasure && (bits & addrs::kItemActive) && !(bits & addrs::kItemShared)) it.at = at;
    return it;
}

// The first map treasure lying in this world that fn takes, or none.
template <typename F>
MapItem each_treasure(uint64_t man, F fn) {
    uint64_t at = man ? rp(man + addrs::kTreasureList) : 0;
    for (int n = 0; at && n < 8192; ++n) {
        const MapItem it = read_item(at);
        at = it.next;
        if (it.at && fn(it)) return it;
    }
    return MapItem{0, 0, -1, -1, 0};
}

// The map treasure lying in this world with this id - or, for id 0, the
// first with this item flag.
MapItem find_treasure(uint64_t man, uint32_t id, int32_t flag) {
    return each_treasure(man, [&](const MapItem& it) { return id ? it.id == id : it.flag == flag; });
}

// What the hook saw picked up, for the next frame (one writer: the game's
// thread; one reader: the frame).
struct Pick {
    int32_t flag, flag2;
};
Pick g_hooked[32];
uint32_t g_hooked_head, g_hooked_tail;
int32_t g_own_call;  // the item flag the plugin itself picks up, removes or sets right now (0: none)

int on_pick_up(BbHookCtx* ctx, void*) {
    if (!ctx->arg[1]) return 0;
    // The caller's own variable, which the game reads next (the checked read
    // may not cover the game's stack).
    const uint32_t id = *reinterpret_cast<const volatile uint32_t*>(ctx->arg[1]);
    const MapItem it = find_treasure(ctx->arg[0], id, 0);
    if (!it.at || it.flag <= 0 || it.flag == __atomic_load_n(&g_own_call, __ATOMIC_RELAXED)) return 0;
    const uint32_t head = g_hooked_head;
    if (head - __atomic_load_n(&g_hooked_tail, __ATOMIC_ACQUIRE) < 32) {
        g_hooked[head % 32] = {it.flag, it.flag2};
        __atomic_store_n(&g_hooked_head, head + 1, __ATOMIC_RELEASE);
    }
    return 0;  // the game picks it up as always
}

// The game's pick-up or removal of an item lying here, done by the plugin
// (the hook leaves it alone).
bool item_call(const addrs::Function& f, uint64_t man, const MapItem& it) {
    static uint32_t id;  // the game reads it
    id = it.id;
    __atomic_store_n(&g_own_call, it.flag, __ATOMIC_RELAXED);
    const bool ok = call(f, man, addr(&id), 0);
    __atomic_store_n(&g_own_call, 0, __ATOMIC_RELAXED);
    return ok;
}

// Picks up every item lying here with this flag, as the player would: how
// many (-1: the game could not be called). flag2: the other flag they set.
int pick_up_all(int32_t flag, int32_t* flag2) {
    const uint64_t man = singleton(addrs::kItemLotMan);
    int n = 0;
    for (; n < 8; ++n) {
        const MapItem it = find_treasure(man, 0, flag);
        if (!it.at) break;
        if (!item_call(addrs::kPickUp, man, it)) return n ? n : -1;
        if (flag2 && it.flag2 > 0) *flag2 = it.flag2;
        if (find_treasure(man, it.id, 0).at) return n + 1;  // still there: no more tries
    }
    return n;
}

// Takes every item lying here with this flag away without giving anything:
// how many.
int remove_all(uint64_t man, int32_t flag) {
    int n = 0;
    for (; n < 8; ++n) {
        const MapItem it = find_treasure(man, 0, flag);
        if (!it.at || !item_call(addrs::kRemoveItem, man, it) || find_treasure(man, it.id, 0).at) break;
    }
    return n;
}

void set_flag_quietly(uint32_t id) {
    __atomic_store_n(&g_own_call, static_cast<int32_t>(id), __ATOMIC_RELAXED);
    api->event_flag_set(id, 1);
    __atomic_store_n(&g_own_call, 0, __ATOMIC_RELAXED);
}

// guest: in the host's world (a joined session starts in her own world,
// before the load takes her there).
bool in_host_world(const World& w) { return S.guest && w.in_world && w.chr_type == 1; }

// Shared: a flag one plain item lot gives, outside the Dream and the chalice dungeons.
bool shared_pickup(uint32_t flag) { return !g_last.in_dream && pickup_lot(flag) > 0; }

// host: what he picked up while together, the latest last, in every
// heartbeat (the last 16).
constexpr int kPickedRing = 64;
uint32_t g_picked[kPickedRing];
int g_picked_total;

// seen: the hook saw it picked up; otherwise a flag turned on, which as the
// two meet or as the world loads is no pickup.
void note_pickup(uint32_t id, bool seen) {
    const double t = now_s();
    if (!S.share_pickups || !together(g_last)) return;
    if (!seen && (g_joined_at <= 0 || t - g_joined_at < 10.0 || !g_last.stable)) return;
    if (static_cast<int32_t>(id) == __atomic_load_n(&g_own_call, __ATOMIC_RELAXED) || !shared_pickup(id)) return;
    for (int i = 0; i < kPickedRing && i < g_picked_total; ++i)
        if (g_picked[(g_picked_total - 1 - i) % kPickedRing] == id) return;
    g_picked[g_picked_total++ % kPickedRing] = id;
    say("pickups: you picked up flag %u (item lot %d) together; the guest gets it too", id, pickup_lot(id));
}

// guest: what she picked up in the host's world, the latest last, in every
// heartbeat (the last 16); the host picks it up too.
uint32_t g_taken[kPickedRing];
int g_taken_total;
FlagSet g_self_picked;  // guest: picked up by her this run (the host's word of it gives nothing more)
FlagSet g_lot_given;    // guest: host pickups given as their item lot: her copy goes when it comes into view

// Whatever she picks up in his world is taken in her own world too.
void guest_picked(const Pick& p) {
    if (!g_guest_pickups_on || !in_host_world(g_last)) return;
    const uint32_t f = static_cast<uint32_t>(p.flag);
    remember_flag(f);
    if (p.flag2 > 0) remember_flag(static_cast<uint32_t>(p.flag2));
    const bool shared = S.share_pickups && shared_pickup(f);
    if (shared && g_self_picked.add(f)) g_taken[g_taken_total++ % kPickedRing] = f;  // once for more of the same lot
    say("pickups: you picked up flag %u in the host's world; %s; it is marked taken in your world", f,
        shared ? "the host gets it too" : S.share_pickups ? "not shared (no plain item lot has it)" : "yours only");
}

// host: the guest's pickups done here, as (her run, flag).
RunSet<256> g_taken_done;

// host: his copy of what she picked up. False: try again with the next heartbeat.
bool host_copy(uint32_t f) {
    const int picked = pick_up_all(static_cast<int32_t>(f), nullptr);
    if (picked < 0) return false;
    if (picked > 0) {
        say("pickups: the guest picked up flag %u; picked up here too (%d item%s): your copy", f, picked, picked == 1 ? "" : "s");
        return true;
    }
    int had = 0;
    if (api->event_flag_get(f, &had) == 0 && had == 1) {
        say("pickups: the guest picked up flag %u; your world had it taken already", f);
        return true;
    }
    const int32_t lot = pickup_lot(f);
    if (lot <= 0) {
        say("pickups: the guest picked up flag %u, which no item lot here gives", f);
        return true;
    }
    if (!award_lot(lot)) return false;
    set_flag_quietly(f);
    say("pickups: the guest picked up flag %u, not lying near you: item lot %d -> your copy; taken in your world", f, lot);
    return true;
}

void taken_from_peer(const Beat& b) {
    if (S.guest || !b.guest || !S.share_pickups || !g_functions_ok) return;
    const int n = b.taken_count < 16 ? b.taken_count : 16;
    for (int i = 0; i < n; ++i) {
        const uint32_t f = b.taken[i];
        if (g_taken_done.has(b.run, f)) continue;
        if (!together(g_last) || !ready_for_items() || !host_copy(f)) return;  // the next heartbeat tries again
        g_taken_done.add(b.run, f);
    }
}

// guest: the pickups she was given (pickups.txt), so the host's list, repeated in every
// heartbeat, gives each once - also across a restart of her game.
RunSet<1024> g_pick_given;

// guest: her copy of what the host picked up.
void pickups_from_peer(const Beat& b) {
    if (!S.guest || b.guest || !S.share_pickups || !g_functions_ok || !ready_for_items()) return;
    const int n = b.pickup_count < 16 ? b.pickup_count : 16;
    for (int i = 0; i < n; ++i) {
        const uint32_t id = b.pickups[i];
        if (g_pick_given.has(b.run, id)) continue;
        const int32_t lot = pickup_lot(id);
        int32_t flag2 = -1;
        const char* how = "no pickup here";
        if (g_self_picked.has(id)) {
            how = "you had picked it up yourself";
        } else {
            // Her copy of the world's items only while she is in his world.
            const int picked = in_host_world(g_last) ? pick_up_all(static_cast<int32_t>(id), &flag2) : 0;
            if (picked < 0) return;  // the next heartbeat tries again
            if (picked > 0) {
                how = "picked up from your copy of it";
            } else if (lot > 0) {
                if (!award_lot(lot)) return;
                if (in_host_world(g_last)) g_lot_given.add(id);
                how = "its item lot given";
            }
        }
        g_pick_given.add(b.run, id);
        g_dirty |= kSavePickups;
        say("pickups: the host picked up flag %u (item lot %d): %s", id, lot, how);
        if (lot <= 0) continue;
        remember_flag(id);  // taken in her own world too
        if (flag2 > 0) remember_flag(static_cast<uint32_t>(flag2));
    }
}

// The treasure lying here whose flag `want` takes, removed: one walk of the list for all
// of them (up to 16), then the removals (which change it).
template <typename F>
void remove_treasure(F want, const char* why) {
    const uint64_t man = singleton(addrs::kItemLotMan);
    MapItem found[16];
    int n = 0;
    each_treasure(man, [&](const MapItem& it) {
        if (want(it.flag)) found[n++] = it;
        return n == 16;
    });
    for (int i = 0; i < n; ++i)
        if (item_call(addrs::kRemoveItem, man, found[i])) say("pickups: flag %d %s", found[i].flag, why);
}

// The frame: what the hook saw; and the guest's copies of items she was
// given as a lot go as they come into view (once a second).
double g_tidy_at;

void pickups_tick(const World& w, double t) {
    uint32_t tail = g_hooked_tail;
    const uint32_t head = __atomic_load_n(&g_hooked_head, __ATOMIC_ACQUIRE);
    for (; tail != head; ++tail) {
        const Pick p = g_hooked[tail % 32];
        if (S.guest) guest_picked(p);
        else note_pickup(static_cast<uint32_t>(p.flag), true);
    }
    __atomic_store_n(&g_hooked_tail, tail, __ATOMIC_RELEASE);
    if (!g_lot_given.count || !in_host_world(w) || !w.stable || t - g_tidy_at < 1.0) return;
    g_tidy_at = t;
    remove_treasure([](int32_t flag) { return flag > 0 && g_lot_given.has(static_cast<uint32_t>(flag)); },
                    "came into view; removed (the host's pickup gave you its lot)");
}

// guest: what she took in her own world stays taken in the host's. Joining,
// her game swaps its flags for the host's (addrs::kEventFlagMan), which hold
// no item flags, so there every item lies for her again. Her world's item
// flags (5xxxxxxx) are copied at each load at home and again after an item
// flag comes on there; in his world, after every load, the items she took
// (those, and what she picked up with him and her world has not had yet) go
// from her view. At home too: what she picked up with him is marked taken only
// after the load has placed it.
struct HomeBlock {
    uint32_t block;
    unsigned char bits[128];
};
HomeBlock g_home[512];
int g_home_count;
uint32_t g_home_size;    // flags to a block
double g_home_read_for;  // the world (C.world_since) they were copied in; 0: copy again
double g_hide_at;

bool item_flag(uint32_t id) { return id >= 50000000 && id < 60000000; }

void read_home_items() {
    g_home_count = 0;
    const uint64_t man = singleton(addrs::kEventFlagMan);
    uint32_t size = 0, stride = 0;
    if (!man || !rd(man + 0x1C, &size, 4) || !rd(man + 0x20, &stride, 4) || size == 0 || size > 1024) return;
    g_home_size = size;
    const uint64_t pool = rp(man + 0x28), head = rp(man + 0x38);
    uint64_t todo[128];
    int depth = 0;
    if (head) todo[depth++] = rp(head + 8);  // the root
    for (int seen = 0; depth > 0 && seen < 16384; ++seen) {
        unsigned char n[0x38];
        if (!rd(todo[--depth], n, sizeof(n)) || n[0x19]) continue;
        uint32_t block = 0;
        int32_t kind = 0;
        uint64_t data = 0;
        memcpy(&block, n + 0x20, 4);
        memcpy(&kind, n + 0x28, 4);
        memcpy(&data, n + 0x30, 8);
        if (depth < 127) memcpy(&todo[depth++], n, 8);
        if (depth < 127) memcpy(&todo[depth++], n + 0x10, 8);
        if (!item_flag(block * size) || g_home_count == 512) continue;
        if (kind == 1) data = pool + static_cast<uint32_t>(static_cast<int32_t>(stride) * static_cast<int32_t>(data));
        else if (kind != 2) continue;
        g_home[g_home_count].block = block;
        if (rd(data, g_home[g_home_count].bits, (size + 7) / 8)) ++g_home_count;
    }
}

bool taken_by_her(int32_t flag) {
    if (flag <= 0) return false;
    const uint32_t f = static_cast<uint32_t>(flag);
    if (g_pending.has(f)) return true;
    if (!g_home_size) return false;
    const uint32_t block = f / g_home_size, bit = f % g_home_size;
    for (int i = 0; i < g_home_count; ++i)
        if (g_home[i].block == block) return (g_home[i].bits[bit >> 3] >> (7 - (bit & 7))) & 1;
    return false;
}

void hide_taken_tick(const World& w, double t) {
    if (!S.guest || !w.stable) return;
    const bool home = w.chr_type == 0 && idle(w);
    if (home) {
        if (g_home_read_for == C.world_since) return;
        g_home_read_for = C.world_since;
        read_home_items();
    } else if (!g_guest_pickups_on || !in_host_world(w) || t - g_hide_at < 2.0) {
        return;
    }
    g_hide_at = t;
    remove_treasure(taken_by_her, "is taken in your world; its copy here removed");
}

// guest: once in the host's world after joining, at his side (the game puts
// her where her sign was: where she rang in her own world).
double g_moved_for;

void arrive_tick(const World& w, double t) {
    if (!S.guest || !S.join_at_host || !in_host_world(w) || g_joined_at <= 0 || g_moved_for == g_joined_at ||
        w.transition || t - C.world_since < 0.5 || !peer_fresh(t) || L.peer.guest || !L.peer.in_world ||
        L.peer.map != w.map)
        return;
    g_moved_for = g_joined_at;
    const float dx = L.peer.pos[0] - w.pos[0], dy = L.peer.pos[1] - w.pos[1], dz = L.peer.pos[2] - w.pos[2];
    const float d = root(dx * dx + dy * dy + dz * dz);
    if (d < 5.0f) return;
    const int rc = api->player_warp(L.peer.block == w.block ? 0 : L.peer.block, L.peer.pos, L.peer.yaw);
    say("join: moving to the host's side (%.0f m away, block 0x%08x) %s", d, L.peer.block, rc == 0 ? "(queued)" : "refused");
}

// Her game places every item (above); with share_pickups what the host's world
// had taken already goes from her view too. She asks about each item's flag once
// each time together (48 to a question, every 2 s, until answered); the host
// answers from his world's flags.
FlagSet g_asked;   // guest: flags answered this time together
double g_asked_for, g_ask_at;

void ask_tick(const World& w, double t) {
    if (!S.share_pickups || !g_guest_pickups_on || !in_host_world(w) || !w.stable || w.in_dream || !peer_fresh(t) ||
        L.fd < 0 || !L.partner_ok || t - g_ask_at < 2.0)
        return;
    g_ask_at = t;
    if (g_asked_for != g_joined_at) {
        g_asked.count = 0;
        g_asked_for = g_joined_at;
    }
    Ask a = {};
    memcpy(a.magic, kAskMagic, 4);
    each_treasure(singleton(addrs::kItemLotMan), [&](const MapItem& it) {
        if (it.flag <= 0 || g_asked.has(static_cast<uint32_t>(it.flag))) return false;
        bool listed = false;
        for (uint32_t k = 0; k < a.count && !listed; ++k) listed = a.flags[k] == it.flag;
        if (!listed) a.flags[a.count++] = it.flag;
        return a.count == 48;
    });
    if (a.count) sendto(L.fd, &a, sizeof(a), 0, reinterpret_cast<const sockaddr*>(&L.partner), sizeof(L.partner));
}

void ask_from_peer(const Ask& a) {
    const uint32_t n = a.count < 48 ? a.count : 48;
    if (memcmp(a.magic, kAskMagic, 4) == 0) {  // host: answer from his world's flags
        if (S.guest || !S.share_pickups || L.fd < 0 || !L.partner_ok) return;
        Ask r = a;
        memcpy(r.magic, kAnswerMagic, 4);
        r.count = n;
        for (uint32_t i = 0; i < n; ++i) {
            int v = 0;
            r.taken[i] = a.flags[i] > 0 && api->event_flag_get(static_cast<uint32_t>(a.flags[i]), &v) == 0 && v == 1;
        }
        sendto(L.fd, &r, sizeof(r), 0, reinterpret_cast<const sockaddr*>(&L.partner), sizeof(L.partner));
        return;
    }
    if (!g_functions_ok || !in_host_world(g_last) || g_asked_for != g_joined_at) return;
    const uint64_t man = singleton(addrs::kItemLotMan);
    int flags = 0, items = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t f = a.flags[i];
        if (f <= 0 || !g_asked.add(static_cast<uint32_t>(f)) || !a.taken[i] || g_self_picked.has(static_cast<uint32_t>(f))) continue;
        const int k = remove_all(man, f);
        flags += k > 0;
        items += k;
    }
    if (items) say("pickups: %d item(s) the host had taken already (%d flag(s)) removed from your view", items, flags);
}

void downed_boss_died(uint32_t id, const char* how);
void lamp_flag_on(uint32_t id);
void lamp_from_host(uint32_t lit);
void carried_flag_on(uint32_t id, const char* how);

// A boss's defeat flag comes on as its defeat event ends, past the setters on_flag
// hears (Cleric Beast's 12411700 went unheard). The game's boss clear (kBlockClear,
// hooked) starts just before: then this area's bosses' flags (a boss flag is
// 1AAB....: area AA) are read every 15 frames for 10 s, against what they were as the
// world settled (and 10 s on: her joining brings the host's). Nothing is read otherwise.
constexpr size_t kBossCount = sizeof(addrs::kBossFlags) / sizeof(addrs::kBossFlags[0]);
struct BossWatch {
    double world = -1;
    bool together;
    double again_at;  // the second look after a new world (0: done)
    double until;     // a boss clear: read until then
    bool on[kBossCount];
} BW;
bool g_boss_cleared;  // the hook's (the game's thread)

int on_block_clear(BbHookCtx*, void*) {
    __atomic_store_n(&g_boss_cleared, true, __ATOMIC_RELAXED);
    return 0;
}

void boss_watched(uint32_t id) {
    for (size_t i = 0; i < kBossCount; ++i)
        if (addrs::kBossFlags[i].flag == id) BW.on[i] = true;
}

void boss_watch_tick(const World& w, double t) {
    if (__atomic_exchange_n(&g_boss_cleared, false, __ATOMIC_RELAXED)) {
        BW.until = t + 10.0;
        say("boss: the game's boss clear began (%s)", together(w) ? "together" : "apart");
    }
    if (!w.stable) return;
    const bool tog = together(w);
    const bool fresh = BW.world != C.world_since || BW.together != tog;
    if (fresh) {
        BW.world = C.world_since;
        BW.together = tog;
        BW.again_at = t + 10.0;
    }
    const bool watching = t < BW.until;  // a kill: changes count
    const bool look = fresh || (BW.again_at && t >= BW.again_at);  // only taken in
    if (!look && (!watching || C.frame % 15)) return;
    if (!fresh && look) BW.again_at = 0;
    const uint32_t area = w.map >> 24;
    for (size_t i = 0; i < kBossCount; ++i) {
        const uint32_t flag = addrs::kBossFlags[i].flag;
        if (flag / 100000 % 100 != area) continue;
        int v = 0;
        const bool on = api->event_flag_get(flag, &v) == 0 && v == 1;
        if (on && !BW.on[i] && watching && !fresh) carried_flag_on(flag, "after the boss clear");
        BW.on[i] = on;
    }
}

void on_flag(uint32_t id, int value, void*) {
    if (value == 1 && id == addrs::kBellRefund && S.free_bells) {
        api->event_flag_set(id, 0);
        say("free bells: the bell cost no Insight, so the game gives none back if nobody comes (flag %u cleared)", id);
    }
    if (value == 1 && !S.guest) note_pickup(id, false);
    if (value == 1 && S.guest && item_flag(id) && g_last.chr_type == 0) g_home_read_for = 0;  // copied again
    if (value == 1) lamp_flag_on(id);
    if (value == 1 && carried(id)) carried_flag_on(id, "heard");
}

// A boss's defeat flag (or an extra carried one) came on.
void carried_flag_on(uint32_t id, const char* how) {
    boss_watched(id);
    const double t = now_s();
    // Flags the host's world brings along as she arrives are not kills.
    const bool kill = together(g_last) && (!S.guest || (g_joined_at > 0 && t - g_joined_at > 10.0));
    if (!kill) return;
    say("boss: %u (%s) down while together (%s); %s", id, boss_name(id), how,
        S.keep_after_boss ? "the guest stays (keep_after_boss)" : "the game sends the guest home shortly; re-join brings her back");
    if (is_boss(id)) downed_boss_died(id, "seen here");
    if (S.guest) {
        if (S.boss_progress) remember_for_guest(id, "seen here");
        if (is_boss(id) && peer_fresh(t) && !L.peer.guest) share_drop(id, L.peer.run, "seen here");
    } else if (g_credits.add(id)) {
        say("boss progress: %u (%s) killed together; telling the guest", id, boss_name(id));
    }
}

// guest: the host's credits come in every heartbeat; each (run, boss) counts once.
RunSet<256> g_seen;

void credit_from_peer(const Beat& b) {
    if (!S.guest || b.guest) return;
    const int n = b.credit_count < 24 ? b.credit_count : 24;
    for (int i = 0; i < n; ++i) {
        const uint32_t id = b.credits[i];
        const bool lamp = lamp_by(id, addrs::lamp_lit_flag) != nullptr;
        if (!lamp && !carried(id)) continue;
        if (is_boss(id)) share_drop(id, b.run, "the host's word");  // once per (run, boss), when she can take it
        if (g_seen.has(b.run, id)) continue;
        g_seen.add(b.run, id);
        if (lamp) {
            lamp_from_host(id);
            continue;
        }
        if (S.boss_progress) remember_for_guest(id, "the host's word");
        if (is_boss(id)) downed_boss_died(id, "the host's word");
    }
}

// guest, at home: which unique drops her own world says she has.
void read_had_flags() {
    bool changed = false;
    for (const addrs::BossDrop& d : addrs::kBossDrops) {
        int v = 0;
        if (d.had_flag && api->event_flag_get(d.had_flag, &v) == 0 && v == 1) changed |= g_had.add(d.had_flag);
    }
    if (changed) g_dirty |= kSaveHad;
}

double g_had_read_for;  // the world (C.world_since) read_had_flags last ran in
double g_pending_tried_for;  // the world the pending flags were last tried in
int g_pending_tried_count;   // and how many were left

void boss_progress_tick(const World& w) {
    if (!S.guest) return;
    // At home: her own world, no session, settled after the load.
    if (w.chr_type != 0 || !idle(w) || !w.stable) return;
    if (g_had_read_for != C.world_since) {
        g_had_read_for = C.world_since;
        read_had_flags();
    }
    if (!g_pending.count) return;
    // Flags that fail stay for the next world; tried once per world unless more come.
    if (g_pending_tried_for == C.world_since && g_pending.count == g_pending_tried_count) return;
    int done = 0, kept = 0, failed = 0, lit = 0;
    for (int i = 0; i < g_pending.count; ++i) {
        const uint32_t id = g_pending.ids[i];
        const addrs::Lamp* lamp = lamp_by(id, addrs::lamp_lit_flag);
        // Kept while its switch is off; item flags (what she took or was given) always go.
        if (!(lamp ? S.lamps : item_flag(id) || S.boss_progress)) {
            g_pending.ids[kept++] = id;
            continue;
        }
        int was = -1;
        if (api->event_flag_get(id, &was) != 0 || (was == 0 && api->event_flag_set(id, 1) != 0)) {
            g_pending.ids[kept++] = id;
            ++failed;
            continue;
        }
        if (lamp) say("lamps: lamp %u-%u %s in your world", lamp->map, lamp->n, was ? "was lit already" : "is now lit");
        else if (is_boss(id)) say("boss progress: %u (%s) %s in your world", id, boss_name(id), was ? "was already dead" : "is now dead");
        else say("boss progress: flag %u %s in your world", id, was ? "was already on" : "is now on");
        if (!was && is_boss(id)) ++done;
        if (!was && lamp) ++lit;
    }
    if (failed) say("boss progress: %d flag(s) could not be read or set here; trying again after the next load", failed);
    const bool changed = kept != g_pending.count;
    g_pending.count = kept;
    g_pending_tried_for = C.world_since;
    g_pending_tried_count = kept;
    if (changed) g_dirty |= kSavePending;
    if (done) note(9950034, "Co-op: bosses defeated in co-op now count in your world");
    if (lit) note(9950035, "Co-op: lamps lit in co-op are now lit in your world");
}

// --- 2./3. travelling and ringing ------------------------------------------------------

struct Join {
    double pause_until;   // Leave now: no ringing or travelling until then
    double last_ring;
    int ring_attempts;    // since the two were last together
    double last_warp;
    double warps[5];      // the last five travels, for the limit
    bool joined_once;
    bool rang_note;       // the "ringing" banner shown this time apart
    bool was_together;
    double last_together_at;
    double backoff_until;
};
Join J;

bool paused(double t) {
    if (t < J.pause_until) return true;
    return peer_fresh(t) && L.peer.pause_ms > 0;
}

alignas(16) float g_warp_position[4] = {0, 0, 0, 1.0f};
alignas(16) float g_warp_orientation[4];  // [1]: the yaw
alignas(16) uint32_t g_warp_map[4];

// The game's forced-summon warp to the host's map and spot (Wozzardman's
// pre-match transition): the map loads with her own world's progress.
bool travel_to_host(const World& w, const Beat& host, double t) {
    if (w.transition) return false;
    int recent = 0;
    for (double at : J.warps)
        if (at > 0 && t - at < 300.0) ++recent;
    if (recent >= 5) {
        if (t - J.last_warp > 60.0) {
            J.last_warp = t;
            say("travel: five trips in five minutes; waiting (the guest does not arrive where the host is?)");
        }
        return false;
    }
    if (t - J.last_warp < 25.0) return false;
    const uint64_t gs = singleton(addrs::kGameStateMan);
    if (!gs) return false;
    J.last_warp = t;
    for (int i = 4; i > 0; --i) J.warps[i] = J.warps[i - 1];
    J.warps[0] = t;
    g_warp_map[0] = host.map;
    memcpy(g_warp_position, host.pos, sizeof(host.pos));
    g_warp_orientation[1] = host.yaw;
    const bool set = call(addrs::kSetForcedPosition, addr(g_warp_position)) &&
                     call(addrs::kSetForcedOrientation, addr(g_warp_orientation)) && call(addrs::kSetForcedMap, addr(g_warp_map)) &&
                     call(addrs::kSetForcedWarp);
    uint64_t selected = 0, transition = 0;
    uint32_t chosen = 0;
    const bool picked = set && call(addrs::kSelectPlacement, 0, 0xFFFFFFFFull, 0, &selected) && (selected & 0xFF) &&
                        rd(gs + 0x0C, &chosen, 4) && chosen == host.map;
    if (!picked) {
        // Leave no forced warp behind for the next death or lamp to use.
        *reinterpret_cast<volatile uint8_t*>(gs + 0x1520) = 0;
        say("travel: the game did not take map 0x%08x (picked 0x%08x, select %d); not travelling", host.map, chosen,
             static_cast<int>(selected & 0xFF));
        return false;
    }
    if (!call(addrs::kStageTransition, 0, 0, 0, &transition) || static_cast<int32_t>(transition) == 0) {
        *reinterpret_cast<volatile uint8_t*>(gs + 0x1520) = 0;
        say("travel: the stage transition to 0x%08x was refused", host.map);
        return false;
    }
    say("travel: from map 0x%08x to the host's 0x%08x at %.1f %.1f %.1f", w.map, host.map, host.pos[0], host.pos[1], host.pos[2]);
    note(9950013, "Co-op: going to your partner");
    return true;
}

void ring(const World& w, double t) {
    const bool host = !S.guest;
    const int attempt = ++J.ring_attempts;
    J.last_ring = t;
    if (attempt > 6) {  // three tries of each kind did nothing: wait a minute
        J.ring_attempts = 0;
        J.backoff_until = t + 60.0;
        say("ring: no bell effect after 6 tries; trying again in 60 s");
        return;
    }
    // Odd tries use the item, as the bell itself does (Wozzardman; the one seen to
    // work live); even ones raise the Lua event the bell raises (bbhost's own
    // network test: no ringing animation).
    if (attempt % 2 == 1) {
        const int32_t goods = host ? addrs::kBeckoningBell : addrs::kSmallResonantBell;
        uint64_t result = 0;
        const uint64_t player = main_player();
        const bool called = player && call(addrs::kUseItem, player, static_cast<uint64_t>(goods), addrs::kBellUseArg, &result);
        say("ring: %s bell (goods %d) as an item at map 0x%08x area %d, try %d -> %s", host ? "Beckoning" : "Small Resonant",
             goods, w.map, w.area, attempt, called ? ((result & 0xFF) ? "used" : "refused") : "not called");
    } else {
        const char* event = host ? "OnEvent_Call_SOS" : "OnEvent_SendSoulSign_NormalCoop";
        const int rc = api->lua_event(event);
        say("ring: Lua event %s at map 0x%08x area %d, try %d -> %s", event, w.map, w.area, attempt, rc == 0 ? "queued" : "refused");
    }
    if (!J.rang_note) {
        J.rang_note = true;
        if (host) note(9950011, "Co-op: ringing for your partner");
        else note(9950012, "Co-op: ringing to join your partner");
    }
}

// Why an automatic step waits, said once a reason has held for 10 s apart (nullptr: it goes ahead).
struct Wait {
    const char* why;
    double since;
    bool said;
};

bool waits(Wait& s, const char* step, const char* why, const World& w, double t) {
    if (why != s.why) s = {why, t, false};
    if (why && !s.said && t - s.since >= 10.0 && !together(w)) {
        s.said = true;
        say("%s waits: %s (session %d, type %d, transition %d)", step, why, w.state, w.chr_type, w.transition);
    }
    return why != nullptr;
}

const char* unsettled(const World& w) {
    if (w.transition) return "a load or forced warp is pending";
    return w.stable ? nullptr : "the world is settling";
}

const char* join_wait(const World& w, double t) {
    if (!S.auto_ring) return "auto_ring is off";
    if (!g_functions_ok) return "the game functions were not found";
    if (!w.in_world) return "not in a world";
    if (w.chr_type != 0) return "not in its own world";
    if (w.in_dream) return "in the Dream or a chalice";
    if (paused(t)) return "paused";
    if (t < J.backoff_until) return "resting after six tries";
    if (!S.auto_rejoin && J.joined_once) return "auto_rejoin is off";
    return nullptr;
}

Wait g_ring_wait;

void guest_tick(const World& w, double t) {
    const char* why = join_wait(w, t);
    if (!why && !idle(w)) why = "busy online";
    if (!why && g_pending.count && g_pending_tried_for != C.world_since) why = "her progress goes into her world first";
    const Beat* host = peer_fresh(t) && !L.peer.guest ? &L.peer : nullptr;
    if (!why && L.partner_ok && !host) why = "no word from the host";
    if (!why && host) {
        if (!host->in_world || !host->stable) why = "the host is loading";
        else if (host->in_dream) why = "the host is in the Dream";
        else if (host->together) why = "the host has someone else";
    }
    if (!why && t - C.world_since < S.rejoin_delay) return;
    if (!why && S.follow_host && host && host->map != w.map) {
        travel_to_host(w, *host, t);
        return;
    }
    if (!why) why = unsettled(w);
    if (!why && api->sp_effect_has(nullptr, addrs::kSmallResonantEffect) == 1) why = "her bell is up";
    if (waits(g_ring_wait, "ring", why, w, t) || t - J.last_ring < 5.0) return;
    ring(w, t);
}

void host_tick(const World& w, double t) {
    const char* why = join_wait(w, t);
    if (!why) why = unsettled(w);
    if (!why && !(idle(w) || w.state == addrs::kStateHosting)) why = "busy online";
    if (!why && together(w)) why = "she is here";
    // Ring as soon as she wants to join or her bell is up, wherever she is (summon anywhere).
    const Beat* guest = peer_fresh(t) && L.peer.guest ? &L.peer : nullptr;
    if (!why && L.partner_ok && !guest) why = "no word from her";
    if (!why && guest && !guest->wants_join && !guest->bell) why = "she is not ringing";
    if (!why && api->sp_effect_has(nullptr, addrs::kBeckoningEffect) == 1) why = "the bell is up";
    if (waits(g_ring_wait, "ring", why, w, t) || t - J.last_ring < 5.0) return;
    ring(w, t);
}

void track_together(const World& w, double t) {
    const bool now = together(w);
    if (now && !J.was_together) {
        g_joined_at = t;
        J.joined_once = true;
        J.ring_attempts = 0;
        J.rang_note = false;
        say("session: together (state %d, chr_type %d, map 0x%08x)", w.state, w.chr_type, w.map);
        note(9950010, "Co-op: together");
    } else if (!now && J.was_together) {
        g_joined_at = 0;
        say("session: apart (state %d, chr_type %d, map 0x%08x%s)", w.state, w.chr_type, w.map, w.in_world ? "" : ", loading");
    }
    if (now) J.last_together_at = t;
    J.was_together = now;
}

// --- 3b. lamps while together ---------------------------------------------------------------
//
// kLamps gives both games their lamps back while together; the rest is what the
// game does not do for two:
//   - a lamp lit by either is lit in both worlds (Elden Ring's graces). Its "Light
//     Lamp" or lit flag coming on - past the first 10 s together and 10 s after a
//     load, as the host's world brings its own lamps along - goes into the
//     heartbeat's credits. The host's game turns on the lit flag of each the guest
//     lit (only that: the lighting event waits on "Light Lamp", so no scene plays);
//     the guest's game keeps both kinds for her own world, turned on once she is
//     home, as the boss kills are.
//   - Return to the Hunter's Dream takes the guest to her own: the game's event for
//     it ends at once in a guest, so the plugin takes her out of the session (the
//     game's Leave) and, from her own world once it has loaded, to her Hunter's
//     Dream (the headstones' lamp travel), where that event sends a lamp's user.

void lamp_revive(const addrs::Lamp& l);

void lamp_lit_together(const addrs::Lamp& l, const char* how) {
    const uint32_t lit = addrs::lamp_lit_flag(l);
    if (!g_credits.add(lit)) return;  // told already
    if (S.guest) remember_flag(lit);
    say("lamps: lamp %u-%u lit together (%s); %s", l.map, l.n, how,
        S.guest ? "telling the host; it is lit in your world once you are home" : "telling the guest");
    lamp_revive(l);
}

// guest: a lamp the host lit while together, for her own world.
void lamp_from_host(uint32_t lit) {
    const addrs::Lamp* l = lamp_by(lit, addrs::lamp_lit_flag);
    if (!S.lamps || !l || g_credits.has(lit) || !remember_flag(lit)) return;  // in g_credits: she lit it herself
    say("lamps: the host lit lamp %u-%u while together; it is lit in your world once you are home", l->map, l->n);
}

// host: the lamps the guest lit here, lit in this world too. One that cannot be set
// now is tried again with the next heartbeat.
void lamps_from_guest(const Beat& b) {
    if (S.guest || !b.guest || !S.lamps || !ready_for_items()) return;
    const int n = b.credit_count < 24 ? b.credit_count : 24;
    for (int i = 0; i < n; ++i) {
        const uint32_t lit = b.credits[i];
        const addrs::Lamp* l = lamp_by(lit, addrs::lamp_lit_flag);
        if (!l || g_credits.has(lit)) continue;
        int was = -1;
        if (api->event_flag_get(lit, &was) != 0 || (was == 0 && api->event_flag_set(lit, 1) != 0)) continue;
        g_credits.add(lit);  // before its change comes back through on_flag
        say("lamps: the guest lit lamp %u-%u; %s", l->map, l->n, was ? "it was lit here already" : "lit here too");
        if (!was) note(9950015, "Co-op: her lamp is lit for you too");
    }
}

struct LampHome {
    const addrs::Lamp* lamp;  // set while she is on the way
    double since;
    bool asked_again;
};
LampHome H;

void lamp_home_start(const addrs::Lamp& l, double t) {
    api->event_flag_set(addrs::lamp_return_flag(l), 0);  // as the lamp's event does: the next use comes on again
    H.lamp = &l;
    H.since = t;
    H.asked_again = false;
    J.pause_until = t + S.pause_seconds;
    const int rc = api->lua_event("OnLeaveMenu_Yes");
    say("lamps: Return to the Hunter's Dream at lamp %u-%u; leaving the session (OnLeaveMenu_Yes %s), then to your own Dream; "
        "re-joining paused for %d s",
        l.map, l.n, rc == 0 ? "queued" : "refused", S.pause_seconds);
    note(9950016, "Co-op: back to your Hunter's Dream");
}

void lamp_home_tick(const World& w, double t) {
    if (!H.lamp) return;
    if (t - H.since > 120.0) {
        say("lamps: not home 120 s after the lamp; the trip to the Hunter's Dream is dropped");
        H.lamp = nullptr;
        return;
    }
    if (together(w)) {
        if (!H.asked_again && t - H.since > 10.0) {
            H.asked_again = true;
            const int rc = api->lua_event("OnLeaveMenu_Yes");
            say("lamps: still together 10 s after the lamp; leaving again (OnLeaveMenu_Yes %s)", rc == 0 ? "queued" : "refused");
        }
        return;
    }
    // Home: her own world, loaded since the lamp, settled.
    if (!w.stable || w.chr_type != 0 || C.world_since <= H.since) return;
    const addrs::Lamp& l = *H.lamp;
    H.lamp = nullptr;
    if ((w.map >> 16) == addrs::kDreamMap) {
        say("lamps: home in the Hunter's Dream already");
        return;
    }
    int first_done = 0;
    api->event_flag_get(addrs::kDreamReturnFlag, &first_done);
    const int32_t to = first_done == 1 ? l.dream : addrs::kDreamReturnFirst;
    const int rc = api->lamp_warp(to);
    say("lamps: home (map 0x%08x); to the Hunter's Dream, return point %d -> %s", w.map, to, rc == 0 ? "on the way" : "not allowed");
}

// A lamp flag on (from on_flag, at the start of the frame after the change).
void lamp_flag_on(uint32_t id) {
    if (!S.lamps || !together(g_last)) return;
    const double t = now_s();
    if (const addrs::Lamp* l = lamp_by(id, addrs::lamp_return_flag)) {
        if (!S.guest) return;  // the host's own lamp event takes the host
        if (H.lamp || (g_last.map >> 16) != addrs::lamp_map(*l)) {
            api->event_flag_set(id, 0);
            say("lamps: lamp %u-%u's Return came on %s; turned off", l->map, l->n, H.lamp ? "while on the way already" : "away from it");
            return;
        }
        lamp_home_start(*l, t);
        return;
    }
    const char* how = "Light Lamp";
    const addrs::Lamp* l = lamp_by(id, addrs::lamp_light_flag);
    if (!l) {
        how = "lit";
        l = lamp_by(id, addrs::lamp_lit_flag);
    }
    if (!l || t - C.world_since < 10.0 || (S.guest && (g_joined_at <= 0 || t - g_joined_at < 10.0))) return;
    lamp_lit_together(*l, how);
}

// --- 6. Elden Ring style deaths -------------------------------------------------------------
//
// While the two are together the game's Player No Dead switch is on: a blow
// that would kill leaves 1 HP instead (the no-death check 0x01A577D0; the
// damage clamp in 0x01A2A770). 1 HP is taken as "down" (a blow that leaves
// exactly 1 HP is taken so too). The one down is out of the fight - unhurt,
// unseen and unheard by enemies, still (out of sight with pick_up off) - and
// the camera follows the partner. It ends:
//   - when a boss dies: back up at full HP, next to the partner;
//   - when the partner picks them up (near them, the game's "Pick Up Item"
//     prompt; one Blood Vial, greyed without one) or lights a lamp (free):
//     back up the same way;
//   - when the partner goes down too: both die for real (the host respawns at
//     the lamp, the guest goes home; re-join brings her back);
//   - when the session ends (a lamp, a leave, a load): a guest goes home
//     anyway, a host left alone dies for real;
//   - Give up in the menu: die now.
// Each machine does this for its own player; the heartbeat says who is down,
// and the partner's enemies ignore the one down (with pick_up off, the
// partner's screen hides them too). A pick-up or a lamp is asked for in the
// heartbeat (revive_ask) and answered there (revived_for); the vial is used
// once the answer comes.

struct Down {
    bool protect;          // Player No Dead is on, by us
    double protect_said;   // when it was last written (written again now and then)
    bool down;
    uint64_t player;       // the PlayerIns put down
    uint8_t saved_flags1, saved_flags2, saved_data;  // its bits as they were
    uint64_t camera;       // the partner's character the camera follows (0: none set by us)
    bool released;         // died for real: nothing more until the next load
    double released_at;
    double apart_since;
    double last_banner;
    uint64_t hidden_peer;  // the partner's character on this screen while they are down (hidden with pick_up off)
    bool warned_write;
    double switches_at;    // when the game's switches were last written while down
    int32_t kneel;         // the kneel played (kKneelAnim + 0, 10 or 20), 0: none
    double kneel_at;
    bool kneel_held;       // its loop started
    bool no_control;       // the player's logic turned off by us
};
Down D;

// Getting the partner up: this side asks with a new number in the heartbeat's
// revive_ask until the answer comes; the side down gets up once for each new
// number and says so in revived_for.
struct Revive {
    uint16_t ask;       // the last number asked (on from the run's, never 0)
    uint8_t how;        // 1 a pick-up (1 Blood Vial), 2 a lamp (free)
    bool pending;       // asked, no answer yet
    double asked_at;
    uint16_t answered;  // the side down: the last ask it got up for
    bool near;          // close enough to pick the partner up, last frame
    bool vial_said;     // "you need a Blood Vial" shown for this time near
};
Revive R;

bool set_debug_flag(uint64_t va, bool on) {
    const uint8_t v = on ? 1 : 0;
    if (api->write(bn(va), &v, 1) == 0) return true;
    if (!D.warned_write) {
        D.warned_write = true;
        say("down: the game's switch at 0x%llx could not be written; deaths stay as the game has them",
            static_cast<unsigned long long>(va));
    }
    return false;
}

// A byte of the game's heap: read (checked), then the bits changed.
bool poke_bits(uint64_t at, uint8_t set, uint8_t clear) {
    uint8_t b = 0;
    if (!rd(at, &b, 1)) return false;
    const uint8_t n = static_cast<uint8_t>((b & ~clear) | set);
    if (n != b) *reinterpret_cast<volatile uint8_t*>(at) = n;
    return true;
}

uint8_t peek(uint64_t at) {
    uint8_t b = 0;
    rd(at, &b, 1);
    return b;
}

uint64_t chr_data(uint64_t chr) { return field(field(chr, addrs::kChrModules), 0x20); }

bool chr_hp(uint64_t chr, int32_t* hp, int32_t* max) {
    const uint64_t data = chr_data(chr);
    return data && rd(data + addrs::kDataHp, hp, 4) && rd(data + addrs::kDataMaxHp, max, 4);
}

// The partner's character on this screen: the other player character
// standing where the partner's heartbeat says they are (within 3 m, in this
// block); 0 when none does. The game holds seven other player characters,
// most of them unused (and where they stand is not kept up), so the nearest
// alone is no proof. Looked for every half second; in between, the one found
// while it is still a player character not being deleted.
uint64_t g_partner;
double g_partner_at;

uint64_t partner_chr() {
    const double t = now_s();
    if (!peer_fresh(t) || !L.peer.in_world || L.peer.block != g_last.block) return g_partner = 0;
    const uint64_t main_vt = rp(main_player());
    if (t - g_partner_at < 0.5) {
        if (g_partner && (!main_vt || rp(g_partner) != main_vt || (peek(g_partner + addrs::kChrFlags1) & addrs::kChrDeleting)))
            g_partner = 0;
        return g_partner;
    }
    g_partner_at = t;
    g_partner = 0;
    BbChr chrs[96];
    size_t count = 0;
    if (!main_vt || api->chr_list(chrs, 96, &count) != 0) return 0;
    if (count > 96) count = 96;
    float best_d = 9.0f;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t ins = addr(chrs[i].ins);
        if (chrs[i].is_player || chrs[i].npc_param != -1 || rp(ins) != main_vt) continue;
        if (peek(ins + addrs::kChrFlags1) & addrs::kChrDeleting) continue;
        const float dx = chrs[i].pos[0] - L.peer.pos[0], dy = chrs[i].pos[1] - L.peer.pos[1], dz = chrs[i].pos[2] - L.peer.pos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < best_d) {
            g_partner = ins;
            best_d = d;
        }
    }
    return g_partner;
}

// Whether a character is still one WorldChrMan holds (not freed).
bool chr_present(uint64_t ins) {
    BbChr chrs[96];
    size_t count = 0;
    if (!ins || api->chr_list(chrs, 96, &count) != 0) return false;
    if (count > 96) count = 96;
    for (size_t i = 0; i < count; ++i)
        if (addr(chrs[i].ins) == ins) return !(peek(ins + addrs::kChrFlags1) & addrs::kChrDeleting);
    return false;
}

void set_protect(bool on, double t) {
    if (on == D.protect && (!on || t - D.protect_said < 1.0)) return;
    if (set_debug_flag(addrs::kPlayerNoDead, on) || !on) D.protect = on;
    D.protect_said = t;
}

// The camera: the partner's character, or (0) back to the game's own choice.
void set_camera(uint64_t target) {
    const uint64_t dbg = singleton(addrs::kWorldChrManDbg);
    uint64_t now = 0;
    if (!dbg || !rd(dbg + 0x108, &now, 8)) return;
    if (target) {
        if (now != target && (now == 0 || now == D.camera)) *reinterpret_cast<volatile uint64_t*>(dbg + 0x108) = target;
        D.camera = target;
    } else if (D.camera) {
        if (now == D.camera) *reinterpret_cast<volatile uint64_t*>(dbg + 0x108) = 0;
        D.camera = 0;
    }
}

// The one down: unhurt, invincible, unseen and unheard by enemies, still and
// (with pick_up off) not drawn. Off puts back exactly the bits it changed.
// The game's switches are written when it changes and once a second; the
// character's bits each frame (the game may set them back).
void down_flags(bool on, bool switches = true) {
    if (switches) {
        set_debug_flag(addrs::kPlayerHide, on);
        set_debug_flag(addrs::kPlayerSilence, on);
    }
    // Only while it is still the main player: after a load starts, the
    // character may be gone (the game makes the new one whole anyway).
    const uint64_t p = D.player;
    if (!p || p != main_player()) return;
    const uint64_t data = chr_data(p);
    if (on) {
        poke_bits(p + addrs::kChrFlags1, addrs::kChrInvincible, S.pick_up ? 0 : addrs::kChrDraw);
        poke_bits(p + addrs::kChrFlags2, addrs::kChrNoMove, 0);
        if (data) poke_bits(data + addrs::kDataFlags, addrs::kDataNoDamage, 0);
    } else {
        const uint8_t f1 = addrs::kChrInvincible | addrs::kChrDraw;
        poke_bits(p + addrs::kChrFlags1, D.saved_flags1 & f1, static_cast<uint8_t>(~D.saved_flags1 & f1));
        poke_bits(p + addrs::kChrFlags2, D.saved_flags2 & addrs::kChrNoMove, static_cast<uint8_t>(~D.saved_flags2 & addrs::kChrNoMove));
        if (data)
            poke_bits(data + addrs::kDataFlags, D.saved_data & addrs::kDataNoDamage,
                      static_cast<uint8_t>(~D.saved_data & addrs::kDataNoDamage));
    }
}

// The one down kneels with no control; standing up comes with the revive.
void down_pose(bool on, double t) {
    const uint64_t me = static_cast<uint64_t>(addrs::kLocalPlayerEntity);
    if (on) {
        if (!g_functions_ok) return;
        D.kneel = addrs::kKneelAnim + 10 * static_cast<int32_t>(C.frame % 3);
        D.kneel_at = t;
        D.kneel_held = false;
        const bool knelt = call(addrs::kForceAnim, 0, me, static_cast<uint64_t>(D.kneel));
        D.no_control = call(addrs::kEnableLogic, 0, me, 0);
        say("down: kneeling (%d) %s; control %s", D.kneel, knelt ? "played" : "not played", D.no_control ? "off" : "NOT off");
    } else if (D.no_control) {
        D.no_control = false;
        const bool back = call(addrs::kEnableLogic, 0, me, 1);
        say("down: control %s", back ? "back" : "NOT back");
    }
}

void go_down(const World& w, double t, uint64_t player) {
    D.down = true;
    D.player = player;
    D.apart_since = 0;
    D.saved_flags1 = peek(player + addrs::kChrFlags1);
    D.saved_flags2 = peek(player + addrs::kChrFlags2);
    const uint64_t data = chr_data(player);
    D.saved_data = data ? peek(data + addrs::kDataFlags) : 0;
    down_flags(true);
    down_pose(true, t);
    say("down: you would have died (map 0x%08x at %.1f %.1f %.1f); waiting for the boss to die, your partner to pick you up "
        "or light a lamp, your partner to fall or the session to end",
        w.map, w.pos[0], w.pos[1], w.pos[2]);
    note(9950017, "Co-op: you are down - your partner fights on");
    D.last_banner = t;
}

// Back to how the game had it, without a death: a load, a guest going home.
void down_reset(const char* why) {
    if (D.down || D.camera) say("down: over (%s)", why);
    if (D.down) down_flags(false);
    if (D.player && D.player == main_player()) down_pose(false, now_s());
    D.no_control = false;  // a new character has its own
    D.kneel = 0;
    set_camera(0);
    D.down = false;
    D.player = 0;
    D.apart_since = 0;
}

// Both down, or alone: die for real. Nothing more until the next load.
void release(const char* why) {
    const uint64_t player = D.player;
    down_reset(why);
    set_debug_flag(addrs::kPlayerNoDead, false);
    D.protect = false;
    D.released = true;
    D.released_at = now_s();
    uint64_t ret = 0;
    const bool called = player && player == main_player() &&
                        call(addrs::kForceDead, 0, static_cast<uint64_t>(addrs::kLocalPlayerEntity), 0, &ret);
    say("down: dying for real (%s) -> %s", why, called ? "ForceDead called" : "ForceDead not called; HP set to 0");
    if (!called) api->player_stat_set("hp", 0);
    if (S.guest) note(9950018, "Co-op: you died - going home");  // the host has the game's own YOU DIED
}

// Up again, at full HP, next to the partner when they are close.
void revive(const World& w, double t, const char* why) {
    const uint64_t player = D.player;
    const int32_t kneel = D.kneel;
    down_reset(why);
    if (kneel && player && player == main_player())
        call(addrs::kForceAnim, 0, static_cast<uint64_t>(addrs::kLocalPlayerEntity), static_cast<uint64_t>(kneel + 2));
    int32_t hp = 0, max = 0;
    if (player && player == main_player() && chr_hp(player, &hp, &max) && max > 0) {
        call(addrs::kChrSetHp, player, static_cast<uint64_t>(max));
        api->player_stat_set("hp", max);
    }
    if (peer_fresh(t) && L.peer.in_world && L.peer.map == w.map) {
        const float dx = L.peer.pos[0] - w.pos[0], dy = L.peer.pos[1] - w.pos[1], dz = L.peer.pos[2] - w.pos[2];
        if (L.peer.block != w.block || dx * dx + dy * dy + dz * dz > 25.0f) {
            const float to[3] = {L.peer.pos[0] + 1.0f, L.peer.pos[1], L.peer.pos[2]};
            const int rc = api->player_warp(L.peer.block == w.block ? 0 : L.peer.block, to, L.peer.yaw);
            say("down: moving next to the partner %s", rc == 0 ? "(queued)" : "refused");
        }
    }
    say("down: back up (%s), HP %d", why, max);
    note(9950020, "Co-op: back on your feet");
}

double g_boss_revive_at;  // a boss died: revive at the next frame (callbacks may run between frames)

void downed_boss_died(uint32_t id, const char* how) {
    if (!D.down) return;
    say("down: %s (%u) is dead (%s)", boss_name(id), id, how);
    g_boss_revive_at = now_s();
}

bool g_give_up;

void downed_tick(const World& w, double t) {
    if (!w.in_world) {  // a load: the game makes everyone whole again
        if (D.down || D.camera) down_reset("loading");
        if (D.protect) set_protect(false, t);
        D.released = false;
        return;
    }
    if (D.released) {
        if (t - D.released_at > 20.0 && w.stable) D.released = false;  // the death did not take us out of the world
        return;
    }
    const uint64_t player = main_player();
    if (D.down && player != D.player) down_reset("another character");
    const bool tog = together(w);
    if (!D.down) {
        g_give_up = false;
        g_boss_revive_at = 0;
        set_protect(S.downed && tog, t);
        int32_t hp = 0, max = 0;
        if (D.protect && tog && chr_hp(player, &hp, &max) && max > 1 && hp == 1) go_down(w, t, player);
        return;
    }
    set_protect(true, t);
    const bool second = t - D.switches_at >= 1.0;
    if (second) D.switches_at = t;
    down_flags(true, second);
    if (D.kneel && !D.kneel_held && t - D.kneel_at >= 4.0) {  // knelt down: hold it
        D.kneel_held = true;
        call(addrs::kForceLoopAnim, 0, static_cast<uint64_t>(addrs::kLocalPlayerEntity), static_cast<uint64_t>(D.kneel + 1));
    }
    if (second && D.no_control) call(addrs::kEnableLogic, 0, static_cast<uint64_t>(addrs::kLocalPlayerEntity), 0);
    if (g_boss_revive_at > 0) {
        g_boss_revive_at = 0;
        revive(w, t, "the boss is dead");
        return;
    }
    // The partner picked us up or lit a lamp: each ask counts once.
    if (tog && peer_fresh(t) && L.peer.together && L.peer.revive_ask && L.peer.revive_ask != R.answered) {
        R.answered = L.peer.revive_ask;
        revive(w, t, L.peer.revive_how == 2 ? "your partner lit a lamp" : "your partner picked you up");
        return;
    }
    if (!S.downed) {
        revive(w, t, "the switch was turned off");
        return;
    }
    if (g_give_up) {
        g_give_up = false;
        release("you gave up");
        return;
    }
    if (tog && peer_fresh(t) && L.peer.downed && L.peer.together) {
        release(L.peer.downed == 2 ? "your partner died" : "your partner is down too");
        return;
    }
    if (!tog) {
        if (!D.apart_since) D.apart_since = t;
        // A guest is sent home (a load) soon after a session ends; a host is left alone.
        if (t - D.apart_since > (S.guest ? 15.0 : 3.0)) {
            release(S.guest ? "the session ended and the game did not send you home" : "your partner is gone");
            return;
        }
    } else {
        D.apart_since = 0;
    }
    // Watch the partner - only while their heartbeat says they are in this
    // world (a partner leaving is told before their character goes).
    const bool watchable = tog && L.peer_at > 0 && t - L.peer_at < 1.0 && L.peer.in_world && L.peer.together;
    const uint64_t partner = S.spectate && watchable ? partner_chr() : 0;
    set_camera(partner);
    if (t - D.last_banner > 20.0) {
        D.last_banner = t;
        if (S.pick_up && S.lamp_revive) note(9950021, "Co-op: you are down - your partner can revive you, light a lamp or kill the boss");
        else if (S.pick_up) note(9950022, "Co-op: you are down - your partner can revive you or kill the boss");
        else if (S.lamp_revive) note(9950023, "Co-op: you are down - your partner can light a lamp or kill the boss");
        else note(9950024, "Co-op: you are down - waiting for the boss");
    }
}

// The partner's character this screen's enemies do not see (0: none), and
// this player while down (Player Hide alone did not stop a fight already
// going). Set on the main thread; read by the hook below, which runs wherever
// enemies think.
uint64_t g_unseen, g_unseen_self;

// Enemies here, while the partner is down: the record of what an enemy sees
// of the partner's character is filled as the game fills it (0x01A55390),
// but says "hidden" - what Player Hide does for the main player, which this
// screen cannot set for someone else's character.
int on_sight_record(BbHookCtx* ctx, void*) {
    const uint64_t unseen = __atomic_load_n(&g_unseen, __ATOMIC_RELAXED), self = __atomic_load_n(&g_unseen_self, __ATOMIC_RELAXED);
    if (!unseen && !self) return 0;
    const uint64_t rec = ctx->arg[1];
    uint64_t chr = 0;
    if (!rec || !rd(ctx->arg[0] + 8, &chr, 8) || !chr || (chr != unseen && chr != self)) return 0;
    uint8_t look = 0, hide_rate_set = 0, pos[16];
    uint32_t size[2];
    const uint64_t m = field(field(chr, 0x3B0), 0x68);
    const uint64_t at = field(field(field(field(chr, 0x58), 8), 0x3B0), 0x68);
    if (rd(chr + 0x281, &look, 1)) *reinterpret_cast<volatile uint8_t*>(rec + 0x5C) = look & 0xF;
    if (m && rd(m + 0x354, size, 8)) memcpy(reinterpret_cast<void*>(rec + 0x50), size, 8);
    if (at && rd(at + 0x1E0, pos, 16)) memcpy(reinterpret_cast<void*>(rec + 0x40), pos, 16);
    if (rd(rec + 0x3D, &hide_rate_set, 1) && !hide_rate_set) *reinterpret_cast<volatile float*>(rec + 0x58) = 0.0f;
    *reinterpret_cast<volatile uint8_t*>(rec + 0x64) = 1;
    return 1;
}

bool g_peer_down;  // the partner is down, as this screen last saw it

// This screen, while the partner is down: enemies do not see their character,
// and with pick_up off it is not drawn either.
void peer_view_tick(const World& w, double t) {
    const bool peer_down = w.in_world && together(w) && peer_fresh(t) && L.peer.downed == 1;
    const uint64_t chr = peer_down ? partner_chr() : 0;
    if (peer_down != g_peer_down) {
        g_peer_down = peer_down;
        if (peer_down) {
            say("down: your partner is down (%.1f m away; their character here %s)",
                root((L.peer.pos[0] - w.pos[0]) * (L.peer.pos[0] - w.pos[0]) + (L.peer.pos[2] - w.pos[2]) * (L.peer.pos[2] - w.pos[2])),
                chr ? "found: enemies here ignore it" : "not found");
            if (S.pick_up && S.lamp_revive) note(9950025, "Co-op: your partner is down - revive them, use a lamp or kill the boss");
            else if (S.pick_up) note(9950026, "Co-op: your partner is down - revive them or kill the boss");
            else if (S.lamp_revive) note(9950027, "Co-op: your partner is down - use a lamp or kill the boss");
            else note(9950028, "Co-op: your partner is down - kill the boss to bring them back");
        } else {
            say("down: your partner is %s", L.peer.downed == 2 ? "dead" : together(w) ? "up again" : "gone");
        }
    }
    if (D.hidden_peer && chr != D.hidden_peer) {  // up again, gone, or another character now: show the old one
        if (chr_present(D.hidden_peer)) poke_bits(D.hidden_peer + addrs::kChrFlags1, addrs::kChrDraw, 0);
        D.hidden_peer = 0;
    }
    if (chr) {
        D.hidden_peer = chr;
        if (!S.pick_up) poke_bits(chr + addrs::kChrFlags1, 0, addrs::kChrDraw);
    }
    __atomic_store_n(&g_unseen, D.hidden_peer, __ATOMIC_RELAXED);
    __atomic_store_n(&g_unseen_self, D.down ? D.player : 0, __ATOMIC_RELAXED);
}

// The player's count of a goods item (0: none) and its inventory record, as
// the game's own lookup finds it.
int32_t goods_count(int32_t id, uint64_t* record = nullptr) {
    static uint32_t key;  // the game reads it
    const uint64_t pgd = field(singleton(addrs::kGameDataMan), 8);
    if (!pgd) return 0;
    key = addrs::kGoodsBits | static_cast<uint32_t>(id);
    uint64_t ret = 0;
    if (!call(addrs::kItemIndex, pgd + addrs::kInventoryList, addr(&key), 0, &ret)) return 0;
    const uint32_t index = static_cast<uint32_t>(ret);
    uint32_t split = 0, last = 0;
    if (index == 0xFFFFFFFFu || !rd(pgd + addrs::kInventorySplit, &split, 4) || !rd(pgd + addrs::kInventoryLast, &last, 4) ||
        index > last)
        return 0;
    const uint64_t base = rp(pgd + (index < split ? addrs::kInventoryLow : addrs::kInventoryHigh));
    const uint64_t rec = base ? base + static_cast<uint64_t>(index < split ? index : index - split) * 16 : 0;
    uint32_t used = 0;
    int32_t count = 0;
    if (!rec || !rd(rec, &used, 4) || !used || !rd(rec + addrs::kItemCount, &count, 4) || count <= 0) return 0;
    if (record) *record = rec;
    return count;
}

// One of a goods item used up: the count lowered, or the last one taken out
// as the game's events take an item (with its quick slot).
bool use_goods(int32_t id) {
    uint64_t rec = 0;
    const int32_t n = goods_count(id, &rec);
    if (n <= 0) return false;
    if (n > 1) {
        *reinterpret_cast<volatile int32_t*>(rec + addrs::kItemCount) = n - 1;
        return true;
    }
    return call(addrs::kItemRemove, 0, addrs::kGoodsBits, static_cast<uint64_t>(id));
}

// --- 1. bells from the start -------------------------------------------------------------

bool flag_is_set(uint32_t id) {
    int v = 0;
    return api->event_flag_get(id, &v) == 0 && v == 1;
}

// A character in its own world, out of cutscenes (so after the first wake): the bell it lacks, once per load
// (and a look 2 s after). What it has is marked given for the game's own routes, so they give
// no second one: the messenger's gift (6622, which the Beckoning Bell's lot sets too) and the
// shop's one Small Resonant Bell (6610, as if bought).
struct Bells {
    double load;     // the load (C.world_since) last looked at
    double look_at;  // 2 s after a gift
    unsigned char* lent;  // lot 10020's row, until the look
    unsigned char saved[addrs::kLotRowBytes];
} B;
Wait g_bells_wait;

// The Small Resonant Bell through the empty lot 10020, its row lent for the award.
bool give_small_bell() {
    size_t bytes = 0;
    auto* row = static_cast<unsigned char*>(api->param_row("ItemLotParam", static_cast<uint32_t>(addrs::kSpareLot), &bytes));
    int32_t id = -1;
    if (!row || bytes < addrs::kLotRowBytes || (memcpy(&id, row + addrs::kLotItemId, 4), id != 0)) {
        say("bells: lot %d is not the empty one 1.09 has (item %d); no Small Resonant Bell", addrs::kSpareLot, id);
        return false;
    }
    memcpy(B.saved, row, sizeof(B.saved));
    B.lent = row;
    memcpy(row + addrs::kLotItemId, &addrs::kSmallResonantBell, 4);
    memcpy(row + addrs::kLotCategory, &addrs::kLotGoods, 4);
    row[addrs::kLotNum] = 1;
    return award_lot(addrs::kSpareLot);
}

void bells_tick(const World& w, double t) {
    if (B.lent && t >= B.look_at) {
        memcpy(B.lent, B.saved, sizeof(B.saved));
        B.lent = nullptr;
    }
    const bool again = B.look_at && t >= B.look_at;
    if (!S.give_bells || (B.load == C.world_since && !again)) return;
    const char* why = !w.in_world ? "not in a world" : !g_functions_ok ? "the game functions were not found" : unsettled(w);
    if (!why && w.chr_type != 0) why = "not in its own world";
    if (!why && flag_is_set(addrs::kCutscene)) why = "a cutscene is playing";
    if (waits(g_bells_wait, "bells", why, w, t)) return;
    B.load = C.world_since;
    B.look_at = 0;
    const bool big = goods_count(addrs::kBeckoningBell) > 0, small = goods_count(addrs::kSmallResonantBell) > 0;
    if (big && !flag_is_set(addrs::kBellsGiven)) api->event_flag_set(addrs::kBellsGiven, 1);
    if (small && !flag_is_set(addrs::kSmallBellSold)) api->event_flag_set(addrs::kSmallBellSold, 1);
    if (again) {
        say("bells: Beckoning Bell %s, Small Resonant Bell %s", big ? "yes" : "no (again at the next load)",
            small ? "yes" : "no (again at the next load)");
        if (big && small) note(9950029, "Co-op: you have both bells");
        return;
    }
    if (big && small) return;
    if (!big) say("bells: the Beckoning Bell (lot %d) -> %s", addrs::kBellLot, award_lot(addrs::kBellLot) ? "given" : "not given");
    if (!small) say("bells: the Small Resonant Bell (lot %d lent) -> %s", addrs::kSpareLot, give_small_bell() ? "given" : "not given");
    B.look_at = t + 2.0;
}

// The prompt's ActionButtonParam row: "Pick Up Item", or once make_revive_row
// has made it, a copy of that row reading "Revive" (the mod's event text).
int32_t g_prompt_row = addrs::kPromptPickUp;
bool g_revive_row_tried, g_revive_said;

// Params and texts load during the boot: once both are there (looked for each
// second), the spare row becomes "Pick Up Item" with the text "Revive".
void make_revive_row() {
    if (g_revive_row_tried || C.frame % 60 != 0) return;
    size_t from_n = 0, to_n = 0;
    constexpr int32_t kText = 9950000;
    const auto* from = static_cast<const uint8_t*>(api->param_row("ActionButtonParam", addrs::kPromptPickUp, &from_n));
    if (!from || !singleton(addrs::kMsgRepositoryPtr)) return;  // not loaded yet
    if (!message_text(addrs::kGroupEventText, kText)) {
        if (!g_revive_said) say("prompt: no Revive text yet (the mod \"Co-op - menu and text\"); the prompt is Pick Up Item");
        g_revive_said = true;
        return;
    }
    g_revive_row_tried = true;
    auto* to = static_cast<uint8_t*>(api->param_row("ActionButtonParam", addrs::kReviveRow, &to_n));
    bool empty = to && to_n == from_n;
    for (size_t i = 0; empty && i < to_n; ++i) empty = to[i] == 0;
    if (!to || to_n != from_n || from_n < addrs::kButtonTextOff + 4 || !empty) {
        say("prompt: ActionButtonParam row %u is %s; the prompt stays Pick Up Item", addrs::kReviveRow,
            !to ? "missing" : "not the empty row it was");
        return;
    }
    memcpy(to, from, from_n);
    memcpy(to + addrs::kButtonTextOff, &kText, 4);
    g_prompt_row = static_cast<int32_t>(addrs::kReviveRow);
    say("prompt: Revive (ActionButtonParam row %u, a copy of %d)", addrs::kReviveRow, addrs::kPromptPickUp);
}

// The game's "Pick Up Item" prompt at the partner, asked for each frame it
// should show (dist: how far they are; greyed: shown, not usable). True on the
// frame it is used.
bool ask_prompt(float dist, bool greyed) {
    const uint64_t man = singleton(addrs::kActionButtonMan);
    if (!man) return false;
    BbCallRegs r = {};
    r.xmm[0] = bb_f32(dist);
    r.arg[0] = man;
    r.arg[1] = addrs::kPromptKey;
    r.arg[2] = static_cast<uint64_t>(g_prompt_row);
    r.arg[4] = greyed;
    return api->call_guest(bn(addrs::kActionPrompt.va), &r) == 0 && (r.ret & 0xFF) != 0;
}

bool partner_is_down(const World& w, double t) {
    return w.in_world && together(w) && peer_fresh(t) && L.peer.downed == 1 && L.peer.together;
}

void ask_partner_up(double t, uint8_t how) {
    if (++R.ask == 0) R.ask = 1;
    R.how = how;
    R.pending = true;
    R.asked_at = t;
    say("down: asking your partner to get up (%s, ask %u)", how == 2 ? "a lamp" : "a pick-up", R.ask);
}

// A lamp lit together (3b) while the partner is down gets them up.
void lamp_revive(const addrs::Lamp& l) {
    const double t = now_s();
    if (!S.lamp_revive || D.down || R.pending || !partner_is_down(g_last, t)) return;
    say("down: lamp %u-%u lit while your partner is down", l.map, l.n);
    ask_partner_up(t, 2);
}

// The frame a lamp's prompt was last asked for (0: never): set by the hook
// below, wherever the talk scripts run.
uint32_t g_lamp_prompt_frame;

int on_action_prompt(BbHookCtx* ctx, void*) {
    if (addrs::lamp_prompt(static_cast<int32_t>(ctx->arg[2])))
        __atomic_store_n(&g_lamp_prompt_frame, C.frame ? C.frame : 1, __ATOMIC_RELAXED);
    return 0;
}

// Getting the partner up. Next to them (2.5 m across, 2 m up or down, by both
// heartbeats: where each game has its own player) the game's Pick Up Item
// prompt shows (greyed without a Blood Vial); pressed, it asks them up, and
// the vial goes once they are. At a lamp (its prompt asked
// for in the last half second), free.
void pick_up_tick(const World& w, double t) {
    if (R.pending) {
        if (L.peer.revived_for == R.ask) {
            R.pending = false;
            if (R.how == 1) {
                const bool used = use_goods(addrs::kBloodVial);
                say("down: your partner is up (picked up; %s)", used ? "1 Blood Vial used" : "no Blood Vial left to use");
                if (used) note(9950030, "Co-op: your partner is up (1 Blood Vial)");
                else note(9950031, "Co-op: your partner is up");
            } else {
                say("down: your partner is up (the lamp)");
                note(9950031, "Co-op: your partner is up");
            }
        } else if (t - R.asked_at > 10.0 || !partner_is_down(w, t)) {
            R.pending = false;
            say("down: ask %u not answered (%s); nothing used", R.ask,
                t - R.asked_at > 10.0 ? "10 s" : L.peer.downed == 1 ? "apart" : "your partner is not down");
        }
    }
    const bool down = !D.down && !R.pending && g_functions_ok && partner_is_down(w, t) && L.peer.block == w.block;
    const uint32_t lamp = __atomic_load_n(&g_lamp_prompt_frame, __ATOMIC_RELAXED);
    if (down && S.lamp_revive && lamp && C.frame - lamp < 30) {
        say("down: at a lamp while your partner is down");
        ask_partner_up(t, 2);
        return;
    }
    const float dx = L.peer.pos[0] - w.pos[0], dy = L.peer.pos[1] - w.pos[1], dz = L.peer.pos[2] - w.pos[2];
    if (!down || !S.pick_up || dx * dx + dz * dz > 2.5f * 2.5f || dy * dy > 2.0f * 2.0f) {
        R.near = false;
        R.vial_said = false;
        return;
    }
    const float dist = root(dx * dx + dy * dy + dz * dz);
    const int32_t vials = goods_count(addrs::kBloodVial);
    if (!R.near) {
        say("down: next to your partner (%.1f m), %d Blood Vial(s)", dist, vials);
    }
    R.near = true;
    if (vials <= 0 && !R.vial_said) {
        R.vial_said = true;
        note(9950033, "Co-op: you need a Blood Vial to revive your partner");
    }
    const bool pressed = ask_prompt(dist, vials <= 0);
    if (vials > 0 && pressed) ask_partner_up(t, 1);
}

// --- enemy health while together ---------------------------------------------------------

// Enemies made stronger: by ChrIns and handle, their own max HP and the one
// given. While together every enemy's max HP (and its HP, in proportion) is
// party_health % of its own, as the game's Lua max-HP rate writes it; apart,
// it goes back. Some enemies' max HP the game sets again and again: those are
// left as the game has them (HP raised once at most).
struct Boosted {
    uint64_t ins;
    uint32_t handle;
    int32_t own, given;
    uint8_t resets;  // times the game set its max HP again; 2: left alone
    bool seen;
};
Boosted g_boosted[512];  // by ins up to the last tick's count, then what it added
int g_boosted_count;
double g_party_at, g_party_on_at;

int cmp_boosted(const void* a, const void* b) {
    const uint64_t x = static_cast<const Boosted*>(a)->ins, y = static_cast<const Boosted*>(b)->ins;
    return x < y ? -1 : x > y;
}

void set_max_hp(uint64_t ins, int32_t from, int32_t to, bool scale_hp) {
    const uint64_t data = chr_data(ins);
    int32_t hp = 0;
    if (!data || from <= 0 || !rd(data + addrs::kDataHp, &hp, 4)) return;
    *reinterpret_cast<volatile int32_t*>(data + addrs::kDataMaxHp) = to;
    if (hp <= 0 || (!scale_hp && hp <= to)) return;
    int64_t now = scale_hp ? static_cast<int64_t>(hp) * to / from : to;
    now = now < 1 ? 1 : now > to ? to : now;
    *reinterpret_cast<volatile int32_t*>(data + addrs::kDataHp) = static_cast<int32_t>(now);
}

void party_tick(const World& w, double t) {
    if (t - g_party_at < 1.0) return;
    g_party_at = t;
    // Together, and she is in his world by both games (the host's game holds
    // seven player characters even alone, so its "others" is no proof).
    const bool on = S.party_health != 100 && !w.in_dream && together(w) && peer_fresh(t) && L.peer.in_world &&
                    (S.guest ? in_host_world(w) : L.peer.chr_type == 1);
    if (on) g_party_on_at = t;
    if (!on && (!g_boosted_count || t - g_party_on_at < 5.0)) return;  // apart for 5 s: not a load's blink
    static BbChr chrs[512];
    size_t count = 0;
    if (!w.in_world || api->chr_list(chrs, 512, &count) != 0) return;
    if (count > 512) count = 512;
    for (int i = 0; i < g_boosted_count; ++i) g_boosted[i].seen = false;
    const size_t sorted = static_cast<size_t>(g_boosted_count);
    int raised = 0, lowered = 0, left = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t ins = addr(chrs[i].ins);
        if (chrs[i].is_player || chrs[i].npc_param == -1 || (peek(ins + addrs::kChrFlags1) & addrs::kChrDeleting)) continue;
        const int32_t hp = chrs[i].hp, max = chrs[i].max_hp;
        if (max <= 0) continue;
        const Boosted key = {ins, 0, 0, 0, 0, false};
        Boosted* b = static_cast<Boosted*>(bsearch(&key, g_boosted, sorted, sizeof(Boosted), cmp_boosted));
        if (b && b->handle != chrs[i].handle) b = nullptr;  // a new character where an old one was
        if (on && !b) {
            if (hp <= 0 || g_boosted_count == 512) continue;
            b = &g_boosted[g_boosted_count++];
            *b = {ins, chrs[i].handle, max, max, 0, false};
        }
        if (!b) continue;
        b->seen = true;
        if (b->resets >= 2) continue;
        bool reset = false;
        if (max != b->given) {  // the game set it again
            reset = true;
            b->own = b->given = max;
            if (++b->resets >= 2) {
                ++left;
                continue;
            }
        }
        const int64_t want = on ? static_cast<int64_t>(b->own) * S.party_health / 100 : b->own;
        if (max != want) {
            set_max_hp(ins, max, want < 1 ? 1 : static_cast<int32_t>(want), !reset);
            b->given = want < 1 ? 1 : static_cast<int32_t>(want);
            ++(on ? raised : lowered);
        }
    }
    int kept = 0;  // gone characters, and all of them once apart
    for (int i = 0; i < g_boosted_count; ++i)
        if (on && g_boosted[i].seen) g_boosted[kept++] = g_boosted[i];
    g_boosted_count = kept;
    qsort(g_boosted, static_cast<size_t>(kept), sizeof(Boosted), cmp_boosted);
    if (raised) say("party: %d enemies raised to %d%% health (%d here now)", raised, S.party_health, g_boosted_count);
    if (lowered) say("party: %d enemies back to their own health", lowered);
    if (left) say("party: %d enemies left at their own health (the game keeps setting it)", left);
}

// --- 5. seamless ---------------------------------------------------------------------------

bool g_seamless_on;
double g_last_check_skip_log;
double g_watchdog_at;

// OnMatchingCheck ends the session when the partner's character is not in
// the world - which is also the case for a while after every load. Skipped
// for 30 s after a load; the watchdog below handles a partner really gone.
int on_matching_check(BbHookCtx* ctx, void*) {
    const double t = now_s();
    if (!g_seamless_on || !(g_last.state == addrs::kStateHosting || g_last.state == addrs::kStateJoined)) return 0;
    if (g_last.in_world && t - C.world_since >= 30.0) return 0;
    if (t - g_last_check_skip_log > 5.0) {
        g_last_check_skip_log = t;
        say("seamless: OnMatchingCheck skipped %.0f s after a load (state %d); the session stays", t - C.world_since, g_last.state);
    }
    ctx->ret = 0;
    return 1;
}

void seamless_tick(const World& w, double t) {
    if (!g_seamless_on || !w.in_world) return;
    if (!(w.state == addrs::kStateHosting || w.state == addrs::kStateJoined)) return;
    // Only a session the two shared before this load (a host waiting for a
    // first summon has no one in it either).
    if (J.last_together_at <= 0 || J.last_together_at >= C.world_since || t - J.last_together_at > 180.0) return;
    if (t - C.world_since < 45.0 || t - g_watchdog_at < 60.0 || partner_chr()) return;
    g_watchdog_at = t;
    const int rc = api->lua_event("OnLeaveMenu_Yes");
    say("seamless: the partner is not back 45 s after the load; leaving (OnLeaveMenu_Yes %s) so re-join brings you together",
         rc == 0 ? "queued" : "refused");
}

// --- 4b. Insight ----------------------------------------------------------------------------
//
// The game gives a visitor no Insight (addrs::kInsightGoods says why). The host counts
// what its world gives it while together - every rise of its Insight but its own
// Insight items - and tells the guest (Beat.insight, this run's total); her game adds
// what it has not added yet. insight.txt keeps "<host run> <added>" over restarts.
struct InsightState {
    int64_t last = -1;   // host: its Insight at the last settled frame (-1: none yet)
    int32_t goods[4];    // host: how many Insight items it had, once a second, the latest last
    double goods_at;
    int rise;            // host: Insight that came, waiting to be told from an item's
    double rise_at;      // when it came
    bool rise_together;
    int32_t rise_goods;  // the most Insight items it had in the seconds before
    uint16_t total;      // host: from its world while together, this run
    uint32_t run;        // guest: the host's run she adds from
    uint16_t added;      // guest: how much of that run's total she has
};
InsightState I;

// A file of two numbers, "<a> <b>" (insight.txt, awards.txt).
bool load_pair(const char* name, unsigned* a, unsigned* b) {
    FILE* f = fopen(in_dir(name), "r");
    if (!f) return false;
    const bool ok = fscanf(f, "%u %u", a, b) == 2;
    fclose(f);
    return ok;
}

void save_pair(const char* name, unsigned a, unsigned b) {
    if (FILE* f = open_tmp(name)) {
        fprintf(f, "%u %u\n", a, b);
        close_tmp(f, name);
    }
}

int32_t insight_goods() {
    int32_t n = 0;
    for (int32_t id : addrs::kInsightGoods) n += goods_count(id);
    return n;
}

// host, while together (and until a rise from then is told): an item raises Insight as it
// is used up, so a rise is the world's unless the items are fewer 2.5 s on than at the most
// in the 3 s before it.
void insight_host_tick(const World& w, double t) {
    int64_t now = -1;
    if (!S.share_insight || !w.stable || (!I.rise && !together(w)) || api->player_stat_get("insight", &now) != 0) {
        I.last = -1;
        return;
    }
    if (I.last < 0) {  // back on: the items as they are now
        const int32_t n = insight_goods();
        for (int32_t& g : I.goods) g = n;
        I.goods_at = t;
    }
    if (t - I.goods_at >= 1.0) {
        I.goods_at = t;
        memmove(I.goods, I.goods + 1, sizeof(I.goods) - sizeof(I.goods[0]));
        I.goods[3] = insight_goods();
    }
    if (I.last >= 0 && now > I.last) {
        if (!I.rise) {
            I.rise_at = t;
            I.rise_together = together(w);
            I.rise_goods = 0;
            for (int32_t n : I.goods) I.rise_goods = n > I.rise_goods ? n : I.rise_goods;
        }
        I.rise += static_cast<int>(now - I.last);
    }
    I.last = now;
    if (!I.rise || t - I.rise_at < 2.5) return;
    const int rise = I.rise;
    I.rise = 0;
    if (insight_goods() < I.rise_goods) {
        say("insight: +%d from an Insight item; yours only", rise);
    } else if (!I.rise_together) {
        say("insight: +%d from the world while apart", rise);
    } else {
        I.total = static_cast<uint16_t>(I.total + rise);
        say("insight: +%d from the world while together; the guest gets it too (%u this run)", rise, I.total);
    }
}

// guest: what the host's world gave him together and she has not had yet, in any
// settled world (as a phantom her Insight is her own character's, as her echoes are).
void insight_guest_tick(double t) {
    if (!S.share_insight || !peer_fresh(t) || L.peer.guest || !ready_for_items()) return;
    const Beat& b = L.peer;
    if (b.run != I.run) {
        if (!b.insight) return;
        I.run = b.run;
        I.added = 0;
    }
    if (b.insight <= I.added) return;
    int64_t have = 0;
    if (api->player_stat_get("insight", &have) != 0) return;
    const int add = b.insight - I.added;
    const int64_t to = have + add < addrs::kInsightMax ? have + add : addrs::kInsightMax;
    if (api->player_stat_set("insight", to) != 0) return;
    I.added = b.insight;
    save_pair("insight.txt", I.run, I.added);
    say("insight: +%d from the host's world (%lld -> %lld)", add, static_cast<long long>(have), static_cast<long long>(to));
    note(9950038, "Co-op: you gained Insight with your partner");
}

// --- 4c. Gestures ----------------------------------------------------------------------------
//
// A gesture either one gets while together (an NPC's: Eileen's Shake off Cape, ...) the
// other gets too. The game gives it to whoever talked, and a visitor's talk goes by the
// host's world's story flags, so hers rarely does. Beat.gestures is what this side got
// together this run; the partner's game marks each it lacks as had, as talk command 66
// does (addrs::kGestures).
struct GestureState {
    uint32_t last;      // the gestures had at the last check
    bool known;         // last is this character's, settled
    uint32_t together;  // got while together, this run (Beat.gestures)
};
GestureState GS;

// The character's gestures (bit g: gesture g); those in `give` it lacks are marked had first.
uint32_t gestures(uint32_t give = 0) {
    const uint64_t list = field(field(singleton(addrs::kGameDataMan), 8), addrs::kGestures);
    uint32_t had = 0;
    for (int i = 0; list && i < addrs::kGestureCount; ++i) {
        const uint64_t at = list + addrs::kGestureSlots + 4 * static_cast<uint64_t>(i);
        uint32_t e = 0;
        if (!rd(at, &e, 4) || e > 0xFFFEFFFFu || (e & 0xFFFE) == 0xFFFE) continue;
        const int g = (static_cast<int16_t>(e & 0xFFFF) >> 1) - 1;
        if (g < 0 || g >= 32) continue;
        if (!(e & 1) && ((give >> g) & 1)) {
            *reinterpret_cast<volatile uint8_t*>(at) = static_cast<uint8_t>(e | 1);
            e |= 1;
        }
        if (e & 1) had |= 1u << g;
    }
    return had;
}

void gestures_tick(const World& w, double t) {
    if (!w.stable) GS.known = false;
    if (!w.stable || C.frame % 15 != 0) return;
    uint32_t had = gestures();
    const uint32_t got = GS.known ? had & ~GS.last : 0;
    if (got && together(w)) {
        GS.together |= got;
        say("gestures: got 0x%x while together; the partner gets them too", got);
    }
    GS.last = had;
    GS.known = true;
    const uint32_t want = peer_fresh(t) && L.peer.guest != S.guest && ready_for_items() ? L.peer.gestures & ~had : 0;
    if (!want) return;
    had = gestures(want);
    GS.last = had;
    if (!(had & want)) return;
    say("gestures: 0x%x from the partner (got together)", had & want);
    note(9950039, "Co-op: you got your partner's gesture too");
}

// --- 4d. Rewards -----------------------------------------------------------------------------
//
// Every item lot the game gives goes through kAwardItemLot (an event's Award Item Lot: a boss's
// drop, an NPC's gift once its talk set the event's flag; Lua's GetRateItem), hooked on both
// machines. One given while together goes to the partner in the heartbeat, whose game gives the
// same lot 5 s later - unless it gave that lot itself meanwhile (an award to all players runs on
// both). The guest gets a boss's drop as share_drop gives it (once; the repeat drop if her world
// had it), nothing she had in her own world, and the lot's flag taken in her world too. Not
// shared: the plugin's own awards, the role lots (addrs::kRoleLots), and an award the game
// refused (a joined player's, with its multiplayer check).
struct Awards {
    int32_t sent[12];  // given here while together this run, the latest at (total - 1) % 12
    uint16_t total;
    int32_t mine[16];  // every lot given here lately, and when (0: counted already)
    double mine_at[16];
    int mine_next;
    unsigned peer_run;  // the partner's run, how many of its awards are seen and handled (awards.txt
    unsigned seen, done;  // keeps "<run> <handled>" across this machine's restarts)
    double due;  // the partner's newest wait until then
} A;

int32_t g_awarded[16];  // the hook's (one writer, the game's thread; one reader, the frame)
uint32_t g_awarded_head, g_awarded_tail;

bool role_lot(int32_t lot) {
    for (int32_t l : addrs::kRoleLots)
        if (l == lot) return true;
    return false;
}

int on_award(BbHookCtx* ctx, void*) {
    const int32_t lot = static_cast<int32_t>(ctx->arg[1]);
    if (lot <= 0 || __atomic_load_n(&g_own_award, __ATOMIC_RELAXED) || role_lot(lot)) return 0;
    if ((ctx->arg[2] & 0xFF) && g_last.state == addrs::kStateJoined) return 0;  // the game gives it nothing
    const uint32_t head = g_awarded_head;
    if (head - __atomic_load_n(&g_awarded_tail, __ATOMIC_ACQUIRE) < 16) {
        g_awarded[head % 16] = lot;
        __atomic_store_n(&g_awarded_head, head + 1, __ATOMIC_RELEASE);
    }
    return 0;
}


// The frame: what the hook saw given.
void awards_tick(const World& w, double t) {
    uint32_t tail = g_awarded_tail;
    const uint32_t head = __atomic_load_n(&g_awarded_head, __ATOMIC_ACQUIRE);
    for (; tail != head; ++tail) {
        const int32_t lot = g_awarded[tail % 16];
        A.mine[A.mine_next] = lot;
        A.mine_at[A.mine_next] = t;
        A.mine_next = (A.mine_next + 1) % 16;
        const bool tog = together(w);
        if (tog && S.share_rewards) A.sent[A.total++ % 12] = lot;
        say("rewards: your game gave you item lot %d%s", lot,
            !tog ? "" : S.share_rewards ? "; your partner gets it too" : "; not shared (share_rewards off)");
    }
    __atomic_store_n(&g_awarded_tail, tail, __ATOMIC_RELEASE);
}

// The partner's award, here: true when done with it (given, or nothing to give).
bool give_award(int32_t lot, uint32_t run, double t) {
    for (int i = 0; i < 16; ++i)
        if (A.mine[i] == lot && A.mine_at[i] > 0 && t - A.mine_at[i] < 120.0) {
            A.mine_at[i] = 0;  // it counts once
            say("rewards: your partner got item lot %d, as you did", lot);
            return true;
        }
    if (S.guest)
        for (const addrs::BossDrop& d : addrs::kBossDrops)
            if (lot == d.lot || lot == d.repeat_lot) {
                share_drop(d.boss, run, "the host's award");
                return g_given.has(run, d.boss) || !S.share_drops;
            }
    const int32_t flag = lot_flag(static_cast<uint32_t>(lot));
    if (S.guest && taken_by_her(flag)) {
        say("rewards: the host got item lot %d; you had it in your world (flag %d)", lot, flag);
        return true;
    }
    // In his world her flags are his: his award marked the lot taken there, and the
    // game gives nothing for a lot it marks taken.
    if (S.guest && flag > 0 && in_host_world(g_last) && flag_is_set(static_cast<uint32_t>(flag)))
        api->event_flag_set(static_cast<uint32_t>(flag), 0);
    if (!award_lot(lot)) return false;
    if (S.guest && flag > 0) remember_flag(static_cast<uint32_t>(flag));  // taken in her own world too
    say("rewards: the %s got item lot %d; you too (lot flag %d)", S.guest ? "host" : "guest", lot, flag);
    note(9950040, "Co-op: you got your partner's reward too");
    return true;
}

void awards_from_peer(const Beat& b) {
    if (!S.share_rewards || !g_functions_ok || static_cast<bool>(b.guest) == S.guest) return;
    const double t = now_s();
    if (b.run != A.peer_run) {
        A.peer_run = b.run;
        A.seen = A.done = 0;
    }
    if (b.award_total > A.seen) {
        A.seen = b.award_total;
        A.due = t + 5.0;
    }
    if (A.done >= b.award_total || t < A.due || !ready_for_items()) return;
    const int n = b.award_count < 12 ? b.award_count : 12;
    const unsigned first = b.award_total - static_cast<unsigned>(n), was = A.done;
    if (A.done < first) {
        say("rewards: %u of your partner's awards came too late to give", first - A.done);
        A.done = first;
    }
    while (A.done < b.award_total && give_award(b.awards[A.done - first], b.run, t)) ++A.done;
    if (A.done != was) save_pair("awards.txt", A.peer_run, A.done);
}

// --- 4e. World progress ---------------------------------------------------------------------
//
// Her world as his: every lasting change the host's world makes while the two are together (a door
// or shortcut opened, an NPC's quest moved on, a boss down, the time of day) goes into hers. The
// game writes its flags from dozens of places - its two setters, which on_flag hears, and many
// inlined copies (talk, map events, the stage update, the boss clear) - so the host compares the
// store itself: a snapshot as the two come together, a compare every 5 s while together and one
// 5 s after they part, each change logged in order. The log goes to her (kWorldMagic) until her
// heartbeat says she has it; what she has not had yet outlives the host's restarts
// (world-out.txt). She keeps each flag's latest value (world.txt) and sets them in her own world's
// store as she is next home: as the load places her (before her map's events read them, where
// they do), and again once it settles, when they are done with. Not carried: the item flags (each
// picks up their own; rewards come as rewards), the chalice dungeons (area 29, and the flags that
// pick one) and the boss clear's co-op flags (addrs.hpp).
constexpr int kPoolSlots = 1194;       // up to the last carried one (category 9, area 36, group 9)
constexpr uint32_t kPoolStride = 128;  // a block's bytes apart, at most
constexpr uint32_t kNotCarried = 0xFFFFFFFFu;
uint32_t g_slot_first[kPoolSlots];  // the slot's first flag; kNotCarried
int8_t g_general_k[10], g_map_k[10], g_area_a[100][10];  // a category's k and an area's a in the tables; -1: none
unsigned char g_pool_base[kPoolSlots * kPoolStride], g_pool_now[kPoolSlots * kPoolStride];
int g_pool_checked;  // 1: the store is laid out as addrs.hpp has it; -1: not (nothing carried)

void build_slots() {
    for (uint32_t& f : g_slot_first) f = kNotCarried;
    memset(g_general_k, -1, sizeof(g_general_k));
    memset(g_map_k, -1, sizeof(g_map_k));
    memset(g_area_a, -1, sizeof(g_area_a));
    for (int8_t k = 0; k < 5; ++k) {
        g_general_k[addrs::kFlagGeneral[k]] = k;
        g_map_k[addrs::kFlagMapKinds[k]] = k;
    }
    for (int8_t a = 0; a < 17; ++a) g_area_a[addrs::kFlagAreas[a][0]][addrs::kFlagAreas[a][1]] = a;
    for (uint32_t g = 0; g < 10; ++g)
        for (uint32_t k = 0; k < 5; ++k) {
            const uint32_t c = addrs::kFlagGeneral[k];
            if (c != addrs::kItemFlags && c * 10000000 + g * 1000 != addrs::kCoopFlags)
                g_slot_first[k + 5 * g] = c * 10000000 + g * 1000;
            for (uint32_t a = 0; a < 17; ++a) {
                const uint32_t m = addrs::kFlagMapKinds[k], area = addrs::kFlagAreas[a][0];
                const uint32_t slot = 50 + 115 * g + 23 * k + a;
                if (m != addrs::kItemFlags && area != addrs::kAreaChalice && slot < kPoolSlots)
                    g_slot_first[slot] = m * 10000000 + area * 100000 + addrs::kFlagAreas[a][1] * 10000 + g * 1000;
            }
        }
}

bool chalice_flag(uint32_t id) { return id >= addrs::kChaliceFirst && id <= addrs::kChaliceLast; }

// A carried flag's slot (as the game's 0x13BC710 finds it), or -1.
int flag_slot(uint32_t id) {
    const uint32_t c = id / 10000000, area = id / 100000 % 100, part = id / 10000 % 10;
    const int g = static_cast<int>(id / 1000 % 10);
    if (c >= 10 || chalice_flag(id)) return -1;
    const int slot = !area && !part ? (g_general_k[c] < 0 ? -1 : g_general_k[c] + 5 * g)
                     : g_map_k[c] < 0 || g_area_a[area][part] < 0 ? -1
                                                                    : 50 + 115 * g + 23 * g_map_k[c] + g_area_a[area][part];
    return slot >= 0 && slot < kPoolSlots && g_slot_first[slot] == id / 1000 * 1000 ? slot : -1;
}

// A block's node in the store's map, found as the game finds it.
bool block_node(uint64_t man, uint32_t block, int32_t* kind, uint32_t* slot) {
    const uint64_t head = rp(man + 0x38);
    uint64_t best = head, n = head ? rp(head + 8) : 0;
    unsigned char node[0x38];
    for (int depth = 0; n && depth < 64 && rd(n, node, sizeof(node)) && !node[0x19]; ++depth) {
        uint32_t key = 0;
        memcpy(&key, node + 0x20, 4);
        if (key < block) {
            memcpy(&n, node + 0x10, 8);
        } else {
            best = n;
            memcpy(&n, node, 8);
        }
    }
    if (!head || best == head || !rd(best, node, sizeof(node))) return false;
    uint32_t key = 0;
    memcpy(&key, node + 0x20, 4);
    if (key != block) return false;
    memcpy(kind, node + 0x28, 4);
    memcpy(slot, node + 0x30, 4);
    return true;
}

struct Pool {
    uint64_t base;
    uint32_t stride;
};

// The store's pool, while its blocks are the pool's; the first time, three blocks as the
// game's map has them (general 9xxx, Cathedral Ward's events, Central Yharnam's talk).
bool flag_pool(Pool* p) {
    const uint64_t man = singleton(addrs::kEventFlagMan);
    uint32_t per = 0;
    int32_t mode = -1;
    if (g_pool_checked < 0 || !man || !rd(man + 0x1C, &per, 4) || !rd(man + 0x20, &p->stride, 4) ||
        !rd(man + addrs::kFlagMode, &mode, 4) || mode != 0)
        return false;
    p->base = rp(man + 0x28);
    bool ok = per == addrs::kFlagsPerBlock && p->stride >= addrs::kFlagsPerBlock / 8 && p->stride <= kPoolStride && p->base;
    if (g_pool_checked) return ok;
    constexpr uint32_t kBlocks[] = {9, 12411, 72400};
    for (uint32_t block : kBlocks) {
        int32_t kind = 0;
        uint32_t slot = 0;
        if (!block_node(man, block, &kind, &slot)) return false;  // not built yet: again later
        ok = ok && kind == 1 && static_cast<int>(slot) == flag_slot(block * 1000);
    }
    g_pool_checked = ok ? 1 : -1;
    say(ok ? "world: the flag store is as expected (%u flags a block, %u bytes apart)"
           : "world: the flag store is not as expected (%u flags a block, %u bytes apart, or its blocks elsewhere); "
             "world progress is not carried",
        per, p->stride);
    return ok;
}

bool read_pool(const Pool& p, unsigned char* out) { return api->read(p.base, out, kPoolSlots * p.stride) == 0; }

// host: this run's changes from the first the guest lacks (number `first`) on: a flag, | kOn when it
// came on; kClear | a slot: the whole block off, before the flags after it (the catch-up); and
// kCatchupEnd after the catch-up's last (she sets none of it before).
constexpr uint32_t kOn = 0x80000000u, kClear = 0x40000000u, kCatchupEnd = kClear | 0xFFFF;
constexpr uint32_t kWorldLog = 65536;
struct WorldOut {
    uint32_t log[kWorldLog];
    uint32_t first, total;
    bool have_base, was_together, full_said;
    bool catchup;              // catchup.txt: the whole world goes once, as the two next come together
    double next_at, apart_at;  // the next compare; the last one after parting (0: none due)
    double saved_at;           // world-out.txt last written anew
} WO;

bool log_entry_ok(uint32_t e) {
    if (e == kCatchupEnd) return true;
    return e & kClear ? (e & 0xFFFF) < kPoolSlots && g_slot_first[e & 0xFFFF] != kNotCarried : flag_slot(e & ~kOn) >= 0;
}

// world-out.txt: what she lacks, a change a line. New ones go on its end; it is written anew as
// she takes them (when she has them all, else at most every 30 s), so after a restart of the
// host's game she may get a few again, in their order.
void save_world_out() {
    FILE* f = open_tmp("world-out.txt");
    if (!f) return;
    for (uint32_t i = 0; i < WO.total - WO.first; ++i) fprintf(f, "%u\n", WO.log[i]);
    close_tmp(f, "world-out.txt");
    WO.saved_at = now_s();
}

void append_world_out(uint32_t from) {  // the changes numbered from `from` on
    FILE* f = g_dir[0] ? fopen(in_dir("world-out.txt"), "a") : nullptr;
    if (!f) return;
    for (uint32_t i = from; i < WO.total; ++i) fprintf(f, "%u\n", WO.log[i - WO.first]);
    fclose(f);
}

void load_world_out() {
    WO.catchup = access(in_dir("catchup.txt"), F_OK) == 0;
    if (WO.catchup) say("world: catchup.txt: your whole world goes to the guest as you next come together");
    FILE* f = fopen(in_dir("world-out.txt"), "r");
    if (!f) return;
    char line[32];
    unsigned e = 0;
    while (WO.total < kWorldLog && fgets(line, sizeof(line), f))  // a line cut off by a crash has no end
        if (strchr(line, '\n') && sscanf(line, "%u", &e) == 1 && log_entry_ok(e)) WO.log[WO.total++] = e;
    fclose(f);
    if (WO.total) say("world: %u change(s) from last time still go to the guest", WO.total);
}

bool log_room(uint32_t n) { return WO.total - WO.first + n <= kWorldLog; }

// The catch-up: every carried block of the snapshot, as a clear and the flags on in it, then the
// end; all of it, or none when it does not fit beside what she has not taken yet.
void world_catchup(uint32_t stride) {
    const uint32_t start = WO.total;
    uint32_t on = 0, blocks = 0;
    bool full = false;
    const auto add = [&](uint32_t e) {
        if (log_room(1)) WO.log[WO.total++ - WO.first] = e;
        else full = true;
    };
    for (int s = 0; s < kPoolSlots && !full; ++s) {
        if (g_slot_first[s] == kNotCarried) continue;
        ++blocks;
        add(kClear | static_cast<uint32_t>(s));
        const unsigned char* block = g_pool_base + s * stride;
        for (uint32_t i = 0; i < addrs::kFlagsPerBlock; ++i) {
            const uint32_t id = g_slot_first[s] + i;
            if ((block[i / 8] >> (7 - i % 8)) & 1 && !chalice_flag(id)) {
                add(id | kOn);
                ++on;
            }
        }
    }
    add(kCatchupEnd);
    if (full) {
        WO.total = start;
        say("world: the catch-up does not fit beside what the guest has not taken yet; next time");
        return;
    }
    append_world_out(start);
    remove(in_dir("catchup.txt"));
    WO.catchup = false;
    say("world: the catch-up: your whole world (%u blocks, %u flags on) goes to the guest; catchup.txt done", blocks, on);
}

void world_compare(const char* when) {
    Pool p;
    if (!flag_pool(&p) || !read_pool(p, g_pool_now)) return;
    const size_t bytes = kPoolSlots * p.stride;
    if (!WO.have_base) {
        memcpy(g_pool_base, g_pool_now, bytes);
        WO.have_base = true;
        if (WO.catchup) world_catchup(p.stride);
        return;
    }
    const uint32_t start = WO.total;
    char list[256] = "";
    size_t used = 0;
    int n = 0, lost = 0;
    for (int s = 0; s < kPoolSlots; ++s) {
        const unsigned char *was = g_pool_base + s * p.stride, *now = g_pool_now + s * p.stride;
        if (g_slot_first[s] == kNotCarried || memcmp(was, now, addrs::kFlagsPerBlock / 8) == 0) continue;
        for (uint32_t i = 0; i < addrs::kFlagsPerBlock / 8; ++i)
            for (uint32_t bit = 0; was[i] != now[i] && bit < 8; ++bit) {
                const uint32_t id = g_slot_first[s] + i * 8 + bit;
                const bool on = now[i] & (0x80 >> bit);
                if (((was[i] ^ now[i]) & (0x80 >> bit)) == 0 || chalice_flag(id)) continue;
                if (!log_room(1)) {
                    ++lost;
                    continue;
                }
                WO.log[WO.total++ - WO.first] = id | (on ? kOn : 0);
                if (n++ < 12 && used < sizeof(list))
                    used += snprintf(list + used, sizeof(list) - used, " %u %s", id, on ? "on" : "off");
            }
    }
    memcpy(g_pool_base, g_pool_now, bytes);
    if (n) {
        say("world: %d change(s) in your world %s:%s%s; they go to the guest", n, when, list, n > 12 ? " ..." : "");
        append_world_out(start);
    }
    if (lost && !WO.full_said) say("world: %d change(s) lost: the guest has not taken the last %u", lost, kWorldLog);
    WO.full_said = lost > 0;
}

void world_out_tick(const World& w, double t) {
    if (S.guest) return;
    const bool tog = S.share_world && together(w);
    if (tog && !WO.was_together) {
        if (WO.apart_at) WO.apart_at = 0;  // back within 5 s: the same snapshot goes on
        else WO.have_base = false;
        WO.next_at = t;
    } else if (!tog && WO.was_together) {
        WO.apart_at = t + 5.0;
    }
    WO.was_together = tog;
    if (tog && t >= WO.next_at) {
        WO.next_at = t + 5.0;
        world_compare("while together");
    } else if (!tog && WO.apart_at && t >= WO.apart_at) {
        WO.apart_at = 0;
        world_compare("as you parted");
        WO.have_base = false;
    }
}

void world_send(double t) {
    if (S.guest || WO.total == WO.first || !peer_fresh(t) || !L.peer.guest) return;
    WorldMsg m = {};
    memcpy(m.magic, kWorldMagic, 4);
    m.run = L.run;
    m.first = WO.first;
    m.count = WO.total - WO.first < 200 ? WO.total - WO.first : 200;
    memcpy(m.changes, WO.log, m.count * sizeof(m.changes[0]));
    sendto(L.fd, &m, sizeof(m), 0, reinterpret_cast<const sockaddr*>(&L.partner), sizeof(L.partner));
}

// host: her heartbeat says how many of this run's she has.
void world_acked(const Beat& b) {
    if (S.guest || !b.guest || b.world_run != L.run || b.world_seen <= WO.first || b.world_seen > WO.total) return;
    memmove(WO.log, WO.log + (b.world_seen - WO.first), (WO.total - b.world_seen) * sizeof(WO.log[0]));
    WO.first = b.world_seen;
    if (WO.first == WO.total || now_s() - WO.saved_at > 30.0) save_world_out();
    if (WO.first == WO.total) say("world: the guest has every change so far (%u this run)", WO.total);
}

// guest: each flag's latest value and the blocks to clear first, until her world has them. A
// change counts over a clear of its block that came before it (`order`, hers, ever rising).
// world.txt: "run <host run> <seen> <next order> <open>", "clear <slot> <order>" and "<flag> <on>
// <order>" lines, a later one winning; new ones go on its end, and it starts anew once her world
// has them.
constexpr uint32_t kWorldHeld = 1u << 16;
struct WorldIn {
    uint32_t run, seen;            // the host's run, and how many of its changes she has
    uint32_t key[kWorldHeld];      // a flag + 1; 0: empty
    bool on[kWorldHeld];
    uint32_t order[kWorldHeld];
    uint32_t cleared[kPoolSlots];  // the block's clear's order; 0: none
    uint32_t count, clears, next_order;
    bool open;                     // a catch-up has begun and its end has not come
    double heard_at;               // the host's last changes (or this start)
    double early_for;              // the world the load's pass ran in (C.world_since), what it changed
    int early_bits;
    uint32_t early_order;          // and the last order it had
    double settled_for;            // the world last looked at once settled
} WI;
unsigned char g_pool_want[kPoolSlots * kPoolStride];

// The order it has now (order 0: the next one); 0 when there is no room.
uint32_t hold_change(uint32_t id, bool on, uint32_t order = 0) {
    for (uint32_t h = (id * 2654435761u) & (kWorldHeld - 1);; h = (h + 1) & (kWorldHeld - 1)) {
        if (WI.key[h] == id + 1 || (!WI.key[h] && WI.count < kWorldHeld / 2)) {
            WI.count += !WI.key[h];
            WI.key[h] = id + 1;
            WI.on[h] = on;
            WI.order[h] = order ? order : ++WI.next_order;
            if (WI.order[h] > WI.next_order) WI.next_order = WI.order[h];
            return WI.order[h];
        }
        if (!WI.key[h]) return 0;  // full
    }
}

uint32_t hold_clear(uint32_t slot, uint32_t order = 0) {
    WI.clears += !WI.cleared[slot];
    WI.cleared[slot] = order ? order : ++WI.next_order;
    if (WI.cleared[slot] > WI.next_order) WI.next_order = WI.cleared[slot];
    return WI.cleared[slot];
}

void put_world_run(FILE* f) { fprintf(f, "run %u %u %u %d\n", WI.run, WI.seen, WI.next_order, WI.open); }

void load_world_in() {
    WI.heard_at = now_s();
    FILE* f = fopen(in_dir("world.txt"), "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        unsigned a = 0, b = 0, c = 0;
        int open = 0;
        if (!strchr(line, '\n')) continue;  // cut off by a crash
        if (sscanf(line, "run %u %u %u %d", &a, &b, &c, &open) >= 3) {
            WI.run = a;
            WI.seen = b;
            if (c > WI.next_order) WI.next_order = c;
            WI.open = open != 0;
        } else if (sscanf(line, "clear %u %u", &a, &b) == 2 && a < kPoolSlots && g_slot_first[a] != kNotCarried && b) {
            hold_clear(a, b);
        } else if (sscanf(line, "%u %u %u", &a, &b, &c) == 3 && flag_slot(a) >= 0 && c) {
            hold_change(a, b != 0, c);
        }
    }
    fclose(f);
    if (WI.count || WI.clears)
        say("world: %u flag(s) and %u block(s) of the host's world still wait for yours%s", WI.count, WI.clears,
            WI.open ? " (a catch-up, not all here yet)" : "");
}

void world_from_host(const WorldMsg& m) {
    if (!S.guest) return;
    if (m.run != WI.run) {
        WI.run = m.run;
        WI.seen = 0;
    }
    if (m.first > WI.seen || m.count > 200 || m.first + m.count <= WI.seen) return;  // the host sends from what she has
    WI.heard_at = now_s();
    FILE* f = S.share_world && g_dir[0] ? fopen(in_dir("world.txt"), "a") : nullptr;
    int held = 0, blocks = 0;
    bool ended = false;
    for (uint32_t i = WI.seen - m.first; i < m.count; ++i, ++WI.seen) {
        const uint32_t e = m.changes[i];
        if (!S.share_world || !log_entry_ok(e)) continue;
        if (e == kCatchupEnd) {
            WI.open = false;
            ended = true;
        } else if (e & kClear) {
            WI.open = true;
            const uint32_t order = hold_clear(e & 0xFFFF);
            if (f) fprintf(f, "clear %u %u\n", e & 0xFFFF, order);
            ++blocks;
        } else if (const uint32_t order = hold_change(e & ~kOn, e & kOn)) {
            if (f) fprintf(f, "%u %d %u\n", e & ~kOn, (e & kOn) != 0, order);
            ++held;
        }
    }
    if (f) {
        put_world_run(f);
        fclose(f);
    }
    if (held || blocks || ended)
        say("world: %d change(s)%s from the host's world; %u flag(s) and %u block(s) wait for yours", held,
            ended ? " and the catch-up's end" : blocks ? " and a catch-up" : "", WI.count, WI.clears);
}

// Something waits and may be set: not while a catch-up is still coming, unless the host has had
// nothing more to send for a minute (its end will not come).
bool world_ready(double t) {
    if (WI.open && peer_fresh(t) && t - WI.heard_at > 60.0) {
        WI.open = false;
        say("world: the catch-up's end did not come; what came of it goes in");
    }
    return (WI.count || WI.clears) && !WI.open;
}

// Her store as the held changes of orders after `after`, up to `upto`, make it (g_pool_want),
// from hers now (g_pool_now): the flags that differ. A cleared block keeps what is not carried
// (the chalice flags).
int world_target(const Pool& p, uint32_t after, uint32_t upto = 0xFFFFFFFFu) {
    memcpy(g_pool_want, g_pool_now, kPoolSlots * p.stride);
    for (int s = 0; s < kPoolSlots; ++s) {
        if (WI.cleared[s] <= after || WI.cleared[s] > upto) continue;
        memset(g_pool_want + s * p.stride, 0, addrs::kFlagsPerBlock / 8);
        for (uint32_t id = addrs::kChaliceFirst; id <= addrs::kChaliceLast; ++id) {
            const uint32_t i = id - g_slot_first[s];
            if (i < addrs::kFlagsPerBlock) g_pool_want[s * p.stride + i / 8] |= g_pool_now[s * p.stride + i / 8] & (0x80 >> (i % 8));
        }
    }
    for (uint32_t h = 0; h < kWorldHeld; ++h) {
        if (!WI.key[h] || WI.order[h] <= after || WI.order[h] > upto) continue;
        const int slot = flag_slot(WI.key[h] - 1);
        if (slot < 0 || (WI.cleared[slot] <= upto && WI.order[h] < WI.cleared[slot])) continue;
        const uint32_t bit = (WI.key[h] - 1) % addrs::kFlagsPerBlock;
        uint8_t& byte = g_pool_want[slot * p.stride + bit / 8];
        const uint8_t mask = static_cast<uint8_t>(0x80 >> (bit & 7));
        byte = WI.on[h] ? static_cast<uint8_t>(byte | mask) : static_cast<uint8_t>(byte & ~mask);
    }
    int bits = 0;
    for (size_t i = 0; i < kPoolSlots * p.stride; ++i) bits += __builtin_popcount(g_pool_want[i] ^ g_pool_now[i]);
    return bits;
}

void world_write(const Pool& p) {
    for (size_t i = 0; i < kPoolSlots * p.stride; ++i) {
        const uint8_t diff = g_pool_want[i] ^ g_pool_now[i];
        if (!diff) continue;
        volatile uint8_t* byte = reinterpret_cast<volatile uint8_t*>(p.base + i);
        *byte = static_cast<uint8_t>((*byte & ~diff) | (g_pool_want[i] & diff));
        g_pool_now[i] = g_pool_want[i];
    }
}

bool at_home_now() {
    const uint64_t player = main_player(), session = singleton(addrs::kSessionManager);
    int32_t type = -1, state = -1;
    return player && session && rd(player + 0x78, &type, 4) && type == 0 && rd(session + 0x124, &state, 4) &&
           state == addrs::kStateIdle && !rp(session + 0x18);
}

// guest, home: as the load places her (before her map's events read the flags, where they do).
void world_load_pass() {
    Pool p;
    if (!S.guest || !S.share_world || !world_ready(now_s()) || !at_home_now() || !flag_pool(&p) || !read_pool(p, g_pool_now))
        return;
    WI.early_bits = world_target(p, 0);
    world_write(p);
    WI.early_for = C.world_since;
    WI.early_order = WI.next_order;
    say("world: as the load placed you: %d flag(s) of your world set as the host's", WI.early_bits);
}

// ... and once it settles: done with them. What came after the load's pass goes in now; the rest
// again only if the load put her old flags back over it (most of them differ again), otherwise
// what her world changed since stays.
void world_in_tick(const World& w, double t) {
    if (!S.guest || !S.share_world || w.chr_type != 0 || !idle(w) || !w.stable || WI.settled_for == C.world_since) return;
    WI.settled_for = C.world_since;  // what comes later waits for the next load
    Pool p;
    if (!world_ready(t) || !flag_pool(&p) || !read_pool(p, g_pool_now)) return;
    const bool early = WI.early_for == C.world_since;
    const int left = early ? world_target(p, 0, WI.early_order) : 0;  // of what the load's pass set
    const bool again = !early || (WI.early_bits > 0 && left * 2 > WI.early_bits);
    const int bits = world_target(p, again ? 0 : WI.early_order);
    world_write(p);
    say("world: settled: %d flag(s) of your world set as the host's now (they show from your next load)%s", bits,
        again ? "" : "; what your world changed since the load stays");
    memset(WI.key, 0, sizeof(WI.key));
    memset(WI.cleared, 0, sizeof(WI.cleared));
    WI.count = WI.clears = 0;
    if (FILE* f = open_tmp("world.txt")) {
        put_world_run(f);
        close_tmp(f, "world.txt");
    }
    if ((early ? WI.early_bits : 0) + bits) note(9950041, "Co-op: your world caught up with your partner's");
}

// --- a one-off gift --------------------------------------------------------------------------
//
// <plugin dir>/gift.txt: lines "insight <n>" (that much more Insight, or less), "gesture <g>" (gesture g)
// and, for the guest, "boss <defeat flag>" (that boss as if killed together: its drop, and its death
// in her world), given to the character in the next settled world (the game saves them), then the
// file goes.
long g_gift_insight;
uint32_t g_gift_gestures;
uint32_t g_gift_bosses[8];
int g_gift_boss_count;

void load_gift() {
    char line[64];
    FILE* f = fopen(in_dir("gift.txt"), "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "insight ", 8) == 0) g_gift_insight += parse_long(line + 8, 0, -addrs::kInsightMax, addrs::kInsightMax);
        const long g = strncmp(line, "gesture ", 8) == 0 ? parse_long(line + 8, -1, 0, 31) : -1;
        if (g >= 0) g_gift_gestures |= 1u << g;
        const long b = strncmp(line, "boss ", 5) == 0 ? parse_long(line + 5, -1, 1, 0xFFFFFFFFL) : -1;
        if (b > 0 && is_boss(static_cast<uint32_t>(b)) && S.guest && g_gift_boss_count < 8)
            g_gift_bosses[g_gift_boss_count++] = static_cast<uint32_t>(b);
    }
    fclose(f);
    say("gift: %ld Insight, gestures 0x%x and %d boss(es) from gift.txt, in the next settled world", g_gift_insight,
        g_gift_gestures, g_gift_boss_count);
}

void gift_tick() {
    int64_t have = 0;
    if ((!g_gift_insight && !g_gift_gestures && !g_gift_boss_count) || !ready_for_items() ||
        api->player_stat_get("insight", &have) != 0)
        return;
    for (int i = 0; i < g_gift_boss_count; ++i) {  // first, as an award can fail (tried again then)
        const uint32_t id = g_gift_bosses[i];
        if (S.boss_progress) remember_for_guest(id, "a gift");
        share_drop(id, 0, "a gift");  // run 0: no host's
        if (S.share_drops && g_functions_ok && boss_drop(id) && !g_given.has(0, id)) return;
    }
    g_gift_boss_count = 0;
    int64_t to = have + g_gift_insight < addrs::kInsightMax ? have + g_gift_insight : addrs::kInsightMax;
    if (to < 0) to = 0;
    if (g_gift_insight && api->player_stat_set("insight", to) != 0) return;
    const uint32_t had = g_gift_gestures ? gestures(g_gift_gestures) : 0;
    GS.last |= had;
    remove(in_dir("gift.txt"));
    say("gift: Insight %lld -> %lld; gestures 0x%x asked, 0x%x had now", static_cast<long long>(have), static_cast<long long>(to),
        g_gift_gestures, had & g_gift_gestures);
    g_gift_insight = 0;
    g_gift_gestures = 0;
}

// --- the heartbeat ---------------------------------------------------------------------------

void link_send(const World& w, double t) {
    if (L.fd < 0 || !L.partner_ok || t - L.last_send < 0.25) return;
    L.last_send = t;
    Beat b = {};
    memcpy(b.magic, kMagic, 4);
    b.guest = S.guest;
    b.in_world = w.in_world;
    b.stable = w.stable;
    b.in_dream = w.in_dream;
    b.together = together(w);
    b.bell = api->sp_effect_has(nullptr, S.guest ? addrs::kSmallResonantEffect : addrs::kBeckoningEffect) == 1;
    b.wants_join = S.guest && S.auto_ring && w.in_world && w.chr_type == 0 && idle(w) && !w.in_dream && !paused(t);
    b.chr_type = w.chr_type;
    b.map = w.map;
    b.block = w.block;
    memcpy(b.pos, w.pos, sizeof(b.pos));
    b.yaw = w.yaw;
    b.pause_ms = t < J.pause_until ? static_cast<uint32_t>((J.pause_until - t) * 1000.0) : 0;
    const int n = g_credits.count < 24 ? g_credits.count : 24;
    b.credit_count = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) b.credits[i] = g_credits.ids[g_credits.count - n + i];
    b.downed = static_cast<uint8_t>(D.down ? 1 : D.released ? 2 : 0);
    b.revive_how = R.pending ? R.how : 0;
    b.revive_ask = R.pending ? R.ask : 0;
    b.revived_for = R.answered;
    b.run = L.run;
    b.rules = my_rules();
    b.party_health = static_cast<uint16_t>(S.party_health);
    b.insight = S.guest ? 0 : I.total;
    b.gestures = GS.together;
    b.award_total = A.total;
    const int an = A.total < 12 ? A.total : 12;
    b.award_count = static_cast<uint8_t>(an);
    for (int i = 0; i < an; ++i) b.awards[i] = A.sent[(A.total - an + i) % 12];
    const int pn = g_picked_total < 16 ? g_picked_total : 16;
    b.pickup_count = static_cast<uint8_t>(pn);
    for (int i = 0; i < pn; ++i) b.pickups[i] = g_picked[(g_picked_total - pn + i) % kPickedRing];
    const int tn = g_taken_total < 16 ? g_taken_total : 16;
    b.taken_count = static_cast<uint8_t>(tn);
    for (int i = 0; i < tn; ++i) b.taken[i] = g_taken[(g_taken_total - tn + i) % kPickedRing];
    b.world_run = S.guest ? WI.run : 0;
    b.world_seen = S.guest ? WI.seen : 0;
    sendto(L.fd, &b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&L.partner), sizeof(L.partner));
    world_send(t);
}

// --- the co-op pages of the game's System menu ------------------------------------------
//
// After bbhost's PC sections (src/engine/option_menu.cpp, GPL-3.0; addrs.hpp has the
// how). System's command list gets a row per co-op page, and Leave Session and Give
// Up while they apply, above Exit Game. A page is one of the game's option screens -
// a sprite the co-op mod's optionsetting.gfx adds - built from the game's own On/Off
// and pick-list rows over bytes of ours; each frame menu_tick takes their changes
// into S and into <plugin dir>/menu.txt. The game calls the functions here straight
// from its menu code.

char g_open_name[32] = "CoopJoin";  // the name table's slot 6 points here
bool g_menu_on, g_menu_said;
uint32_t g_menu_tried_frame;
bool g_menu_leave, g_menu_give_up;  // YES in a question: done on the next frame

// The game's std::function (MSVC's): the callable inline - its vtable, then the
// function - and a pointer to it at +0x20. Built in place and copied by whoever
// keeps it.
struct GuestFunction {
    alignas(16) uint8_t buf[0x28] = {};
    GuestFunction(uint64_t vtable_va, uint64_t fn) {
        const uint64_t vt = api->guest_addr(bn(vtable_va)), self = addr(buf);
        memcpy(buf, &vt, 8);
        memcpy(buf + 0x08, &fn, 8);
        memcpy(buf + 0x20, &self, 8);
    }
};

// A row's text and line help, 0x40 apart, as every row builder reads them.
struct Captions {
    alignas(16) uint8_t buf[0x80] = {};
    explicit Captions(uint32_t id) {
        call6(addrs::kMsgText, addr(buf), addrs::kGroupMenuText, id);
        call6(addrs::kMsgText, addr(buf + 0x40), addrs::kGroupLineHelp, id);
    }
    ~Captions() {
        free_wstring(buf + 0x08);
        free_wstring(buf + 0x48);
    }
};

// A list entry (0x48 bytes): the value, the text drawn, a wstring of it, a flag.
void make_entry(uint8_t (&entry)[0x48], int32_t value, const char16_t* text) {
    if (!text) text = u"";
    memset(entry, 0, sizeof(entry));
    memcpy(entry, &value, 4);
    memcpy(entry + 8, &text, 8);
    call6(addrs::kWstrCpy, addr(entry + 0x10), addr(text));
    entry[0x40] = 1;
}

void free_entries(uint8_t* first, uint64_t count) {
    for (uint64_t k = 0; k < count; ++k) free_wstring(first + k * 0x48 + 0x18);
}

void add_toggle_row(uint64_t dialog, MenuToggle& t) {
    t.value = t.last = *t.setting ? 1 : 0;
    Captions c(t.id);
    alignas(16) uint8_t list[0x100] = {};
    call6(addrs::kBuildOnOff, addr(list));
    uint8_t def = t.def ? 1 : 0;
    call6(addrs::kAddChoiceRow, dialog, addr(c.buf), addr(&t.value), addr(list), addr(&def));
    uint64_t n = 0;
    memcpy(&n, list + 0x98, 8);
    free_entries(list, n < 8 ? n : 0);
}

// Enemy health: the choice nearest the setting (bbport.ini takes any number).
void add_health_row(uint64_t dialog) {
    constexpr uint32_t kCount = sizeof(kHealth) / sizeof(kHealth[0]);
    g_health = kHealth[0];
    for (const int32_t h : kHealth)
        if (abs(h - S.party_health) < abs(g_health - S.party_health)) g_health = h;
    g_health_last = g_health;
    Captions c(kHealthRow);
    alignas(16) uint8_t list[0x1000] = {};
    for (uint32_t i = 0; i < kCount; ++i) {
        alignas(16) uint8_t entry[0x48];
        make_entry(entry, kHealth[i], message_text(addrs::kGroupMenuText, kHealthText + i));
        call6(addrs::kListAppend, addr(list), addr(entry));
        free_entries(entry, 1);
    }
    int32_t def = 120;
    call6(addrs::kAddListRow, dialog, addr(c.buf), addr(&g_health), addr(list), addr(&def));
    free_entries(list, kCount);
}

// The pages' sprites. Page P's rows are 125000 + 1000 P on, and its System row
// 110006 + P. Deaths (126) ends with the pick list, which opens over whatever is
// below it, so its movie has no slot for the engine's Defaults row.
constexpr const char* kPages[] = {"CoopJoin", "CoopDeaths", "CoopTogether", "CoopItems"};
constexpr uint32_t kFirstPageBlock = 125, kFirstPageRow = 110006;

// A page's rows, built when the game opens its dialog.
template <int P>
void page_handler(uint64_t dialog, uint64_t) {
    alignas(16) uint8_t scratch[0x100] = {};
    call6(addrs::kScratchInit, addr(scratch));
    for (MenuToggle& t : g_toggles)
        if (t.id / 1000 == kFirstPageBlock + P) add_toggle_row(dialog, t);
    if (kFirstPageBlock + P == kHealthRow / 1000) add_health_row(dialog);
    say("menu: %s opened", kPages[P]);
}

// One reference less on a game object (a reference count at +8); the last one
// runs its deleting destructor, vtable slot 0.
void release_ref(uint64_t obj) {
    if (!obj) return;
    auto* refs = reinterpret_cast<int32_t*>(obj + 8);
    if ((*refs)-- != 1) return;
    uint64_t vt = 0, dtor = 0;
    if (rd(obj, &vt, 8) && rd(vt, &dtor, 8) && dtor) reinterpret_cast<void (*)(uint64_t)>(dtor)(obj);
}

// The engine's Defaults row taken off again, as a vector element is destroyed,
// and the rows laid out anew.
void drop_defaults_row(uint64_t dialog) {
    uint64_t begin = 0, end = 0;
    if (!rd(dialog + addrs::kItemsBegin, &begin, 8) || !rd(dialog + addrs::kItemsEnd, &end, 8) || !begin ||
        end < begin + addrs::kItemStride)
        return;
    auto* rec = reinterpret_cast<uint8_t*>(end - addrs::kItemStride);
    const char16_t* label = nullptr;
    memcpy(&label, rec, 8);
    if (!label || label != message_text(addrs::kGroupMenuText, addrs::kDefaultsRowId)) {
        say("menu: the last row is not Defaults; left alone");
        return;
    }
    free_wstring(rec + 0x08);
    free_wstring(rec + 0x48);
    release_ref(*reinterpret_cast<uint64_t*>(rec + addrs::kItemWidget));
    end -= addrs::kItemStride;
    memcpy(reinterpret_cast<void*>(dialog + addrs::kItemsEnd), &end, 8);
    call6(addrs::kLayoutRows, dialog, 0);
}

// A page's opener: its sprite's name through the table slot, its handler.
template <int P>
int64_t page_open(uint64_t root, uint64_t params) {
    snprintf(g_open_name, sizeof(g_open_name), "%s", kPages[P]);
    const uint64_t dialog = call6(addrs::kOpenSection, root, params, addr(g_open_name),
                                  addr(reinterpret_cast<void*>(&page_handler<P>)), 0, 0);
    if (dialog && kFirstPageBlock + P == kHealthRow / 1000) drop_defaults_row(dialog);
    return static_cast<int64_t>(dialog);
}

// What a System row runs when picked: the option movie, then the page's opener.
template <int P>
uint64_t page_row(uint64_t out, uint64_t ctx) {
    GuestFunction f(addrs::kOpenerFunctorVTable, addr(reinterpret_cast<void*>(&page_open<P>)));
    call6(addrs::kOpenSectionFlow, out, ctx, addr(f.buf));
    return out;
}

// A YES/NO question as the game's Exit Game asks it: a box with a dialog text
// (group 204) from the menu's factory, a YES command around `yes`, the popup.
uint64_t ask_popup(uint64_t out, uint64_t factory, uint32_t question, int64_t (*yes)(int64_t, int64_t)) {
    alignas(16) uint8_t caption[0x40] = {};
    call6(addrs::kMsgText, addr(caption), addrs::kGroupDialog, question);
    uint64_t box = 0, command = 0;
    call6(addrs::kBoxFromCaption, addr(&box), factory, addr(caption));
    GuestFunction action(addrs::kOpenerFunctorVTable, addr(reinterpret_cast<void*>(yes)));
    call6(addrs::kCommandFromAction, addr(&command), addr(action.buf), 2);
    call6(addrs::kPopupFromBox, out, addr(&box), addr(&command));
    release_ref(command);
    release_ref(box);
    free_wstring(caption + 0x08);
    return out;
}

int64_t leave_yes(int64_t, int64_t) {
    __atomic_store_n(&g_menu_leave, true, __ATOMIC_RELAXED);
    return 0;
}
int64_t give_up_yes(int64_t, int64_t) {
    __atomic_store_n(&g_menu_give_up, true, __ATOMIC_RELAXED);
    return 0;
}
uint64_t leave_opener(uint64_t out, uint64_t factory) { return ask_popup(out, factory, 920101, &leave_yes); }
uint64_t give_up_opener(uint64_t out, uint64_t factory) { return ask_popup(out, factory, 920102, &give_up_yes); }

// The System builder's last row (0x180 bytes), or null.
uint8_t* builder_last_row(uint64_t builder) {
    uint64_t begin = 0, end = 0;
    if (!rd(builder + addrs::kBuilderBegin, &begin, 8) || !rd(builder + addrs::kBuilderEnd, &end, 8) || !begin ||
        end <= begin || (end - begin) % addrs::kBuilderRowStride != 0)
        return nullptr;
    return reinterpret_cast<uint8_t*>(end - addrs::kBuilderRowStride);
}

// Exit Game's row holds its opener somewhere in its std::function.
bool row_is_exit_game(const uint8_t* row) {
    const uint64_t want = api->guest_addr(bn(addrs::kExitGameOpener));
    for (size_t i = 0; i + 8 <= addrs::kBuilderRowStride; i += 8) {
        uint64_t v = 0;
        memcpy(&v, row + i, 8);
        if (v == want) return true;
    }
    return false;
}

// The game's own pop: the row's destructor (vtable slot 1), the end one row back.
void pop_last_row(uint64_t builder) {
    uint8_t* row = builder_last_row(builder);
    uint64_t vt = 0, dtor = 0;
    if (!row || !rd(addr(row), &vt, 8) || !rd(vt + 8, &dtor, 8) || !dtor) return;
    reinterpret_cast<void (*)(uint64_t)>(dtor)(addr(row));
    const uint64_t end = addr(row);
    memcpy(reinterpret_cast<void*>(builder + addrs::kBuilderEnd), &end, 8);
}

void add_command(uint64_t builder, uint32_t id, uint64_t opener) {
    Captions c(id);
    GuestFunction f(addrs::kRowFunctorVTable, opener);
    call6(addrs::kAddExitRow, builder, addr(c.buf), addr(f.buf));
}

// The one call that turns the System builder into its dialog comes here first.
uint64_t coop_system_finalize(uint64_t out, uint64_t builder, uint64_t arg3) {
    uint8_t* last = builder_last_row(builder);
    const bool move_exit = last && row_is_exit_game(last);
    if (move_exit) pop_last_row(builder);
    static uint64_t (*const rows[])(uint64_t, uint64_t) = {&page_row<0>, &page_row<1>, &page_row<2>, &page_row<3>};
    int64_t flag = 0;
    for (uint32_t p = 0; p < 4; ++p) {
        Captions c(kFirstPageRow + p);
        GuestFunction f(addrs::kRowFunctorVTable, addr(reinterpret_cast<void*>(rows[p])));
        call6(addrs::kAddCommandRow, builder, addr(c.buf), addr(f.buf), addr(&flag));
    }
    if (together(g_last)) add_command(builder, 110010, addr(reinterpret_cast<void*>(&leave_opener)));
    if (D.down) add_command(builder, 110011, addr(reinterpret_cast<void*>(&give_up_opener)));
    if (move_exit) add_command(builder, addrs::kExitGameRowId, api->guest_addr(bn(addrs::kExitGameOpener)));
    return call6(addrs::kSystemFinalize, out, builder, arg3);
}

// The pages go in once the game has the co-op mod's text (its menu.msgbnd and
// optionsetting.gfx come together): the name table's free slot 6 names our page,
// the loop over the table walks 7, and the call that finalises System goes
// through the 16 bytes of padding before 0x01BB4C70 to coop_system_finalize.
void install_menu() {
    if (g_menu_on || !g_menu_ok || C.frame - g_menu_tried_frame < 60) return;
    g_menu_tried_frame = C.frame;
    if (!message_text(addrs::kGroupMenuText, kFirstPageRow)) {
        if (singleton(addrs::kMsgRepositoryPtr) && !g_menu_said) {
            g_menu_said = true;
            say("menu: no co-op text in the game (the mod \"Co-op - menu and text\" is not installed); the System menu stays as it is");
        }
        return;
    }
    const uint64_t slot = bn(addrs::kSectionNameTable) + 6 * 8;
    uint64_t slots[2] = {1, 1};
    uint8_t count = 0;
    if (!rd(slot, slots, 16) || slots[0] || slots[1] || !rd(bn(addrs::kSectionCountImm), &count, 1) || count != 6) {
        g_menu_ok = false;
        say("menu: the option screens' name table is not as expected; the co-op pages are off");
        return;
    }
    uint8_t jump[13] = {0x49, 0xBB};  // movabs r11, coop_system_finalize; jmp r11
    const uint64_t target = addr(reinterpret_cast<void*>(&coop_system_finalize));
    memcpy(jump + 2, &target, 8);
    jump[10] = 0x41;
    jump[11] = 0xFF;
    jump[12] = 0xE3;
    uint8_t call_pad[5] = {0xE8};
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(addrs::kTrampolinePad) - static_cast<int64_t>(addrs::kSystemFinalizeCall + 5));
    memcpy(call_pad + 1, &rel, 4);
    const uint64_t name = addr(g_open_name);
    const uint8_t six = 6, seven = 7;
    const char* failed = api->write(slot, &name, 8)                                                       ? "name table"
                         : api->patch(bn(addrs::kSectionCountImm), &six, &seven, 1)                      ? "section count"
                         : api->patch(bn(addrs::kTrampolinePad), addrs::kPadBytes, jump, sizeof(jump)) ? "padding"
                         : api->patch(bn(addrs::kSystemFinalizeCall), addrs::kSystemFinalizeCallBytes, call_pad, 5) ? "finalise call"
                                                                                                          : nullptr;
    g_menu_on = !failed;
    g_menu_ok = !failed;
    if (failed) say("menu: the %s could not be patched; the co-op pages are off", failed);
    else say("menu: the co-op pages are in System");
}

void save_menu() {
    FILE* f = open_tmp("menu.txt");
    if (!f) return;
    fprintf(f, "# Set in the game's System menu; each wins over bbport.ini until the overlay changes it.\n");
    for (const MenuToggle& t : g_toggles)
        if (t.saved >= 0) fprintf(f, "%s %d\n", t.key, t.saved);
    if (g_health_saved) fprintf(f, "party_health %d\n", g_health_saved);
    close_tmp(f, "menu.txt");
}

void load_menu() {
    char line[96], key[64];
    FILE* f = g_dir[0] ? fopen(in_dir("menu.txt"), "r") : nullptr;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (line[0] == '#' || sscanf(line, "%63s %d", key, &v) != 2) continue;
        for (MenuToggle& t : g_toggles)
            if (strcmp(t.key, key) == 0) t.saved = v ? 1 : 0;
        if (strcmp(key, "party_health") == 0 && v >= 100 && v <= 300) g_health_saved = v;
    }
    fclose(f);
}

// An overlay change of a setting the menu had set: the overlay's is the latest.
void forget_menu(const char* key) {
    bool had = false;
    for (MenuToggle& t : g_toggles)
        if (strcmp(t.key, key) == 0 && t.saved >= 0) {
            t.saved = -1;
            had = true;
        }
    if (strcmp(key, "party_health") == 0 && g_health_saved) {
        g_health_saved = 0;
        had = true;
    }
    if (had) save_menu();
}

void on_action(const char* action, void*);

// The rows' changes, and YES to a question, on the main thread.
void menu_tick() {
    install_menu();
    if (__atomic_exchange_n(&g_menu_leave, false, __ATOMIC_RELAXED)) on_action("leave", nullptr);
    if (__atomic_exchange_n(&g_menu_give_up, false, __ATOMIC_RELAXED)) on_action("give_up", nullptr);
    bool changed = false;
    for (MenuToggle& t : g_toggles) {
        if (t.value == t.last) continue;
        t.last = t.value;
        t.saved = t.value ? 1 : 0;
        *t.setting = t.value != 0;
        say("menu: %s %s", t.key, t.value ? "on" : "off");
        changed = true;
    }
    if (g_health != g_health_last) {
        g_health_last = g_health;
        if (g_health >= 100 && g_health <= 300) {
            g_health_saved = S.party_health = g_health;
            say("menu: party_health %d%%", g_health);
            changed = true;
        }
    }
    if (!changed) return;
    save_menu();
    set_groups();
}

// --- the frame, options and actions -------------------------------------------------------------

void save_dirty() {
    if (g_dirty & kSavePending) save_set("pending.txt", g_pending);
    if (g_dirty & kSaveHad) save_set("had.txt", g_had);
    if (g_dirty & kSaveDrops) save_runs("drops.txt", g_given);
    if (g_dirty & kSavePickups) save_runs("pickups.txt", g_pick_given);
    g_dirty = 0;
}

void on_frame(void*) {
    ++C.frame;
    const double t = now_s();
    link_poll(t);
    const World w = observe(t);
    g_last = w;
    if (w.in_world && S.share_pickups && !g_pick_built) build_pickup_table();
    make_revive_row();
    menu_tick();
    pickups_tick(w, t);
    ask_tick(w, t);
    free_bells_tick();
    bells_tick(w, t);
    track_together(w, t);
    arrive_tick(w, t);
    downed_tick(w, t);
    boss_watch_tick(w, t);
    awards_tick(w, t);
    world_out_tick(w, t);
    world_in_tick(w, t);
    if (S.guest) insight_guest_tick(t);
    else insight_host_tick(w, t);
    gestures_tick(w, t);
    gift_tick();
    peer_view_tick(w, t);
    pick_up_tick(w, t);
    boss_progress_tick(w);
    hide_taken_tick(w, t);
    party_tick(w, t);
    if (S.guest) lamp_home_tick(w, t);
    seamless_tick(w, t);
    if (S.guest) guest_tick(w, t);
    else host_tick(w, t);
    link_send(w, t);
    if (g_dirty) save_dirty();
}

void on_world_load(uint32_t block, void*) {
    C.world_since = now_s();
    C.world_seen = true;  // bbport calls this with the player there: observe() keeps the stamp
    say("world: placed in block 0x%08x", block);
    world_load_pass();
}

void on_option(const char* key, const char* value, void*) {
    const bool was_guest = S.guest;
    char partner[64];
    snprintf(partner, sizeof(partner), "%s", S.partner);
    const int port = S.port;
    const bool summon_anywhere = S.summon_anywhere, seamless = S.seamless;
    if (key) forget_menu(key);
    read_settings();
    S.port = port;  // these take effect at the next start
    S.summon_anywhere = summon_anywhere;
    S.seamless = seamless;
    say("option %s = %s", key, value ? value : "");
    if (strcmp(partner, S.partner) != 0) link_set_partner();
    if (was_guest != S.guest) {
        J.ring_attempts = 0;
        L.warned_role = false;
    }
    set_groups();
}

void on_action(const char* action, void*) {
    const double t = now_s();
    if (strcmp(action, "leave") == 0) {
        J.pause_until = t + S.pause_seconds;
        const int rc = together(g_last) ? api->lua_event("OnLeaveMenu_Yes") : 0;
        say("action leave: %s; re-joining paused for %d s", together(g_last) ? (rc == 0 ? "leaving" : "leave refused") : "not together",
             S.pause_seconds);
        if (together(g_last)) note(9950036, "Co-op: left the co-op session");
    } else if (strcmp(action, "give_up") == 0) {
        g_give_up = D.down;
        say("action give up: %s", D.down ? "dying at the next frame" : "you are not down");
    } else if (strcmp(action, "join") == 0) {
        J.pause_until = 0;
        J.backoff_until = 0;
        J.last_ring = 0;
        J.last_warp = 0;
        J.ring_attempts = 0;
        J.joined_once = false;
        C.world_since = t - 600.0;  // no settle wait
        say("action join: pause cleared, ringing now");
        if (!together(g_last)) note(9950037, "Co-op: joining your partner");
    }
}

}  // namespace

extern "C" BB_PLUGIN_EXPORT int bb_plugin_init(const BbHostApi* a) {
    if (!a || a->version < 11 || a->size < sizeof(BbHostApi)) return 1;
    api = a;
    return 0;
}

extern "C" BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* a) {
    if (!a || a->version < 11 || a->size < sizeof(BbHostApi)) return 1;
    api = a;
    if (!api->eboot_is_109()) {
        say("not the 1.09 eboot (%s); off", api->eboot_sha256());
        return 1;
    }
    const char* dir = api->plugin_dir("bbcoop");
    snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : "");
    load_menu();  // what the game's System menu set, over bbport.ini
    read_settings();
    say("0.10.2 as %s; free bells %d, give bells %d, ring %d, summon anywhere %d, follow %d, re-join %d, stay after boss %d, lamps %d, talk %d, boss progress %d, "
        "drops %d, insight %d, rewards %d, world %d, pickups %d (copied %d), down not dead %d, spectate %d, pick up %d, lamp revive %d, seamless %d",
        S.guest ? "guest" : "host", S.free_bells, S.give_bells, S.auto_ring, S.summon_anywhere, S.follow_host, S.auto_rejoin, S.keep_after_boss,
        S.lamps, S.guest_talk, S.boss_progress, S.share_drops, S.share_insight, S.share_rewards, S.share_world, S.guest_pickups, S.share_pickups, S.downed,
        S.spectate, S.pick_up, S.lamp_revive, S.seamless);
    g_functions_ok = check_functions(addrs::kFunctions, "travelling, ringing and the seamless hook are off");
    g_menu_ok = check_functions(addrs::kMenuFunctions, "the game's own menu and text are off");
    if (S.summon_anywhere)
        apply_group("summon_anywhere", addrs::kSummonAnywhere, true);
    // Placed whatever the settings (the game's menu turns them on and off as it runs); they act
    // only while someone is down, something is picked up or given, or a boss dies.
    const auto hook = [](const char* what, const addrs::Function& f, int (*fn)(BbHookCtx*, void*)) {
        const int rc = api->hook2(bn(f.va), f.prologue, f.n, fn, nullptr);
        say("%s hook at 0x%llx -> %s", what, static_cast<unsigned long long>(f.va),
            rc == 0 ? "ok" : rc == 1 ? "bytes differ" : "not placed");
        return rc == 0;
    };
    if (g_functions_ok) {
        hook("down: enemy sight", addrs::kSightRecord, &on_sight_record);
        hook("down: lamp prompt", addrs::kActionPrompt, &on_action_prompt);
        g_pick_hook = hook("pickups: pick-up", addrs::kPickUp, &on_pick_up);
        hook("rewards: award", addrs::kAwardItemLot, &on_award);
        hook("boss: boss clear", addrs::kBlockClear, &on_block_clear);
    }
    set_groups();
    if (S.seamless && g_functions_ok && apply_group("seamless", addrs::kSeamless, true)) {
        g_seamless_on = hook("seamless: OnMatchingCheck", addrs::kOnMatchingCheck, &on_matching_check);
        if (!g_seamless_on) apply_group("seamless", addrs::kSeamless, false);
    }
    load_set("pending.txt", &g_pending);
    load_set("had.txt", &g_had);
    load_runs("drops.txt", &g_given);
    load_runs("pickups.txt", &g_pick_given);
    unsigned run = 0, added = 0;
    if (load_pair("insight.txt", &run, &added) && added <= 0xFFFF) {
        I.run = run;
        I.added = static_cast<uint16_t>(added);
    }
    load_pair("awards.txt", &A.peer_run, &A.done);
    build_slots();
    if (S.guest) load_world_in();
    else load_world_out();
    load_gift();
    L.run = static_cast<uint32_t>(time(nullptr)) ^ (static_cast<uint32_t>(getpid()) << 12);
    R.ask = static_cast<uint16_t>(L.run);
    if (g_pending.count) say("boss progress: %d kill(s) waiting from last time", g_pending.count);
    link_set_partner();
    link_open();
    api->on_frame(&on_frame, nullptr);
    api->on_world_load(&on_world_load, nullptr);
    api->on_event_flag(&on_flag, nullptr);
    api->on_option("bbcoop", &on_option, nullptr);
    api->on_action("bbcoop", &on_action, nullptr);
    return 0;
}
