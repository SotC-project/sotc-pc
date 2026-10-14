// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

union SDL_Event;

namespace Sotc::Menu {

/// Closes the page that is open.
void Toggle();
bool IsOpen();
/// Opens a page from a line of the game's own menus: free play from the title menu, the PC
/// options (graphics, key bindings) from the game's Settings list, where they stand as pages of
/// that list.
void OpenFreePlay();
void OpenGraphics();
void OpenKeyBindings();
/// The question before leaving to the desktop.
void OpenQuit();
/// True while the page is shown in the place of the game's title or pause menu.
bool ReplacesGameMenu();
/// Leaves a free play battle for the title menu: the game is started again without the
/// battle's options.
void LeaveFreePlay();
/// Whether the frame now going to the screen is to be shown black; set by the presenter from
/// the decision made when that frame was drawn.
void SetFrameCovered(bool covered);
bool FrameCovered();
/// 0 to 1: how far the title picture is drawn enlarged from its left side, to put open sky
/// behind a page that stands in for the title menu. Call once per frame.
float SceneShift();
/// Called when the game's own code has faulted and the program is about to close: starts the
/// game again from its beginning instead (the game saves by itself, so little is lost).
/// Not in scripted test runs, and not a third time in a row. True when a restart was begun.
bool RecoverFromCrash();
/// The menu hotkey (F2, L3 + R3): closes a page that is open. The pages are reached from the
/// game's own menus only.
void HotkeyToggle();
/// The game announced the start of play on its console. A free play battle boots into the
/// starting field first and warps to the colossus a moment later, announcing play twice.
void OnGameFlowStart();
/// True from the start of a free play battle's process until the warp to the colossus is under
/// way: the picture is kept black meanwhile, so the starting field and the warp are not seen.
bool StartCovered(bool period_only = false);
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

/// A restart closes the window and opens another a few seconds later. So that the desktop is not
/// seen in between, the closing instance first starts this program as a cover: a black window in
/// the same place that says the game is loading and goes away when the new game window is up.
/// Returns true when this process was started as that cover and has done its work.
bool RunRestartCover();

} // namespace Sotc::Menu
