// SPDX-License-Identifier: GPL-2.0-or-later

// The loading animation played over the black wait of a free play start, see loading.h.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <stb_image_write.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/sotc/hitch.h"
#include "core/sotc/loading.h"
#include "core/sotc/menu.h"
#include "core/sotc/screens.h"
#include "imgui/imgui_texture.h"

namespace Sotc::Loading {

namespace {

constexpr char Magic[8] = {'S', 'O', 'T', 'C', 'L', 'D', '0', '1'};
constexpr u32 FrameMs = 33;      // one recorded frame per this much time
constexpr size_t MaxFrames = 240; // eight seconds
constexpr size_t MinLoop = 24;    // a turn shorter than this is not believed

struct Shot {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

std::mutex mutex;
std::vector<Shot> shots;        // the recording in progress
std::atomic<u64> last_shot_ms{0};
std::atomic_bool recording_done{false};
std::atomic_bool file_ready{false}; // a new file was written; load it

// The animation as loaded: one texture per frame.
std::vector<ImGui::RefCountedTexture> textures;
std::atomic_bool have_animation{false};
bool load_tried = false;

std::filesystem::path FilePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "sotc_loading.bin";
}

/// All the light in the frame: the figure is a few bright pixels on black, so a mean would
/// round to nothing.
u64 Brightness(const Shot& shot) {
    u64 sum = 0;
    for (size_t i = 0; i + 3 < shot.rgba.size(); i += 4) {
        sum += shot.rgba[i] + shot.rgba[i + 1] + shot.rgba[i + 2];
    }
    return sum;
}

u64 Difference(const Shot& a, const Shot& b) {
    u64 sum = 0;
    // Every fourth pixel is plenty to compare two frames.
    for (size_t i = 0; i + 3 < a.rgba.size(); i += 16) {
        sum += static_cast<u64>(std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    }
    return sum;
}

/// Picks one turn of the animation out of the recording and writes it to the user folder.
void Store(std::vector<Shot> taken) {
    // The game fades its animation in and out: a third of a second at each end is left out.
    // (Its brightness swings widely within a turn, so that is no guide to where the fades are.)
    constexpr size_t Fade = 12;
    if (taken.size() < Fade * 2 + MinLoop * 2) {
        LOG_INFO(Core, "Loading animation: recording too short ({} frames), not kept",
                 taken.size());
        return;
    }
    const size_t first = Fade;
    const size_t last = taken.size() - Fade;
    // The frame most like the first one, at least a short turn later, closes the loop.
    size_t close = first + MinLoop;
    u64 best = ~0ULL;
    for (size_t i = first + MinLoop; i < last; i++) {
        const u64 difference = Difference(taken[first], taken[i]);
        if (difference < best) {
            best = difference;
            close = i;
        }
    }
    std::ofstream out{FilePath(), std::ios::binary | std::ios::trunc};
    const u32 count = static_cast<u32>(close - first);
    out.write(Magic, sizeof(Magic));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (size_t i = first; i < close; i++) {
        std::vector<u8> png;
        stbi_write_png_to_func(
            [](void* context, void* data, int size) {
                auto* bytes = static_cast<std::vector<u8>*>(context);
                const u8* from = static_cast<const u8*>(data);
                bytes->insert(bytes->end(), from, from + size);
            },
            &png, static_cast<int>(taken[i].width), static_cast<int>(taken[i].height), 4,
            taken[i].rgba.data(), 0);
        const u32 size = static_cast<u32>(png.size());
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(reinterpret_cast<const char*>(png.data()), size);
    }
    if (out) {
        LOG_INFO(Core, "Loading animation: {} frames kept from a recording of {}", count,
                 taken.size());
        file_ready = true;
    }
}

void Load() {
    std::ifstream in{FilePath(), std::ios::binary};
    char magic[sizeof(Magic)]{};
    u32 count = 0;
    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || std::memcmp(magic, Magic, sizeof(Magic)) != 0 || count == 0 || count > MaxFrames) {
        return;
    }
    std::vector<ImGui::RefCountedTexture> loaded;
    for (u32 i = 0; i < count; i++) {
        u32 size = 0;
        in.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (!in || size == 0 || size > (8u << 20)) {
            return;
        }
        std::vector<u8> png(size);
        in.read(reinterpret_cast<char*>(png.data()), size);
        if (!in) {
            return;
        }
        loaded.push_back(ImGui::RefCountedTexture::DecodePngTexture(std::move(png)));
    }
    textures = std::move(loaded);
    have_animation = true;
}

} // namespace

bool WantsCapture() {
    if (have_animation.load() || recording_done.load() || !Menu::StartCovered(true)) {
        return false;
    }
    // Only frames of the game's loading screen: little drawn, but something.
    const u32 binds = Screens::FrameBinds();
    if (binds < 20 || binds >= 600) {
        return false;
    }
    const u64 now = SDL_GetTicks();
    if (now - last_shot_ms.load() < FrameMs) {
        return false;
    }
    last_shot_ms = now;
    return true;
}

void OnCapture(std::span<const u8> rgba, u32 width, u32 height) {
    std::scoped_lock lock{mutex};
    if (recording_done.load() || shots.size() >= MaxFrames) {
        return;
    }
    Shot shot{width, height, {rgba.begin(), rgba.end()}};
    // Nothing of the figure in it yet: not part of the animation.
    if (Brightness(shot) == 0) {
        return;
    }
    shots.push_back(std::move(shot));
}

namespace {
std::atomic<u64> last_probe_ms{0};
std::atomic<u64> last_light_ms{0}; // when a probed row last held anything but black
} // namespace

bool WantsProbe() {
    // Not while the program is starting up, and not during a free play start, which is covered
    // from end to end anyway.
    const u64 now = SDL_GetTicks();
    if (!have_animation.load() || now < 25000 || Menu::StartCovered(true)) {
        last_light_ms = now;
        return false;
    }
    if (now - last_probe_ms.load() < 100) {
        return false;
    }
    last_probe_ms = now;
    return true;
}

void OnProbe(std::span<const u8> rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i] > 7 || rgba[i + 1] > 7 || rgba[i + 2] > 7) {
            last_light_ms = SDL_GetTicks();
            return;
        }
    }
}

