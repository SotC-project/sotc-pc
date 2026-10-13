// SPDX-License-Identifier: GPL-2.0-or-later

// Which of the game's screens is up, see screens.h.

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <map>
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
// The menu cursor. The game has two sets of menu pictures: the full-size one of a normal
// start (80x80) and a half-size one used when it is started straight into a scene, as
// free play battles are (40x40).
// The third is the arrow of the Arabic menus, which point the other way (full-size set).
constexpr std::array<u64, 3> TitleMenuTextures{0x3e6ebf384eebcdcdULL, 0xc697b950114221edULL,
                                               0x8a16d24f1bbf8e8aULL};
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
std::atomic<u64> full_size_ms{0};
std::atomic<u64> skip_probe{0};
std::atomic_bool skip_draw{false};

// Texture binds since the last frame went to the screen, and in the frame before that.
std::atomic<u32> binds_now{0};
std::atomic<u32> binds_last{0};

std::mutex used_mutex;
std::set<u64> used;
std::map<u64, std::pair<u32, u32>> used_sizes;

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

void TextureUsed(u64 content_hash, u32 width, u32 height) {
    binds_now.fetch_add(1, std::memory_order_relaxed);
    // The sheet all the lettering of the game's menus is drawn from.
    // In the half-size set it is known by its contents when first used; in the full-size
    // set it is the one picture of 1920x540, which the game writes all its text into.
    constexpr u64 MenuLettering = 0xa7e173b9b30fa486ULL;
    const bool lettering = content_hash == MenuLettering || (width == 1920 && height == 540);
    if (width == 1920 && height == 540) {
        full_size_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    }
    if (content_hash == skip_probe.load(std::memory_order_relaxed) ||
        (lettering && MenuExt::HideText())) {
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
        used_sizes[content_hash] = {width, height};
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

namespace {
std::atomic<u64> column_ms{0};
std::atomic_bool letterboxed{false};
} // namespace

bool WantsColumn() {
    const u64 now = SDL_GetTicks();
    if (now < 3000 || now - column_ms.load(std::memory_order_relaxed) < 200) {
        return false;
    }
    column_ms.store(now, std::memory_order_relaxed);
    return true;
}

void OnColumn(std::span<const u8> rgba, u32 height) {
    if (height < 100 || rgba.size() < size_t(height) * 4) {
        return;
    }
    const u32 width = static_cast<u32>(rgba.size() / 4 / height);
    const auto lit = [&](u32 from, u32 to) {
        for (u32 y = from; y < to; y++) {
            for (u32 x = 0; x < width; x++) {
                const u8* pixel = rgba.data() + (size_t(y) * width + x) * 4;
                if (pixel[0] > 7 || pixel[1] > 7 || pixel[2] > 7) {
                    return true;
                }
            }
        }
        return false;
    };
    // Bars top and bottom (the game's are about a tenth of the height each), a picture
    // between them.
    letterboxed = !lit(0, height * 6 / 100) && !lit(height * 94 / 100, height) &&
                  lit(height * 20 / 100, height * 80 / 100);
}

bool FullSizeMenus() {
    const u64 seen = full_size_ms.load(std::memory_order_relaxed);
    return seen != 0 && SDL_GetTicks() - seen < 2000;
}

bool Letterboxed() {
    return letterboxed.load(std::memory_order_relaxed);
}

bool PauseMenu() {
    return Recent(pause_seen_ms);
}

bool KnowsTitleMenu() {
    return !TitleMenuTextures.empty();
}

void DumpUsed(const std::string& label) {
    std::set<u64> hashes;
    std::map<u64, std::pair<u32, u32>> sizes;
    {
        std::scoped_lock lock{used_mutex};
        hashes.swap(used);
        sizes = used_sizes;
    }
    if (label == "clear") {
        return;
    }
    std::ofstream out{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                      fmt::format("texuse_{}.txt", label)};
    for (const u64 hash : hashes) {
        out << fmt::format("{:016x} {}x{}\n", hash, sizes[hash].first, sizes[hash].second);
    }
}

} // namespace Sotc::Screens
