// SPDX-License-Identifier: GPL-2.0-or-later

// Extra lines in the game's own menus, see menu_ext.h.

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/pad/pad.h"
#include "core/sotc/achievements.h"
#include "core/sotc/menu.h"
#include "core/sotc/menu_ext.h"
#include "core/sotc/screens.h"
#include "core/sotc/ui.h"
#include "input/controller.h"

namespace Sotc::MenuExt {

namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;

// All places are in the game's own screen units: -1 to 1 across, 1 (top) to -1 down.
constexpr float RowStep = 1.0f / 12.0f; // one menu line
constexpr float ArrowWidth = 0.042f;
constexpr float ArrowHeight = 0.074f;

enum class Kind { None, Title, Pause };

struct Root {
    float arrow_x;  // left edge of the arrow
    float arrow_y0; // bottom edge of the arrow on the first line
    std::array<const char*, 3> lines;
    int num_lines;
};
constexpr Root TitleRoot{-0.786f, 0.139f, {"Free Play", "Achievements", "Quit"}, 3};
constexpr Root PauseRoot{-0.695f, 0.087f, {"Achievements", nullptr, nullptr}, 1};

const Root& RootOf(Kind kind) {
    return kind == Kind::Pause ? PauseRoot : TitleRoot;
}

std::mutex mutex;
bool on_extra = false; // the cursor is on one of the extra lines
int extra_index = 0;
std::array<bool, 4> swallowed{};
std::atomic<Input::GameController*> pad{nullptr};

// Lines in the game's title menu. It differs with the saves present, so it is found out: one
// step up from the first line lands on the last. 0 = not known yet. The pause menu has four.
std::atomic<int> title_lines{0};
constexpr int PauseLines = 4;

// Button presses sent to the game to move its cursor, one after the other.
struct Tap {
    OrbisPadButtonDataOffset button;
    int frame = 0;
};
std::deque<Tap> taps;

std::filesystem::path CountFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "sotc_title_menu.txt";
}

void LoadCount() {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    int count = 0;
    std::ifstream{CountFile()} >> count;
    if (count >= 1 && count <= 8) {
        title_lines = count;
    }
}

void StoreCount(int count) {
    title_lines = count;
    std::ofstream{CountFile()} << count << "\n";
}

/// The menu on screen with the cursor on one of its lines, and which line.
Kind Current(int& line) {
    float x, y;
    if (!Screens::Arrow(x, y)) {
        return Kind::None;
    }
    const Kind kind = Screens::PauseMenu() ? Kind::Pause : Kind::Title;
    const Root& root = RootOf(kind);
    if (std::abs(x - root.arrow_x) > 0.004f) {
        return Kind::None; // another list (a sub-menu)
    }
    const float steps = (root.arrow_y0 - y) / RowStep;
    const int nearest = static_cast<int>(std::lround(steps));
    if (nearest < 0 || nearest > 7 || std::abs(steps - nearest) > 0.07f) {
        return Kind::None;
    }
    line = nearest;
    return kind;
}

int LinesOf(Kind kind) {
    return kind == Kind::Pause ? PauseLines : title_lines.load();
}

/// The game's own Settings line: first in the pause menu, and in the title menu the one above
/// Image Gallery, its last. Confirming it opens the port's settings page, which holds the PC
/// options and a line that carries on into the game's own settings.
int SettingsLine(Kind kind) {
    if (kind == Kind::Pause) {
        return 0;
    }
    const int lines = title_lines.load();
    return lines >= 2 ? lines - 2 : -1;
}

// Set by the settings page's "Game Settings" line: confirm the game's Settings line for real.
std::atomic_bool pass_settings{false};

bool PanelOpen() {
    return Menu::IsOpen() || Achievements::IsPanelOpen();
}

void Activate(Kind kind, int index) {
    const std::string_view line = RootOf(kind).lines[index];
    if (line == "Free Play") {
        Menu::OpenFreePlay();
    } else if (line == "Achievements") {
        if (!Achievements::IsPanelOpen()) {
            Achievements::TogglePanel();
        }
    } else if (line == "Quit") {
        Menu::OpenQuit();
    }
}

