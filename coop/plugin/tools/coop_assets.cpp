// coop_assets: the Bloodborne (PS4 1.09, CUSA00900) menu files for the
// "Co-op - menu and text" loose-file mod.
//
// Adapted from bbhost src/engine/menu_assets.cpp, GPL-3.0. The movie and
// message generators below are its code with the runtime FS, logging, stamp
// cache, built-in strings and title.gfx Quit-row edit removed, the sections
// and the event-text id range changed, and the strings read from a TSV at
// run time. This file is therefore GPL-3.0 as well.
//
//   coop_assets <dvdroot_ps4 dir> <messages.tsv> <out mod dir>
//
// Writes, from the dump's own files (which it only reads):
//   <out>/dvdroot_ps4/menu/optionsetting.gfx   the dump's movie plus four
//       co-op sections cloned from ControllSetting (CoopJoin, CoopDeaths,
//       CoopTogether, CoopItems; CoopDeaths without the Defaults caption)
//       and the System list's scroll bar;
//   <out>/dvdroot_ps4/msg/<lang>/menu.msgbnd.dcx   for every language folder
//       of the dump that has one, with the TSV's ids added (English text in
//       every language). Ids 920000..929999 go to the dialog groups 78 and 204,
//       9950000..9959999 to event text (group 30), all others to menu text
//       (200) and line help (201). An id a group already has is refused.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -o coop_assets coop_assets.cpp -lz

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Two edits. The movie: each section is a clone of the dump's
// ControllSetting sprite under a new instance name and character id, with its
// title retitled to the mod's message id, its static text dropped, its
// widget rows kept, dropped or added to fit the section, and its placement
// put on the main timeline hidden (the game shows the section it opens); then
// the System list gets the movie's own ScrollBarV, since its rows scroll in
// five lines. The messages: the mod's ids added to the menu text (group 200)
// and line help (201) FMGs of the dump's DCX-compressed BND4, the dialog
// groups (78, 204) and the event text (30).

