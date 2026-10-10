// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include "common/types.h"

namespace Sotc::PrepScreen {

enum class Choice { Prepare, Skip };

/// Identifies this program file (size and time of its last change): text that differs after
/// an update of the game.
std::string ProgramSignature();

/// The shader preparation screen: a panel filling the game window, shown before the renderer
/// draws anything. It is drawn by the system rather than by the renderer, because the renderer
/// is not complete while its saved pipelines load. Does nothing when the window is hidden.
class Screen {
public:
    Screen();
    ~Screen();

    /// Asks whether to prepare the shaders now and waits for the answer. Returns Prepare
    /// without asking when there is no visible window.
    Choice Ask();

    /// Shows the progress bar and keeps the window answering the system. `first_time` adds a
    /// line saying why this start takes longer.
    void Progress(u32 done, u32 total, bool first_time);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Sotc::PrepScreen
