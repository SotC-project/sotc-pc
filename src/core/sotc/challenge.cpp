// SPDX-License-Identifier: GPL-2.0-or-later

// Free play challenge: a timed fight against one colossus, graded on time and damage taken.
// The game gives no direct signal for either, so the run is followed from outside: the clock
// starts on the first input after loading, the colossus falling arrives as its trophy, and
// health is read off the red bar the game draws bottom right.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "core/sotc/challenge.h"
#include "core/sotc/lang.h"
#include "core/sotc/menu.h"
#include "core/sotc/menu_ext.h"
#include "core/sotc/screens.h"
#include "core/sotc/ui.h"

namespace Sotc::Challenge {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<const char*, 16> Names{
    "Valus",   "Quadratus", "Gaius",   "Phaedra", "Avion",   "Barba",   "Hydrus", "Kuromori",
    "Basaran", "Dirge",     "Celosia", "Pelagia", "Phalanx", "Cenobia", "Argus",  "Malus",
};

// Time attack limits of the PS4 game in seconds: normal, hard. The published tables found list
// twelve of the sixteen; colossi 12-15 are estimates.
constexpr std::array<std::array<int, 2>, 16> Limits{{
    {150, 165}, {210, 270}, {360, 390}, {300, 375}, {300, 255}, {210, 360}, {540, 450}, {360, 420},
    {330, 345}, {330, 270}, {270, 255}, {360, 420}, {360, 330}, {270, 300}, {390, 510}, {570, 645},
}};
// How long the fight takes a typical player who knows the game but is not racing, in seconds.
// These are estimates, not measured data.
constexpr std::array<int, 16> Averages{
    360, 480, 600, 540, 480, 480, 540, 600, 600, 480, 540, 600, 600, 480, 720, 900,
};

enum class Phase { Off, Loading, Ready, Running, Won, Died };

struct State {
    bool parsed = false;
    Phase phase = Phase::Off;
    int colossus = 1;
    std::string difficulty = "normal";
    double limit = 0.0;
    double average = 0.0;
    Clock::time_point start{};
    double final_time = 0.0;

    // Health bar tracking. Lengths are fractions of the image width.
    float full_length = 0.0f;
    float health = 1.0f; // last value read, 0..1
    float damage = 0.0f; // sum of all drops, in health bars
    bool bar_seen = false;
    Clock::time_point last_visible{};
    Clock::time_point last_probe{};
    Clock::time_point last_log{};
    // The colossus's introduction plays between black bars; the strip read for the health bar
    // lies inside the lower one. While it is pure black the player has no control yet.
    bool strip_seen = false;
    bool letterboxed = true;
    // After a death: the game's Game Over screen (Retry, End) is up underneath the result.
    bool game_over_seen = false;
    Clock::time_point game_over_last{};
};

std::mutex mutex;
State state;
std::atomic_bool enabled_cache{false};
std::atomic_bool parsed_cache{false};

void Parse() {
    if (state.parsed) {
        return;
    }
    state.parsed = true;
    // SOTC_CHALLENGE=<colossus 1-16>,<difficulty>
    if (const char* env = std::getenv("SOTC_CHALLENGE")) {
        int colossus = 0;
        char difficulty[16]{};
        if (std::sscanf(env, "%d,%15[a-z]", &colossus, difficulty) == 2 && colossus >= 1 &&
            colossus <= 16) {
            state.colossus = colossus;
            state.difficulty = difficulty;
            state.limit = TimeLimit(colossus, state.difficulty == "hard");
            state.average = AverageTime(colossus);
            state.phase = Phase::Loading;
            enabled_cache = true;
        }
    }
    parsed_cache = true;
}

double Elapsed() {
    return std::chrono::duration<double>(Clock::now() - state.start).count();
}

void Finish(bool died) {
    if (state.phase != Phase::Running) {
        return;
    }
    state.final_time = Elapsed();
    state.phase = died ? Phase::Died : Phase::Won;
    LOG_INFO(Lib_NpTrophy, "Challenge {}: {:.1f} s, damage {:.2f}", died ? "lost" : "won",
             state.final_time, state.damage);
}

/// Grade from time against two marks: the game's time attack limit and a typical player's
/// time. Damage taken lowers it.
const char* Grade() {
    if (state.phase == Phase::Died) {
        return "F";
    }
    static constexpr std::array<const char*, 6> Grades{"D", "C", "B", "A", "S", "S+"};
    const double time = state.final_time;
    const double limit = state.limit;
    const double average = std::max(state.average, limit * 1.2);
    int grade = 0;
    if (time <= limit * 0.60) {
        grade = 5;
    } else if (time <= limit * 0.75) {
        grade = 4;
    } else if (time <= limit) {
        grade = 3;
    } else if (time <= (limit + average) * 0.5) {
        grade = 2;
    } else if (time <= average) {
        grade = 1;
    }
    // S+ is a clean run; each further half bar of damage costs a grade.
    if (grade == 5 && state.damage > 0.0f) {
        grade = 4;
    }
    if (state.damage > 0.5f) {
        grade--;
    }
    if (state.damage > 1.0f) {
        grade--;
    }
    return Grades[std::max(grade, 0)];
}

std::string FormatTime(double seconds) {
    const int total = static_cast<int>(seconds);
    return fmt::format("{}:{:02d}.{}", total / 60, total % 60,
                       static_cast<int>((seconds - total) * 10.0));
}

/// A mark to aim at, in whole seconds.
std::string FormatMark(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    return fmt::format("{}:{:02d}", total / 60, total % 60);
}

} // namespace

