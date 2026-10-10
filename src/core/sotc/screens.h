// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"

namespace Sotc::Screens {

/// Called by the texture cache for every texture a draw uses, with the hash of its contents.
/// The game says nothing about which of its screens is up, so that is read from the textures
/// only that screen draws.
void TextureUsed(u64 content_hash);

/// True while the game's title menu (New Game, Continue, ...) is on screen.
bool TitleMenu();
/// True while the game's pause menu is on screen.
bool PauseMenu();
/// False when no texture is known for a screen yet, so callers can fall back.
bool KnowsTitleMenu();

/// Test runs (SOTC_TEXUSE=1): writes the hashes used since the last call to
/// <user>/texuse_<label>.txt and starts over.
void DumpUsed(const std::string& label);

} // namespace Sotc::Screens
