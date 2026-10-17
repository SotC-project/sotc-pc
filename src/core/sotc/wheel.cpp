// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/libraries/pad/pad.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/sotc/menu.h"
#include "core/sotc/menu_ext.h"
#include "core/sotc/screens.h"
#include "core/sotc/ui.h"
#include "core/sotc/wheel.h"
#include "input/input_mouse.h"

namespace Sotc::Wheel {

namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;

// The game's item state, at fixed places of its image (offsets from its base):
// a set of owned items, a set of items put in the weapon cycle, and the weapon in hand.
constexpr u64 OwnedItems = 0x325F7A2;
constexpr u64 EquippedItems = 0x325F7AA;
constexpr u64 WeaponInHand = 0x2DCDD38;

struct Weapon {
    s32 kind; // the game's weapon-in-hand number
    u32 item; // the item that has to be owned and in the cycle for it
    const char* name;
};

// Clockwise from the top.
constexpr std::array<Weapon, 11> Weapons{{
    {1, 21, "Sword"},
    {3, 22, "Sword of the Sun"},
    {4, 23, "Queen's Sword"},
    {5, 24, "Sword of Dormin"},
    {2, 25, "Sword of Life"},
    {11, 16, "Torch"},
    {9, 20, "Harpoon of Thunder"},
    {8, 18, "Explosive Arrows"},
    {7, 19, "Whistling Arrows"},
    {6, 17, "Bow"},
    {10, 15, "Empty Hands"},
}};

std::atomic_bool open{false};
float motion_x = 0.0f, motion_y = 0.0f;
int chosen = -1;

// A change of weapon in progress: the game's weapon buttons are pressed one at a time until the
// wanted weapon is in hand, since the game cycles through the weapons it has in its cycle.
struct Change {
    s32 kind = 0;
    int presses = 0;
    int unchanged = 0;
    s32 last_seen = -1;
    u32 visited = 0; // weapons seen in hand since the last change of button
    u64 next_ms = 0;
    bool forward = true;
} change;

u8* Game(u64 offset, size_t size) {
    const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    if (!module || !module->IsValid()) {
        return nullptr;
    }
    const VAddr address = module->GetBaseAddress() + offset;
    if (!Core::Memory::Instance()->IsValidMapping(address, size)) {
        return nullptr;
    }
    return reinterpret_cast<u8*>(address);
}

bool HasBit(u64 set, u32 item) {
    const u8* bytes = Game(set + item / 8, 1);
    return bytes && (*bytes & (1u << (item % 8)));
}

void SetBit(u64 set, u32 item) {
    if (u8* bytes = Game(set + item / 8, 1)) {
        *bytes |= static_cast<u8>(1u << (item % 8));
    }
}

s32 InHand() {
    const u8* value = Game(WeaponInHand, 4);
    return value ? *reinterpret_cast<const s32*>(value) : -1;
}

bool InPlay() {
    return !Screens::PauseMenu() && !Screens::TitleMenu() && !Screens::GameOver() &&
           !Screens::Letterboxed() && !Menu::IsOpen() && !Ui::DialogOpen() &&
           InHand() > 0;
}

void Take(const Weapon& weapon) {
    // Bonus weapons the player has not won yet are given here, as the user asked; the game
    // keeps them with the save like any other item.
    SetBit(OwnedItems, weapon.item);
    SetBit(EquippedItems, weapon.item);
    // Swords are on the right button, bows on the left; the rest is tried on the right first.
    change = {.kind = weapon.kind,
              .next_ms = SDL_GetTicks() + 150,
              .forward = weapon.kind < 6 || weapon.kind > 9};
    LOG_INFO(Core, "Weapon wheel: {} (kind {}), in hand {}", weapon.name, weapon.kind, InHand());
}

void RunChange() {
    if (change.kind == 0 || SDL_GetTicks() < change.next_ms) {
        return;
    }
    const s32 now = InHand();
    if (now == change.kind || change.presses >= 24 || now <= 0) {
        if (now != change.kind) {
            LOG_WARNING(Core, "Weapon wheel: kind {} not reached, in hand {}", change.kind, now);
        }
        Input::ranged_out = now >= 6 && now <= 9;
        change.kind = 0;
        return;
    }
    // A button that brings nothing new (twice no change, or back round to a weapon already
    // seen): the other button.
    change.unchanged = now == change.last_seen ? change.unchanged + 1 : 0;
    const bool round = change.last_seen != now && (change.visited & (1u << (now & 31)));
    change.visited |= 1u << (now & 31);
    if (change.unchanged >= 2 || round) {
        change.forward = !change.forward;
        change.unchanged = 0;
        change.visited = 1u << (now & 31);
    }
    change.last_seen = now;
    MenuExt::PressButton(static_cast<u32>(change.forward ? OrbisPadButtonDataOffset::Right
                                                 : OrbisPadButtonDataOffset::Left));
    ++change.presses;
    change.next_ms = SDL_GetTicks() + 280;
}

void DrawWheel() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = Ui::Scale();
    const ImVec2 center{display.x * 0.5f, display.y * 0.5f};
    const float radius = 300.0f * scale;
    auto* draw = ImGui::GetForegroundDrawList();
    Ui::Veil(0.62f);
    draw->AddCircle(center, radius * 0.32f, IM_COL32(244, 242, 232, 90), 64, 1.5f * scale);

