// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

// Performance readout for benchmark footage: frame rate (now, average, 1% low), a frame-time
// graph, processor load (whole machine, this program, its busiest thread), graphics load, and
// memory in use. Drawn by the runtime into its own picture, so it is in captured frames and
// needs no outside overlay hooked into the renderer.
//
// Off unless SOTC_PERFHUD is set. SOTC_PERFHUD_LABEL is a line of free text shown on top (scene,
// preset, resolution); SOTC_PERFHUD_NOTE a line shown at the foot.
namespace Sotc::PerfHud {

bool Enabled();
/// One game frame went to the screen, this long after the one before. Any thread.
void OnFrame(double gap_ms);
/// Graphics memory the renderer has allocated, in bytes. Set by the renderer.
inline std::atomic<u64> device_memory{0};
/// Draws the readout. Call once per frame from the overlay.
void Draw();

} // namespace Sotc::PerfHud
