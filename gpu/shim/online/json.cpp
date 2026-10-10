// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: from bbhost (github.com/droogie/bbhost, GPL-3.0-or-later), unchanged.
#include "online/json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace json {

Value Value::make_object() {
    Value v;
    v.type = Type::Object;
    return v;
}

Value Value::make_array() {
    Value v;
    v.type = Type::Array;
    return v;
}

Value& Value::set(const std::string& key, Value v) {
    if (type != Type::Object) throw std::logic_error("json: set on a non-object");
    for (auto& kv : object) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return kv.second;
        }
    }
    object.emplace_back(key, std::move(v));
    return object.back().second;
}

Value& Value::push(Value v) {
    if (type != Type::Array) throw std::logic_error("json: push on a non-array");
    array.push_back(std::move(v));
    return array.back();
}

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

namespace {

void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void dump_value(std::string& out, const Value& v, int indent, int depth) {
    const auto newline = [&](int d) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * d), ' ');
    };
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: {
        // JSON has no NaN or infinity; exact floats go through f32() bits.
        if (!std::isfinite(v.number)) {
            out += "null";
            break;
        }
        char buf[40];
        if (v.number == std::floor(v.number) && std::fabs(v.number) < 9007199254740992.0) {
            std::snprintf(buf, sizeof(buf), "%.0f", v.number);
        } else {
            std::snprintf(buf, sizeof(buf), "%.17g", v.number);
        }
        out += buf;
        break;
    }
    case Value::Type::String: dump_string(out, v.string); break;
    case Value::Type::Array:
        if (v.array.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            dump_value(out, v.array[i], indent, depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    case Value::Type::Object:
        if (v.object.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < v.object.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            dump_string(out, v.object[i].first);
            out += indent > 0 ? ": " : ":";
            dump_value(out, v.object[i].second, indent, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
}

struct Parser {
    const std::string& s;
    std::size_t pos = 0;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at byte " + std::to_string(pos);
        return false;
    }
    void skip_ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t')) ++pos;
    }
    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (s.compare(pos, n, word) != 0) return fail("invalid literal");
        pos += n;
        return true;
    }
    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    bool hex4(std::uint32_t& out) {
        if (pos + 4 > s.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        if (pos >= s.size() || s[pos] != '"') return fail("expected string");
        ++pos;
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= s.size()) break;
            const char e = s[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xd800 && cp < 0xdc00) {
                    std::uint32_t lo = 0;
                    if (pos + 2 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') return fail("unpaired surrogate");
                    pos += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xdc00 || lo >= 0xe000) return fail("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                }
                utf8(out, cp);
                break;
            }
            default: return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(double& out) {
        const std::size_t start = pos;
        if (pos < s.size() && s[pos] == '-') ++pos;
        const auto digits = [&] {
            const std::size_t d = pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
            return pos > d;
        };
        if (!digits()) return fail("invalid number");
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            if (!digits()) return fail("invalid number");
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            if (!digits()) return fail("invalid number");
        }
        out = std::strtod(s.substr(start, pos - start).c_str(), nullptr);
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skip_ws();
        if (pos >= s.size()) return fail("unexpected end of input");
        const char c = s[pos];
        if (c == '{') {
            ++pos;
            out = Value::make_object();
            skip_ws();
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return true;
            }
            for (;;) {
                skip_ws();
                std::string key;
                if (!string(key)) return false;
                skip_ws();
                if (pos >= s.size() || s[pos] != ':') return fail("expected ':'");
                ++pos;
                Value member;
                if (!value(member, depth + 1)) return false;
                if (out.find(key)) return fail("duplicate member");
                out.object.emplace_back(std::move(key), std::move(member));
                skip_ws();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++pos;
            out = Value::make_array();
            skip_ws();
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            }
            for (;;) {
                Value element;
                if (!value(element, depth + 1)) return false;
                out.array.push_back(std::move(element));
                skip_ws();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out = Value(std::string());
            return string(out.string);
        }
        if (c == 't') {
            out = Value(true);
            return literal("true");
        }
        if (c == 'f') {
            out = Value(false);
            return literal("false");
        }
        if (c == 'n') {
            out = Value();
            return literal("null");
        }
        out = Value(0.0);
        return number(out.number);
    }
};

[[noreturn]] void type_error(const char* key, const char* want) {
    throw std::runtime_error(std::string("manifest member '") + key + "' must be " + want);
}

}  // namespace

