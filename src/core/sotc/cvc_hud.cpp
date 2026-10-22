// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <random>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include "common/singleton.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/sotc/challenge.h"
#include "core/sotc/cvc_hud.h"
#include "core/sotc/cvc_physics.h"
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

// The pulse: what to draw and until when (SDL ticks).
struct Pulse {
    std::mutex lock;
    float charge_at[3]{};
    u64 charge_from = 0, charge_until = 0;
    float from[3]{}, to[3]{};
    u64 bolt_from = 0, bolt_until = 0;
};
Pulse g_pulse;

// World to screen through the render camera's view-projection (camera_patch.py leaves the
// render camera component at cave 0x1593d48; its +0x380 holds view, projection and, at +0xc0,
// their product, row-vector form). False behind the camera.
bool Project(u64 image, const float* p, const ImVec2& display, ImVec2& out) {
    u64 camera = 0, matrices = 0;
    if (!Read(image + 0x1593d48, camera) || !Read(camera + 0x380, matrices) ||
        !Core::Memory::Instance()->IsValidMapping(matrices + 0xc0, 64)) {
        return false;
    }
    const auto* m = reinterpret_cast<const volatile float*>(matrices + 0xc0);
    float clip[4];
    for (int j = 0; j < 4; j++) {
        clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
    }
    if (clip[3] <= 0.01f) {
        return false;
    }
    out.x = (clip[0] / clip[3] * 0.5f + 0.5f) * display.x;
    out.y = (0.5f - clip[1] / clip[3] * 0.5f) * display.y;
    return true;
}

void DrawPulse(u64 image, ImDrawList* draw, const ImVec2& display, float scale, u64 now) {
    float charge_at[3], from[3], to[3];
    u64 charge_from, charge_until, bolt_from, bolt_until;
    {
        std::scoped_lock guard{g_pulse.lock};
        std::copy_n(g_pulse.charge_at, 3, charge_at), std::copy_n(g_pulse.from, 3, from), std::copy_n(g_pulse.to, 3, to);
        charge_from = g_pulse.charge_from, charge_until = g_pulse.charge_until;
        bolt_from = g_pulse.bolt_from, bolt_until = g_pulse.bolt_until;
    }
    static std::mt19937 rng{7};
    std::uniform_real_distribution<float> jitter(-1.0f, 1.0f);
    // Charging: a pale blue glow at its mouth, growing and throbbing.
    if (now < charge_until) {
        ImVec2 at;
        if (Project(image, charge_at, display, at)) {
            const float t = float(now - charge_from) / float(std::max<u64>(charge_until - charge_from, 1));
            const float throb = 0.75f + 0.25f * std::sin(float(now) * 0.03f);
            const float r = (6.0f + 18.0f * t) * throb * scale;
            draw->AddCircleFilled(at, r * 2.2f, IM_COL32(120, 190, 255, int(40 * t)), 24);
            draw->AddCircleFilled(at, r * 1.3f, IM_COL32(160, 215, 255, int(90 * t)), 24);
            draw->AddCircleFilled(at, r * 0.6f, IM_COL32(235, 245, 255, int(200 * t)), 16);
        }
    }
    // The bolt: a jagged line, new each frame (it flickers), bright core and pale halo, fading.
    if (now < bolt_until) {
        ImVec2 a, b;
        if (Project(image, from, display, a) && Project(image, to, display, b)) {
            const float life = 1.0f - float(now - bolt_from) / float(std::max<u64>(bolt_until - bolt_from, 1));
            const ImVec2 d{b.x - a.x, b.y - a.y};
            const float length = std::max(std::hypot(d.x, d.y), 1.0f);
            const ImVec2 n{-d.y / length, d.x / length};
            const int parts = std::clamp(int(length / (14.0f * scale)), 6, 40);
            std::vector<ImVec2> path;
            for (int i = 0; i <= parts; i++) {
                const float t = float(i) / float(parts);
                const float sway = (i == 0 || i == parts) ? 0.0f : jitter(rng) * 9.0f * scale;
                path.push_back({a.x + d.x * t + n.x * sway, a.y + d.y * t + n.y * sway});
            }
            const int alpha = int(255 * life);
            draw->AddPolyline(path.data(), int(path.size()), IM_COL32(110, 170, 255, alpha / 4), 0, 22.0f * scale);
            draw->AddPolyline(path.data(), int(path.size()), IM_COL32(150, 205, 255, alpha / 2), 0, 9.0f * scale);
            draw->AddPolyline(path.data(), int(path.size()), IM_COL32(240, 248, 255, alpha), 0, 3.0f * scale);
            // A fork or two off the main bolt.
            for (int f = 0; f < 2; f++) {
                const int from_part = 2 + int(rng() % std::max(parts - 4, 1));
                ImVec2 p = path[from_part];
                for (int s = 0; s < 4; s++) {
                    const ImVec2 q{p.x + (d.x / parts) * 0.8f + n.x * jitter(rng) * 14.0f * scale,
                                   p.y + (d.y / parts) * 0.8f + n.y * jitter(rng) * 14.0f * scale};
                    draw->AddLine(p, q, IM_COL32(190, 225, 255, alpha / 2), 1.5f * scale);
                    p = q;
                }
            }
            draw->AddCircleFilled(a, 16.0f * scale * life, IM_COL32(170, 215, 255, alpha / 2), 20);
            draw->AddCircleFilled(b, 26.0f * scale * life, IM_COL32(200, 230, 255, alpha / 2), 24);
            draw->AddCircleFilled(b, 10.0f * scale * life, IM_COL32(250, 252, 255, alpha), 16);
        }
    }
}

} // namespace

