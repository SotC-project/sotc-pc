// SPDX-License-Identifier: GPL-2.0-or-later

// Extra lines in the game's own menus, see menu_ext.h.

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/pad/pad.h"
#include "core/memory.h"
#include "core/sotc/achievements.h"
#include "core/sotc/menu.h"
#include "core/sotc/menu_ext.h"
#include "core/sotc/screens.h"
#include "core/sotc/ui.h"
#include "input/controller.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sotc::MenuExt {

namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;

// All places are in the game's own screen units: -1 to 1 across, 1 (top) to -1 down.
constexpr float RowStep = 1.0f / 12.0f; // one menu line
constexpr float ArrowHeight = 0.074f;
constexpr float SameLine = 0.012f; // two cursor heights this close are the same line

enum class Kind { None, Title, Pause };

struct Root {
    float arrow_x; // left edge of the arrow
    std::array<const char*, 3> lines;
    int num_lines;
};
constexpr Root TitleRoot{-0.786f, {"Free Play", "Achievements", "Quit"}, 3};
constexpr Root PauseRoot{-0.695f, {"Achievements", nullptr, nullptr}, 1};

const Root& RootOf(Kind kind) {
    return kind == Kind::Pause ? PauseRoot : TitleRoot;
}

std::mutex mutex;
bool on_extra = false; // the cursor is on one of the extra lines
int extra_index = 0;
std::array<bool, 4> swallowed{};
std::atomic<Input::GameController*> pad{nullptr};

// Where the game's last line is (the height of the cursor on it). The extra lines go below it,
// one line apart, and the game's Settings line is the one above it in the title menu.
//
// The pause menu always has the same four lines. The title menu does not: with a save it
// starts with Continue and three lines about that save, set closer together than the lines
// proper, and has Load Game as well; the whole list then sits elsewhere on the screen. So for
// the title the place is found out each time the game runs, by sending the cursor one step up
// from the first line, which lands on the last, and one step down again. Unknown = 2.
constexpr float PauseFirstY = 0.087f;
constexpr float PauseLastY = PauseFirstY - 3.0f * RowStep;
constexpr float Unknown = 2.0f;
std::atomic<float> title_last_y{Unknown};

// Button presses sent to the game to move its cursor, one after the other.
struct Tap {
    OrbisPadButtonDataOffset button;
    int frame = 0;
};
std::deque<Tap> taps;

// A click on one of the game's own lines: the cursor is walked there by presses, and when it
// has arrived the line is chosen.
enum class Then { Nothing, Choose, Settings, StepToFirst };
struct Seek {
    bool active = false;
    float y = 0.0f;
    Then then = Then::Nothing;
    int presses_left = 0;
};
Seek seek;

std::atomic_bool paused_in_cutscene{false};

/// The menu on screen with the cursor on one of its lines, and how high the cursor is.
Kind Current(float& y) {
    float x;
    if (!Screens::Arrow(x, y)) {
        return Kind::None;
    }
    const Kind kind = Screens::PauseMenu() ? Kind::Pause : Kind::Title;
    if (std::abs(x - RootOf(kind).arrow_x) > 0.004f) {
        return Kind::None; // another list (a sub-menu)
    }
    // Pausing a cutscene brings up a menu of its own in the pause menu's place (Display,
    // Audio, Open Photo Mode, Skip Cinematic). It is left alone. Whether a cutscene is on
    // is taken from the black bars of the picture as it was before the menu came up.
    if (kind == Kind::Pause && paused_in_cutscene.load()) {
        return Kind::None;
    }
    return kind;
}

float LastY(Kind kind) {
    return kind == Kind::Pause ? PauseLastY : title_last_y.load();
}

bool Known(Kind kind) {
    return LastY(kind) < 1.5f;
}

bool AtLast(Kind kind, float y) {
    return Known(kind) && std::abs(y - LastY(kind)) < SameLine;
}

/// The game's own Settings line: first in the pause menu, and in the title menu the one above
/// Image Gallery, its last. Confirming it opens the port's settings page, which holds the PC
/// options and a line that carries on into the game's own settings.
float SettingsY(Kind kind) {
    return kind == Kind::Pause ? PauseFirstY : LastY(kind) + RowStep;
}