void QueueTap(OrbisPadButtonDataOffset button, int times = 1) {
    for (int i = 0; i < times; i++) {
        taps.push_back({button});
    }
}

} // namespace

bool OnNav(Nav nav, bool pressed, Input::GameController* controller) {
    pad = controller;
    std::scoped_lock lock{mutex};
    const size_t slot = static_cast<size_t>(nav);
    if (!pressed) {
        return std::exchange(swallowed[slot], false);
    }
    if (PanelOpen()) {
        // An overlay panel has the input; the game's menu underneath must not move.
        swallowed[slot] = true;
        return true;
    }
    int line = 0;
    const Kind kind = Current(line);
    if (kind == Kind::None) {
        on_extra = false;
        return false;
    }
    const int lines = LinesOf(kind);
    const Root& root = RootOf(kind);
    if (on_extra) {
        switch (nav) {
        case Nav::Up:
            if (extra_index == 0) {
                on_extra = false; // back onto the game's last line, where its cursor still is
            } else {
                --extra_index;
            }
            break;
        case Nav::Down:
            extra_index = std::min(extra_index + 1, root.num_lines - 1);
            break;
        case Nav::Confirm:
            Activate(kind, extra_index);
            break;
        case Nav::Back:
            on_extra = false;
            return false;
        }
        swallowed[slot] = true;
        return true;
    }
    if (nav == Nav::Confirm && line == SettingsLine(kind) && taps.empty()) {
        Menu::OpenSettings();
        swallowed[slot] = true;
        return true;
    }
    if (nav == Nav::Down && lines > 0 && line == lines - 1 && taps.empty()) {
        on_extra = true;
        extra_index = 0;
        swallowed[slot] = true;
        return true;
    }
    return false;
}

int FilterLeftY(int value, Input::GameController* controller) {
    static int held = 0;         // direction the stick is pushed in: -1 up, 1 down
    static bool swallowing = false;
    const int direction = value > 80 ? 1 : value < -80 ? -1 : 0;
    if (direction != held) {
        if (held != 0) {
            OnNav(held > 0 ? Nav::Down : Nav::Up, false, controller);
            swallowing = false;
        }
        held = direction;
        if (direction != 0) {
            swallowing = OnNav(direction > 0 ? Nav::Down : Nav::Up, true, controller);
        }
    }
    return swallowing ? 0 : value;
}

bool OnClick(float x, float y) {
    std::scoped_lock lock{mutex};
    if (PanelOpen()) {
        return false;
    }
    int line = 0;
    const Kind kind = Current(line);
    const int lines = LinesOf(kind);
    if (kind == Kind::None || lines == 0 || !taps.empty()) {
        return false;
    }
    const Root& root = RootOf(kind);
    const float sx = x * 2.0f - 1.0f;
    const float sy = 1.0f - y * 2.0f;
    if (sx < root.arrow_x || sx > root.arrow_x + 0.36f) {
        return false;
    }
    const float top = root.arrow_y0 + ArrowHeight * 0.5f + RowStep * 0.5f;
    const int row = static_cast<int>(std::floor((top - sy) / RowStep));
    if (row < 0 || row >= lines + root.num_lines) {
        return false;
    }
    if (row >= lines) {
        on_extra = true;
        extra_index = row - lines;
        Activate(kind, extra_index);
        return true;
    }
    // One of the game's lines: walk its cursor there, then confirm.
    on_extra = false;
    const int current = line;
    QueueTap(row > current ? OrbisPadButtonDataOffset::Down : OrbisPadButtonDataOffset::Up,
             std::abs(row - current));
    if (row == SettingsLine(kind)) {
        Menu::OpenSettings();
    } else {
        QueueTap(OrbisPadButtonDataOffset::Cross);
    }
    return true;
}

bool AtTitleRoot() {
    int line = 0;
    return Current(line) == Kind::Title;
}

