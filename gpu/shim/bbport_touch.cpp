// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_touch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#include "imgui.h"

#include "shim/bb_env.h"
#include "bbport_overlay.h"

namespace BbTouch {
namespace {

// runtime_pad.c's DualShock 4 button bits
enum : std::uint32_t {
    L3 = 0x2, R3 = 0x4, OPTIONS = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
    L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800, TRIANGLE = 0x1000, CIRCLE = 0x2000,
    CROSS = 0x4000, SQUARE = 0x8000, TOUCHPAD = 0x100000,
    MENU = 0x80000000u, // not the game's: opens the port's menu
};

enum class Shape { Circle, Pill, Triangle, Ring, Cross, Square, Arrow };

/// A button, placed in units of the screen's height (x from 0 to the aspect, y 0 to 1); a
/// negative x counts from the right edge.
struct Button {
    const char* label;
    std::uint32_t bit;
    float x, y, r;
    Shape shape;
    float angle = 0.0f; // arrows: the direction (radians, 0 = right)
};

constexpr float Face = 0.068f, Shoulder = 0.062f, Small = 0.045f;
constexpr float FaceX = -0.27f, FaceY = 0.70f, FaceGap = 0.135f;
constexpr float PadX = 0.20f, PadY = 0.27f, PadGap = 0.085f;
constexpr float Pi = 3.14159265f;

const std::array<Button, 17> buttons{{
    {"", TRIANGLE, FaceX, FaceY - FaceGap, Face, Shape::Triangle},
    {"", CIRCLE, FaceX + FaceGap, FaceY, Face, Shape::Ring},
    {"", CROSS, FaceX, FaceY + FaceGap, Face, Shape::Cross},
    {"", SQUARE, FaceX - FaceGap, FaceY, Face, Shape::Square},
    {"R1", R1, -0.11f, 0.42f, Shoulder, Shape::Circle},
    {"R2", R2, -0.11f, 0.24f, Shoulder, Shape::Circle},
    {"L1", L1, -0.29f, 0.42f, Shoulder, Shape::Circle},
    {"L2", L2, -0.29f, 0.24f, Shoulder, Shape::Circle},
    {"R3", R3, FaceX - FaceGap * 1.9f, FaceY - FaceGap * 1.1f, Small, Shape::Circle},
    {"L3", L3, 0.08f, 0.10f, Small, Shape::Circle},
    {"", UP, PadX, PadY - PadGap, Small, Shape::Arrow, -Pi / 2},
    {"", DOWN, PadX, PadY + PadGap, Small, Shape::Arrow, Pi / 2},
    {"", LEFT, PadX - PadGap, PadY, Small, Shape::Arrow, Pi},
    {"", RIGHT, PadX + PadGap, PadY, Small, Shape::Arrow, 0.0f},
    {"Touch", TOUCHPAD, 0.0f, 0.075f, Small, Shape::Pill}, // x: centre - 0.12 (Place)
    {"Options", OPTIONS, 0.0f, 0.075f, Small, Shape::Pill}, // x: centre + 0.12
    {"Menu", MENU, -0.08f, 0.075f, Small, Shape::Pill},
}};

/// The button's centre in height units for an aspect `a`.
float PlaceX(const Button& b, float a) {
    if (b.bit == TOUCHPAD) return a * 0.5f - 0.12f;
    if (b.bit == OPTIONS) return a * 0.5f + 0.12f;
    return b.x < 0.0f ? a + b.x : b.x;
}

constexpr float StickRadius = 0.13f;
/// Left of this share of the width a finger moves; right of it, the camera.
constexpr float LeftShare = 0.45f;

enum class Role { None, Button, LeftStick, RightStick };

struct Finger {
    SDL_FingerID id = 0;
    Role role = Role::None;
    std::uint32_t bit = 0;
    float ox = 0, oy = 0, x = 0, y = 0; // height units
};

std::mutex mutex;
std::array<Finger, 10> fingers{};
std::atomic<bool> gamepad{false};
float aspect = 16.0f / 9.0f;

/// The stick's value from a finger: origin to finger over the radius, the origin following
/// a finger that goes further (turning back answers at once).
void Follow(Finger& f) {
    const float dx = f.x - f.ox, dy = f.y - f.oy;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > StickRadius) {
        f.ox = f.x - dx * StickRadius / len;
        f.oy = f.y - dy * StickRadius / len;
    }
}

std::uint8_t Axis(float v) {
    const float c = std::clamp(v / StickRadius, -1.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(127.5f + c * 127.5f));
}

const Button* HitButton(float x, float y) {
    const Button* best = nullptr;
    float best_d = 0.0f;
    for (const Button& b : buttons) {
        if (!b.bit) continue;
        const float dx = x - PlaceX(b, aspect), dy = y - b.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float reach = b.r * (b.shape == Shape::Pill ? 1.6f : 1.3f);
        if (d < reach && (!best || d < best_d)) {
            best = &b;
            best_d = d;
        }
    }
    return best;
}

void ReleaseAll() {
    for (Finger& f : fingers) f = {};
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* env = bb_env("BB_TOUCH_CONTROLS");
        if (env && *env) return env[0] == '1';
#ifdef __ANDROID__
        return true;
#else
        return false;
#endif
    }();
    return on;
}