namespace {

using Bytes = std::vector<std::uint8_t>;

using Fail = std::runtime_error;

std::uint16_t u16(const Bytes& b, std::size_t i) {
    if (i + 2 > b.size()) throw Fail{"truncated"};
    return static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8));
}
std::uint32_t u32(const Bytes& b, std::size_t i) {
    if (i + 4 > b.size()) throw Fail{"truncated"};
    return static_cast<std::uint32_t>(b[i]) | (static_cast<std::uint32_t>(b[i + 1]) << 8) |
           (static_cast<std::uint32_t>(b[i + 2]) << 16) | (static_cast<std::uint32_t>(b[i + 3]) << 24);
}
std::uint64_t u64(const Bytes& b, std::size_t i) { return u32(b, i) | (static_cast<std::uint64_t>(u32(b, i + 4)) << 32); }
std::uint32_t be32(const Bytes& b, std::size_t i) {
    if (i + 4 > b.size()) throw Fail{"truncated"};
    return (static_cast<std::uint32_t>(b[i]) << 24) | (static_cast<std::uint32_t>(b[i + 1]) << 16) |
           (static_cast<std::uint32_t>(b[i + 2]) << 8) | b[i + 3];
}
void put16(Bytes& b, std::size_t i, std::uint32_t v) {
    b[i] = static_cast<std::uint8_t>(v);
    b[i + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(Bytes& b, std::size_t i, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[i + k] = static_cast<std::uint8_t>(v >> (8 * k));
}
void put64(Bytes& b, std::size_t i, std::uint64_t v) {
    put32(b, i, static_cast<std::uint32_t>(v));
    put32(b, i + 4, static_cast<std::uint32_t>(v >> 32));
}
void putbe32(Bytes& b, std::size_t i, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[i + k] = static_cast<std::uint8_t>(v >> (24 - 8 * k));
}
void app16(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void app(Bytes& b, const Bytes& src, std::size_t from, std::size_t to) { b.insert(b.end(), src.begin() + from, src.begin() + to); }
void app(Bytes& b, const Bytes& src) { b.insert(b.end(), src.begin(), src.end()); }
void app(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

// ---------------------------------------------------------------- the movie

constexpr int kEnd = 0, kShowFrame = 1, kPlace2 = 26, kDefineSprite = 39, kPlace3 = 70, kSymbolClass = 76;

struct Tag {
    int code;
    std::size_t head, s, e;
};

std::vector<Tag> tags(const Bytes& b, std::size_t i, std::size_t end) {
    std::vector<Tag> out;
    while (i < end) {
        const std::uint16_t th = u16(b, i);
        const std::size_t head = i;
        i += 2;
        const int code = th >> 6;
        std::size_t length = th & 0x3f;
        if (length == 0x3f) {
            length = u32(b, i);
            i += 4;
        }
        if (i + length > b.size()) throw Fail{"a tag runs past the end"};
        out.push_back({code, head, i, i + length});
        if (code == kEnd) break;
        i += length;
    }
    return out;
}

std::size_t body_start(const Bytes& b) {
    const int nbits = b.at(8) >> 3;
    return 8 + (5 + 4 * nbits + 7) / 8 + 4;
}

Bytes tag_bytes(int code, const Bytes& body) {
    Bytes out;
    if (body.size() < 0x3f) {
        app16(out, static_cast<std::uint32_t>((code << 6) | body.size()));
    } else {
        app16(out, static_cast<std::uint32_t>((code << 6) | 0x3f));
        out.resize(out.size() + 4);
        put32(out, out.size() - 4, static_cast<std::uint32_t>(body.size()));
    }
    app(out, body);
    return out;
}

std::string cstr(const Bytes& b, std::size_t i, std::size_t* next) {
    std::size_t j = i;
    while (j < b.size() && b[j]) ++j;
    if (j >= b.size()) throw Fail{"an unterminated name"};
    *next = j + 1;
    return std::string(b.begin() + i, b.begin() + j);
}

// An n-bit two's complement field as a number.
std::int32_t sext(std::uint32_t v, int n) {
    if (n && ((v >> (n - 1)) & 1)) return static_cast<std::int32_t>(static_cast<std::int64_t>(v) - (std::int64_t{1} << n));
    return static_cast<std::int32_t>(v);
}

struct BitR {
    const Bytes& b;
    std::size_t i;
    int bit = 0;
    std::uint32_t u(int n) {
        std::uint32_t v = 0;
        for (int k = 0; k < n; ++k) {
            v = (v << 1) | ((b.at(i) >> (7 - bit)) & 1);
            if (++bit == 8) {
                bit = 0;
                ++i;
            }
        }
        return v;
    }
    std::int32_t s(int n) { return sext(u(n), n); }
    std::size_t align() {
        if (bit) {
            bit = 0;
            ++i;
        }
        return i;
    }
};

struct BitW {
    std::vector<int> bits;
    void u(std::uint32_t v, int n) {
        for (int k = n - 1; k >= 0; --k) bits.push_back((v >> k) & 1);
    }
    Bytes out() {
        while (bits.size() % 8) bits.push_back(0);
        Bytes r;
        for (std::size_t i = 0; i < bits.size(); i += 8) {
            int v = 0;
            for (int k = 0; k < 8; ++k) v = (v << 1) | bits[i + k];
            r.push_back(static_cast<std::uint8_t>(v));
        }
        return r;
    }
};

int sbits(std::int64_t v) {
    int n = 1;
    while (!(-(std::int64_t{1} << (n - 1)) <= v && v < (std::int64_t{1} << (n - 1)))) ++n;
    return n;
}

// A matrix as the section clone keeps it: the scale and rotation
// fields exactly as read, the translation as numbers.
struct Matrix {
    bool has_scale = false, has_rot = false;
    int sn = 0, rn = 0;
    std::uint32_t sa = 0, sc = 0, ra = 0, rc = 0;
    std::int64_t tx = 0, ty = 0;
};

Matrix read_matrix(const Bytes& b, std::size_t i, std::size_t* end) {
    BitR r{b, i};
    Matrix m;
    if (r.u(1)) {
        m.has_scale = true;
        m.sn = static_cast<int>(r.u(5));
        m.sa = r.u(m.sn);
        m.sc = r.u(m.sn);
    }
    if (r.u(1)) {
        m.has_rot = true;
        m.rn = static_cast<int>(r.u(5));
        m.ra = r.u(m.rn);
        m.rc = r.u(m.rn);
    }
    const int n = static_cast<int>(r.u(5));
    m.tx = r.s(n);
    m.ty = r.s(n);
    *end = r.align();
    return m;
}

Bytes write_matrix(const Matrix& m) {
    BitW w;
    w.u(m.has_scale, 1);
    if (m.has_scale) {
        w.u(static_cast<std::uint32_t>(m.sn), 5);
        w.u(m.sa, m.sn);
        w.u(m.sc, m.sn);
    }
    w.u(m.has_rot, 1);
    if (m.has_rot) {
        w.u(static_cast<std::uint32_t>(m.rn), 5);
        w.u(m.ra, m.rn);
        w.u(m.rc, m.rn);
    }
    // BitW::u writes only the low n bits, so the two's complement needs no mask.
    const int n = std::max(sbits(m.tx), sbits(m.ty));
    w.u(static_cast<std::uint32_t>(n), 5);
    w.u(static_cast<std::uint32_t>(m.tx), n);
    w.u(static_cast<std::uint32_t>(m.ty), n);
    return w.out();
}

std::size_t skip_cxform(const Bytes& b, std::size_t i) {
    BitR r{b, i};
    const std::uint32_t has_add = r.u(1), has_mul = r.u(1);
    const int n = static_cast<int>(r.u(4));
    if (has_mul) r.u(n * 4);
    if (has_add) r.u(n * 4);
    return r.align();
}

struct Place2 {
    std::uint8_t flags = 0;
    std::uint16_t depth = 0;
    std::optional<std::uint16_t> cid;
    std::optional<Matrix> mat;
    std::optional<std::string> name;
    Bytes mid, tail;
};

Place2 parse_place2(const Bytes& b, std::size_t s, std::size_t e) {
    Place2 p;
    std::size_t i = s;
    p.flags = b.at(i);
    p.depth = u16(b, i + 1);
    i += 3;
    if (p.flags & 0x02) {
        p.cid = u16(b, i);
        i += 2;
    }
    if (p.flags & 0x04) p.mat = read_matrix(b, i, &i);
    const std::size_t mat_end = i;
    if (p.flags & 0x08) i = skip_cxform(b, i);
    if (p.flags & 0x10) i += 2;
    const std::size_t name_start = i;
    if (p.flags & 0x20) p.name = cstr(b, i, &i);
    p.mid.assign(b.begin() + mat_end, b.begin() + name_start);
    p.tail.assign(b.begin() + i, b.begin() + e);
    return p;
}

Bytes build_place2(const Place2& p) {
    // The name flag follows the name, which a moved row may gain or lose.
    const auto flags = static_cast<std::uint8_t>(p.name ? p.flags | 0x20 : p.flags & ~0x20);
    Bytes out;
    out.push_back(flags);
    app16(out, p.depth);
    if (flags & 0x02) app16(out, p.cid.value_or(0));
    if (flags & 0x04) {
        if (!p.mat) throw Fail{"a placement with a matrix flag and no matrix"};
        app(out, write_matrix(*p.mat));
    }
    app(out, p.mid);
    if (p.name) {
        app(out, *p.name);
        out.push_back(0);
    }
    app(out, p.tail);
    return out;
}

Bytes build_place3_hidden(std::uint16_t cid, std::uint16_t depth, const Matrix& mat, const std::string& name) {
    Bytes out;
    out.push_back(0x02 | 0x04 | 0x20);
    out.push_back(0x20);
    app16(out, depth);
    app16(out, cid);
    app(out, write_matrix(mat));
    app(out, name);
    out.push_back(0);
    out.push_back(0);  // visible = false
    return out;
}

std::map<std::uint16_t, std::string> symbol_classes(const Bytes& b) {
    std::map<std::uint16_t, std::string> out;
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        if (t.code != kSymbolClass) continue;
        const std::uint16_t n = u16(b, t.s);
        std::size_t i = t.s + 2;
        for (int k = 0; k < n; ++k) {
            const std::uint16_t cid = u16(b, i);
            out[cid] = cstr(b, i + 2, &i);
        }
    }
    return out;
}

std::optional<Bytes> add_symbol_class(const Bytes& b, std::uint16_t cid, const std::string& name) {
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        if (t.code != kSymbolClass) continue;
        const std::uint16_t n = u16(b, t.s);
        Bytes body(b.begin() + t.s, b.begin() + t.e);
        put16(body, 0, n + 1u);
        app16(body, cid);
        app(body, name);
        body.push_back(0);
        Bytes out(b.begin(), b.begin() + t.head);
        app(out, tag_bytes(kSymbolClass, body));
        app(out, b, t.e, b.size());
        return out;
    }
    return std::nullopt;
}

std::optional<std::uint16_t> place_char(const Bytes& b, std::size_t ts, int code) {
    if (code != kPlace2 && code != kPlace3) return std::nullopt;
    const std::size_t off = code == kPlace3 ? 4 : 3;
    if (!(b.at(ts) & 0x02)) return std::nullopt;
    return u16(b, ts + off);
}

std::optional<Tag> find_sprite(const Bytes& b, std::uint16_t cid) {
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        if (t.code == kDefineSprite && u16(b, t.s) == cid) return t;
    }
    return std::nullopt;
}

std::uint16_t max_character_id(const Bytes& b) {
    static const std::set<int> kDefines = {2,  6,  20, 21, 22, 32, 33, 34, 36, 37, 39, 46, 48, 60, 75, 83, 84,
                                           1001, 1003, 1005, 1006, 1007, 1008, 1009, 1010};
    std::uint16_t best = 0;
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        if (kDefines.count(t.code) && t.e - t.s >= 2) best = std::max(best, u16(b, t.s));
    }
    return best;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// One section cloned from ControllSetting into the movie. Its Item_6_0 is the
// Defaults caption: moved to the defaults slot when the section has one, and
// dropped with the drop rows when it has none.
Bytes add_option_section(const Bytes& b, const std::string& name, int title_id, int section_depth,
                         std::optional<int> defaults_slot, std::set<std::string> drop_names) {
    if (b.size() < 16 || std::memcmp(b.data(), "GFX", 3) != 0) throw Fail{"optionsetting.gfx is not an uncompressed GFX"};
    const std::string clone = "ControllSetting", top = "Top";
    const auto names = symbol_classes(b);
    struct Main {
        Place2 p;
        std::size_t s;
    };
    std::map<std::string, Main> places;
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        if (t.code != kPlace2) continue;
        Place2 p = parse_place2(b, t.s, t.e);
        if (p.name && !p.name->empty()) places[*p.name] = Main{p, t.s};
    }
    for (const std::string& want : {clone, top}) {
        if (!places.count(want)) throw Fail{"no main-timeline instance named " + want};
    }
    if (places.count(name)) throw Fail{"already has an instance named " + name};

    const Place2& clone_p = places[clone].p;
    const std::uint16_t new_cid = static_cast<std::uint16_t>(max_character_id(b) + 1);
    const auto clone_sprite = find_sprite(b, clone_p.cid.value_or(0));
    if (!clone_sprite) throw Fail{"no DefineSprite for " + clone};
    const std::size_t cs = clone_sprite->s, ce = clone_sprite->e;

    // The title: the first child sprite with a StaticText_, copied under the
    // next character id with the message id swapped in place.
    const auto title_cid = static_cast<std::uint16_t>(new_cid + 1);
    std::optional<std::uint16_t> title_old;
    Bytes title_sprite_body;
    for (const Tag& t : tags(b, cs + 4, ce)) {
        const auto cid = place_char(b, t.s, t.code);
        if (!cid) continue;
        const auto child = find_sprite(b, *cid);
        if (!child) continue;
        for (const Tag& t2 : tags(b, child->s + 4, child->e)) {
            if (t2.code != kPlace2) continue;
            const Place2 q = parse_place2(b, t2.s, t2.e);
            if (!q.name || !starts_with(*q.name, "StaticText_")) continue;
            const std::string& old_name = *q.name;
            const std::string new_name = "StaticText_" + std::to_string(title_id);
            if (new_name.size() != old_name.size()) throw Fail{"the title id is not the same length as " + old_name};
            title_sprite_body.assign(b.begin() + child->s, b.begin() + child->e);
            put16(title_sprite_body, 0, title_cid);
            auto hit = std::search(title_sprite_body.begin(), title_sprite_body.end(), old_name.begin(), old_name.end());
            if (hit == title_sprite_body.end()) throw Fail{"cannot find " + old_name + " in the title sprite"};
            std::copy(new_name.begin(), new_name.end(), hit);
            title_old = cid;
            break;
        }
        if (title_old) break;
    }
    if (!title_old) throw Fail{"no title StaticText found in the clone"};

    std::optional<std::uint16_t> label_depth;
    std::int64_t label_dy = 0;
    std::optional<std::string> label_name;
    if (defaults_slot) {
        label_name = "Item_" + std::to_string(*defaults_slot) + "_0";
        std::map<std::string, std::int64_t> ys;
        bool label_is_row = false;
        for (const Tag& t : tags(b, cs + 4, ce)) {
            if (t.code != kPlace2) continue;
            const Place2 p2 = parse_place2(b, t.s, t.e);
            if (!p2.name) continue;
            if ((*p2.name == "Item_0_0" || *p2.name == "Item_1_0") && p2.mat) ys[*p2.name] = p2.mat->ty;
            if (*p2.name == "Item_6_0") label_depth = p2.depth;
            else if (*p2.name == *label_name) label_is_row = true;
        }
        if (!label_depth || ys.size() != 2) throw Fail{"the donor has no Item_6_0 label slot, or no two rows"};
        label_dy = (ys["Item_1_0"] - ys["Item_0_0"]) * (*defaults_slot - 6);
        if (label_is_row && !drop_names.count(*label_name)) throw Fail{*label_name + " is a row; drop it"};
    } else {
        drop_names.insert("Item_6_0");
    }

    Bytes new_body;
    app16(new_body, new_cid);
    app(new_body, b, cs + 2, cs + 4);
    for (const Tag& t : tags(b, cs + 4, ce)) {
        if (t.code == kPlace2) {
            Place2 p2 = parse_place2(b, t.s, t.e);
            const bool named = p2.name && !p2.name->empty();
            if (named && starts_with(*p2.name, "StaticText")) continue;
            if (named && drop_names.count(*p2.name)) continue;
            if (label_depth && p2.depth == *label_depth && (label_dy || named)) {
                if (p2.mat) p2.mat->ty += label_dy;
                p2.name = named ? label_name : std::nullopt;
                app(new_body, tag_bytes(kPlace2, build_place2(p2)));
                continue;
            }
        }
        if (place_char(b, t.s, t.code) == title_old) {
            Bytes body(b.begin() + t.s, b.begin() + t.e);
            put16(body, t.code == kPlace3 ? 4 : 3, title_cid);
            app(new_body, tag_bytes(t.code, body));
            continue;
        }
        app(new_body, b, t.head, t.e);
    }

    if (!clone_p.mat) throw Fail{clone + " is placed without a matrix"};
    const Bytes new_section = tag_bytes(kPlace3, build_place3_hidden(new_cid, static_cast<std::uint16_t>(section_depth), *clone_p.mat, name));

    const std::size_t top_s = places[top].s;
    Bytes out(b.begin(), b.begin() + body_start(b));
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        app(out, b, t.head, t.e);
        if (t.code == kDefineSprite && t.s == cs) {
            app(out, tag_bytes(kDefineSprite, title_sprite_body));
            app(out, tag_bytes(kDefineSprite, new_body));
        } else if (t.code == kPlace2 && t.s == top_s) {
            app(out, new_section);
        }
    }
    if (auto it = names.find(clone_p.cid.value_or(0)); it != names.end()) {
        if (auto with_class = add_symbol_class(out, new_cid, it->second)) out = std::move(*with_class);
    }
    put32(out, 4, static_cast<std::uint32_t>(out.size()));
    return out;
}