const char* ColossusName(int colossus) {
    return Names[std::clamp(colossus, 1, 16) - 1];
}

double TimeLimit(int colossus, bool hard) {
    return Limits[std::clamp(colossus, 1, 16) - 1][hard ? 1 : 0];
}

bool LimitIsEstimate(int colossus) {
    return colossus >= 12 && colossus <= 15;
}

double AverageTime(int colossus) {
    return Averages[std::clamp(colossus, 1, 16) - 1];
}

bool Enabled() {
    if (!parsed_cache) {
        std::lock_guard lock{mutex};
        Parse();
    }
    return enabled_cache;
}

bool WantsHudStrip() {
    if (!Enabled()) {
        return false;
    }
    std::lock_guard lock{mutex};
    if (state.phase != Phase::Running && state.phase != Phase::Ready) {
        return false;
    }
    const auto now = Clock::now();
    if (now - state.last_probe < std::chrono::milliseconds(120)) {
        return false;
    }
    state.last_probe = now;
    return true;
}

void OnHudStrip(std::span<const u8> rgba, u32 width, u32 height) {
    if (width == 0 || height == 0 || rgba.size() < static_cast<size_t>(width) * height * 4) {
        return;
    }
    {
        bool black = true;
        for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n && black; i++) {
            black = rgba[i * 4] < 8 && rgba[i * 4 + 1] < 8 && rgba[i * 4 + 2] < 8;
        }
        std::lock_guard lock{mutex};
        state.strip_seen = true;
        state.letterboxed = black;
    }
    // Columns that hold the bar's saturated red in any row.
    int best_start = -1, best_end = -1, run_start = -1, gap = 0;
    const auto close_run = [&](int end) {
        if (run_start >= 0 && end - run_start > best_end - best_start) {
            best_start = run_start;
            best_end = end;
        }
        run_start = -1;
    };
    int last_red = -1;
    for (u32 x = 0; x < width; x++) {
        bool red = false;
        for (u32 y = 0; y < height && !red; y++) {
            const u8* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            red = p[0] > 170 && p[1] < 70 && p[2] < 70;
        }
        if (red) {
            if (run_start < 0) {
                run_start = static_cast<int>(x);
            }
            last_red = static_cast<int>(x);
            gap = 0;
        } else if (run_start >= 0 && ++gap > 3) {
            close_run(last_red + 1);
        }
    }
    close_run(last_red + 1);

    const float strip_width = StripRight - StripLeft;
    const float length =
        best_start < 0 ? 0.0f : static_cast<float>(best_end - best_start) / width * strip_width;
    const float right_edge =
        best_start < 0 ? 0.0f : StripLeft + static_cast<float>(best_end) / width * strip_width;
    // The bar is anchored on its right end; anything else red is scenery.
    const bool visible = length > 0.004f && right_edge > 0.915f && right_edge < 0.945f;

    std::lock_guard lock{mutex};
    if (state.phase != Phase::Running) {
        return;
    }
    const auto now = Clock::now();
    if (visible) {
        const bool first = !state.bar_seen;
        state.bar_seen = true;
        // The longest the bar has been is full health (free play starts there).
        state.full_length = std::max(state.full_length, length);
        const float health = std::clamp(length / state.full_length, 0.0f, 1.0f);
        const double hidden_for =
            first ? 0.0 : std::chrono::duration<double>(now - state.last_visible).count();
        // Back at full after vanishing while low: the game restarted the fight, Wander died.
        if (!first && hidden_for > 2.0 && state.health < 0.5f && health > 0.95f) {
            state.damage += state.health;
            Finish(true);
            return;
        }
        // The bar's glowing tip flickers by a few percent; only a clear drop is a hit.
        if (!first && health < state.health - 0.08f) {
            state.damage += state.health - health;
        }
        state.health = health;
        state.last_visible = now;
    }
    if (now - state.last_log > std::chrono::seconds(5)) {
        state.last_log = now;
        LOG_INFO(Lib_NpTrophy, "Challenge: bar {} length {:.4f} full {:.4f} health {:.2f} "
                               "damage {:.2f}",
                 visible ? "visible" : "hidden", length, state.full_length, state.health,
                 state.damage);
    }
}

