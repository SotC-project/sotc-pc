// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Sotc::FirstRun {

/// On the first start of an install, picks graphics settings for this PC (GPU, memory, display
/// size) and saves them through Settings.ps1. Does nothing unless
/// `new_player` (this install has never been played: no play time and no save). Returns true when
/// settings were written, so the caller reloads them. Call after the settings file exists.
bool ConfigureForThisPc(bool new_player);

/// On the first start of an install: two lines of text on a black screen before the game's own
/// intros, with the game held meanwhile. Call once per frame from the overlay.
void DrawOpeningMessages();
/// True while they are on screen: the overlay must keep drawing although the game is held.
bool OpeningActive();

} // namespace Sotc::FirstRun
