// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <fmt/format.h>
#include <windows.h>

#include "common/logging/log.h"
#include "common/types.h"

/// Test aid: finds the game code that writes a few bytes of its memory. The page is made
/// read-only; a write there faults, the writer is logged when it touched the watched bytes
/// (code offset and the return addresses on its stack, as eboot offsets), the instruction is
/// stepped over with the page writable, and the page is protected again.
namespace Sotc::Watch {

inline std::atomic<u64> watch_addr{0}, watch_size{0}, image_base{0}, image_size{0};
inline std::atomic<int> hits_left{0};
inline std::atomic<DWORD> page_prot{0};
inline std::atomic<u64> want{0}; // when set: only report writes that leave this value there
inline thread_local bool stepping = false;
inline thread_local std::string pending; // the writer, reported after its write when `want` is set

inline u64 Page(u64 address) {
    return address & ~u64(0xfff);
}

inline void Arm(u64 address, u64 size, u64 base, u64 code_size, int hits, u64 wanted = 0) {
    want = wanted;
    watch_addr = address;
    watch_size = size;
    image_base = base;
    image_size = code_size;
    hits_left = hits;
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(Page(address)), 0x1000, PAGE_READONLY, &old);
    page_prot = old;
    LOG_INFO(Core, "watch armed at {:#x} ({} bytes), page protection was {:#x}", address, size,
             old);
}

/// True when the exception was the watch's and has been dealt with.
inline bool Handle(EXCEPTION_POINTERS* e) {
    const u64 page = Page(watch_addr.load());
    if (!page) {
        return false;
    }
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && stepping) {
        stepping = false;
        if (!pending.empty() &&
            *reinterpret_cast<const u64*>(watch_addr.load()) == want.load() &&
            hits_left.fetch_sub(1) > 0) {
            LOG_INFO(Core, "watch: {}", pending);
        }
        pending.clear();
        DWORD old = 0;
        if (hits_left.load() > 0) {
            VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old);
        }
        return true;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1) {
        return false;
    }
    const u64 target = e->ExceptionRecord->ExceptionInformation[1];
    if (Page(target) != page) {
        return false;
    }
    const u64 base = image_base.load();
    if (target >= watch_addr.load() && target < watch_addr.load() + watch_size.load() &&
        (want.load() != 0 || hits_left.fetch_sub(1) > 0)) {
        std::string frames;
        const auto* stack = reinterpret_cast<const u64*>(e->ContextRecord->Rsp);
        for (int i = 0, found = 0; i < 2048 && found < 24; i++) {
            u64 value = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), stack + i, &value, 8, &got)) {
                break;
            }
            if (value > base && value < base + image_size.load()) {
                frames += fmt::format(" {:x}", value - base);
                found++;
            }
        }
        const std::string line = fmt::format("write to {:#x} by code {:x}, stack{}", target,
                                             e->ContextRecord->Rip - base, frames);
        if (want.load() != 0) {
            pending = line;
        } else {
            LOG_INFO(Core, "watch: {}", line);
        }
    }
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 0x1000, page_prot.load(), &old);
    stepping = true;
    e->ContextRecord->EFlags |= 0x100; // trap after this one instruction
    return true;
}

} // namespace Sotc::Watch
