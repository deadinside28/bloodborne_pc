// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui_manager.h"
#include "ui_strings.h"
#include "tab_settings.h"
#include "tab_scanner.h"
#include "tab_watchlist.h"
#include "tab_hexview.h"
#include "tab_debugger.h"
#include "mem_editor.h"
#include "mem_scanner.h"
#include "breakpoint.h"

#include <imgui.h>
#include <chrono>

namespace UI {

static TabId s_requested_tab = TabId::None;
static std::string s_status_msg;
static std::chrono::steady_clock::time_point s_status_time{};
static bool s_memory_window_open = false;

void UiManager::RequestTab(TabId id) {
    s_requested_tab = id;
    s_memory_window_open = true;
}

void UiManager::SetStatus(const std::string& msg) {
    s_status_msg = msg;
    s_status_time = std::chrono::steady_clock::now();
}

bool UiManager::IsMemoryWindowOpen() {
    return s_memory_window_open;
}

void UiManager::SetMemoryWindowOpen(bool open) {
    s_memory_window_open = open;
}

void UiManager::ToggleMemoryWindow() {
    s_memory_window_open = !s_memory_window_open;
}

void UiManager::InitStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    // Rounding & borders
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize  = 1.0f;
    style.PopupBorderSize  = 1.0f;

    // Spacing
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.CellPadding       = ImVec2(7.0f, 5.0f);

    // Color Palette: Sleek Deep Slate with Teal/Cyan Highlights
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.53f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.11f, 0.14f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.13f, 0.17f, 0.60f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.13f, 0.17f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.28f, 0.36f, 0.65f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.15f, 0.17f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.33f, 0.42f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.14f, 0.16f, 0.21f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.09f, 0.10f, 0.13f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.09f, 0.10f, 0.13f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.28f, 0.36f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.38f, 0.48f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.40f, 0.48f, 0.60f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.24f, 0.82f, 0.72f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.24f, 0.82f, 0.72f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.34f, 0.92f, 0.82f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.18f, 0.22f, 0.29f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.32f, 0.42f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.32f, 0.40f, 0.52f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.25f, 0.33f, 0.80f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.35f, 0.46f, 0.90f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.35f, 0.44f, 0.58f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.26f, 0.34f, 0.80f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.30f, 0.37f, 0.48f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.38f, 0.46f, 0.60f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.24f, 0.28f, 0.36f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.34f, 0.40f, 0.52f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.44f, 0.52f, 0.66f, 0.90f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.16f, 0.21f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.28f, 0.34f, 0.44f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.21f, 0.26f, 0.35f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.17f, 0.20f, 0.27f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.16f, 0.19f, 0.25f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.25f, 0.29f, 0.38f, 0.75f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.19f, 0.22f, 0.29f, 0.45f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.11f, 0.12f, 0.16f, 0.55f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(0.14f, 0.16f, 0.21f, 0.55f);
    colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.24f, 0.55f, 0.85f, 0.35f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.24f, 0.82f, 0.72f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.20f, 0.55f, 0.82f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.65f, 0.95f, 1.00f);
}

void UiManager::Tick() {
    Debugger::MemoryEditor::Get().TickFreeze();
}

