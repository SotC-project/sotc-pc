// SPDX-License-Identifier: GPL-2.0-or-later

// The PC menu: an overlay over the game with the things the console game has no screen for.
// Free play boots straight into a colossus arena through the engine's own start options, which
// are only read at launch, so starting a battle restarts the process with those options.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/devtools/layer.h"
#include "core/sotc/achievements.h"
#include "core/sotc/challenge.h"
#include "core/sotc/menu.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Sotc::Menu {

namespace {

enum class Page { Main, FreePlay };

std::atomic_bool open{false};
Page page = Page::Main;
int selected = 0;

// Free play choices.
int colossus = 0;   // 0-15
int difficulty = 1; // easy, normal, hard
bool challenge = false;
bool all_items = true;
bool no_damage = false;
bool unlimited_grip = false;
bool max_damage = false;

constexpr std::array<const char*, 3> Difficulties{"Easy", "Normal", "Hard"};
constexpr std::array<const char*, 3> DifficultyIds{"easy", "normal", "hard"};

bool InFreePlay() {
    const char* env = std::getenv("SOTC_FREEPLAY");
    return env && *env;
}

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

/// Starts this program again with the given free play options and closes this instance.
void Relaunch(const std::string& freeplay_args, const std::string& challenge_spec) {
#ifdef _WIN32
    SetEnv("SOTC_FREEPLAY", freeplay_args);
    SetEnv("SOTC_CHALLENGE", challenge_spec);
    SetEnv("SOTC_WAIT_PID", std::to_string(GetCurrentProcessId()));
    // Scripted test runs hand the next instance its own script.
    const char* next_script = std::getenv("SOTC_SCRIPT_NEXT");
    SetEnv("SOTC_SCRIPT", next_script ? next_script : "");
    SetEnv("SOTC_SCRIPT_NEXT", "");
    // Same program, same arguments, same folder; the environment carries the request.
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) {
        LOG_ERROR(Core, "Free play: could not restart the game, error {}", GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
#else
    LOG_ERROR(Core, "Free play restart is only implemented on Windows");
#endif
}

void StartBattle() {
    std::string args = fmt::format("+nointro +skipSplash=true +boss={}", colossus);
    if (difficulty == 2) {
        args += " +shadow_setHardMode=true";
    } else if (difficulty == 0) {
        args += " +shadow_setEasyMode=true";
    }
    // A graded run is played without help.
    if (!challenge) {
        if (all_items) {
            args += " +shadow_startWithAllItems=true";
        }
        if (no_damage) {
            args += " +shadow_noDamage=true";
        }
        if (unlimited_grip) {
            args += " +shadow_unlimitedGrip=true";
        }
        if (max_damage) {
            args += " +shadow_maxAttackDamage=true";
        }
    }
    Relaunch(args,
             challenge ? fmt::format("{},{}", colossus + 1, DifficultyIds[difficulty]) : "");
}

struct Row {
    std::string label;
    std::string value; // shown right aligned; empty for plain entries
    bool enabled = true;
};

std::string OnOff(bool value) {
    return value ? "On" : "Off";
}

std::string Minutes(double seconds) {
    const int total = static_cast<int>(seconds);
    return fmt::format("{}:{:02d}", total / 60, total % 60);
}

std::vector<Row> BuildRows() {
    std::vector<Row> rows;
    if (page == Page::Main) {
        rows.push_back({"Free play", ">"});
        rows.push_back({"Achievements", ">"});
        if (InFreePlay()) {
            rows.push_back({"Return to the main game", ""});
        }
        rows.push_back({"Quit game", ""});
        rows.push_back({"Close", ""});
    } else {
        rows.push_back({"Colossus", fmt::format("< {}. {} >", colossus + 1,
                                                Challenge::ColossusName(colossus + 1))});
        rows.push_back({"Difficulty", fmt::format("< {} >", Difficulties[difficulty])});
        rows.push_back({"Challenge (timed, graded D to S+, F on death)", OnOff(challenge)});
        rows.push_back({"All weapons and items", OnOff(all_items && !challenge), !challenge});
        rows.push_back({"Take no damage", OnOff(no_damage && !challenge), !challenge});
        rows.push_back({"Unlimited grip", OnOff(unlimited_grip && !challenge), !challenge});
        rows.push_back({"Maximum attack damage", OnOff(max_damage && !challenge), !challenge});
        rows.push_back({"Start battle (restarts the game)", ""});
        rows.push_back({"Back", ""});
    }
    return rows;
}

/// direction: 0 = confirm, -1 / +1 = left / right.
void Activate(int row, int direction) {
    if (page == Page::Main) {
        int index = row;
        if (!InFreePlay() && index >= 2) {
            index++; // skip the absent "return" entry
        }
        if (direction != 0) {
            return;
        }
        switch (index) {
        case 0:
            page = Page::FreePlay;
            selected = 0;
            break;
        case 1:
            open = false;
            if (!Achievements::IsPanelOpen()) {
                Achievements::TogglePanel();
            }
            break;
        case 2:
            Relaunch("", "");
            break;
        case 3: {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
            break;
        }
        default:
            open = false;
            break;
        }
        return;
    }
    const int step = direction == 0 ? 1 : direction;
    switch (row) {
    case 0:
        colossus = (colossus + step + 16) % 16;
        break;
    case 1:
        difficulty = (difficulty + step + 3) % 3;
        break;
    case 2:
        challenge = !challenge;
        break;
    case 3:
        all_items = !all_items;
        break;
    case 4:
        no_damage = !no_damage;
        break;
    case 5:
        unlimited_grip = !unlimited_grip;
        break;
    case 6:
        max_damage = !max_damage;
        break;
    case 7:
        if (direction == 0) {
            StartBattle();
        }
        break;
    default:
        if (direction == 0) {
            page = Page::Main;
            selected = 0;
        }
        break;
    }
}

} // namespace

void Toggle() {
    if (!open) {
        page = Page::Main;
        selected = 0;
    }
    open = !open;
}

bool IsOpen() {
    return open;
}

void Draw() {
    using namespace ImGui;

    const bool pad_combo =
        (IsKeyDown(ImGuiKey_GamepadL3) && IsKeyPressed(ImGuiKey_GamepadR3, false)) ||
        (IsKeyDown(ImGuiKey_GamepadR3) && IsKeyPressed(ImGuiKey_GamepadL3, false));
    if (pad_combo && !Achievements::IsPanelOpen()) {
        Toggle();
    }
    if (!open) {
        return;
    }

    const std::vector<Row> rows = BuildRows();
    const int count = static_cast<int>(rows.size());
    const auto step_to_enabled = [&](int direction) {
        for (int i = 0; i < count; i++) {
            selected = (selected + direction + count) % count;
            if (rows[selected].enabled) {
                break;
            }
        }
    };
    if (IsKeyPressed(ImGuiKey_DownArrow) || IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
        IsKeyPressed(ImGuiKey_S)) {
        step_to_enabled(1);
    }
    if (IsKeyPressed(ImGuiKey_UpArrow) || IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
        IsKeyPressed(ImGuiKey_W)) {
        step_to_enabled(-1);
    }
    selected = std::clamp(selected, 0, count - 1);
    int activate_row = -1, activate_direction = 0;
    if (IsKeyPressed(ImGuiKey_Enter, false) || IsKeyPressed(ImGuiKey_Space, false) ||
        IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
        activate_row = selected;
    } else if (IsKeyPressed(ImGuiKey_LeftArrow) || IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
               IsKeyPressed(ImGuiKey_A)) {
        activate_row = selected;
        activate_direction = -1;
    } else if (IsKeyPressed(ImGuiKey_RightArrow) || IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
               IsKeyPressed(ImGuiKey_D)) {
        activate_row = selected;
        activate_direction = 1;
    }
    const bool back = IsKeyPressed(ImGuiKey_Escape, false) || IsKeyPressed(ImGuiKey_Backspace, false) ||
                      IsKeyPressed(ImGuiKey_GamepadFaceRight, false);

    const ImVec2 display = GetIO().DisplaySize;
    const float scale = display.y / 720.0f;
    const float width = (page == Page::Main ? 380.0f : 620.0f) * scale;
    SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    SetNextWindowSize({width, 0.0f}, ImGuiCond_Always);
    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(30, 31, 28, 240));
    PushStyleColor(ImGuiCol_Border, IM_COL32(120, 124, 108, 255));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0f * scale, 16.0f * scale});
    PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    if (Begin("SotcMenu", nullptr,
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                  ImGuiWindowFlags_AlwaysAutoResize)) {
        SetWindowFontScale(1.5f * scale);
        TextColored({0.90f, 0.88f, 0.80f, 1.0f}, "%s",
                    page == Page::Main ? "Shadow of the Colossus" : "Free play");
        SetWindowFontScale(1.1f * scale);
        Dummy({0.0f, 6.0f * scale});
        auto* draw = GetWindowDrawList();
        const float row_height = GetTextLineHeight() + 14.0f * scale;
        for (int i = 0; i < count; i++) {
            const Row& row = rows[i];
            const ImVec2 origin = GetCursorScreenPos();
            const float row_width = GetContentRegionAvail().x;
            PushID(i);
            if (InvisibleButton("row", {row_width, row_height}) && row.enabled) {
                selected = i;
                activate_row = i;
                activate_direction = 0;
            }
            if (IsItemHovered() && row.enabled &&
                (GetIO().MouseDelta.x != 0.0f || GetIO().MouseDelta.y != 0.0f)) {
                selected = i;
            }
            PopID();
            if (i == selected) {
                draw->AddRectFilled(origin, {origin.x + row_width, origin.y + row_height - 2.0f},
                                    IM_COL32(74, 96, 52, 235), 3.0f);
            }
            const ImU32 color = !row.enabled   ? IM_COL32(110, 110, 104, 255)
                                : i == selected ? IM_COL32(244, 240, 222, 255)
                                                : IM_COL32(200, 200, 188, 255);
            const float text_y = origin.y + 6.0f * scale;
            draw->AddText({origin.x + 10.0f * scale, text_y}, color, row.label.c_str());
            if (!row.value.empty()) {
                const float value_width = CalcTextSize(row.value.c_str()).x;
                draw->AddText({origin.x + row_width - value_width - 10.0f * scale, text_y}, color,
                              row.value.c_str());
            }
        }
        Dummy({0.0f, 4.0f * scale});
        SetWindowFontScale(0.95f * scale);
        if (page == Page::FreePlay) {
            const bool hard = difficulty == 2;
            TextColored({0.72f, 0.74f, 0.66f, 1.0f},
                        "Time attack limit %s%s     Typical player time %s (estimate)",
                        Minutes(Challenge::TimeLimit(colossus + 1, hard)).c_str(),
                        Challenge::LimitIsEstimate(colossus + 1) ? " (estimate)" : "",
                        Minutes(Challenge::AverageTime(colossus + 1)).c_str());
        }
        PushStyleColor(ImGuiCol_Text, IM_COL32(158, 163, 148, 255));
        TextWrapped("Up / Down: choose    Left / Right: change    Enter / Cross: select    "
                    "Esc / Circle: back");
        PopStyleColor();
    }
    End();
    PopStyleVar(3);
    PopStyleColor(2);

    if (activate_row >= 0 && rows[activate_row].enabled) {
        Activate(activate_row, activate_direction);
    } else if (back) {
        if (page == Page::FreePlay) {
            page = Page::Main;
            selected = 0;
        } else {
            open = false;
        }
    }
}

