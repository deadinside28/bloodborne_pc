// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "bbport_strings.h"
#include "ui_manager.h"
#include "ui_strings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;
// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

// Present rate for the FPS counter: original frames vs total presented frames.
std::chrono::steady_clock::time_point last_present_total{};
std::chrono::steady_clock::time_point last_present_orig{};
float total_ms_avg = 0.0f;
float orig_ms_avg = 0.0f;
float frame_ms_avg = 0.0f;
std::chrono::steady_clock::time_point last_generated_time{};

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key, SDL_Scancode scancode) {
    if (key >= SDLK_A && key <= SDLK_Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - SDLK_A));
    }
    if (key >= 'a' && key <= 'z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - 'a'));
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (key - SDLK_0));
    }
    switch (scancode) {
    case SDL_SCANCODE_KP_0: return ImGuiKey_Keypad0;
    case SDL_SCANCODE_KP_1: return ImGuiKey_Keypad1;
    case SDL_SCANCODE_KP_2: return ImGuiKey_Keypad2;
    case SDL_SCANCODE_KP_3: return ImGuiKey_Keypad3;
    case SDL_SCANCODE_KP_4: return ImGuiKey_Keypad4;
    case SDL_SCANCODE_KP_5: return ImGuiKey_Keypad5;
    case SDL_SCANCODE_KP_6: return ImGuiKey_Keypad6;
    case SDL_SCANCODE_KP_7: return ImGuiKey_Keypad7;
    case SDL_SCANCODE_KP_8: return ImGuiKey_Keypad8;
    case SDL_SCANCODE_KP_9: return ImGuiKey_Keypad9;
    case SDL_SCANCODE_KP_PERIOD: return ImGuiKey_KeypadDecimal;
    case SDL_SCANCODE_KP_DIVIDE: return ImGuiKey_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS: return ImGuiKey_KeypadSubtract;
    case SDL_SCANCODE_KP_PLUS: return ImGuiKey_KeypadAdd;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS: return ImGuiKey_KeypadEqual;
    default: break;
    }
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_LGUI: return ImGuiKey_LeftSuper;
    case SDLK_RGUI: return ImGuiKey_RightSuper;
    case SDLK_COMMA: return ImGuiKey_Comma;
    case SDLK_PERIOD: return ImGuiKey_Period;
    case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
    case SDLK_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDLK_PRINTSCREEN: return ImGuiKey_PrintScreen;
    case SDLK_PAUSE: return ImGuiKey_Pause;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

#define S(id) BbStrings::Get(BbStrings::StringId::id, lang)

void RenderLanguageSection(BbSettings::Values& s, int lang) {
    ImGui::SeparatorText(S(SectionLanguage));
    int cur_lang = s.menu_language.load();
    if (ImGui::BeginCombo(S(LanguageInterface), BbSettings::LanguageName(cur_lang))) {
        for (int i = 0; i < BbSettings::LangCount; ++i) {
            if (ImGui::Selectable(BbSettings::LanguageName(i), i == cur_lang)) {
                Store(s.menu_language, i, true);
            }
        }
        ImGui::EndCombo();
    }
}