bool Shown() {
    return Enabled() && !gamepad.load(std::memory_order_relaxed) && !BbOverlay::MenuOpen();
}

bool HandleEvent(const SDL_Event& event) {
    if (!Enabled()) return false;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // the menu takes taps as clicks; otherwise a tap is the controls'
        return Shown();
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        break;
    default:
        return false;
    }
    const SDL_TouchFingerEvent& t = event.tfinger;
    std::scoped_lock lock{mutex};
    if (SDL_Window* window = SDL_GetWindowFromID(t.windowID)) {
        int w = 0, h = 0;
        if (SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0) {
            aspect = float(w) / float(h);
        }
    }
    if (!Shown()) {
        ReleaseAll();
        return false;
    }
    const float x = t.x * aspect, y = t.y;
    auto it = std::find_if(fingers.begin(), fingers.end(),
                           [&](const Finger& f) { return f.role != Role::None && f.id == t.fingerID; });
    if (event.type == SDL_EVENT_FINGER_DOWN) {
        if (it == fingers.end()) {
            it = std::find_if(fingers.begin(), fingers.end(), [](const Finger& f) { return f.role == Role::None; });
        }
        if (it == fingers.end()) return true;
        Finger f{.id = t.fingerID, .ox = x, .oy = y, .x = x, .y = y};
        if (const Button* b = HitButton(x, y)) {
            f.role = Role::Button;
            f.bit = b->bit;
        } else {
            f.role = x < aspect * LeftShare ? Role::LeftStick : Role::RightStick;
        }
        // one finger per stick: a second one there takes it over
        if (f.role != Role::Button) {
            for (Finger& other : fingers) {
                if (other.role == f.role) other = {};
            }
        }
        if (f.bit == MENU) {
            BbOverlay::RequestOpen();
            return true;
        }
        *it = f;
        return true;
    }
    if (it == fingers.end()) return true;
    if (event.type == SDL_EVENT_FINGER_MOTION) {
        it->x = x;
        it->y = y;
        if (it->role == Role::LeftStick || it->role == Role::RightStick) Follow(*it);
        return true;
    }
    *it = {}; // up or cancelled
    return true;
}

bool Read(bool has_gamepad, std::uint32_t& out_buttons, std::uint8_t sticks[4]) {
    gamepad.store(has_gamepad, std::memory_order_relaxed);
    out_buttons = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 128;
    if (!Shown()) return false;
    std::scoped_lock lock{mutex};
    bool any = false;
    for (const Finger& f : fingers) {
        switch (f.role) {
        case Role::Button:
            out_buttons |= f.bit;
            any = true;
            break;
        case Role::LeftStick:
        case Role::RightStick: {
            const int i = f.role == Role::LeftStick ? 0 : 2;
            sticks[i] = Axis(f.x - f.ox);
            sticks[i + 1] = Axis(f.y - f.oy);
            any = true;
            break;
        }
        default:
            break;
        }
    }
    return any;
}

