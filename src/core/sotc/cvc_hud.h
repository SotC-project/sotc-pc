// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Colossus vs colossus: the battle's own readout, drawn over the game's picture the way the
// Time Attack readout is (no window): each colossus's name and health bar along the top, and
// a knockout banner naming the winner at the end. It reads the battle's state from the cave
// the CvC helpers keep in the game's image (sdl_window.cpp: cvctarget, cvcbrawl).
namespace Sotc::CvcHud {

void Draw();

/// Basaran's pulse, drawn into the picture (its own tts_pls_* effects are not loaded in the arena
/// the fight is in): a glow gathering at its mouth for `seconds`, then a bolt from its mouth to
/// where it struck, flickering out over `seconds`. World positions.
void Charge(const float* at, float seconds);
void Bolt(const float* from, const float* to, float seconds);

} // namespace Sotc::CvcHud
