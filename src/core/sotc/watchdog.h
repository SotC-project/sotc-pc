// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Sotc::Watchdog {

/// Starts the startup watchdog (see watchdog.cpp). SOTC_WATCHDOG_S sets the wait in seconds,
/// 0 turns it off; the default is 120.
void Start();

} // namespace Sotc::Watchdog
