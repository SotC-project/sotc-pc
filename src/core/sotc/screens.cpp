// SPDX-License-Identifier: GPL-2.0-or-later

// Which of the game's screens is up, see screens.h.

#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>

#include <SDL3/SDL_timer.h>
#include <fmt/format.h>

#include "common/path_util.h"
#include "core/sotc/screens.h"

namespace Sotc::Screens {

namespace {

// Textures drawn on that screen and on no other; found with SOTC_TEXUSE runs.
constexpr std::array<u64, 0> TitleMenuTextures{};
constexpr std::array<u64, 3> PauseMenuTextures{0x3e6ebf384eebcdcdULL, 0x53539718d28a642fULL,
                                               0xf87d7fa10c4d64d9ULL};

std::atomic<u64> title_seen_ms{0};
std::atomic<u64> pause_seen_ms{0};

std::mutex used_mutex;
std::set<u64> used;

bool Recent(const std::atomic<u64>& seen_ms) {
    const u64 seen = seen_ms.load(std::memory_order_relaxed);
    return seen != 0 && SDL_GetTicks() - seen < 300;
}

template <size_t N>
bool Contains(const std::array<u64, N>& list, u64 hash) {
    for (const u64 entry : list) {
        if (entry == hash) {
            return true;
        }
    }
    return false;
}

} // namespace

void TextureUsed(u64 content_hash) {
    if (Contains(TitleMenuTextures, content_hash)) {
        title_seen_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    } else if (Contains(PauseMenuTextures, content_hash)) {
        pause_seen_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    }
    static const bool recording = std::getenv("SOTC_TEXUSE") != nullptr;
    if (recording) {
        std::scoped_lock lock{used_mutex};
        used.insert(content_hash);
    }
}

bool TitleMenu() {
    return Recent(title_seen_ms);
}

bool PauseMenu() {
    return Recent(pause_seen_ms);
}

bool KnowsTitleMenu() {
    return !TitleMenuTextures.empty();
}

void DumpUsed(const std::string& label) {
    std::set<u64> hashes;
    {
        std::scoped_lock lock{used_mutex};
        hashes.swap(used);
    }
    if (label == "clear") {
        return;
    }
    std::ofstream out{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                      fmt::format("texuse_{}.txt", label)};
    for (const u64 hash : hashes) {
        out << fmt::format("{:016x}\n", hash);
    }
}

} // namespace Sotc::Screens
