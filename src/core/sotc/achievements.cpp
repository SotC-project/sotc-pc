// SPDX-License-Identifier: GPL-2.0-or-later

// Achievements for the PC build. The game reports unlocks through sceNpTrophyUnlockTrophy; the
// names and descriptions in its trophy package are encrypted with a console key this build does
// not ship, so the list below carries the published English texts. Icons in the package are plain
// PNGs and are read from the game's own files.

#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <imgui.h>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/libraries/np/trophy_ui.h"
#include "core/sotc/lang.h"
#include "core/sotc/achievements.h"
#include "core/sotc/screens.h"
#include "core/sotc/ui.h"
#include "imgui/imgui_texture.h"

namespace Sotc::Achievements {

namespace {

struct Entry {
    char grade; // P, G, S, B
    const char* name;
    const char* description;
};

// Indexed by trophy id.
constexpr std::array<Entry, 38> Entries{{
    {'P', "The Horned Boy", "Acquire all trophies"},
    {'G', "Speed King", "Obtain all Time Attack items together as one set"},
    {'G', "Intrepid Mortal", "Max out Wander's health and stamina bars"},
    {'G', "Speed Demon", "Complete Hard Mode under 5:41:28"},
    {'G', "Grounded Scaler", "Defeat Colossus 8 before it can turn back over"},
    {'G', "Resist the Wrist", "Defeat Colossus 3 without breaking his wrist guard"},
    {'S', "Reach the Gate", "Cross the bridge to the entrance of the Forbidden Lands"},
    {'S', "Last Man Standing", "Complete a single playthrough without dying once"},
    {'S', "Bearer of the Curse", "Complete the game on any difficulty"},
    {'S', "Dormin's Rage", "Use Dormin's breath attack"},
    {'S', "Seeking Salvation", "Pray at all shrines"},
    {'S', "Trick Rider", "Perform all stunts with Agro"},
    {'S', "Fruit of the Garden", "Taste the poisoned fruit"},
    {'B', "Sword of Her Majesty", "Defeat any colossus with the Queen's Sword"},
    {'B', "Paint the Target", "Use the sword to focus on a vital point"},
    {'B', "Fruit of the Land", "Eat a piece of fruit"},
    {'B', "Skilled Warrior", "Defeat a colossus with a downward jump stab"},
    {'B', "The Past that Defines Thee", "Defeat any colossus while in Reminiscence Mode"},
    {'B', "Five-Lined Skink", "Collect a silver lizard tail"},
    {'B', "Animals of the Land", "Interact with a dove, hawk, fish and turtle"},
    {'B', "Boon of the Nomad", "Find the barrel in the hidden cave"},
    {'B', "Trick Arrow Skills", "Shoot a lizard with a special arrow"},
    {'B', "Valley of the Wanderer", "Defeat the 1st colossus"},
    {'B', "The Mammoth", "Defeat the 2nd colossus"},
    {'B', "Wake the Knight", "Defeat the 3rd colossus"},
    {'B', "Land of the Gravestones", "Defeat the 4th colossus"},
    {'B', "Riding the Wind", "Defeat the 5th colossus"},
    {'B', "Tomb of the Giant", "Defeat the 6th colossus"},
    {'B', "Waves of Lightning", "Defeat the 7th colossus"},
    {'B', "Scaler of the Colosseum", "Defeat the 8th colossus"},
    {'B', "Lurker of the Cave", "Defeat the 9th colossus"},
    {'B', "Mystery in the Sand", "Defeat the 10th colossus"},
    {'B', "Guardian and the Pit", "Defeat the 11th colossus"},
    {'B', "Thunder of the Lake", "Defeat the 12th colossus"},
    {'B', "Signs amidst the Storm", "Defeat the 13th colossus"},
    {'B', "Shield of the Colossus", "Defeat the 14th colossus"},
    {'B', "Valley of the Fallen", "Defeat the 15th colossus"},
    {'B', "Last of the Colossus", "Defeat the 16th colossus"},
}};

std::mutex mutex;
bool loaded = false;
std::array<u64, Entries.size()> unlock_time{}; // seconds since epoch, 0 = locked
std::array<ImGui::RefCountedTexture, Entries.size()> icons;
bool icons_loaded = false;
std::atomic_bool panel_open = false;

std::filesystem::path BaseDir() {
    return Common::FS::GetUserPath(Common::FS::PathType::TrophyDir) / "sotc";
}

std::filesystem::path IconPath(s32 id) {
    return BaseDir() / "icons" / fmt::format("TROP{:03d}.PNG", id);
}

std::string_view GradeCode(char grade) {
    switch (grade) {
    case 'P':
        return "P";
    case 'G':
        return "G";
    case 'S':
        return "S";
    default:
        return "B";
    }
}

u32 ReadBe32(const u8* p) {
    return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]);
}