void PassToGameSettings() {
    pass_settings = true;
}

bool HideArrow() {
    return on_extra || HideText();
}

bool HideText() {
    return Menu::ReplacesGameMenu() || Achievements::ReplacesGameMenu();
}

void Draw() {
    LoadCount();
    std::scoped_lock lock{mutex};
    Input::GameController* controller = pad.load();

    if (!PanelOpen() && pass_settings.exchange(false)) {
        QueueTap(OrbisPadButtonDataOffset::Cross);
    }
    // Presses on their way to the game: down for three frames, up for three.
    if (!taps.empty() && controller) {
        Tap& tap = taps.front();
        if (tap.frame == 0) {
            controller->Button(tap.button, true);
        } else if (tap.frame == 3) {
            controller->Button(tap.button, false);
        }
        if (++tap.frame > 6) {
            taps.pop_front();
        }
    }

    int line = 0;
    const Kind kind = Current(line);
    if (kind == Kind::None) {
        static int gone_frames = 0;
        if (!Screens::Arrow() && ++gone_frames > 20) {
            on_extra = false;
            gone_frames = 0;
        }
        return;
    }

    // How many lines the title menu has: step up from its first line once and see where the
    // cursor lands, then step back down.
    static int probe = 0; // 0 idle, 1 stepped up and waiting, 2 done for this session
    static int probe_frames = 0;
    static int steady_frames = 0;
    if (kind == Kind::Title && title_lines == 0 && !PanelOpen()) {
        if (probe == 0) {
            steady_frames = line == 0 ? steady_frames + 1 : 0;
            if (line != 0) {
                // The player moved first; go by what is seen.
                title_lines = std::max(3, line + 1);
            } else if (steady_frames > 30 && controller && taps.empty()) {
                QueueTap(OrbisPadButtonDataOffset::Up);
                probe = 1;
                probe_frames = 0;
            }
        } else if (probe == 1) {
            if (line != 0) {
                StoreCount(line + 1);
                LOG_INFO(Core, "Title menu has {} lines", line + 1);
                QueueTap(OrbisPadButtonDataOffset::Down);
                probe = 2;
            } else if (++probe_frames > 90) {
                title_lines = 3; // no answer; the usual count, not stored
                probe = 2;
            }
        }
    }
    // A cursor seen below the known last line corrects the count.
    if (kind == Kind::Title && title_lines != 0 && line >= title_lines) {
        StoreCount(line + 1);
    }

    const int lines = LinesOf(kind);
    if (lines == 0 || PanelOpen()) {
        return;
    }
    const Root& root = RootOf(kind);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const auto to_pixels = [&](float sx, float sy) {
        return ImVec2{(sx + 1.0f) * 0.5f * display.x, (1.0f - sy) * 0.5f * display.y};
    };
    auto* draw = ImGui::GetForegroundDrawList();
    // The game writes its menus in near-black on these screens.
    constexpr ImU32 Ink = IM_COL32(22, 22, 20, 255);
    const float size = 34.0f * Ui::Scale();
    for (int i = 0; i < root.num_lines; i++) {
        const float bottom = root.arrow_y0 - (lines + i) * RowStep;
        const float middle = bottom + ArrowHeight * 0.5f;
        const ImVec2 text_at = to_pixels(root.arrow_x + 0.036f, middle);
        const ImVec2 extent = Ui::TextSize(size, root.lines[i]);
        // Plain, as the game's own lines are: no shadow.
        draw->AddText(Ui::Serif(), size, {text_at.x, text_at.y - extent.y * 0.5f}, Ink,
                      root.lines[i]);
        if (on_extra && extra_index == i) {
            const ImVec2 a = to_pixels(root.arrow_x + 0.005f, middle + 0.026f);
            const ImVec2 b = to_pixels(root.arrow_x + 0.005f, middle - 0.026f);
            const ImVec2 c = to_pixels(root.arrow_x + 0.028f, middle);
            draw->AddTriangleFilled(a, c, b, Ink);
        }
    }
}

} // namespace Sotc::MenuExt
