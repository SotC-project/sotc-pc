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
#include "core/devtools/layer.h"
#include "core/libraries/pad/pad.h"
#include "core/memory.h"
#include "core/sotc/achievements.h"
#include "core/sotc/lang.h"
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
// The pause menu gets no line of its own: Achievements is reached from the title menu only.
constexpr Root PauseRoot{-0.695f, {nullptr, nullptr, nullptr}, 0};

const Root& RootOf(Kind kind) {
    return kind == Kind::Pause ? PauseRoot : TitleRoot;
}

std::mutex mutex;
bool on_extra = false; // the cursor is on one of the extra lines
// While the cursor is on the extra lines the game's own cursor, which is not drawn then, takes
// one step for each step made there: the game so plays its sound for it. Where it stands for
// each extra line is chosen so that every step, and the step back onto the game's list at either
// end, is one step of the game's cursor (see HiddenY).
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
// The title menu's first line: the list is a ring, and the extra lines stand between its last
// line and its first.
std::atomic<float> title_first_y{Unknown};

// Button presses sent to the game to move its cursor, one after the other.
struct Tap {
    OrbisPadButtonDataOffset button;
    u64 pressed_ms = 0;
    bool released = false;
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
// The game's arrow stays blank while its cursor walks from its hidden place back onto a line of
// the game's list.
std::atomic_bool arrow_returning{false};

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
    // The game's settings list stands where the pause menu does and is drawn with its
    // pictures, half a line lower: only a cursor on one of the pause menu's four lines counts.
    if (kind == Kind::Pause) {
        bool on_line = false;
        for (int line = 0; line < 4; line++) {
            on_line |= std::abs(y - (PauseFirstY - line * RowStep)) < 0.02f;
        }
        if (!on_line) {
            return Kind::None;
        }
    }
    // The list of difficulties that New Game opens (Easy, Normal, Hard) stands where the title
    // menu does. Its three lines are a fraction of a line off the title menu's in both sets
    // of menu pictures, and that is what tells them apart.
    if (kind == Kind::Title) {
        constexpr float NormalY = -0.0370f;
        for (int line = -1; line <= 1; line++) {
            if (std::abs(y - (NormalY + line * RowStep)) < 0.004f) {
                return Kind::None;
            }
        }
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

/// Where the game's hidden cursor stands while the cursor is on an extra line of the title menu.
/// On the first it is one line above the game's last, so one step down brings it out on that
/// line; on the last it is one line below the game's first, so one step up brings it out there;
/// in between it is on the line that is one step from both. That works out for the title menu's
/// five lines with a save and its three without; a list of another length costs a step more
/// here and there, which the walk to the wanted line makes up.
float HiddenY(int index) {
    const float last = title_last_y.load();
    const float first = title_first_y.load();
    if (index == 0) {
        return last + RowStep;
    }
    if (index == 1) {
        return last + 2.0f * RowStep;
    }
    // With a save the first line (Continue) has lines about the save under it; the four lines
    // at the foot are a line apart.
    const bool even = first < 1.5f && first - last < 3.2f * RowStep;
    return even ? first - RowStep : last + 3.0f * RowStep;
}

/// Sends the game's cursor to a line. With `returning` its arrow is blank until it is there.
void SeekTo(float y, Then then = Then::Nothing, bool returning = false) {
    seek = {true, y, then, 6};
    arrow_returning = returning;
}

/// Puts the cursor on an extra line.
void GoToExtra(int index) {
    on_extra = true;
    extra_index = index;
    SeekTo(HiddenY(index));
}

/// What stands at a place of the screen in a menu: nothing, an extra line, or a line of the
/// game's (given by the height its cursor has there).
struct Spot {
    enum class What { None, Extra, Line } what = What::None;
    int extra = 0;
    float y = 0.0f;
    Then then = Then::Choose; // what a click there does, for a line of the game's
};

Spot SpotAt(Kind kind, float sx, float sy) {
    Spot spot;
    const Root& root = RootOf(kind);
    if (!Known(kind) || sx < root.arrow_x || sx > root.arrow_x + 0.36f) {
        return spot;
    }
    // Rows counted down from the game's last line: 0 is that line, 1 the first extra line;
    // -1 the game's line above it, and so on.
    const float last_middle = LastY(kind) + ArrowHeight * 0.5f;
    const int row = static_cast<int>(std::lround((last_middle - sy) / RowStep));
    if (row > root.num_lines) {
        return spot;
    }
    if (row >= 1) {
        spot.what = Spot::What::Extra;
        spot.extra = row - 1;
        return spot;
    }
    if (kind == Kind::Pause && row < -3) {
        return spot;
    }
    spot.what = Spot::What::Line;
    // The four lines at the foot of either menu are a line apart. Higher up the title menu
    // with a save has Continue and the lines about it: that place means Continue, its first
    // line.
    const float first = title_first_y.load();
    if (kind == Kind::Title && first < 1.5f) {
        if (sy > first + ArrowHeight * 0.5f + RowStep * 0.5f) {
            spot.what = Spot::What::None;
            return spot;
        }
        if (row < -3 || std::abs(first - (LastY(kind) - row * RowStep)) < RowStep * 0.5f) {
            spot.y = first;
            return spot;
        }
    }
    if (row >= -3) {
        spot.y = LastY(kind) - row * RowStep;
        if ((row == -1 && kind == Kind::Title) || (row == -3 && kind == Kind::Pause)) {
            spot.then = Then::Settings;
        }
    } else {
        spot.y = LastY(kind);
        spot.then = Then::StepToFirst;
    }
    return spot;
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
            static const bool debug = std::getenv("SOTC_RENAME_DEBUG") != nullptr;
            // Some of the game's memory is mapped open to running as well as writing; the
            // runtime's second view of the game's memory is too, and is left out (it is the
            // same memory).
            const bool open = info.Protect == PAGE_EXECUTE_READWRITE &&
                              Core::Memory::Instance()->IsValidMapping(
                                  reinterpret_cast<VAddr>(info.BaseAddress), 1);
            if (info.State != MEM_COMMIT ||
                (info.Protect != PAGE_READWRITE && !watched && !open) ||
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
                        // Not the text being looked for itself, nor the copy just read: with
                        // that one overwritten the rest of the search looks for the wrong words.
                        const auto within = [&](const char* data, size_t size) {
                            const u8* const first = reinterpret_cast<const u8*>(data);
                            return target + from.size() > first && target < first + size;
                        };
                        if (within(old_text.data(), old_text.size()) ||
                            within(old_wide.data(), old_wide.size()) ||
                            within(block.data(), block.size())) {
                            continue;
                        }
                        if (debug) {
                            LOG_INFO(Core, "rename: at {:#x} protect {:#x} type {:#x} wide {}",
                                     reinterpret_cast<u64>(target), u32(info.Protect),
                                     u32(info.Type), wide);
                        }
                        auto* memory = Core::Memory::Instance();
                        // Only memory the game itself mapped, by the memory manager's records:
                        // never the runtime's own or a driver's that happens to hold the words.
                        if (!memory->IsValidMapping(reinterpret_cast<VAddr>(target), to.size())) {
                            continue;
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

std::atomic<float> test_pointer_x{0.0f}, test_pointer_y{0.0f};
std::atomic_bool test_pointer_moved{false};

/// The pointer's place in the game's screen units; false when it has not moved this frame.
bool PointerMoved(float& sx, float& sy) {
    if (test_pointer_moved.exchange(false)) {
        sx = test_pointer_x.load() * 2.0f - 1.0f;
        sy = 1.0f - test_pointer_y.load() * 2.0f;
        return true;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f) || io.DisplaySize.x <= 0.0f ||
        io.DisplaySize.y <= 0.0f || !Overlay::MenuCursorActive()) {
        return false;
    }
    sx = io.MousePos.x / io.DisplaySize.x * 2.0f - 1.0f;
    sy = 1.0f - io.MousePos.y / io.DisplaySize.y * 2.0f;
    return true;
}

/// The game's other lists (its settings, the difficulties, the save slots, ...): their lines are
/// not known here, so the cursor is stepped towards the pointer and watched. It stops when it is
/// on the pointer's line, at the end of the list, or when a step took it further away.
void FollowPointer(float arrow_x, float arrow_y, int rest_frames) {
    static bool following = false;
    static int last_direction = 0;
    static float last_distance = 0.0f, stepped_from = 0.0f;
    static float pointer_x = 0.0f, pointer_y = 0.0f;
    if (PointerMoved(pointer_x, pointer_y)) {
        following = true;
        last_direction = 0;
    }
    if (!following || !taps.empty() || rest_frames <= 6) {
        return;
    }
    if (pointer_x < arrow_x - 0.03f || pointer_x > arrow_x + 0.9f) {
        following = false;
        return;
    }
    const float off = pointer_y - (arrow_y + ArrowHeight * 0.5f); // above the cursor: positive
    const int direction = off > 0.0f ? -1 : 1;
    if (std::abs(off) < RowStep * 0.55f ||
        (last_direction != 0 &&
         (direction != last_direction || std::abs(arrow_y - stepped_from) < SameLine ||
          std::abs(off) >= last_distance))) {
        following = false;
        return;
    }
    QueueTap(direction < 0 ? OrbisPadButtonDataOffset::Up : OrbisPadButtonDataOffset::Down);
    last_direction = direction;
    last_distance = std::abs(off);
    stepped_from = arrow_y;
}

/// A cutscene can be left by holding the confirm button, which the game only says once a
/// button is pressed. Said here for the first seconds of every scene, in the black bar at the
/// foot of the picture; not while a key is down or was a moment ago, when the game's own hint
/// stands in that place.
void DrawSkipHint() {
    static u64 since_ms = 0, touched_ms = 0;
    const bool scene = Screens::Letterboxed() && !Screens::PauseMenu() && !Screens::Arrow() &&
                       !Overlay::InTitleMenu() && !PanelOpen() && !Ui::DialogOpen();
    if (!scene) {
        since_ms = 0;
        return;
    }
    const u64 now = SDL_GetTicks();
    if (since_ms == 0) {
        since_ms = now;
    }
    const ImGuiIO& io = ImGui::GetIO();
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key))) {
            touched_ms = now;
            break;
        }
    }
    if (now - since_ms > 8000 || (touched_ms != 0 && now - touched_ms < 4000)) {
        return;
    }
    Ui::Prompt(ImGui::GetForegroundDrawList(), io.DisplaySize.x * 0.95f, io.DisplaySize.y * 0.938f,
               "HOLD TO SKIP", Ui::Action::Confirm);
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
                // Back onto the game's last line.
                on_extra = false;
                SeekTo(LastY(kind), Then::Nothing, true);
            } else {
                GoToExtra(extra_index - 1);
            }
            break;
        case Nav::Down:
            if (extra_index + 1 < root.num_lines) {
                GoToExtra(extra_index + 1);
            } else if (title_first_y.load() < 1.5f) {
                // Round to the game's first line.
                on_extra = false;
                SeekTo(title_first_y.load(), Then::Nothing, true);
            }
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
    if (root.num_lines > 0 && taps.empty() && !seek.active) {
        // The extra lines stand between the game's last line and its first: down from the
        // last and up from the first lead onto them.
        if (nav == Nav::Down && AtLast(kind, y)) {
            GoToExtra(0);
            swallowed[slot] = true;
            return true;
        }
        if (nav == Nav::Up && std::abs(y - title_first_y.load()) < SameLine) {
            GoToExtra(root.num_lines - 1);
            swallowed[slot] = true;
            return true;
        }
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
    if (kind == Kind::None || !Known(kind) || !taps.empty() ||
        (seek.active && seek.then != Then::Nothing)) {
        return false;
    }
    const Spot spot = SpotAt(kind, x * 2.0f - 1.0f, 1.0f - y * 2.0f);
    if (spot.what == Spot::What::None) {
        return false;
    }
    if (spot.what == Spot::What::Extra) {
        if (!on_extra || extra_index != spot.extra) {
            GoToExtra(spot.extra);
        }
        Activate(kind, spot.extra);
        return true;
    }
    on_extra = false;
    SeekTo(spot.y, spot.then);
    seek.presses_left = 10;
    return true;
}

