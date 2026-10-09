// SPDX-License-Identifier: GPL-2.0-or-later

// Achievements for the PC build. The game reports unlocks through sceNpTrophyUnlockTrophy; the
// names and descriptions in its trophy package are encrypted with a console key this build does
// not ship, so the list below carries the published English texts. Icons in the package are plain
// PNGs and are read from the game's own files.

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
#include "core/sotc/achievements.h"
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
    {'P', "Trophy 1", ""},
    {'G', "Trophy 2", ""},
    {'G', "Trophy 3", ""},
    {'G', "Trophy 4", ""},
    {'G', "Trophy 5", ""},
    {'G', "Trophy 6", ""},
    {'S', "Trophy 7", ""},
    {'S', "Trophy 8", ""},
    {'S', "Trophy 9", ""},
    {'S', "Trophy 10", ""},
    {'S', "Trophy 11", ""},
    {'S', "Trophy 12", ""},
    {'S', "Trophy 13", ""},
    {'B', "Trophy 14", ""},
    {'B', "Trophy 15", ""},
    {'B', "Trophy 16", ""},
    {'B', "Trophy 17", ""},
    {'B', "Trophy 18", ""},
    {'B', "Trophy 19", ""},
    {'B', "Trophy 20", ""},
    {'B', "Trophy 21", ""},
    {'B', "Trophy 22", ""},
    {'B', "Trophy 23", ""},
    {'B', "Trophy 24", ""},
    {'B', "Trophy 25", ""},
    {'B', "Trophy 26", ""},
    {'B', "Trophy 27", ""},
    {'B', "Trophy 28", ""},
    {'B', "Trophy 29", ""},
    {'B', "Trophy 30", ""},
    {'B', "Trophy 31", ""},
    {'B', "Trophy 32", ""},
    {'B', "Trophy 33", ""},
    {'B', "Trophy 34", ""},
    {'B', "Trophy 35", ""},
    {'B', "Trophy 36", ""},
    {'B', "Trophy 37", ""},
    {'B', "Trophy 38", ""},
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

void TogglePanel() {
    panel_open = !panel_open;
}

bool IsPanelOpen() {
    return panel_open;
}

void Draw() {
    using namespace ImGui;

    const bool pad_combo =
        (IsKeyDown(ImGuiKey_GamepadL3) && IsKeyPressed(ImGuiKey_GamepadR3, false)) ||
        (IsKeyDown(ImGuiKey_GamepadR3) && IsKeyPressed(ImGuiKey_GamepadL3, false));
    if (pad_combo) {
        TogglePanel();
    }
    if (!panel_open) {
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

    const ImVec2 display = GetIO().DisplaySize;
    const float scale = display.y / 1080.0f;
    const ImVec2 size{std::min(display.x * 0.9f, 980.0f * scale), display.y * 0.82f};
    SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    SetNextWindowSize(size, ImGuiCond_Always);
    // Weathered stone and moss, after the shrine walls.
    PushStyleColor(ImGuiCol_WindowBg, IM_COL32(30, 31, 28, 238));
    PushStyleColor(ImGuiCol_Border, IM_COL32(120, 124, 108, 255));
    PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(104, 138, 66, 255));
    PushStyleColor(ImGuiCol_FrameBg, IM_COL32(58, 60, 54, 255));
    PushStyleColor(ImGuiCol_Button, IM_COL32(70, 74, 64, 255));
    PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(104, 138, 66, 255));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding, {22.0f * scale, 18.0f * scale});
    PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    if (Begin("Achievements", nullptr,
              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
        SetWindowFontScale(2.0f * scale);
        TextColored({0.90f, 0.88f, 0.80f, 1.0f}, "Achievements");
        SetWindowFontScale(1.35f * scale);
        SameLine();
        const std::string count = fmt::format("{} / {}", unlocked, Entries.size());
        const char* close_label = "Close";
        const float close_width = CalcTextSize(close_label).x + GetStyle().FramePadding.x * 2.0f;
        SetCursorPosX(GetWindowWidth() - GetStyle().WindowPadding.x - close_width);
        if (Button(close_label)) {
            panel_open = false;
        }
        ProgressBar(static_cast<float>(unlocked) / Entries.size(), {-1.0f, GetFrameHeight()},
                    count.c_str());
        Spacing();

        const float icon = 84.0f * scale;
        const float row = icon + 14.0f * scale;
        const float footer = GetTextLineHeightWithSpacing() + 6.0f * scale;
        if (BeginChild("list", {0.0f, -footer}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
            const float step = row * 14.0f * GetIO().DeltaTime;
            if (IsKeyDown(ImGuiKey_DownArrow) || IsKeyDown(ImGuiKey_GamepadDpadDown)) {
                SetScrollY(GetScrollY() + step);
            }
            if (IsKeyDown(ImGuiKey_UpArrow) || IsKeyDown(ImGuiKey_GamepadDpadUp)) {
                SetScrollY(GetScrollY() - step);
            }
            auto* draw = GetWindowDrawList();
            for (size_t i = 0; i < Entries.size(); i++) {
                const Entry& entry = Entries[i];
                const bool done = unlock_time[i] != 0;
                const ImVec2 origin = GetCursorScreenPos();
                const float width = GetContentRegionAvail().x;
                draw->AddRectFilled(origin, {origin.x + width, origin.y + row - 6.0f * scale},
                                    done ? IM_COL32(62, 72, 50, 200) : IM_COL32(46, 47, 43, 200),
                                    3.0f);
                const ImVec2 icon_min{origin.x + 6.0f * scale, origin.y + 4.0f * scale};
                const ImVec2 icon_max{icon_min.x + icon, icon_min.y + icon};
                if (icons[i]) {
                    draw->AddImage(icons[i].GetTexture().im_id, icon_min, icon_max, {0, 0}, {1, 1},
                                   done ? IM_COL32_WHITE : IM_COL32(90, 90, 90, 255));
                } else {
                    draw->AddRectFilled(icon_min, icon_max, IM_COL32(70, 72, 66, 255));
                }
                const float text_x = icon_max.x + 14.0f * scale;
                const float line = GetTextLineHeight();
                draw->AddText({text_x, origin.y + 8.0f * scale},
                              done ? IM_COL32(236, 232, 214, 255) : IM_COL32(186, 186, 176, 255),
                              entry.name);
                draw->AddText({text_x, origin.y + 8.0f * scale + line * 1.25f},
                              done ? IM_COL32(196, 198, 184, 255) : IM_COL32(150, 150, 142, 255),
                              entry.description);
                std::string status = GradeName(entry.grade);
                if (done) {
                    const std::time_t when = static_cast<std::time_t>(unlock_time[i]);
                    char date[32]{};
                    if (const std::tm* local = std::localtime(&when)) {
                        std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", local);
                    }
                    status += fmt::format("  -  unlocked {}", date);
                } else {
                    status += "  -  locked";
                }
                draw->AddText({text_x, origin.y + 8.0f * scale + line * 2.5f},
                              done ? GradeColor(entry.grade) : IM_COL32(132, 132, 124, 255),
                              status.c_str());
                Dummy({width, row});
            }
        }
        EndChild();
        TextColored({0.62f, 0.64f, 0.58f, 1.0f},
                    "F2 or L3 + R3: close     Up / Down, mouse wheel: scroll");
    }
    End();
    PopStyleVar(3);
    PopStyleColor(7);
}

} // namespace Sotc::Achievements
