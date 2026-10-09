// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Sotc::Watchdog {

/// Starts the startup watchdog (see watchdog.cpp). SOTC_WATCHDOG_S sets the wait in seconds,
/// 0 turns it off; the default is 120.
void Start();

/// Called from the crash handler: writes the faulting thread's call stack, with names where
/// symbols are available, to crash_stack.txt in the log folder. `context_record` is the
/// CONTEXT of the exception.
void WriteCrashStack(void* context_record, unsigned long long fault_address);

} // namespace Sotc::Watchdog