void SetPad(Input::GameController* controller) {
    pad = controller;
}

void TestPointer(float x, float y) {
    test_pointer_x = x;
    test_pointer_y = y;
    test_pointer_moved = true;
}

bool AtTitleRoot() {
    float y = 0.0f;
    return Current(y) == Kind::Title;
}

void PassToGameSettings() {
    pass_settings = true;
}

bool HideArrow() {
    return on_extra || arrow_returning.load() || HideText();
}

bool HideText() {
    return Menu::ReplacesGameMenu() || Achievements::ReplacesGameMenu() || Ui::DialogOpen();
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
    // Presses on their way to the game: down for a twentieth of a second, then up for one. By
    // the clock: the picture may be drawn several times for each time the game reads the pad.
    const u64 now_ms = SDL_GetTicks();
    if (!taps.empty() && controller) {
        Tap& tap = taps.front();
        if (tap.pressed_ms == 0) {
            controller->Button(tap.button, true);
            tap.pressed_ms = now_ms;
        } else if (!tap.released && now_ms - tap.pressed_ms >= 50) {
            controller->Button(tap.button, false);
            tap.released = true;
        } else if (now_ms - tap.pressed_ms >= 110) {
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
    DrawSkipHint();

    // How long the game's cursor has rested where it is, in sixtieths of a second.
    static float rest_y = Unknown;
    static u64 rest_since = 0;
    float arrow_x = 0.0f, arrow_y = 0.0f;
    const bool arrow = Screens::Arrow(arrow_x, arrow_y);
    if (!arrow || std::abs(arrow_y - rest_y) >= SameLine) {
        rest_y = arrow ? arrow_y : Unknown;
        rest_since = now_ms;
    }
    int rest_frames = static_cast<int>((now_ms - rest_since) * 60 / 1000);

    float y = 0.0f;
    const Kind kind = Current(y);
    if (kind == Kind::None) {
        static u64 arrow_seen_ms = 0;
        if (arrow) {
            arrow_seen_ms = now_ms;
        } else if (now_ms - arrow_seen_ms > 330) {
            on_extra = false;
            seek.active = false;
            arrow_returning = false;
        }
        if (arrow && controller && !PanelOpen() && !Ui::DialogOpen()) {
            FollowPointer(arrow_x, arrow_y, rest_frames);
        }
        return;
    }
    RenameQuitLine(kind == Kind::Pause ? 1 : 0);

    // Free play has no title menu of its own: leaving a battle for the main menu (the pause
    // menu's last line, or End after a defeat) starts the game again plainly.
    static bool pause_seen = false, leaving = false;
    pause_seen |= kind == Kind::Pause;
    if (kind == Kind::Title && pause_seen && !leaving) {
        leaving = true;
        Menu::LeaveFreePlay();
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
            // With a save the list always stands in the same place, known by its first line
            // (Continue): nothing needs finding out, and the menu comes up without a sound.
            constexpr float ContinueY = 0.3611f, WithSaveLastY = -0.1843f;
            if (probe == 0 && rest_frames > 12 && std::abs(y - ContinueY) < 0.004f) {
                title_first_y = y;
                title_last_y = WithSaveLastY;
                probe = 4;
            } else if (probe == 0 && rest_frames > 45) {
                probe_start = y;
                QueueTap(OrbisPadButtonDataOffset::Up);
                probe = 1;
                rest_frames = 0, rest_since = now_ms;
            } else if (probe == 1 && rest_frames > 8 && std::abs(y - probe_start) < 0.03f &&
                       probe_presses < 8) {
                // The menu was not taking presses yet (it fades in): once more.
                ++probe_presses;
                probe_start = y;
                QueueTap(OrbisPadButtonDataOffset::Up);
                rest_frames = 0, rest_since = now_ms;
            } else if (probe == 1 && rest_frames > 8) {
                probe_candidate = y;
                QueueTap(OrbisPadButtonDataOffset::Down);
                probe = 2;
                rest_frames = 0, rest_since = now_ms;
            } else if (probe == 2 && rest_frames > 8) {
                if (probe_candidate < probe_start - SameLine && y > probe_candidate + SameLine) {
                    // Up went to a lower line and down came back up: the lower one is the last.
                    title_last_y = probe_candidate;
                    title_first_y = y;
                    LOG_INFO(Core, "Title menu: last line at {:.3f}, first at {:.3f}",
                             probe_candidate, y);
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
                    title_first_y = y;
                    LOG_INFO(Core, "Title menu: last line at {:.3f}, first at {:.3f} (found by "
                                   "walking)",
                             probe_candidate, y);
                    probe = 4;
                    seek = {true, probe_start, Then::Nothing, 10};
                } else if (++probe_presses > 10) {
                    probe = 4; // gave up; the lowest line seen will do when it comes
                    title_last_y = std::min(y, probe_candidate);
                } else {
                    probe_candidate = y;
                    QueueTap(OrbisPadButtonDataOffset::Down);
                    rest_frames = 0, rest_since = now_ms;
                }
            }
        }
    }

    // A click being followed up.
    if (seek.active && taps.empty() && rest_frames > 6 && !PanelOpen()) {
        if (std::abs(y - seek.y) < SameLine) {
            seek.active = false;
            arrow_returning = false;
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
            arrow_returning = false;
        } else {
            QueueTap(seek.y < y ? OrbisPadButtonDataOffset::Down : OrbisPadButtonDataOffset::Up);
            rest_frames = 0, rest_since = now_ms;
        }
    }

    if (!Known(kind) || PanelOpen() || Ui::DialogOpen()) {
        return;
    }
    // The pointer over a line puts the cursor on it.
    if (float sx, sy; controller && PointerMoved(sx, sy) &&
                      !(seek.active && seek.then != Then::Nothing)) {
        const Spot spot = SpotAt(kind, sx, sy);
        if (spot.what == Spot::What::Extra) {
            if (!on_extra || extra_index != spot.extra) {
                GoToExtra(spot.extra);
            }
        } else if (spot.what == Spot::What::Line && spot.then != Then::StepToFirst) {
            const bool there = seek.active ? std::abs(seek.y - spot.y) < SameLine
                                           : std::abs(y - spot.y) < SameLine;
            if (on_extra || !there) {
                const bool from_extra = std::exchange(on_extra, false);
                SeekTo(spot.y, Then::Nothing, from_extra);
            }
        }
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
        const char* const line = Lang::Tr(root.lines[i]).c_str();
        const ImVec2 extent = Ui::TextSize(size, line);
        // Plain, as the game's own lines are: no shadow.
        draw->AddText(Ui::Serif(), size, {text_at.x, text_at.y - extent.y * 0.5f}, Ink,
                      line);
        if (Screens::FullSizeMenus()) {
            // The full-size menus are set in a heavier cut of the face: drawn once more a
            // touch to the right, the added lines match them.
            draw->AddText(Ui::Serif(), size,
                          {text_at.x + 0.6f * Ui::Scale(), text_at.y - extent.y * 0.5f}, Ink,
                          line);
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
