// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Debug aid: SOTC_PROFILE=1 samples the calling thread's stack once a millisecond and logs the
// hottest call sites inside this executable every 10 seconds. Addresses are symbolized offline:
//   llvm-symbolizer --obj=shadps4.exe -i <addr>...

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <windows.h>
#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore {

inline void StartThreadSampler() {
    static std::atomic_bool started{false};
    if (!std::getenv("SOTC_PROFILE") || started.exchange(true)) {
        return;
    }
    HANDLE target{};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread([target] {
        // Bounds of this executable image, to tell our frames from driver and system frames.
        const auto base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const u64 end = base + nt->OptionalHeader.SizeOfImage;

        using Key = std::array<u64, 3>; // innermost three frames inside this executable
        std::map<Key, u32> counts;
        u32 total = 0, outside = 0;
        auto last = GetTickCount64();
        while (true) {
            Sleep(1);
            if (SuspendThread(target) == DWORD(-1)) {
                return;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            Key key{};
            if (GetThreadContext(target, &ctx)) {
                u32 found = 0;
                bool leaf_inside = ctx.Rip >= base && ctx.Rip < end;
                if (leaf_inside) {
                    key[found++] = ctx.Rip;
                }
                // Scan the stack for return addresses into this image instead of unwinding:
                // RtlVirtualUnwind faults on frames of guest code, which has no unwind data.
                std::array<u64, 768> stack;
                SIZE_T got{};
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp),
                                  stack.data(), sizeof(stack), &got);
                for (size_t i = 0; i < got / 8 && found < key.size(); ++i) {
                    if (stack[i] >= base && stack[i] < end) {
                        key[found++] = stack[i];
                    }
                }
                ++total;
                outside += !leaf_inside;
                ++counts[key];
            }
            ResumeThread(target);

            if (GetTickCount64() - last >= 10000) {
                last = GetTickCount64();
                std::vector<std::pair<Key, u32>> top(counts.begin(), counts.end());
                std::ranges::sort(top, [](auto& a, auto& b) { return a.second > b.second; });
                std::string text;
                for (size_t i = 0; i < std::min<size_t>(top.size(), 45); ++i) {
                    text += fmt::format("\n  {:5.1f}% {:#x} < {:#x} < {:#x}",
                                        100.0 * top[i].second / total, top[i].first[0],
                                        top[i].first[1], top[i].first[2]);
                }
                LOG_ERROR(Render, "profile: {} samples, {:.0f}% with the leaf outside this image{}",
                          total, 100.0 * outside / std::max(total, 1u), text);
                // Full histogram for offline aggregation (overwritten each interval).
                if (FILE* file = std::fopen("profile_full.txt", "w")) {
                    std::fprintf(file, "samples %u outside %u\n", total, outside);
                    for (const auto& [frames, count] : top) {
                        std::fprintf(file, "%u %llx %llx %llx\n", count, frames[0], frames[1],
                                     frames[2]);
                    }
                    std::fclose(file);
                }
                counts.clear();
                total = outside = 0;
            }
        }
    }).detach();
}

} // namespace VideoCore

#else

namespace VideoCore {
inline void StartThreadSampler() {}
} // namespace VideoCore

#endif
