// SPDX-License-Identifier: GPL-2.0-or-later

// The PC menu: an overlay over the game with the things the console game has no screen for.
// Free play boots straight into a colossus arena through the engine's own start options, which
// are only read at launch, so starting a battle restarts the process with those options.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "common/elf_info.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "input/input_handler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "core/devtools/layer.h"
#include "core/sotc/achievements.h"
#include "core/sotc/challenge.h"
#include "core/sotc/menu.h"
#include "core/sotc/ui.h"

#ifdef _WIN32
#include <windows.h>
#endif

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace Sotc::Menu {

namespace {

enum class Page { Main, FreePlay, Graphics, Controls };

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
    std::function<void(int)> act; // 0 = confirm, -1 / +1 = left / right
};

std::string OnOff(bool value) {
    return value ? "On" : "Off";
}

std::string Minutes(double seconds) {
    const int total = static_cast<int>(seconds);
    return fmt::format("{}:{:02d}", total / 60, total % 60);
}

std::string Capitalized(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
    return text;
}

void GoTo(Page next) {
    page = next;
    selected = 0;
}

void QuitGame() {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

/// The value written after "key": in a flat JSON text, without quotes; empty when absent.
std::string JsonValue(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\"");
    if (at == std::string::npos) {
        return {};
    }
    auto start = text.find(':', at);
    if (start == std::string::npos) {
        return {};
    }
    start = text.find_first_not_of(" \t\"", start + 1);
    const auto end = text.find_first_of(",\"\r\n}", start);
    return text.substr(start, end - start);
}

// ---- Graphics and other settings ----
// The same options Settings.bat offers. They are engine start options and program settings, read
// when the game starts, so applying them writes them through Settings.ps1 and restarts.

struct Option {
    const char* label;
    const char* param; // Settings.ps1 parameter
    std::vector<std::string> values;
    const char* description;
    int index = 0;
};

std::vector<Option> options;
std::string apply_error;

const std::vector<std::string> OnOffValues{"off", "on"};

Option* FindOption(std::string_view param) {
    for (Option& option : options) {
        if (param == option.param) {
            return &option;
        }
    }
    return nullptr;
}

void SetOption(std::string_view param, const std::string& value) {
    if (Option* option = FindOption(param)) {
        for (size_t i = 0; i < option->values.size(); i++) {
            if (option->values[i] == value) {
                option->index = static_cast<int>(i);
            }
        }
    }
}

void LoadOptions() {
    options = {
        {"Preset", "Preset", {"custom", "low", "medium", "high", "ultra"},
         "Sets the detail options below together"},
        {"Frame Rate Limit", "Fps", {"30", "60", "90", "120", "144", "165", "240", "uncapped"},
         "Highest frame rate the game will run at"},
        {"VSync", "VSync", OnOffValues, "Removes tearing; holds the frame rate to the display's"},
        {"Fullscreen", "Fullscreen", OnOffValues, "Fill the screen instead of a window"},
        {"Window Size", "Window", {"720p", "1080p", "1440p", "4k"}, "Size of the game window"},
        {"FSR Upscaling", "Upscaling", OnOffValues,
         "Sharper picture when the window is larger than 1080p. Switches at once"},
        {"Shadows", "Shadows", OnOffValues, "Sun and spot shadows"},
        {"Motion Blur", "MotionBlur", OnOffValues, "Blur on fast movement"},
        {"Bloom", "Bloom", OnOffValues, "Glow around bright light"},
        {"Depth of Field", "DepthOfField", OnOffValues, "Soft focus in the distance and in cutscenes"},
        {"Vignette", "Vignette", OnOffValues, "Darkened picture corners"},
        {"Fur", "Fur", OnOffValues, "Fur on the colossi"},
        {"Fog", "Fog", OnOffValues, "Distance fog"},
        {"Mist Particles", "MistParticles", OnOffValues,
         "Drifting mist. Renders far too bright in this build; best left off"},
        {"Texture Filtering", "TextureFiltering", {"1x", "2x", "4x", "8x", "16x"},
         "Sharpness of surfaces seen at an angle"},
        {"Draw Distance", "DrawDistance", {"reduced", "full"},
         "How far small objects are drawn. Full costs frame rate"},
        {"Texture Detail", "TextureDetail", {"low", "normal", "high"}, "Sharpness of textures"},
        {"Sharpening", "Sharpening", {"off", "normal", "strong"}, "Sharpening of the final picture"},
        {"Button Icons", "Prompts", {"auto", "keyboard", "xbox", "playstation"},
         "Auto follows the device you are using"},
        {"Achievement Sound", "AchievementVolume", {"0", "25", "50", "75", "100"},
         "Loudness of the achievement sound, percent"},
    };
    // Defaults, as Settings.ps1 has them, for options no file names yet.
    for (const char* param : {"Shadows", "MotionBlur", "Bloom", "DepthOfField", "Vignette", "Fur", "Fog"}) {
        SetOption(param, "on");
    }
    SetOption("TextureFiltering", "4x");
    SetOption("TextureDetail", "normal");
    SetOption("Sharpening", "normal");
    const auto user = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    const std::string graphics = ReadText(user / "sotc_graphics.json");
    for (Option& option : options) {
        const std::string value = JsonValue(graphics, option.param);
        if (!value.empty()) {
            SetOption(option.param, value);
        }
    }
    const std::string config = ReadText(user / "config.json");
    const int clock = std::atoi(JsonValue(config, "vblank_frequency").c_str());
    // The game shows every second tick of the frame clock, so the clock runs at twice the limit.
    SetOption("Fps", clock >= 1000 || clock <= 0 ? "uncapped" : std::to_string(clock / 2));
    SetOption("VSync", JsonValue(config, "present_mode") == "Fifo" ? "on" : "off");
    SetOption("Fullscreen", JsonValue(config, "full_screen") == "true" ? "on" : "off");
    SetOption("Upscaling", JsonValue(config, "fsr_enabled") == "true" ? "on" : "off");
    const int height = std::atoi(JsonValue(config, "window_height").c_str());
    SetOption("Window", height >= 2160 ? "4k" : height >= 1440 ? "1440p" : height >= 1080 ? "1080p" : "720p");
    const std::string forced = ReadText(user / "sotc_textures" / "force.txt");
    SetOption("Prompts", "auto");
    for (const char* set : {"keyboard", "xbox", "playstation"}) {
        if (forced.rfind(set, 0) == 0) {
            SetOption("Prompts", set);
        }
    }
    const std::string volume = ReadText(user / "sotc_achievement_volume.txt");
    SetOption("AchievementVolume", "100");
    if (!volume.empty()) {
        const int percent = std::clamp(std::atoi(volume.c_str()), 0, 100);
        SetOption("AchievementVolume", std::to_string((percent + 12) / 25 * 25));
    }
    apply_error.clear();
}

/// What each preset sets, as Settings.ps1 defines them.
void ApplyPreset(const std::string& preset) {
    struct Values {
        const char* name;
        const char *shadows, *blur, *dof, *fur, *filtering, *distance, *detail;
    };
    static constexpr Values Presets[]{
        {"low", "off", "off", "off", "off", "2x", "reduced", "low"},
        {"medium", "on", "off", "on", "on", "4x", "reduced", "normal"},
        {"high", "on", "on", "on", "on", "8x", "full", "normal"},
        {"ultra", "on", "on", "on", "on", "16x", "full", "high"},
    };
    for (const Values& values : Presets) {
        if (preset == values.name) {
            SetOption("Shadows", values.shadows);
            SetOption("MotionBlur", values.blur);
            SetOption("DepthOfField", values.dof);
            SetOption("Fur", values.fur);
            SetOption("TextureFiltering", values.filtering);
            SetOption("DrawDistance", values.distance);
            SetOption("TextureDetail", values.detail);
            SetOption("Bloom", "on");
            SetOption("Vignette", "on");
            SetOption("Fog", "on");
            SetOption("MistParticles", "off");
            SetOption("Sharpening", "normal");
        }
    }
}

/// Writes the chosen options through Settings.ps1 and restarts the game with them.
void ApplyOptions() {
#ifdef _WIN32
    std::error_code ec;
    const auto script = std::filesystem::current_path(ec) / "Settings.ps1";
    if (!std::filesystem::exists(script, ec)) {
        apply_error = "Settings.ps1 was not found next to the game";
        return;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto game_dir = mnt->GetHostPath("/app0");
    std::wstring command = L"pwsh.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
                           script.wstring() + L"\" -GameDir \"" + game_dir.wstring() + L"\"";
    for (const Option& option : options) {
        if (std::string_view{option.param} == "Preset") {
            continue; // its effect is already in the options it sets
        }
        const std::string value = option.values[option.index];
        command += L" -" + std::wstring(option.param, option.param + std::strlen(option.param)) +
                   L" " + std::wstring(value.begin(), value.end());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        apply_error = "PowerShell 7 (pwsh) could not be started";
        return;
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        apply_error = "The settings could not be saved";
        return;
    }
    const char* freeplay = std::getenv("SOTC_FREEPLAY");
    const char* challenge_spec = std::getenv("SOTC_CHALLENGE");
    Relaunch(freeplay ? freeplay : "", challenge_spec ? challenge_spec : "");
#else
    apply_error = "Only available on Windows";
#endif
}

// ---- Key bindings ----

struct Binding {
    const char* label;
    const char* output; // name in the input file
    std::string key;    // current keyboard or mouse input
};

std::vector<Binding> bindings{
    {"Move Forward", "axis_left_y_minus"}, {"Move Back", "axis_left_y_plus"},
    {"Move Left", "axis_left_x_minus"},    {"Move Right", "axis_left_x_plus"},
    {"Jump", "cross"},                     {"Attack", "square"},
    {"Grab / Use / Pray", "r2"},           {"Crouch / Roll", "circle"},
    {"Raise Sword / Aim", "r1"},           {"Mount / Spur Agro", "triangle"},
    {"Walk / Run", "walk_toggle"},
    {"View Colossus", "l2"},               {"Camera Focus", "l1"},
    {"Change Sword", "pad_right"},         {"Change Ranged Weapon", "pad_left"},
    {"Unequip", "pad_up"},                 {"Photo Mode", "pad_down"},
    {"World Map", "touchpad_center"},      {"Pause", "options"},
    {"Zoom (R3)", "r3"},                   {"L3", "l3"},
};
std::atomic<int> capture_row{-1};
// The menu was opened straight on a page (free play from the title menu): Back closes it.
bool opened_on_page = false;
bool wait_for_release = false;
std::atomic_bool bindings_dirty{true};
std::mutex bindings_mutex;

std::filesystem::path BindingsFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "input_config" /
           (std::string(Common::ElfInfo::Instance().GameSerial()) + ".ini");
}

/// Splits "output = input  # note" into its two names; false for other lines.
bool ParseBindingLine(const std::string& line, std::string& output, std::string& input) {
    std::string text = line.substr(0, line.find('#'));
    const auto equals = text.find('=');
    if (equals == std::string::npos) {
        return false;
    }
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r");
        const auto last = value.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
    };
    output = trim(text.substr(0, equals));
    input = trim(text.substr(equals + 1));
    return !output.empty() && !input.empty();
}