// The scroll bar: a placement's (depth, character, matrix, name) with
// PlaceObject3's class name skipped.
struct Placement {
    std::uint16_t depth = 0;
    std::optional<std::uint16_t> cid;
    Matrix mat;  // all zero, so no scale and no move, when it has none
    std::optional<std::string> name;
};

Placement placement(const Bytes& b, int code, std::size_t s) {
    Placement p;
    const std::uint8_t flags = b.at(s);
    std::size_t i = s + 1;
    std::uint8_t flags3 = 0;
    if (code == kPlace3) flags3 = b.at(i++);
    p.depth = u16(b, i);
    i += 2;
    if (code == kPlace3 && ((flags3 & 0x08) || ((flags3 & 0x10) && (flags & 0x02)))) cstr(b, i, &i);
    if (flags & 0x02) {
        p.cid = u16(b, i);
        i += 2;
    }
    if (flags & 0x04) p.mat = read_matrix(b, i, &i);
    if (flags & 0x08) i = skip_cxform(b, i);
    if (flags & 0x10) i += 2;
    if (flags & 0x20) p.name = cstr(b, i, &i);
    return p;
}

Bytes add_scrollbar(const Bytes& b, const std::string& list, double ax, double ay) {
    const std::string name = "ScrollBarV";
    std::optional<Placement> target;
    for (const Tag& t : tags(b, body_start(b), b.size())) {
        if (t.code != kPlace2 && t.code != kPlace3) continue;
        Placement p = placement(b, t.code, t.s);
        if (p.name && *p.name == list) {
            target = p;
            break;
        }
    }
    const auto ls = target && target->cid ? find_sprite(b, *target->cid) : std::nullopt;
    if (!ls) throw Fail{"no main-timeline sprite named " + list};
    const Matrix& lm = target->mat;
    if (lm.ra || lm.rc) throw Fail{list + " is rotated"};
    const double sx = lm.has_scale ? sext(lm.sa, lm.sn) / 65536.0 : 1.0;
    const double sy = lm.has_scale ? sext(lm.sc, lm.sn) / 65536.0 : 1.0;

    // The first ScrollBarV of any sprite, in file order.
    std::optional<std::uint16_t> donor;
    for (const Tag& s : tags(b, body_start(b), b.size())) {
        if (s.code != kDefineSprite) continue;
        for (const Tag& t : tags(b, s.s + 4, s.e)) {
            if (t.code != kPlace2 && t.code != kPlace3) continue;
            const Placement p = placement(b, t.code, t.s);
            if (p.name && *p.name == name && p.cid) {
                donor = p.cid;
                break;
            }
        }
        if (donor) break;
    }
    if (!donor) throw Fail{"no " + name + " anywhere to reuse"};

    int depth = 0;
    std::optional<std::size_t> first_show;
    for (const Tag& t : tags(b, ls->s + 4, ls->e)) {
        if (t.code == kPlace2 || t.code == kPlace3) {
            const Placement p = placement(b, t.code, t.s);
            depth = std::max<int>(depth, p.depth);
            if (p.name && *p.name == name) throw Fail{list + " already has a " + name};
        }
        if (t.code == kShowFrame && !first_show) first_show = t.head;
    }
    if (!first_show) throw Fail{list + " has no frames"};
    // Python's round() rounds half to even, as nearbyint does in the default mode.
    Matrix at;
    at.tx = static_cast<std::int64_t>(std::nearbyint((ax * 20 - static_cast<double>(lm.tx)) / sx));
    at.ty = static_cast<std::int64_t>(std::nearbyint((ay * 20 - static_cast<double>(lm.ty)) / sy));
    Bytes place;
    place.push_back(0x02 | 0x04 | 0x20);
    app16(place, static_cast<std::uint32_t>(depth + 1));
    app16(place, *donor);
    app(place, write_matrix(at));
    app(place, name);
    place.push_back(0);
    Bytes new_sprite(b.begin() + ls->s, b.begin() + *first_show);
    app(new_sprite, tag_bytes(kPlace2, place));
    app(new_sprite, b, *first_show, ls->e);
    Bytes out(b.begin(), b.begin() + ls->head);
    app(out, tag_bytes(kDefineSprite, new_sprite));
    app(out, b, ls->e, b.size());
    put32(out, 4, static_cast<std::uint32_t>(out.size()));
    return out;
}