std::filesystem::path LaunchOptionsFile(const std::filesystem::path& original) {
    static std::mutex mutex;
    static bool built = false;
    static std::filesystem::path replacement;
    std::lock_guard lock{mutex};
    if (!built) {
        built = true;
        const char* env = std::getenv("SOTC_FREEPLAY");
        if (env && *env) {
            std::error_code ec;
            const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "temp";
            std::filesystem::create_directories(dir, ec);
            const auto path = dir / "CommandLineArgs.freeplay.txt";
            std::ifstream in(original, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            while (!text.empty() &&
                   (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
                text.pop_back();
            }
            // One option per line with CRLF ends, as the file is written elsewhere.
            std::string args = env;
            for (size_t pos = 0; (pos = args.find(' ', pos)) != std::string::npos; pos += 2) {
                args.replace(pos, 1, "\r\n");
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text << "\r\n" << args << "\r\n";
            if (out) {
                replacement = path;
                LOG_INFO(Core, "Free play launch options: {}", env);
            }
        }
    }
    return replacement.empty() ? original : replacement;
}

void WaitForPreviousInstance() {
#ifdef _WIN32
    const char* env = std::getenv("SOTC_WAIT_PID");
    if (!env || !*env) {
        return;
    }
    const DWORD pid = static_cast<DWORD>(std::strtoul(env, nullptr, 10));
    _putenv_s("SOTC_WAIT_PID", "");
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }
#endif
}

} // namespace Sotc::Menu