bool IsKeyboardInput(const std::string& name) {
    return Input::string_to_keyboard_key_map.contains(name);
}

void LoadBindings() {
    std::lock_guard lock{bindings_mutex};
    for (Binding& binding : bindings) {
        binding.key.clear();
    }
    std::ifstream file(BindingsFile());
    std::string line, output, input;
    while (std::getline(file, line)) {
        if (!ParseBindingLine(line, output, input) || !IsKeyboardInput(input)) {
            continue;
        }
        for (Binding& binding : bindings) {
            if (output == binding.output && binding.key.empty()) {
                binding.key = input;
            }
        }
    }
    bindings_dirty = false;
}

/// Redraws the key pictures the game's hints use from the bindings file (glyphs.py, needs
/// Python with Pillow), then has the texture cache read them again. Runs beside the game.
void RegenerateGlyphs() {
#ifdef _WIN32
    std::error_code ec;
    const auto script = std::filesystem::current_path(ec) / "glyphs.py";
    if (!std::filesystem::exists(script, ec)) {
        return;
    }
    const auto textures = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "sotc_textures";
    std::wstring command = L"python.exe \"" + script.wstring() + L"\" \"" + textures.wstring() +
                           L"\" --ini \"" + BindingsFile().wstring() + L"\"";
    std::thread{[command]() mutable {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process)) {
            return;
        }
        WaitForSingleObject(process.hProcess, 30000);
        DWORD exit_code = 1;
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (exit_code == 0) {
            ++VideoCore::g_pack_generation;
        }
    }}.detach();
