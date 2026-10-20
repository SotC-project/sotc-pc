// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Colossus vs colossus: the battle's own readout, drawn over the game's picture the way the
// Time Attack readout is (no window): each colossus's name and health bar along the top, and
// a knockout banner naming the winner at the end. It reads the battle's state from the cave
// the CvC helpers keep in the game's image (sdl_window.cpp: cvctarget, cvcbrawl).
namespace Sotc::CvcHud {

void Draw();

} // namespace Sotc::CvcHud
