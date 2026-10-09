// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Sotc::Achievements {

enum class UnlockResult { Unlocked, AlreadyUnlocked, UnknownId, IsPlatinum };

/// Records the unlock, shows the notification and returns what happened. `platinum_id` receives
/// the platinum's id when this unlock completes the set, -1 otherwise.
UnlockResult Unlock(s32 id, s32* platinum_id);

void TogglePanel();
bool IsPanelOpen();
/// Draws the panel when open and handles the pad shortcut (L3 + R3); F2 arrives through the
/// hotkey layer. Call once per frame.
void Draw();

} // namespace Sotc::Achievements