void Draw(float width, float height) {
    if (!Shown() || height <= 0.0f) return;
    std::array<Finger, 10> now;
    float a;
    {
        std::scoped_lock lock{mutex};
        now = fingers;
        a = aspect;
    }
    // the drawn image may not be the window's shape (the overlay draws at the swapchain's size)
    const float sx = width / a, sy = height;
    auto at = [&](float x, float y) { return ImVec2{x * sx, y * sy}; };
    std::uint32_t held = 0;
    for (const Finger& f : now) {
        if (f.role == Role::Button) held |= f.bit;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 line = IM_COL32(235, 230, 215, 150), fill = IM_COL32(20, 20, 20, 70),
                pressed = IM_COL32(235, 230, 215, 110);
    const float thick = std::max(1.5f, sy * 0.004f);
    for (const Button& b : buttons) {
        if (!b.bit) continue;
        const ImVec2 c = at(PlaceX(b, a), b.y);
        const float r = b.r * sy;
        const bool down = (held & b.bit) != 0;
        if (b.shape == Shape::Pill) {
            const float hw = r * 1.6f, hh = r * 0.7f;
            draw->AddRectFilled({c.x - hw, c.y - hh}, {c.x + hw, c.y + hh}, down ? pressed : fill, hh);
            draw->AddRect({c.x - hw, c.y - hh}, {c.x + hw, c.y + hh}, line, hh, 0, thick);
        } else {
            draw->AddCircleFilled(c, r, down ? pressed : fill);
            draw->AddCircle(c, r, line, 0, thick);
        }
        const float s = r * 0.45f;
        switch (b.shape) {
        case Shape::Triangle:
            draw->AddTriangle({c.x, c.y - s}, {c.x + s * 0.95f, c.y + s * 0.7f}, {c.x - s * 0.95f, c.y + s * 0.7f},
                              line, thick * 1.3f);
            break;
        case Shape::Ring:
            draw->AddCircle(c, s, line, 0, thick * 1.3f);
            break;
        case Shape::Cross:
            draw->AddLine({c.x - s, c.y - s}, {c.x + s, c.y + s}, line, thick * 1.3f);
            draw->AddLine({c.x - s, c.y + s}, {c.x + s, c.y - s}, line, thick * 1.3f);
            break;
        case Shape::Square:
            draw->AddRect({c.x - s * 0.85f, c.y - s * 0.85f}, {c.x + s * 0.85f, c.y + s * 0.85f}, line, 0.0f, 0,
                          thick * 1.3f);
            break;
        case Shape::Arrow: {
            const float ca = std::cos(b.angle), sa = std::sin(b.angle);
            auto p = [&](float u, float v) { return ImVec2{c.x + (u * ca - v * sa) * s, c.y + (u * sa + v * ca) * s}; };
            draw->AddTriangleFilled(p(0.9f, 0.0f), p(-0.6f, 0.8f), p(-0.6f, -0.8f), line);
            break;
        }
        default: {
            const float size = b.shape == Shape::Pill ? r * 0.75f : r * 0.8f;
            const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.0f, b.label);
            draw->AddText(font, size, {c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, line, b.label);
            break;
        }
        }
    }
    // the sticks: where a finger holds one, else a faint hint where the left one is usually used
    bool left = false;
    for (const Finger& f : now) {
        if (f.role != Role::LeftStick && f.role != Role::RightStick) continue;
        left |= f.role == Role::LeftStick;
        draw->AddCircleFilled(at(f.ox, f.oy), StickRadius * sy, fill);
        draw->AddCircle(at(f.ox, f.oy), StickRadius * sy, line, 0, thick);
        draw->AddCircleFilled(at(f.x, f.y), StickRadius * sy * 0.45f, pressed);
    }
    if (!left) {
        draw->AddCircle(at(0.32f, 0.70f), StickRadius * sy, IM_COL32(235, 230, 215, 60), 0, thick);
        draw->AddCircleFilled(at(0.32f, 0.70f), StickRadius * sy * 0.45f, IM_COL32(235, 230, 215, 40));
    }
}

} // namespace BbTouch

extern "C" int bbgpu_touch_pad(int gamepad, std::uint32_t* buttons, std::uint8_t* sticks) {
    std::uint32_t b = 0;
    const bool any = BbTouch::Read(gamepad != 0, b, sticks);
    *buttons = b;
    return any ? 2 : BbTouch::Shown() ? 1 : 0;
}