#endif
}

/// Gives `output` the keyboard or mouse input `key` (taking it from any other action) and
/// reloads the bindings.
void Rebind(const std::string& output, const std::string& key) {
    const auto path = BindingsFile();
    std::vector<std::string> lines;
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
    }
    std::vector<std::string> kept;
    size_t insert_at = std::string::npos;
    std::string line_output, line_input;
    for (const std::string& line : lines) {
        if (ParseBindingLine(line, line_output, line_input) && IsKeyboardInput(line_input) &&
            (line_output == output || line_input == key)) {
            if (line_output == output && insert_at == std::string::npos) {
                insert_at = kept.size();
            }
            continue;
        }
        kept.push_back(line);
    }
    const std::string added = output + " = " + key;
    if (insert_at == std::string::npos) {
        kept.push_back(added);
    } else {
        kept.insert(kept.begin() + insert_at, added);
    }
    {
        std::ofstream file(path, std::ios::trunc);
        for (const std::string& line : kept) {
            file << line << "\n";
        }
    }
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
    bindings_dirty = true;
    RegenerateGlyphs();
}

std::string KeyLabel(const std::string& key) {
    static const std::map<std::string, std::string> Names{
        {"leftbutton", "Left Mouse"},   {"rightbutton", "Right Mouse"},
        {"middlebutton", "Middle Mouse"}, {"mousewheelup", "Wheel Up"},
        {"mousewheeldown", "Wheel Down"}, {"lshift", "Left Shift"},
        {"rshift", "Right Shift"},       {"lctrl", "Left Ctrl"},
        {"rctrl", "Right Ctrl"},         {"lalt", "Left Alt"},
        {"space", "Space"},              {"enter", "Enter"},
        {"escape", "Esc"},               {"tab", "Tab"},
        {"backspace", "Backspace"},      {"up", "Up"},
        {"down", "Down"},                {"left", "Left"},
        {"right", "Right"},
    };
    if (key.empty()) {
        return "-";
    }
    if (const auto it = Names.find(key); it != Names.end()) {
        return it->second;
    }
    std::string upper = key;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return upper;
}

