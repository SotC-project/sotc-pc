// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <atomic>

#include "SDL3/SDL.h"
#include "common/types.h"

namespace Input {

enum MouseMode {
    Off = 0,
    Joystick,
    Gyro,
    Touchpad,
};

bool ToggleMouseModeTo(MouseMode m);
extern float test_mouse_delta[2];
/// Set while a menu, an overlay panel or a cutscene is on screen: the mouse then turns nothing.
extern std::atomic_bool mouse_look_blocked;
/// A ranged weapon (the bow) is in Wander's hand: the last weapon button pressed in play was
/// d-pad left; right (sword) or up (put away) end it. The game says nothing about it.
extern std::atomic_bool ranged_out;
/// Mouse movement made while the view does not follow it (summed until read), for overlays
/// that point with the mouse in relative mode, like the weapon wheel.
extern std::atomic<float> menu_motion_x, menu_motion_y;
/// Set while the aim button (R1) is held: the mouse then also moves the left stick, which is
/// what the game aims the bow with.
extern std::atomic_bool aim_held;
/// The game's aim button (raise the sword, draw the bow) is Circle instead of R1: the story run
/// with the Legacy button scheme chosen in the game's settings.
/// The game's button scheme the keys follow in this run ("Legacy", "LegacyRemap",
/// "ModernRemap"); empty where the game is in its default scheme.
std::string StoryScheme();
/// The pad button that aims the bow in this run, by its name in the bindings file.
std::string AimButton();
void SetMouseToJoystick(int joystick);
void SetMouseParams(float mouse_deadzone_offset, float mouse_speed, float mouse_speed_offset);
void SetMouseInvert(bool x, bool y);
void SetMouseGyroRollMode(bool mode);

void EmulateJoystick(GameController* controller, u32 interval);
void EmulateGyro(GameController* controller, u32 interval);

void ApplyMouseInputBlockers();

// Polls the mouse for changes
Uint32 MousePolling(void* param, Uint32 id, Uint32 interval);

} // namespace Input