// The movie: the four co-op sections, then the System list's scroll bar
// (the System list has up to twelve rows with the co-op ones).
Bytes build_option_movie(const Bytes& src) {
    Bytes m = add_option_section(src, "CoopJoin", 125020, 383, 6, {});
    m = add_option_section(m, "CoopDeaths", 126020, 384, std::nullopt, {});  // no Defaults: its last row is a pick list
    m = add_option_section(m, "CoopTogether", 127020, 385, 4, {"Item_4_0", "Item_5_0"});
    m = add_option_section(m, "CoopItems", 128020, 386, 3, {"Item_3_0", "Item_4_0", "Item_5_0"});
    return add_scrollbar(m, "Top", 1290, 354.5);
}

// ---------------------------------------------------------------- the messages

constexpr std::uint32_t kMenuText = 200, kLineHelp = 0xc9;
// Dialog text: ids from 920000 go to group 78 (MenuMan's generic dialog,
// sub_2022750) and group 204 (the options screens' own questions, sub_1f24850)
// instead of the menu groups.
constexpr std::uint32_t kDialogText = 78, kScreenDialogText = 0xcc;
constexpr std::int32_t kFirstDialogId = 920000, kLastDialogId = 929999;
// Event text: ids 9950000..9959999 go to group 30 (イベントテキスト), the
// prompt and the messages bbcoop shows on screen.
constexpr std::uint32_t kEventText = 30;
constexpr std::int32_t kFirstEventId = 9950000, kLastEventId = 9959999;
int message_kind(std::int32_t id) {
    return id >= kFirstEventId && id <= kLastEventId ? 2 : id >= kFirstDialogId && id <= kLastDialogId ? 1 : 0;
}