void RenderUpscalerSection(BbSettings::Values& s, int lang) {
    ImGui::SeparatorText(S(SectionUpscaler));
    std::string fsr411_label = "FSR 4.1.1";
    if (s.fsr411_fp8.load()) {
        fsr411_label += " (FP8 / Float)";
    } else if (s.fsr411_fp8emu.load()) {
        fsr411_label += " (FP8 emulado)";
    } else {
        fsr411_label += " (INT8)";
    }
    const char* upscalers[] = {
        S(UpscalerOff),
        "FSR 3.1",
        "FSR 4 (INT8)",
        fsr411_label.c_str(),
        S(UpscalerTaa),
        "DLSS (NVIDIA RTX)"
    };
    static_assert(sizeof(upscalers) / sizeof(upscalers[0]) == BbSettings::UpscalerCount);
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(S(UpscalerCombo), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss ? s.dlss_supported.load()
                : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", S(UpscalerNotSupported));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", S(UpscalerWorkInProgress));
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && s.upscaler == BbSettings::UpscalerDlss) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
        ImGui::PopTextWrapPos();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s: %s",
                           S(Fsr4Unavailable), problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(S(Fsr4ActiveModeSelected));
        ImGui::PopTextWrapPos();
    }
    if (s.upscaler == BbSettings::UpscalerDlss) {
        Hint(UI::L(
            "NVIDIA DLSS Super Resolution (RTX GPUs; Native AA is DLAA). Needs the DLSS bridge "
            "and NVIDIA's library next to the game; otherwise FSR 3.1 is used.",
            "NVIDIA DLSS Super Resolution (GPUs RTX; Native AA é DLAA). Requer a ponte DLSS "
            "e a biblioteca da NVIDIA junto ao jogo; caso contrário, usa o FSR 3.1.",
            "NVIDIA DLSS Super Resolution (видеокарты RTX; Native AA — это DLAA). Нужны мост DLSS "
            "и библиотека NVIDIA рядом с игрой; без них используется FSR 3.1."));
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            if (s.fsr411_fp8.load()) {
                Hint(S(HintFsr411Fp8));
            } else {
                Hint(S(HintFsr411));
            }
        } else {
            Hint(S(HintFsr4));
        }
        Checkbox(S(Fsr4AutoExposure), s.fsr4_auto_exposure);
        Checkbox(S(Fsr4InvertJitter), s.fsr4_invert_jitter);
        Hint(S(HintFsr4Ghosting));
    }
}

void RenderPresetSection(BbSettings::Values& s, int lang) {
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(S(PresetCombo), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, %s %dx%d)",
                          BbSettings::PresetName(i), scale,
                          S(PresetRenderWord),
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", S(HintTaa));
    }
    ImGui::Text("%s: %d x %d", S(ActiveSceneRender),
                s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("%s: %s", S(StartupPreset), BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(S(HintRenderResAuto));
        } else {
            Hint(S(HintRenderResManual));
        }
    } else {
        Hint(S(HintRenderResLive));
    }
    Checkbox(S(SharpeningRcas), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(S(SharpnessIntensity), s.sharpness, 0.0f, 2.0f);
    Hint(S(HintSharpness));
    ImGui::EndDisabled();
    Checkbox(S(SubpixelJitter), s.jitter);
    Hint(S(HintJitter));
}

