// SPDX-License-Identifier: MIT
// verify <eboot.elf>: checks every patch's expected bytes and every called or
// hooked function's first bytes in addrs.hpp against the decrypted 1.09 ELF,
// and that each patch's replacement is the same length. Exit 0 when all match.
#include "../addrs.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Segment {
    uint64_t va, offset, size;
};

unsigned char* g_elf;
long g_elf_size;
Segment g_segments[64];
int g_segment_count;

uint64_t u64_at(long at) {
    uint64_t v = 0;
    memcpy(&v, g_elf + at, 8);
    return v;
}

bool load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    g_elf_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_elf = static_cast<unsigned char*>(malloc(static_cast<size_t>(g_elf_size)));
    const bool ok = g_elf && fread(g_elf, 1, static_cast<size_t>(g_elf_size), f) == static_cast<size_t>(g_elf_size);
    fclose(f);
    if (!ok || g_elf_size < 0x40 || memcmp(g_elf, "\x7f" "ELF", 4) != 0) return false;
    const uint64_t phoff = u64_at(0x20);
    uint16_t phentsize = 0, phnum = 0;
    memcpy(&phentsize, g_elf + 0x36, 2);
    memcpy(&phnum, g_elf + 0x38, 2);
    for (int i = 0; i < phnum && g_segment_count < 64; ++i) {
        const long p = static_cast<long>(phoff) + static_cast<long>(i) * phentsize;
        if (p + 0x38 > g_elf_size) return false;
        const uint64_t offset = u64_at(p + 0x08), va = u64_at(p + 0x10), filesz = u64_at(p + 0x20);
        if (filesz) g_segments[g_segment_count++] = {va, offset, filesz};
    }
    return g_segment_count > 0;
}

const unsigned char* at(uint64_t va, size_t n) {
    for (int i = 0; i < g_segment_count; ++i) {
        const Segment& s = g_segments[i];
        if (va >= s.va && va + n <= s.va + s.size && s.offset + (va - s.va) + n <= static_cast<uint64_t>(g_elf_size))
            return g_elf + s.offset + (va - s.va);
    }
    return nullptr;
}

int g_bad;

void hex(const unsigned char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) printf("%02x", b[i]);
}

void check(const char* group, const addrs::Function& f) {
    const unsigned char* have = at(f.va, f.n);
    const bool ok = have && memcmp(have, f.prologue, f.n) == 0;
    if (!ok) ++g_bad;
    printf("%s %-14s va 0x%08llx bn 0x%08llx  ", ok ? "OK " : "BAD", group, static_cast<unsigned long long>(f.va),
           static_cast<unsigned long long>(f.va + addrs::kBnSlide));
    hex(f.prologue, f.n);
    printf("  %s\n", f.what);
}

void check(const char* group, const addrs::Patch& p) {
    const unsigned char* have = at(p.va, p.n);
    const bool ok = have && memcmp(have, p.expect, p.n) == 0;
    if (!ok) ++g_bad;
    printf("%s %-14s va 0x%08llx bn 0x%08llx  ", ok ? "OK " : "BAD", group, static_cast<unsigned long long>(p.va),
           static_cast<unsigned long long>(p.va + addrs::kBnSlide));
    hex(p.expect, p.n);
    printf(" -> ");
    hex(p.bytes, p.n);
    printf("  %s\n", p.what);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !load(argv[1])) {
        fprintf(stderr, "usage: verify <decrypted 1.09 eboot.elf>\n");
        return 2;
    }
    for (const addrs::Patch& p : addrs::kSummonAnywhere) check("summon", p);
    for (const addrs::Patch& p : addrs::kKeepAfterBoss) check("keep_boss", p);
    for (const addrs::Patch& p : addrs::kHelperInsight) check("helper_insight", p);
    for (const addrs::Patch& p : addrs::kLamps) check("lamps", p);
    for (const addrs::Patch& p : addrs::kGuestTalk) check("guest_talk", p);
    for (const addrs::Patch& p : addrs::kSeamless) check("seamless", p);
    for (const addrs::Patch& p : addrs::kGuestPickups) check("guest_pickups", p);
    for (const addrs::Function* f : addrs::kFunctions) check("function", *f);
    for (const addrs::Function* f : addrs::kMenuFunctions) check("menu", *f);
    {
        const unsigned char* call = at(addrs::kSystemFinalizeCall, 5);
        const unsigned char* pad = at(addrs::kTrampolinePad, 16);
        const unsigned char* imm = at(addrs::kSectionCountImm - 3, 4);
        const unsigned char cmp[4] = {0x48, 0x83, 0xFA, 0x06};
        int32_t rel = 0;
        if (call) memcpy(&rel, call + 1, 4);
        const bool ok = call && pad && imm && memcmp(call, addrs::kSystemFinalizeCallBytes, 5) == 0 &&
                        memcmp(pad, addrs::kPadBytes, 16) == 0 && memcmp(imm, cmp, 4) == 0 &&
                        addrs::kSystemFinalizeCall + 5 + rel == addrs::kSystemFinalize.va;
        if (!ok) ++g_bad;
        printf("%s %-14s finalise call 0x%08llx, pad 0x%08llx, section count 0x%08llx\n", ok ? "OK " : "BAD", "menu",
               static_cast<unsigned long long>(addrs::kSystemFinalizeCall),
               static_cast<unsigned long long>(addrs::kTrampolinePad),
               static_cast<unsigned long long>(addrs::kSectionCountImm));
    }
    {
        auto same = [](uint64_t va, const void* want, size_t n) {
            const unsigned char* have = at(va, n);
            return have && memcmp(have, want, n) == 0;
        };
        const bool ok = same(addrs::kFlagGeneralTable, addrs::kFlagGeneral, sizeof addrs::kFlagGeneral) &&
                        same(addrs::kFlagMapTable, addrs::kFlagMapKinds, sizeof addrs::kFlagMapKinds) &&
                        same(addrs::kFlagAreaTable, addrs::kFlagAreas, sizeof addrs::kFlagAreas);
        if (!ok) ++g_bad;
        printf("%s %-14s flag store slot tables 0x%08llx, 0x%08llx, 0x%08llx\n", ok ? "OK " : "BAD", "world",
               static_cast<unsigned long long>(addrs::kFlagGeneralTable), static_cast<unsigned long long>(addrs::kFlagMapTable),
               static_cast<unsigned long long>(addrs::kFlagAreaTable));
    }
    printf("%d mismatch(es)\n", g_bad);
    return g_bad ? 1 : 0;
}