// ---- Pages ----

std::vector<Row> BuildRows() {
    std::vector<Row> rows;
    if (page == Page::Main) {
        // Outside free play this page is the PC settings; free play and the achievements have
        // their own lines in the game's menus.
        if (InFreePlay()) {
            rows.push_back({"Free Play", "", "Fight any colossus", true, false,
                            [](int) { GoTo(Page::FreePlay); }});
        }
        rows.push_back({"Settings", "", "Graphics, display, button icons and sound",
                        true, false, [](int) {
                            LoadOptions();
                            GoTo(Page::Graphics);
                        }});
        rows.push_back({"Key Bindings", "", "Choose the keys and mouse buttons", true, false,
                        [](int) {
                            bindings_dirty = true;
                            GoTo(Page::Controls);
                        }});
        if (InFreePlay()) {
            rows.push_back({"Achievements", "", "View your achievements", true, false, [](int) {
                                open = false;
                                if (!Achievements::IsPanelOpen()) {
                                    Achievements::TogglePanel();
                                }
                            }});
        }
        if (InFreePlay()) {
            rows.push_back({"Return to Title", "", "Leave free play; the game restarts", true,
                            false, [](int) { Relaunch("", ""); }});
        }
        rows.push_back({"Quit Game", "", "Close the game", true, false, [](int) { QuitGame(); }});
        rows.push_back({"Resume", "", "Return to the game", true, false,
                        [](int) { open = false; }});
    } else if (page == Page::FreePlay) {
        const std::string target = fmt::format(
            "Target time {}{}", Minutes(Challenge::TimeLimit(colossus + 1, difficulty == 2)),
            Challenge::LimitIsEstimate(colossus + 1) ? " (approximate)" : "");
        const char* helper_note = "Not available in Time Attack";
        const auto step = [](int direction) { return direction == 0 ? 1 : direction; };
        rows.push_back({"Colossus",
                        fmt::format("{}. {}", colossus + 1, Challenge::ColossusName(colossus + 1)),
                        "Choose the colossus to fight", true, true,
                        [step](int d) { colossus = (colossus + step(d) + 16) % 16; }});
        rows.push_back({"Difficulty", Difficulties[difficulty], "Choose the difficulty", true,
                        true, [step](int d) { difficulty = (difficulty + step(d) + 3) % 3; }});
        rows.push_back({"Time Attack", OnOff(challenge),
                        "Timed battle on fixed terms, ranked from D to S+ on time and damage taken. " + target,
                        true, true, [](int) { challenge = !challenge; }});
        // A time attack is fought on fixed terms: the start the game gives every colossus
        // battle, and none of the helpers. Only the difficulty is the player's choice.
        if (!challenge) {
            rows.push_back({"All Weapons and Items", OnOff(all_items && !challenge),
                            challenge ? helper_note : "Start with every weapon and item", !challenge,
                            true, [](int) { all_items = !all_items; }});
            rows.push_back({"Invincibility", OnOff(no_damage && !challenge),
                            challenge ? helper_note : "Wander takes no damage", !challenge, true,
                            [](int) { no_damage = !no_damage; }});
            rows.push_back({"Unlimited Grip", OnOff(unlimited_grip && !challenge),
                            challenge ? helper_note : "The grip gauge never empties", !challenge,
                            true, [](int) { unlimited_grip = !unlimited_grip; }});
            rows.push_back({"Maximum Damage", OnOff(max_damage && !challenge),
                            challenge ? helper_note : "Every sword strike deals full damage",
                            !challenge, true, [](int) { max_damage = !max_damage; }});
        }
        rows.push_back({"Begin Battle", "", "The game restarts and loads the battle", true, false,
                        [](int d) {
                            if (d == 0) {
                                StartBattle();
                            }
                        }});
        rows.push_back({"Back", "", "Return to the previous menu", true, false, [](int d) {
                            if (d == 0) {
                                if (opened_on_page) {
                                    open = false;
                                } else {
                                    GoTo(Page::Main);
                                }
                            }
                        }});
    } else if (page == Page::Graphics) {
        for (size_t i = 0; i < options.size(); i++) {
            Option& option = options[i];
            rows.push_back({option.label, Capitalized(option.values[option.index]),
                            option.description, true, true, [i](int d) {
                                Option& changed = options[i];
                                const int count = static_cast<int>(changed.values.size());
                                changed.index = (changed.index + (d == 0 ? 1 : d) + count) % count;
                                const std::string_view param{changed.param};
                                if (param == "Upscaling" && presenter) {
                                    // The runtime's own filter, so it can switch while playing.
                                    presenter->GetFsrSettingsRef().enable =
                                        changed.values[changed.index] == "on";
                                }
                                if (param == "Preset") {
                                    ApplyPreset(changed.values[changed.index]);
                                } else if (param != "Fps" && param != "VSync" &&
                                           param != "Fullscreen" && param != "Window" &&
                                           param != "Upscaling" && param != "Prompts" &&
                                           param != "AchievementVolume") {
                                    SetOption("Preset", "custom");
                                }
                            }});
        }
        rows.push_back({"Apply and Restart", "",
                        apply_error.empty() ? "Saves the settings; the game restarts to use them"
                                            : apply_error,
                        true, false, [](int d) {
                            if (d == 0) {
                                ApplyOptions();
                            }
                        }});
        rows.push_back({"Back", "", "Leave without saving", true, false, [](int d) {
                            if (d == 0) {
                                GoTo(Page::Main);
                            }
                        }});
    } else {
        if (bindings_dirty) {
            LoadBindings();
        }
        std::lock_guard lock{bindings_mutex};
        for (size_t i = 0; i < bindings.size(); i++) {
            const bool capturing = capture_row == static_cast<int>(i);
            rows.push_back({bindings[i].label,
                            capturing ? "Press a key or mouse button" : KeyLabel(bindings[i].key),
                            capturing ? "Esc cancels" : "Select, then press the new key or button",
                            true, false, [i](int d) {
                                if (d == 0) {
                                    capture_row = static_cast<int>(i);
                                }
                            }});
        }
        rows.push_back({"Back", "", "Changes are saved as you make them", true, false, [](int d) {
                            if (d == 0) {
                                GoTo(Page::Main);
                            }
                        }});
    }
    return rows;
}

