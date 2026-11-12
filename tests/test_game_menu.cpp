// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual page definitions and settings bindings without a guest or Vulkan device.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "shim/bbport_game_menu.cpp"

extern "C" void* runtime_low_map(size_t, int) {
    std::abort(); // This test must never install hooks or call guest code.
}
namespace BbGuestHooks {
void ApplyGameEffects() {}
}

int main() {
    using namespace BbGameMenu;
    DefineTexts();
    assert(!pages.empty() && pages.size() <= MaxPages);
    std::array<int, FieldCount> occurrences{};
    for (const Page& page : pages) {
        if (page.rows.size() > page.layout->rows) {
            std::fprintf(stderr, "FAIL: page has %zu controls for %zu layout rows\n",
                         page.rows.size(), page.layout->rows);
            return 1;
        }
        assert(!page.rows.empty());
        for (const Row& row : page.rows) {
            ++occurrences[row.field];
            byte_field[row.field] = row.kind == Kind::Toggle || row.kind == Kind::Slider;
            assert(OurText(row.name) && OurText(row.help));
        }
    }
    // Pagination must keep every display control reachable, with no duplicated bindings.
    for (int field = Aspect; field <= TextureQuality; ++field) {
        assert(occurrences[field] == 1);
    }
    for (int count : occurrences) assert(count <= 1);

    // A row's label and choices remain available in both supported game-menu languages.
    LoadValues(); // The aspect-ratio and resolution choices use dynamic texts.
    for (const auto language : {GameLanguage::English, GameLanguage::Russian}) {
        game_language = language;
        for (const Page& page : pages) {
            assert(*TextOf(page.name) && *TextOf(page.help));
            for (const Row& row : page.rows) {
                assert(*TextOf(row.name) && *TextOf(row.help));
                for (u32 choice : row.choices) assert(*TextOf(choice));
            }
        }
    }

    char path[] = "/tmp/bbport-game-menu-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    auto& settings = BbSettings::Get();
    LoadValues();
    const auto change = [&](Field field, int value) {
        std::array<int, FieldCount> before{}, after{};
        Fill(settings, before.data());
        if (byte_field[field]) bytes[field] = u8(value);
        else values[field] = value;
        Poll();
        Fill(settings, after.data());
        assert(after[field] == value);
        for (int other = 0; other < FieldCount; ++other) {
            if (other != field) assert(after[other] == before[other]);
        }
    };
    for (int value : {BbSettings::TqFull, BbSettings::TqHalf, BbSettings::TqQuarter,
                      BbSettings::TqAuto}) {
        change(TextureQuality, value);
    }
    for (int value : {0, 2, 3, 1}) change(ModelLod, value);
    for (int value : {5, 10, 1}) change(Sharpness, value);
    // Persisted values use the same keys as the launcher and the overlay.
    std::ifstream file(path);
    std::stringstream saved;
    saved << file.rdbuf();
    assert(saved.str().find("texture_quality=auto\n") != std::string::npos);
    assert(saved.str().find("model_lod=0\n") != std::string::npos);
    unlink(path);
    std::puts("PASS: menu page capacity, reachable controls, translations and independent settings");
}