u64 ReadBe64(const u8* p) {
    return u64(ReadBe32(p)) << 32 | ReadBe32(p + 4);
}

/// Copies the unencrypted icon PNGs out of the game's trophy package.
void ExtractIcons() {
    std::error_code ec;
    const auto icon_dir = BaseDir() / "icons";
    if (std::filesystem::exists(IconPath(0), ec)) {
        return;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto trp_path = mnt->GetHostPath("/app0/sce_sys/trophy/trophy00.trp");
    std::ifstream file(trp_path, std::ios::binary);
    if (!file) {
        LOG_WARNING(Lib_NpTrophy, "Achievement icons: cannot open {}", trp_path.string());
        return;
    }
    std::array<u8, 0x60> header;
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!file || ReadBe32(header.data()) != 0xDCA24D00) {
        return;
    }
    const u32 count = ReadBe32(header.data() + 16);
    const u32 entry_size = ReadBe32(header.data() + 20);
    if (entry_size < 48 || entry_size > 4096 || count > 4096) {
        return;
    }
    std::filesystem::create_directories(icon_dir, ec);
    std::vector<u8> entry(entry_size);
    for (u32 i = 0; i < count; i++) {
        file.seekg(0x60 + u64(i) * entry_size);
        file.read(reinterpret_cast<char*>(entry.data()), entry_size);
        if (!file) {
            return;
        }
        const char* raw_name = reinterpret_cast<const char*>(entry.data());
        const std::string name(raw_name, strnlen(raw_name, 32));
        if (!name.starts_with("TROP") || !name.ends_with(".PNG")) {
            continue;
        }
        const u64 pos = ReadBe64(entry.data() + 32);
        const u64 len = ReadBe64(entry.data() + 40);
        if (len > 16 * 1024 * 1024) {
            continue;
        }
        std::vector<char> data(len);
        file.seekg(pos);
        file.read(data.data(), len);
        if (!file) {
            file.clear();
            continue;
        }
        std::ofstream out(icon_dir / name, std::ios::binary);
        out.write(data.data(), data.size());
    }
}

void Save() {
    std::error_code ec;
    std::filesystem::create_directories(BaseDir(), ec);
    const auto path = BaseDir() / "unlocked.txt";
    const auto temp = BaseDir() / "unlocked.tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << "# Shadow of the Colossus achievements: <id> <unix time unlocked>\n";
        for (size_t i = 0; i < Entries.size(); i++) {
            if (unlock_time[i] != 0) {
                out << i << ' ' << unlock_time[i] << '\n';
            }
        }
    }
    std::filesystem::rename(temp, path, ec);
}

void EnsureLoaded() {
    if (loaded) {
        return;
    }
    loaded = true;
    ExtractIcons();
    std::ifstream in(BaseDir() / "unlocked.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        unsigned id = 0;
        unsigned long long when = 0;
        if (std::sscanf(line.c_str(), "%u %llu", &id, &when) == 2 && id < Entries.size()) {
            unlock_time[id] = when;
        }
    }
}

void Notify(s32 id) {
    Libraries::Np::NpTrophy::AddTrophyToQueue(IconPath(id), Entries[id].name,
                                              GradeCode(Entries[id].grade));
}

ImU32 GradeColor(char grade) {
    switch (grade) {
    case 'P':
        return IM_COL32(190, 215, 235, 255);
    case 'G':
        return IM_COL32(226, 190, 92, 255);
    case 'S':
        return IM_COL32(196, 200, 204, 255);
    default:
        return IM_COL32(196, 132, 84, 255);
    }
}

const char* GradeName(char grade) {
    switch (grade) {
    case 'P':
        return "Platinum";
    case 'G':
        return "Gold";
    case 'S':
        return "Silver";
    default:
        return "Bronze";
    }
}

} // namespace

