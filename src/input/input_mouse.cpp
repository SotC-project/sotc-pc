// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "common/assert.h"
#include "common/types.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input_mouse.h"

#include <common/singleton.h>
#include <emulator.h>
#include "SDL3/SDL.h"

extern Frontend::WindowSDL* g_window;

namespace Input {

extern std::list<std::pair<InputEvent, bool>> pressed_keys;

int mouse_joystick_binding = 0;
float mouse_deadzone_offset = 0.5, mouse_speed = 1, mouse_speed_offset = 0.1250;
bool mouse_gyro_roll_mode = false;
Uint32 mouse_polling_id = 0;
MouseMode mouse_mode = MouseMode::Off;
// Motion added to the real mouse movement per poll; set by the scripted test driver.
float test_mouse_delta[2] = {0.0f, 0.0f};
bool mouse_invert_x = false, mouse_invert_y = false;

// Switches mouse to a set mode or turns mouse emulation off if it was already in that mode.
// Returns whether the mode is turned on.
bool ToggleMouseModeTo(MouseMode m) {
    if (mouse_mode == m) {
        mouse_mode = MouseMode::Off;
    } else {
        mouse_mode = m;
    }
    return mouse_mode == m;
}

void SetMouseToJoystick(int joystick) {
    mouse_joystick_binding = joystick;
}

void SetMouseInvert(bool x, bool y) {
    mouse_invert_x = x;
    mouse_invert_y = y;
}

void SetMouseParams(float mdo, float ms, float mso) {
    mouse_deadzone_offset = mdo;
    mouse_speed = ms;
    mouse_speed_offset = mso;
}

void SetMouseGyroRollMode(bool mode) {
    mouse_gyro_roll_mode = mode;
}

std::atomic_bool mouse_look_blocked{false};
std::atomic_bool aim_held{false};

void EmulateJoystick(GameController* controller, u32 interval) {

    Axis axis_x, axis_y;
    switch (mouse_joystick_binding) {
    case 1:
        axis_x = Axis::LeftX;
        axis_y = Axis::LeftY;
        break;
    case 2:
        axis_x = Axis::RightX;
        axis_y = Axis::RightY;
        break;
    default:
        return; // no update needed
    }

    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);
    // The scripted test driver gives its motion per 33 ms, whatever the polling interval.
    const float polls = std::max(interval, 1u) / 33.0f;
    d_x += test_mouse_delta[0] * polls;
    d_y += test_mouse_delta[1] * polls;
    if (mouse_invert_x) {
        d_x = -d_x;
    }
    if (mouse_invert_y) {
        d_y = -d_y;
    }

    // The mouse's speed, in counts per 33 ms, evened out over about 25 ms: a mouse reports less
    // often than this is polled, and a stick that fell to rest between two reports made the
    // camera stutter.
    static float speed_x = 0.0f, speed_y = 0.0f;
    static bool mouse_moving = false;
    static bool aim_moving = false;
    const auto rest = [&](Axis x, Axis y) {
        controller->Axis(x, GetAxis(-0x80, 0x7f, 0), false);
        controller->Axis(y, GetAxis(-0x80, 0x7f, 0), false);
    };
    const bool aiming = aim_held.load() && mouse_joystick_binding == 2;
    if (aim_moving && !aiming) {
        rest(Axis::LeftX, Axis::LeftY);
        aim_moving = false;
    }
    if (mouse_look_blocked.load()) {
        // A menu or a cutscene is up: the movement is read and dropped.
        speed_x = speed_y = 0.0f;
        if (mouse_moving) {
            rest(axis_x, axis_y);
            mouse_moving = false;
        }
        if (aim_moving) {
            rest(Axis::LeftX, Axis::LeftY);
            aim_moving = false;
        }
        return;
    }
    const float blend = 1.0f - std::exp(-float(interval) / 25.0f);
    speed_x += (d_x / polls - speed_x) * blend;
    speed_y += (d_y / polls - speed_y) * blend;
    const float speed = std::sqrt(speed_x * speed_x + speed_y * speed_y);

    // Centre the stick once when the mouse stops; writing zero on every poll would also wipe
    // out a real gamepad's stick, which shares the axis.
    if (speed < 0.05f) {
        speed_x = speed_y = 0.0f;
        if (mouse_moving) {
            rest(axis_x, axis_y);
            mouse_moving = false;
        }
        if (aim_moving) {
            rest(Axis::LeftX, Axis::LeftY);
            aim_moving = false;
        }
        return;
    }
    // The stick leaves its dead zone at once and from there follows the mouse's speed, so the
    // view turns as far as the hand moves.
    const float output_speed =
        SDL_clamp(mouse_deadzone_offset * 128 + speed * mouse_speed + mouse_speed_offset * 128,
                  0.0f, 128.0f);
    const float a_x = speed_x / speed * output_speed, a_y = speed_y / speed * output_speed;
    controller->Axis(axis_x, GetAxis(-0x80, 0x7f, a_x), false);
    controller->Axis(axis_y, GetAxis(-0x80, 0x7f, a_y), false);
    mouse_moving = true;
    if (aiming) {
        controller->Axis(Axis::LeftX, GetAxis(-0x80, 0x7f, a_x), false);
        controller->Axis(Axis::LeftY, GetAxis(-0x80, 0x7f, a_y), false);
        aim_moving = true;
    }
}

constexpr float constant_down_accel[3] = {0.0f, 9.81f, 0.0f};
void EmulateGyro(GameController* controller, u32 interval) {
    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);
    controller->UpdateAcceleration(constant_down_accel);
    float gyro_from_mouse[3] = {-d_y / 100, -d_x / 100, 0.0f};
    if (mouse_gyro_roll_mode) {
        gyro_from_mouse[1] = 0.0f;
        gyro_from_mouse[2] = -d_x / 100;
    }
    controller->UpdateGyro(gyro_from_mouse);
}

void EmulateTouchpad(GameController* controller, u32 interval) {
    float x, y;
    SDL_MouseButtonFlags mouse_buttons = SDL_GetMouseState(&x, &y);
    controller->SetTouchpadState(0, (mouse_buttons & SDL_BUTTON_LMASK) != 0,
                                 std::clamp(x / g_window->GetWidth(), 0.0f, 1.0f),
                                 std::clamp(y / g_window->GetHeight(), 0.0f, 1.0f));
    controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad,
                       (mouse_buttons & SDL_BUTTON_RMASK) != 0);
}

void ApplyMouseInputBlockers() {
    switch (mouse_mode) {
    case MouseMode::Touchpad:
        for (auto& k : pressed_keys) {
            if (k.first.input.sdl_id == SDL_BUTTON_LEFT ||
                k.first.input.sdl_id == SDL_BUTTON_RIGHT) {
                k.second = true;
            }
        }
        break;
    default:
        break;
    }
}

Uint32 MousePolling(void* param, Uint32 id, Uint32 interval) {
    auto* controller = (GameController*)param;
    switch (mouse_mode) {
    case MouseMode::Joystick:
        EmulateJoystick(controller, interval);
        break;
    case MouseMode::Gyro:
        EmulateGyro(controller, interval);
        break;
    case MouseMode::Touchpad:
        EmulateTouchpad(controller, interval);
        break;

    default:
        break;
    }
    return interval;
}

} // namespace Input
