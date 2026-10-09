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
#include "core/sotc/ui.h"

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
    std::string value;       // empty for plain entries
    std::string description; // shown at the bottom while the row is selected
    bool enabled = true;
    bool adjustable = false; // left / right changes the value
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
        rows.push_back({"Free Play", "", "Fight any colossus"});
        rows.push_back({"Achievements", "", "View your achievements"});
        if (InFreePlay()) {
            rows.push_back({"Return to Main Game", "", "Leave free play and load your save"});
        }
        rows.push_back({"Quit Game", "", "Close the game"});
        rows.push_back({"Resume", "", "Return to the game"});
    } else {
        const std::string target = fmt::format(
            "Target time {}{}", Minutes(Challenge::TimeLimit(colossus + 1, difficulty == 2)),
            Challenge::LimitIsEstimate(colossus + 1) ? " (approximate)" : "");
        const char* helper_note = challenge ? "Not available in Time Attack" : "";
        rows.push_back({"Colossus",
                        fmt::format("{}. {}", colossus + 1, Challenge::ColossusName(colossus + 1)),
                        "Choose the colossus to fight", true, true});
        rows.push_back({"Difficulty", Difficulties[difficulty], "Choose the difficulty", true,
                        true});
        rows.push_back({"Time Attack", OnOff(challenge),
                        "Timed battle, ranked from D to S+ on time and damage taken. " + target,
                        true, true});
        rows.push_back({"All Weapons and Items", OnOff(all_items && !challenge),
                        challenge ? helper_note : "Start with every weapon and item", !challenge,
                        true});
        rows.push_back({"Invincibility", OnOff(no_damage && !challenge),
                        challenge ? helper_note : "Wander takes no damage", !challenge, true});
        rows.push_back({"Unlimited Grip", OnOff(unlimited_grip && !challenge),
                        challenge ? helper_note : "The grip gauge never empties", !challenge,
                        true});
        rows.push_back({"Maximum Damage", OnOff(max_damage && !challenge),
                        challenge ? helper_note : "Every sword strike deals full damage",
                        !challenge, true});
        rows.push_back({"Begin Battle", "", "The game restarts and loads the battle"});
        rows.push_back({"Back", "", "Return to the previous menu"});
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
    const bool back = IsKeyPressed(ImGuiKey_Escape, false) ||
                      IsKeyPressed(ImGuiKey_Backspace, false) ||
                      IsKeyPressed(ImGuiKey_GamepadFaceRight, false);

    // Laid out like the game's own menus: title top left, entries below it with a triangle on
    // the selected one, values in a column to the right, prompts bottom right.
    const ImVec2 display = GetIO().DisplaySize;
    const float scale = Ui::Scale();
    auto* draw = GetForegroundDrawList();
    Ui::Veil(0.72f);
    const float title_x = display.x * 0.15f;
    const float label_x = display.x * 0.20f;
    const float value_x = display.x * 0.56f;
    float y = display.y * 0.24f;
    Ui::Text(draw, 60.0f * scale, {title_x, y}, Ui::TextBright,
             page == Page::Main ? "Shadow of the Colossus" : "Free Play");
    y += 104.0f * scale;
    const float row_height = 52.0f * scale;
    const float text_size = 34.0f * scale;
    const ImVec2 mouse = GetIO().MousePos;
    const bool mouse_moved = GetIO().MouseDelta.x != 0.0f || GetIO().MouseDelta.y != 0.0f;
    for (int i = 0; i < count; i++) {
        const Row& row = rows[i];
        const float row_y = y + row_height * i;
        const bool hovered = mouse.x >= title_x && mouse.x <= display.x * 0.75f &&
                             mouse.y >= row_y && mouse.y < row_y + row_height;
        if (hovered && row.enabled) {
            if (mouse_moved) {
                selected = i;
            }
            if (IsMouseClicked(ImGuiMouseButton_Left)) {
                selected = i;
                activate_row = i;
                // A click on the left half of a value steps it back.
                activate_direction = row.adjustable && mouse.x < value_x ? -1 : 0;
            }
        }
        const ImU32 color =
            !row.enabled ? Ui::TextDim : i == selected ? Ui::TextBright : Ui::TextNormal;
        const float text_y = row_y + (row_height - Ui::TextSize(text_size, row.label).y) * 0.5f;
        if (i == selected) {
            Ui::Triangle(draw, {label_x - 22.0f * scale, row_y + row_height * 0.5f},
                         16.0f * scale, true, Ui::TextBright);
        }
        Ui::Text(draw, text_size, {label_x, text_y}, color, row.label);
        if (!row.value.empty()) {
            Ui::TextCentered(draw, text_size, value_x, text_y, color, row.value);
            if (row.adjustable && row.enabled) {
                const float arrow = 13.0f * scale;
                Ui::Triangle(draw, {value_x - 190.0f * scale, row_y + row_height * 0.5f}, arrow,
                             false, color);
                Ui::Triangle(draw, {value_x + 190.0f * scale, row_y + row_height * 0.5f}, arrow,
                             true, color);
            }
        }
    }
    const Row& current = rows[selected];
    if (!current.description.empty()) {
        Ui::TextCentered(draw, 28.0f * scale, display.x * 0.5f, display.y * 0.86f,
                         Ui::TextNormal, current.description);
    }
    const float prompt_x = display.x * 0.95f;
    float prompt_y = display.y * 0.865f;
    if (current.adjustable) {
        prompt_y += Ui::Prompt(draw, prompt_x, prompt_y, "CHANGE", Ui::Action::Change);
    }
    prompt_y += Ui::Prompt(draw, prompt_x, prompt_y, "BACK", Ui::Action::Back);
    if (!current.adjustable) {
        Ui::Prompt(draw, prompt_x, prompt_y, "CONFIRM", Ui::Action::Confirm);
    }

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