void OnGameStarted() {
    if (!Enabled()) {
        return;
    }
    std::lock_guard lock{mutex};
    if (state.phase == Phase::Loading) {
        state.phase = Phase::Ready;
    }
}

void OnPlayerInput() {
    if (!Enabled()) {
        return;
    }
    std::lock_guard lock{mutex};
    // Not during the introduction: the clock starts with the first input once the player can move.
    if (state.phase == Phase::Ready && state.strip_seen && !state.letterboxed) {
        state.phase = Phase::Running;
        state.start = Clock::now();
        LOG_INFO(Lib_NpTrophy, "Challenge started: colossus {} ({}), limit {:.0f} s",
                 state.colossus, state.difficulty, state.limit);
    }
}

void OnTrophy(s32 id) {
    if (!Enabled() || id < 22 || id > 37) {
        return;
    }
    std::lock_guard lock{mutex};
    Finish(false);
}

void DebugFinish(bool died) {
    if (!Enabled()) {
        return;
    }
    std::lock_guard lock{mutex};
    Finish(died);
}

namespace {

/// Text whose digits each take the same width, so a running clock does not shake as its
/// figures change. `anchor` is 0 for left, 0.5 for centred, 1 for right aligned at `x`.
void Figures(ImDrawList* draw, float size, float x, float y, float anchor, ImU32 color,
             const std::string& text) {
    float digit = 0.0f;
    for (char c = '0'; c <= '9'; c++) {
        digit = std::max(digit, Ui::TextSize(size, std::string(1, c)).x);
    }
    const auto width_of = [&](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) ? digit
                                                           : Ui::TextSize(size, std::string(1, c)).x;
    };
    float total = 0.0f;
    for (const char c : text) {
        total += width_of(c);
    }
    float at = x - total * anchor;
    for (const char c : text) {
        const std::string one(1, c);
        const float w = width_of(c);
        Ui::Text(draw, size, {at + (w - Ui::TextSize(size, one).x) * 0.5f, y}, color, one);
        at += w;
    }
}

/// A small spaced-out caption, as the game sets its labels.
void Caption(ImDrawList* draw, float size, float x, float y, float anchor, ImU32 color,
             std::string_view text) {
    // In the game's language first; then capitals, a space between the letters. The text is
    // UTF-8: a letter may be several bytes, which stay together, and the capitals of the
    // accented Latin, Cyrillic and Greek letters are found by their places in the code charts.
    const std::string& shown = Lang::Tr(text);
    std::string spaced;
    for (size_t i = 0; i < shown.size();) {
        const unsigned char lead = static_cast<unsigned char>(shown[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (length == 1) {
            spaced += static_cast<char>(std::toupper(lead));
        } else if (length == 2 && i + 1 < shown.size()) {
            u32 cp = ((lead & 0x1f) << 6) | (static_cast<unsigned char>(shown[i + 1]) & 0x3f);
            if ((cp >= 0xe0 && cp <= 0xfe && cp != 0xf7) || (cp >= 0x430 && cp <= 0x44f) ||
                (cp >= 0x3b1 && cp <= 0x3c9 && cp != 0x3c2)) {
                cp -= 0x20;
            } else if (cp == 0x3c2) {
                cp = 0x3a3;
            } else if (cp >= 0x450 && cp <= 0x45f) {
                cp -= 0x50;
            } else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17e)) {
                cp -= (cp & 1) ? 0 : 1;
            } else if (cp >= 0x100 && cp <= 0x177 && cp != 0x131 && cp != 0x138) {
                cp -= cp & 1;
            }
            spaced += static_cast<char>(0xc0 | (cp >> 6));
            spaced += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            spaced.append(shown, i, length);
        }
        spaced += ' ';
        i += length;
    }
    if (!spaced.empty()) {
        spaced.pop_back();
    }
    const float width = Ui::TextSize(size, spaced).x;
    Ui::Text(draw, size, {x - width * anchor, y}, color, spaced);
}