void UiManager::RenderMemoryWindow() {
    if (!s_memory_window_open) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float scale = 1.0f;
    if (viewport && viewport->WorkSize.y > 0.0f) {
        scale = std::max(viewport->WorkSize.y / 1080.0f, 0.75f);
    }

    ImVec2 default_size(860.0f * scale, 620.0f * scale);
    ImVec2 default_pos(viewport ? (viewport->WorkPos.x + 590.0f * scale) : 590.0f,
                       viewport ? (viewport->WorkPos.y + 30.0f * scale) : 30.0f);
    if (viewport && viewport->WorkSize.x < (590.0f + 860.0f) * scale) {
        default_pos.x = viewport->WorkPos.x + std::max(15.0f * scale, viewport->WorkSize.x - default_size.x - 15.0f * scale);
    }

    ImGui::SetNextWindowPos(default_pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(default_size, ImGuiCond_FirstUseEver);

    bool keep_open = s_memory_window_open;
    char window_title[128];
    std::snprintf(window_title, sizeof(window_title), "%s###bbport_memtools",
                  L("Memory Operations & Reverse Engineering",
                    "Operacoes de Memoria & Engenharia Reversa",
                    "Операции с памятью и реверс-инжиниринг"));

    if (!ImGui::Begin(window_title, &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        s_memory_window_open = keep_open;
        return;
    }
    s_memory_window_open = keep_open;

    auto& scanner = Debugger::MemoryScanner::Get();
    auto& editor = Debugger::MemoryEditor::Get();
    auto& bp = Debugger::BreakpointManager::Get();

    // Top Header info bar inside the Memory Tools window
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.75f, 1.0f), "%s",
                       L("Hunter's Direct Memory Workspace", "Espaco de Memoria do Cacador", "Рабочая область памяти"));
    ImGui::SameLine();
    ImGui::TextDisabled("| %s: 0x1000000000",
                        L("PS4 Direct Heap", "Heap Direto PS4", "Прямая куча PS4"));
    ImGui::Separator();

    // Badges in tab titles (using ###StaticID so tab identity never changes)
    char scanner_title[64];
    if (scanner.IsScanning()) {
        std::snprintf(scanner_title, sizeof(scanner_title), "%s (%.0f%%)###ScannerTab",
                      L("Scanner", "Scanner", "Сканер"), scanner.GetProgress() * 100.0f);
    } else {
        std::snprintf(scanner_title, sizeof(scanner_title), "%s###ScannerTab",
                      L("Scanner", "Scanner", "Сканер"));
    }

    char watchlist_title[64];
    if (editor.GetCount() > 0) {
        std::snprintf(watchlist_title, sizeof(watchlist_title), "%s (%zu)###WatchlistTab",
                      L("Watchlist", "Watchlist", "Таблица"), editor.GetCount());
    } else {
        std::snprintf(watchlist_title, sizeof(watchlist_title), "%s###WatchlistTab",
                      L("Watchlist", "Watchlist", "Таблица"));
    }

    char debugger_title[64];
    if (bp.HasActiveWatchpoint()) {
        std::snprintf(debugger_title, sizeof(debugger_title), "%s (*)###DebuggerTab",
                      L("Debugger", "Depurador", "Отладчик"));
    } else {
        std::snprintf(debugger_title, sizeof(debugger_title), "%s###DebuggerTab",
                      L("Debugger", "Depurador", "Отладчик"));
    }

    if (ImGui::BeginTabBar("MemoryTabBar", ImGuiTabBarFlags_None)) {
        ImGuiTabItemFlags f_scanner = (s_requested_tab == TabId::Scanner) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(scanner_title, nullptr, f_scanner)) {
            TabScanner::Render();
            ImGui::EndTabItem();
        }

        ImGuiTabItemFlags f_watchlist = (s_requested_tab == TabId::Watchlist) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(watchlist_title, nullptr, f_watchlist)) {
            TabWatchlist::Render();
            ImGui::EndTabItem();
        }

        ImGuiTabItemFlags f_hex = (s_requested_tab == TabId::HexView) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(L("Hex Inspector###HexTab", "Inspetor Hex###HexTab", "Hex-инспектор###HexTab"), nullptr, f_hex)) {
            TabHexView::Render();
            ImGui::EndTabItem();
        }

        ImGuiTabItemFlags f_dbg = (s_requested_tab == TabId::Debugger) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(debugger_title, nullptr, f_dbg)) {
            TabDebugger::Render();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    s_requested_tab = TabId::None;

    // Toast status notification bar if active
    if (!s_status_msg.empty()) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_status_time).count();
        if (elapsed < 3500) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.90f, 0.65f, 1.0f));
            ImGui::Text("[OK] %s", s_status_msg.c_str());
            ImGui::PopStyleColor();
        } else {
            s_status_msg.clear();
        }
    }

    ImGui::End();
}

void UiManager::Render() {
    RenderMemoryWindow();
}

} // namespace UI
