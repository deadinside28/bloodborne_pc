// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_keyboard.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <vector>
#include "bbport_strings.h"
#include "imgui.h"

namespace BbVirtualKeyboard {

namespace {

Callbacks callbacks{};
int selected_row = 1;
int selected_col = 0;
bool caps_lock = true;
uint64_t last_stick_tick = 0;

const std::array<const char*, 12> row0 = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "_"};
const std::array<const char*, 10> row1_upper = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
const std::array<const char*, 10> row1_lower = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
const std::array<const char*, 10> row2_upper = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "'"};
const std::array<const char*, 10> row2_lower = {"a", "s", "d", "f", "g", "h", "j", "k", "l", "'"};
const std::array<const char*, 10> row3_upper = {"Z", "X", "C", "V", "B", "N", "M", ",", ".", "/"};
const std::array<const char*, 10> row3_lower = {"z", "x", "c", "v", "b", "n", "m", ",", ".", "/"};

enum ActionKey {
    ActCaps = 0,
    ActSpace,
    ActBackspace,
    ActClear,
    ActConfirm,
    ActCancel,
    ActCount
};

int RowLength(int r) {
    switch (r) {
    case 0: return 12;
    case 1: return 10;
    case 2: return 10;
    case 3: return 10;
    case 4: return ActCount;
    default: return 1;
    }
}

void TriggerAction(int action) {
    switch (action) {
    case ActCaps: caps_lock = !caps_lock; break;
    case ActSpace: if (callbacks.on_append) callbacks.on_append(" "); break;
    case ActBackspace: if (callbacks.on_backspace) callbacks.on_backspace(); break;
    case ActClear: if (callbacks.on_clear) callbacks.on_clear(); break;
    case ActConfirm: if (callbacks.on_confirm) callbacks.on_confirm(); break;
    case ActCancel: if (callbacks.on_cancel) callbacks.on_cancel(); break;
    }
}

void TriggerSelected() {
    if (selected_row == 0) {
        if (callbacks.on_append) callbacks.on_append(row0[selected_col]);
    } else if (selected_row == 1) {
        if (callbacks.on_append) callbacks.on_append(caps_lock ? row1_upper[selected_col] : row1_lower[selected_col]);
    } else if (selected_row == 2) {
        if (callbacks.on_append) callbacks.on_append(caps_lock ? row2_upper[selected_col] : row2_lower[selected_col]);
    } else if (selected_row == 3) {
        if (callbacks.on_append) callbacks.on_append(caps_lock ? row3_upper[selected_col] : row3_lower[selected_col]);
    } else if (selected_row == 4) {
        TriggerAction(selected_col);
    }
}

void MoveCursor(int d_row, int d_col) {
    if (d_row != 0) {
        selected_row = (selected_row + d_row + 5) % 5;
    }
    const int len = RowLength(selected_row);
    if (d_col != 0) {
        selected_col = (selected_col + d_col + len) % len;
    } else {
        selected_col = std::clamp(selected_col, 0, len - 1);
    }
}

} // namespace

void SetCallbacks(const Callbacks& cb) {
    callbacks = cb;
}

void Reset() {
    selected_row = 1;
    selected_col = 0;
    caps_lock = true;
}

bool HandleGamepadEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (event.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: MoveCursor(-1, 0); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: MoveCursor(1, 0); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: MoveCursor(0, -1); return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: MoveCursor(0, 1); return true;
        case SDL_GAMEPAD_BUTTON_SOUTH: TriggerSelected(); return true;
        case SDL_GAMEPAD_BUTTON_WEST: if (callbacks.on_backspace) callbacks.on_backspace(); return true;
        case SDL_GAMEPAD_BUTTON_NORTH: if (callbacks.on_append) callbacks.on_append(" "); return true;
        case SDL_GAMEPAD_BUTTON_EAST: if (callbacks.on_cancel) callbacks.on_cancel(); return true;
        case SDL_GAMEPAD_BUTTON_START: if (callbacks.on_confirm) callbacks.on_confirm(); return true;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: caps_lock = !caps_lock; return true;
        default: break;
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        constexpr int16_t Deadzone = 18000;
        const uint64_t now = SDL_GetTicks();
        if (now - last_stick_tick > 180) {
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
                if (event.gaxis.value > Deadzone) { MoveCursor(0, 1); last_stick_tick = now; return true; }
                if (event.gaxis.value < -Deadzone) { MoveCursor(0, -1); last_stick_tick = now; return true; }
            } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                if (event.gaxis.value > Deadzone) { MoveCursor(1, 0); last_stick_tick = now; return true; }
                if (event.gaxis.value < -Deadzone) { MoveCursor(-1, 0); last_stick_tick = now; return true; }
            }
        }
    }
    return false;
}