std::size_t find(const Bytes& b, const char* what, std::size_t n) {
    auto it = std::search(b.begin(), b.end(), what, what + n);
    return it == b.end() ? std::string::npos : static_cast<std::size_t>(it - b.begin());
}

struct BndFile {
    std::uint64_t flags;
    std::uint32_t id;
    Bytes name;  // as stored, without its terminator
    Bytes data;
};

struct Bnd4 {
    Bytes head;
    std::uint64_t esize = 0;
    std::vector<BndFile> files;
};

Bnd4 bnd_read(const Bytes& b) {
    if (b.size() < 0x40 || std::memcmp(b.data(), "BND4", 4) != 0) throw Fail{"not a BND4"};
    Bnd4 r;
    r.head.assign(b.begin(), b.begin() + 0x40);
    r.esize = u64(b, 0x20);
    const std::uint32_t n = u32(b, 0x0c);
    if (b[0x30] == 0) throw Fail{"the bundle's names are not UTF-16"};
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::size_t e = 0x40 + k * r.esize;
        BndFile f;
        f.flags = u64(b, e);
        const std::uint64_t size = u64(b, e + 8);
        const std::uint32_t off = u32(b, e + 0x18);
        f.id = u32(b, e + 0x1c);
        const std::uint32_t name_off = u32(b, e + 0x20);
        // UTF-16 code unit by code unit (changed from bbhost, whose byte
        // search for "\0\0" stopped inside "\一行ヘルプ.fmg": '\' is 5C 00
        // and U+4E00 is 00 4E, so the name was cut to "...\64bit\").
        std::size_t z = name_off;
        while (z + 1 < b.size() && !(b[z] == 0 && b[z + 1] == 0)) z += 2;
        if (z + 1 >= b.size()) throw Fail{"an unterminated bundle entry name"};
        f.name.assign(b.begin() + name_off, b.begin() + z);
        if (off + size > b.size()) throw Fail{"a bundle entry runs past the end"};
        f.data.assign(b.begin() + off, b.begin() + off + size);
        r.files.push_back(std::move(f));
    }
    return r;
}

