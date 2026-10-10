// SPDX-License-Identifier: GPL-3.0-or-later
// bbport online play: from bbhost (github.com/droogie/bbhost, GPL-3.0-or-later), unchanged.
// Minimal JSON for draw-capture manifests and replay reports. Objects keep
// insertion order. Numbers are doubles, so 64-bit values (addresses, hashes)
// travel as "0x..." strings and floats that must round-trip exactly travel as
// their IEEE bit patterns (f32 / read_f32).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type(Type::Bool), boolean(b) {}
    Value(double d) : type(Type::Number), number(d) {}
    Value(float d) : type(Type::Number), number(d) {}
    Value(int v) : type(Type::Number), number(v) {}
    Value(unsigned v) : type(Type::Number), number(v) {}
    Value(long v) : type(Type::Number), number(static_cast<double>(v)) {}
    Value(unsigned long v) : type(Type::Number), number(static_cast<double>(v)) {}
    Value(long long v) : type(Type::Number), number(static_cast<double>(v)) {}
    Value(unsigned long long v) : type(Type::Number), number(static_cast<double>(v)) {}
    Value(const char* s) : type(Type::String), string(s) {}
    Value(std::string s) : type(Type::String), string(std::move(s)) {}

    static Value make_object();
    static Value make_array();
    // Adds or replaces a member (objects only).
    Value& set(const std::string& key, Value v);
    Value& push(Value v);
    const Value* find(const std::string& key) const;
};

std::string dump(const Value& v, int indent = 2);
bool parse(const std::string& text, Value& out, std::string& error);

std::string hex(std::uint64_t v);
bool parse_hex(const std::string& s, std::uint64_t& out);
// {"bits": "0x3f800000", "value": 1}: the bits are authoritative.
Value f32(float f);

// Typed reads that throw std::runtime_error naming the member.
const Value& member(const Value& obj, const char* key);
std::uint64_t as_u64(const Value& v, const char* what);  // exact integer number or hex string
std::uint64_t u64(const Value& obj, const char* key);
std::uint32_t u32(const Value& obj, const char* key);
double num(const Value& obj, const char* key);
bool flag(const Value& obj, const char* key);
const std::string& str(const Value& obj, const char* key);
const std::vector<Value>& arr(const Value& obj, const char* key);
float as_f32(const Value& v, const char* what);        // an f32() object
float read_f32(const Value& obj, const char* key);     // the f32() object held in obj[key]

}  // namespace json
