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
#include <fmt/format.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "core/sotc/challenge.h"
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
    if (state.phase != Phase::Running) {
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
    if (state.phase == Phase::Ready) {
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

void Draw() {
    if (!Enabled()) {
        return;
    }
    std::lock_guard lock{mutex};
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = Ui::Scale();
    const float center = display.x * 0.5f;
    auto* draw = ImGui::GetForegroundDrawList();
    std::string difficulty = state.difficulty;
    if (!difficulty.empty()) {
        difficulty[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(difficulty[0])));
    }
    const std::string title =
        fmt::format("{}  -  {}", Names[state.colossus - 1], difficulty);

    if (state.phase == Phase::Loading || state.phase == Phase::Ready) {
        Ui::TextCentered(draw, 40.0f * scale, center, 40.0f * scale, Ui::TextBright,
                         "Time Attack");
        Ui::TextCentered(draw, 30.0f * scale, center, 92.0f * scale, Ui::TextNormal, title);
        Ui::TextCentered(draw, 26.0f * scale, center, 134.0f * scale, Ui::TextNormal,
                         fmt::format("Target time {}   -   the clock starts when you move",
                                     FormatTime(state.limit)));
        return;
    }
    if (state.phase == Phase::Running) {
        const double elapsed = Elapsed();
        const bool over = elapsed > state.limit;
        Ui::TextCentered(draw, 56.0f * scale, center, 32.0f * scale,
                         over ? IM_COL32(226, 120, 96, 255) : Ui::TextBright,
                         FormatTime(elapsed));
        Ui::TextCentered(draw, 26.0f * scale, center, 100.0f * scale, Ui::TextNormal,
                         fmt::format("Target {}", FormatTime(state.limit)));
        return;
    }

    // Results, set out like a page of the game's menus.
    const bool died = state.phase == Phase::Died;
    Ui::Veil(0.78f);
    float y = display.y * 0.16f;
    Ui::TextCentered(draw, 34.0f * scale, center, y, Ui::TextNormal, "Time Attack");
    y += 56.0f * scale;
    Ui::TextCentered(draw, 62.0f * scale, center, y, Ui::TextBright, title);
    y += 104.0f * scale;
    Ui::TextCentered(draw, 34.0f * scale, center, y, Ui::TextNormal,
                     died ? "Wander has fallen" : "The colossus has fallen");
    y += 70.0f * scale;
    Ui::TextCentered(draw, 30.0f * scale, center, y, Ui::TextNormal, "Rank");
    y += 30.0f * scale;
    const char* grade = Grade();
    const ImU32 grade_color = died ? IM_COL32(196, 76, 64, 255)
                              : grade[0] == 'S' ? IM_COL32(232, 204, 124, 255)
                                                : Ui::TextBright;
    Ui::TextCentered(draw, 210.0f * scale, center, y, grade_color, grade);
    y += 250.0f * scale;
    // Two columns: what it is, and the figure.
    const float left = center - 250.0f * scale;
    const float right = center + 250.0f * scale;
    const float line = 50.0f * scale;
    const float size = 34.0f * scale;
    const auto row = [&](const char* label, const std::string& value) {
        Ui::Text(draw, size, {left, y}, Ui::TextNormal, label);
        Ui::TextRight(draw, size, right, y, Ui::TextBright, value);
        y += line;
    };
    row("Time", FormatTime(state.final_time));
    row("Target time", FormatTime(state.limit));
    row("Average time", FormatTime(state.average));
    row("Damage taken", fmt::format("{:.0f}%", state.damage * 100.0f));
    Ui::Prompt(draw, display.x * 0.95f, display.y * 0.90f, "MENU", Ui::Action::Menu);
}

} // namespace Sotc::Challenge
