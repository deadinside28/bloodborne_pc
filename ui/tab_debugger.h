// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace UI {

class TabDebugger {
public:
    static void Render();
    static void SetDisasmAddress(uintptr_t address);
    static void SetBreakpointAddress(uintptr_t address);
};

} // namespace UI