std::string dump(const Value& v, int indent) {
    std::string out;
    dump_value(out, v, indent, 0);
    out += '\n';
    return out;
}

bool parse(const std::string& text, Value& out, std::string& error) {
    Parser p{text};
    if (!p.value(out, 0)) {
        error = p.error;
        return false;
    }
    p.skip_ws();
    if (p.pos != text.size()) {
        error = "trailing characters at byte " + std::to_string(p.pos);
        return false;
    }
    return true;
}

std::string hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

bool parse_hex(const std::string& s, std::uint64_t& out) {
    if (s.size() < 3 || s.size() > 18 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    out = 0;
    for (std::size_t i = 2; i < s.size(); ++i) {
        const char c = s[i];
        out <<= 4;
        if (c >= '0' && c <= '9') out |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    return true;
}

Value f32(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Value v = Value::make_object();
    v.set("bits", hex(bits));
    v.set("value", std::isfinite(f) ? Value(static_cast<double>(f)) : Value(std::isnan(f) ? "nan" : f > 0 ? "inf" : "-inf"));
    return v;
}

const Value& member(const Value& obj, const char* key) {
    if (obj.type != Value::Type::Object) throw std::runtime_error(std::string("manifest: expected an object holding '") + key + "'");
    const Value* v = obj.find(key);
    if (!v) throw std::runtime_error(std::string("manifest member '") + key + "' is missing");
    return *v;
}

std::uint64_t as_u64(const Value& v, const char* what) {
    if (v.type == Value::Type::String) {
        std::uint64_t out = 0;
        if (!parse_hex(v.string, out)) type_error(what, "a 0x hex string");
        return out;
    }
    if (v.type != Value::Type::Number || v.number < 0 || v.number != std::floor(v.number) || v.number >= 9007199254740992.0) {
        type_error(what, "a non-negative integer");
    }
    return static_cast<std::uint64_t>(v.number);
}

std::uint64_t u64(const Value& obj, const char* key) { return as_u64(member(obj, key), key); }

std::uint32_t u32(const Value& obj, const char* key) {
    const std::uint64_t v = u64(obj, key);
    if (v > 0xffffffffull) type_error(key, "a 32-bit value");
    return static_cast<std::uint32_t>(v);
}

double num(const Value& obj, const char* key) {
    const Value& v = member(obj, key);
    if (v.type != Value::Type::Number) type_error(key, "a number");
    return v.number;
}

bool flag(const Value& obj, const char* key) {
    const Value& v = member(obj, key);
    if (v.type != Value::Type::Bool) type_error(key, "a boolean");
    return v.boolean;
}

const std::string& str(const Value& obj, const char* key) {
    const Value& v = member(obj, key);
    if (v.type != Value::Type::String) type_error(key, "a string");
    return v.string;
}

const std::vector<Value>& arr(const Value& obj, const char* key) {
    const Value& v = member(obj, key);
    if (v.type != Value::Type::Array) type_error(key, "an array");
    return v.array;
}

float as_f32(const Value& v, const char* what) {
    if (v.type != Value::Type::Object) type_error(what, "an object with float bits");
    const std::uint64_t bits = u64(v, "bits");
    if (bits > 0xffffffffull) type_error(what, "32-bit float bits");
    const std::uint32_t b = static_cast<std::uint32_t>(bits);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

float read_f32(const Value& obj, const char* key) { return as_f32(member(obj, key), key); }

}  // namespace json
