// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include "common/types.h"

union SDL_Event;

namespace BbVirtualKeyboard {

struct Callbacks {
    void (*on_append)(const char* str){};
    void (*on_backspace)(){};
    void (*on_clear)(){};
    void (*on_confirm)(){};
    void (*on_cancel)(){};
};

void SetCallbacks(const Callbacks& cb);
bool HandleGamepadEvent(const SDL_Event& event);
void Render(const std::string& title, const std::string& current_text, int lang, float display_scale);
void Reset();

} // namespace BbVirtualKeyboard
