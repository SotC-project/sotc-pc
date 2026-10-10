// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

union SDL_Event;

namespace Sotc::Menu {

/// Opens or closes the PC menu (F2, L3 + R3): free play, achievements, quit.
void Toggle();
bool IsOpen();
/// Opens the free play page, or the PC settings page, from a line of the game's own menu.
void OpenFreePlay();
void OpenSettings();
/// The menu hotkey (F2, L3 + R3). It only works during a free play battle, where the result
/// screen points to it; everywhere else the menu is reached from the game's menus.
void HotkeyToggle();
/// While the key bindings page waits for a key: takes the event and returns true.
bool CaptureInput(const SDL_Event* event);
/// Draws the menu when open and handles the pad shortcut. Call once per frame.
void Draw();

/// Free play starts the game again with extra engine options (SOTC_FREEPLAY). Given the launch
/// options file the game would read, returns the file to read instead: a copy with the extra
/// options appended, or `original` when no free play run was requested.
std::filesystem::path LaunchOptionsFile(const std::filesystem::path& original);

/// Called first thing in main: when this process was started by a relaunch, waits for the
/// previous one to exit so both never hold the GPU and the save at once.
void WaitForPreviousInstance();

} // namespace Sotc::Menu