Bytes bnd_pack(const Bnd4& r) {
    Bytes names;
    std::vector<std::uint32_t> name_off;
    const std::size_t base = 0x40 + r.files.size() * r.esize;
    for (const BndFile& f : r.files) {
        name_off.push_back(static_cast<std::uint32_t>(base + names.size()));
        app(names, f.name);
        names.push_back(0);
        names.push_back(0);
    }
    const std::size_t data_start = base + names.size();
    Bytes blobs;
    std::vector<std::uint32_t> offs;
    for (const BndFile& f : r.files) {
        // 16-byte aligned in the file, as the dump's are (changed from bbhost,
        // which aligned them relative to the end of the names only).
        blobs.resize(blobs.size() + ((0x10 - (data_start + blobs.size()) % 0x10) % 0x10), 0);
        offs.push_back(static_cast<std::uint32_t>(data_start + blobs.size()));
        app(blobs, f.data);
    }
    Bytes out = r.head;
    put64(out, 0x28, data_start);
    for (std::size_t k = 0; k < r.files.size(); ++k) {
        Bytes e(r.esize, 0);
        put64(e, 0, r.files[k].flags);
        put64(e, 8, r.files[k].data.size());
        put64(e, 16, r.files[k].data.size());
        put32(e, 0x18, offs[k]);
        put32(e, 0x1c, r.files[k].id);
        put32(e, 0x20, name_off[k]);
        app(out, e);
    }
    app(out, names);
    app(out, blobs);
    return out;
}

using Fmg = std::map<std::int32_t, std::optional<Bytes>>;  // id -> UTF-16LE text, nullopt for an id with none

