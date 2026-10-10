// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "bbport_settings.h"

namespace UI {

inline int GetCurrentLang() {
    return BbSettings::Get().menu_language.load();
}

inline const char* L(const char* en, const char* pt_br, const char* ru = nullptr) {
    const int lang = GetCurrentLang();
    if (lang == BbSettings::LangPortuguese) {
        return pt_br;
    }
    if (lang == BbSettings::LangRussian && ru) {
        return ru;
    }
    return en;
}

} // namespace UI
