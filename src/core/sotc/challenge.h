// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include "common/types.h"

namespace Sotc::Challenge {

/// Part of the frame the health bar is read from, as fractions of the game image.
inline constexpr float StripLeft = 0.60f;
inline constexpr float StripRight = 0.96f;
inline constexpr float StripTop = 0.952f;
inline constexpr float StripBottom = 0.978f;

/// Reference data, colossus 1-16.
const char* ColossusName(int colossus);
/// The game's time attack limit in seconds.
double TimeLimit(int colossus, bool hard);
/// True where the limit is an estimate rather than a published value.
bool LimitIsEstimate(int colossus);
/// Estimated time a typical player needs, in seconds.
double AverageTime(int colossus);

/// True when a challenge run was requested at launch (SOTC_CHALLENGE).
bool Enabled();
/// True when the renderer should hand over the health bar strip of this frame.
bool WantsHudStrip();
/// RGBA8 pixels of the strip described by the Strip* constants.
void OnHudStrip(std::span<const u8> rgba, u32 width, u32 height);
/// The game left its title screen and loading; the clock may start on the next input.
void OnGameStarted();
/// The player pressed a key or button, or moved a stick.
void OnPlayerInput();
/// The game reported a trophy; ids 22-37 are the sixteen colossi falling.
void OnTrophy(s32 id);
/// Test hooks: end the run as a win or a death.
void DebugFinish(bool died);
/// Draws the clock while the run is on and the results screen afterwards.
void Draw();

} // namespace Sotc::Challenge
