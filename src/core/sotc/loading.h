// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"

// The wait before a free play battle. The game restarts for it, and between the restart and the
// battle it shows its loading animation for only part of the time: the rest is black, or the
// starting field and some blue frames that must not be seen. So the port keeps the picture
// black for the whole wait and plays the loading animation over it itself.
//
// The animation is not shipped: it is recorded from the game's own picture on the player's
// machine the first time it plays (the corner of the frame it is drawn in, one turn of it) and
// kept in the user folder. Until that has happened, the game's own animation is let through
// instead and the rest of the wait is plain black.
namespace Sotc::Loading {

/// The part of the game picture the animation is drawn in, as fractions of its width and height.
inline constexpr float RegionLeft = 0.84f;
inline constexpr float RegionTop = 0.62f;
inline constexpr float RegionRight = 1.0f;
inline constexpr float RegionBottom = 0.96f;

/// True when the frame being made should have that region read back for the recording.
/// Called on the GPU thread, once per game frame.
bool WantsCapture();
/// The region of one frame, RGBA.
void OnCapture(std::span<const u8> rgba, u32 width, u32 height);
/// Other loads: the game leaves the picture plain black for a while here and there (going back to
/// the title, a retry, loading a save). Two thin rows of the picture are looked at ten times a
/// second; black for half a second while the game is reading its files counts as a load, and the
/// animation is played over it. A black moment in a cutscene reads no files and is left alone.
inline constexpr float ProbeRows[2] = {0.22f, 0.47f};
/// True when the frame being made should have those rows read back. GPU thread, once per frame.
bool WantsProbe();
/// One of the rows, RGBA.
void OnProbe(std::span<const u8> rgba);
/// True when the recorded animation is at hand, so the whole wait can be covered.
bool HasAnimation();
/// Draws the black cover and the animation, with their fades. `covered` says whether the
/// picture is to be hidden now. Call once per frame from the overlay.
void Draw(bool covered);

} // namespace Sotc::Loading
