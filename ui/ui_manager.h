// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace UI {

enum class TabId {
    None = -1,
    Scanner = 0,
    Watchlist = 1,
    HexView = 2,
    Debugger = 3
};

class UiManager {
public:
    static void InitStyle();
    static void Render();
    static void RenderMemoryWindow();
    static void Tick();

    static void RequestTab(TabId id);
    static void SetStatus(const std::string& msg);

    static bool IsMemoryWindowOpen();
    static void SetMemoryWindowOpen(bool open);
    static void ToggleMemoryWindow();
};

} // namespace UI
