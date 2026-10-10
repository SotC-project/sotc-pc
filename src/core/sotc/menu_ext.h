// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Input {
class GameController;
}

namespace Sotc::MenuExt {

/// Extra lines under the game's own title menu (Free Play, Achievements, PC Settings) and pause
/// menu (Achievements, PC Settings). The lists are the game's, so the lines are drawn over the
/// picture where the next entries would be, and the cursor is taken over when it would step past
/// the game's last entry. Where the game's cursor is comes from the arrow it draws (see
/// Screens::ArrowAt).

enum class Nav { Up, Down, Confirm, Back };

/// A menu direction or button, from the keyboard or a controller. Returns true when the game
/// must not see it. Called from the input thread.
bool OnNav(Nav nav, bool pressed, Input::GameController* controller);
/// The left stick's vertical value on its way to the game (-128 to 127, down positive); returns
/// the value the game should get.
int FilterLeftY(int value, Input::GameController* controller);
/// A left click at a place in the window (0 to 1 across and down). True when a menu line took it.
bool OnClick(float x, float y);
/// Draws the extra lines and runs the cursor bookkeeping. Call once per frame from the overlay.
void Draw();
/// True while the cursor is on an extra line: the game's own arrow is then drawn blank.
bool HideArrow();

} // namespace Sotc::MenuExt