Fmg fmg_read(const Bytes& b) {
    if (b.size() < 0x28 || b[2] != 2) throw Fail{"an FMG that is not version 2"};
    const std::uint32_t gcount = u32(b, 0x0c), scount = u32(b, 0x10);
    const std::uint64_t soff = u64(b, 0x18);
    Fmg ids;
    for (std::uint32_t g = 0; g < gcount; ++g) {
        const auto idx = static_cast<std::int32_t>(u32(b, 0x28 + g * 16));
        const auto first = static_cast<std::int32_t>(u32(b, 0x28 + g * 16 + 4));
        const auto last = static_cast<std::int32_t>(u32(b, 0x28 + g * 16 + 8));
        for (std::int32_t mid = first, k = 0; mid <= last; ++mid, ++k) {
            if (static_cast<std::uint32_t>(idx + k) >= scount) throw Fail{"an FMG group past its strings"};
            const std::uint64_t o = u64(b, soff + static_cast<std::uint64_t>(idx + k) * 8);
            if (!o) {
                ids[mid] = std::nullopt;
                continue;
            }
            // UTF-16 code unit by code unit (changed from bbhost, whose byte
            // search for "\0\0" cut a string at any ASCII character followed
            // by one of U+xx00 - "HP\u3000...", "\n一...", Korean " 가" - and
            // so truncated hundreds of the game's own CJK strings).
            std::size_t e = o;
            while (e + 1 < b.size() && !(b[e] == 0 && b[e + 1] == 0)) e += 2;
            if (e + 1 >= b.size()) throw Fail{"an unterminated FMG string"};
            ids[mid] = Bytes(b.begin() + o, b.begin() + e);
        }
    }
    return ids;
}

Bytes fmg_write(const Fmg& ids) {
    std::vector<std::int32_t> order;
    for (const auto& [k, v] : ids) order.push_back(k);
    struct Group {
        std::size_t idx;
        std::int32_t first, last;
    };
    std::vector<Group> groups;
    std::size_t run_start = 0;
    for (std::size_t i = 1; i <= order.size(); ++i) {
        if (i == order.size() || order[i] != order[i - 1] + 1) {
            groups.push_back({run_start, order[run_start], order[i - 1]});
            run_start = i;
        }
    }
    std::map<Bytes, std::size_t> strings;
    Bytes pool;
    for (std::int32_t mid : order) {
        const auto& t = ids.at(mid);
        if (!t || strings.count(*t)) continue;
        strings[*t] = pool.size();
        app(pool, *t);
        pool.push_back(0);
        pool.push_back(0);
    }
    const std::size_t soff = 0x28 + groups.size() * 16, body = soff + order.size() * 8;
    Bytes out(body + pool.size(), 0);
    out[2] = 2;
    put32(out, 0x08, 1);
    put32(out, 0x0c, static_cast<std::uint32_t>(groups.size()));
    put32(out, 0x10, static_cast<std::uint32_t>(order.size()));
    put32(out, 0x14, 0xff);
    put64(out, 0x18, soff);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        put32(out, 0x28 + g * 16, static_cast<std::uint32_t>(groups[g].idx));
        put32(out, 0x28 + g * 16 + 4, static_cast<std::uint32_t>(groups[g].first));
        put32(out, 0x28 + g * 16 + 8, static_cast<std::uint32_t>(groups[g].last));
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& t = ids.at(order[i]);
        put64(out, soff + i * 8, t ? body + strings[*t] : 0);
    }
    std::copy(pool.begin(), pool.end(), out.begin() + body);
    put32(out, 0x04, static_cast<std::uint32_t>(out.size()));
    return out;
}

Bytes utf16(const std::string& s) {
    Bytes out;
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t c = static_cast<std::uint8_t>(s[i]);
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<std::uint8_t>(s[i]) & 0x3f);
        if (c >= 0x10000) {
            c -= 0x10000;
            app16(out, 0xd800 + (c >> 10));
            app16(out, 0xdc00 + (c & 0x3ff));
        } else {
            app16(out, c);
        }
    }
    return out;
}

struct Message {
    std::string text, help;
};

std::map<std::int32_t, Message> read_messages(const std::string& all) {
    std::map<std::int32_t, Message> rows;
    std::istringstream in(all);
    for (std::string line; std::getline(in, line);) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (line.empty() || (first != std::string::npos && line[first] == '#')) continue;
        std::vector<std::string> parts;
        std::istringstream fields(line);
        for (std::string f; std::getline(fields, f, '\t');) parts.push_back(f);
        if (parts.size() < 2) throw Fail{"messages: a row without text: " + line};
        if (parts[0].empty() || parts[0].size() > 9 || parts[0].find_first_not_of("0123456789") != std::string::npos)
            throw Fail{"messages: not an id: " + line};
        const auto unescape = [](std::string t) {  // "\n" is a line break
            for (std::size_t k = 0; (k = t.find("\\n", k)) != std::string::npos; ++k) t.replace(k, 2, "\n");
            return t;
        };
        const Message m{unescape(parts[1]), parts.size() > 2 ? unescape(parts[2]) : ""};
        if (!rows.emplace(std::atoi(parts[0].c_str()), m).second) throw Fail{"messages: id " + parts[0] + " twice"};
    }
    return rows;
}

