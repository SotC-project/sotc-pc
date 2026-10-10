// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/types.h"

/// What the renderer did since the last presented frame. When a frame takes long, these say
/// why: the presenter prints and clears them (see VideoOutDriver::Flip).
namespace Sotc::Hitch {

inline std::atomic<u32> pipelines{0};         ///< pipelines built because the game first used them
inline std::atomic<u32> pipeline_us{0};       ///< time that took, microseconds
inline std::atomic<u32> texture_uploads{0};   ///< textures filled from game memory
inline std::atomic<u32> texture_kb{0};        ///< their size
inline std::atomic<u32> buffer_kb{0};         ///< graphics memory newly allocated for buffers
inline std::atomic<u32> images_freed{0};
inline std::atomic<u32> gpu_busy_us{0};       ///< time the command processor spent working
inline std::atomic<u32> memory_faults{0};     ///< writes and reads caught on tracked game memory
inline std::atomic<u32> file_read_kb{0};      ///< data the game read from its files
inline std::atomic<u32> file_read_us{0};      ///< time those reads took

/// Running totals of the renderer's stage timers, in processor ticks: draws, compute
/// dispatches, buffer binding, texture binding, copies of GPU results back to game memory. They
/// start over every few seconds. Defined beside the timers in the rasterizer.
void RendererTicks(u64 out[5]);

/// Adds the time from its creation to its end to a counter.
struct Timer {
    std::atomic<u32>& counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit Timer(std::atomic<u32>& counter_) : counter{counter_} {}
    ~Timer() {
        counter += u32(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count());
    }
};      ///< images the collector released

} // namespace Sotc::Hitch