bool AtSettings(Kind kind, float y) {
    return Known(kind) && std::abs(y - SettingsY(kind)) < SameLine;
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

/// The pause menu's last line reads "Quit Game" but leads to the title menu, not out of the
/// game. The game keeps its menu texts in memory as plain strings; the first time the pause
/// menu is up, the copies of that one are found and given a name of the same length that says
/// what the line does. Memory is read and written through the system, which reports a page that
/// has gone away instead of faulting on it.
/// It is done twice: when the title menu first comes up, before the game has built its pause
/// menu from those texts (the full-size menus lay their text out once and keep it), and when
/// the pause menu first comes up, for the menus that read the text each time.
void RenameQuitLine(int pass) {
#ifdef _WIN32
    static std::atomic<int> passes_done{0};
    if (passes_done.fetch_or(1 << pass) & (1 << pass)) {
        return;
    }
    std::thread{[] {
        const std::string old_text = std::string("Quit") + " Game";
        const std::string new_text = "Main Menu";
        const auto widen = [](const std::string& text) {
            std::string wide;
            for (const char c : text) {
                wide += c;
                wide += '\0';
            }
            return wide;
        };
        const std::string old_wide = widen(old_text);
        const std::string new_wide = widen(new_text);
        const HANDLE process = GetCurrentProcess();
        std::vector<char> block(4 << 20);
        u32 renamed = 0;
        u32 renamed_watched = 0;
        u64 scanned = 0;
        MEMORY_BASIC_INFORMATION info{};
        for (u8* address = reinterpret_cast<u8*>(0x10000);
             VirtualQuery(address, &info, sizeof(info)) == sizeof(info);
             address = static_cast<u8*>(info.BaseAddress) + info.RegionSize) {
            // Pages the game may write, and pages closed to writing for the moment because the
            // GPU side watches them: those are written through their backing memory.
            const bool watched = info.Protect == PAGE_READONLY;
            if (info.State != MEM_COMMIT || (info.Protect != PAGE_READWRITE && !watched) ||
                info.Type == MEM_IMAGE) {
                continue;
            }
            u8* const begin = static_cast<u8*>(info.BaseAddress);
            for (size_t offset = 0; offset < info.RegionSize;) {
                // Blocks overlap by the length of the text, so one across a boundary is seen.
                const size_t want = std::min(block.size(), info.RegionSize - offset);
                SIZE_T got = 0;
                if (!ReadProcessMemory(process, begin + offset, block.data(), want, &got) ||
                    got < old_text.size() + 1) {
                    break;
                }
                scanned += got;
                // The text as one byte a letter, and as two (the game keeps both kinds). A match
                // inside a longer word is left alone; one followed by a question mark or the
                // end of the text is the line.
                for (const bool wide : {false, true}) {
                    const std::string& from = wide ? old_wide : old_text;
                    const std::string& to = wide ? new_wide : new_text;
                    const std::string_view view{block.data(), got};
                    for (size_t at = view.find(from); at != view.npos; at = view.find(from, at + 1)) {
                        const size_t end = at + from.size();
                        const bool letter_after =
                            end < got && std::isalpha(static_cast<unsigned char>(view[end]));
                        const bool letter_before =
                            !wide && at > 0 &&
                            std::isalpha(static_cast<unsigned char>(view[at - 1]));
                        if (letter_after || letter_before) {
                            continue;
                        }
                        u8* const target = begin + offset + at;
                        auto* memory = Core::Memory::Instance();
                        if (watched && !memory->IsValidMapping(reinterpret_cast<VAddr>(target),
                                                               to.size())) {
                            continue; // read-only memory of the program's own, not the game's
                        }
                        if (watched ? memory->TryWriteBacking(target, to.data(), to.size())
                                    : WriteProcessMemory(process, target, to.data(), to.size(),
                                                         nullptr) != 0) {
                            ++renamed;
                            renamed_watched += watched;
                        }
                    }
                }
                if (got < want) {
                    break;
                }
                offset += want == block.size() ? want - old_wide.size() - 2 : want;
            }
        }
        LOG_INFO(Core, "Pause menu line renamed in {} places, {} of them in watched memory ({} MB "
                       "looked through)",
                 renamed, renamed_watched, scanned >> 20);
    }}.detach();
#endif
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
    float y = 0.0f;
    const Kind kind = Current(y);
    if (kind == Kind::None) {
        on_extra = false;
        return false;
    }
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
    if (nav == Nav::Confirm && AtSettings(kind, y) && taps.empty()) {
        Menu::OpenSettings();
        swallowed[slot] = true;
        return true;
    }
    if (nav == Nav::Down && AtLast(kind, y) && taps.empty()) {
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
    float cursor_y = 0.0f;
    const Kind kind = Current(cursor_y);
    if (kind == Kind::None || !Known(kind) || !taps.empty() || seek.active) {
        return false;
    }
    const Root& root = RootOf(kind);
    const float sx = x * 2.0f - 1.0f;
    const float sy = 1.0f - y * 2.0f;
    if (sx < root.arrow_x || sx > root.arrow_x + 0.36f) {
        return false;
    }
    // Rows counted down from the game's last line: 0 is that line, 1 the first extra line;
    // -1 the game's line above it, and so on.
    const float last_middle = LastY(kind) + ArrowHeight * 0.5f;
    const int row = static_cast<int>(std::lround((last_middle - sy) / RowStep));
    if (row > root.num_lines) {
        return false;
    }
    if (row >= 1) {
        on_extra = true;
        extra_index = row - 1;
        Activate(kind, extra_index);
        return true;
    }
    on_extra = false;
    if (kind == Kind::Pause && row < -3) {
        return false;
    }
    // The four lines at the foot of either menu are a line apart. Higher up the title menu
    // with a save has Continue and the lines about it: a click there means Continue, its first
    // line, which is one step down from the last.
    if (row >= -3) {
        seek = {true, LastY(kind) - row * RowStep, row == -1 && kind == Kind::Title ? Then::Settings
                                                    : Then::Choose, 10};
        if (kind == Kind::Pause && row == -3) {
            seek.then = Then::Settings;
        }
    } else {
        seek = {true, LastY(kind), Then::StepToFirst, 10};
    }
    return true;
}

bool AtTitleRoot() {
    float y = 0.0f;
    return Current(y) == Kind::Title;
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
    std::scoped_lock lock{mutex};
    if (!Screens::PauseMenu()) {
        paused_in_cutscene = Screens::Letterboxed();
    }
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

    if (std::getenv("SOTC_MENULOG")) {
        static u64 logged = 0;
        float ax = 0.0f, ay = 0.0f;
        if (Screens::Arrow(ax, ay) && SDL_GetTicks() - logged > 1500) {
            logged = SDL_GetTicks();
            LOG_INFO(Core, "menu cursor at {:.4f} {:.4f}, pause {}, last line {:.3f}, pad {}", ax,
                     ay, Screens::PauseMenu(), title_last_y.load(), pad.load() != nullptr);
        }
    }
    float y = 0.0f;
    const Kind kind = Current(y);
    if (kind == Kind::None) {
        static int gone_frames = 0;
        if (!Screens::Arrow() && ++gone_frames > 20) {
            on_extra = false;
            seek.active = false;
            gone_frames = 0;
        }
        return;
    }
    RenameQuitLine(kind == Kind::Pause ? 1 : 0);

    // How long the cursor has rested where it is.
    static float rest_y = Unknown;
    static int rest_frames = 0;
    if (std::abs(y - rest_y) < SameLine) {
        ++rest_frames;
    } else {
        rest_y = y;
        rest_frames = 0;
    }

    // Where the title menu's last line is: one step up from where the cursor rests when the
    // menu comes up (its first line) lands on the last; a step down from there must then come
    // back up to the first, or the start was not the first line and the cursor is walked down
    // until it wraps. A cursor seen lower than what is known corrects it at any time.
    static int probe = 0; // 0 not begun, 1 stepped up, 2 stepped down to check, 3 walking, 4 done
    static float probe_start = 0.0f, probe_candidate = 0.0f;
    static int probe_presses = 0;
    if (kind == Kind::Title) {
        if (Known(kind) && y < title_last_y.load() - SameLine && taps.empty() && rest_frames > 6) {
            title_last_y = y;
        }
        if (!Known(kind) && !PanelOpen() && controller && taps.empty() && !seek.active) {
            if (probe == 0 && rest_frames > 30) {
                probe_start = y;
                QueueTap(OrbisPadButtonDataOffset::Up);
                probe = 1;
                rest_frames = 0;
            } else if (probe == 1 && rest_frames > 8) {
                probe_candidate = y;
                QueueTap(OrbisPadButtonDataOffset::Down);
                probe = 2;
                rest_frames = 0;
            } else if (probe == 2 && rest_frames > 8) {
                if (probe_candidate < probe_start - SameLine && y > probe_candidate + SameLine) {
                    // Up went to a lower line and down came back up: the lower one is the last.
                    title_last_y = probe_candidate;
                    LOG_INFO(Core, "Title menu: last line at {:.3f}", probe_candidate);
                    probe = 4;
                } else {
                    probe = 3;
                    probe_presses = 0;
                    probe_candidate = y;
                }
            } else if (probe == 3 && rest_frames > 8) {
                if (y > probe_candidate + SameLine) {
                    // Wrapped to the top: the line before was the last. Walk back to the start.
                    title_last_y = probe_candidate;
                    LOG_INFO(Core, "Title menu: last line at {:.3f} (found by walking)",
                             probe_candidate);
                    probe = 4;
                    seek = {true, probe_start, Then::Nothing, 10};
                } else if (++probe_presses > 10) {
                    probe = 4; // gave up; the lowest line seen will do when it comes
                    title_last_y = std::min(y, probe_candidate);
                } else {
                    probe_candidate = y;
                    QueueTap(OrbisPadButtonDataOffset::Down);
                    rest_frames = 0;
                }
            }
        }
    }

    // A click being followed up.
    if (seek.active && taps.empty() && rest_frames > 6 && !PanelOpen()) {
        if (std::abs(y - seek.y) < SameLine) {
            seek.active = false;
            switch (seek.then) {
            case Then::Choose:
                QueueTap(OrbisPadButtonDataOffset::Cross);
                break;
            case Then::Settings:
                Menu::OpenSettings();
                break;
            case Then::StepToFirst:
                QueueTap(OrbisPadButtonDataOffset::Down);
                QueueTap(OrbisPadButtonDataOffset::Cross);
                break;
            case Then::Nothing:
                break;
            }
        } else if (seek.presses_left-- <= 0) {
            seek.active = false;
        } else {
            QueueTap(seek.y < y ? OrbisPadButtonDataOffset::Down : OrbisPadButtonDataOffset::Up);
            rest_frames = 0;
        }
    }

    if (!Known(kind) || PanelOpen()) {
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
        const float bottom = LastY(kind) - (i + 1) * RowStep;
        const float middle = bottom + ArrowHeight * 0.5f;
        const ImVec2 text_at = to_pixels(root.arrow_x + 0.036f, middle);
        const ImVec2 extent = Ui::TextSize(size, root.lines[i]);
        // Plain, as the game's own lines are: no shadow.
        draw->AddText(Ui::Serif(), size, {text_at.x, text_at.y - extent.y * 0.5f}, Ink,
                      root.lines[i]);
        if (Screens::FullSizeMenus()) {
            // The full-size menus are set in a heavier cut of the face: drawn once more a
            // touch to the right, the added lines match them.
            draw->AddText(Ui::Serif(), size,
                          {text_at.x + 0.6f * Ui::Scale(), text_at.y - extent.y * 0.5f}, Ink,
                          root.lines[i]);
        }
        if (on_extra && extra_index == i) {
            const ImVec2 a = to_pixels(root.arrow_x + 0.005f, middle + 0.026f);
            const ImVec2 b = to_pixels(root.arrow_x + 0.005f, middle - 0.026f);
            const ImVec2 c = to_pixels(root.arrow_x + 0.028f, middle);
            draw->AddTriangleFilled(a, c, b, Ink);
        }
    }
}

} // namespace Sotc::MenuExt
