// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// On-screen touch controls: a floating stick on the left, a camera stick on the right, the
// DualShock 4's buttons drawn over the game. The window feeds finger events in, the overlay draws
// them, runtime_pad.c reads them as pad input (bbgpu_touch_pad).
#pragma once

#include <cstdint>

union SDL_Event;

namespace BbTouch {

/// BB_TOUCH_CONTROLS: on by default on Android.
bool Enabled();
/// A finger event; true when it was taken. Mouse events too while the controls are in use (taps
/// come also as mouse clicks; they must not take the mouse for mouse look).
bool HandleEvent(const SDL_Event& event);
/// Shown now: enabled, no gamepad in use, the port's menu closed.
bool Shown();
/// Draws them (on ImGui's foreground list, inside the overlay's frame).
void Draw(float width, float height);
/// The pad state they make: DS4 button bits (runtime_pad.c's BTN_*), the sticks (128 centre).
/// `gamepad`: a gamepad is connected (the controls then hide and read nothing).
bool Read(bool gamepad, std::uint32_t& buttons, std::uint8_t sticks[4]);

} // namespace BbTouch