void RenderReactivitySection(BbSettings::Values& s, int lang) {
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::SeparatorText(S(SectionReactivity));
    ImGui::BeginDisabled(taa);
    Checkbox(S(ReactivityEnable), s.reactive);
    Hint(S(HintReactivity));
    ImGui::BeginDisabled(!s.reactive);
    Slider(S(ReactivityScale), s.reactive_scale, 0.0f, 4.0f);
    Slider(S(ReactivityThreshold), s.reactive_threshold, 0.0f, 1.0f);
    Slider(S(ReactivityMax), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(S(ReactivityShowMask), &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(S(ObjectMotionVectors), s.object_motion);
    Hint(S(HintObjectMotion));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(S(ObjectMotionShowDebug), &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(S(HintObjectMotionColors));
    ImGui::EndDisabled(); // upscaler off
}

void RenderFrameGenSection(BbSettings::Values& s, int lang) {
    ImGui::SeparatorText(S(SectionFrameGeneration));
    Checkbox(S(FrameGenerationEnable), s.frame_generation);
    Hint(S(HintFrameGeneration));
    if (const char* fg_prob = s.frame_generation_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", fg_prob);
        ImGui::PopTextWrapPos();
    }
}

void RenderResolutionSection(BbSettings::Values& s, int lang) {
    ImGui::SeparatorText(S(SectionOutputResolution));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(S(OutputResolutionCombo), outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(S(HintOutputResolutionFixed));
    } else {
        Hint(S(HintOutputResolutionLive));
    }
    const char* live_modes[] = {
        S(LiveModeAuto),
        S(LiveModeDisabled),
        S(LiveModeEnabled)
    };
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(S(DynamicResolutionChange), live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint(S(HintLiveResolution));
}

void RenderEffectsSection(BbSettings::Values& s, int lang) {
    ImGui::SeparatorText(S(SectionGameEffects));
    const char* lods[] = {
        S(LodMax),
        S(LodDefault),
        S(LodLower),
        S(LodMin)
    };
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo(S(LodCombo), lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        Checkbox(BbSettings::EffectLabel(e, lang), s.effects[e]);
        if (std::string_view(effect.key) == "debug_camera") {
            Hint(UI::L("Hold Cross and press L3 (keyboard: Space + Z).",
                       "Segure Cross e aperte L3 (teclado: Space + Z).",
                       "Удерживайте Cross и нажмите L3 (клавиатура: Space + Z)."));
        } else if (std::string_view(effect.key) == "debug_menu") {
            Hint(UI::L("Left side of the touchpad (Tab); right side: Backspace. Needs the adhoc folder from Nexus mod #253 (the fonts in adhoc/font).",
                       "Lado esquerdo do touchpad (Tab); lado direito: Backspace. Requer a pasta adhoc do mod #253 do Nexus (fontes em adhoc/font).",
                       "Левая сторона тачпада (Tab), правая — Backspace. Нужна папка adhoc из мода Nexus #253 (шрифты в adhoc/font)."));
        }
    }
    Checkbox(S(PuddleReflections), s.puddle_reflections);
    Hint(S(HintPuddleReflections));
    Hint(S(HintGameEffects));
    Hint(S(HintSpecialControls));

    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   (s.startup_draw_pipe == BbSettings::DrawPipeOff && s.draw_pipe != BbSettings::DrawPipeOff) ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", S(RestartWarning));
        if (ImGui::Button(S(ApplyAndRestart))) {
            BbSettings::Save();
            runtime_restart();
        }
    }
}

void RenderPerformanceSection(BbSettings::Values& s, int lang) {
    ImGui::SeparatorText(S(SectionMisc));
    Checkbox(S(ShowFpsCorner), s.show_fps);
    const char* pipe_modes[] = {
        S(DrawPipeModeOff),
        S(DrawPipeModeOn),
        S(DrawPipeModeHybrid)
    };
    int current_pipe = std::clamp(s.draw_pipe.load(), 0, 2);
    if (ImGui::BeginCombo(S(DrawPipe), pipe_modes[current_pipe])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(pipe_modes[i], i == current_pipe)) {
                Store(s.draw_pipe, i, true);
            }
        }
        ImGui::EndCombo();
    }
}

void RenderControlsSection(BbSettings::Values& s) {
    ImGui::Spacing();
    ImGui::SeparatorText(UI::L("Mouse & Controls", "Mouse e Controles", "Мышь и управление"));
    Slider(UI::L("Mouse Sensitivity", "Sensibilidade do Mouse", "Чувствительность мыши"), s.mouse_sensitivity, 0.1f, 5.0f);
    Checkbox(UI::L("Invert Mouse Y", "Inverter Eixo Y do Mouse", "Инвертировать мышь по Y"), s.mouse_invert_y);
    Checkbox(UI::L("Invert Mouse X", "Inverter Eixo X do Mouse", "Инвертировать мышь по X"), s.mouse_invert_x);
    Checkbox(UI::L("Capture Mouse Cursor", "Capturar Cursor no Jogo", "Захватывать курсор в игре"), s.mouse_capture);
    Hint(UI::L("Locks cursor inside the game window during gameplay. Press Insert or F10 to release.",
               "Trava o cursor na janela do jogo durante a partida. Pressione Insert ou F10 para liberar.",
               "Блокирует курсор в окне игры во время игры. Нажмите Insert или F10 для освобождения."));
}

void RenderMemoryToolsSection() {
    ImGui::Spacing();
    ImGui::SeparatorText(UI::L("Memory & Reverse Engineering", "Operações de Memória", "Операции с памятью"));
    const bool mem_open = UI::UiManager::IsMemoryWindowOpen();
    if (mem_open) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.48f, 1.0f));
        if (ImGui::Button(UI::L("Hide Memory Tools Window###btn_memtools",
                                "Ocultar Janela de Memória###btn_memtools",
                                "Скрыть окно памяти###btn_memtools"), ImVec2(-1, 32.0f * base_scale))) {
            UI::UiManager::SetMemoryWindowOpen(false);
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.32f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.42f, 0.54f, 1.0f));
        if (ImGui::Button(UI::L("Open Memory Tools Window (Scanner, Watchlist, Debugger)###btn_memtools",
                                "Abrir Janela de Memória (Scanner, Watchlist, Debugger)###btn_memtools",
                                "Открыть окно памяти (Сканер, Таблица, Отладчик)###btn_memtools"), ImVec2(-1, 32.0f * base_scale))) {
            UI::UiManager::SetMemoryWindowOpen(true);
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::TextDisabled("%s", UI::L("Opens a separate floating window for memory scanning and live debugging",
                                   "Abre uma janela flutuante separada para varredura e depuração de memória",
                                   "Открывает отдельное плавающее окно для сканирования и отладки памяти"));
}