    const float length = std::hypot(motion_x, motion_y);
    if (length > 40.0f) {
        // Keep the pointer on the rim: a long swing does not have to be undone to go back.
        motion_x *= 120.0f / std::max(length, 120.0f);
        motion_y *= 120.0f / std::max(length, 120.0f);
        const float angle = std::atan2(motion_x, -motion_y); // 0 at the top, clockwise
        const float step = 2.0f * std::numbers::pi_v<float> / Weapons.size();
        const float turns = std::fmod(angle + step * 0.5f + 4.0f * std::numbers::pi_v<float>,
                                      2.0f * std::numbers::pi_v<float>);
        chosen = static_cast<int>(turns / step) % static_cast<int>(Weapons.size());
    }
    const s32 in_hand = InHand();
    for (size_t i = 0; i < Weapons.size(); i++) {
        const Weapon& weapon = Weapons[i];
        const float angle = 2.0f * std::numbers::pi_v<float> * i / Weapons.size();
        const ImVec2 at{center.x + std::sin(angle) * radius, center.y - std::cos(angle) * radius};
        const bool selected = static_cast<int>(i) == chosen;
        const bool owned = HasBit(OwnedItems, weapon.item);
        const ImU32 color = selected  ? Ui::TextBright
                            : owned   ? Ui::TextNormal
                                      : IM_COL32(150, 148, 140, 255);
        if (selected) {
            draw->AddCircleFilled(at, 70.0f * scale, IM_COL32(0, 0, 0, 150), 48);
            draw->AddCircle(at, 70.0f * scale, IM_COL32(244, 242, 232, 200), 48, 2.0f * scale);
        }
        Ui::TextCentered(draw, 30.0f * scale, at.x, at.y - 16.0f * scale, color, weapon.name);
        if (weapon.kind == in_hand) {
            Ui::Triangle(draw, {at.x, at.y + 26.0f * scale}, 9.0f * scale, true, color);
        }
    }
    if (chosen >= 0) {
        const Weapon& weapon = Weapons[chosen];
        Ui::TextCentered(draw, 40.0f * scale, center.x, center.y - 24.0f * scale, Ui::TextBright,
                         weapon.name);
        if (!HasBit(OwnedItems, weapon.item)) {
            Ui::TextCentered(draw, 26.0f * scale, center.x, center.y + 24.0f * scale,
                             Ui::TextNormal, "Bonus item: given when chosen");
        }
    }
}

} // namespace

bool IsOpen() {
    return open.load();
}

void Draw() {
    RunChange();
    const bool held = (ImGui::IsKeyDown(ImGuiKey_G) && !ImGui::GetIO().WantTextInput);
    if (!open) {
        if (!held || change.kind != 0 || !InPlay()) {
            return;
        }
        open = true;
        chosen = -1;
        motion_x = motion_y = 0.0f;
        Input::menu_motion_x = 0.0f;
        Input::menu_motion_y = 0.0f;
        // The game stands still while the wheel is up.
        DebugState.PauseGuestThreads();
    }
    motion_x += Input::menu_motion_x.exchange(0.0f);
    motion_y += Input::menu_motion_y.exchange(0.0f);
    if (held) {
        DrawWheel();
        return;
    }
    open = false;
    DebugState.ResumeGuestThreads();
    if (chosen >= 0) {
        Take(Weapons[chosen]);
    }
}

} // namespace Sotc::Wheel