namespace {

/// A load with nothing on screen: black for half a second, and the game busy reading.
bool BlackLoad() {
    static u64 sample_ms = 0;
    static u64 sample_kb = 0;
    static u64 busy_until_ms = 0;
    const u64 now = SDL_GetTicks();
    if (now - sample_ms >= 250) {
        const u64 total = Hitch::file_read_total_kb.load();
        // Two megabytes a second or more: loading, not a cutscene's own trickle of sound.
        if (total - sample_kb >= 512) {
            busy_until_ms = now + 1500;
        }
        sample_ms = now;
        sample_kb = total;
    }
    if (!have_animation.load() || now < 25000) {
        return false;
    }
    const u64 light = last_light_ms.load();
    return now - last_probe_ms.load() < 1000 && now - light >= 500 && now < busy_until_ms;
}

} // namespace

bool HasAnimation() {
    return have_animation.load();
}

void Draw(bool covered) {
    if (!load_tried || file_ready.exchange(false)) {
        load_tried = true;
        Load();
    }
    // The recording ends with the wait; it is sorted out and written off this thread.
    if (!recording_done.load() && !Menu::StartCovered(true)) {
        std::scoped_lock lock{mutex};
        if (!shots.empty()) {
            recording_done = true;
            std::thread{Store, std::move(shots)}.detach();
            shots.clear();
        }
    }

    // The cover comes down at once and lifts over a quarter of a second; the figure fades in
    // and out with it a little more slowly, as the game's own does.
    static float cover = -1.0f;
    static float figure = 0.0f;
    static u64 last_ms = 0;
    const u64 now = SDL_GetTicks();
    const float passed = last_ms == 0 ? 0.0f : static_cast<float>(now - last_ms) / 1000.0f;
    last_ms = now;
    if (cover < 0.0f) {
        cover = covered ? 1.0f : 0.0f;
    }
    // Over a load the game left black there is nothing to hide, only the figure to add; it
    // goes as soon as the picture is back.
    const bool black_load = !covered && BlackLoad();
    if (black_load) {
        covered = true;
    }
    cover = covered ? 1.0f : std::max(0.0f, cover - passed / 0.25f);
    figure = covered ? std::min(1.0f, figure + passed / 0.5f) : std::max(0.0f, figure - passed / 0.25f);
    if (cover <= 0.0f) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({0.0f, 0.0f}, display, IM_COL32(0, 0, 0, static_cast<int>(255.0f * cover)));
    if (textures.empty() || figure <= 0.0f) {
        return;
    }
    const auto& texture = textures[(now / FrameMs) % textures.size()];
    if (!texture) {
        return;
    }
    draw->AddImage(texture.GetTexture().im_id,
                   {display.x * RegionLeft, display.y * RegionTop},
                   {display.x * RegionRight, display.y * RegionBottom}, {0.0f, 0.0f},
                   {1.0f, 1.0f}, IM_COL32(255, 255, 255, static_cast<int>(255.0f * figure)));
}

} // namespace Sotc::Loading
