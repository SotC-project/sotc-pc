// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include <SDL3/SDL_timer.h>

#include "common/types.h"

// The last few hundred changes the game made to its address space (map, unmap, commit,
// decommit, release, protect), kept in memory for the crash report: when the game's allocator
// faults over a block that has turned to zeros, this says whether a mapping change touched it.
namespace Sotc::MemTrace {

struct Entry {
    u64 ms;
    u64 address;
    u64 size;
    char kind; // M map, F map file, U unmap, C pool commit, D pool decommit, R release, P protect
};

inline constexpr size_t Capacity = 1024;
inline std::array<Entry, Capacity> entries{};
inline std::atomic<u64> written{0};

inline void Add(char kind, u64 address, u64 size) {
    const u64 slot = written.fetch_add(1, std::memory_order_relaxed);
    entries[slot % Capacity] = {SDL_GetTicks(), address, size, kind};
}

/// Calls `visit` for each entry, oldest first.
template <typename Visit>
void ForEach(Visit&& visit) {
    const u64 end = written.load(std::memory_order_relaxed);
    const u64 begin = end > Capacity ? end - Capacity : 0;
    for (u64 slot = begin; slot < end; slot++) {
        visit(entries[slot % Capacity]);
    }
}

} // namespace Sotc::MemTrace