#undef S

void Menu() {
    auto& s = BbSettings::Get();
    const int lang = s.menu_language.load();
    #define S(id) BbStrings::Get(BbStrings::StringId::id, lang)

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where it was moved last (bbport.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(viewport->WorkPos.x + 30.0f * base_scale, viewport->WorkPos.y + 30.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(560.0f * base_scale, 620.0f * base_scale), ImGuiCond_Appearing);
    bool keep_open = true;
    char title[128];
    std::snprintf(title, sizeof(title), "%s  (Insert / L3+R3)###bbport_settings", S(WindowTitle));
    if (!ImGui::Begin(title, &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }
    const float orig_fps = orig_ms_avg > 0.0f ? 1000.0f / orig_ms_avg : 0.0f;
    const float total_fps = total_ms_avg > 0.0f ? 1000.0f / total_ms_avg : orig_fps;
    const float display_ms = total_ms_avg > 0.0f ? total_ms_avg : orig_ms_avg;
    if (s.frame_generation.load()) {
        ImGui::Text("%.0f \\ %.0f FPS  (%.1f %s)", orig_fps, total_fps, display_ms, S(FpsMs));
    } else {
        ImGui::Text("%.0f FPS  (%.1f %s)", total_fps > 0.0f ? total_fps : orig_fps, display_ms, S(FpsMs));
    }

    RenderGraphicsSettings();

    ImGui::Spacing();
    if (ImGui::Button(S(CloseButton))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", S(SavedToIni));
    ImGui::End();
    #undef S
    if (!keep_open) {
        SetOpen(false);
    }

    // Render the separate Memory Operations & Reverse Engineering window
    UI::UiManager::RenderMemoryWindow();
}

void FpsCounter() {
    const auto& s = BbSettings::Get();
    const int lang = s.menu_language.load();

    const float orig_fps = orig_ms_avg > 0.0f ? 1000.0f / orig_ms_avg : 0.0f;
    const float total_fps = total_ms_avg > 0.0f ? 1000.0f / total_ms_avg : orig_fps;
    const float display_ms = total_ms_avg > 0.0f ? total_ms_avg : orig_ms_avg;

    std::string upscaler_label;
    if (s.upscaler == BbSettings::UpscalerFsr3) {
        upscaler_label = "FSR 3.1";
    } else if (s.upscaler == BbSettings::UpscalerFsr4) {
        upscaler_label = "FSR 4";
    } else if (s.upscaler == BbSettings::UpscalerFsr411) {
        upscaler_label = s.fsr411_fp8.load() ? "FSR 4.1.1 (FP8)" : "FSR 4.1.1";
    } else if (s.upscaler == BbSettings::UpscalerTaa) {
        upscaler_label = "TAA";
    } else if (s.upscaler == BbSettings::UpscalerDlss) {
        upscaler_label = "DLSS";
    }

    const bool fg_setting_on = s.frame_generation.load();
    const auto now = std::chrono::steady_clock::now();
    const bool fg_active = fg_setting_on ||
        (last_generated_time.time_since_epoch().count() != 0 &&
         (now - last_generated_time) < std::chrono::milliseconds(1500));

    char text_buf[256];
    if (fg_active) {
        if (!upscaler_label.empty()) {
            upscaler_label += " + FG";
        } else {
            upscaler_label = "FG";
        }
        std::snprintf(text_buf, sizeof(text_buf), "%.0f \\ %.0f FPS  %.1f %s  %s",
                       orig_fps, total_fps,
                       display_ms,
                       BbStrings::Get(BbStrings::StringId::FpsMs, lang),
                       upscaler_label.c_str());
    } else {
        std::snprintf(text_buf, sizeof(text_buf), "%.0f FPS  %.1f %s  %s",
                       total_fps > 0.0f ? total_fps : orig_fps,
                       display_ms,
                       BbStrings::Get(BbStrings::StringId::FpsMs, lang),
                       upscaler_label.c_str());
    }

    const ImVec2 text_size = ImGui::CalcTextSize(text_buf);
    const float h_padding = ImGui::GetStyle().WindowPadding.x * 2.0f + 20.0f * base_scale;
    const float needed_w = text_size.x + h_padding;

    static float max_counter_width = 0.0f;
    static int last_mode_key = -1;
    const int current_mode_key = (s.upscaler.load() & 0xFF) |
                                 (fg_active ? 0x100 : 0) |
                                 (lang << 16) |
                                 (int(base_scale * 100.0f) << 24);
    if (current_mode_key != last_mode_key) {
        last_mode_key = current_mode_key;
        max_counter_width = 0.0f;
    }
    if (needed_w > max_counter_width) {
        max_counter_width = needed_w;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(max_counter_width, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    if (avail_w > text_size.x) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - text_size.x) * 0.5f);
    }
    ImGui::TextUnformatted(text_buf);

    if (fg_active && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s: %.0f FPS | %s: %.0f FPS",
                          UI::L("Original Frames", "Quadros Originais", "Исходные кадры"),
                          orig_fps,
                          UI::L("Total Frames", "Quadros Totais", "Всего кадров"),
                          total_fps);
    }
    ImGui::End();
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const auto& s = BbSettings::Get();
    const int lang = s.menu_language.load();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float display_scale = viewport ? (viewport->WorkSize.y / 1080.0f) : 1.0f;
    BbVirtualKeyboard::Render(title, text, lang, display_scale);
}

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static std::vector<std::string> queue;
    static bool release = false;
    static ImGuiKey held = ImGuiKey_None;
    if (!path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (release) {
        io.AddKeyEvent(held, false);
        release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.empty() && now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        if (FILE* f = std::fopen(path, "r")) {
            char token[32];
            while (std::fscanf(f, "%31s", token) == 1) {
                queue.emplace_back(token);
            }
            std::fclose(f);
            std::remove(path);
        }
    }
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

} // namespace

void RenderGraphicsSettings() {
    auto& s = BbSettings::Get();
    const int lang = s.menu_language.load();

    RenderLanguageSection(s, lang);
    RenderUpscalerSection(s, lang);
    RenderPresetSection(s, lang);
    RenderReactivitySection(s, lang);
    RenderFrameGenSection(s, lang);
    RenderResolutionSection(s, lang);
    RenderEffectsSection(s, lang);
    RenderPerformanceSection(s, lang);
    RenderControlsSection(s);
    RenderMemoryToolsSection();
}

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text,
                   const BbVirtualKeyboard::Callbacks& callbacks) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    if (active && !prompt_active) {
        BbVirtualKeyboard::Reset();
    }
    if (callbacks.on_append || callbacks.on_confirm) {
        BbVirtualKeyboard::SetCallbacks(callbacks);
    }
    prompt_active = active;
}

