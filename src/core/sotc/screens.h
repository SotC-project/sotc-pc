// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"

namespace Sotc::Screens {

/// Called by the texture cache for every texture a draw uses, with the hash of its contents.
/// The game says nothing about which of its screens is up, so that is read from the textures
/// only that screen draws.
void TextureUsed(u64 content_hash);

/// The game's menu arrow was drawn with its left edge at x and its bottom edge at y, in screen
/// units (-1 to 1 across, 1 at the top to -1).
void ArrowAt(float x, float y);
/// Where the arrow was drawn in the last few frames; false when no menu shows one.
bool Arrow(float& x, float& y);
bool Arrow();

/// True while the game's title menu (New Game, Continue, ...) is on screen.
bool TitleMenu();
/// True while the game's pause menu is on screen.
bool PauseMenu();
/// True while the game's Game Over screen (Retry / End) is on screen.
bool GameOver();
/// False when no texture is known for a screen yet, so callers can fall back.
bool KnowsTitleMenu();

/// True once after a draw bound the game's menu arrow texture; the renderer then hands that
/// draw's vertex data to ArrowDraw.
/// Marks the end of a game frame, and how many textures that frame bound: a few for a plain
/// screen, thousands for a scene.
void EndFrame();
u32 FrameBinds();
/// True once after a texture was bound whose draw is to be left out; clears the mark.
bool TakeSkipDraw();
/// Test aid: leave out every draw that uses this texture (0 = none).
void SetSkipProbe(u64 content_hash);
bool TakeArrowDraw();
/// Vertex data of the draw that shows the menu arrow: where on screen the game's cursor is.
/// `viewport` is x scale, x offset, y scale, y offset of the draw's viewport.
void ArrowDraw(const u8* vertices, u32 stride, u32 count, const float viewport[4]);
/// Test runs (SOTC_ARROWPROBE=1): one data buffer of the vertex shader of that draw.
void ArrowProbeBuffer(u32 index, const u8* data, u32 size);
bool ArrowProbeActive();

/// Test runs (SOTC_TEXUSE=1): writes the hashes used since the last call to
/// <user>/texuse_<label>.txt and starts over.
void DumpUsed(const std::string& label);

} // namespace Sotc::Screens