const char* PageTitle() {
    switch (page) {
    case Page::FreePlay:
        return "Free Play";
    case Page::Graphics:
        return "Settings";
    case Page::Controls:
        return "Key Bindings";
    default:
        return InFreePlay() ? "Shadow of the Colossus" : "PC Settings";
    }
}

} // namespace

void Toggle() {
    if (!open) {
        GoTo(Page::Main);
        opened_on_page = false;
    }
    capture_row = -1;
    open = !open;
}

bool IsOpen() {
    return open;
}

void OpenFreePlay() {
    capture_row = -1;
    GoTo(Page::FreePlay);
    opened_on_page = true;
    wait_for_release = true;
    open = true;
}

void OpenSettings() {
    capture_row = -1;
    GoTo(Page::Main);
    opened_on_page = false;
    wait_for_release = true;
    open = true;
}

namespace {
std::atomic<int> flow_starts{0};
std::atomic<u64> second_start_ms{0};
} // namespace

void OnGameFlowStart() {
    if (++flow_starts == 2) {
        second_start_ms = SDL_GetTicks();
    }
}

bool StartCovered() {
    static const bool free_play = InFreePlay();
    if (!free_play) {
        return false;
    }
    const u64 now = SDL_GetTicks();
    // Never for longer than a start can take, whatever the game does.
    if (now > 90000) {
        return false;
    }
    const u64 second = second_start_ms.load();
    // The game's own loading screen is up shortly after the second announcement.
    return second == 0 || now - second < 700;
}

