// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace UI {

class TabHexView {
public:
    static void Render();
    static void NavigateTo(uintptr_t addr);
};

} // namespace UI