// The message bundle: the mod's ids in groups 200 and 201, the dialog groups
// and the event text.
Bytes build_menu_messages(const Bytes& raw, const std::map<std::int32_t, Message>& rows) {
    if (raw.size() < 0x2c || std::memcmp(raw.data(), "DCX\0", 4) != 0 || std::memcmp(raw.data() + 0x28, "DFLT", 4) != 0)
        throw Fail{"the message bundle is not DCX/DFLT"};
    const std::size_t i = find(raw, "DCA\0", 4), dcs = find(raw, "DCS\0", 4);
    if (i == std::string::npos || dcs == std::string::npos) throw Fail{"no DCA or DCS block"};
    const std::size_t from = i + be32(raw, i + 4);
    const Bytes dcx_head(raw.begin(), raw.begin() + from);
    uLongf size = be32(raw, dcs + 4);
    Bytes payload(size);
    if (from > raw.size() || uncompress(payload.data(), &size, raw.data() + from, static_cast<uLong>(raw.size() - from)) != Z_OK ||
        size != payload.size())
        throw Fail{"the message bundle does not inflate"};
    Bnd4 bnd = bnd_read(payload);
    static const std::set<std::uint32_t> kGroups = {kMenuText, kLineHelp, kDialogText, kScreenDialogText, kEventText};
    std::set<std::uint32_t> added;
    for (BndFile& f : bnd.files) {
        if (!kGroups.count(f.id)) continue;
        const int group_kind = f.id == kEventText ? 2 : (f.id == kDialogText || f.id == kScreenDialogText) ? 1 : 0;
        Fmg ids = fmg_read(f.data);
        // The ids are unique, so adding one cannot hide a clash of a later one.
        for (const auto& [mid, m] : rows) {
            if (message_kind(mid) != group_kind) continue;
            if (ids.count(mid)) throw Fail{"message id " + std::to_string(mid) + " is already in group " + std::to_string(f.id)};
            const std::string& s = f.id == kLineHelp ? m.help : m.text;
            ids[mid] = s.empty() ? std::nullopt : std::optional<Bytes>(utf16(s));
        }
        f.data = fmg_write(ids);
        added.insert(f.id);
    }
    if (added != kGroups)
        throw Fail{"the message bundle lacks the menu text, line help, dialog or event text group"};
    const Bytes packed = bnd_pack(bnd);
    uLongf clen = compressBound(static_cast<uLong>(packed.size()));
    Bytes comp(clen);
    if (compress2(comp.data(), &clen, packed.data(), static_cast<uLong>(packed.size()), 9) != Z_OK) throw Fail{"compress2"};
    comp.resize(clen);
    Bytes out = dcx_head;
    putbe32(out, dcs + 4, static_cast<std::uint32_t>(packed.size()));
    putbe32(out, dcs + 8, static_cast<std::uint32_t>(clen));
    app(out, comp);
    return out;
}

// ---------------------------------------------------------------- files

namespace fs = std::filesystem;

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Fail{"cannot read " + p.string()};
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

const char kMovie[] = "menu/optionsetting.gfx";
const char kMessageDir[] = "msg";
const char kMessageFile[] = "menu.msgbnd.dcx";

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <dvdroot_ps4 dir> <messages.tsv> <out mod dir>\n", argv[0]);
        return 2;
    }
    const fs::path dvdroot = argv[1], tsv = argv[2], out = fs::path(argv[3]) / "dvdroot_ps4";
    try {
        const Bytes tsv_text = read_file(tsv);
        const auto rows = read_messages(std::string(tsv_text.begin(), tsv_text.end()));
        if (rows.empty()) throw Fail{tsv.string() + " has no messages"};

        const Bytes movie_src = read_file(dvdroot / kMovie);

        // Every language folder with a menu bundle, in a stable order.
        std::vector<std::string> langs;
        for (const auto& e : fs::directory_iterator(dvdroot / kMessageDir))
            if (e.is_directory() && fs::is_regular_file(e.path() / kMessageFile)) langs.push_back(e.path().filename().string());
        std::sort(langs.begin(), langs.end());
        if (langs.empty()) throw Fail{"the dump has no " + std::string(kMessageDir) + "/*/" + kMessageFile};

        // Everything is made before anything is written, and nothing is
        // written over one of the dump's own files.
        std::vector<std::pair<fs::path, Bytes>> made;
        made.emplace_back(out / kMovie, build_option_movie(movie_src));
        for (const std::string& lang : langs) {
            const fs::path rel = fs::path(kMessageDir) / lang / kMessageFile;
            try {
                made.emplace_back(out / rel, build_menu_messages(read_file(dvdroot / rel), rows));
            } catch (const Fail& f) {
                throw Fail{lang + ": " + f.what()};
            }
        }
        for (const auto& [path, bytes] : made) {
            std::error_code ec;
            const fs::path src = dvdroot / fs::relative(path, out);
            if (fs::exists(path, ec) && fs::equivalent(path, src, ec)) throw Fail{path.string() + " is the dump's own file"};
        }
        for (const auto& [path, bytes] : made) {
            if (!write_file(path, bytes)) throw Fail{"cannot write " + path.string()};
            std::printf("wrote %s (%zu bytes)\n", path.string().c_str(), bytes.size());
        }
        std::printf("%zu language bundles, %zu messages\n", langs.size(), rows.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "coop_assets: %s\n", e.what());
        return 1;
    }
    return 0;
}