UnlockResult Unlock(s32 id, s32* platinum_id) {
    if (platinum_id) {
        *platinum_id = -1;
    }
    if (id < 0 || id >= static_cast<s32>(Entries.size())) {
        return UnlockResult::UnknownId;
    }
    if (Entries[id].grade == 'P') {
        return UnlockResult::IsPlatinum;
    }
    // Free play (a chosen battle, with helpers on offer) earns nothing.
    if (const char* free_play = std::getenv("SOTC_FREEPLAY"); free_play && *free_play) {
        return UnlockResult::AlreadyUnlocked;
    }
    bool platinum = false;
    {
        std::lock_guard lock{mutex};
        EnsureLoaded();
        if (unlock_time[id] != 0) {
            return UnlockResult::AlreadyUnlocked;
        }
        const u64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        unlock_time[id] = now;
        platinum = unlock_time[0] == 0;
        for (size_t i = 1; i < Entries.size(); i++) {
            platinum = platinum && unlock_time[i] != 0;
        }
        if (platinum) {
            unlock_time[0] = now;
        }
        Save();
    }
    LOG_INFO(Lib_NpTrophy, "Achievement unlocked: {} ({})", Entries[id].name, id);
    Notify(id);
    if (platinum) {
        if (platinum_id) {
            *platinum_id = 0;
        }
        // The popup shows one achievement at a time; let the first one finish.
        std::thread{[] {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<s32>(EmulatorSettings.GetTrophyNotificationDuration() * 1000) + 250));
            Notify(0);
        }}.detach();
    }
    return UnlockResult::Unlocked;
}

float NotificationVolume() {
    // Read each time: the popup is rare and the file may change between sessions.
    std::ifstream in(Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                     "sotc_achievement_volume.txt");
    int percent = 100;
    if (in >> percent) {
        return std::clamp(percent, 0, 100) / 100.0f;
    }
    return 1.0f;
}

void TogglePanel() {
    panel_open = !panel_open;
}

bool IsPanelOpen() {
    return panel_open;
}