void Charge(const float* at, float seconds) {
    std::scoped_lock guard{g_pulse.lock};
    std::copy_n(at, 3, g_pulse.charge_at);
    g_pulse.charge_from = SDL_GetTicks();
    g_pulse.charge_until = g_pulse.charge_from + u64(seconds * 1000.0f);
}

void Bolt(const float* from, const float* to, float seconds) {
    std::scoped_lock guard{g_pulse.lock};
    std::copy_n(from, 3, g_pulse.from), std::copy_n(to, 3, g_pulse.to);
    g_pulse.bolt_from = SDL_GetTicks();
    g_pulse.bolt_until = g_pulse.bolt_from + u64(seconds * 1000.0f);
    g_pulse.charge_until = 0;
}

namespace {
// Debug (CVC_HULLS set): each collision part's outline over the picture (the 2-D hull of its
// points on screen), A orange, B cyan: how the physics' shapes sit on the colossi.
void DrawShapes(u64 image, ImDrawList* draw, const ImVec2& display) {
    for (int k = 0; k < 2; k++) {
        for (const auto& part : Sotc::CvcPhysics::Shapes(k)) {
            std::vector<ImVec2> pts;
            for (const auto& p : part) {
                ImVec2 at;
                if (Project(image, p.data(), display, at)) {
                    pts.push_back(at);
                }
            }
            if (pts.size() < 3) {
                continue;
            }
            // Monotone chain hull on screen.
            std::sort(pts.begin(), pts.end(), [](const ImVec2& a, const ImVec2& b) {
                return a.x < b.x || (a.x == b.x && a.y < b.y);
            });
            std::vector<ImVec2> hull(2 * pts.size());
            size_t n = 0;
            const auto cross = [](const ImVec2& o, const ImVec2& a, const ImVec2& b) {
                return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
            };
            for (size_t i = 0; i < pts.size(); i++) {
                while (n >= 2 && cross(hull[n - 2], hull[n - 1], pts[i]) <= 0.0f) {
                    n--;
                }
                hull[n++] = pts[i];
            }
            for (size_t i = pts.size() - 1, t = n + 1; i-- > 0;) {
                while (n >= t && cross(hull[n - 2], hull[n - 1], pts[i]) <= 0.0f) {
                    n--;
                }
                hull[n++] = pts[i];
            }
            draw->AddPolyline(hull.data(), int(n), k ? IM_COL32(80, 220, 255, 200) : IM_COL32(255, 160, 40, 200),
                              ImDrawFlags_Closed, 1.5f);
        }
    }
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

    if (!Screens::Letterboxed()) {
        DrawPulse(image, draw, display, scale, now);
        if (std::getenv("CVC_HULLS")) {
            DrawShapes(image, draw, display);
        }
    }

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
