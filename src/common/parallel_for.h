// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/types.h"

namespace Common {

/// A few helper threads that share out pieces of one job with the caller and are otherwise
/// asleep. For work on the renderer's command thread that has no order of its own, such as a
/// copy of several megabytes: that thread is the one the frame rate waits for, while most
/// cores stand idle.
///
/// One job at a time. `Run` returns when every piece is done.
class ParallelFor {
public:
    static ParallelFor& Instance() {
        static ParallelFor pool;
        return pool;
    }

    template <typename Func>
    void Run(size_t pieces, Func&& func) {
        if (pieces <= 1) {
            if (pieces == 1) {
                func(size_t{0});
            }
            return;
        }
        // One job at a time; a second caller waits its turn.
        std::scoped_lock turn{run_mutex};
        struct Call {
            static void Invoke(void* context, size_t index) {
                (*static_cast<Func*>(context))(index);
            }
        };
        {
            std::scoped_lock lock{mutex};
            invoke = &Call::Invoke;
            context = &func;
            count = pieces;
            next.store(0, std::memory_order_relaxed);
            done.store(0, std::memory_order_relaxed);
            ++generation;
        }
        wake.notify_all();
        Work(&Call::Invoke, &func, pieces);
        // The helpers finish within microseconds of the caller; sleeping would cost more.
        while (done.load(std::memory_order_acquire) < pieces) {
            std::this_thread::yield();
        }
        {
            std::scoped_lock lock{mutex};
            context = nullptr;
        }
        // No helper may still hold this job when the next one resets the counters.
        while (active.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    /// Copies `size` bytes, in pieces on several threads when it is large.
    void Copy(void* dest, const void* src, size_t size) {
        static constexpr size_t Piece = 512 * 1024;
        if (size < 2 * Piece) {
            std::memcpy(dest, src, size);
            return;
        }
        Run((size + Piece - 1) / Piece, [&](size_t index) {
            const size_t offset = index * Piece;
            std::memcpy(static_cast<u8*>(dest) + offset, static_cast<const u8*>(src) + offset,
                        std::min(Piece, size - offset));
        });
    }

private:
    using Invoker = void (*)(void*, size_t);

    ParallelFor() {
        const unsigned cores = std::max(std::thread::hardware_concurrency(), 2u);
        const unsigned helpers = std::min(3u, cores - 1);
        for (unsigned i = 0; i < helpers; ++i) {
            std::thread{[this] { Loop(); }}.detach();
        }
    }

    void Work(Invoker call, void* ctx, size_t pieces) {
        for (size_t index = next.fetch_add(1, std::memory_order_relaxed); index < pieces;
             index = next.fetch_add(1, std::memory_order_relaxed)) {
            call(ctx, index);
            done.fetch_add(1, std::memory_order_release);
        }
    }

    void Loop() {
        u64 seen = 0;
        for (;;) {
            Invoker call;
            void* ctx;
            size_t pieces;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [&] { return generation != seen; });
                seen = generation;
                call = invoke;
                ctx = context;
                pieces = count;
                if (ctx) {
                    active.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            // A helper that wakes after the job is over finds every piece taken.
            if (ctx) {
                Work(call, ctx, pieces);
                active.fetch_sub(1, std::memory_order_release);
            }
        }
    }

    std::mutex run_mutex;
    std::mutex mutex;
    std::condition_variable wake;
    Invoker invoke{};
    void* context{};
    size_t count{};
    u64 generation{};
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    std::atomic<u32> active{0};
};

} // namespace Common