static const char* OverlayGetClipboardText(ImGuiContext*) {
    static char* clipboard_buf = nullptr;
    if (clipboard_buf) {
        SDL_free(clipboard_buf);
        clipboard_buf = nullptr;
    }
    if (SDL_HasClipboardText()) {
        clipboard_buf = SDL_GetClipboardText();
    }
    return clipboard_buf;
}

static void OverlaySetClipboardText(ImGuiContext*, const char* text) {
    if (text) {
        SDL_SetClipboardText(text);
    }
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_GetClipboardTextFn = OverlayGetClipboardText;
    platform_io.Platform_SetClipboardTextFn = OverlaySetClipboardText;
    io.GetClipboardTextFn = [](void*) -> const char* {
        return OverlayGetClipboardText(nullptr);
    };
    io.SetClipboardTextFn = [](void*, const char* text) {
        OverlaySetClipboardText(nullptr, text);
    };

    ImGui::StyleColorsDark();
    UI::UiManager::InitStyle();

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config,
                                   io.Fonts->GetGlyphRangesCyrillic());

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    const bool is_prompt = prompt_active;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key, event.key.scancode); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (is_prompt && BbVirtualKeyboard::HandleGamepadEvent(event)) {
            return true;
        }
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (is_prompt && BbVirtualKeyboard::HandleGamepadEvent(event)) {
            return true;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open && !is_prompt) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open && !is_prompt) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open && !is_prompt) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
    return false;
}