void Draw() {
    using namespace ImGui;

    if (!panel_open) {
        return;
    }
    if (IsKeyPressed(ImGuiKey_Escape, false) || IsKeyPressed(ImGuiKey_F2, false) ||
        IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        panel_open = false;
        return;
    }

    std::lock_guard lock{mutex};
    EnsureLoaded();
    if (!icons_loaded) {
        icons_loaded = true;
        for (size_t i = 0; i < Entries.size(); i++) {
            std::error_code ec;
            if (std::filesystem::exists(IconPath(static_cast<s32>(i)), ec)) {
                icons[i] = ImGui::RefCountedTexture::DecodePngFile(IconPath(static_cast<s32>(i)));
            }
        }
    }

    size_t unlocked = 0;
    for (const u64 when : unlock_time) {
        unlocked += when != 0;
    }

    // Laid out like the game's own menus (see core/sotc/ui.h).
    const ImVec2 display = GetIO().DisplaySize;
    const float scale = Ui::Scale();
    auto* draw = GetForegroundDrawList();
    // Opened from the title or pause menu, the list takes the menu's place the way the game's
    // own screens do (its Settings, for one): the picture washed out pale, the text in ink.
    // Over a free play battle it is a pale list on a dark veil.
    const bool in_place = ReplacesGameMenu();
    struct InkScope {
        explicit InkScope(bool ink) {
            Ui::SetInk(ink);
        }
        ~InkScope() {
            Ui::SetInk(false);
        }
    } ink_scope{in_place};
    if (!in_place) {
        Ui::Veil(0.80f);
    } else if (!Screens::PauseMenu()) {
        // The pause menu has washed its picture out already; the title has not.
        draw->AddRectFilled({0.0f, 0.0f}, display, IM_COL32(212, 211, 204, 226));
    }
    const float left = display.x * 0.15f;
    const float right = display.x * 0.85f;
    float top = display.y * 0.10f;
    Ui::Text(draw, 60.0f * scale, {left, top}, Ui::TextBright, "Achievements");
    Ui::TextRight(draw, 40.0f * scale, right, top + 14.0f * scale, Ui::TextNormal,
                  fmt::format("{} / {}", unlocked, Entries.size()));
    top += 92.0f * scale;
    // Progress: a thin line, pale where earned.
    draw->AddRectFilled({left, top}, {right, top + 3.0f * scale}, IM_COL32(90, 90, 84, 255));
    draw->AddRectFilled({left, top},
                        {left + (right - left) * unlocked / Entries.size(), top + 3.0f * scale},
                        Ui::TextBright);
    top += 26.0f * scale;

    const float bottom = display.y * 0.84f;
    const float row = 108.0f * scale;
    const float icon = 88.0f * scale;
    static float scroll = 0.0f;
    const float max_scroll = std::max(0.0f, row * Entries.size() - (bottom - top));
    const float step = row * 9.0f * GetIO().DeltaTime;
    if (IsKeyDown(ImGuiKey_DownArrow) || IsKeyDown(ImGuiKey_S) ||
        IsKeyDown(ImGuiKey_GamepadDpadDown)) {
        scroll += step;
    }
    if (IsKeyDown(ImGuiKey_UpArrow) || IsKeyDown(ImGuiKey_W) || IsKeyDown(ImGuiKey_GamepadDpadUp)) {
        scroll -= step;
    }
    scroll -= GetIO().MouseWheel * row;
    scroll = std::clamp(scroll, 0.0f, max_scroll);

    draw->PushClipRect({0.0f, top}, {display.x, bottom}, true);
    for (size_t i = 0; i < Entries.size(); i++) {
        const float y = top + row * i - scroll;
        if (y + row < top || y > bottom) {
            continue;
        }
        const Entry& entry = Entries[i];
        const bool done = unlock_time[i] != 0;
        const ImVec2 icon_min{left, y + (row - icon) * 0.5f};
        const ImVec2 icon_max{icon_min.x + icon, icon_min.y + icon};
        if (icons[i]) {
            draw->AddImage(icons[i].GetTexture().im_id, icon_min, icon_max, {0, 0}, {1, 1},
                           done ? IM_COL32_WHITE : IM_COL32(86, 86, 86, 255));
        }
        const float text_x = icon_max.x + 26.0f * scale;
        Ui::Text(draw, 36.0f * scale, {text_x, y + 14.0f * scale},
                 done ? Ui::TextBright : Ui::TextNormal, entry.name);
        Ui::Text(draw, 26.0f * scale, {text_x, y + 58.0f * scale},
                 done ? Ui::TextNormal : Ui::TextDim, entry.description);
        std::string status = Lang::Tr(GradeName(entry.grade));
        if (done) {
            const std::time_t when = static_cast<std::time_t>(unlock_time[i]);
            char date[32]{};
            if (const std::tm* local = std::localtime(&when)) {
                // English writes the month out; the other languages get the date in figures,
                // which reads the same in all of them.
                std::strftime(date, sizeof(date), Lang::Code() == "en" ? "%d %B %Y" : "%d.%m.%Y",
                              local);
            }
            status += fmt::format("   -   {}", date);
        }
        Ui::TextRight(draw, 26.0f * scale, right, y + 22.0f * scale,
                      !done ? Ui::TextDim : in_place ? Ui::TextBright : GradeColor(entry.grade),
                      status);
        draw->AddLine({left, y + row - 1.0f}, {right, y + row - 1.0f}, IM_COL32(70, 70, 66, 160));
    }
    draw->PopClipRect();
    // Where the list stands, like the game's own scroll marker.
    if (max_scroll > 0.0f) {
        const float track_x = right + 34.0f * scale;
        draw->AddLine({track_x, top}, {track_x, bottom}, Ui::TextNormal, 1.5f);
        draw->AddCircleFilled({track_x, top + (bottom - top) * scroll / max_scroll}, 5.0f * scale,
                              Ui::TextBright);
    }
    const float prompt_x = display.x * 0.95f;
    float prompt_y = display.y * 0.865f;
    prompt_y += Ui::Prompt(draw, prompt_x, prompt_y, "SCROLL", Ui::Action::Choose);
    Ui::Prompt(draw, prompt_x, prompt_y, "BACK", Ui::Action::Back);
}

bool ReplacesGameMenu() {
    const char* free_play = std::getenv("SOTC_FREEPLAY");
    return IsPanelOpen() && !(free_play && *free_play);
}

} // namespace Sotc::Achievements