void Render(const std::string& title, const std::string& current_text, int lang, float display_scale) {
    const float scale = std::max(1.0f, display_scale);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.96f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.11f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.50f, 0.40f, 0.20f, 0.85f));

    ImGui::Begin("##virtual_keyboard_dialog", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);

    // Title Header with ornate Bloodborne gold accent
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.82f, 0.45f, 1.0f));
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Input text display box - expands full width of dialog (-1.0f)
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0.05f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.60f, 0.50f, 0.25f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.90f, 0.65f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 8.0f * scale));

    ImGui::SetNextItemWidth(-1.0f);
    std::string display_str = current_text + "_";
    ImGui::InputText("##text_display", display_str.data(), display_str.size() + 1,
                     ImGuiInputTextFlags_ReadOnly);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    const ImVec2 key_size(44.0f * scale, 38.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * scale);

    const auto render_char_row = [&](int r, auto& keys) {
        const float total_w = keys.size() * key_size.x + (keys.size() - 1) * ImGui::GetStyle().ItemSpacing.x;
        const float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > total_w) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - total_w) * 0.5f);
        }
        for (size_t c = 0; c < keys.size(); ++c) {
            if (c > 0) ImGui::SameLine();
            const bool is_sel = (selected_row == r && selected_col == static_cast<int>(c));
            if (is_sel) {
                // Bloodborne luminous golden selected key highlight
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.62f, 0.18f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.72f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.95f, 0.55f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.02f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
            } else {
                // Dark slate button
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.24f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.28f, 0.38f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.35f, 0.40f, 0.60f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (ImGui::Button(keys[c], key_size)) {
                selected_row = r;
                selected_col = static_cast<int>(c);
                if (callbacks.on_append) callbacks.on_append(keys[c]);
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
    };

    render_char_row(0, row0);
    render_char_row(1, caps_lock ? row1_upper : row1_lower);
    render_char_row(2, caps_lock ? row2_upper : row2_lower);
    render_char_row(3, caps_lock ? row3_upper : row3_lower);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Action Keys Row
    const struct ActionDef {
        int act;
        BbStrings::StringId sid;
        ImVec4 base_color;
        ImVec4 hover_color;
    } actions[ActCount] = {
        {ActCaps, BbStrings::StringId::KbCaps,
         caps_lock ? ImVec4(0.20f, 0.42f, 0.68f, 0.95f) : ImVec4(0.18f, 0.20f, 0.26f, 0.90f),
         caps_lock ? ImVec4(0.26f, 0.50f, 0.78f, 1.0f) : ImVec4(0.24f, 0.28f, 0.36f, 1.0f)},
        {ActSpace, BbStrings::StringId::KbSpace,
         ImVec4(0.22f, 0.24f, 0.32f, 0.90f), ImVec4(0.28f, 0.32f, 0.42f, 1.0f)},
        {ActBackspace, BbStrings::StringId::KbBackspace,
         ImVec4(0.40f, 0.20f, 0.20f, 0.90f), ImVec4(0.50f, 0.25f, 0.25f, 1.0f)},
        {ActClear, BbStrings::StringId::KbClear,
         ImVec4(0.32f, 0.18f, 0.18f, 0.90f), ImVec4(0.42f, 0.22f, 0.22f, 1.0f)},
        {ActConfirm, BbStrings::StringId::KbConfirm,
         ImVec4(0.18f, 0.48f, 0.26f, 0.95f), ImVec4(0.24f, 0.58f, 0.32f, 1.0f)},
        {ActCancel, BbStrings::StringId::KbCancel,
         ImVec4(0.45f, 0.18f, 0.18f, 0.95f), ImVec4(0.55f, 0.24f, 0.24f, 1.0f)},
    };

    // Calculate dynamic widths based on actual text size to eliminate any truncation
    std::array<float, ActCount> btn_widths{};
    float total_act_w = 0.0f;
    for (int i = 0; i < ActCount; ++i) {
        const char* label = BbStrings::Get(actions[i].sid, lang);
        const float text_w = ImGui::CalcTextSize(label).x;
        // Space gets a bit more breath, buttons have generous padding
        float pad = (actions[i].act == ActSpace) ? 36.0f * scale : 20.0f * scale;
        float w = std::max(key_size.x * 1.3f, text_w + pad);
        btn_widths[i] = w;
        total_act_w += w;
    }
    total_act_w += (ActCount - 1) * ImGui::GetStyle().ItemSpacing.x;

    const float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w > total_act_w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - total_act_w) * 0.5f);
    }

    for (int i = 0; i < ActCount; ++i) {
        if (i > 0) ImGui::SameLine();
        const auto& a = actions[i];
        const bool is_sel = (selected_row == 4 && selected_col == i);
        const float btn_w = btn_widths[i];

        if (is_sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.20f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.75f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.95f, 0.60f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.02f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, a.base_color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a.hover_color);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.40f, 0.40f, 0.45f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        }

        const char* label = BbStrings::Get(a.sid, lang);
        if (ImGui::Button(label, ImVec2(btn_w, key_size.y))) {
            selected_row = 4;
            selected_col = i;
            TriggerAction(a.act);
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    ImGui::PopStyleVar(); // FrameRounding
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Helper text footer with subtle muted tone
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.65f, 1.0f));
    ImGui::TextUnformatted(BbStrings::Get(BbStrings::StringId::PromptGamepadHelp, lang));
    ImGui::TextUnformatted(BbStrings::Get(BbStrings::StringId::PromptKeyboardHelp, lang));
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleColor(2); // WindowBg, Border
    ImGui::PopStyleVar(2);   // WindowRounding, WindowBorderSize
}

} // namespace BbVirtualKeyboard
