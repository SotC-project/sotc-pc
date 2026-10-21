// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "common/singleton.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/sotc/challenge.h"
#include "core/sotc/cvc_hud.h"
#include "core/sotc/menu.h"
#include "core/sotc/screens.h"
#include "core/sotc/ui.h"

namespace Sotc::CvcHud {

namespace {

// The cave (image offsets): the boss table entries of A and B, their health managers
// (health at +0x50, full health at +0x4c), the fight set up, the camera on it, the winner.
constexpr u64 EntryA = 0x1593b28, EntryB = 0x1593b30;
constexpr u64 ManagerA = 0x1593e00, ManagerB = 0x1593e08;
constexpr u64 SetUp = 0x1593ef0, CameraSettled = 0x1593ae8, Winner = 0x1593efb;
constexpr u64 BossTable = 0x16d21f8, BossEntrySize = 0x1e8;

constexpr ImU32 BarFill = IM_COL32(236, 233, 222, 235);
constexpr ImU32 BarLost = IM_COL32(232, 132, 104, 220);
constexpr ImU32 BarBack = IM_COL32(6, 6, 6, 120);
constexpr ImU32 BarEdge = IM_COL32(236, 233, 222, 110);

struct Side {
    int boss = -1;      // boss table index, 0-15
    float health = 1.0f; // share of full health
};

u64 ImageBase() {
    const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    return module && module->IsValid() ? module->GetBaseAddress() : 0;
}

template <typename T>
bool Read(u64 address, T& value) {
    if (!address || !Core::Memory::Instance()->IsValidMapping(address, sizeof(T))) {
        return false;
    }
    value = *reinterpret_cast<const volatile T*>(address);
    return true;
}

Side ReadSide(u64 image, u64 entry_slot, u64 manager_slot) {
    Side side;
    u64 entry = 0, table = 0, manager = 0;
    if (Read(image + entry_slot, entry) && Read(image + BossTable + 0x20, table) && entry >= table &&
        (entry - table) % BossEntrySize == 0 && (entry - table) / BossEntrySize < 16) {
        side.boss = int((entry - table) / BossEntrySize);
    }
    float health = 0.0f, full = 0.0f;
    if (Read(image + manager_slot, manager) && Read(manager + 0x50, health) && Read(manager + 0x4c, full) &&
        full > 0.0f) {
        side.health = std::clamp(health / full, 0.0f, 1.0f);
    }
    return side;
}

} // namespace

void Draw() {
    const u64 image = ImageBase();
    // Frames in a row without the loading cover: the first uncovered frame can still be the
    // cover's last, so the readout waits for three.
    static int uncovered = 0;
    uncovered = Menu::FrameCovered() ? 0 : std::min(uncovered + 1, 3);
    u8 set_up = 0, settled = 0, winner = 0;
    if (!image || !Read(image + SetUp, set_up) || !set_up || !Read(image + CameraSettled, settled) || !settled ||
        uncovered < 3 || Screens::PauseMenu()) {
        return;
    }
    Read(image + Winner, winner);
    const Side sides[2]{ReadSide(image, EntryA, ManagerA), ReadSide(image, EntryB, ManagerB)};
    if (sides[0].boss < 0 || sides[1].boss < 0) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = Ui::Scale();
    const float center = display.x * 0.5f;
    auto* draw = ImGui::GetForegroundDrawList();
    const u64 now = SDL_GetTicks();

    // What was lost to the last blow stays shown in a warm colour for a moment, then drains.
    static float shown[2]{1.0f, 1.0f};
    static u64 last_ms = now;
    const float dt = std::clamp(float(now - last_ms) / 1000.0f, 0.0f, 0.1f);
    last_ms = now;

    // Names and bars along the top, A on the left and B on the right, each bar full toward its
    // own edge of the screen and emptying toward the middle. Not over a cutscene.
    if (!Screens::Letterboxed()) {
        const float width = 560.0f * scale, height = 9.0f * scale, gap = 70.0f * scale;
        const float top = 66.0f * scale, name_size = 18.0f * scale;
        for (int k = 0; k < 2; k++) {
            const float left = k == 0 ? center - gap * 0.5f - width : center + gap * 0.5f;
            const float right = left + width;
            const float health = sides[k].health;
            if (shown[k] > health) {
                shown[k] = std::max(health, shown[k] - dt * 0.35f);
            } else {
                shown[k] = health;
            }
            const auto span = [&](float from, float to) {
                // A share of the bar, counted from the colossus's own edge.
                return k == 0 ? ImVec2{left + width * from, left + width * to}
                              : ImVec2{right - width * to, right - width * from};
            };
            draw->AddRectFilled({left - 2.0f * scale, top - 2.0f * scale},
                                {right + 2.0f * scale, top + height + 2.0f * scale}, BarBack, 2.0f * scale);
            const ImVec2 lost = span(health, shown[k]);
            draw->AddRectFilled({lost.x, top}, {lost.y, top + height}, BarLost);
            const ImVec2 kept = span(0.0f, health);
            draw->AddRectFilled({kept.x, top}, {kept.y, top + height}, BarFill);
            draw->AddRect({left - 2.0f * scale, top - 2.0f * scale},
                          {right + 2.0f * scale, top + height + 2.0f * scale}, BarEdge, 2.0f * scale, 0,
                          1.0f * scale);
            Ui::Caption(draw, name_size, k == 0 ? left : right, top - name_size - 10.0f * scale,
                        k == 0 ? 0.0f : 1.0f, Ui::TextBright, Challenge::ColossusName(sides[k].boss + 1));
        }
    }

    // The knockout: a banner across the middle naming the winner, fading in.
    if (winner == 1 || winner == 2) {
        static u64 since = 0;
        static u8 seen = 0;
        if (seen != winner) {
            seen = winner, since = now;
        }
        const float opacity = std::clamp(float(now - since) / 600.0f, 0.0f, 1.0f);
        const int first = draw->VtxBuffer.Size;
        const float y = display.y * 0.34f;
        const float band = 150.0f * scale;
        draw->AddRectFilledMultiColor({0.0f, y - band * 0.5f}, {display.x, y}, IM_COL32(0, 0, 0, 0),
                                      IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
        draw->AddRectFilledMultiColor({0.0f, y}, {display.x, y + band * 0.5f}, IM_COL32(0, 0, 0, 150),
                                      IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
        Ui::Caption(draw, 16.0f * scale, center, y - 52.0f * scale, 0.5f, Ui::TextNormal, "Knockout");
        const float big = 54.0f * scale;
        Ui::TextCentered(draw, big, center, y - big * 0.5f, Ui::TextBright,
                         Challenge::ColossusName(sides[winner - 1].boss + 1));
        Ui::Caption(draw, 16.0f * scale, center, y + 34.0f * scale, 0.5f, Ui::TextNormal, "Winner");
        Ui::Fade(draw, first, opacity);
    }
}

} // namespace Sotc::CvcHud
