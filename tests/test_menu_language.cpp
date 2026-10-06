// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-language-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    assert(s.menu_language == MenuLanguage::Russian);
    assert(std::strcmp(MenuText("English", "Русский"), "Русский") == 0);
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    s.sharpness = 0.3f;
    s.output_res = OutputDefault;
    Load();
    assert(s.menu_language == MenuLanguage::Russian); // older INIs keep the original language
    s.menu_language = MenuLanguage::Russian;
    assert(std::strcmp(MenuText("English", "Русский"), "Русский") == 0);
    Save();
    FILE* config = std::fopen(path, "r");
    assert(config);
    char contents[4096]{};
    const size_t size = std::fread(contents, 1, sizeof(contents) - 1, config);
    assert(size > 0 && std::strstr(contents, "menu_language=ru\n"));
    std::fclose(config);
    s.menu_language = MenuLanguage::English;
    Load();
    assert(s.menu_language == MenuLanguage::Russian);
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(s.sharpness == 0.3f && s.output_res == OutputDefault);
    assert(!ResolutionNeedsRestart());
    s.menu_language = MenuLanguage::English;
    Save();
    s.menu_language = MenuLanguage::Russian;
    Load();
    assert(s.menu_language == MenuLanguage::English);
    config = std::fopen(path, "w");
    assert(config);
    std::fputs("menu_language=invalid\n", config);
    std::fclose(config);
    Load();
    assert(s.menu_language == MenuLanguage::English);
    for (const auto& effect : Effects) {
        assert(effect.label && effect.label[0] && effect.label_ru && effect.label_ru[0]);
        s.menu_language = MenuLanguage::Russian;
        assert(MenuText(effect.label, effect.label_ru) == effect.label_ru);
        s.menu_language = MenuLanguage::English;
        assert(MenuText(effect.label, effect.label_ru) == effect.label);
    }
    std::remove(path);
    std::puts("PASS: menu language switches live, persists and leaves graphics settings unchanged");
}