constexpr ImU32 Warm = IM_COL32(232, 132, 104, 255);
constexpr ImU32 Gold = IM_COL32(232, 204, 124, 255);

} // namespace

void Draw() {
    if (!Enabled()) {
        return;
    }
    std::lock_guard lock{mutex};
    // Wander's death: the game puts up its Game Over screen.
    if (state.phase == Phase::Running && Screens::GameOver()) {
        // The blow that killed him took what health he had left; the bar is gone by now.
        state.damage += state.health;
        Finish(true);
    }
    // Retry on the game's Game Over screen starts the fight again: a new attempt, clock and
    // health lost back at nothing, started by the first move as before.
    if (state.phase == Phase::Died) {
        const auto now = Clock::now();
        if (Screens::GameOver()) {
            state.game_over_seen = true;
            state.game_over_last = now;
        } else if (state.game_over_seen && Screens::Arrow()) {
            // End was chosen: the game asks whether to leave for the main menu. That is not a
            // retry; the wait below only counts time with no menu on screen.
            state.game_over_last = now;
            if (MenuExt::AtTitleRoot()) {
                // Yes: the battle is over. Free play goes back to a plain start of the game.
                state.phase = Phase::Off;
                Menu::LeaveFreePlay();
                return;
            }
        } else if (state.game_over_seen && now - state.game_over_last > std::chrono::seconds(2)) {
            state.game_over_seen = false;
            state.phase = Phase::Ready;
            state.damage = 0.0f;
            state.health = 1.0f;
            state.bar_seen = false;
            state.final_time = 0.0;
            LOG_INFO(Lib_NpTrophy, "Challenge restarted after a retry");
        }
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = Ui::Scale();
    const float center = display.x * 0.5f;
    auto* draw = ImGui::GetForegroundDrawList();
    std::string difficulty = state.difficulty;
    if (!difficulty.empty()) {
        difficulty[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(difficulty[0])));
    }
    const std::string name = Names[state.colossus - 1];
    const auto rule = [&](float y, float half_width, ImU32 color) {
        draw->AddLine({center - half_width, y}, {center + half_width, y}, color, 1.5f * scale);
    };
    constexpr ImU32 RuleColor = IM_COL32(236, 233, 222, 110);
    // During the battle the readout is a small panel in the top left corner, clear of the
    // colossus and of the game's own bar, ring, hints and subtitles.
    const float left = 40.0f * scale;
    const float top = 34.0f * scale;
    const float width = 228.0f * scale;
    const float pad = 14.0f * scale;
    const auto panel = [&](float height) {
        draw->AddRectFilled({left, top}, {left + width, top + height}, IM_COL32(6, 6, 6, 104),
                            6.0f * scale);
    };
    const float inner_left = left + pad;
    const float inner_right = left + width - pad;
    const std::string heading = fmt::format("{}  -  {}", name, difficulty);

    if (state.phase == Phase::Loading || state.phase == Phase::Ready) {
        // Nothing over the colossus's introduction: the card waits, like the clock, until the
        // black bars are gone and the player can move.
        if (state.phase == Phase::Loading || !state.strip_seen || state.letterboxed) {
            return;
        }
        panel(104.0f * scale);
        float y = top + 10.0f * scale;
        Caption(draw, 13.0f * scale, inner_left, y, 0.0f, Ui::TextNormal, "Time Attack");
        y += 20.0f * scale;
        Caption(draw, 13.0f * scale, inner_left, y, 0.0f, Ui::TextBright, heading);
        y += 22.0f * scale;
        draw->AddLine({inner_left, y}, {inner_right, y}, RuleColor, 1.0f * scale);
        y += 8.0f * scale;
        Caption(draw, 12.0f * scale, inner_left, y, 0.0f, Ui::TextNormal,
                fmt::format("{} {}", Lang::Tr("Target"), FormatMark(state.limit)));
        y += 18.0f * scale;
        Caption(draw, 12.0f * scale, inner_left, y, 0.0f, Ui::TextNormal,
                "Starts when you move");
        return;
    }
    if (state.phase == Phase::Running) {
        // The clock; under a thin rule, the target on the left and the health lost so far on
        // the right.
        const double elapsed = Elapsed();
        const bool over = elapsed > state.limit;
        panel(104.0f * scale);
        float y = top + 10.0f * scale;
        Caption(draw, 12.0f * scale, inner_left, y, 0.0f, Ui::TextNormal, heading);
        y += 16.0f * scale;
        Figures(draw, 34.0f * scale, inner_left, y, 0.0f, over ? Warm : Ui::TextBright,
                FormatTime(elapsed));
        y += 42.0f * scale;
        draw->AddLine({inner_left, y}, {inner_right, y}, RuleColor, 1.0f * scale);
        y += 8.0f * scale;
        Caption(draw, 12.0f * scale, inner_left, y + 3.0f * scale, 0.0f, Ui::TextNormal, "Target");
        Figures(draw, 17.0f * scale, inner_left + 62.0f * scale, y, 0.0f, Ui::TextBright,
                FormatMark(state.limit));
        const int damage = static_cast<int>(state.damage * 100.0f + 0.5f);
        const std::string lost = fmt::format("{}%", damage);
        Figures(draw, 17.0f * scale, inner_right, y, 1.0f,
                damage == 0 ? Ui::TextNormal : damage > 50 ? Warm : Ui::TextBright, lost);
        Caption(draw, 12.0f * scale, inner_right - 44.0f * scale, y + 3.0f * scale, 1.0f,
                Ui::TextNormal, "Lost");
        return;
    }

    // Results, set out like a page of the game's menus.
    const bool died = state.phase == Phase::Died;
    if (died && state.game_over_seen && !Screens::GameOver() && Screens::Arrow()) {
        return; // the game's own question is up; nothing goes over it
    }
    // After a death the game's Game Over screen is underneath; it is covered whole, so the
    // two pages do not read through each other.
    if (died && Screens::GameOver()) {
        // All of it but the game's Retry and End lines, bottom left, and its confirm hint,
        // bottom right: those are the way on from here.
        constexpr ImU32 Black = IM_COL32(0, 0, 0, 255);
        const float w = display.x, h = display.y;
        draw->AddRectFilled({0.0f, 0.0f}, {w, h * 0.875f}, Black);
        draw->AddRectFilled({w * 0.17f, h * 0.875f}, {w * 0.86f, h}, Black);
        draw->AddRectFilled({0.0f, h * 0.965f}, {w * 0.17f, h}, Black);
        draw->AddRectFilled({w * 0.86f, h * 0.875f}, {w, h * 0.935f}, Black);
        draw->AddRectFilled({w * 0.86f, h * 0.98f}, {w, h}, Black);
    } else {
        Ui::Veil(died ? 1.0f : 0.80f);
    }
    float y = display.y * 0.13f;
    Caption(draw, 26.0f * scale, center, y, 0.5f, Ui::TextNormal, "Time Attack");
    y += 50.0f * scale;
    Ui::TextCentered(draw, 66.0f * scale, center, y, Ui::TextBright, name);
    y += 92.0f * scale;
    Caption(draw, 22.0f * scale, center, y, 0.5f, Ui::TextNormal,
            fmt::format("{}  -  {}", Lang::Tr(difficulty),
                        Lang::Tr(died ? "Wander has fallen" : "the colossus has fallen")));
    y += 54.0f * scale;
    rule(y, 300.0f * scale, RuleColor);
    y += 34.0f * scale;
    Caption(draw, 22.0f * scale, center, y, 0.5f, Ui::TextNormal, "Rank");
    y += 22.0f * scale;
    const char* grade = Grade();
    const ImU32 grade_color = died ? IM_COL32(196, 76, 64, 255)
                              : grade[0] == 'S' ? Gold
                                                : Ui::TextBright;
    Ui::TextCentered(draw, 200.0f * scale, center, y, grade_color, grade);
    y += 236.0f * scale;
    rule(y, 300.0f * scale, RuleColor);
    y += 30.0f * scale;
    // Two columns: what it is, and the figure.
    const float column_left = center - 300.0f * scale;
    const float right = center + 300.0f * scale;
    const float line = 52.0f * scale;
    const float size = 34.0f * scale;
    const auto row = [&](const char* label, const std::string& value, ImU32 color) {
        Caption(draw, 20.0f * scale, column_left, y + 9.0f * scale, 0.0f, Ui::TextNormal, label);
        Figures(draw, size, right, y, 1.0f, color, value);
        y += line;
    };
    const bool in_time = !died && state.final_time <= state.limit;
    row("Time", FormatTime(state.final_time), in_time ? Gold : Ui::TextBright);
    row("Target time", FormatMark(state.limit), Ui::TextNormal);
    row("Typical time", FormatMark(state.average), Ui::TextNormal);
    const int damage = static_cast<int>(state.damage * 100.0f + 0.5f);
    row("Health lost", fmt::format("{}%", damage), damage > 50 ? Warm : Ui::TextBright);
    Ui::Prompt(draw, display.x * 0.95f, display.y * 0.885f, "MENU", Ui::Action::Menu);
}

} // namespace Sotc::Challenge
