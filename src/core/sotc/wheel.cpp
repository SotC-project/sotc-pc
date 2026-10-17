// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <numbers>
#include <string>
#include <vector>

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/libraries/kernel/time.h"
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
using Clock = std::chrono::steady_clock;
Clock::time_point opened_at;

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

ImU32 Fade(ImU32 color, float alpha) {
    const u32 a = static_cast<u32>(((color >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// Draws `text` centred in a box `width` wide from `top` down, on up to two lines, made smaller
// until it fits; never wider than the box.
void FitText(ImDrawList* draw, float size, float min_size, float center_x, float top, float width,
             ImU32 color, const std::string& text) {
    for (float sz = size;; sz -= 1.0f) {
        std::vector<std::string> lines{""};
        bool fits = true;
        for (size_t from = 0; from < text.size();) {
            const size_t to = text.find(' ', from);
            const std::string word = text.substr(from, to == std::string::npos ? to : to - from);
            from = to == std::string::npos ? text.size() : to + 1;
            const std::string joined = lines.back().empty() ? word : lines.back() + " " + word;
            if (Ui::TextSize(sz, joined).x <= width) {
                lines.back() = joined;
            } else if (!lines.back().empty() && lines.size() < 2 &&
                       Ui::TextSize(sz, word).x <= width) {
                lines.push_back(word);
            } else {
                fits = false;
                break;
            }
        }
        if (fits || sz <= min_size) {
            for (size_t i = 0; i < lines.size(); i++) {
                Ui::TextCentered(draw, sz, center_x, top + i * sz * 1.05f, color, lines[i]);
            }
            return;
        }
    }
}

// A small line drawing of each weapon in a box `size` wide around `c`.
void DrawIcon(ImDrawList* draw, s32 kind, ImVec2 c, float size, ImU32 color) {
    const float u = size * 0.5f;
    const float w = std::max(1.5f, size * 0.055f);
    // Blades and shafts lie on the diagonal, point up right; (x, y) in -1..1, y down.
    const auto P = [&](float x, float y) {
        constexpr float k = 0.70710678f;
        return ImVec2{c.x + (x * k - y * k) * u, c.y + (x * k + y * k) * u};
    };
    const auto Q = [&](float x, float y) { return ImVec2{c.x + x * u, c.y + y * u}; }; // upright
    const auto sword = [&] {
        draw->AddQuadFilled(P(-0.09f, 0.4f), P(-0.09f, -0.75f), P(0.09f, -0.75f), P(0.09f, 0.4f),
                            color);
        draw->AddTriangleFilled(P(-0.09f, -0.75f), P(0.0f, -1.0f), P(0.09f, -0.75f), color);
        draw->AddLine(P(-0.38f, 0.42f), P(0.38f, 0.42f), color, w * 1.4f);
        draw->AddLine(P(0.0f, 0.42f), P(0.0f, 0.8f), color, w * 1.2f);
        draw->AddCircleFilled(P(0.0f, 0.88f), u * 0.09f, color, 12);
    };
    const auto arrow = [&] {
        draw->AddLine(P(0.0f, 0.95f), P(0.0f, -0.65f), color, w);
        draw->AddTriangleFilled(P(-0.16f, -0.62f), P(0.0f, -1.0f), P(0.16f, -0.62f), color);
        draw->AddLine(P(0.0f, 0.7f), P(-0.2f, 0.95f), color, w);
        draw->AddLine(P(0.0f, 0.7f), P(0.2f, 0.95f), color, w);
    };
    const auto rays = [&](ImVec2 at, float inner, float outer, float turn, bool alternate) {
        for (int r = 0; r < 8; r++) {
            const float a = r * 0.785398f + turn;
            const float len = alternate && r % 2 ? inner + (outer - inner) * 0.5f : outer;
            draw->AddLine({at.x + std::cos(a) * inner * u, at.y + std::sin(a) * inner * u},
                          {at.x + std::cos(a) * len * u, at.y + std::sin(a) * len * u}, color,
                          w * 0.8f);
        }
    };
    switch (kind) {
    case 1: // Sword
        sword();
        break;
    case 3: // Sword of the Sun: a sun beside it
        sword();
        draw->AddCircle(Q(-0.62f, -0.62f), u * 0.17f, color, 16, w);
        rays(Q(-0.62f, -0.62f), 0.25f, 0.36f, 0.0f, false);
        break;
    case 4: { // Queen's Sword: a crown
        sword();
        const std::array crown{Q(-0.85f, -0.45f), Q(-0.85f, -0.8f), Q(-0.68f, -0.6f),
                               Q(-0.55f, -0.85f), Q(-0.42f, -0.6f), Q(-0.25f, -0.8f),
                               Q(-0.25f, -0.45f)};
        draw->AddPolyline(crown.data(), static_cast<int>(crown.size()), color, ImDrawFlags_Closed, w);
        break;
    }
    case 5: // Sword of Dormin: an eye
        sword();
        draw->AddBezierQuadratic(Q(-0.9f, -0.6f), Q(-0.6f, -0.95f), Q(-0.3f, -0.6f), color, w, 12);
        draw->AddBezierQuadratic(Q(-0.9f, -0.6f), Q(-0.6f, -0.25f), Q(-0.3f, -0.6f), color, w, 12);
        draw->AddCircleFilled(Q(-0.6f, -0.6f), u * 0.08f, color, 10);
        break;
    case 2: // Sword of Life: a leaf
        sword();
        draw->AddBezierQuadratic(Q(-0.85f, -0.35f), Q(-0.85f, -0.9f), Q(-0.3f, -0.9f), color, w, 12);
        draw->AddBezierQuadratic(Q(-0.85f, -0.35f), Q(-0.3f, -0.35f), Q(-0.3f, -0.9f), color, w, 12);
        break;
    case 6: // Bow
        draw->AddBezierQuadratic(Q(0.35f, -0.95f), Q(-0.85f, 0.0f), Q(0.35f, 0.95f), color, w * 1.6f,
                                 20);
        draw->AddLine(Q(0.35f, -0.95f), Q(0.35f, 0.95f), color, w * 0.6f);
        draw->AddLine(Q(-0.45f, 0.0f), Q(0.9f, 0.0f), color, w);
        draw->AddTriangleFilled(Q(0.75f, -0.12f), Q(1.0f, 0.0f), Q(0.75f, 0.12f), color);
        break;
    case 7: // Whistling Arrows: sound rings at the head
        arrow();
        draw->AddBezierQuadratic(Q(0.55f, -0.95f), Q(0.95f, -0.95f), Q(0.95f, -0.55f), color,
                                 w * 0.8f, 10);
        draw->AddBezierQuadratic(Q(0.35f, -1.0f), Q(1.0f, -1.15f), Q(1.0f, -0.35f), color, w * 0.8f,
                                 10);
        break;
    case 8: // Explosive Arrows: a burst at the head
        arrow();
        rays(Q(0.72f, -0.72f), 0.08f, 0.34f, 0.39f, true);
        break;
    case 9: { // Harpoon of Thunder: barbed head and a bolt
        draw->AddLine(P(0.0f, 1.0f), P(0.0f, -0.55f), color, w * 1.2f);
        draw->AddTriangleFilled(P(-0.2f, -0.55f), P(0.0f, -1.0f), P(0.2f, -0.55f), color);
        draw->AddLine(P(-0.2f, -0.55f), P(-0.3f, -0.35f), color, w);
        draw->AddLine(P(0.2f, -0.55f), P(0.3f, -0.35f), color, w);
        const std::array bolt{Q(-0.55f, -0.95f), Q(-0.8f, -0.55f), Q(-0.55f, -0.55f),
                              Q(-0.8f, -0.15f)};
        draw->AddPolyline(bolt.data(), static_cast<int>(bolt.size()), color, ImDrawFlags_None, w);
        break;
    }
    case 11: // Torch: a stick and its flame
        draw->AddLine(Q(0.0f, 0.95f), Q(0.0f, -0.15f), color, w * 2.0f);
        draw->AddBezierQuadratic(Q(-0.3f, -0.2f), Q(-0.45f, -0.75f), Q(0.0f, -1.0f), color, w, 12);
        draw->AddBezierQuadratic(Q(0.3f, -0.2f), Q(0.45f, -0.75f), Q(0.0f, -1.0f), color, w, 12);
        draw->AddLine(Q(-0.3f, -0.2f), Q(0.3f, -0.2f), color, w);
        break;
    default: // Empty hands: an open hand
        draw->AddRect(Q(-0.42f, -0.05f), Q(0.38f, 0.75f), color, u * 0.18f, ImDrawFlags_None, w);
        for (int f = 0; f < 4; f++) {
            const float x = -0.3f + f * 0.21f;
            draw->AddLine(Q(x, -0.05f), Q(x, f == 0 || f == 3 ? -0.6f : -0.85f), color, w * 1.3f);
        }
        draw->AddLine(Q(-0.42f, 0.35f), Q(-0.75f, 0.0f), color, w * 1.3f);
        break;
    }
}

void DrawWheel() {
    constexpr float Pi = std::numbers::pi_v<float>;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = Ui::Scale();
    const ImVec2 center{display.x * 0.5f, display.y * 0.5f};
    const float now = std::chrono::duration<float>(Clock::now() - opened_at).count();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);

    // Opening: the wheel fades in and grows to its size over a sixth of a second.
    const float open_t = std::clamp(now / 0.16f, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - open_t) * (1.0f - open_t) * (1.0f - open_t);
    const float radius = 300.0f * scale * (0.86f + 0.14f * ease);
    const float slot = 74.0f * scale; // each weapon's circle; neighbours keep clear of each other
    const float middle = 110.0f * scale;
    auto* draw = ImGui::GetForegroundDrawList();
    Ui::Veil(0.62f * ease);
    draw->AddCircleFilled(center, middle, Fade(IM_COL32(0, 0, 0, 130), ease), 64);
    draw->AddCircle(center, middle, Fade(IM_COL32(244, 242, 232, 90), ease), 64, 1.5f * scale);

    const float length = std::hypot(motion_x, motion_y);
    if (length > 40.0f) {
        // Keep the pointer on the rim: a long swing does not have to be undone to go back.
        motion_x *= 120.0f / std::max(length, 120.0f);
        motion_y *= 120.0f / std::max(length, 120.0f);
        const float angle = std::atan2(motion_x, -motion_y); // 0 at the top, clockwise
        const float step = 2.0f * Pi / Weapons.size();
        const float turns = std::fmod(angle + step * 0.5f + 4.0f * Pi, 2.0f * Pi);
        chosen = static_cast<int>(turns / step) % static_cast<int>(Weapons.size());
    }

    // Each slot grows into its selection and shrinks back, and the pointer at the middle swings
    // to the chosen slot by the shorter way round.
    static std::array<float, Weapons.size()> grow{};
    static float pointer = 0.0f;
    static bool pointer_shown = false;
    const float follow = 1.0f - std::exp(-dt / 0.06f);
    for (size_t i = 0; i < Weapons.size(); i++) {
        grow[i] += ((static_cast<int>(i) == chosen ? 1.0f : 0.0f) - grow[i]) * follow;
    }
    if (now < 0.02f) {
        grow.fill(0.0f);
        pointer_shown = false;
    }
    if (chosen >= 0) {
        const float target = 2.0f * Pi * chosen / Weapons.size();
        if (!pointer_shown) {
            pointer = target;
            pointer_shown = true;
        }
        const float diff = std::remainder(target - pointer, 2.0f * Pi);
        pointer += diff * (1.0f - std::exp(-dt / 0.05f));
        const ImVec2 dir{std::sin(pointer), -std::cos(pointer)};
        const ImVec2 side{-dir.y, dir.x};
        const ImVec2 tip{center.x + dir.x * (middle + 18.0f * scale),
                         center.y + dir.y * (middle + 18.0f * scale)};
        const ImVec2 base{center.x + dir.x * (middle + 4.0f * scale),
                          center.y + dir.y * (middle + 4.0f * scale)};
        draw->AddTriangleFilled(tip, {base.x + side.x * 10.0f * scale, base.y + side.y * 10.0f * scale},
                                {base.x - side.x * 10.0f * scale, base.y - side.y * 10.0f * scale},
                                Fade(Ui::TextBright, ease));
    }

    const s32 in_hand = InHand();
    for (size_t i = 0; i < Weapons.size(); i++) {
        const Weapon& weapon = Weapons[i];
        const float angle = 2.0f * Pi * i / Weapons.size();
        const ImVec2 at{center.x + std::sin(angle) * radius, center.y - std::cos(angle) * radius};
        const float g = grow[i];
        const float r = slot * (1.0f + 0.14f * g);
        const bool owned = HasBit(OwnedItems, weapon.item);
        const ImU32 base_color = owned ? Ui::TextNormal : IM_COL32(150, 148, 140, 255);
        const ImU32 color = Fade(g > 0.5f ? Ui::TextBright : base_color, ease);
        draw->AddCircleFilled(at, r, Fade(IM_COL32(0, 0, 0, 110 + static_cast<int>(90 * g)), ease), 48);
        draw->AddCircle(at, r, Fade(IM_COL32(244, 242, 232, 70 + static_cast<int>(160 * g)), ease), 48,
                        (1.2f + 1.3f * g) * scale);
        if (g > 0.05f) {
            // A soft ring breathing around the chosen slot.
            const float pulse = 0.5f + 0.5f * std::sin(now * 6.0f);
            draw->AddCircle(at, r + (6.0f + 4.0f * pulse) * scale,
                            Fade(IM_COL32(244, 242, 232, 255), g * (0.18f + 0.22f * pulse) * ease),
                            48, 2.0f * scale);
        }
        DrawIcon(draw, weapon.kind, {at.x, at.y - r * 0.3f}, r * 0.72f, color);
        // The name in the lower part of the circle, where it is about 1.4 radii wide.
        FitText(draw, 19.0f * scale * (1.0f + 0.1f * g), 11.0f * scale, at.x, at.y + r * 0.18f,
                r * 1.4f, color, weapon.name);
        if (weapon.kind == in_hand) {
            Ui::Triangle(draw, {at.x, at.y + r + 9.0f * scale}, 8.0f * scale, true,
                         Fade(Ui::TextBright, ease));
        }
    }
    if (chosen >= 0) {
        const Weapon& weapon = Weapons[chosen];
        const bool bonus = !HasBit(OwnedItems, weapon.item);
        FitText(draw, 34.0f * scale, 16.0f * scale, center.x,
                center.y - (bonus ? 44.0f : 22.0f) * scale, middle * 1.6f,
                Fade(Ui::TextBright, ease), weapon.name);
        if (bonus) {
            FitText(draw, 20.0f * scale, 12.0f * scale, center.x, center.y + 30.0f * scale,
                    middle * 1.5f, Fade(Ui::TextNormal, ease), "Bonus item: given when chosen");
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
        // The game runs in slow motion while the wheel is up; its sound plays on.
        Libraries::Kernel::Dev::SetTimeScale(0.2);
        opened_at = Clock::now();
    }
    motion_x += Input::menu_motion_x.exchange(0.0f);
    motion_y += Input::menu_motion_y.exchange(0.0f);
    if (held) {
        DrawWheel();
        return;
    }
    open = false;
    Libraries::Kernel::Dev::SetTimeScale(1.0);
    if (chosen >= 0) {
        Take(Weapons[chosen]);
    }
}

} // namespace Sotc::Wheel
