// SPDX-License-Identifier: GPL-2.0-or-later

// Which of the game's screens is up, see screens.h.

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <mutex>
#include <set>

#include <SDL3/SDL_timer.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/sotc/menu_ext.h"
#include "core/sotc/screens.h"

namespace Sotc::Screens {

namespace {

// Found with SOTC_TEXUSE runs. The first is drawn by every menu of the game (title menu, its
// difficulty list, pause menu) and never in play, subtitles included; the second by the pause
// menu only. So the title menu is "a menu without the pause menu's texture".
constexpr std::array<u64, 1> TitleMenuTextures{0x3e6ebf384eebcdcdULL};
constexpr std::array<u64, 1> PauseMenuTextures{0x53539718d28a642fULL};
// Drawn only by the Game Over screen (the stone mask with Retry and End), found by comparing
// what it draws on several deaths with what battles, the pause menu and the title draw. Both
// must be drawn at once. (A third texture seen at first belongs to the sigil on the mask, which
// is not always lit.)
constexpr std::array<u64, 2> GameOverTextures{
    0x2b552bbe9ab54934ULL,
    0xbb2c6cf34aa4df52ULL,
};

std::atomic<u64> title_seen_ms{0};
std::atomic_bool arrow_in_draw{false};
std::atomic<float> arrow_x{0.0f};
std::atomic<float> arrow_y{0.0f};
std::atomic<u64> arrow_ms{0};
std::atomic<u64> pause_seen_ms{0};
std::array<std::atomic<u64>, GameOverTextures.size()> game_over_seen_ms{};

// Draws that use a texture from this list are left out (see TakeSkipDraw).
std::atomic<u64> skip_probe{0};
std::atomic_bool skip_draw{false};

// Texture binds since the last frame went to the screen, and in the frame before that.
std::atomic<u32> binds_now{0};
std::atomic<u32> binds_last{0};

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
    binds_now.fetch_add(1, std::memory_order_relaxed);
    // The sheet all the lettering of the game's menus is drawn from.
    constexpr u64 MenuLettering = 0xa7e173b9b30fa486ULL;
    if (content_hash == skip_probe.load(std::memory_order_relaxed) ||
        (content_hash == MenuLettering && MenuExt::HideText())) {
        skip_draw.store(true, std::memory_order_relaxed);
    }
    if (Contains(TitleMenuTextures, content_hash)) {
        title_seen_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
        arrow_in_draw.store(true, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < GameOverTextures.size(); i++) {
        if (GameOverTextures[i] == content_hash) {
            game_over_seen_ms[i].store(SDL_GetTicks(), std::memory_order_relaxed);
        }
    }
    if (Contains(PauseMenuTextures, content_hash)) {
        pause_seen_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    }
    static const bool recording = std::getenv("SOTC_TEXUSE") != nullptr;
    if (recording) {
        std::scoped_lock lock{used_mutex};
        used.insert(content_hash);
    }
}

bool TakeSkipDraw() {
    return skip_draw.load(std::memory_order_relaxed) &&
           skip_draw.exchange(false, std::memory_order_relaxed);
}

void SetSkipProbe(u64 content_hash) {
    skip_probe = content_hash;
}

bool TakeArrowDraw() {
    return arrow_in_draw.load(std::memory_order_relaxed) &&
           arrow_in_draw.exchange(false, std::memory_order_relaxed);
}

void ArrowDraw(const u8* vertices, u32 stride, u32 count, const float viewport[4]) {
    // SOTC_ARROWPROBE=1: write the raw vertex data now and then, to learn its layout.
    static const bool probe = std::getenv("SOTC_ARROWPROBE") != nullptr;
    static u64 last_ms = 0;
    if (!probe || SDL_GetTicks() - last_ms < 500) {
        return;
    }
    last_ms = SDL_GetTicks();
    std::string text = fmt::format("arrow draw: stride {} count {} viewport {} {} {} {} |", stride,
                                   count, viewport[0], viewport[1], viewport[2], viewport[3]);
    for (u32 v = 0; v < std::min(count, 4u); v++) {
        text += " [";
        for (u32 offset = 0; offset + 4 <= std::min(stride, 32u); offset += 4) {
            float value;
            std::memcpy(&value, vertices + v * stride + offset, 4);
            text += fmt::format("{:.3f} ", value);
        }
        text += "]";
    }
    LOG_ERROR(Render, "{}", text);
}

void ArrowAt(float x, float y) {
    arrow_x.store(x, std::memory_order_relaxed);
    arrow_y.store(y, std::memory_order_relaxed);
    arrow_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
}

bool Arrow(float& x, float& y) {
    x = arrow_x.load(std::memory_order_relaxed);
    y = arrow_y.load(std::memory_order_relaxed);
    return Recent(arrow_ms);
}

bool Arrow() {
    return Recent(arrow_ms);
}

bool ArrowProbeActive() {
    static const bool probe = std::getenv("SOTC_ARROWPROBE") != nullptr;
    return probe;
}

void ArrowProbeBuffer(u32 index, const u8* data, u32 size) {
    static u64 last_ms[8]{};
    if (index >= 8 || SDL_GetTicks() - last_ms[index] < 500) {
        return;
    }
    last_ms[index] = SDL_GetTicks();
    std::string text = fmt::format("arrow shader buffer {} size {} |", index, size);
    for (u32 offset = 0; offset + 4 <= std::min(size, 128u); offset += 4) {
        float value;
        std::memcpy(&value, data + offset, 4);
        text += fmt::format(" {:.3f}", value);
    }
    LOG_ERROR(Render, "{}", text);
}

bool TitleMenu() {
    return Recent(title_seen_ms) && !Recent(pause_seen_ms);
}

bool GameOver() {
    for (const auto& seen : game_over_seen_ms) {
        if (!Recent(seen)) {
            return false;
        }
    }
    return true;
}

void EndFrame() {
    binds_last.store(binds_now.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
}

u32 FrameBinds() {
    return binds_last.load(std::memory_order_relaxed);
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