bool Visible() {
    return initialized && (menu_open || prompt_active || BbSettings::Get().show_fps);
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, bool is_generated) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();

    // Pacing of all presented frames (real + generated)
    if (last_present_total.time_since_epoch().count() != 0) {
        const float ms = std::chrono::duration<float, std::milli>(now - last_present_total).count();
        if (ms > 0.0f && ms < 1000.0f) {
            total_ms_avg = total_ms_avg == 0.0f ? ms : total_ms_avg * 0.95f + ms * 0.05f;
            frame_ms_avg = total_ms_avg;
        }
    }
    last_present_total = now;

    // Pacing of original game frames (real frames only)
    if (is_generated) {
        last_generated_time = now;
    } else {
        if (last_present_orig.time_since_epoch().count() != 0) {
            const float orig_ms = std::chrono::duration<float, std::milli>(now - last_present_orig).count();
            if (orig_ms > 0.0f && orig_ms < 1000.0f) {
                orig_ms_avg = orig_ms_avg == 0.0f ? orig_ms : orig_ms_avg * 0.95f + orig_ms * 0.05f;
            }
        }
        last_present_orig = now;
    }

    if (orig_ms_avg == 0.0f) {
        orig_ms_avg = total_ms_avg;
    }
    if (total_ms_avg == 0.0f) {
        total_ms_avg = orig_ms_avg;
    }

    if (std::getenv("BB_MENU_KEYS_FILE") && initialized) {
        std::scoped_lock lock{imgui_mutex};
        ScriptedKeys();
    }

    UI::UiManager::Tick();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    const float dt_ms = total_ms_avg > 0.0f ? total_ms_avg : 16.666f;
    io.DeltaTime = dt_ms > 0.0f && dt_ms < 1000.0f ? dt_ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