void HotkeyToggle() {
    if (open || InFreePlay()) {
        Toggle();
    }
}

bool CaptureInput(const SDL_Event* event) {
    const int row = capture_row;
    if (row < 0 || !open) {
        return false;
    }
    std::string key;
    if (event->type == SDL_EVENT_KEY_DOWN) {
        if (event->key.key == SDLK_ESCAPE) {
            capture_row = -1;
            return true;
        }
        for (const auto& [name, code] : Input::string_to_keyboard_key_map) {
            if (code == event->key.key && !name.ends_with("button") &&
                !name.starts_with("mousewheel")) {
                key = name;
                break;
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        key = event->button.button == SDL_BUTTON_LEFT     ? "leftbutton"
              : event->button.button == SDL_BUTTON_RIGHT  ? "rightbutton"
              : event->button.button == SDL_BUTTON_MIDDLE ? "middlebutton"
                                                          : "";
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        key = event->wheel.y > 0 ? "mousewheelup" : event->wheel.y < 0 ? "mousewheeldown" : "";
    }
    if (!key.empty() && IsKeyboardInput(key)) {
        Rebind(bindings[row].output, key);
        capture_row = -1;
    }
    // While waiting for a key, nothing else gets the input.
    return true;
}

void Draw() {
    using namespace ImGui;

    const bool pad_combo =
        (IsKeyDown(ImGuiKey_GamepadL3) && IsKeyPressed(ImGuiKey_GamepadR3, false)) ||
        (IsKeyDown(ImGuiKey_GamepadR3) && IsKeyPressed(ImGuiKey_GamepadL3, false));
    if (pad_combo && !Achievements::IsPanelOpen()) {
        HotkeyToggle();
    }
    if (!open) {
        return;
    }

    const std::vector<Row> rows = BuildRows();
    const int count = static_cast<int>(rows.size());
    const bool capturing = capture_row >= 0;
    const auto step_to_enabled = [&](int direction) {
        for (int i = 0; i < count; i++) {
            selected = (selected + direction + count) % count;
            if (rows[selected].enabled) {
                break;
            }
        }
    };
    int activate_row = -1, activate_direction = 0;
    bool back = false;
    // Opened from a line of the game's menu: the press that chose that line is still held, and
    // must not also choose a row here.
    if (wait_for_release) {
        wait_for_release = IsKeyDown(ImGuiKey_Enter) || IsKeyDown(ImGuiKey_Space) ||
                           IsKeyDown(ImGuiKey_GamepadFaceDown) || IsMouseDown(0);
    }
    if (!capturing && !wait_for_release) {
        if (IsKeyPressed(ImGuiKey_DownArrow) || IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
            IsKeyPressed(ImGuiKey_S)) {
            step_to_enabled(1);
        }
        if (IsKeyPressed(ImGuiKey_UpArrow) || IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
            IsKeyPressed(ImGuiKey_W)) {
            step_to_enabled(-1);
        }
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
        back = IsKeyPressed(ImGuiKey_Escape, false) || IsKeyPressed(ImGuiKey_Backspace, false) ||
               IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    }
    selected = std::clamp(selected, 0, count - 1);

    // Laid out like the game's own menus: title top left, entries below it with a triangle on
    // the selected one, values in a column to the right, prompts bottom right.
    const ImVec2 display = GetIO().DisplaySize;
    const float scale = Ui::Scale();
    auto* draw = GetForegroundDrawList();
    Ui::Veil(0.72f);
    const float title_x = display.x * 0.15f;
    const float label_x = display.x * 0.20f;
    const float value_x = display.x * 0.58f;
    float y = display.y * 0.17f;
    Ui::Text(draw, 60.0f * scale, {title_x, y}, Ui::TextBright, PageTitle());
    y += 100.0f * scale;
    const float row_height = 50.0f * scale;
    const float text_size = 34.0f * scale;
    // Long pages scroll to keep the selection in view.
    constexpr int MaxVisible = 11;
    static int first_visible = 0;
    if (count <= MaxVisible) {
        first_visible = 0;
    } else {
        first_visible = std::clamp(first_visible, std::max(0, selected - MaxVisible + 1),
                                   std::min(selected, count - MaxVisible));
    }
    const int last_visible = std::min(count, first_visible + MaxVisible);
    const ImVec2 mouse = GetIO().MousePos;
    const bool mouse_moved = GetIO().MouseDelta.x != 0.0f || GetIO().MouseDelta.y != 0.0f;
    for (int i = first_visible; i < last_visible; i++) {
        const Row& row = rows[i];
        const float row_y = y + row_height * (i - first_visible);
        const bool hovered = mouse.x >= title_x && mouse.x <= display.x * 0.80f &&
                             mouse.y >= row_y && mouse.y < row_y + row_height;
        if (hovered && row.enabled && !capturing) {
            if (mouse_moved) {
                selected = i;
            }
            if (IsMouseClicked(ImGuiMouseButton_Left)) {
                selected = i;
                activate_row = i;
                // A click left of a value steps it back.
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
                Ui::Triangle(draw, {value_x - 200.0f * scale, row_y + row_height * 0.5f}, arrow,
                             false, color);
                Ui::Triangle(draw, {value_x + 200.0f * scale, row_y + row_height * 0.5f}, arrow,
                             true, color);
            }
        }
    }
    if (count > MaxVisible) {
        // Where the list stands, like the game's own scroll marker.
        const float track_x = display.x * 0.82f;
        const float track_bottom = y + row_height * MaxVisible;
        draw->AddLine({track_x, y}, {track_x, track_bottom}, Ui::TextNormal, 1.5f);
        draw->AddCircleFilled(
            {track_x, y + (track_bottom - y) * first_visible / (count - MaxVisible)},
            5.0f * scale, Ui::TextBright);
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

    if (activate_row >= 0 && rows[activate_row].enabled && rows[activate_row].act) {
        rows[activate_row].act(activate_direction);
    } else if (back) {
        if (page == Page::Main || (page == Page::FreePlay && opened_on_page)) {
            open = false;
        } else {
            GoTo(Page::Main);
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
