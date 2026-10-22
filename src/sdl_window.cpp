// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <set>
#include <vector>
#include <array>
#include <random>
#include <atomic>
#include <list>
#include <mutex>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/path_util.h"
#include "common/singleton.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/sotc/achievements.h"
#include "core/sotc/challenge.h"
#include "core/sotc/cvc_hud.h"
#include "core/sotc/cvc_physics.h"
#include "core/sotc/menu.h"
#include "core/sotc/watch.h"
#include "core/sotc/wheel.h"
#include "core/sotc/menu_ext.h"
#include "core/sotc/perf_hud.h"
#include "core/sotc/screens.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/logging/formatter.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "core/user_settings.h"
#include "imgui/friends_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input/input_mouse.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include <core/emulator_settings.h>
#include "core/libraries/keyboard/keyboard.h"
#include "core/libraries/mouse/sdl_mouse.h"
#include "video_core/texture_cache/texture_cache.h"

CMRC_DECLARE(res);

// Set by the video-out driver on the first flip.
extern std::atomic<u64> g_first_flip_ms;

extern std::atomic<int> g_sotc_record;

namespace Frontend {

using namespace Libraries::Pad;

static OrbisPadButtonDataOffset SDLGamepadToOrbisButton(u8 button) {
    using OPBDO = OrbisPadButtonDataOffset;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return OPBDO::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return OPBDO::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return OPBDO::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return OPBDO::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return OPBDO::Cross;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return OPBDO::Triangle;
    case SDL_GAMEPAD_BUTTON_WEST:
        return OPBDO::Square;
    case SDL_GAMEPAD_BUTTON_EAST:
        return OPBDO::Circle;
    case SDL_GAMEPAD_BUTTON_START:
        return OPBDO::Options;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return OPBDO::L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return OPBDO::R1;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return OPBDO::L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return OPBDO::R3;
    default:
        return OPBDO::None;
    }
}

static Uint32 SDLCALL PollController(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollState();
    return interval;
}

// Test driver: SOTC_SCRIPT="95:cross,100:shot,160:quit" runs pad presses, screenshots and exit at
// the given seconds after start, so runs need no desktop focus.
// Window that scripted key and mouse events are addressed to; the overlay UI ignores events
// that name no window.
static SDL_WindowID g_script_window = 0;

// Virtual gamepad for scripted controller tests (SOTC_VPAD=1). SDL reports it like a real
// device, so its input takes the same path as a physical controller: detection, gamepad
// events and the binding layer.
static SDL_Joystick* g_virtual_pad = nullptr;

static void AttachVirtualPad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = 0x045E; // reported as an Xbox 360 pad
    desc.product_id = 0x028E;
    desc.name = "Scripted test gamepad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    g_virtual_pad = id ? SDL_OpenJoystick(id) : nullptr;
    if (!g_virtual_pad) {
        LOG_ERROR(Input, "Could not create the virtual gamepad: {}", SDL_GetError());
    }
}

/// Test aid: Wander's object (first of list world+0x228), for a colossus left without an opponent:
/// its one-entry target list must never hold 0 (lock-first-node's callers use the object, 0x578889).
static u64 PlayerObject(u64 image) {
    const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
    const u64 world = q(image + 0x15fe570), base = q(image + 0x2f3b8c0);
    const s32 handle = world ? *reinterpret_cast<const volatile s32*>(world + 0x228) : 0;
    const u64 node = handle ? (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff) : 0;
    return node ? q(node) : 0;
}

/// Test aid: line tests for the CvC spectator camera with the game's own camera collision
/// (re/cvc/camera_patch.py). cvccam queues up to 40 lines and sets the state to 1; on the game
/// thread the camera calls CvcRayThunk, which runs 0x7a7e40 for each (true when nothing is in the
/// way, else the hit position at +0 of its 0x30-byte result) and sets the state to 2.
struct CvcRays {
    float from[52][4], to[52][4], hit[52][4]; // hit: where the line stops (to when clear)
    int count = 0;
    std::atomic<int> state{0}; // 0 idle, 1 queued, 2 done
    u64 image = 0;
    // Motions to start on colossi, on the game thread: a knocked-out guest's death (its host level's
    // death sequence only plays the level's own boss's), attacks and hit reactions (cvcbrawl).
    // Static MotionKind records (e.g. Knight_died_early). A slot is free while its object is 0.
    struct MotionRequest {
        std::atomic<u64> object{0};
        u64 motion = 0;
        float blend = 0.0f; // seconds of crossfade from the pose it had (0x635740's float)
        int waited = 0; // camera updates it has waited for its colossus's AI turn
    } motions[4];
    // Effects to start (game thread): an effect's name (static text) at a point.
    struct EffectRequest {
        std::atomic<const char*> name{nullptr};
        float at[4]{};
        float yaw = 0.0f; // its heading about the vertical (radians)
    } effects[24];
    // Basaran's own pulse (game thread; re/cvc/fx/PULSE.md): 1 start the charge on `object`, 2 fire
    // from it at `target`.
    std::atomic<int> pulse{0};
    u64 pulse_object = 0;
    float pulse_target[4]{};
    u64 pulse_charge = 0; // the charge effect's actor while it gathers
    // Sounds to play (game thread): a sound's name (static text) at a point.
    struct SoundRequest {
        std::atomic<const char*> name{nullptr};
        float at[3]{};
    } sounds[8];
};
static CvcRays g_cvc_rays;

static u32 MoveCharacter(u64 object, const float delta[3]);
static void RotateCharacter(u64 object, float yaw);

/// Colossus vs colossus bodies, moved on the game thread once a frame (CvcRayWork, from the
/// camera update) so nothing a fight adds to their own animation lands between two frames:
/// the push of a blow (a velocity that dies away), turning toward the other while it walks or
/// turns (eased in and out), and two bodies that touch easing apart. cvcbrawl asks for these and
/// reads back what each one is playing.
struct CvcBody {
    std::atomic<u64> object{0};
    std::mutex lock;
    float push[2]{};            // knockback velocity, units/s in x and z (lock)
    float apart[2]{};           // easing apart from the other body, units/s (lock)
    float step[2]{};            // stepping in to strike, units/s (lock; unused: bodies walk in)
    float ours = 0.0f;          // how far our code moved its root lately (no animation of its own: glides in the log)
    float ours_xz[2]{};         // what our code moved its root by since the last trace record (x, z)
    float ours_q[2]{};          // the same since the last quality record (CVC_QUALITY)
    std::atomic<float> turn{0}; // yaw rate asked for (rad/s, + counter-clockwise from above)
    float turning = 0.0f;       // yaw rate now, eased toward `turn`
    // Read back: the motion it plays (static MotionKind record), its frame and last frame.
    std::atomic<u64> motion{0};
    std::atomic<float> frame{0}, frames{0};
    float last[3]{};            // root position the frame before (pops in the log)
    u64 last_motion = 0;
    std::vector<std::array<float, 3>> pose; // bones relative to the root the frame before (snaps in the log)
};
static CvcBody g_cvc_body[2];

/// A blow's contact, found on the game thread with the game's own collision: while one colossus
/// swings (cvcbrawl sets `attacker`), each of its bones moving faster than `least` is swept from
/// where it was the frame before to where it is (0x7a7e40, the camera's filter), and a sweep that
/// meets something on the other colossus's body (near its bones, not the attacker's) is the blow.
/// A blow found by the physics (CvcPhysicsWork, game thread) for cvcbrawl: the strongest first
/// meeting of the two bodies since cvcbrawl last took one.
struct CvcBlow {
    std::mutex lock;
    bool ready = false;
    int striker = 0, bone = -1;
    float point[3]{}, normal[3]{}, impulse = 0.0f, closing = 0.0f;
};
static CvcBlow g_cvc_blow;
static std::atomic<float> g_cvc_gap{1e9f}; // least gap between the bodies since cvcbrawl reset it (misses in the log)
static std::atomic<float> g_cvc_gap_now{1e9f}; // the gap between the bodies this frame
static std::atomic<bool> g_cvc_touch_now{false}; // their hulls meet this frame
static std::atomic<float> g_cvc_depth_now{0.0f}; // how far their hulls are inside each other this frame
static std::atomic<int> g_cvc_contacts_now{0};    // how many part pairs meet this frame
// The spectator camera as last set (eye, point looked at), for the quality log.
static std::array<std::atomic<float>, 6> g_cvc_cam_now{};
// What cvcbrawl has each fighter doing (its Act), for the quality log.
static std::atomic<int> g_cvc_act_now[2]{};
// How much of the last snap's difference each colossus still carries (CvcInertiaWork), for the log.
static std::atomic<float> g_cvc_blend_now[2]{};
// How fast each colossus's striking part must meet the other for a blow (cvcbrawl: its moves' swing;
// 40 the default): slow heavy strikers (Quadratus's head, Basaran's slams) meet slower than a sword.
static std::atomic<float> g_cvc_coming[2]{40.0f, 40.0f};
// The ground under colossus A (the game's line test from just above its feet, 40 down; cvccam's ray
// batch), and the steady-clock second it was measured; NaN: nothing there.
static std::atomic<float> g_cvc_ground_a{std::numeric_limits<float>::quiet_NaN()};
static std::atomic<double> g_cvc_ground_a_at{0.0};

struct CvcContact {
    std::atomic<int> attacker{-1};
    std::atomic<float> least{60.0f};
    // The other's body around its bones (its collision covers only parts of it, e.g. Gaius's
    // climbable armour): a sweep passing this close to one of its bones touches it too.
    std::atomic<float> body{8.0f};
    std::atomic<bool> found{false};
    float point[3]{}, speed = 0.0f;
    int bone = -1;
    std::atomic<float> closest{1e9f}; // nearest a swept bone came to the other's bones (misses in the log)
    std::atomic<int> closest_bone{-1};
    std::vector<std::array<float, 3>> before[2]; // bones (world) the frame before
};
static CvcContact g_cvc_contact;
/// A colossus's physical reaction to a blow, laid over its animated pose: the game rebuilds the
/// final bone matrices (component +0x198) every frame before the camera update, and a change made
/// to them there is what gets drawn (tested), so each frame the pose is bent anew from the
/// game's own and nothing accumulates. Two springs: the body tilts away from the blow about a
/// point on the ground under it (bones weighted by their height, so the feet stay planted), and
/// the bones around the contact are driven in along the blow (a dent); both swing back and settle.
struct CvcReact {
    std::mutex lock;
    // The lean: a rotation about a horizontal axis through the feet, as a vector (x, z: its axis
    // times its angle; radians) and its rate (rad/s), so blows from any side add up.
    float tilt[2]{}, tilt_v[2]{};
    float dir[3]{};                   // the blow's direction (unit)
    float point[3]{};                 // where it landed
    float dent = 0.0f, dent_v = 0.0f; // units, units/s
    float sway = 0.0f, sway_v = 0.0f; // the whole body's mass shifted along the blow (units, units/s)
    // Off its footing until then (steady clock seconds): the motors holding it upright are weak, it
    // goes over much further before they bring it back.
    float weak_until = 0.0f;
};
static CvcReact g_cvc_react[2];

/// The physics' answer to a blow on colossus k (Sotc::CvcPhysics): `spin` its change of angular
/// velocity (rad/s; its horizontal part leans it), `give` how fast the struck part yields at
/// `point` along `dir` (x, z; units/s), `sway` how fast its upper body is carried along (units/s).
static void CvcReactSpin(int k, const float* spin, const float* point, const float* dir, float give, float sway) {
    auto& r = g_cvc_react[k];
    std::scoped_lock guard{r.lock};
    const float n = std::max(std::hypot(dir[0], dir[1]), 0.001f);
    r.dir[0] = dir[0] / n, r.dir[1] = 0.0f, r.dir[2] = dir[1] / n;
    std::memcpy(r.point, point, 12);
    r.tilt_v[0] += spin[0], r.tilt_v[1] += spin[2];
    r.dent_v += give;
    r.sway_v += sway;
}

/// A blow on colossus k at `point` along `dir` (x, z) with impulse `force` (1: a full blow; a graze
/// 0.1, the heaviest 3); `share` scales it (the striker rocks back a little the other way).
static void CvcReactImpulse(int k, const float* dir, const float* point, float force, float share = 1.0f) {
    auto& r = g_cvc_react[k];
    std::scoped_lock guard{r.lock};
    const float n = std::max(std::hypot(dir[0], dir[1]), 0.001f);
    r.dir[0] = dir[0] / n, r.dir[1] = 0.0f, r.dir[2] = dir[1] / n;
    // Tilting away from the blow: about the horizontal axis up x dir.
    std::memcpy(r.point, point, 12);
    r.tilt_v[0] += r.dir[2] * 1.5f * force * share, r.tilt_v[1] -= r.dir[0] * 1.5f * force * share;
    r.dent_v += 30.0f * force * share;
    r.sway_v += 26.0f * force * share;
}

/// Per-limb give: the struck part's chain of bones bending at its joints and springing back to the
/// animated pose (the motors), each bone's turn a rotation vector (world axis x angle, radians) and
/// its rate (rad/s).
struct CvcFlex {
    std::mutex lock;
    int kind = -1;
    std::array<std::array<float, 3>, Sotc::CvcPhysics::MaxBones> angle{}, rate{};
};
static CvcFlex g_cvc_flex[2];

/// Whether colossus `kind`'s bone is part of a limb, which may give: a bone with few bones below it
/// (at most a sixth of its skeleton, 3 at least), not the root, hips or spine, which carry the whole
/// body (turned there, the bodies bent in half: bones 1 and 5 in the quality log).
static bool CvcLimb(int kind, int bone) {
    const auto* parents = Sotc::CvcPhysics::Parents(kind);
    if (!parents || bone <= 0 || bone >= int(parents->size())) {
        return false;
    }
    static std::map<int, std::vector<int>> below; // per kind: how many bones hang below each (itself too)
    auto& count = below[kind];
    if (count.empty()) {
        count.assign(parents->size(), 0);
        for (size_t b = 0; b < parents->size(); b++) {
            for (int d = int(b), guard = 0; d >= 0 && guard < 300; d = (*parents)[size_t(d)], guard++) {
                count[size_t(d)]++;
            }
        }
    }
    return count[size_t(bone)] <= std::max(3, int(parents->size()) / 6);
}

/// A blow felt by colossus k's bone `bone` at `point`, pushing along `dir` (unit) at `speed`
/// (units/s, what that region of it is given): that bone and up to three above it turn about their
/// joints (r x v / r^2, halving up the chain).
static void CvcFlexImpulse(int k, int kind, const float* bones, int bone, const float* point, const float* dir,
                           float speed) {
    const auto* parents = Sotc::CvcPhysics::Parents(kind);
    if (!parents || bone <= 0 || bone >= int(parents->size())) {
        return;
    }
    auto& f = g_cvc_flex[k];
    std::scoped_lock guard{f.lock};
    f.kind = kind;
    float share = 1.0f;
    for (int b = bone, depth = 0; b > 0 && depth < 4 && CvcLimb(kind, b); b = (*parents)[b], depth++, share *= 0.5f) {
        const float* m = bones + b * 16;
        const float r[3]{point[0] - m[12], point[1] - m[13], point[2] - m[14]};
        const float r2 = std::max(r[0] * r[0] + r[1] * r[1] + r[2] * r[2], 4.0f);
        const float w[3]{(r[1] * dir[2] - r[2] * dir[1]), (r[2] * dir[0] - r[0] * dir[2]), (r[0] * dir[1] - r[1] * dir[0])};
        auto& rate = f.rate[size_t(b)];
        for (int c = 0; c < 3; c++) {
            rate[c] += w[c] * speed / r2 * share;
        }
        if (const float n = std::hypot(rate[0], rate[1], rate[2]); n > 8.0f) {
            for (float& c : rate) {
                c *= 8.0f / n;
            }
        }
    }
}

/// A part of colossus k inside the other: its bone turned at its joint so the point moves `by`
/// along `dir` (unit), out of it (position, not speed: the joint's spring brings it back).
static void CvcFlexShift(int k, int kind, const float* bones, int bone, const float* point, const float* dir, float by) {
    const auto* parents = Sotc::CvcPhysics::Parents(kind);
    if (!parents || bone <= 0 || bone >= int(parents->size())) {
        return;
    }
    if (!CvcLimb(kind, bone)) {
        return;
    }
    auto& f = g_cvc_flex[k];
    std::scoped_lock guard{f.lock};
    f.kind = kind;
    const float* m = bones + bone * 16;
    const float r[3]{point[0] - m[12], point[1] - m[13], point[2] - m[14]};
    const float r2 = std::max(r[0] * r[0] + r[1] * r[1] + r[2] * r[2], 4.0f);
    auto& angle = f.angle[size_t(bone)];
    angle[0] += (r[1] * dir[2] - r[2] * dir[1]) * by / r2;
    angle[1] += (r[2] * dir[0] - r[0] * dir[2]) * by / r2;
    angle[2] += (r[0] * dir[1] - r[1] * dir[0]) * by / r2;
}

using CvcAngles = std::array<std::array<float, 3>, Sotc::CvcPhysics::MaxBones>;
static void CvcFlexBend(int kind, const CvcAngles& angle, float* matrices, int count);

/// Colossus k's limbs giving (CvcFlex) on its bone matrices: the springs stepped, then each bone
/// with a turn turns itself and everything below it about its joint, parents first. Its matrices
/// hold its bones twice (35 of them for Gaius: 35-69 repeat 0-34): both turned.
static void CvcFlexWork(int k, u64 matrices, int count, float dt) {
    auto& f = g_cvc_flex[k];
    CvcAngles angle;
    int kind;
    {
        std::scoped_lock guard{f.lock};
        kind = f.kind;
        constexpr float w0 = 9.0f, zeta = 0.45f; // (back to its pose in about half a second)
        for (size_t b = 0; b < f.angle.size() && dt > 0.0f; b++) {
            for (int c = 0; c < 3; c++) {
                f.rate[b][c] += (-w0 * w0 * f.angle[b][c] - 2.0f * zeta * w0 * f.rate[b][c]) * dt;
                f.angle[b][c] += f.rate[b][c] * dt;
            }
            // (A joint gives so far: 0.15 rad, 9 degrees; at 0.4 the quality log had every limb at the
            // limit while the bodies were locked together.)
            if (const float a = std::hypot(f.angle[b][0], f.angle[b][1], f.angle[b][2]); a > 0.15f) {
                for (float& c : f.angle[b]) {
                    c *= 0.15f / a;
                }
            }
        }
        angle = f.angle;
    }
    if (matrices) {
        static float m[2 * Sotc::CvcPhysics::MaxBones * 16];
        std::memcpy(m, reinterpret_cast<const void*>(matrices), size_t(count) * 64);
        CvcFlexBend(kind, angle, m, count);
        std::memcpy(reinterpret_cast<void*>(matrices), m, size_t(count) * 64);
    }
}

/// Bones bent by their turns (CvcFlex angles) on matrices `m` (`count` of them, 4x4 row vectors;
/// past its bones' count the same bones again): each bone with a turn turns itself and everything
/// below it about its joint, parents first.
static void CvcFlexBend(int kind, const CvcAngles& angle, float* matrices, int count) {
    const auto* parents = Sotc::CvcPhysics::Parents(kind);
    if (!parents) {
        return;
    }
    // Parents first, and what hangs below each bone (itself included), once per kind.
    const int bones = int(parents->size());
    static std::map<int, std::pair<std::vector<int>, std::vector<std::vector<int>>>> trees;
    auto& [order, below] = trees[kind];
    if (order.empty()) {
        below.resize(size_t(bones));
        order.push_back(0);
        for (size_t i = 0; i < order.size(); i++) {
            for (int c = 0; c < bones; c++) {
                if ((*parents)[c] == order[i]) {
                    order.push_back(c);
                }
            }
        }
        for (int b = 0; b < bones; b++) {
            for (int d = b; d >= 0; d = (*parents)[d]) {
                below[size_t(d)].push_back(b);
            }
        }
    }
    for (const int b : order) {
        const auto& t = angle[size_t(b)];
        const float a = std::hypot(t[0], t[1], t[2]);
        if (a < 1e-4f) {
            continue;
        }
        const float axis[3]{t[0] / a, t[1] / a, t[2] / a}, c = std::cos(a), s = std::sin(a);
        const auto rotate = [&](float* v) {
            const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
            const float cr[3]{axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
            for (int q = 0; q < 3; q++) {
                v[q] = v[q] * c + cr[q] * s + axis[q] * dot * (1.0f - c);
            }
        };
        const float* joint = matrices + b * 16;
        const float pivot[3]{joint[12], joint[13], joint[14]};
        for (const int d : below[size_t(b)]) {
            for (const int i : {d, d + bones}) {
                if (i >= count) {
                    continue;
                }
                float* m = matrices + i * 16;
                for (int row = 0; row < 3; row++) {
                    float v[3]{m[row * 4], m[row * 4 + 1], m[row * 4 + 2]};
                    rotate(v);
                    m[row * 4] = v[0], m[row * 4 + 1] = v[1], m[row * 4 + 2] = v[2];
                }
                float v[3]{m[12] - pivot[0], m[13] - pivot[1], m[14] - pivot[2]};
                rotate(v);
                m[12] = pivot[0] + v[0], m[13] = pivot[1] + v[1], m[14] = pivot[2] + v[2];
            }
        }
    }
}

/// No snaps of the pose: when colossus k's motion changes (or its root jumps), the drawn pose that
/// frame starts from where the frame before left it, and the difference dies away (about 0.15 s),
/// so a motion begun on a pose that does not join the last one (Gaius's sword at the start of his
/// slam, Basaran's head into its walk, its root dropping into its slam) carries on from it.
static void CvcInertiaWork(int k, u64 matrices, int count, float dt) {
    constexpr int most = 2 * Sotc::CvcPhysics::MaxBones;
    static float last[2][most * 16], now[most * 16];
    // Each bone's difference at the snap: the turn from where it is drawn now to where it was (axis,
    // angle) and the move (x y z); both drawn at `amount` of themselves, dying away. (A bone is turned
    // along its shortest way, rigidly: the matrices blended entry by entry and squared again flipped
    // bones over, 180 degrees in the quality log.)
    static float turn[2][most][4], move[2][most][3], amount[2]{};
    // Whether each bone hangs rigidly from its parent (as recorded: re/cvc/skeleton.py's guess), its
    // offset in its parent's frame the same in both poses: then it is drawn where its parent puts it.
    static bool rigid[2][most]{};
    static float was_off[2][most][3]{}; // each bone's offset from its parent at the snap, as it was drawn
    static bool have[2]{};
    static u64 last_motion[2]{};
    static float last_step[2]{}; // how far its root moved the frame before (a steady lunge is no snap)
    static float fade[2]{0.15f, 0.15f}; // how long this snap's difference takes to die away (s)
    const u64 motion = g_cvc_body[k].motion.load();
    const size_t n = size_t(count) * 16;
    std::memcpy(now, reinterpret_cast<const void*>(matrices), n * 4);
    float step = 0.0f;
    if (have[k]) {
        step = std::hypot(now[12] - (last[k][12] - move[k][0][0] * amount[k]),
                          now[13] - (last[k][13] - move[k][0][1] * amount[k]),
                          now[14] - (last[k][14] - move[k][0][2] * amount[k]));
    }
    // (A snap: its motion changed, or its root moved a unit more than it did the frame before; a
    // steady lunge or walk is none: at a fixed 0.6 a frame, every frame of a lunge restarted it and the
    // bodies and the view following them shook.)
    const bool snap = have[k] && motion && last_motion[k] && (motion != last_motion[k] || step > last_step[k] + 1.0f);
    last_step[k] = step;
    const auto unit = [](const float* v, float* u) {
        const float l = std::hypot(v[0], v[1], v[2]);
        for (int c = 0; c < 3; c++) {
            u[c] = l > 1e-6f ? v[c] / l : 0.0f;
        }
        return l > 1e-6f;
    };
    if (snap) {
        for (int b = 0; b < count; b++) {
            const float *was = last[k] + b * 16, *is = now + b * 16;
            // W = sum over the axes of was_i (x) is_i: the world turn taking each axis now to where it was.
            float a[3][3], c[3][3], w[9]{};
            bool ok = true;
            for (int i = 0; i < 3; i++) {
                ok &= unit(was + i * 4, a[i]) && unit(is + i * 4, c[i]);
            }
            for (int i = 0; i < 3 && ok; i++) {
                for (int r = 0; r < 3; r++) {
                    for (int q = 0; q < 3; q++) {
                        w[r * 3 + q] += a[i][r] * c[i][q];
                    }
                }
            }
            const float cosine = std::clamp((w[0] + w[4] + w[8] - 1.0f) / 2.0f, -1.0f, 1.0f);
            float axis[3]{w[7] - w[5], w[2] - w[6], w[3] - w[1]};
            const float angle = std::acos(cosine);
            if (!ok || !unit(axis, axis) || angle < 1e-4f) {
                turn[k][b][0] = turn[k][b][1] = turn[k][b][2] = turn[k][b][3] = 0.0f;
            } else {
                turn[k][b][0] = axis[0], turn[k][b][1] = axis[1], turn[k][b][2] = axis[2], turn[k][b][3] = angle;
            }
            for (int q = 0; q < 3; q++) {
                move[k][b][q] = was[12 + q] - is[12 + q];
            }
        }
        if (const auto* parents = Sotc::CvcPhysics::Parents(g_cvc_flex[k].kind)) {
            for (int b = 0; b < std::min(int(parents->size()), count); b++) {
                const int q = (*parents)[size_t(b)];
                rigid[k][b] = false;
                if (q < 0 || q >= count) {
                    continue;
                }
                float error = 0.0f;
                for (int row = 0; row < 3; row++) {
                    float ax[3], bx[3];
                    unit(last[k] + q * 16 + row * 4, ax), unit(now + q * 16 + row * 4, bx);
                    float was = 0.0f, is = 0.0f;
                    for (int c = 0; c < 3; c++) {
                        was += ax[c] * (last[k][b * 16 + 12 + c] - last[k][q * 16 + 12 + c]);
                        is += bx[c] * (now[b * 16 + 12 + c] - now[q * 16 + 12 + c]);
                    }
                    error += (was - is) * (was - is);
                }
                rigid[k][b] = error < 0.3f * 0.3f;
                for (int c = 0; c < 3; c++) {
                    was_off[k][b][c] = last[k][b * 16 + 12 + c] - last[k][q * 16 + 12 + c];
                }
            }
        }
        amount[k] = 1.0f;
        // (The further the motion puts it, the longer: Basaran's death motion moves its root 13.8, which
        // in 0.15 s still read as a jump; 0.15 s and 0.03 s a unit more, at most 0.7 s.)
        const float moved_by = std::hypot(move[k][0][0], move[k][0][1], move[k][0][2]);
        fade[k] = std::min(0.15f + 0.03f * moved_by, 0.7f);
    } else {
        amount[k] *= std::exp(-dt / fade[k]);
    }
    last_motion[k] = motion, have[k] = true;
    g_cvc_blend_now[k] = amount[k];
    if (amount[k] > 1e-3f) {
        const auto rotate = [&](int b, float* v) { // bone b's turn, at `amount` of it, on vector v
            const float* t = turn[k][b];
            if (t[3] <= 0.0f) {
                return;
            }
            const float angle = t[3] * amount[k], c = std::cos(angle), sn = std::sin(angle);
            const float dot = t[0] * v[0] + t[1] * v[1] + t[2] * v[2];
            const float cr[3]{t[1] * v[2] - t[2] * v[1], t[2] * v[0] - t[0] * v[2], t[0] * v[1] - t[1] * v[0]};
            for (int q = 0; q < 3; q++) {
                v[q] = v[q] * c + cr[q] * sn + t[q] * dot * (1.0f - c);
            }
        };
        // Positions down the skeleton: a bone where its parent is drawn, at its own offset from it
        // (turned as its parent is), so no bone's length changes. (Each moved by its own share, they
        // drifted from their parents: Gaius's sword bone 31 units in the quality log.)
        const auto* parents = Sotc::CvcPhysics::Parents(g_cvc_flex[k].kind);
        const int bones = parents ? std::min(int(parents->size()), count) : 0;
        static std::map<int, std::vector<int>> orders;
        auto& order = orders[g_cvc_flex[k].kind];
        if (bones && order.empty()) {
            std::vector<bool> placed(size_t(bones), false);
            for (int pass = 0; pass < bones && int(order.size()) < bones; pass++) {
                for (int b = 0; b < bones; b++) {
                    const int q = (*parents)[size_t(b)];
                    if (!placed[size_t(b)] && (q < 0 || q >= bones || placed[size_t(q)])) {
                        placed[size_t(b)] = true, order.push_back(b);
                    }
                }
            }
        }
        static float anim[most * 16];
        std::memcpy(anim, now, n * 4);
        for (int b = 0; b < count; b++) {
            for (int row = 0; row < 3; row++) {
                rotate(b, now + b * 16 + row * 4);
            }
        }
        for (const int b : order) {
            const int q = (*parents)[size_t(b)];
            float* m = now + b * 16;
            if (q < 0 || q >= bones) {
                for (int c = 0; c < 3; c++) {
                    m[12 + c] = anim[b * 16 + 12 + c] + move[k][b][c] * amount[k];
                }
                continue;
            }
            if (!rigid[k][b]) {
                // (Not fixed in its parent's frame: its offset from the parent blended from the one it
                // had, at the length it has now; moved by its own share it was stretched 6 units.)
                float off[3], length = 0.0f, blended = 0.0f;
                for (int c = 0; c < 3; c++) {
                    const float is = anim[b * 16 + 12 + c] - anim[q * 16 + 12 + c];
                    off[c] = is + (was_off[k][b][c] - is) * amount[k];
                    length += is * is, blended += off[c] * off[c];
                }
                const float scale = blended > 1e-8f ? std::sqrt(length / blended) : 1.0f;
                for (int c = 0; c < 3; c++) {
                    m[12 + c] = now[q * 16 + 12 + c] + off[c] * scale;
                }
                continue;
            }
            float off[3]{anim[b * 16 + 12] - anim[q * 16 + 12], anim[b * 16 + 13] - anim[q * 16 + 13],
                         anim[b * 16 + 14] - anim[q * 16 + 14]};
            rotate(q, off);
            for (int c = 0; c < 3; c++) {
                m[12 + c] = now[q * 16 + 12 + c] + off[c];
            }
        }
        for (int b = bones; b < count; b++) { // the list's second copy (and any past it) as the first
            if (b - bones < bones) {
                std::memcpy(now + b * 16 + 12, now + (b - bones) * 16 + 12, 12);
            } else {
                for (int c = 0; c < 3; c++) {
                    now[b * 16 + 12 + c] += move[k][b][c] * amount[k];
                }
            }
        }
        if (!bones) {
            for (int b = 0; b < count; b++) {
                for (int c = 0; c < 3; c++) {
                    now[b * 16 + 12 + c] += move[k][b][c] * amount[k];
                }
            }
        }
    }
    std::memcpy(last[k], now, n * 4);
    std::memcpy(reinterpret_cast<void*>(matrices), now, n * 4);
}

/// Quality log (CVC_QUALITY set): each frame, each colossus, to testrun/cvc_quality.bin, how its
/// drawn pose differs from the one its animation gave (`anim`, before our smoothing, giving and
/// leaning): the most any joint's bend changed (degrees), a bone's length to its parent changed, a
/// bone moved, a bone's axes' lengths changed (not rigid); and its root, what our code moved it by,
/// its motion, whether the bodies touch and how deep, its lowest bone (feet sliding).
/// re/cvc/quality.py reads it (with the log's hits) into a per-fight summary.
static void CvcQuality(int k, u64 object, const float* anim, const float* drawn, int kind) {
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        out.open("D:/sotc-pc/testrun/cvc_quality.bin", std::ios::binary | std::ios::trunc);
        opened = true;
    }
    const auto* parents = Sotc::CvcPhysics::Parents(kind);
    if (!parents) {
        return;
    }
#pragma pack(push, 1)
    struct Record {
        double t;
        u32 k;
        float pos[3], ours[2];
        u64 motion;
        float frame;
        u32 touching;
        float depth, bend;
        s32 bend_bone;
        float stretch;
        s32 stretch_bone;
        float offset;
        s32 offset_bone;
        float scale;
        s32 scale_bone, low_bone;
        float low[3], yaw;
        u32 frame_no, contacts;
        float cam[6];
        s32 act;
        float give, tilt, blend;
        float low_anim[3]; // the lowest bone where its animation has it
    } r{};
#pragma pack(pop)
    auto& body = g_cvc_body[k];
    r.t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    r.k = u32(k);
    const auto* p = reinterpret_cast<const volatile float*>(object + 0x130);
    r.pos[0] = p[0], r.pos[1] = p[1], r.pos[2] = p[2];
    r.ours[0] = body.ours_q[0], r.ours[1] = body.ours_q[1];
    body.ours_q[0] = body.ours_q[1] = 0.0f;
    r.motion = body.motion.load(), r.frame = body.frame.load();
    r.touching = g_cvc_touch_now.load() ? 1 : 0, r.depth = g_cvc_depth_now.load();
    r.bend_bone = r.stretch_bone = r.offset_bone = r.scale_bone = -1;
    const int bones = int(parents->size());
    float lowest = 1e9f;
    const auto length = [](const float* m, int row) { return std::hypot(m[row * 4], m[row * 4 + 1], m[row * 4 + 2]); };
    for (int b = 0; b < bones; b++) {
        const float *a = anim + b * 16, *d = drawn + b * 16;
        const float off = std::hypot(d[12] - a[12], d[13] - a[13], d[14] - a[14]);
        if (off > r.offset) {
            r.offset = off, r.offset_bone = b;
        }
        for (int row = 0; row < 3; row++) {
            if (const float sc = std::abs(length(d, row) - length(a, row)); sc > r.scale) {
                r.scale = sc, r.scale_bone = b;
            }
        }
        if (d[13] < lowest) {
            lowest = d[13], r.low_bone = b, r.low[0] = d[12], r.low[1] = d[13], r.low[2] = d[14];
        }
        const int q = (*parents)[size_t(b)];
        if (q < 0 || q >= bones) {
            continue;
        }
        const float *ap = anim + q * 16, *dp = drawn + q * 16;
        const float la = std::hypot(a[12] - ap[12], a[13] - ap[13], a[14] - ap[14]);
        const float ld = std::hypot(d[12] - dp[12], d[13] - dp[13], d[14] - dp[14]);
        // (Only a link its animation keeps the same length: the recorded skeleton's guessed parents
        // include some that are no joint, whose length the animation itself changes.)
        static std::map<std::pair<int, int>, std::pair<float, float>> span; // kind, bone -> least, most
        auto& [least, longest] = span.try_emplace({kind, b}, la, la).first->second;
        least = std::min(least, la), longest = std::max(longest, la);
        if (longest - least < 0.3f && std::abs(ld - la) > r.stretch) {
            r.stretch = std::abs(ld - la), r.stretch_bone = b;
        }
        // The joint's bend: its rotation relative to its parent's, drawn against animated
        // (rel = R_b R_p^T, rows the axes; the angle of rel_d rel_a^T).
        float rel_a[9], rel_d[9];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                rel_a[i * 3 + j] = rel_d[i * 3 + j] = 0.0f;
                for (int c = 0; c < 3; c++) {
                    rel_a[i * 3 + j] += a[i * 4 + c] * ap[j * 4 + c] / std::max(length(a, i) * length(ap, j), 1e-6f);
                    rel_d[i * 3 + j] += d[i * 4 + c] * dp[j * 4 + c] / std::max(length(d, i) * length(dp, j), 1e-6f);
                }
            }
        }
        float trace = 0.0f;
        for (int i = 0; i < 9; i++) {
            trace += rel_d[i] * rel_a[i];
        }
        const float bend = std::acos(std::clamp((trace - 1.0f) / 2.0f, -1.0f, 1.0f)) * 57.29578f;
        if (bend > r.bend) {
            r.bend = bend, r.bend_bone = b;
        }
    }
    r.yaw = std::atan2(drawn[2], drawn[0]);
    static u32 frames = 0;
    r.frame_no = k == 0 ? ++frames : frames;
    r.contacts = u32(g_cvc_contacts_now.load());
    for (int c = 0; c < 6; c++) {
        r.cam[c] = g_cvc_cam_now[c].load();
    }
    r.act = g_cvc_act_now[k].load();
    r.blend = g_cvc_blend_now[k].load();
    if (r.low_bone >= 0) {
        std::memcpy(r.low_anim, anim + r.low_bone * 16 + 12, 12);
    }
    {
        std::scoped_lock guard{g_cvc_flex[k].lock};
        for (const auto& a : g_cvc_flex[k].angle) {
            r.give = std::max(r.give, std::hypot(a[0], a[1], a[2]) * 57.29578f);
        }
    }
    {
        std::scoped_lock guard{g_cvc_react[k].lock};
        r.tilt = std::hypot(g_cvc_react[k].tilt[0], g_cvc_react[k].tilt[1]) * 57.29578f;
    }
    out.write(reinterpret_cast<const char*>(&r), sizeof(r));
    // Every bone's drawn position (cvc_bones.bin: t, k, count, then x y z each).
    static std::ofstream bones_out("D:/sotc-pc/testrun/cvc_bones.bin", std::ios::binary | std::ios::trunc);
    const u32 head[2]{u32(k), u32(bones)};
    bones_out.write(reinterpret_cast<const char*>(&r.t), 8);
    bones_out.write(reinterpret_cast<const char*>(head), 8);
    for (int b = 0; b < bones; b++) {
        bones_out.write(reinterpret_cast<const char*>(drawn + b * 16 + 12), 12);
    }
}

// The pose each colossus's animation gave this frame, before any of ours (the quality log).
static const bool g_cvc_quality = std::getenv("CVC_QUALITY") != nullptr;
static float g_cvc_anim[2][2 * Sotc::CvcPhysics::MaxBones * 16];
static int g_cvc_anim_count[2]{};

/// No snaps, before anything reads the pose: the physics too sees each body as it is drawn. (It read
/// the animation's own pose: at a motion change the jump to the next one's first frame read as a
/// strike at 150 units a second.)
static void CvcInertiaPass(float dt) {
    for (int k = 0; k < 2; k++) {
        g_cvc_anim_count[k] = 0;
        const u64 object = g_cvc_body[k].object.load();
        if (!object || *reinterpret_cast<const volatile u8*>(object + 0xc) != 9) {
            continue;
        }
        const u64 matrices = *reinterpret_cast<const volatile u64*>(object + 0x120 + 0x198);
        const int count = Sotc::CvcPhysics::Matrices(g_cvc_flex[k].kind);
        if (count && matrices && Core::Memory::Instance()->IsValidMapping(matrices, size_t(count) * 64)) {
            if (g_cvc_quality) {
                std::memcpy(g_cvc_anim[k], reinterpret_cast<const void*>(matrices), size_t(count) * 64);
                g_cvc_anim_count[k] = count;
            }
            CvcInertiaWork(k, matrices, count, dt);
        }
    }
}

static void CvcReactWork(u64 image, float dt) {
    for (int k = 0; k < 2; k++) { // the limbs give, then the whole body leans (no snaps: CvcInertiaPass)
        const u64 object = g_cvc_body[k].object.load();
        if (!object || *reinterpret_cast<const volatile u8*>(object + 0xc) != 9) {
            continue;
        }
        const u64 matrices = *reinterpret_cast<const volatile u64*>(object + 0x120 + 0x198);
        const int count = Sotc::CvcPhysics::Matrices(g_cvc_flex[k].kind);
        if (count && matrices && Core::Memory::Instance()->IsValidMapping(matrices, size_t(count) * 64)) {
            CvcFlexWork(k, matrices, count, dt);
        }
    }
    for (int k = 0; k < 2; k++) {
        auto& r = g_cvc_react[k];
        const u64 object = g_cvc_body[k].object.load();
        float tilt, dent, sway, axis[3]{}, dir[3], point[3];
        {
            std::scoped_lock guard{r.lock};
            // Damped springs (the tilt slower, about 1.1 a second; the dent quicker).
            const auto spring = [&](float& x, float& v, float w0, float zeta) {
                const float a = -w0 * w0 * x - 2.0f * zeta * w0 * v;
                v += a * dt;
                x += v * dt;
                if (std::abs(x) < 1e-4f && std::abs(v) < 1e-3f) {
                    x = v = 0.0f;
                }
            };
            if (dt > 0.0f) {
                const float now_s = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const bool weak = now_s < r.weak_until;
                spring(r.tilt[0], r.tilt_v[0], weak ? 2.2f : 6.0f, weak ? 0.5f : 0.32f);
                spring(r.tilt[1], r.tilt_v[1], weak ? 2.2f : 6.0f, weak ? 0.5f : 0.32f);
                spring(r.dent, r.dent_v, 12.0f, 0.45f);
                spring(r.sway, r.sway_v, 5.0f, 0.4f);
                // (A rigid body rocks on its feet a few degrees; off its feet, more: at 0.5 / 1.05 rad,
                // bent by height, the bodies folded over, the quality log's worst bends.)
                const float most = weak ? 0.3f : 0.12f;
                if (const float lean = std::hypot(r.tilt[0], r.tilt[1]); lean > most) {
                    r.tilt[0] *= most / lean, r.tilt[1] *= most / lean;
                }
            }
            tilt = std::hypot(r.tilt[0], r.tilt[1]), dent = r.dent, sway = r.sway;
            if (tilt > 0.0f) {
                axis[0] = r.tilt[0] / tilt, axis[2] = r.tilt[1] / tilt;
            }
            std::memcpy(dir, r.dir, 12), std::memcpy(point, r.point, 12);
        }
        if (!object || (tilt == 0.0f && dent == 0.0f && sway == 0.0f) ||
            *reinterpret_cast<const volatile u8*>(object + 0xc) != 9) {
            continue;
        }
        const auto* at = reinterpret_cast<const volatile float*>(object + 0x130);
        const u64 matrices = *reinterpret_cast<const volatile u64*>(object + 0x120 + 0x198);
        const int count = Sotc::CvcPhysics::Matrices(g_cvc_flex[k].kind);
        if (!count || !matrices || !Core::Memory::Instance()->IsValidMapping(matrices, size_t(count) * 64)) {
            continue;
        }
        float low = 1e9f, high = -1e9f;
        for (int i = 0; i < count; i++) {
            const auto* m = reinterpret_cast<const volatile float*>(matrices + i * 64 + 0x30);
            low = std::min(low, float(m[1])), high = std::max(high, float(m[1]));
        }
        if (count == 0 || high - low < 1.0f) {
            continue;
        }
        const float pivot[3]{at[0], low, at[2]};
        float moved = 0.0f;
        for (int i = 0; i < count; i++) {
            auto* m = reinterpret_cast<volatile float*>(matrices + i * 64);
            // Rodrigues: rotate by tilt about `axis` through `pivot`, every bone alike (rigid).
            const float angle = tilt, c = std::cos(angle), s = std::sin(angle);
            const auto rotate = [&](float* v) {
                const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
                const float cr[3]{axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2],
                                  axis[0] * v[1] - axis[1] * v[0]};
                for (int q = 0; q < 3; q++) {
                    v[q] = v[q] * c + cr[q] * s + axis[q] * dot * (1.0f - c);
                }
            };
            for (int row = 0; row < 3; row++) { // the bone's own axes turn with it
                float v[3]{m[row * 4], m[row * 4 + 1], m[row * 4 + 2]};
                rotate(v);
                m[row * 4] = v[0], m[row * 4 + 1] = v[1], m[row * 4 + 2] = v[2];
            }
            float p[3]{m[12] - pivot[0], m[13] - pivot[1], m[14] - pivot[2]};
            rotate(p);
            // (No dent or sway: bones driven in near the contact, or the upper body carried along,
            // moved bones without their parents: stretched up to 9 units in the quality log.)
            float shift = 0.0f;
            for (int q = 0; q < 3; q++) {
                const float to = pivot[q] + p[q];
                shift += (to - m[12 + q]) * (to - m[12 + q]);
                m[12 + q] = to;
            }
            moved = std::max(moved, std::sqrt(shift));
        }
        static int logged[2]{};
        if (++logged[k] % 60 == 0) {
            LOG_INFO(Core, "cvcreact: {} tilt {:.3f} dent {:.1f}: bones moved up to {:.1f} ({} bones, {:.0f}..{:.0f} high)",
                     k ? "B" : "A", tilt, dent, moved, count, low, high);
        }
    }
    for (int k = 0; k < 2 && g_cvc_quality; k++) {
        const u64 object = g_cvc_body[k].object.load();
        if (g_cvc_anim_count[k] && object) {
            const u64 matrices = *reinterpret_cast<const volatile u64*>(object + 0x120 + 0x198);
            CvcQuality(k, object, g_cvc_anim[k], reinterpret_cast<const float*>(matrices), g_cvc_flex[k].kind);
        }
    }
}
// Test aid (cvcfree=B:40): a colossus left to its own AI (not held, no moves of ours) until then.
static std::atomic<s64> g_cvc_free_until[2]{};
// Camera shake of the spectator camera (cvccam): how far it throws the view (units) and when it
// started (steady clock, seconds).
static std::atomic<float> g_cvc_shake{0.0f}, g_cvc_shake_at{-10.0f};

// Hit-stop: the game's motion update 0x636650 (rdi the character, xmm0 the frame's time step)
// runs through CvcMotionThunk, which slows the clock of these two characters by these factors
// (1: as normal), so a blow freezes both for a moment.
extern "C" {
u64 g_cvc_hold_obj[2]{};
float g_cvc_hold_scale[2]{1.0f, 1.0f};
u64 g_cvc_motion_back = 0; // image + 0x63665d: the motion update after its replaced prologue
}
static std::atomic<s64> g_cvc_hold_until[2]{}; // steady clock ticks the hit-stop lasts until

extern "C" __attribute__((naked)) void CvcMotionThunk() {
    asm volatile("cmpq g_cvc_hold_obj(%rip), %rdi\n jne 1f\n"
                 "vmulss g_cvc_hold_scale(%rip), %xmm0, %xmm0\n jmp 2f\n"
                 "1: cmpq g_cvc_hold_obj+8(%rip), %rdi\n jne 2f\n"
                 "vmulss g_cvc_hold_scale+4(%rip), %xmm0, %xmm0\n"
                 // The replaced prologue of 0x636650, then on into it.
                 "2: push %rbp\n mov %rsp, %rbp\n push %r15\n push %r14\n push %r13\n push %r12\n"
                 "push %rbx\n jmp *g_cvc_motion_back(%rip)\n");
}

/// Routes the game's motion update through CvcMotionThunk (once; false if the code there is not
/// the expected prologue).
static bool CvcInstallMotionHook(u64 image) {
    static const u8 prologue[13]{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    auto* code = reinterpret_cast<u8*>(image + 0x636650);
    if (g_cvc_motion_back == image + 0x63665d) {
        return true;
    }
    if (std::memcmp(code, prologue, sizeof(prologue)) != 0) {
        return false;
    }
    g_cvc_motion_back = image + 0x63665d;
    u8 jump[13]{0x48, 0xb8}; // mov rax, CvcMotionThunk; jmp rax; nop
    const u64 thunk = reinterpret_cast<u64>(&CvcMotionThunk);
    std::memcpy(jump + 2, &thunk, 8);
    jump[10] = 0xff, jump[11] = 0xe0, jump[12] = 0x90;
    DWORD old = 0;
    VirtualProtect(code, sizeof(jump), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(code, jump, sizeof(jump));
    VirtualProtect(code, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(jump));
    return true;
}

/// Freezes both colossi's animation (factor `scale`) for `ms` milliseconds: the impact of a blow.
static void CvcHitStop(u64 a, u64 b, int ms, float scale) {
    const s64 until = (std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)).time_since_epoch().count();
    g_cvc_hold_scale[0] = scale, g_cvc_hold_scale[1] = scale;
    g_cvc_hold_obj[0] = a, g_cvc_hold_obj[1] = b;
    g_cvc_hold_until[0] = until, g_cvc_hold_until[1] = until;
}

/// Asks for one of the game's effects (by its file name, e.g. "npe/knight_metal_react.npe";
/// static text) at a point.
static void CvcEffect(const char* name, const float* at, float yaw = 0.0f) {
    for (auto& slot : g_cvc_rays.effects) {
        if (slot.name.load() == nullptr) {
            std::memcpy(slot.at, at, 12);
            slot.at[3] = 1.0f;
            slot.yaw = yaw;
            const char* expected = nullptr;
            if (slot.name.compare_exchange_strong(expected, name)) {
                return;
            }
        }
    }
}

/// The live effect descriptor registered under `name` (the game's effect map, a tree at
/// image+0x30157d0: node +0 left, +0x10 right, +0x19 end marker, +0x20 the descriptor, whose
/// first word is its name, +0x28 its effect record), walked without changing it: the game's own
/// lookup 0x68f580 adds an empty record for a name it lacks, which the game later trips on.
static u64 CvcFindEffect(u64 image, std::string_view name, bool list = false, u64* found_record = nullptr) {
    const u64 head = *reinterpret_cast<const volatile u64*>(image + 0x30157d0);
    auto* memory = Core::Memory::Instance();
    if (!head || !memory->IsValidMapping(head, 0x30)) {
        return 0;
    }
    std::vector<u64> stack{*reinterpret_cast<const volatile u64*>(head + 8)};
    for (int seen = 0; !stack.empty() && seen < 8192; seen++) {
        const u64 node = stack.back();
        stack.pop_back();
        if (!node || node == head || !memory->IsValidMapping(node, 0x30) ||
            *reinterpret_cast<const volatile u8*>(node + 0x19)) {
            continue;
        }
        const u64 descriptor = *reinterpret_cast<const volatile u64*>(node + 0x20);
        const u64 record = *reinterpret_cast<const volatile u64*>(node + 0x28);
        const u64 text = descriptor && memory->IsValidMapping(descriptor, 8)
                             ? *reinterpret_cast<const volatile u64*>(descriptor) : 0;
        if (list && text && memory->IsValidMapping(text, 8)) {
            LOG_INFO(Core, "cvc: effect loaded: {} ({:#x})", reinterpret_cast<const char*>(text), record);
        }
        if (!list && record && text && memory->IsValidMapping(text, name.size() + 1) &&
            std::memcmp(reinterpret_cast<const char*>(text), name.data(), name.size()) == 0 &&
            reinterpret_cast<const char*>(text)[name.size()] == 0) {
            if (found_record) {
                *found_record = record;
            }
            return descriptor;
        }
        stack.push_back(u64(*reinterpret_cast<const volatile u64*>(node)));
        stack.push_back(u64(*reinterpret_cast<const volatile u64*>(node + 0x10)));
    }
    return 0;
}

/// Starts a requested effect through the game's effect bridge (re/cvc/fx/SHAKE_PARTICLES.md):
/// 0x5f5930 builds its transform, 0x690950 starts it; the game ends it itself.
static void CvcEffectWork(u64 image, const char* name, const float* at, float yaw) {
    u64 record = 0;
    const u64 live = CvcFindEffect(image, name, false, &record);
    if (!live) {
        LOG_INFO(Core, "cvc: effect {} not loaded", name);
        return;
    }
    alignas(16) u8 transform[0x40]{};
    alignas(16) const float rotation[4]{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    alignas(16) const float position[4]{at[0], at[1], at[2], 1.0f};
    reinterpret_cast<void(PS4_SYSV_ABI*)(void*, const float*, const float*)>(image + 0x5f5930)(transform, rotation,
                                                                                              position);
    const u64 effect = reinterpret_cast<u64(PS4_SYSV_ABI*)(u64, const void*, u64, u64)>(image + 0x690950)(
        live, transform, 0, 0);
    LOG_INFO(Core, "cvc: effect {} at {:.0f} {:.0f} {:.0f}: {:#x}", name, at[0], at[1], at[2], effect);
    // (Its particle resource, record +0x60: state +0x98 (0xb loaded), resolved +0x20, +0x60, its
    // loader +0x68; and the game-object resource +0x98 the same way.)
    static std::unordered_map<std::string, int> told;
    if (told[name]++ == 0 && record) {
        const auto q = [](u64 a) { return Core::Memory::Instance()->IsValidMapping(a, 8) ? *reinterpret_cast<const volatile u64*>(a) : 0ull; };
        for (const u64 at_res : {0x60ull, 0x98ull}) {
            const u64 res = q(record + at_res);
            LOG_INFO(Core, "cvc: effect {} resource +{:#x} {:#x}: state {:#x} resolved {:#x} +0x60 {:#x} loader {:#x} +0 {:#x} +8 {:#x} +0x10 {:#x}",
                     name, at_res, res, res ? u32(q(res + 0x98)) : 0u, res ? q(res + 0x20) : 0, res ? u32(q(res + 0x60)) : 0u,
                     res ? q(res + 0x68) : 0, res ? q(res) : 0, res ? q(res + 8) : 0, res ? q(res + 0x10) : 0);
        }
    }
}

/// Plays one of the game's sounds by name at a world point (game thread), as its own sound
/// emitters do (re/cvc/fx/SOUND.md): PlayNamedSound 0xa5fb70 paused, the point through the sound
/// space matrix at image+0x160430c (3x4, as 0x6b383a applies it), SetPosition 0xa5fc70, then
/// SetPaused 0xa5fb10 off. The names are the game's own (logged with soundlog): knight_body*,
/// kame_stone_*, ktvo_growl*, ...; an unknown one plays nothing.
static void CvcSoundWork(u64 image, const char* name, const float* at) {
    const u64 system = *reinterpret_cast<const volatile u64*>(image + 0x32f8ff0);
    if (!system) {
        return;
    }
    const u32 handle = reinterpret_cast<u32(PS4_SYSV_ABI*)(u64, const char*, bool)>(image + 0xa5fb70)(system, name, true);
    if (handle == 0xffffffffu) {
        LOG_INFO(Core, "cvc: sound {} not found", name);
        return;
    }
    const auto* m = reinterpret_cast<const float*>(image + 0x160430c);
    alignas(16) float position[4]{}, velocity[4]{};
    for (int c = 0; c < 3; c++) {
        position[c] = at[0] * m[c] + at[1] * m[3 + c] + at[2] * m[6 + c] + m[9 + c];
    }
    reinterpret_cast<void(PS4_SYSV_ABI*)(u64, u32, const float*, const float*)>(image + 0xa5fc70)(system, handle, position,
                                                                                                velocity);
    reinterpret_cast<void(PS4_SYSV_ABI*)(u64, u32, bool)>(image + 0xa5fb10)(system, handle, false);
    LOG_INFO(Core, "cvc: sound {} at {:.0f} {:.0f} {:.0f}: {:#x}", name, at[0], at[1], at[2], handle);
}

static void CvcSound(const char* name, const float* at) {
    for (auto& slot : g_cvc_rays.sounds) {
        if (slot.name.load() == nullptr) {
            std::memcpy(slot.at, at, 12);
            const char* expected = nullptr;
            if (slot.name.compare_exchange_strong(expected, name)) {
                return;
            }
        }
    }
}

/// The effects of a blow landing at `point` on `victim` (its strength 0.5-1.6), the game's own:
/// at the contact, Gaius's armour struck (metal and chips) or Basaran's stone shell (chips), and
/// smoke; dust thrown up from the ground below.
static void CvcImpact(u64 image, u64 victim, bool armoured, const float* point, const float* ground,
                      float strength, bool heavy) {
    LOG_INFO(Core, "cvcbrawl: impact at {:.0f} {:.0f} {:.0f}, strength {:.2f}{}", point[0], point[1], point[2],
             strength, heavy ? ", heavy" : "");
    if (armoured) {
        CvcEffect("npe/knight_metal_react.npe", point);
        CvcEffect("npe/arrow_hit_spark.npe", point);
        if (heavy) {
            CvcEffect("npe/knight_metal_chip.npe", point);
        }
    } else {
        CvcEffect("npe/knight_add_chip.npe", point);
        CvcEffect("npe/arrow_hit_smoke.npe", point);
        if (heavy) {
            CvcEffect("npe/knight_shockwave_add.npe", point);
        }
    }
    if (heavy) {
        CvcEffect("npe/player_stone_dust.npe", ground);
    }
    // The crash of it (on Gaius's armour or Basaran's stone), stones falling, and the one hit
    // crying out (their own damage voices).
    static std::mt19937 rng{std::random_device{}()};
    static const char* const armour[]{"knight_body1", "knight_body3", "knight_body5", "knight_body8"};
    static const char* const stone[]{"kame_stone_2", "kame_stone_4", "kame_stone_6"};
    static const char* const pebbles[]{"kame_stone_s1", "kame_stone_s3", "kame_stone_s5"};
    static const char* const growl_knight[]{"knight_damage_voice_2", "knight_damage_voice_5", "knight_damage_voice_2"};
    static const char* const growl_kame[]{"kmvo_dam1", "kmvo_dam3", "kmvo_dam5"};
    CvcSound(armoured ? armour[rng() % 4] : stone[rng() % 3], point);
    CvcSound(pebbles[rng() % 3], ground);
    CvcSound(armoured ? growl_knight[rng() % 3] : growl_kame[rng() % 3], point);
}

/// The two colossi as physical bodies (Sotc::CvcPhysics): their own collision hulls riding their
/// animated bones; where they meet they are pushed out of each other, exchange momentum by their
/// masses (their roots pushed, their bodies leaned by the angular impulse), and a meeting that
/// strikes hard enough is a blow for cvcbrawl, there and then.
static void CvcPhysicsWork(u64 image, float dt) {
    const auto copy = [](u64 at, void* to, size_t size) {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), to, size, &got) && got == size;
    };
    const u64 table = *reinterpret_cast<const volatile u64*>(image + 0x16d21f8 + 0x20);
    int kinds[2]{-1, -1};
    static float bones[2][Sotc::CvcPhysics::MaxBones * 16];
    int counts[2]{};
    float roots[2][3]{};
    u64 objects[2]{};
    for (int k = 0; k < 2; k++) {
        objects[k] = g_cvc_body[k].object.load();
        const u64 entry = *reinterpret_cast<const volatile u64*>(image + (k ? 0x1593b30 : 0x1593b28));
        u64 matrices = 0;
        if (!objects[k] || *reinterpret_cast<const volatile u8*>(objects[k] + 0xc) != 9 || entry < table) {
            return;
        }
        kinds[k] = int((entry - table) / 0x1e8);
        const auto* parents = Sotc::CvcPhysics::Parents(kinds[k]);
        counts[k] = parents ? int(parents->size()) : 0;
        if (!counts[k] || !copy(objects[k] + 0x120 + 0x198, &matrices, 8) ||
            !copy(matrices, bones[k], size_t(counts[k]) * 64) || !copy(objects[k] + 0x130, roots[k], 12)) {
            static s64 told = 0;
            if (const s64 now = SDL_GetTicks(); now > told + 5000) {
                told = now;
                LOG_INFO(Core, "cvcphys: {} (kind {}) not read: {} bones, matrices {:#x}", k ? "B" : "A", kinds[k],
                         counts[k], matrices);
            }
            return;
        }
    }
    // (As they are drawn: limbs bent where they gave, so a part bent out of the other is out of it.)
    for (int k = 0; k < 2; k++) {
        CvcAngles angle;
        {
            std::scoped_lock guard{g_cvc_flex[k].lock};
            angle = g_cvc_flex[k].angle;
            g_cvc_flex[k].kind = kinds[k];
        }
        CvcFlexBend(kinds[k], angle, bones[k], counts[k]);
    }
    const float* bone_sets[2]{bones[0], bones[1]};
    const float* root_sets[2]{roots[0], roots[1]};
    Sotc::CvcPhysics::Result result;
    if (!Sotc::CvcPhysics::Step(kinds, bone_sets, root_sets, dt, result)) {
        return;
    }
    // Only a blow shoves a body (a new meeting with weight behind it, its flinch playing): bodies
    // resting against each other are only kept out of each other, gently.
    // (One swing is one shove: a blade sweeping through several of the other's hull pieces met each
    // anew; a shove for each slid the struck one along.)
    bool blow = false;
    static auto last_shove = std::chrono::steady_clock::time_point{};
    for (const auto& hit : result.hits) {
        blow = blow || (hit.first && hit.impulse >= 8000.0f && hit.closing >= 40.0f);
    }
    if (blow && std::chrono::steady_clock::now() < last_shove + std::chrono::milliseconds(500)) {
        blow = false;
    } else if (blow) {
        last_shove = std::chrono::steady_clock::now();
    }
    for (int k = 0; k < 2; k++) {
        if (result.shift[k][0] != 0.0f || result.shift[k][2] != 0.0f) {
            MoveCharacter(objects[k], result.shift[k].data());
            g_cvc_body[k].ours += std::hypot(result.shift[k][0], result.shift[k][2]);
            g_cvc_body[k].ours_xz[0] += result.shift[k][0], g_cvc_body[k].ours_xz[1] += result.shift[k][2];
            g_cvc_body[k].ours_q[0] += result.shift[k][0], g_cvc_body[k].ours_q[1] += result.shift[k][2];
        }
        if (blow) {
            std::scoped_lock guard{g_cvc_body[k].lock};
            g_cvc_body[k].push[0] += result.kick[k][0], g_cvc_body[k].push[1] += result.kick[k][2];
        }
    }
    for (float was = g_cvc_gap.load(); result.gap < was && !g_cvc_gap.compare_exchange_weak(was, result.gap);) {
    }
    g_cvc_gap_now = result.gap;
    g_cvc_touch_now = !result.hits.empty();
    {
        float depth = 0.0f;
        for (const auto& hit : result.hits) {
            depth = std::max(depth, hit.depth);
        }
        g_cvc_depth_now = depth;
        g_cvc_contacts_now = int(result.hits.size());
    }
    // (Clipping in the log: the deepest the bodies were inside each other over each 2 s.)
    {
        static float deepest = 0.0f;
        static int deep_bone[2]{};
        static auto since = std::chrono::steady_clock::now();
        for (const auto& hit : result.hits) {
            if (hit.depth > deepest) {
                deepest = hit.depth, deep_bone[hit.striker] = hit.bone, deep_bone[1 - hit.striker] = hit.struck_bone;
            }
        }
        if (std::chrono::steady_clock::now() > since + std::chrono::seconds(2)) {
            if (deepest > 0.0f) {
                LOG_INFO(Core, "cvcphys: deepest {:.1f} inside each other (A bone {}, B bone {})", deepest, deep_bone[0], deep_bone[1]);
            }
            deepest = 0.0f, since = std::chrono::steady_clock::now();
        }
    }
    const Sotc::CvcPhysics::Hit* strongest = nullptr;
    for (const auto& hit : result.hits) {
        if (!strongest || hit.impulse > strongest->impulse) {
            strongest = &hit;
        }
        if (hit.first) {
            LOG_INFO(Core, "cvcphys: {} bone {} met {} at {:.0f} {:.0f} {:.0f}, closing {:.0f}, impulse {:.0f}, depth {:.2f}",
                     hit.striker ? "B" : "A", hit.bone, hit.striker ? "A" : "B", hit.point[0], hit.point[1], hit.point[2],
                     hit.closing, hit.impulse, hit.depth);
        }
    }
    if (strongest && strongest->impulse > 0.0f) {
        // Each body gives: the struck leans away and yields where it was met, the striker a little
        // back through its arm (the angular impulse; the yield as fast as its root was kicked).
        for (int k = 0; k < 2; k++) {
            const float sign = k == strongest->striker ? -1.0f : 1.0f;
            const float dir[2]{strongest->normal[0] * sign, strongest->normal[2] * sign};
            const float kicked = std::hypot(result.kick[k][0], result.kick[k][2]);
            CvcReactSpin(k, result.spin[k].data(), strongest->point.data(), dir, std::min(kicked * 6.0f, 40.0f),
                         std::min(kicked * 1.5f, 20.0f));
        }
    }
    // Parts inside each other bend out at their joints (half the depth a step, shared by inverse
    // mass), so a head slammed into the other comes off it rather than both bodies sliding.
    for (const auto& hit : result.hits) {
        if (hit.depth < 0.5f) {
            continue;
        }
        const int struck = 1 - hit.striker;
        const float ma = Sotc::CvcPhysics::Mass(kinds[hit.striker]), mb = Sotc::CvcPhysics::Mass(kinds[struck]);
        const float share = 1.0f / ma + 1.0f / mb, out = std::min(hit.depth * 0.5f, 1.0f);
        const float back[3]{-hit.normal[0], -hit.normal[1], -hit.normal[2]};
        CvcFlexShift(hit.striker, kinds[hit.striker], bones[hit.striker], hit.bone, hit.point.data(), back,
                     out * (1.0f / ma) / share);
        CvcFlexShift(struck, kinds[struck], bones[struck], hit.struck_bone, hit.point.data(), hit.normal.data(),
                     out * (1.0f / mb) / share);
    }
    // Each new meeting with some weight bends the struck one's limb there (the region of it given
    // the impulse over about an eighth of its mass).
    for (const auto& hit : result.hits) {
        if (hit.first && hit.impulse > 4000.0f) {
            const int struck = 1 - hit.striker;
            CvcFlexImpulse(struck, kinds[struck], bones[struck], hit.struck_bone, hit.point.data(), hit.normal.data(),
                           hit.impulse / (Sotc::CvcPhysics::Mass(kinds[struck]) * 0.12f));
        }
    }
    // A blow for cvcbrawl: the strongest new meeting with some weight behind it.
    // (Bodies settling or resting against each other are not blows: a blow comes in at 40 units/s
    // or more, with some weight.)
    constexpr float least = 8000.0f, coming = 40.0f;
    // Where the blow lands on the struck one's own surface: the game's line test along the blow
    // through the hulls' meeting point, against its triangle meshes (the hulls only approximate them).
    const auto on_surface = [&](const Sotc::CvcPhysics::Hit& hit, float* at) {
        const u64 filter = *reinterpret_cast<const volatile u64*>(image + 0x1593ed8);
        if (!filter) {
            return false;
        }
        using LineTest = u8 PS4_SYSV_ABI (*)(void* hit, const float* from, const float* to, u64 filter, u32 single,
                                             u64 ignore);
        const auto test = reinterpret_cast<LineTest>(image + 0x7a7e40);
        alignas(16) const float from[4]{hit.point[0] - hit.normal[0] * 8.0f, hit.point[1] - hit.normal[1] * 8.0f,
                                        hit.point[2] - hit.normal[2] * 8.0f, 1.0f};
        alignas(16) const float to[4]{hit.point[0] + hit.normal[0] * 8.0f, hit.point[1] + hit.normal[1] * 8.0f,
                                      hit.point[2] + hit.normal[2] * 8.0f, 1.0f};
        alignas(16) float found[12]{};
        if (test(found, from, to, filter, 1, objects[hit.striker]) ||
            *reinterpret_cast<const u64*>(found + 8) != objects[1 - hit.striker]) {
            return false;
        }
        std::memcpy(at, found, 12);
        return true;
    };
    for (const auto& hit : result.hits) {
        const float need = std::min(coming, g_cvc_coming[hit.striker].load());
        if (!hit.first || hit.impulse < least * need / coming || hit.closing < need) {
            continue;
        }
        float point[3]{hit.point[0], hit.point[1], hit.point[2]};
        const bool surface = on_surface(hit, point);
        static int surfaced = 0, blows = 0;
        surfaced += surface, blows++;
        if (blows % 5 == 0) {
            LOG_INFO(Core, "cvcphys: {} of {} blows put on the struck one's own surface", surfaced, blows);
        }
        std::scoped_lock guard{g_cvc_blow.lock};
        if (!g_cvc_blow.ready || hit.impulse > g_cvc_blow.impulse) {
            g_cvc_blow.ready = true, g_cvc_blow.striker = hit.striker, g_cvc_blow.bone = hit.bone;
            std::memcpy(g_cvc_blow.point, point, 12), std::memcpy(g_cvc_blow.normal, hit.normal.data(), 12);
            g_cvc_blow.impulse = hit.impulse, g_cvc_blow.closing = hit.closing;
        }
    }
}

/// Research probe (CVC_PROBE_SKEL set): every 3rd frame, up to 900 samples, appends colossus k's
/// bone matrices (its count, then count x 64 bytes) to testrun/pose_<A|B>.bin; the skeleton's
/// parents are then found offline (re/cvc/skeleton.py: a bone keeps a fixed offset in its parent).
static void CvcProbeSkeleton(u64 image, int k, u64 object) {
    static int frame[2]{}, samples[2]{};
    if (samples[k] >= 900 || frame[k]++ % 3) {
        return;
    }
    const auto copy = [](u64 at, void* to, size_t size) {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), to, size, &got) && got == size;
    };
    u64 matrices = 0;
    float root[4]{};
    if (!copy(object + 0x120 + 0x198, &matrices, 8) || !copy(object + 0x130, root, 16)) {
        return;
    }
    std::vector<float> pose;
    for (float m[16]; pose.size() < 256 * 16 && copy(matrices + pose.size() * 4, m, 64) && m[15] == 1.0f &&
                      std::hypot(m[12] - root[0], m[14] - root[2]) < 400.0f;) {
        pose.insert(pose.end(), m, m + 16);
    }
    std::ofstream out(fmt::format("D:/sotc-pc/testrun/pose_{}.bin", k ? "B" : "A"),
                      std::ios::binary | (samples[k] ? std::ios::app : std::ios::trunc));
    const u32 count = u32(pose.size() / 16);
    out.write(reinterpret_cast<const char*>(&count), 4);
    out.write(reinterpret_cast<const char*>(pose.data()), std::streamsize(pose.size() * 4));
    // Its collision as the game's line test sees it: 48 rays in from random directions at random
    // bones; each hit on it (sample, point, the hit's word +0x28) to testrun/rays_<A|B>.bin.
    const u64 filter = *reinterpret_cast<const volatile u64*>(image + 0x1593ed8);
    const u64 other = g_cvc_body[1 - k].object.load();
    if (filter && count >= 20) {
        using LineTest = u8 PS4_SYSV_ABI (*)(void* hit, const float* from, const float* to, u64 filter, u32 single,
                                             u64 ignore);
        const auto test = reinterpret_cast<LineTest>(image + 0x7a7e40);
        static std::mt19937 rng{11};
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::ofstream rays(fmt::format("D:/sotc-pc/testrun/rays_{}.bin", k ? "B" : "A"),
                           std::ios::binary | (samples[k] ? std::ios::app : std::ios::trunc));
        for (int r = 0; r < 48; r++) {
            const float* b = &pose[(rng() % (count / 2)) * 16 + 12]; // (its bones are listed twice)
            float d[3];
            do {
                d[0] = unit(rng), d[1] = unit(rng), d[2] = unit(rng);
            } while (std::hypot(d[0], d[1], d[2]) > 1.0f || std::hypot(d[0], d[1], d[2]) < 0.1f);
            const float n = std::hypot(d[0], d[1], d[2]);
            alignas(16) const float from[4]{b[0] + d[0] / n * 30.0f, b[1] + d[1] / n * 30.0f, b[2] + d[2] / n * 30.0f, 1.0f};
            alignas(16) const float to[4]{b[0] - d[0] / n * 2.0f, b[1] - d[1] / n * 2.0f, b[2] - d[2] / n * 2.0f, 1.0f};
            alignas(16) float hit[12]{};
            if (test(hit, from, to, filter, 1, other) || *reinterpret_cast<const u64*>(hit + 8) != object) {
                continue; // clear, or met something else first
            }
            const u32 sample = u32(samples[k]);
            rays.write(reinterpret_cast<const char*>(&sample), 4);
            rays.write(reinterpret_cast<const char*>(hit), 12);
            rays.write(reinterpret_cast<const char*>(hit + 10), 8);
        }
    }
    samples[k]++;
}

/// Research trace (CVC_TRACE set): each frame, each colossus's root, what our code moved it by since
/// the frame before, its motion and frame, and whether the bodies touch, to testrun/root_trace.bin
/// (re/cvc/trace.py finds the root moves neither its motion nor our code explains: snaps).
static void CvcTrace(int k, u64 object) {
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        out.open("D:/sotc-pc/testrun/root_trace.bin", std::ios::binary | std::ios::trunc);
        opened = true;
    }
    auto& body = g_cvc_body[k];
    struct Record {
        float t;
        u32 k;
        float pos[3], ours[2], frame;
        u64 motion;
        u32 touching, act;
    } r{};
    r.t = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
    r.k = u32(k);
    const auto* p = reinterpret_cast<const volatile float*>(object + 0x130);
    r.pos[0] = p[0], r.pos[1] = p[1], r.pos[2] = p[2];
    r.ours[0] = body.ours_xz[0], r.ours[1] = body.ours_xz[1];
    body.ours_xz[0] = body.ours_xz[1] = 0.0f;
    r.frame = body.frame.load();
    r.motion = body.motion.load();
    r.touching = g_cvc_touch_now.load() ? 1 : 0;
    out.write(reinterpret_cast<const char*>(&r), sizeof(r));
}

/// A motion record (absolute) whose animation data is in: 0x6049e0 (its last frame, the game's own
/// motion start too) reads [[[motion+0x128]]+0x28] and faults on a null there (Valus's, Buffalo_walk
/// on Quadratus).
static bool CvcMotionLoaded(u64 motion) {
    const auto read = [](u64 at) {
        return at && Core::Memory::Instance()->IsValidMapping(at, 8) ? *reinterpret_cast<const volatile u64*>(at) : u64(0);
    };
    const u64 table = motion ? read(read(motion + 0x128)) : 0;
    return table && read(table + 0x28);
}

static void CvcBodyWork(u64 image) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point before = Clock::now();
    const float took = std::chrono::duration<float>(Clock::now() - before).count();
    const float dt = std::clamp(took, 0.0f, 0.05f);
    before = Clock::now();
    if (took > 0.07f && took < 5.0f && g_cvc_body[0].object.load()) {
        LOG_INFO(Core, "cvcbody: a long frame, {:.0f} ms", took * 1000.0f); // (a stall, not a snap of the pose)
    }
    const s64 now = Clock::now().time_since_epoch().count();
    for (int k = 0; k < 2; k++) {
        if (g_cvc_hold_obj[k] && now > g_cvc_hold_until[k].load()) {
            g_cvc_hold_obj[k] = 0, g_cvc_hold_scale[k] = 1.0f;
        }
    }
    for (int k = 0; k < 2; k++) {
        CvcBody& body = g_cvc_body[k];
        const u64 object = body.object.load();
        if (!object || *reinterpret_cast<const volatile u8*>(object + 0xc) != 9) {
            continue;
        }
        // What it plays: component +0x260 the motion, +0x274 its frame; 0x6049e0 its last frame.
        const u64 component = object + 0x120;
        const u64 motion = *reinterpret_cast<const volatile u64*>(component + 0x260);
        body.motion = motion ? motion - image : 0;
        body.frame = *reinterpret_cast<const volatile float*>(component + 0x274);
        body.frames = CvcMotionLoaded(motion)
                          ? float(reinterpret_cast<s32 PS4_SYSV_ABI (*)(u64)>(image + 0x6049e0)(motion + 0x128))
                          : 0.0f;
        if (dt <= 0.0f) {
            continue;
        }
        float delta[3]{};
        {
            std::scoped_lock guard{body.lock};
            // A blow's push dies away over about a third of a second (friction on the ground).
            const float keep = std::exp(-dt / 0.3f);
            for (int c = 0; c < 2; c++) {
                delta[c * 2] = (body.push[c] + body.apart[c] + body.step[c]) * dt;
                body.push[c] *= keep;
                if (std::abs(body.push[c]) < 0.05f) {
                    body.push[c] = 0.0f;
                }
            }
        }
        if (delta[0] != 0.0f || delta[2] != 0.0f) {
            MoveCharacter(object, delta);
            body.ours += std::hypot(delta[0], delta[2]);
            body.ours_xz[0] += delta[0], body.ours_xz[1] += delta[2];
            body.ours_q[0] += delta[0], body.ours_q[1] += delta[2];
        }
        {
            static Clock::time_point since[2]{};
            if (Clock::now() > since[k] + std::chrono::seconds(2)) {
                if (body.ours > 0.5f) {
                    LOG_INFO(Core, "cvcbody: {} moved by us {:.1f} in 2 s (motion {:#x})", k ? "B" : "A", body.ours,
                             body.motion.load());
                }
                body.ours = 0.0f, since[k] = Clock::now();
            }
        }
        // Turning eased toward the rate asked for (up to 2 rad/s² either way): no sudden swing.
        const float want = body.turn.load();
        body.turning += std::clamp(want - body.turning, -2.0f * dt, 2.0f * dt);
        if (std::abs(body.turning) > 0.001f) {
            RotateCharacter(object, body.turning * dt);
        }
        if (std::getenv("CVC_PROBE_SKEL")) {
            CvcProbeSkeleton(image, k, object);
        }
        if (std::getenv("CVC_TRACE")) {
            CvcTrace(k, object);
        }
        // Research probe (CVC_PROBE_LEASH): Basaran shoved along x by 5, 10, 20 and 40 units, 10 s
        // apart, its root logged every 6 frames: how far it may go and how the game brings it back.
        if (k == 1 && std::getenv("CVC_PROBE_LEASH")) {
            static int frames = 0;
            static float home[3]{};
            const auto* p = reinterpret_cast<const volatile float*>(object + 0x130);
            if (frames == 300) {
                home[0] = p[0], home[1] = p[1], home[2] = p[2];
            }
            for (const auto& [at, by] : {std::pair{600, 5.0f}, std::pair{1200, 10.0f}, std::pair{1800, 20.0f},
                                         std::pair{2400, 40.0f}}) {
                if (frames == at) {
                    const float shove[3]{by, 0.0f, 0.0f};
                    MoveCharacter(object, shove);
                    LOG_INFO(Core, "cvcleash: shoved {:.0f}", by);
                }
            }
            if (frames > 300 && frames % 6 == 0 && frames < 3000) {
                LOG_INFO(Core, "cvcleash: frame {} off home {:.1f} {:.1f} {:.1f}", frames, p[0] - home[0], p[1] - home[1],
                         p[2] - home[2]);
            }
            frames++;
        }
        // A pop in the log: the root jumped more than a walk does in a frame.
        const auto* at = reinterpret_cast<const volatile float*>(object + 0x130);
        const float moved = std::hypot(at[0] - body.last[0], at[2] - body.last[2]);
        if (body.last_motion && moved > 2.0f && moved < 400.0f) {
            LOG_INFO(Core, "cvcbody: {} root jumped {:.1f} in a frame (motion {:#x}, was {:#x}, ours {:.1f})",
                     k ? "B" : "A", moved, body.motion.load(), body.last_motion,
                     std::hypot(delta[0], delta[2]));
        }
        if (body.last_motion > 1 && body.motion.load() != body.last_motion) {
            LOG_INFO(Core, "cvcbody: {} motion {:#x} -> {:#x}", k ? "B" : "A", body.last_motion, body.motion.load());
        }
        // A snap of the pose: a bone of the skeleton (relative to the root) moving more in one frame
        // than any motion moves it.
        {
            const u64 matrices = *reinterpret_cast<const volatile u64*>(component + 0x198);
            std::vector<std::array<float, 3>> pose;
            for (int i = 0; matrices && i < 128 && Core::Memory::Instance()->IsValidMapping(matrices + i * 64, 64); i++) {
                const auto* m = reinterpret_cast<const volatile float*>(matrices + i * 64 + 0x30);
                if (m[3] != 1.0f || std::hypot(m[0] - at[0], m[2] - at[2]) > 400.0f) {
                    break;
                }
                pose.push_back({m[0] - at[0], m[1] - at[1], m[2] - at[2]});
            }
            if (pose.size() == body.pose.size()) {
                float jump = 0.0f;
                int worst = -1;
                for (size_t i = 0; i < pose.size(); i++) {
                    const float d = std::hypot(pose[i][0] - body.pose[i][0], pose[i][1] - body.pose[i][1],
                                               pose[i][2] - body.pose[i][2]);
                    if (d > jump) {
                        jump = d, worst = int(i);
                    }
                }
                if (jump > 8.0f) {
                    LOG_INFO(Core, "cvcbody: {} pose snapped {:.1f} (bone {}) in a frame, motion {:#x} frame {:.1f}{}",
                             k ? "B" : "A", jump, worst, body.motion.load(), body.frame.load(),
                             body.motion.load() != body.last_motion ? " (motion just changed)" : "");
                }
            }
            body.pose.swap(pose);
        }
        body.last[0] = at[0], body.last[1] = at[1], body.last[2] = at[2];
        body.last_motion = body.motion.load() ? body.motion.load() : 1;
    }
    CvcInertiaPass(dt);
    CvcPhysicsWork(image, dt);
    CvcReactWork(image, dt);
}

/// Asks for a motion to be started on a colossus at the next camera update (game thread).
/// `blend`: seconds it crossfades from the pose it had (the game's own blend, component +0x980..;
/// 0: it snaps to the new motion's first frame).
// Guards the motion requests: one waiting per colossus, the newest (two started in the same
// frame, the second blending over the first's first frame, crashed the game's object update at
// 0x61af2f).
static std::mutex g_cvc_motion_lock;

static void CvcPlayMotion(u64 image, u64 object, u64 motion, float blend = 0.0f) {
    if (!CvcMotionLoaded(image + motion)) {
        static std::set<u64> told;
        if (told.insert(motion).second) {
            LOG_INFO(Core, "cvcmotion: {:#x} not loaded, not played", motion);
        }
        return;
    }
    std::scoped_lock guard{g_cvc_motion_lock};
    g_cvc_rays.image = image;
    for (auto& slot : g_cvc_rays.motions) {
        if (slot.object.load() == object) {
            slot.motion = image + motion;
            slot.blend = blend;
            return;
        }
    }
    for (auto& slot : g_cvc_rays.motions) {
        u64 expected = 0;
        if (slot.object.load() == 0) {
            slot.motion = image + motion;
            slot.blend = blend;
            if (slot.object.compare_exchange_strong(expected, object)) {
                return;
            }
        }
    }
}

/// Basaran's own pulse, as its motion callbacks do it (re/cvc/fx/PULSE.md): the charge
/// (Reservoir_turtle_pulse, attached to it through 0x6da930, started 0x6da9f0 mode 3; 0x789600) and
/// the shot (its firing point 0x6da400, a quaternion along the shot 0x582940, the game's projectile
/// 0x703880 kind 0; 0x789280), which flies, meets what it meets and starts its own hit effect.
static void CvcPulseWork(CvcRays& r, int what) {
    const u64 image = r.image, basaran = r.pulse_object;
    const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
    if (what == 1) {
        const u64 charge = reinterpret_cast<u64(PS4_SYSV_ABI*)(u64, u64, u64)>(image + 0x6da930)(
            q(image + 0x1f553c0), basaran, q(image + 0x24bfb58));
        if (charge) {
            reinterpret_cast<void(PS4_SYSV_ABI*)(u64, s32, s32, u64)>(image + 0x6da9f0)(charge, -1, 3, 0);
        }
        r.pulse_charge = charge;
        LOG_INFO(Core, "cvcpulse: charge {:#x}", charge);
        return;
    }
    alignas(16) float start[4]{}, forward[4]{}, orientation[4]{};
    reinterpret_cast<void(PS4_SYSV_ABI*)(float*, u64, u64)>(image + 0x6da400)(start, basaran, q(image + 0x24bfca8));
    for (int c = 0; c < 3; c++) {
        forward[c] = r.pulse_target[c] - start[c];
    }
    const float n = std::max(std::hypot(forward[0], forward[1], forward[2]), 0.001f);
    for (int c = 0; c < 3; c++) {
        forward[c] /= n;
    }
    reinterpret_cast<void(PS4_SYSV_ABI*)(float*, const float*, const float*)>(image + 0x582940)(orientation, forward, forward);
    if (r.pulse_charge) {
        reinterpret_cast<void(PS4_SYSV_ABI*)(u64)>(image + 0x577960)(r.pulse_charge); // the charge ends as it fires
        r.pulse_charge = 0;
    }
    reinterpret_cast<void(PS4_SYSV_ABI*)(u64, const float*, const float*, s32)>(image + 0x703880)(basaran, start, orientation, 0);
    LOG_INFO(Core, "cvcpulse: fired from {:.0f} {:.0f} {:.0f} at {:.0f} {:.0f} {:.0f}", start[0], start[1], start[2],
             r.pulse_target[0], r.pulse_target[1], r.pulse_target[2]);
}

/// Starts a requested motion on a colossus (game thread).
static void CvcStartMotion(CvcRays& r, CvcRays::MotionRequest& request, u64 object) {
        const u64 component = object + 0x120;
        const u64 motion = request.motion;
        const u64 workspace = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(r.image + 0x681ce0)(object);
        if (workspace) {
            *reinterpret_cast<volatile u8*>(workspace + 0x1648) = 1;
        }
        // As the game's motion start 0x6179f0 does once it has resolved the motion (its callers
        // first mark the AI workspace's +0x1648): root motion 0x634600 / 0x634560, then 0x635740.
        // (Its mode-0 path instead, 0x63c2d0 and 0x62ea70 with the motion put in, crashed later
        // in the object update, 0x61af2f.)
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, u64, s32, float)>(r.image + 0x634600)(
            component + 0x10bc, object, motion, -1, 0.0f);
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, u64)>(r.image + 0x634560)(component, object, motion);
        // Its float: over how many of the new motion's frames (30 a second) it fades in over the
        // pose it had (0x635210 sets the blend +0x980, 0x63b7b0 runs it while +0x6ff is set).
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, float)>(r.image + 0x635740)(component, motion,
                                                                                 request.blend * 30.0f);
        // As 0x6179f0 then does (0x617b73): +0x52c cleared. Left set, the object update reads a
        // motion at +0x268 that can be null (0x64bf50, crashes seen after Basaran's idle or walk).
        *reinterpret_cast<volatile u8*>(component + 0x52c) = 0;
        LOG_INFO(Core, "cvc: motion {:#x} on {:#x} (now {:#x}), blend {:.2f} s over {:.1f} frames (+0x6ff {})",
                 motion - r.image, object, *reinterpret_cast<const volatile u64*>(component + 0x260) - r.image,
                 request.blend, *reinterpret_cast<const volatile float*>(component + 0x980),
                 *reinterpret_cast<const volatile u8*>(component + 0x6ff));
        request.object.store(0);
}

/// A colossus kept out of its AI, at the moment its AI would have run (contact_patch.py HELD,
/// through CvcAiThunk): motions asked for it start here, before the frame's animation update
/// (started at the camera update, after it, the next picture showed a broken pose).
extern "C" PS4_SYSV_ABI void CvcAiWork(u64 object) {
    auto& r = g_cvc_rays;
    std::scoped_lock guard{g_cvc_motion_lock};
    for (auto& request : r.motions) {
        if (request.object.load() == object && r.image && request.motion) {
            CvcStartMotion(r, request, object);
        }
    }
}

extern "C" __attribute__((naked)) void CvcAiThunk() {
    asm volatile("pushfq\n push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n"
                 "push %r8\n push %r9\n push %r10\n push %r11\n push %rbp\n"
                 "mov %rsp, %rbp\n and $-32, %rsp\n sub $512, %rsp\n"
                 "vmovups %ymm0, 0(%rsp)\n vmovups %ymm1, 32(%rsp)\n vmovups %ymm2, 64(%rsp)\n"
                 "vmovups %ymm3, 96(%rsp)\n vmovups %ymm4, 128(%rsp)\n vmovups %ymm5, 160(%rsp)\n"
                 "vmovups %ymm6, 192(%rsp)\n vmovups %ymm7, 224(%rsp)\n vmovups %ymm8, 256(%rsp)\n"
                 "vmovups %ymm9, 288(%rsp)\n vmovups %ymm10, 320(%rsp)\n vmovups %ymm11, 352(%rsp)\n"
                 "vmovups %ymm12, 384(%rsp)\n vmovups %ymm13, 416(%rsp)\n vmovups %ymm14, 448(%rsp)\n"
                 "vmovups %ymm15, 480(%rsp)\n"
                 "call CvcAiWork\n"
                 "vmovups 0(%rsp), %ymm0\n vmovups 32(%rsp), %ymm1\n vmovups 64(%rsp), %ymm2\n"
                 "vmovups 96(%rsp), %ymm3\n vmovups 128(%rsp), %ymm4\n vmovups 160(%rsp), %ymm5\n"
                 "vmovups 192(%rsp), %ymm6\n vmovups 224(%rsp), %ymm7\n vmovups 256(%rsp), %ymm8\n"
                 "vmovups 288(%rsp), %ymm9\n vmovups 320(%rsp), %ymm10\n vmovups 352(%rsp), %ymm11\n"
                 "vmovups 384(%rsp), %ymm12\n vmovups 416(%rsp), %ymm13\n vmovups 448(%rsp), %ymm14\n"
                 "vmovups 480(%rsp), %ymm15\n"
                 "mov %rbp, %rsp\n pop %rbp\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
                 "pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n popfq\n ret\n");
}

// Test aid (soundlog): the names the game asks its sound system for (PlayNamedSound 0xa5fb70,
// rsi the name), logged once each, to find a colossus's own impact and voice sounds by name.
extern "C" {
u64 g_cvc_sound_back = 0; // image + 0xa5fb7d: past the replaced prologue
}
extern "C" PS4_SYSV_ABI void CvcSoundName(u64, const char* name) {
    static std::mutex lock;
    static std::unordered_map<std::string, int> seen;
    if (!name || !Core::Memory::Instance()->IsValidMapping(reinterpret_cast<u64>(name), 1)) {
        return;
    }
    std::scoped_lock guard{lock};
    if (seen.size() < 4000 && seen[name]++ == 0) {
        LOG_INFO(Core, "soundlog: {}", name);
    }
}

extern "C" __attribute__((naked)) void CvcSoundNameThunk() {
    asm volatile("pushfq\n push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n"
                 "push %r8\n push %r9\n push %r10\n push %r11\n push %rbp\n"
                 "mov %rsp, %rbp\n and $-32, %rsp\n sub $512, %rsp\n"
                 "vmovups %ymm0, 0(%rsp)\n vmovups %ymm1, 32(%rsp)\n vmovups %ymm2, 64(%rsp)\n"
                 "vmovups %ymm3, 96(%rsp)\n vmovups %ymm4, 128(%rsp)\n vmovups %ymm5, 160(%rsp)\n"
                 "vmovups %ymm6, 192(%rsp)\n vmovups %ymm7, 224(%rsp)\n vmovups %ymm8, 256(%rsp)\n"
                 "vmovups %ymm9, 288(%rsp)\n vmovups %ymm10, 320(%rsp)\n vmovups %ymm11, 352(%rsp)\n"
                 "vmovups %ymm12, 384(%rsp)\n vmovups %ymm13, 416(%rsp)\n vmovups %ymm14, 448(%rsp)\n"
                 "vmovups %ymm15, 480(%rsp)\n"
                 "call CvcSoundName\n"
                 "vmovups 0(%rsp), %ymm0\n vmovups 32(%rsp), %ymm1\n vmovups 64(%rsp), %ymm2\n"
                 "vmovups 96(%rsp), %ymm3\n vmovups 128(%rsp), %ymm4\n vmovups 160(%rsp), %ymm5\n"
                 "vmovups 192(%rsp), %ymm6\n vmovups 224(%rsp), %ymm7\n vmovups 256(%rsp), %ymm8\n"
                 "vmovups 288(%rsp), %ymm9\n vmovups 320(%rsp), %ymm10\n vmovups 352(%rsp), %ymm11\n"
                 "vmovups 384(%rsp), %ymm12\n vmovups 416(%rsp), %ymm13\n vmovups 448(%rsp), %ymm14\n"
                 "vmovups 480(%rsp), %ymm15\n"
                 "mov %rbp, %rsp\n pop %rbp\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
                 "pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n popfq\n push %rbp\n mov %rsp, %rbp\n push %r15\n push %r14\n push %rbx\n push %rax\n mov %rsi, %rbx\n jmp *g_cvc_sound_back(%rip)\n");
}

extern "C" PS4_SYSV_ABI void CvcRayWork() {
    auto& r = g_cvc_rays;
    if (r.image) {
        CvcBodyWork(r.image);
    }
    // Motions on a colossus whose AI is held start at its AI's turn (CvcAiWork); any other, or
    // one still waiting after a few frames (its AI not reached), here.
    std::unique_lock motion_guard{g_cvc_motion_lock};
    for (auto& request : r.motions) {
        const u64 object = request.object.load();
        if (!object || !r.image || !request.motion) {
            request.waited = 0;
            continue;
        }
        const bool held = object == *reinterpret_cast<const volatile u64*>(r.image + 0x1593ae0) ||
                          object == *reinterpret_cast<const volatile u64*>(r.image + 0x1593b48);
        if (held && ++request.waited < 4) {
            continue;
        }
        if (held) {
            LOG_INFO(Core, "cvc: motion {:#x} on {:#x} started late (no AI turn)", request.motion - r.image, object);
        }
        request.waited = 0;
        CvcStartMotion(r, request, object);
    }
    motion_guard.unlock();
    if (const int pulse = r.pulse.exchange(0); pulse && r.image && r.pulse_object) {
        CvcPulseWork(r, pulse);
    }
    for (auto& request : r.sounds) {
        const char* name = request.name.load();
        if (name && r.image) {
            CvcSoundWork(r.image, name, request.at);
            request.name.store(nullptr);
        }
    }
    for (auto& request : r.effects) {
        const char* name = request.name.load();
        if (name && r.image) {
            CvcEffectWork(r.image, name, request.at, request.yaw);
            request.name.store(nullptr);
        }
    }
    if (r.state.load() != 1 || !r.image) {
        return;
    }
    // The camera's collision filter, captured at its own line test (0 until that has run once).
    const u64 filter = *reinterpret_cast<const volatile u64*>(r.image + 0x1593ed8);
    // A filter kept on some caller's stack (this one) would be stale here: not used.
    const u64 stack = reinterpret_cast<u64>(&filter);
    if (!filter || (filter > stack - 0x40000 && filter < stack + 0x40000)) {
        return;
    }
    using LineTest = u8 PS4_SYSV_ABI (*)(void* hit, const float* from, const float* to, u64 filter,
                                         u32 single, u64 ignore);
    const auto test = reinterpret_cast<LineTest>(r.image + 0x7a7e40);
    // Both of the filter's masks (r8b 1 [filter+0x18], the camera's; 0 [filter+0x19]): the camera's
    // alone lets lines through some boulders. The nearer hit counts.
    for (int i = 0; i < r.count; i++) {
        std::memcpy(r.hit[i], r.to[i], 16);
        for (const u32 single : {1u, 0u}) {
            alignas(16) float hit[12]{};
            const auto distance = [&](const float* p) {
                return std::hypot(p[0] - r.from[i][0], p[1] - r.from[i][1], p[2] - r.from[i][2]);
            };
            if (!test(hit, r.from[i], r.to[i], filter, single, 0) && distance(hit) < distance(r.hit[i])) {
                std::memcpy(r.hit[i], hit, 16);
            }
        }
    }
    r.state.store(2);
}

// Called from guest code in the middle of the camera update: keeps every register the SysV call
// may change (nothing there expects a call), on a 32-byte aligned stack.
extern "C" __attribute__((naked)) void CvcRayThunk() {
    asm volatile("pushfq\n push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n"
                 "push %r8\n push %r9\n push %r10\n push %r11\n push %rbp\n"
                 "mov %rsp, %rbp\n and $-32, %rsp\n sub $512, %rsp\n"
                 "vmovups %ymm0, 0(%rsp)\n vmovups %ymm1, 32(%rsp)\n vmovups %ymm2, 64(%rsp)\n"
                 "vmovups %ymm3, 96(%rsp)\n vmovups %ymm4, 128(%rsp)\n vmovups %ymm5, 160(%rsp)\n"
                 "vmovups %ymm6, 192(%rsp)\n vmovups %ymm7, 224(%rsp)\n vmovups %ymm8, 256(%rsp)\n"
                 "vmovups %ymm9, 288(%rsp)\n vmovups %ymm10, 320(%rsp)\n vmovups %ymm11, 352(%rsp)\n"
                 "vmovups %ymm12, 384(%rsp)\n vmovups %ymm13, 416(%rsp)\n vmovups %ymm14, 448(%rsp)\n"
                 "vmovups %ymm15, 480(%rsp)\n"
                 "call CvcRayWork\n"
                 "vmovups 0(%rsp), %ymm0\n vmovups 32(%rsp), %ymm1\n vmovups 64(%rsp), %ymm2\n"
                 "vmovups 96(%rsp), %ymm3\n vmovups 128(%rsp), %ymm4\n vmovups 160(%rsp), %ymm5\n"
                 "vmovups 192(%rsp), %ymm6\n vmovups 224(%rsp), %ymm7\n vmovups 256(%rsp), %ymm8\n"
                 "vmovups 288(%rsp), %ymm9\n vmovups 320(%rsp), %ymm10\n vmovups 352(%rsp), %ymm11\n"
                 "vmovups 384(%rsp), %ymm12\n vmovups 416(%rsp), %ymm13\n vmovups 448(%rsp), %ymm14\n"
                 "vmovups 480(%rsp), %ymm15\n"
                 "mov %rbp, %rsp\n pop %rbp\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
                 "pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n popfq\n ret\n");
}

/// Test aid: moves a game character (an object of type 9) by a world-space delta. Besides the
/// world position copies, the character keeps its transform three times (object+0x1e0, the
/// movement code's copies at +0x13f8 and +0x14b8) and rebuilds world from them each frame
/// (0x6611c0, 0x641850); each holds its translation at +0x10 (variant 0, +0x30 = 0) or, relative to
/// the object it stands on, at +0x18 (variant 2, 0x59a8d0). Adding the world delta there assumes
/// that object is not rotated. Returns the main transform's variant.
/// Colossus vs colossus: both are where the fight starts (cvcplace), so the spectator camera may
/// report itself settled.
static std::atomic<bool> cvc_placed{false};
/// The guest has been brought beside the host (cvcplace). Until then the spectator camera leaves
/// Wander where he is: brought between a host and a guest still far away, he leaves the host
/// behind and the game tears it down.
static std::atomic<bool> cvc_guest_beside{false};

static u32 MoveCharacter(u64 object, const float delta[3]) {
    // Only copies that hold where it is now: in some characters (Basaran) these offsets hold
    // other points (0x750 his territory's centre, a zero 0x1e0 translation), and shifting those
    // made the game tear him down.
    const float* at_now = reinterpret_cast<const float*>(object + 0x130);
    const float here[3]{at_now[0], at_now[1], at_now[2]};
    const auto near_here = [&](const float* p) {
        return std::abs(p[0] - here[0]) < 50.0f && std::abs(p[1] - here[1]) < 50.0f &&
               std::abs(p[2] - here[2]) < 50.0f;
    };
    for (const u64 at : {0x130, 0x140, 0x160, 0x750, 0x7b0, 0x7c0, 0x7f0, 0x1640}) {
        auto* p = reinterpret_cast<float*>(object + at);
        if (!near_here(p)) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            p[k] += delta[k];
        }
    }
    for (const u64 transform : {0x1e0, 0x13f8, 0x14b8}) {
        const u32 variant = *reinterpret_cast<const u32*>(object + transform + 0x30);
        if (variant != 0 && variant != 2) {
            continue;
        }
        auto* p = reinterpret_cast<float*>(object + transform + (variant == 0 ? 0x10 : 0x18));
        if (variant == 0 && !near_here(p)) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            p[k] += delta[k];
        }
    }
    return *reinterpret_cast<const u32*>(object + 0x1e0 + 0x30);
}

/// Turns a character about the vertical by `yaw` radians (positive: counter-clockwise seen from
/// above). Its heading is a quaternion (x, y, z, w) at object+0x170, kept again at the other
/// offsets below (found by scanning colossi while they turned); each is turned by the same amount.
static void RotateCharacter(u64 object, float yaw) {
    const float s = std::sin(yaw * 0.5f), c = std::cos(yaw * 0.5f);
    for (const u64 at : {0x170, 0x180, 0x1a0, 0x760, 0x770, 0x8b8, 0xa90, 0x1b10, 0x1b70}) {
        auto* q = reinterpret_cast<float*>(object + at);
        const float len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (std::abs(len - 1.0f) > 0.01f) {
            continue; // not a heading in this character
        }
        // (0, s, 0, c) * q
        const float x = c * q[0] + s * q[2], y = c * q[1] + s * q[3], z = c * q[2] - s * q[0],
                    w = c * q[3] - s * q[1];
        q[0] = x, q[1] = y, q[2] = z, q[3] = w;
    }
}

static Uint32 SDLCALL ScriptTick(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    using Libraries::Pad::OrbisPadButtonDataOffset;
    struct Step {
        double at;
        std::string action;
    };
    static const std::vector<Step> steps = [] {
        std::vector<Step> out;
        // "@<path>": the script is in that file (Windows caps an environment variable at 32 KB).
        std::string text = std::getenv("SOTC_SCRIPT");
        if (text.starts_with("@res:")) {
            // A script built into the program (the Free Play colossus-vs-colossus battle).
            const auto file = cmrc::res::get_filesystem().open("src/resources/" + text.substr(5));
            text.assign(file.begin(), file.end());
        } else if (text.starts_with('@')) {
            std::ifstream file{text.substr(1)};
            text.assign(std::istreambuf_iterator<char>(file), {});
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }
        }
        std::stringstream ss{text};
        for (std::string item; std::getline(ss, item, ',');) {
            const auto colon = item.find(':');
            out.push_back({std::stod(item.substr(0, colon)), item.substr(colon + 1)});
        }
        // Steps run in order, one per tick: a step listed after a later one would wait for it.
        std::stable_sort(out.begin(), out.end(),
                         [](const Step& a, const Step& b) { return a.at < b.at; });
        return out;
    }();
    // Script seconds are counted from the first presented frame plus the usual boot time, so
    // a longer start-up (pipeline cache preload) does not shift the script against the game.
    const u64 first_flip = g_first_flip_ms.load(std::memory_order_relaxed);
    if (first_flip == 0) {
        return interval;
    }
    static const u64 usual_boot_ms = [] {
        const char* value = std::getenv("SOTC_SCRIPT_BOOT_MS");
        return value ? u64(std::atoll(value)) : 9000ULL;
    }();
    const u64 start = first_flip > usual_boot_ms ? first_flip - usual_boot_ms : 0;
    static size_t next = 0;
    static u64 release_at = 0;
    static OrbisPadButtonDataOffset held{};
    struct HeldKey {
        SDL_Event event;
        u64 release_at;
    };
    static std::vector<HeldKey> held_keys;
    struct HeldPad {
        int index;
        bool is_axis;
        u64 release_at;
    };
    static std::vector<HeldPad> held_pad;
    static u64 mouse_release_at = 0;
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    const u64 now = SDL_GetTicks();
    if (release_at && now >= release_at) {
        controller->Button(held, false);
        release_at = 0;
    }
    // Actions that are due now: from the timed script, and from SOTC_SCRIPT_FILE, a text file
    // that is polled for appended lines (one action per line, run as soon as it is seen) so a
    // run can be steered while it is in progress.
    std::vector<std::string> due;
    while (next < steps.size() && (now - start) / 1000.0 >= steps[next].at) {
        due.push_back(steps[next++].action);
    }
    static const char* live_path = std::getenv("SOTC_SCRIPT_FILE");
    static std::streamoff live_offset = 0;
    if (live_path) {
        std::ifstream live{live_path, std::ios::binary};
        live.seekg(0, std::ios::end);
        const std::streamoff size = live.tellg();
        if (size > live_offset) {
            std::string text(size - live_offset, 0);
            live.seekg(live_offset);
            live.read(text.data(), text.size());
            // Only consume complete lines.
            const auto last_newline = text.rfind(10);
            if (last_newline != std::string::npos) {
                live_offset += last_newline + 1;
                std::stringstream lines{text.substr(0, last_newline)};
                for (std::string line; std::getline(lines, line);) {
                    while (!line.empty() && (line.back() == 13 || line.back() == 32)) {
                        line.pop_back();
                    }
                    if (!line.empty()) {
                        due.push_back(line);
                    }
                }
            }
        }
    }
    for (const std::string& queued : due) {
        // "ifset=<image offset>:<action>" / "ifzero=...": run the action only while the u64 there
        // is non-zero / zero (e.g. a cave slot that says the colossi are set up). May be chained,
        // also with "ifcine:" / "ifplay:".
        std::string action = queued;
        bool run = true;
        while (run && (action.starts_with("ifset=") || action.starts_with("ifzero=") ||
                       action.starts_with("ifcine:") || action.starts_with("ifplay:"))) {
            // "ifcine:" / "ifplay:": only while a cutscene plays / does not.
            if (action.starts_with("ifcine:") || action.starts_with("ifplay:")) {
                run = action.starts_with("ifcine:") == Sotc::Screens::Letterboxed();
                action = action.substr(7);
                continue;
            }
            const bool want_set = action.starts_with("ifset=");
            const auto colon = action.find(':');
            if (colon == std::string::npos) {
                run = false;
                break;
            }
            const u64 offset = std::stoull(action.substr(action.find('=') + 1), nullptr, 16);
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 value = module ? *reinterpret_cast<const u64*>(module->GetBaseAddress() + offset) : 0;
            run = want_set == (value != 0);
            action = action.substr(colon + 1);
        }
        if (!run) {
            continue;
        }
        static const std::unordered_map<std::string, OrbisPadButtonDataOffset> buttons = {
            {"cross", OrbisPadButtonDataOffset::Cross},
            {"circle", OrbisPadButtonDataOffset::Circle},
            {"triangle", OrbisPadButtonDataOffset::Triangle},
            {"square", OrbisPadButtonDataOffset::Square},
            {"options", OrbisPadButtonDataOffset::Options},
            {"up", OrbisPadButtonDataOffset::Up},
            {"down", OrbisPadButtonDataOffset::Down},
            {"left", OrbisPadButtonDataOffset::Left},
            {"right", OrbisPadButtonDataOffset::Right},
        };
        if (action == "shotui") {
            // Screenshot including the runtime's own overlays (system dialogs).
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        } else if (action == "shot") {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        } else if (action == "quit") {
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        } else if (const auto it = buttons.find(action); it != buttons.end()) {
            held = it->second;
            controller->Button(held, true);
            release_at = now + 250;
        } else if (action == "cover=hold" || action == "cover=release") {
            // The loading screen of a free play start stays until the battle is set up.
            Sotc::Menu::HoldCover(action == "cover=hold");
        } else if (action == "openfreeplay") {
            // Test: the Free Play page, as its title menu line opens it.
            Sotc::Menu::OpenFreePlay();
        } else if (action == "leavefreeplay") {
            // Back to a plain start of the game (the end of a Free Play battle).
            Sotc::Menu::LeaveFreePlay();
        } else if (action == "rec=start" || action == "rec=stop" || action == "rec=ui") {
            // Footage capture: the game picture, the finished picture with overlays, or off
            // (see SOTC_RECORD in the presenter).
            g_sotc_record = action == "rec=start" ? 1 : action == "rec=ui" ? 2 : 0;
            if (g_sotc_record) {
                // The readout's average and 1% low then cover the footage and nothing before.
                Sotc::PerfHud::Restart();
            }
        } else if (action == "chal=win" || action == "chal=dead") {
            // Test: end a challenge run.
            Sotc::Challenge::DebugFinish(action == "chal=dead");
        } else if (action.starts_with("cvcsound=")) {
            // Test: "cvcsound=kame_stone_6" plays that game sound at the fight's midpoint (colossi A
            // and B), "cvcsound=kame_stone_6@wander" at Wander (where the listener may be).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            static std::list<std::string> names; // kept: the request holds the text
            std::string name = action.substr(9);
            const bool at_wander = name.ends_with("@wander");
            if (at_wander) {
                name.resize(name.size() - 7);
            }
            names.push_back(name);
            const u64 a = image ? *reinterpret_cast<const volatile u64*>(image + 0x1593b08) : 0;
            const u64 b = image ? *reinterpret_cast<const volatile u64*>(image + 0x1593b10) : 0;
            const u64 player = image ? PlayerObject(image) : 0;
            float at[3]{};
            if (at_wander && player) {
                std::memcpy(at, reinterpret_cast<const float*>(player + 0x130), 12);
            } else if (a && b) {
                for (int c = 0; c < 3; c++) {
                    at[c] = (reinterpret_cast<const float*>(a + 0x130)[c] + reinterpret_cast<const float*>(b + 0x130)[c]) / 2;
                }
            }
            g_cvc_rays.image = image;
            CvcSound(names.back().c_str(), at);
            LOG_INFO(Core, "cvcsound: {} at {:.0f} {:.0f} {:.0f} (at {} ms)", name, at[0], at[1], at[2],
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count());
        } else if (action.starts_with("cvceffect=")) {
            // Test: "cvceffect=npe/tts_pls_muzle.npe" starts that effect at the fight's midpoint, 25 up.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            static std::list<std::string> names; // kept: the request holds the text
            std::string name = action.substr(10);
            const bool on_a = name.ends_with("@a"); // on colossus A's chest instead
            if (on_a) {
                name.resize(name.size() - 2);
            }
            names.push_back(name);
            const u64 a = image ? *reinterpret_cast<const volatile u64*>(image + 0x1593b08) : 0;
            const u64 b = image ? *reinterpret_cast<const volatile u64*>(image + 0x1593b10) : 0;
            float at[3]{};
            if (on_a && a) {
                std::memcpy(at, reinterpret_cast<const float*>(a + 0x130), 12);
                at[1] += 20.0f;
                LOG_INFO(Core, "cvceffect: {} on A (at {} ms)", name,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count());
            } else if (a && b) {
                for (int c = 0; c < 3; c++) {
                    at[c] = (reinterpret_cast<const float*>(a + 0x130)[c] + reinterpret_cast<const float*>(b + 0x130)[c]) / 2;
                }
                at[1] += 25.0f;
            }
            g_cvc_rays.image = image;
            CvcEffect(names.back().c_str(), at);
        } else if (action.starts_with("cvcwander=")) {
            // Test: "cvcwander=90" puts Wander 90 in front of colossus B (along its root bone's -z),
            // on its ground (cvcbrawl's pulse experiments: does its AI fire at him from there?).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            const float ahead = std::stof(action.substr(10));
            const u64 b = image ? *reinterpret_cast<const volatile u64*>(image + 0x1593b10) : 0;
            const u64 player = image ? PlayerObject(image) : 0;
            const u64 mats = b ? *reinterpret_cast<const volatile u64*>(b + 0x120 + 0x198) : 0;
            if (b && player && mats) {
                const auto* m = reinterpret_cast<const float*>(mats);
                float fx = -m[8], fz = -m[10];
                const float n = std::max(std::hypot(fx, fz), 0.001f);
                fx /= n, fz /= n;
                const auto* pb = reinterpret_cast<const float*>(b + 0x130);
                const auto* pp = reinterpret_cast<const float*>(player + 0x130);
                const float delta[3]{pb[0] + fx * ahead - pp[0], pb[1] + 5.0f - pp[1], pb[2] + fz * ahead - pp[2]};
                MoveCharacter(player, delta);
                LOG_INFO(Core, "cvcwander: Wander to {:.0f} {:.0f} {:.0f}", pp[0], pp[1], pp[2]);
            }
        } else if (action.starts_with("cvcfree=")) {
            // Test: "cvcfree=B:40" leaves colossus B (or A) to its own AI for 40 s (cvcbrawl).
            const int k = action[8] == 'B' ? 1 : 0;
            const float seconds = std::stof(action.substr(10));
            g_cvc_free_until[k] = (std::chrono::steady_clock::now() + std::chrono::milliseconds(s64(seconds * 1000)))
                                      .time_since_epoch().count();
            LOG_INFO(Core, "cvcfree: {} on its own AI for {:.0f} s", k ? "B" : "A", seconds);
        } else if (action == "soundlog") {
            // Test: log the name of every sound the game plays (CvcSoundName), once each.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            static const u8 prologue[13]{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xf3};
            auto* code = reinterpret_cast<u8*>(image + 0xa5fb70);
            if (image && std::memcmp(code, prologue, sizeof(prologue)) == 0) {
                g_cvc_sound_back = image + 0xa5fb7d;
                u8 jump[13]{0x48, 0xb8};
                const u64 thunk = reinterpret_cast<u64>(&CvcSoundNameThunk);
                std::memcpy(jump + 2, &thunk, 8);
                jump[10] = 0xff, jump[11] = 0xe0, jump[12] = 0x90;
                DWORD old = 0;
                VirtualProtect(code, sizeof(jump), PAGE_EXECUTE_READWRITE, &old);
                std::memcpy(code, jump, sizeof(jump));
                VirtualProtect(code, sizeof(jump), old, &old);
                FlushInstructionCache(GetCurrentProcess(), code, sizeof(jump));
                LOG_INFO(Core, "soundlog: on");
            }
        } else if (action == "cvcrot") {
            // Test: yaw-only unit quaternions inside colossi A and B next to their root bone's
            // heading, to find where a character's heading is kept (RotateCharacter).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            for (const u64 slot : {0x1593b08ull, 0x1593b10ull}) {
                const u64 object = image ? *reinterpret_cast<const u64*>(image + slot) : 0;
                if (!object) {
                    continue;
                }
                const auto* bones = reinterpret_cast<const float*>(*reinterpret_cast<const u64*>(object + 0x120 + 0x198));
                LOG_INFO(Core, "cvcrot: {} root x({:.2f} {:.2f} {:.2f}) y({:.2f} {:.2f} {:.2f}) z({:.2f} {:.2f} {:.2f})",
                         slot == 0x1593b08 ? "A" : "B", bones[0], bones[1], bones[2], bones[4], bones[5], bones[6],
                         bones[8], bones[9], bones[10]);
                const float root_yaw = std::atan2(bones[0], bones[2]); // heading of the root's x axis
                std::string quats;
                for (u64 at = 0; at + 16 <= 0x2000; at += 4) {
                    const auto* f = reinterpret_cast<const float*>(object + at);
                    const float len = f[0] * f[0] + f[1] * f[1] + f[2] * f[2] + f[3] * f[3];
                    if (std::abs(len - 1.0f) < 0.005f && std::abs(f[0]) < 0.2f && std::abs(f[2]) < 0.2f &&
                        std::abs(f[1]) > 0.05f && std::abs(f[3]) > 0.0001f) {
                        quats += fmt::format(" +{:x}:{:.2f}", at, 2.0f * std::atan2(f[1], f[3]));
                    }
                }
                LOG_INFO(Core, "cvcrot: {} root x yaw {:.2f}; yaw quaternions{}", slot == 0x1593b08 ? "A" : "B",
                         root_yaw, quats);
            }
        } else if (action.starts_with("objlist=")) {
            // Test: walks a game object list the way the game's GetFirstObj (eboot 0x7c9820) does
            // and dumps each object (0x800 bytes) to <user>/obj_<label>_<n>.bin.
            // "objlist=228" is the list at world + 0x228 (the colossi); world from 0x15fe570.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const VAddr image = module ? module->GetBaseAddress() : 0;
            const auto read = [](VAddr address, auto& value) {
                SIZE_T got = 0;
                return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                         &value, sizeof(value), &got) &&
                       got == sizeof(value);
            };
            u64 world = 0, handle_base = 0;
            s32 handle = 0;
            static int pass = 0;
            pass++;
            // "objlist=228:0/40/0" also moves every object by that much (all copies of its
            // position found in the object: 0x130 0x140 0x160 0x750 0x7b0 0x7c0 0x7f0 0x1640).
            // "objlist=238#0:..." moves only that entry of the list.
            const u64 field = std::stoull(action.substr(8), nullptr, 16);
            const auto hash_at = action.find('#');
            const int only = hash_at != std::string::npos ? std::stoi(action.substr(hash_at + 1)) : -1;
            float move[3]{};
            // "objlist=228:@x/y/z" puts them there instead.
            const auto colon = action.find(':');
            const bool absolute = colon != std::string::npos && action[colon + 1] == '@';
            const bool moving = colon != std::string::npos &&
                                sscanf(action.c_str() + colon + 1 + absolute, "%f/%f/%f",
                                       &move[0], &move[1], &move[2]) == 3;
            if (image && read(image + 0x15fe570, world) && read(world + field, handle)) {
                read(image + 0x2f3b8c0, handle_base);
                u64 node = (handle < 0 ? handle_base : 0) + (u32(handle) & 0x7fffffff);
                for (int n = 0; node && n < 64; n++) {
                    u64 object = 0, next = 0;
                    u8 skip = 0;
                    if (!read(node, object) || !read(node + 0x18, skip) || !read(node + 0x20, next)) {
                        break;
                    }
                    std::vector<char> bytes(0x8000);
                    SIZE_T got = 0;
                    if (object) {
                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object),
                                          bytes.data(), bytes.size(), &got);
                        std::ofstream{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                                          fmt::format("obj_{:x}_{}_{}.bin", field, pass, n),
                                      std::ios::binary}
                            .write(bytes.data(), got);
                    }
                    if (object && action.ends_with(":findref")) {
                        // "objlist=238:findref": game memory within 4 MB of each object that
                        // points at its position (object+0x130), with 0x60 bytes from there-0x90.
                        std::vector<u64> chunk(0x800000 / 8);
                        SIZE_T got = 0;
                        ReadProcessMemory(GetCurrentProcess(),
                                          reinterpret_cast<const void*>(object - 0x400000),
                                          chunk.data(), chunk.size() * 8, &got);
                        for (size_t i = 0; i < got / 8; i++) {
                            if (chunk[i] == object + 0x130) {
                                const u64 at = object - 0x400000 + i * 8;
                                const float* m = reinterpret_cast<const float*>(at - 0x90);
                                LOG_INFO(Core, "findref #{}: {:#x} (obj{:+#x}) m {} {} {} {} | {} {} {} {} | {} {} {} {}",
                                         n, at, s64(at - object), m[0], m[1], m[2], m[3], m[4], m[5],
                                         m[6], m[7], m[12], m[13], m[14], m[15]);
                            }
                        }
                    }
                    if (object && n == 0 && action.find(":watchat=") != std::string::npos) {
                        // "objlist=238:watchat=7030": who writes 12 bytes at that offset.
                        const u64 off = std::stoull(action.substr(action.find(":watchat=") + 9),
                                                    nullptr, 16);
                        Sotc::Watch::Arm(object + off, 12, image, 0x1600000, 6);
                    }
                    if (object && n == 0 && action.ends_with(":watchreadA")) {
                        // "objlist=228:watchreadA": code reading the first object's position
                        // while colossus A (cvctarget) is the object being updated.
                        Sotc::Watch::reads = true;
                        Sotc::Watch::only_when = image + 0x1593b00;
                        Sotc::Watch::only_value = *reinterpret_cast<const u64*>(image + 0x1593b08);
                        Sotc::Watch::Arm(object + 0x130, 0x10, image, 0x1600000, 1000000);
                    }
                    if (object && n == 0 && action.ends_with(":watchjump")) {
                        // "objlist=238:watchjump": who moves the first object's x by over 50.
                        Sotc::Watch::jumps_only = true;
                        Sotc::Watch::Arm(object + 0x130, 4, image, 0x1600000, 6);
                    }
                    if (object && n == 0 && action.ends_with(":watchpos")) {
                        // "objlist=238:watchpos": who writes the first object's position.
                        Sotc::Watch::Arm(object + 0x130, 12, image, 0x1600000, 6);
                    }
                    if (object && n == 0 && action.ends_with(":watchq")) {
                        // "objlist=238:watchq": who queues the first object for destruction.
                        Sotc::Watch::Arm(image + 0x2f296d0, 8, image, 0x1600000, 4, object);
                    }
                    if (object && n == 0 && action.ends_with(":watch")) {
                        // "objlist=238:watch": who writes the first object's flag bytes.
                        Sotc::Watch::Arm(object + 0xc, 4, image, 0x1600000, 8);
                    }
                    if (moving && object && (only < 0 || only == n) &&
                        Core::Memory::Instance()->IsValidMapping(object, 0x1650)) {
                        float delta[3]{move[0], move[1], move[2]};
                        if (absolute) {
                            const auto* now = reinterpret_cast<const float*>(object + 0x130);
                            for (int k = 0; k < 3; k++) {
                                delta[k] = move[k] - now[k];
                            }
                        }
                        const u32 variant = MoveCharacter(object, delta);
                        LOG_INFO(Core, "objlist move #{}: variant {} delta {} {} {}", n, variant,
                                 delta[0], delta[1], delta[2]);
                    }
                    LOG_INFO(Core, "objlist {:x} #{}: node {:#x} object {:#x} type {} skip {}",
                             field, n, node, object, got > 0xc ? int(u8(bytes[0xc])) : -1, skip);
                    node = next;
                }
            }
            LOG_INFO(Core, "objlist {:x}: world {:#x} handle {:#x}", field, world, u32(handle));
        } else if (action.starts_with("layers=")) {
            // Test: the game's streamed level layers (list at image 0x3017e88, next at +0xb8) with
            // their type hash (*(+0x88)+8) and state (+0xa0), to <user>/layers_<label>.txt.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const auto read = [](u64 address, auto& value) {
                SIZE_T got = 0;
                return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                         &value, sizeof(value), &got) &&
                       got == sizeof(value);
            };
            std::ofstream out{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                              ("layers_" + action.substr(7) + ".txt")};
            u64 layer = 0;
            if (module) {
                read(module->GetBaseAddress() + 0x3017e88, layer);
            }
            for (int n = 0; layer && n < 4000; n++) {
                u64 info = 0, next = 0;
                u32 hash = 0, state = 0;
                read(layer + 0x88, info);
                read(info + 8, hash);
                read(layer + 0xa0, state);
                out << fmt::format("{:x} {:08x} {:08x}\n", layer, hash, state);
                if (!read(layer + 0xb8, next)) {
                    break;
                }
                layer = next;
            }
        } else if (action.starts_with("pinlayers=")) {
            // Test: keeps the layers now in the list loaded for that many seconds, by holding
            // their countdown (+0xa0, bits 20-29) at the game's "requested" value from a thread.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            std::vector<u64> pinned;
            u64 layer = module ? *reinterpret_cast<const u64*>(module->GetBaseAddress() + 0x3017e88)
                               : 0;
            for (int n = 0; layer && n < 64; n++) {
                pinned.push_back(layer);
                layer = *reinterpret_cast<const u64*>(layer + 0xb8);
            }
            const int seconds = std::stoi(action.substr(10));
            LOG_INFO(Core, "pinning {} layers for {} s", pinned.size(), seconds);
            std::thread([pinned, seconds] {
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                while (std::chrono::steady_clock::now() < end) {
                    for (const u64 l : pinned) {
                        auto* state = reinterpret_cast<std::atomic<u32>*>(l + 0xa0);
                        u32 v = state->load();
                        state->compare_exchange_weak(v, (v & ~0x3ff00000u) | 0x1ff00000u);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }).detach();
        } else if (action.starts_with("sheet=") || action.starts_with("loadsheet=")) {
            // Test: queues a load request for one of the game's streamed packages ("sheets"),
            // as the game's 0x6af8e0 does: the registry at image 0x16d3848 and the package id
            // (free list 0x30262c0/0x30262c8, queue 0x30262f0..0x30262f8, next at +0x48).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            const u32 id = std::stoul(action.substr(action.find('=') + 1), nullptr, 16);
            const auto q = [&](u64 a) { return *reinterpret_cast<u64*>(a); };
            const u64 desc = image ? image + 0x16d3848 : 0;
            if (action.starts_with("loadsheet=") && desc) {
                const u64 node = q(image + 0x30262c0);
                if (node) {
                    const u64 next = q(node + 0x48);
                    *reinterpret_cast<u64*>(image + 0x30262c0) = next;
                    if (!next) {
                        *reinterpret_cast<u64*>(image + 0x30262c8) = 0;
                    }
                    *reinterpret_cast<u8*>(node + 4) &= 0xfe;
                    *reinterpret_cast<u32*>(node) = 0;
                    *reinterpret_cast<u64*>(node + 8) = desc;
                    *reinterpret_cast<u32*>(node + 0x10) = id;
                    *reinterpret_cast<u64*>(node + 0x40) = 0;
                    *reinterpret_cast<u64*>(node + 0x48) = 0;
                    const u64 tail = q(image + 0x30262f8);
                    if (tail) {
                        *reinterpret_cast<u64*>(tail + 0x48) = node;
                    } else {
                        *reinterpret_cast<u64*>(image + 0x30262f0) = node;
                    }
                    *reinterpret_cast<u64*>(image + 0x30262f8) = node;
                    LOG_INFO(Core, "sheet {:#x}: load queued", id);
                }
            }
        } else if (action.starts_with("hold=")) {
            // Test: keeps a 32-bit word of the game image at a value for a while, rewriting it
            // every millisecond ("hold=325f430:3:20000").
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            unsigned long long offset = 0;
            unsigned value = 0, ms = 0;
            sscanf(action.c_str() + 5, "%llx:%u:%u", &offset, &value, &ms);
            if (module) {
                auto* word = reinterpret_cast<std::atomic<u32>*>(module->GetBaseAddress() + offset);
                std::thread([word, value, ms] {
                    const auto end =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                    while (std::chrono::steady_clock::now() < end) {
                        word->store(value);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }).detach();
            }
        } else if (action.starts_with("patch=")) {
            // Test: overwrite game code at an image offset with hex bytes
            // ("patch=6a9950:b801000000c3" makes that function return 1).
            const auto colon = action.find(':');
            const u64 offset = std::stoull(action.substr(6, colon - 6), nullptr, 16);
            std::vector<u8> bytes;
            for (size_t i = colon + 1; i + 1 < action.size(); i += 2) {
                bytes.push_back(static_cast<u8>(std::stoul(action.substr(i, 2), nullptr, 16)));
            }
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            if (module && !bytes.empty()) {
                auto* code = reinterpret_cast<u8*>(module->GetBaseAddress() + offset);
                DWORD old = 0;
                VirtualProtect(code, bytes.size(), PAGE_EXECUTE_READWRITE, &old);
                std::memcpy(code, bytes.data(), bytes.size());
                VirtualProtect(code, bytes.size(), old, &old);
                FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
                LOG_INFO(Core, "script {}: {} bytes written", action, bytes.size());
            }
        } else if (action.starts_with("setlist=")) {
            // Test: "setlist=228:238#1" points the head of list world+0x228 at the second node of
            // list world+0x238 (handle = 0x80000000 | (node - *(image+0x2f3b8c0))).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            unsigned long long dst = 0, src = 0;
            int pick = 0;
            sscanf(action.c_str() + 8, "%llx:%llx#%d", &dst, &src, &pick);
            const u64 world = image ? *reinterpret_cast<const u64*>(image + 0x15fe570) : 0;
            const u64 base = image ? *reinterpret_cast<const u64*>(image + 0x2f3b8c0) : 0;
            if (world) {
                const s32 handle = *reinterpret_cast<const s32*>(world + src);
                u64 node = (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff);
                for (int n = 0; node && n < pick; n++) {
                    node = *reinterpret_cast<const u64*>(node + 0x20);
                }
                if (node) {
                    const u32 value = node >= base ? 0x80000000u | u32(node - base) : u32(node);
                    *reinterpret_cast<u32*>(world + dst) = value;
                    LOG_INFO(Core, "setlist: world+{:x} = {:#x} (node {:#x})", dst, value, node);
                }
            }
        } else if (action.starts_with("floor=")) {
            // Test: "floor=238#0:63:60000" keeps that list entry from sinking below height 63 for
            // 60 s (every copy of its position's y), for colossi outside their own arena, whose
            // ground they would fall through.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            unsigned long long src = 0;
            int pick = 0;
            float floor_y = 0.0f;
            unsigned ms = 0;
            sscanf(action.c_str() + 6, "%llx#%d:%f:%u", &src, &pick, &floor_y, &ms);
            const u64 world = image ? *reinterpret_cast<const u64*>(image + 0x15fe570) : 0;
            const u64 base = image ? *reinterpret_cast<const u64*>(image + 0x2f3b8c0) : 0;
            if (world) {
                const s32 handle = *reinterpret_cast<const s32*>(world + src);
                u64 node = (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff);
                for (int n = 0; node && n < pick; n++) {
                    node = *reinterpret_cast<const u64*>(node + 0x20);
                }
                const u64 object = node ? *reinterpret_cast<const u64*>(node) : 0;
                LOG_INFO(Core, "floor: object {:#x} kept above {}", object, floor_y);
                if (object) {
                    std::thread([object, floor_y, ms] {
                        const auto end =
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                        while (std::chrono::steady_clock::now() < end) {
                            const float y = *reinterpret_cast<const float*>(object + 0x134);
                            if (y < floor_y) {
                                const float lift[3]{0.0f, floor_y - y, 0.0f};
                                MoveCharacter(object, lift);
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }).detach();
                }
            }
        } else if (action.starts_with("cvcplay=")) {
            // Test: "cvcplay=A:1d3a5f0:3000" starts that motion (static MotionKind, image offset) on
            // colossus A (or B) and keeps its AI out (cave 0x1593ae0) for 3000 ms, so the AI does
            // not replace it.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            char who = 'A';
            unsigned long long motion = 0;
            unsigned ms = 0;
            sscanf(action.c_str() + 8, "%c:%llx:%u", &who, &motion, &ms);
            const u64 object = image ? *reinterpret_cast<const u64*>(image + (who == 'B' ? 0x1593b10 : 0x1593b08)) : 0;
            if (object && motion) {
                *reinterpret_cast<volatile u64*>(image + 0x1593ae0) = object;
                CvcPlayMotion(image, object, motion);
                std::thread([image, object, ms] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                    auto* slot = reinterpret_cast<volatile u64*>(image + 0x1593ae0);
                    if (*slot == object) {
                        *slot = 0;
                    }
                }).detach();
            }
        } else if (action == "pinloaded") {
            // Colossus vs colossus with any guest: before the jump to the host's arena, one more
            // reference on every package loaded now (request counts, a byte per stage index, at
            // *(image+0x3026330)), so the guest colossus and its arena's data stay (for Gaius
            // these were found one by one: his motions, switch and E2 area layouts).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            auto* counts = image ? *reinterpret_cast<u8**>(image + 0x3026330) : nullptr;
            int pinned = 0;
            for (int i = 0; counts && i < 0x700; i++) { // ~0x6da stages; other data follows
                if (counts[i] > 0 && counts[i] < 0xff) {
                    counts[i]++, pinned++;
                }
            }
            LOG_INFO(Core, "pinloaded: {} packages kept", pinned);
        } else if (action.starts_with("cvcplace=")) {
            // Colossus vs colossus in any arena: "cvcplace=80" puts the guest A (cave 0x1593b08)
            // 80 units from the host B (0x1593b10) on the side toward Wander, who arrived over open
            // ground, keeps A from sinking below B's ground (it walks only on its own arena's
            // ground) and records the middle as the fight's centre (0x1593b78 x, 0x1593b7c z).
            // "cvcplace=55@930/-900": on the side toward that point instead; "...@930/-900+110" also
            // walks the host 110 units toward it once its first-sight cutscene is over, in steps of
            // one unit a frame (moved before or during that cutscene, or in one jump after it, the
            // game puts it back or tears it down).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            float dist = 0.0f, open[2]{}, walk_out = 0.0f;
            const bool has_open =
                sscanf(action.c_str() + 9, "%f@%f/%f+%f", &dist, &open[0], &open[1], &walk_out) >= 3;
            const u64 a = image ? *reinterpret_cast<const u64*>(image + 0x1593b08) : 0;
            const u64 b = image ? *reinterpret_cast<const u64*>(image + 0x1593b10) : 0;
            const u64 wander = image ? PlayerObject(image) : 0;
            if (a && b && wander) {
                std::thread([image, a, b, wander, dist, has_open, open, walk_out] {
                    const auto* pb = reinterpret_cast<const volatile float*>(b + 0x130);
                    const auto* pa = reinterpret_cast<const volatile float*>(a + 0x130);
                    const auto* pw = reinterpret_cast<const volatile float*>(wander + 0x130);
                    float floor_y = 0.0f;
                    const auto place = [&] {
                        float dx = (has_open ? open[0] : pw[0]) - pb[0], dz = (has_open ? open[1] : pw[2]) - pb[2];
                        const float len = std::max(std::hypot(dx, dz), 0.001f);
                        dx /= len, dz /= len;
                        const float to[3]{pb[0] + dx * dist, pb[1] + 3.0f, pb[2] + dz * dist};
                        // In steps of 10 units a frame: one jump of hundreds of units while the host
                        // was already awake (its first-sight cutscene played before the setup) crashed
                        // the game at once in its per-frame list processing (unlink at 0x5fbfcc).
                        for (int i = 0; i < 200; i++) {
                            float delta[3]{to[0] - pa[0], to[1] - pa[1], to[2] - pa[2]};
                            const float left = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
                            if (left > 10.0f) {
                                for (float& d : delta) {
                                    d *= 10.0f / left;
                                }
                            }
                            MoveCharacter(a, delta);
                            if (left <= 10.0f) {
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        }
                        auto* centre = reinterpret_cast<volatile float*>(image + 0x1593b78);
                        centre[0] = pb[0] + dx * dist / 2, centre[1] = pb[2] + dz * dist / 2;
                        floor_y = pb[1] - 3.0f;
                        cvc_guest_beside = true;
                        LOG_INFO(Core, "cvcplace: B at {:.0f} {:.0f} {:.0f}, A to {:.0f} {:.0f} {:.0f} ({:.0f} from B), kept "
                                 "above {:.0f}", pb[0], pb[1], pb[2], to[0], to[1], to[2], dist, floor_y);
                    };
                    place();
                    // The host walked out toward the open side, two units a frame (it must be awake:
                    // moved before or during its first-sight cutscene the game puts it back).
                    const auto walk_host_out = [&] {
                        float moved = 0.0f;
                        while (moved < walk_out && *reinterpret_cast<const volatile u64*>(b) != 0) {
                            const float dx = open[0] - pb[0], dz = open[1] - pb[2];
                            const float len = std::max(std::hypot(dx, dz), 0.001f);
                            const float step[3]{2.0f * dx / len, 0.0f, 2.0f * dz / len};
                            MoveCharacter(b, step);
                            moved += 2.0f;
                            std::this_thread::sleep_for(std::chrono::milliseconds(33));
                        }
                        if (walk_out > 0.0f) {
                            LOG_INFO(Core, "cvcplace: B slid {:.0f} to {:.0f} {:.0f} {:.0f}, object {} (at {} ms)", moved, pb[0], pb[1],
                                     pb[2], *reinterpret_cast<const volatile u64*>(b) != 0 ? "kept" : "torn down", SDL_GetTicks());
                        }
                    };
                    // The host's first-sight cutscene can follow (Wander brought near). It puts the
                    // host back where the game has it, so then A is placed beside it again. The fight
                    // counts as placed (cvc_placed) once it is over, or after 10 s without one.
                    const auto start = std::chrono::steady_clock::now();
                    bool cutscene = false;
                    const auto end = start + std::chrono::hours(1);
                    while (std::chrono::steady_clock::now() < end) {
                        u8 type = 0;
                        SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a + 0xc), &type, 1,
                                               &got) || type != 9) {
                            cvc_placed = true;
                            return;
                        }
                        if (!cvc_placed) {
                            const bool now = Sotc::Screens::Letterboxed();
                            if (now && !cutscene) {
                                LOG_INFO(Core, "cvcplace: a cutscene plays (at {} ms)", SDL_GetTicks());
                            }
                            if (cutscene && !now) {
                                walk_host_out();
                                place();
                                cvc_placed = true;
                            } else if (!cutscene && !now && std::chrono::steady_clock::now() > start + std::chrono::seconds(10)) {
                                // None followed: it played before the setup (the host already awake),
                                // so the host is walked out now.
                                walk_host_out();
                                place();
                                cvc_placed = true;
                            }
                            cutscene = cutscene || now;
                        }
                        // A net under it, not a floor. Where the game's line test finds ground under it
                        // (measured within the last second), only a fall through that ground is caught,
                        // back onto it. Where there is none, the guess (3 below the host's root) holds it.
                        // (Held at the guess whenever it was below, Gaius shook up and down on Quadratus's
                        // ground, 1.5-3.8 lower: lifted, then set down by the game, every other frame.)
                        const float y = pa[1];
                        const double now_s =
                            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        const float ground = g_cvc_ground_a.load();
                        const bool measured = now_s - g_cvc_ground_a_at.load() < 1.0;
                        float catch_at = floor_y, put_at = floor_y;
                        if (measured && !std::isnan(ground)) {
                            catch_at = ground - 2.0f, put_at = ground;
                        } else if (!measured) {
                            catch_at = floor_y - 6.0f; // (not measured yet: only a long fall)
                        }
                        if (y < catch_at && put_at - y < (measured ? 6.0f : 1e9f)) {
                            const float lift[3]{0.0f, put_at - y, 0.0f};
                            MoveCharacter(a, lift);
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }).detach();
            }
        } else if (action.starts_with("cvctarget=")) {
            // Test: data for the per-colossus target stubs in the code segment's spare tail
            // (cave page image 0x1593000 made writable): at 0x1593b00 the object being updated,
            // 0x1593b08/10 colossus A/B objects, 0x1593b18 the player list (world+0x228),
            // 0x1593b20/24 one-entry lists holding B's / A's node handle.
            // "cvctarget=238#0:238#1".
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            unsigned long long la = 0, lb = 0;
            int ia = 0, ib = 0, boss_a = -1, boss_b = -1;
            sscanf(action.c_str() + 10, "%llx#%d:%llx#%d:%d:%d", &la, &ia, &lb, &ib, &boss_a,
                   &boss_b);
            const u64 world = image ? *reinterpret_cast<const u64*>(image + 0x15fe570) : 0;
            const u64 base = image ? *reinterpret_cast<const u64*>(image + 0x2f3b8c0) : 0;
            const auto node_of = [&](u64 field, int pick) -> u64 {
                const s32 handle = *reinterpret_cast<const s32*>(world + field);
                u64 node = (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff);
                for (int n = 0; node && n < pick; n++) {
                    node = *reinterpret_cast<const u64*>(node + 0x20);
                }
                return node;
            };
            // A usable colossus: a live character (type 9, not being torn down) with an AI
            // workspace (0x746d50). After a boss jump the list briefly holds stand-ins that are not.
            const auto usable = [&](u64 node) {
                const u64 object = node ? *reinterpret_cast<const u64*>(node) : 0;
                if (!object || *reinterpret_cast<const u8*>(object + 0xc) != 9 ||
                    *reinterpret_cast<const u64*>(object + 0x1690) == 0) {
                    return false;
                }
                const u32 flags = *reinterpret_cast<const u32*>(object + 0xc);
                return !((flags & 0x10000) && *reinterpret_cast<const u64*>(object) == 0);
            };
            u64 na = world ? node_of(la, ia) : 0, nb = world ? node_of(lb, ib) : 0;
            if (na && !usable(na)) {
                na = 0;
            }
            if (nb && !usable(nb)) {
                // The first other usable entry of B's list instead.
                nb = 0;
                for (u64 node = node_of(lb, 0); node; node = *reinterpret_cast<const u64*>(node + 0x20)) {
                    if (node != na && usable(node) &&
                        *reinterpret_cast<const u64*>(node) != (na ? *reinterpret_cast<const u64*>(na) : 0)) {
                        nb = node;
                        break;
                    }
                }
            }
            if (!na || !nb) {
                // Not both loaded yet: keep any earlier setup, so the action can be repeated.
                LOG_INFO(Core, "cvctarget: not ready (A node {:#x}, B node {:#x})", na, nb);
            } else {
                DWORD old = 0;
                VirtualProtect(reinterpret_cast<void*>(image + 0x1593000), 0x1000,
                               PAGE_EXECUTE_READWRITE, &old);
                const auto handle_of = [&](u64 node) {
                    return node >= base ? 0x80000000u | u32(node - base) : u32(node);
                };
                auto* data = reinterpret_cast<u8*>(image + 0x1593b00);
                *reinterpret_cast<u64*>(data + 0x08) = na ? *reinterpret_cast<const u64*>(na) : 0;
                *reinterpret_cast<u64*>(data + 0x10) = nb ? *reinterpret_cast<const u64*>(nb) : 0;
                *reinterpret_cast<u64*>(data + 0x18) = world + 0x228;
                // One-entry lists for the redirect: our own nodes in the cave (object at +0,
                // skip byte +0x18, next +0x20), as list nodes of the game come and go. The cave
                // lies below 2 GB, so a plain (non-negative) handle is the node's address.
                const u64 obj_a = na ? *reinterpret_cast<const u64*>(na) : 0;
                const u64 obj_b = nb ? *reinterpret_cast<const u64*>(nb) : 0;
                auto* node_for_a = reinterpret_cast<u64*>(image + 0x1593f00); // holds B
                auto* node_for_b = reinterpret_cast<u64*>(image + 0x1593f40); // holds A
                std::memset(node_for_a, 0, 0x40);
                std::memset(node_for_b, 0, 0x40);
                node_for_a[0] = obj_b;
                node_for_b[0] = obj_a;
                if (action.find(":oneway") != std::string::npos) {
                    // B keeps its real target (the player list's first object).
                    const u64 player_node = node_of(0x228, 0);
                    node_for_b[0] = player_node ? *reinterpret_cast<const u64*>(player_node) : 0;
                }
                *reinterpret_cast<u32*>(data + 0x20) = u32(image + 0x1593f00);
                *reinterpret_cast<u32*>(data + 0x24) = u32(image + 0x1593f40);
                // 0x1593b28/30: their boss-table entries (table at *(image+0x16d21f8+0x20),
                // 0x1e8 per colossus), which GetCurrentBoss answers while each one updates.
                const u64 table = *reinterpret_cast<const u64*>(image + 0x16d21f8 + 0x20);
                *reinterpret_cast<u64*>(data + 0x28) = boss_a >= 0 ? table + boss_a * 0x1e8ull : 0;
                *reinterpret_cast<u64*>(data + 0x30) = boss_b >= 0 ? table + boss_b * 0x1e8ull : 0;
                *reinterpret_cast<u64*>(data + 0x38) = 0; // no knockout loser yet
                *reinterpret_cast<u64*>(data + 0x40) = 0;
                *reinterpret_cast<u64*>(data + 0x48) = 0; // no colossus kept out of the AI
                // Knockout not shown, not filming (cvc_arena.sh FILM; ifset/ifzero read 8 bytes).
                *reinterpret_cast<u64*>(data + 0x50) = 0;
                *reinterpret_cast<u64*>(data + 0x58) = 0;
                *reinterpret_cast<u64*>(data + 0x60) = 0; // the colossi have not closed in yet (cvccam)
                *reinterpret_cast<u64*>(data + 0x68) = 0; // not yet back to the title (Free Play CvC)
                *reinterpret_cast<u64*>(data + 0x70) = 0; // PARENTGUARD count (re/cvc/contact_patch.py)
                *reinterpret_cast<u64*>(data + 0x78) = 0; // no fight centre yet (cvcplace)
                *reinterpret_cast<u64*>(image + 0x1593ae0) = 0; // nobody's AI held for a move (cvcbrawl)
                *reinterpret_cast<u64*>(image + 0x1593ae8) = 0; // the spectator camera has not settled yet
                cvc_placed = false;
                cvc_guest_beside = false;
                LOG_INFO(Core, "cvctarget: A {:#x} (node {:#x}) <-> B {:#x} (node {:#x}) (at {} ms)",
                         *reinterpret_cast<u64*>(data + 0x08), na,
                         *reinterpret_cast<u64*>(data + 0x10), nb, SDL_GetTicks());
            }
        } else if (action == "sampcur") {
            // Test: how often the update wrapper's current object (image 0x1593b00) is colossus
            // A or B (0x1593b08/10), sampled for 2 s.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 data = module ? module->GetBaseAddress() + 0x1593b00 : 0;
            std::thread([data] {
                const auto* v = reinterpret_cast<const volatile u64*>(data);
                int a = 0, b = 0, total = 0;
                std::map<u64, int> seen;
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < end) {
                    const u64 cur = v[0];
                    a += cur == v[1];
                    b += cur == v[2];
                    seen[cur]++;
                    total++;
                }
                LOG_INFO(Core, "sampcur: {} samples, A {}, B {}, {} distinct", total, a, b, seen.size());
            }).detach();
        } else if (action.starts_with("awaitmoved=")) {
            // Test: "awaitmoved=238:120:1593ef8" sets the byte at that image offset (cave page made
            // writable) once the first object of list world+0x238 has moved more than 2 units from
            // where it was first seen (a colossus whose logic has started), giving up after 120 s.
            // "238#1:..." watches the second entry.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            unsigned long long field = 0, flag = 0;
            float seconds = 0;
            int pick = 0;
            const bool picked = action.find('#') != std::string::npos;
            // An optional last field: how many 200 ms checks in a row without a cinematic (15).
            int calm_needed = 15;
            if (picked) {
                sscanf(action.c_str() + 11, "%llx#%d:%f:%llx:%d", &field, &pick, &seconds, &flag, &calm_needed);
            } else {
                sscanf(action.c_str() + 11, "%llx:%f:%llx:%d", &field, &seconds, &flag, &calm_needed);
            }
            if (image) {
                DWORD old = 0;
                VirtualProtect(reinterpret_cast<void*>((image + flag) & ~u64(0xfff)), 0x1000,
                               PAGE_EXECUTE_READWRITE, &old);
            }
            std::thread([image, field, pick, seconds, flag, calm_needed] {
                const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
                const auto end = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(u64(seconds * 1000));
                u64 first_object = 0;
                float first[3]{};
                while (image && std::chrono::steady_clock::now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    const u64 world = q(image + 0x15fe570), base = q(image + 0x2f3b8c0);
                    if (!world) {
                        continue;
                    }
                    const s32 handle = *reinterpret_cast<const volatile s32*>(world + field);
                    u64 node = (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff);
                    for (int n = 0; node && n < pick; n++) {
                        node = q(node + 0x20);
                    }
                    const u64 object = node ? q(node) : 0;
                    if (!object) {
                        continue;
                    }
                    const auto* p = reinterpret_cast<const volatile float*>(object + 0x130);
                    if (object != first_object) {
                        first_object = object;
                        first[0] = p[0], first[1] = p[1], first[2] = p[2];
                        LOG_INFO(Core, "awaitmoved: object {:#x} found (at {} ms)", object, SDL_GetTicks());
                        continue;
                    }
                    // It moves during its intro cinematic already, and a boss jump started then
                    // never finishes loading: also wait for 3 s without a cinematic (the
                    // letterbox amount, image 0x2dcdf04, see Screens::Letterboxed).
                    thread_local int calm = 0;
                    const float cinematic = *reinterpret_cast<const volatile float*>(image + 0x2dcdf04);
                    if ((cinematic == 0.0f) != (calm > 0)) {
                        LOG_INFO(Core, "awaitmoved: cinematic {} (at {} ms)", cinematic == 0.0f ? "off" : "on", SDL_GetTicks());
                    }
                    calm = cinematic == 0.0f ? calm + 1 : 0;
                    if (std::hypot(p[0] - first[0], p[2] - first[2]) > 2.0f && calm >= calm_needed) {
                        *reinterpret_cast<volatile u8*>(image + flag) = 1;
                        LOG_INFO(Core, "awaitmoved: object {:#x} moved, flag {:#x} set (at {} ms)", object, flag, SDL_GetTicks());
                        return;
                    }
                }
                LOG_INFO(Core, "awaitmoved: gave up (object {:#x})", first_object);
            }).detach();
        } else if (action.starts_with("cvckeep=")) {
            // Test: "cvckeep=400" keeps colossi A and B (cave 0x1593b08/10) pointing at live
            // characters of list world+0x238 for 400 s: a boss's object can be replaced by another
            // one (or freed) while the fight runs. When A or B is no longer a usable entry of the
            // list, the other usable entry takes its place, in its slot and in the one-entry
            // target list of the other colossus (0x1593f40 holds A, 0x1593f00 holds B), unless the
            // fight is over (winner byte 0x1593efb).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            const float seconds = std::stof(action.substr(8));
            std::thread([image, seconds] {
                const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
                const auto usable = [&](u64 object) {
                    if (!object || *reinterpret_cast<const volatile u8*>(object + 0xc) != 9 ||
                        q(object + 0x1690) == 0) {
                        return false;
                    }
                    const u32 flags = *reinterpret_cast<const volatile u32*>(object + 0xc);
                    return !((flags & 0x10000) && q(object) == 0);
                };
                const auto end = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(u64(seconds * 1000));
                bool waiting[2]{}; // A / B gone with no replacement yet
                // Ticks A / B has been missing in a row: moving a colossus can make it unusable
                // for a moment, which is not being gone.
                int missing[2]{};
                bool retargeted[2]{}; // the other's target list set to Wander while this one was torn down
                // Ticks each usable object has been listed in a row: a boss object the game has
                // just created is not set up for the AI yet (crash at 0x839ccd when adopted at once).
                std::unordered_map<u64, int> age;
                while (image && std::chrono::steady_clock::now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    const u64 world = q(image + 0x15fe570), base = q(image + 0x2f3b8c0);
                    if (!world) {
                        continue;
                    }
                    std::vector<u64> live;
                    const s32 handle = *reinterpret_cast<const volatile s32*>(world + 0x238);
                    for (u64 node = handle ? (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff) : 0;
                         node && live.size() < 16; node = q(node + 0x20)) {
                        if (usable(q(node))) {
                            live.push_back(q(node));
                        }
                    }
                    std::unordered_map<u64, int> older;
                    for (const u64 o : live) {
                        older[o] = age.contains(o) ? age[o] + 1 : 1;
                    }
                    age.swap(older);
                    const auto listed = [&](u64 o) {
                        return std::find(live.begin(), live.end(), o) != live.end();
                    };
                    for (const u64 slot : {0x1593b08ull, 0x1593b10ull}) {
                        const u64 current = q(image + slot);
                        bool& empty = waiting[slot == 0x1593b10];
                        if (current ? listed(current) : !empty) {
                            if (current && std::exchange(retargeted[slot == 0x1593b10], false) &&
                                !*reinterpret_cast<const volatile u8*>(image + 0x1593efb)) {
                                *reinterpret_cast<volatile u64*>(image + (slot == 0x1593b08 ? 0x1593f40 : 0x1593f00)) =
                                    current; // back: the other targets it again
                            }
                            missing[slot == 0x1593b10] = 0;
                            continue; // fine, or cleared on purpose (defeat)
                        }
                        // Torn down (+0 cleared) during a first-sight cutscene: Valus's comes back, so
                        // meanwhile the other only targets Wander; Barba's is freed (filled with
                        // 0xdeadbeef54321abc), no moment's lapse: the other's target list pointed at
                        // freed memory (crash at 0x60541a), so it goes at once.
                        const bool torn = current && q(current) == 0 &&
                                          (*reinterpret_cast<const volatile u32*>(current + 0xc) & 0x10000);
                        const bool freed = torn && [&] {
                            for (u64 at = current; at < current + 0x200; at += 8) {
                                if (q(at) == 0xdeadbeef54321abcull) {
                                    return true;
                                }
                            }
                            return false;
                        }();
                        if (torn && !freed && !*reinterpret_cast<const volatile u8*>(image + 0x1593efb)) {
                            *reinterpret_cast<volatile u64*>(image + (slot == 0x1593b08 ? 0x1593f40 : 0x1593f00)) =
                                PlayerObject(image);
                            retargeted[slot == 0x1593b10] = true;
                        }
                        if (current && !freed && ++missing[slot == 0x1593b10] < 30) {
                            if (missing[slot == 0x1593b10] == 1) {
                                LOG_INFO(Core, "cvckeep: {} {:#x} unusable (type {}, flags {:#x}, +0 {:#x}, +0x1690 {:#x})",
                                         slot == 0x1593b08 ? "A" : "B", current,
                                         *reinterpret_cast<const volatile u8*>(current + 0xc),
                                         *reinterpret_cast<const volatile u32*>(current + 0xc), q(current),
                                         q(current + 0x1690));
                            }
                            continue;
                        }
                        const u64 other = q(image + (slot == 0x1593b08 ? 0x1593b10 : 0x1593b08));
                        u64 replacement = 0;
                        for (const u64 o : live) {
                            if (o != other && age[o] >= 30) {
                                replacement = o;
                                break;
                            }
                        }
                        if (!current && !replacement) {
                            continue; // still nobody to take the place
                        }
                        empty = !replacement;
                        *reinterpret_cast<volatile u64*>(image + slot) = replacement;
                        if (!*reinterpret_cast<const volatile u8*>(image + 0x1593efb)) {
                            // A's target list holds B and the other way round; meanwhile Wander.
                            *reinterpret_cast<volatile u64*>(
                                image + (slot == 0x1593b08 ? 0x1593f40 : 0x1593f00)) =
                                replacement ? replacement : PlayerObject(image);
                        }
                        LOG_INFO(Core, "cvckeep: {} {:#x} gone, now {:#x}", slot == 0x1593b08 ? "A" : "B",
                                 current, replacement);
                    }
                }
            }).detach();
        } else if (action.starts_with("cvccam=")) {
            // Test: spectator vantage. "cvccam=300:180:40": for 300 s keep Wander (list world+0x228)
            // beside the colossi A/B (cave 0x1593b08/10): when he is not between 0.6 and 1.6 times
            // 180 units from their midpoint, he is put 180 units from it, square to the line
            // between them, 40 units above it (he then drops to the ground). With "view
            // colossus" held the game's camera then looks past him at the fight.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            float seconds = 0, dist = 0, height = 0, steer = 1;
            // A fourth value 0 leaves the camera to the game (e.g. "view colossus" held).
            sscanf(action.c_str() + 7, "%f:%f:%f:%f", &seconds, &dist, &height, &steer);
            std::thread([image, seconds, dist, height, steer] {
                const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
                const auto end = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(u64(seconds * 1000));
                float side = 1.0f;
                int tick = 0;
                while (std::chrono::steady_clock::now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    const u64 a = q(image + 0x1593b08), b = q(image + 0x1593b10);
                    const u64 world = q(image + 0x15fe570), base = q(image + 0x2f3b8c0);
                    if ((!a && !b) || !world || !cvc_guest_beside) {
                        continue;
                    }
                    // Where A and B are, or were last: after a knockout the loser's slot is cleared
                    // and the camera keeps both the winner and where the loser fell in view. A
                    // position far out is an object freed under us (cvckeep replaces it).
                    static float last[2][3];
                    static bool seen[2];
                    std::vector<std::array<float, 3>> body_points; // both skeletons, this tick
                    for (int k = 0; k < 2; k++) {
                        // A knocked-out colossus is followed through the loser slot (0x1593b38) once
                        // its own is cleared: its body still falls some way.
                        const u8 won = *reinterpret_cast<const volatile u8*>(image + 0x1593efb);
                        u64 object = (k ? b : a) ? (k ? b : a) : won == (k ? 1 : 2) ? q(image + 0x1593b38) : 0;
                        if (object && !(k ? b : a)) {
                            // Only while it is still a character (it may be freed after its death).
                            u8 type = 0;
                            SIZE_T got = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + 0xc),
                                                   &type, 1, &got) ||
                                type != 9) {
                                object = 0;
                            }
                        }
                        const auto* p = object ? reinterpret_cast<const float*>(object + 0x130) : nullptr;
                        if (p && std::abs(p[0]) < 1e5f && std::abs(p[1]) < 1e5f && std::abs(p[2]) < 1e5f) {
                            std::memcpy(last[k], p, sizeof(last[k]));
                            seen[k] = true;
                            // Across the ground, the middle of its skeleton (bone world matrices at
                            // component +0x198, translation +0x30): a falling colossus's body ends up
                            // far from its root position.
                            const u64 matrices = q(object + 0x120 + 0x198);
                            float sum[2]{};
                            int bones = 0;
                            for (int i = 0; matrices && i < 256; i++) {
                                float m[4]{};
                                SIZE_T got = 0;
                                if (!ReadProcessMemory(GetCurrentProcess(),
                                                       reinterpret_cast<const void*>(matrices + i * 64 + 0x30), m,
                                                       sizeof(m), &got) ||
                                    m[3] != 1.0f || std::hypot(m[0] - p[0], m[2] - p[2]) > 400.0f) {
                                    break;
                                }
                                sum[0] += m[0], sum[1] += m[2], bones++;
                                body_points.push_back({m[0], m[1], m[2]});
                            }
                            if (bones > 0) {
                                last[k][0] = sum[0] / bones, last[k][2] = sum[1] / bones;
                            }
                        }
                    }
                    if (!seen[0] || !seen[1]) {
                        continue;
                    }
                    const float* pa = last[0];
                    const float* pb = last[1];
                    // Camera: the game's camera (0x79efb0: slot [0x3259cf8] of the table after it)
                    // orbits Wander; its yaw (+0x1770, radians) is atan2(dx, dz) of where it looks,
                    // its position +0x1740. It is turned through the mouse-look input toward the
                    // fight's midpoint (mouse right lowers the yaw), so it moves as smoothly as
                    // when a player turns it.
                    // With a fourth value 2 the camera is ours instead (re/cvc/camera_patch.py copies
                    // the pose at cave 0x1593ee0 over the game's current one each frame): a
                    // broadcast view slowly circling the fight's midpoint, far enough back and high
                    // enough above it for both colossi, eased toward where it should be.
                    if (steer == 2.0f) {
                        static float pose[2][20];
                        *reinterpret_cast<volatile u64*>(image + 0x1593ee8) =
                            reinterpret_cast<u64>(&CvcRayThunk); // line tests (camera_patch.py)
                        static int which = 0;
                        static float cam[3]{}, look[3]{}, angle = 0.0f;
                        // Farthest the camera may be from the fight in the direction of its last
                        // blocked line (line 37), until that line is clear again.
                        static float cap = 1e9f, cap_dir[3]{};
                        static bool placed = false;
                        // After a knockout (winner byte 0x1593efb: 1 A, 2 B) the loser's death is the
                        // picture: the view favours it while the winner walks off.
                        const u8 winner = *reinterpret_cast<const volatile u8*>(image + 0x1593efb);
                        float mid[3]{(pa[0] + pb[0]) / 2, std::max(pa[1], pb[1]), (pa[2] + pb[2]) / 2};
                        float sep = std::hypot(pa[0] - pb[0], pa[2] - pb[2]);
                        if (winner == 1 || winner == 2) {
                            // On the body, drawn at most 12 units toward the winner, framed tight.
                            const float* loser_at = winner == 1 ? pb : pa;
                            const float* winner_at = winner == 1 ? pa : pb;
                            const float pull = std::min(0.25f * sep, 12.0f) / std::max(sep, 0.001f);
                            mid[0] = loser_at[0] + (winner_at[0] - loser_at[0]) * pull;
                            mid[1] = loser_at[1];
                            mid[2] = loser_at[2] + (winner_at[2] - loser_at[2]) * pull;
                            sep = std::min(sep, 80.0f);
                        }
                        const auto wrap = [](float a) {
                            while (a > 3.14159265f) {
                                a -= 6.2831853f;
                            }
                            while (a < -3.14159265f) {
                                a += 6.2831853f;
                            }
                            return a;
                        };
                        // Fallback: high above the fight and close in, steeper the further apart
                        // they are, square to the line between them (a canyon's cliffs and rock
                        // pillars stay beside and below such a camera).
                        float radius = std::max(dist, 0.5f * sep + 20.0f);
                        float rise = std::max(height, 0.8f * sep + 40.0f);
                        float target_angle = std::atan2(pb[2] - pa[2], pb[0] - pa[0]) + 1.5707963f;
                        // Better, a broadcast angle checked with the game's own camera collision:
                        // lines from just above the fight out to 12 spots around it (every 30
                        // degrees from the current angle) at that distance and height. The camera
                        // turns toward the nearest clear one and, while its own line is blocked,
                        // comes in in front of the obstacle (as the game's camera does). Hits on
                        // the colossi themselves do not count.
                        static float clear_part[13]; // of each line, up to the first hit (12: the overhead view)
                        // Whether the colossi can be seen from each spot (from the camera itself for
                        // the current angle): lines from there to both (after a knockout, the loser).
                        static bool visible[12];
                        // The ground under each spot (a line 300 down), above the fight's floor: the rock
                        // pillars on the rim of Argus's courtyard have no collision for the lines across,
                        // so a spot above that rim reads as clear while its rocks fill the picture. Over
                        // ground well above the fight's floor (a rim, a cliff top), far below it (the chasm
                        // around Argus's fortress, pillars rising out of it) or none at all (outside the
                        // world as loaded) is no spot to film from.
                        static float ground[12];
                        static float tested_angle = 0.0f;
                        static bool tested = false;
                        const auto offset = [](int k) { return (k <= 6 ? k : k - 12) * 0.5235988f; };
                        const float centre[3]{mid[0], mid[1] + 20.0f, mid[2]};
                        // Close (the arena is foggy): both in view while fighting, the body after.
                        const float reach = winner == 1 || winner == 2
                                                ? std::max(dist, 60.0f)
                                                : std::clamp(1.3f * sep + 50.0f, dist, 170.0f); // both whole in the picture, not out of the arena
                        if (sep < 200.0f) {
                            // They have closed in: the fight has begun (cvc_arena.sh FILM starts here).
                            *reinterpret_cast<volatile u8*>(image + 0x1593b60) = 1;
                        }
                        // Spot height above the centre; low when the view from above is blocked (the
                        // fight under the rock arch), where only spots beside them can see in.
                        static bool low = false;
                        const float lift = (0.45f * reach - 10.0f) * (low ? 0.3f : 1.0f);
                        auto& rays = g_cvc_rays;
                        if (rays.state.load() == 2) {
                            for (int k = 0; k < 13; k++) {
                                const float *f = rays.from[k], *t = rays.to[k], *h = rays.hit[k];
                                bool on_colossus = false;
                                for (const float* p : {pa, pb}) {
                                    on_colossus |= std::hypot(h[0] - p[0], h[2] - p[2]) < 25.0f &&
                                                   h[1] < p[1] + 80.0f;
                                }
                                const float part = std::hypot(h[0] - f[0], h[1] - f[1], h[2] - f[2]) /
                                                   std::max(std::hypot(t[0] - f[0], t[1] - f[1], t[2] - f[2]), 0.001f);
                                clear_part[k] = on_colossus ? 1.0f : std::clamp(part, 0.0f, 1.0f);
                            }
                            static int reports = 0;
                            if (reports++ % 60 == 0) {
                                LOG_INFO(Core, "cvccam: lines clear {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} "
                                         "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} (angle {:.2f}, reach {:.0f}), ground {}",
                                         clear_part[0], clear_part[1], clear_part[2], clear_part[3],
                                         clear_part[4], clear_part[5], clear_part[6], clear_part[7],
                                         clear_part[8], clear_part[9], clear_part[10], clear_part[11],
                                         tested_angle, reach, [&] {
                                             std::string g;
                                             for (const float y : ground) {
                                                 g += y < -1e8f ? " -" : fmt::format(" {:.0f}", y);
                                             }
                                             return g;
                                         }());
                                LOG_INFO(Core, "cvccam: line filter {:#x}",
                                         *reinterpret_cast<const volatile u64*>(image + 0x1593ed8));
                            }
                            {
                                // The camera itself (line 37): kept in front of anything between it
                                // and the fight, as the game's camera is, also while it moves.
                                const float *f = rays.from[37], *t = rays.to[37], *h = rays.hit[37];
                                const float full = std::hypot(t[0] - f[0], t[1] - f[1], t[2] - f[2]);
                                const float part = std::hypot(h[0] - f[0], h[1] - f[1], h[2] - f[2]);
                                cap = 1e9f;
                                bool on_colossus = false; // a colossus between: not a wall
                                for (const float* p : {pa, pb}) {
                                    on_colossus |= std::hypot(h[0] - p[0], h[2] - p[2]) < 25.0f &&
                                                   h[1] < p[1] + 80.0f;
                                }
                                if (!on_colossus && part < full - 1.0f && full > 1.0f) {
                                    cap = std::max(part - 8.0f, 0.0f);
                                    for (int c = 0; c < 3; c++) {
                                        cap_dir[c] = (t[c] - f[c]) / full;
                                    }
                                }
                            }
                            {
                                const float *f = rays.from[50], *t = rays.to[50], *h = rays.hit[50];
                                const bool hit = std::hypot(h[0] - t[0], h[1] - t[1], h[2] - t[2]) > 0.05f;
                                g_cvc_ground_a = hit ? h[1] : std::numeric_limits<float>::quiet_NaN();
                                static int told = 0;
                                if (told++ % 20 == 0) {
                                    LOG_INFO(Core, "cvccam: ground under A: from {:.1f} {:.1f} {:.1f} to {:.1f} hit {:.1f} {:.1f} {:.1f}",
                                             f[0], f[1], f[2], t[1], h[0], h[1], h[2]);
                                }
                                g_cvc_ground_a_at = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                                (void)f;
                            }
                            for (int k = 0; k < 12; k++) {
                                const float *f = rays.from[38 + k], *h = rays.hit[38 + k];
                                ground[k] = f[1] - h[1] < 299.0f ? h[1] - mid[1] : -1e9f;
                            }
                            for (int k = 0; k < 12; k++) {
                                visible[k] = true;
                                for (int j = 13 + 2 * k; j < 15 + 2 * k; j++) {
                                    const float *t = rays.to[j], *h = rays.hit[j];
                                    visible[k] &= std::hypot(h[0] - t[0], h[2] - t[2]) < 25.0f;
                                }
                            }
                            tested = true;
                            rays.state.store(0);
                        }
                        const float overhead_radius = radius, overhead_rise = rise;
                        if (tested) {
                            // Clear, near the current angle, and side-on to the line between them
                            // (from along it one colossus hides the other, and a blow that lands
                            // reads as a gap).
                            const float axis = std::atan2(pb[2] - pa[2], pb[0] - pa[0]);
                            // After a knockout: from the side away from the winner, which would
                            // otherwise stand between the camera and the body.
                            const bool after = winner == 1 || winner == 2;
                            const float* winner_at = winner == 1 ? pa : pb;
                            const float away = std::atan2(winner_at[2] - mid[2], winner_at[0] - mid[0]);
                            const auto score = [&](int k) {
                                const float at = tested_angle + offset(k);
                                // A fully clear line counts double: a spot parked in front of rock
                                // shows that rock in the corner of the picture.
                                return 2.0f * clear_part[k] - 0.2f * std::abs(offset(k)) -
                                       (after ? 0.8f * std::cos(at - away) : 1.5f * std::abs(std::cos(at - axis))) -
                                       (visible[k] ? 0.0f : 1.5f) - (ground[k] > -40.0f && ground[k] < 12.0f ? 0.0f : 1.5f);
                            };
                            int pick = 0;
                            for (int k = 1; k < 12; k++) {
                                if (score(k) > score(pick)) {
                                    pick = k;
                                }
                            }
                            target_angle = tested_angle + offset(pick);
                            low = clear_part[12] < 0.6f;
                            if (low) {
                                // Under cover: beside the fight, in front of whatever its own line meets.
                                const float part = std::max(clear_part[0] - 25.0f / reach, 0.3f);
                                radius = reach * part, rise = 20.0f + lift * part;
                            } else if (clear_part[0] >= 0.6f && visible[0]) {
                                // In front of whatever blocks the far end of its own line.
                                const float part = clear_part[0] - 25.0f / reach; // 25 units off the hit
                                radius = reach * part, rise = 20.0f + lift * part;
                            } else {
                                // Rock close around this angle, or between it and the colossi: the
                                // overhead view (on its own line, in front of anything above) until a
                                // clear angle is reached.
                                const float part = std::max(clear_part[12] - 0.05f, 0.3f);
                                radius = overhead_radius * part, rise = 20.0f + (overhead_rise - 20.0f) * part;
                            }
                        }
                        static int queue_tick = 0; // twenty rounds a second: 76 line tests each
                        if (++queue_tick % 3 == 0 && rays.state.load() == 0) {
                            for (int k = 0; k < 12; k++) {
                                const float at = angle + offset(k);
                                const float to[4]{centre[0] + reach * std::cos(at), centre[1] + lift,
                                                  centre[2] + reach * std::sin(at), 1.0f};
                                std::memcpy(rays.from[k], centre, 12), rays.from[k][3] = 1.0f;
                                std::memcpy(rays.to[k], to, 16);
                            }
                            const float over[4]{mid[0] + overhead_radius * std::cos(angle), mid[1] + overhead_rise,
                                                mid[2] + overhead_radius * std::sin(angle), 1.0f};
                            std::memcpy(rays.from[12], centre, 12), rays.from[12][3] = 1.0f;
                            std::memcpy(rays.to[12], over, 16);
                            // Sight lines: from each spot (the camera itself for this angle) to the
                            // colossi, 10 units up their bodies.
                            const float* loser_at = winner == 1 ? pb : winner == 2 ? pa : nullptr;
                            const float* subjects[2]{loser_at ? loser_at : pa, loser_at ? loser_at : pb};
                            for (int k = 0; k < 12; k++) {
                                const float at = angle + offset(k);
                                const float spot[4]{k == 0 ? cam[0] : centre[0] + reach * std::cos(at),
                                                    k == 0 ? cam[1] : centre[1] + lift,
                                                    k == 0 ? cam[2] : centre[2] + reach * std::sin(at), 1.0f};
                                for (int j = 0; j < 2; j++) {
                                    const float target[4]{subjects[j][0], subjects[j][1] + 10.0f, subjects[j][2], 1.0f};
                                    std::memcpy(rays.from[13 + 2 * k + j], spot, 16);
                                    std::memcpy(rays.to[13 + 2 * k + j], target, 16);
                                }
                            }
                            std::memcpy(rays.from[37], centre, 12), rays.from[37][3] = 1.0f;
                            std::memcpy(rays.to[37], cam, 12), rays.to[37][3] = 1.0f;
                            for (int k = 0; k < 12; k++) { // ground under each spot
                                std::memcpy(rays.from[38 + k], rays.from[13 + 2 * k], 16);
                                std::memcpy(rays.to[38 + k], rays.from[13 + 2 * k], 16);
                                rays.to[38 + k][1] -= 300.0f;
                            }
                            { // the ground under A (its feet: the object's position)
                                const float from[4]{pa[0], pa[1] + 0.5f, pa[2], 1.0f};
                                const float to[4]{pa[0], pa[1] - 40.0f, pa[2], 1.0f};
                                std::memcpy(rays.from[50], from, 16), std::memcpy(rays.to[50], to, 16);
                            }
                            rays.count = 51, rays.image = image, tested_angle = angle;
                            rays.state.store(1);
                        }
                        angle = placed ? angle + wrap(target_angle - angle) * 0.03f : target_angle;
                        // Eased around the fight (angle, distance, height) rather than in a straight
                        // line: a change of view swings round or comes in, it does not cut through the
                        // rock between two spots. In quickly, out slowly.
                        static float reach_now = 0.0f, rise_now = 0.0f;
                        if (!placed) {
                            reach_now = radius, rise_now = rise;
                        } else {
                            reach_now += (radius - reach_now) * (radius < reach_now ? 0.15f : 0.03f);
                            rise_now += (rise - rise_now) * (rise < rise_now ? 0.1f : 0.03f);
                        }
                        cam[0] = mid[0] + reach_now * std::cos(angle), cam[1] = mid[1] + rise_now;
                        cam[2] = mid[2] + reach_now * std::sin(angle);
                        const float want_look[3]{mid[0], mid[1] + 6.0f, mid[2]};
                        const float ease = placed ? 0.03f : 1.0f;
                        for (int k = 0; k < 3; k++) {
                            look[k] += (want_look[k] - look[k]) * ease;
                        }
                        placed = true;
                        // One second on: the picture has settled (cvc_arena.sh lifts the loading
                        // screen then).
                        // Counted only once the fight is placed and while no cutscene plays.
                        static int settled = 0;
                        settled = cvc_placed && !Sotc::Screens::Letterboxed() ? settled + 1 : 0;
                        if (settled == 60) {
                            *reinterpret_cast<volatile u64*>(image + 0x1593ae8) = 1;
                        }
                        // Never inside or against a colossus: at least 22 units from every bone. Eased
                        // out (a blow can throw a body at the eye: jumping clear read as a cut).
                        const float free_cam[3]{cam[0], cam[1], cam[2]};
                        for (int pass = 0; pass < 2; pass++) {
                            for (const auto& b : body_points) {
                                const float d[3]{cam[0] - b[0], cam[1] - b[1], cam[2] - b[2]};
                                const float length = std::hypot(d[0], d[1], d[2]);
                                if (length < 22.0f) {
                                    const float up = length > 0.001f ? 22.0f / length : 0.0f;
                                    for (int c = 0; c < 3; c++) {
                                        cam[c] = length > 0.001f ? b[c] + d[c] * up : cam[c] + (c == 1 ? 22.0f : 0.0f);
                                    }
                                }
                            }
                        }
                        {
                            static float clear_now[3]{};
                            for (int c = 0; c < 3; c++) {
                                clear_now[c] += (cam[c] - free_cam[c] - clear_now[c]) * 0.12f;
                                cam[c] = free_cam[c] + clear_now[c];
                            }
                        }
                        {
                            const float d[3]{cam[0] - centre[0], cam[1] - centre[1], cam[2] - centre[2]};
                            const float length = std::hypot(d[0], d[1], d[2]);
                            const float along = (d[0] * cap_dir[0] + d[1] * cap_dir[1] + d[2] * cap_dir[2]) /
                                                std::max(length, 0.001f);
                            if (length > cap && along > 0.7f) {
                                for (int c = 0; c < 3; c++) {
                                    cam[c] = centre[c] + d[c] * (cap / length);
                                }
                                // The eased distance and height follow, so it does not push back out.
                                reach_now = std::hypot(cam[0] - mid[0], cam[2] - mid[2]);
                                rise_now = cam[1] - mid[1];
                            }
                        }
                        // A heavy blow shakes the view: the eye and, less, the point looked at move
                        // by a few overlapping wobbles that die away in about half a second.
                        float eye[3]{cam[0], cam[1], cam[2]}, at[3]{look[0], look[1], look[2]};
                        {
                            const float t = std::chrono::duration<float>(
                                std::chrono::steady_clock::now().time_since_epoch()).count() -
                                g_cvc_shake_at.load();
                            const float amount = t >= 0.0f && t < 1.5f ? g_cvc_shake.load() * std::exp(-t / 0.22f) : 0.0f;
                            if (amount > 0.01f) {
                                const float w[3]{std::sin(t * 61.0f) + 0.5f * std::sin(t * 97.0f + 1.3f),
                                                 std::sin(t * 73.0f + 0.7f) + 0.5f * std::sin(t * 113.0f),
                                                 std::sin(t * 67.0f + 2.1f) + 0.5f * std::sin(t * 89.0f + 0.4f)};
                                for (int c = 0; c < 3; c++) {
                                    eye[c] += amount * w[c];
                                    at[c] += amount * 0.4f * w[(c + 1) % 3];
                                }
                            }
                        }
                        float dir[3]{at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
                        const float length = std::max(std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] +
                                                                dir[2] * dir[2]), 0.001f);
                        for (float& c : dir) {
                            c /= length;
                        }
                        for (int c = 0; c < 3; c++) {
                            g_cvc_cam_now[c] = eye[c], g_cvc_cam_now[3 + c] = at[c];
                        }
                        float* out = pose[which ^= 1];
                        const float values[15]{eye[0], eye[1], eye[2], 1.0f, at[0], at[1], at[2], 1.0f,
                                               dir[0], dir[1], dir[2], 0.0f, std::atan2(dir[0], dir[2]),
                                               std::asin(dir[1]), length};
                        std::memcpy(out, values, sizeof(values));
                        // [16..19]: up, so the picture never rolls with the game's camera shake.
                        out[16] = 0.0f, out[17] = 1.0f, out[18] = 0.0f, out[19] = 0.0f;
                        *reinterpret_cast<float* volatile*>(image + 0x1593ee0) = out;
                    }
                    {
                        const s32 slot = *reinterpret_cast<const volatile s32*>(image + 0x3259cf8);
                        const u64 camera = slot >= 0 && slot < 8 ? q(image + 0x3259d00 + slot * 8ull) : 0;
                        if (camera && steer == 1.0f) {
                            const auto* cp = reinterpret_cast<const volatile float*>(camera + 0x1740);
                            const float yaw = *reinterpret_cast<const volatile float*>(camera + 0x1770);
                            const float want = std::atan2((pa[0] + pb[0]) / 2 - cp[0],
                                                          (pa[2] + pb[2]) / 2 - cp[2]);
                            float error = want - yaw;
                            while (error > 3.14159265f) {
                                error -= 6.2831853f;
                            }
                            while (error < -3.14159265f) {
                                error += 6.2831853f;
                            }
                            // Near half a turn the error's sign flips back and forth: keep turning
                            // the way already chosen until the error is small.
                            static float turning = 0.0f;
                            if (std::abs(error) > 2.0f && turning != 0.0f) {
                                error = std::copysign(std::abs(error), -turning);
                            }
                            // Small stick values fall in the game's dead zone: at least 8 counts.
                            const float push = std::clamp(-error * 40.0f, -35.0f, 35.0f);
                            turning = std::abs(error) < 0.04f ? 0.0f : push;
                            Input::test_mouse_delta[0] =
                                std::abs(error) < 0.04f ? 0.0f
                                                        : std::copysign(std::max(std::abs(push), 8.0f), push);
                        }
                    }
                    if (++tick % 30 != 0) {
                        continue;
                    }
                    const s32 handle = *reinterpret_cast<const volatile s32*>(world + 0x228);
                    const u64 node = (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff);
                    const u64 player = node ? q(node) : 0;
                    if (!player) {
                        continue;
                    }
                    auto* pp = reinterpret_cast<float*>(player + 0x130);
                    const float mx = (pa[0] + pb[0]) / 2, my = std::max(pa[1], pb[1]),
                                mz = (pa[2] + pb[2]) / 2;
                    const float d = std::hypot(pp[0] - mx, pp[2] - mz);
                    if (tick % 120 == 0) {
                        const s32 slot = *reinterpret_cast<const volatile s32*>(image + 0x3259cf8);
                        const u64 camera = slot >= 0 && slot < 8 ? q(image + 0x3259d00 + slot * 8ull) : 0;
                        const auto* cp = camera ? reinterpret_cast<const float*>(camera + 0x1740) : pp;
                        LOG_INFO(Core, "cvccam: Wander {:.0f} from the fight, yaw {:.2f}, mouse {:.1f}; "
                                 "A {:.0f} {:.0f} {:.0f} B {:.0f} {:.0f} {:.0f} W {:.0f} {:.0f} {:.0f} "
                                 "cam {:.0f} {:.0f} {:.0f}", d,
                                 camera ? *reinterpret_cast<const float*>(camera + 0x1770) : 0.0f,
                                 Input::test_mouse_delta[0], pa[0], pa[1], pa[2], pb[0], pb[1], pb[2],
                                 pp[0], pp[1], pp[2], cp[0], cp[1], cp[2]);
                    }
                    // Far enough back for both to fit in the picture.
                    const float want_dist = std::max(dist, 0.9f * std::hypot(pa[0] - pb[0], pa[2] - pb[2]));
                    // With our own camera (2) he is only put out of the way once: moving him again
                    // later made the game re-create a colossus's object (B gone in runs 26 and 28).
                    static bool moved_once = false;
                    if ((d > 0.5f * want_dist && d < 2.0f * want_dist) || (steer == 2.0f && moved_once)) {
                        continue;
                    }
                    moved_once = true;
                    // Along the line from the fight to where Wander already stands: open ground he
                    // reached himself, where a spot square to the colossi may be inside a cliff.
                    float px = pp[0] - mx, pz = pp[2] - mz;
                    const float len = std::hypot(px, pz);
                    if (len < 1.0f) {
                        px = pb[2] - pa[2], pz = pa[0] - pb[0];
                    }
                    const float norm = std::max(std::hypot(px, pz), 0.001f);
                    px /= norm, pz /= norm;
                    float target[3]{mx + side * px * want_dist, std::max(pp[1], my) + height,
                                    mz + side * pz * want_dist};
                    if (steer == 2.0f) {
                        // Our own camera: Wander only has to be out of the way. A spot worked out
                        // from the fight can be above a hole (he fell through the ground, then the
                        // game re-created a colossus); this ledge of Basaran's arena held in every run.
                        // Elsewhere he stays where he arrived (open ground, away from the host).
                        if (std::hypot(737.0f - mx, -990.0f - mz) > 400.0f) {
                            continue;
                        }
                        target[0] = 737.0f, target[1] = 92.0f, target[2] = -990.0f;
                    }
                    const float delta[3]{target[0] - pp[0], target[1] - pp[1], target[2] - pp[2]};
                    const u32 variant = MoveCharacter(player, delta);
                    LOG_INFO(Core, "cvccam: Wander to {:.0f} {:.0f} {:.0f} (was {:.0f} from the fight, "
                             "variant {})", target[0], target[1], target[2], d, variant);
                }
                Input::test_mouse_delta[0] = 0.0f;
                if (steer == 2.0f) {
                    *reinterpret_cast<volatile u64*>(image + 0x1593ee0) = 0; // camera back to the game
                    *reinterpret_cast<volatile u64*>(image + 0x1593ee8) = 0; // no more line tests
                }
            }).detach();
        } else if (action == "cvcbones") {
            // Test: the segments the contact test (re/cvc/contact_patch.py) tries against the
            // other colossus's attack surfaces: each colossus's skeleton as a chain of segments
            // between bones (ids index its world matrices, object+0x120 -> +0x198, 64 bytes each,
            // translation at +0x30; see 0x616470) plus feet (-2) to the highest bone. The table
            // is built here and its address put at cave 0x1593dc0; earlier tables stay allocated
            // as the game may be reading one.
            struct Segment {
                u64 victim;
                s32 bone_a, bone_b;
                u64 attacker_list;
                u64 hit_counter;
            };
            static std::vector<std::unique_ptr<std::vector<Segment>>> tables;
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            auto table = std::make_unique<std::vector<Segment>>();
            for (const u64 slot : {0x1593b08ull, 0x1593b10ull}) {
                const u64 object = image ? *reinterpret_cast<const u64*>(image + slot) : 0;
                if (!object) {
                    continue;
                }
                const u64 matrices = *reinterpret_cast<const u64*>(object + 0x120 + 0x198);
                const auto* pos = reinterpret_cast<const float*>(object + 0x130);
                int top = -2, count = 0;
                float top_y = pos[1];
                for (int i = 0; matrices && i < 512; i++) {
                    float m[4]{};
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(),
                                           reinterpret_cast<const void*>(matrices + i * 64 + 0x30), m,
                                           sizeof(m), &got) ||
                        m[3] != 1.0f || std::hypot(m[0] - pos[0], m[2] - pos[2]) > 400.0f) {
                        break;
                    }
                    count++;
                    if (m[1] > top_y) {
                        top_y = m[1], top = i;
                    }
                }
                // A's attacker is B: the list holding B (0x1593b20), hits counted as by B.
                const bool is_a = slot == 0x1593b08;
                const u64 list = image + (is_a ? 0x1593b20 : 0x1593b24);
                const u64 counter = image + (is_a ? 0x1593ecc : 0x1593ec8);
                table->push_back({object, -2, top, list, counter});
                const int step = std::max(1, count / 40);
                for (int i = 0; i + step < count; i += step) {
                    table->push_back({object, i, i + step, list, counter});
                }
                LOG_INFO(Core, "cvcbones: object {:#x}: {} bones, highest {} at {:.0f} above its feet",
                         object, count, top, top_y - pos[1]);
            }
            // Check of the test itself: Wander (first of list world+0x228) with the bones his own
            // damage test uses (0x71d8e5: ids at *(0x24c0c78)+0x10 and *(0x24c0c70)+0x10) against
            // colossus A's attacks, counted at cave 0x1593ed0.
            if (image) {
                const u64 world = *reinterpret_cast<const u64*>(image + 0x15fe570);
                const u64 base = *reinterpret_cast<const u64*>(image + 0x2f3b8c0);
                const s32 handle = world ? *reinterpret_cast<const s32*>(world + 0x228) : 0;
                const u64 node = handle ? (handle < 0 ? base : 0) + (u32(handle) & 0x7fffffff) : 0;
                const u64 player = node ? *reinterpret_cast<const u64*>(node) : 0;
                const u64 id_a = *reinterpret_cast<const u64*>(image + 0x24c0c78);
                const u64 id_b = *reinterpret_cast<const u64*>(image + 0x24c0c70);
                if (player && id_a && id_b) {
                    const s32 bone_a = *reinterpret_cast<const s32*>(id_a + 0x10);
                    const s32 bone_b = *reinterpret_cast<const s32*>(id_b + 0x10);
                    table->push_back({player, bone_a, bone_b, image + 0x1593b24, image + 0x1593ed0});
                    LOG_INFO(Core, "cvcbones: Wander {:#x} bones {:#x} {:#x}, hits so far {}", player,
                             bone_a, bone_b, *reinterpret_cast<const u32*>(image + 0x1593ed0));
                }
            }
            table->push_back({});
            *reinterpret_cast<volatile u64*>(image + 0x1593dc0) = reinterpret_cast<u64>(table->data());
            LOG_INFO(Core, "cvcbones: {} segments", table->size() - 1);
            tables.push_back(std::move(table));
        } else if (action.starts_with("cvcbrawl=")) {
            // Colossus vs colossus fighting with their own moves. "cvcbrawl=300:20:15:12:60:1200":
            // for 300 s; within reach a colossus plays one of its own attacks (see Moves below), and
            // during it a bone of the attacker moving faster than 60 units/s that comes within 12
            // units of any bone of the other is the blow: A's takes 20 off B's manager health
            // (0x1593e08 -> +0x50), B's 15 off A's (0x1593e00); below 1, a share of full health.
            // (The last value, once a per-attacker cooldown, is no longer used.) Bones: the world
            // matrices of object+0x120 -> +0x198 (64 bytes, translation at +0x30). Counts blows like
            // cvchits= (0x1593ec8 by A, 0x1593ecc by B).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            float seconds = 0, dmg_a = 0, dmg_b = 0, radius = 0, speed = 0, cooldown = 0;
            sscanf(action.c_str() + 9, "%f:%f:%f:%f:%f:%f", &seconds, &dmg_a, &dmg_b, &radius, &speed,
                   &cooldown);
            std::thread([image, seconds, dmg_a, dmg_b, radius, speed] {
                const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
                using Clock = std::chrono::steady_clock;
                const auto end = Clock::now() + std::chrono::milliseconds(u64(seconds * 1000));
                using Points = std::vector<std::array<float, 3>>;
                // A colossus's bone positions; empty while its matrices are not where it is.
                const auto bones = [&](u64 object, Points& out) {
                    out.clear();
                    const u64 matrices = q(object + 0x120 + 0x198);
                    const auto* pos = reinterpret_cast<const volatile float*>(object + 0x130);
                    for (int i = 0; matrices && i < 256; i++) {
                        float m[4]{};
                        SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(),
                                               reinterpret_cast<const void*>(matrices + i * 64 + 0x30),
                                               m, sizeof(m), &got)) {
                            break;
                        }
                        // (A matrix that is no bone's, or far off, is passed over: stopping there left
                        // Phaedra's list empty, its distance to the other unknown.)
                        if (m[3] != 1.0f || std::hypot(m[0] - pos[0], m[2] - pos[2]) > 400.0f) {
                            if (i >= 2 * Sotc::CvcPhysics::MaxBones) {
                                break;
                            }
                            continue;
                        }
                        out.push_back({m[0], m[1], m[2]});
                    }
                };
                const auto distance = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
                    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                                     (a[2] - b[2]) * (a[2] - b[2]));
                };
                Points now[2], before[2];
                auto last = Clock::now();
                while (Clock::now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    const u64 objects[2]{q(image + 0x1593b08), q(image + 0x1593b10)};
                    const u64 managers[2]{q(image + 0x1593e00), q(image + 0x1593e08)};
                    if (!objects[0] || !objects[1] || !managers[0] || !managers[1]) {
                        continue;
                    }
                    const float dt = std::chrono::duration<float>(Clock::now() - last).count();
                    last = Clock::now();
                    static int ticks = 0;
                    if (++ticks % 120 == 0) {
                        const auto* pa = reinterpret_cast<const volatile float*>(objects[0] + 0x130);
                        const auto* pb = reinterpret_cast<const volatile float*>(objects[1] + 0x130);
                        LOG_INFO(Core, "cvcbrawl: A frame {:.1f} at {:.0f} {:.0f}, B frame {:.1f} at {:.0f} {:.0f}, holds {:#x} {:#x} "
                                 "(scale {:.2f} {:.2f})",
                                 g_cvc_body[0].frame.load(), pa[0], pa[2], g_cvc_body[1].frame.load(), pb[0], pb[2],
                                 g_cvc_hold_obj[0], g_cvc_hold_obj[1], g_cvc_hold_scale[0], g_cvc_hold_scale[1]);
                    }
                    if (ticks % 600 == 0) {
                        LOG_INFO(Core, "cvcbrawl: motions A {:#x} B {:#x}, health A {:.0f} B {:.0f}",
                                 q(objects[0] + 0x380) - image, q(objects[1] + 0x380) - image,
                                 *reinterpret_cast<const float*>(managers[0] + 0x50),
                                 *reinterpret_cast<const float*>(managers[1] + 0x50));
                    }
                    for (int k = 0; k < 2; k++) {
                        before[k].swap(now[k]);
                        bones(objects[k], now[k]);
                    }
                    // Their own moves, per colossus (static MotionKind records; ms: the longest one
                    // lasts). Once the fight is placed both are kept out of their AI (caves
                    // 0x1593ae0 A, 0x1593b48 B; contact_patch.py), so all they do is these, each one
                    // of their own animations: standing, turning on the spot, walking up, an attack;
                    // hit, a flinch chosen by where the blow landed and, from a heavy blow, stepping
                    // back, with a push that dies away (CvcBodyWork).
                    struct Moves {
                        std::vector<std::pair<u64, int>> attacks;
                        std::vector<u64> front, high, back; // flinches: hit from the front, on the head, from behind
                        float reach = 0.0f;   // closes in on the other from at most this far
                        u64 walk = 0;         // to close in
                        float strike = 28.0f; // nearest bones of the two at most this far to strike
                        int forward = 1;      // root bone axis it faces along (0 x, 1 y, 2 z), negative: -axis
                        u64 turn_left = 0, turn_right = 0; // its turns on the spot
                        float swing = 0.0f;   // a striking bone at least this fast (units/s); 0: the set speed
                        u64 idle = 0;         // standing
                        u64 retreat = 0;      // stepping back
                        float mass = 1.0f;    // how hard blows push it (more: less)
                        float aim = 0.95f;    // how squarely (cosine) it faces the other to strike
                        float body = 8.0f;    // its body's reach around its bones (contact)
                        // Holds its ground: out of reach, it waits for the other to come to it (Basaran,
                        // once brought out into the field, cannot walk far from where it was put).
                        bool holds = false;
                        u64 strike_left = 0, strike_right = 0; // attacks to one side (the rest straight ahead)
                        // An attack's next part, played when the part before ends (its AI would pick it):
                        // ended on its own last frame the pose snaps (Gaius's sword) to whatever follows.
                        std::unordered_map<u64, u64> follow;
                        bool pulse = false; // fires its electric pulse (Basaran)
                        // Knocked down by a heavy blow: its own fall and recovery, each part played to its
                        // end (a part given with ms: held that long, e.g. lying in a loop).
                        std::vector<std::pair<u64, int>> down;
                        std::vector<u64> idles; // standing variants, now and then
                        // How hard its blows land for the speed of the striking bone (its mass behind
                        // them): Basaran's slow, heavy slams count as much as Gaius's fast sword.
                        float heft = 1.0f;
                    };
                    static const std::unordered_map<int, Moves> kinds{
                        // Gaius: Knight_attack_1 (slam), _2; chest / belly, head / face, back / neck
                        // damage; Knight_walk; Knight_Standing_left/right_rotation; Knight_Standing;
                        // Knight_retreat.
                        // (The slam three times in four: the sweep, made against Wander, mostly passes
                        // beside or under a colossus's own collision.)
                        {2, {{{0x1d3a5f0, 4500}, {0x1d3a5f0, 4500}, {0x1d3a5f0, 4500}, {0x1d3b620, 6000}},
                             {0x1d3cd40, 0x1d3d1e0}, {0x1d3c650, 0x1d3c8a0}, {0x1d3cf90, 0x1d3caf0},
                             // (strike 3: its slam met the other's own collision from 2-5 apart, passed
                             // short from 8; it steps in to there)
                             250.0f, 0x1d38340, 3.0f, 1, 0x1d37c50, 0x1d37a00, 0.0f, 0x1d377b0, 0x1d387e0, 1.0f, 0.85f, 8.0f, false, 0, 0,
                             // attack_1_Introduction > attack_one_stop_(success); attack_2_Introduction >
                             // _loop > _quit
                             {{0x1d3a5f0, 0x1d3a840}, {0x1d3b620, 0x1d3b870}, {0x1d3b870, 0x1d3bac0}}, false,
                             // (no knockdown: Knight_fall's own effects crash the game's effect update,
                             // 0x693f57); Knight_Search
                             {}, {0x1d38ed0}}}, // a wide sword sweep
                        // Basaran: hand attacks (slow slams); Turtle_shudder; Turtle_walking_1;
                        // Turtle_Standing_left/right_rotation_1; Turtle_Standing; no stepping back (out
                        // in the field it is held to its spot: Turtle_retreat_1 slid it off, then the game
                        // pulled it back in a snap).
                        // (Its slam opens with both_hands_attack_1 only: hands_attack_2 (middle), its
                        // second part, begins reared up and started from a stand it jumps there.)
                        {8, {{{0x1d59230, 3500}},
                             {0x1d5a4b0}, {0x1d5a4b0}, {0x1d5a4b0},
                             // (aim 0.8: it strikes with the other up to 37 degrees aside, its side slams)
                             250.0f, 0x1d553c0, 3.0f, -2, 0x1d54f20, 0x1d54a80, 30.0f, 0x1d51e90, 0, 1.7f, 0.8f, 10.0f, true, 0x1d59dc0, 0x1d59920,
                             // both_hands_attack_1 > hands_attack_2 (middle); each hands_attack_2 > its _3
                             {{0x1d59230, 0x1d59480}, {0x1d59480, 0x1d596d0}, {0x1d59920, 0x1d59b70},
                              {0x1d59dc0, 0x1d5a010}}, true,
                             // Turtle_overturn_the_beginning_(right) > turned_over_the_first_two > turned_over >
                             // overturned_loop (3 s on its back) > flip_over_stop_1 > _2; Turtle_Standing_1, _2
                             {{0x1d56640, 0}, {0x1d56890, 0}, {0x1d56f80, 0}, {0x1d571d0, 3000}, {0x1d57420, 0}, {0x1d57670, 0}},
                             {0x1d545e0, 0x1d54830}, 4.0f}},
                        // Phaedra (Kirin): right / left foot attack_1 (stomps); chest (before), head
                        // (small), chest behind right / left damage; Kirin_walking_loop; Standing left /
                        // right rotation; Kirin_Tachi (standing); Kirin_recession. Its root faces along -z
                        // (re/cvc forward axis: the high end of its body). No fall (untried: Gaius's own
                        // crashed the game's effect update).
                        {3, {{{0x1ce3c10, 4000}, {0x1ce4c40, 4000}},
                             {0x1ce9640}, {0x1ce93f0}, {0x1ce9890, 0x1ce9ae0},
                             250.0f, 0x1ce2990, 6.0f, -2, 0x1ce24f0, 0x1ce22a0, 0.0f, 0x1ce1e00, 0x1ce32d0, 1.0f, 0.85f, 8.0f, false, 0, 0,
                             {}, false, {}, {}, 1.5f}},
                        // Valus (Minotaur): right / left foot attack (stomps), club_attack_2A (its club
                        // brought down, 2A > 2B > 2C > 2D > 2E); right / left chest, head, back damage;
                        // walking_loop_1; Standing left / right rotation; Standing; recession. Its root
                        // faces along +y (re/cvc: the way it walks). No fall (untried).
                        {0, {{{0x1d07bd0, 4000}, {0x1d07e20, 4000}, {0x1d08070, 7000}},
                             {0x1d0d3b0, 0x1d0d600}, {0x1d0ccc0}, {0x1d0dcf0},
                             250.0f, 0x1d06ba0, 4.0f, 1, 0x1d06700, 0x1d064b0, 0.0f, 0x1d06010, 0x1d074e0, 1.2f, 0.85f, 8.0f, false, 0, 0,
                             {{0x1d08070, 0x1d08510}, {0x1d08510, 0x1d08760}, {0x1d08760, 0x1d089b0}, {0x1d089b0, 0x1d08e50}},
                             false, {}, {}, 1.4f}},
                        // Argus (Minotaur_C): as Valus, its own set; heart damage among the front ones.
                        {14, {{{0x1cf04e0, 4000}, {0x1cf0730, 4000}, {0x1cf1510, 7000}},
                              {0x1cf7d20, 0x1cf7f70}, {0x1cf7630}, {0x1cf8660},
                              250.0f, 0x1cef010, 6.0f, 1, 0x1ceeb70, 0x1cee920, 0.0f, 0x1cee480, 0x1cefdf0, 3.0f, 0.85f, 10.0f, false, 0, 0,
                              {{0x1cf1510, 0x1cf19b0}, {0x1cf19b0, 0x1cf1c00}, {0x1cf1c00, 0x1cf1e50}, {0x1cf1e50, 0x1cf20a0}},
                              false, {}, {}, 2.0f}},
                        // Quadratus (Boss_A): right arm attack started > attack > stop, left arm attack >
                        // stop (foreleg stomps); no damage motions: its shudder, head swing for the head;
                        // walk; left / right rotation; Standing; recession. Its root faces along -z.
                        // (The right stomp from its strike: its wind-up, right arm attack started, plays 9 s,
                        // the whole stomp 21 s.)
                        // Its head swing (a butt) two times in three: its forelegs come down 10 ahead of its
                        // root, its own head 20 ahead, so against a standing colossus a stomp only reaches
                        // what its head already touches.
                        {1, {{{0x1d63840, 3500}, {0x1d63840, 3500}, {0x1d62810, 5000}},
                             {0x1d62120}, {0x1d62120}, {0x1d62120},
                             // (strike 2.5: its head swings across where it stands, not out to the other: from 5-8
                             // apart it met Gaius at 0-4 units/s; swing 20: slower than a sword)
                             250.0f, 0x1d61590, 2.5f, -2, 0x1d60ea0, 0x1d60c50, 20.0f, 0x1d607b0, 0x1d61ed0, 1.4f, 0.85f, 8.0f, false, 0, 0,
                             {{0x1d62810, 0x1d62cb0}, {0x1d62f00, 0x1d633a0}}, false, {}, {}, 2.5f}},
                        // Cenobia (Cerberus): close range attack started (right) / beginning (left) >
                        // its attack, and (vertical); back_damage for all; walk_1_loop; Standing Fast
                        // left / right rotation; Standing; no stepping back. Its root faces along +z.
                        {13, {{{0x1cd0b00, 3000}, {0x1cd0d50, 3000}, {0x1cd1440, 3000}},
                              {0x1cd2b60}, {0x1cd2b60}, {0x1cd2b60},
                              250.0f, 0x1ccb7c0, 3.0f, 2, 0x1cceaa0, 0x1cce850, 0.0f, 0x1cc84e0, 0, 0.5f, 0.85f, 5.0f, false, 0, 0,
                              {{0x1cd0b00, 0x1cd0fa0}, {0x1cd0d50, 0x1cd11f0}}, false, {}, {}, 0.8f}},
                        // (straight: both_hands_attack_1 / hands_attack_2 (middle); to the left / right:
                        // hands_attack_2 (left) / (right))
                    };
                    const u64 table = q(image + 0x16d21f8 + 0x20);
                    const auto kind_of = [&](int k) -> const Moves* {
                        const u64 entry = q(image + (k ? 0x1593b30 : 0x1593b28));
                        const auto it = kinds.find(entry >= table ? int((entry - table) / 0x1e8) : -1);
                        return it == kinds.end() ? nullptr : &it->second;
                    };
                    enum class Act { Idle, Turn, Walk, Attack, Flinch, Retreat, Pulse, Down };
                    struct Fighter {
                        Act act = Act::Idle;
                        u64 motion = 0;
                        Clock::time_point since, until;
                        bool heavy = false; // a flinch followed by stepping back
                        bool bait = false;  // walking up to be struck (the other holds its ground)
                        size_t part = 0;    // Down: which part of its fall and recovery
                    };
                    static Fighter fighter[2];
                    static int attacking = -1;
                    static int last_attacker = int(std::random_device{}() % 2); // who goes first: either
                    static bool landed = false;
                    // Balance lost to blows (impulses add up, it drains back slowly, 0.04 a second); past 1.8 (two full blows close together)
                    // the colossus loses its footing (Basaran overturns, Gaius staggers far).
                    static float balance[2]{};
                    static Clock::time_point balance_at = Clock::now();
                    {
                        const float elapsed = std::chrono::duration<float>(Clock::now() - balance_at).count();
                        balance_at = Clock::now();
                        for (float& b : balance) {
                            b = std::max(0.0f, b - 0.04f * elapsed);
                        }
                    }
                    // Basaran's pulse: its phase (0 charging, 1 fired, 2 done), when it fired, where it
                    // struck, and a strike on the other waiting for the blow code below.
                    static int pulse_phase = 0;
                    static Clock::time_point pulse_fired;
                    static std::array<float, 3> pulse_point{};
                    static bool pulse_on_target = false;
                    static int pulse_hit_for = -1;
                    static std::array<float, 3> pulse_from{}; // where it left its mouth
                    static float pulse_drawn = 0.0f;          // how far along the bolt has been drawn (0-1)
                    static std::vector<std::array<float, 3>> landing; // (test aid, below)
                    static float nearest_swing = 1e9f, nearest_any = 1e9f; // closest a moving / any bone came (misses in the log)
                    static Clock::time_point attack_from, next_attack = Clock::now();
                    static Clock::time_point pulse_ready{}; // Basaran's pulse ready again (at most every 25 s)
                    static std::array<float, 2> attack_root{}, target_root{}; // where each stood when it began
                    static std::mt19937 rng{std::random_device{}()};
                    const bool over = *reinterpret_cast<const volatile u8*>(image + 0x1593efb) != 0;
                    const auto* ra = reinterpret_cast<const volatile float*>(objects[0] + 0x130);
                    const auto* rb = reinterpret_cast<const volatile float*>(objects[1] + 0x130);
                    // The bodies' frame work only once the fight is placed (before, B can be a stand-in
                    // the game is tearing down).
                    for (int k = 0; k < 2; k++) {
                        g_cvc_body[k].object = over || !cvc_placed ? g_cvc_body[k].object.load() : objects[k];
                    }
                    // Bodies apart: the nearest two bones.
                    float closest = 1e9f;
                    for (const auto& p : now[0]) {
                        for (const auto& t : now[1]) {
                            closest = std::min(closest, distance(p, t));
                        }
                    }
                    // How squarely k faces the other (cosine) and to which side the other is (cross
                    // product sign, > 0: to its left), from its root bone's forward axis.
                    // (Toward `target`, x z, instead when given.)
                    const auto face = [&](int k, float& cosine, float& side, const float* target = nullptr) {
                        const Moves* moves = kind_of(k);
                        const u64 mats = moves ? q(objects[k] + 0x120 + 0x198) : 0;
                        float m[12]{};
                        SIZE_T got = 0;
                        if (!mats || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(mats), m,
                                                        sizeof(m), &got)) {
                            return false;
                        }
                        const int axis = std::abs(moves->forward);
                        const float sign = moves->forward < 0 ? -1.0f : 1.0f;
                        const float fx = sign * m[axis * 4], fz = sign * m[axis * 4 + 2];
                        const float* me = k ? (const float*)rb : (const float*)ra;
                        const float* other = target ? target : k ? (const float*)ra : (const float*)rb;
                        const float tx = other[0] - me[0], tz = other[2] - me[2];
                        cosine = (fx * tx + fz * tz) / std::max(std::hypot(fx, fz) * std::hypot(tx, tz), 0.001f);
                        side = fx * tz - fz * tx;
                        return true;
                    };
                    // Turning toward the other while it turns, walks or winds up (eased on the game
                    // thread), at most `rate` rad/s, slower as it comes round.
                    const auto steer = [&](int k, float rate, const float* target = nullptr) {
                        float cosine = 0.0f, side = 0.0f;
                        if (!face(k, cosine, side, target)) {
                            g_cvc_body[k].turn = 0.0f;
                            return;
                        }
                        const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
                        const float speed = std::min(rate, angle * 1.5f);
                        g_cvc_body[k].turn = side > 0.0f ? speed : -speed;
                    };
                    // The spot in front of k where the other should stand for k's blows to land on it:
                    // x, (y), z.
                    const auto front = [&](int k, float* spot) {
                        const Moves* moves = kind_of(k);
                        const u64 mats = moves ? q(objects[k] + 0x120 + 0x198) : 0;
                        float m[12]{};
                        SIZE_T got = 0;
                        if (!mats || now[k].empty() ||
                            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(mats), m, sizeof(m), &got)) {
                            return false;
                        }
                        const int axis = std::abs(moves->forward);
                        const float sign = moves->forward < 0 ? -1.0f : 1.0f;
                        float fx = sign * m[axis * 4], fz = sign * m[axis * 4 + 2];
                        const float n = std::max(std::hypot(fx, fz), 0.001f);
                        fx /= n, fz /= n;
                        // (Its hands come down 19-23 ahead of its root, measured; from 30 ahead they
                        // fell 9-10 short of the other's bones, so it stands at 25.)
                        const float* me = k ? (const float*)rb : (const float*)ra;
                        spot[0] = me[0] + fx * 25.0f, spot[1] = me[1], spot[2] = me[2] + fz * 25.0f;
                        return true;
                    };
                    // k's mouth: its frontmost bone along its facing (the head), 2 further on.
                    const auto mouth_of = [&](int k, float* at) {
                        const Moves* moves = kind_of(k);
                        const u64 mats = moves ? q(objects[k] + 0x120 + 0x198) : 0;
                        float m[12]{};
                        SIZE_T got = 0;
                        if (!mats || now[k].empty() ||
                            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(mats), m, sizeof(m), &got)) {
                            return false;
                        }
                        const int axis = std::abs(moves->forward);
                        const float sign = moves->forward < 0 ? -1.0f : 1.0f;
                        float fx = sign * m[axis * 4], fz = sign * m[axis * 4 + 2];
                        const float n = std::max(std::hypot(fx, fz), 0.001f);
                        fx /= n, fz /= n;
                        float best = -1e9f;
                        for (const auto& b : now[k]) {
                            const float along = b[0] * fx + b[2] * fz;
                            if (along > best) {
                                best = along, at[0] = b[0] + fx * 2.0f, at[1] = b[1], at[2] = b[2] + fz * 2.0f;
                            }
                        }
                        return true;
                    };
                    // Against one that holds its ground (Basaran: its head long before it, its slams coming
                    // down under its forelegs), k's place is beside its head, 20 ahead of it and 12 to the
                    // side k is on: each in the other's reach there. False when the other does not hold.
                    const auto flank = [&](int k, float* spot) {
                        const Moves* moves = kind_of(k);
                        const Moves* other = kind_of(1 - k);
                        float ahead[3]{};
                        if (!moves || moves->holds || !other || !other->holds || !front(1 - k, ahead)) {
                            return false;
                        }
                        const float* it = k ? (const float*)ra : (const float*)rb;
                        const float* me = k ? (const float*)rb : (const float*)ra;
                        const float fx = (ahead[0] - it[0]) / 25.0f, fz = (ahead[2] - it[2]) / 25.0f;
                        const float side = (me[0] - it[0]) * -fz + (me[2] - it[2]) * fx >= 0.0f ? 1.0f : -1.0f;
                        spot[0] = it[0] + fx * 20.0f - fz * 12.0f * side;
                        spot[1] = it[1];
                        spot[2] = it[2] + fz * 20.0f + fx * 12.0f * side;
                        return true;
                    };
                    const auto at_flank = [&](int k) {
                        float spot[3]{};
                        const float* me = k ? (const float*)rb : (const float*)ra;
                        return flank(k, spot) && std::hypot(me[0] - spot[0], me[2] - spot[2]) < 6.0f;
                    };
                    // Crossfade per move: standing, walking and turning ease in; an attack and a
                    // flinch come in quicker.
                    const auto play = [&](int k, u64 motion, Act act, int ms) {
                        // (Never the same motion started again within a second: a walk restarted every
                        // tick tripped the game's own motion state, "No Entry != Here", and crashed it.)
                        if (fighter[k].motion == motion && Clock::now() < fighter[k].since + std::chrono::seconds(1)) {
                            fighter[k].act = act, fighter[k].until = Clock::now() + std::chrono::milliseconds(ms);
                            return;
                        }
                        const float blend = act == Act::Attack ? 0.4f : act == Act::Flinch ? 0.25f
                                          : act == Act::Idle ? 0.6f : 0.35f;
                        CvcPlayMotion(image, objects[k], motion, blend);
                        fighter[k] = {act, motion, Clock::now(), Clock::now() + std::chrono::milliseconds(ms)};
                        g_cvc_act_now[k] = int(act);
                        g_cvc_body[k].turn = 0.0f;
                    };
                    // Its own animation played through: at its last frame, or replaced by the game's
                    // next one, or past its time.
                    const auto played = [&](int k) {
                        const Fighter& f = fighter[k];
                        const CvcBody& body = g_cvc_body[k];
                        if (Clock::now() > f.until) {
                            return true;
                        }
                        if (Clock::now() < f.since + std::chrono::milliseconds(300)) {
                            return false;
                        }
                        return body.motion.load() != f.motion ||
                               (body.frames.load() > 0.0f && body.frame.load() >= body.frames.load() - 0.5f);
                    };
                    const auto stand = [&](int k) {
                        {
                            std::scoped_lock guard{g_cvc_body[k].lock};
                            g_cvc_body[k].step[0] = g_cvc_body[k].step[1] = 0.0f;
                        }
                        const Moves* moves = kind_of(k);
                        const bool was_attacking = fighter[k].act == Act::Attack;
                        if (moves && !moves->idles.empty() && rng() % 100 < 30) {
                            play(k, moves->idles[rng() % moves->idles.size()], Act::Idle, 0); // now and then, another stance
                        } else if (moves && moves->idle) {
                            play(k, moves->idle, Act::Idle, 0);
                        } else {
                            fighter[k].act = Act::Idle;
                        }
                        if (attacking == k) {
                            if (was_attacking && landing.size() > 31) {
                                LOG_INFO(Core, "cvcbrawl: {} bones came down at (ahead, left) 17 {:.0f} {:.0f}, 18 {:.0f} {:.0f}, "
                                         "22 {:.0f} {:.0f}, 23 {:.0f} {:.0f}, 28 {:.0f} {:.0f}, 31 {:.0f} {:.0f}", k ? "B" : "A",
                                         landing[17][0], landing[17][1], landing[18][0], landing[18][1], landing[22][0],
                                         landing[22][1], landing[23][0], landing[23][1], landing[28][0], landing[28][1],
                                         landing[31][0], landing[31][1]);
                            }
                            landing.clear();
                            if (was_attacking && !landed) {
                                float cosine = 0.0f, side = 0.0f;
                                face(k, cosine, side);
                                const float* me = k ? (const float*)rb : (const float*)ra;
                                const float* it = k ? (const float*)ra : (const float*)rb;
                                const float d = std::hypot(it[0] - me[0], it[2] - me[2]);
                                const float sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
                                LOG_INFO(Core, "cvcbrawl: {} missed (the bodies came within {:.1f} of each other; the other at "
                                         "{:.0f} ahead, {:.0f} {}; it came on {:.1f}, the other moved {:.1f}; act {})", k ? "B" : "A",
                                         g_cvc_gap.load(), d * cosine, d * sine, side > 0.0f ? "left" : "right",
                                         std::hypot(me[0] - attack_root[0], me[2] - attack_root[1]),
                                         std::hypot(it[0] - target_root[0], it[2] - target_root[1]), int(fighter[1 - k].act));
                            }
                            attacking = -1;
                            next_attack = Clock::now() + std::chrono::milliseconds(400 + rng() % 700);
                        }
                    };
                    const auto attack = [&](int k) {
                        const Moves* moves = kind_of(k);
                        auto [motion, ms] = moves->attacks[rng() % moves->attacks.size()];
                        // The other off to one side: the attack to that side, if it has one.
                        float cosine = 1.0f, side = 0.0f;
                        // (Its side slams bring its head down on the other; its middle one mostly lands
                        // short: two in three to the side the other leans to.)
                        if (moves->strike_left && face(k, cosine, side) && (cosine < 0.97f || rng() % 3 != 0)) {
                            motion = side > 0.0f ? moves->strike_left : moves->strike_right, ms = 3000;
                        }
                        (void)ms; // it ends when its last part does (Act::Attack below); 12 s at most
                        play(k, motion, Act::Attack, 12000);
                        attacking = k, landed = false, attack_from = Clock::now();
                        g_cvc_gap = 1e9f;
                        {
                            const float* me = k ? (const float*)rb : (const float*)ra;
                            const float* it = k ? (const float*)ra : (const float*)rb;
                            attack_root = {me[0], me[2]}, target_root = {it[0], it[2]};
                        }
                        nearest_swing = nearest_any = 1e9f;
                        LOG_INFO(Core, "cvcbrawl: {} attacks ({:#x}, bodies {:.0f} apart)", k ? "B" : "A", motion,
                                 closest);
                    };
                    // Facing the other: walk up to it, or strike when near enough.
                    const auto engage = [&](int k) {
                        const Moves* moves = kind_of(k);
                        last_attacker = k, attacking = k;
                        float cosine = 0.0f, side = 0.0f;
                        if ((closest < moves->strike || moves->holds) && face(k, cosine, side) && cosine < moves->aim &&
                            moves->turn_left && moves->turn_right) {
                            // Near but not square to it (a slam lands in front): round first.
                            play(k, side > 0.0f ? moves->turn_left : moves->turn_right, Act::Turn, 4000);
                            LOG_INFO(Core, "cvcbrawl: {} squares up ({:.2f})", k ? "B" : "A", cosine);
                        } else if (moves->holds && [&] {
                                       // (Its slam: the other in front of it, near enough to step in on: its
                                       // hands come down about 22 ahead of its root.)
                                       const float* other = k ? (const float*)ra : (const float*)rb;
                                       const float* me = k ? (const float*)rb : (const float*)ra;
                                       return std::hypot(other[0] - me[0], other[2] - me[2]) > 40.0f;
                                   }()) {
                            // It waits; the other comes on into its reach (then it strikes). Or, from
                            // where it is, it fires its pulse at it.
                            const int j = 1 - k;
                            const Moves* other = kind_of(j);
                            // Its weapon well out of reach of its slam (beyond 50), at most every 30 s (each pulse
                            // knocks the other back out: more often it pinned it there); else
                            // reach it slams.

                            const float* it = k ? (const float*)ra : (const float*)rb;
                            const float* self = k ? (const float*)rb : (const float*)ra;
                            const bool out_of_reach = std::hypot(it[0] - self[0], it[2] - self[2]) > 35.0f;
                            if (moves->pulse && out_of_reach && Clock::now() > pulse_ready) {
                                pulse_ready = Clock::now() + std::chrono::seconds(25);
                                play(k, moves->idle, Act::Pulse, 6000);
                                pulse_phase = 0, landed = false, attack_from = Clock::now();
                                float mouth[3]{};
                                mouth_of(k, mouth);
                                // (Its own tts_pls_* charge and bolt effects are not resident in this arena, and
                                // Wander's sword effects s_* crash outside his hands: general sparks stand in.)
                                CvcEffect("npe/torch_spark.npe", mouth);
                                CvcEffect("npe/player_spark.npe", mouth);
                                CvcSound(rng() % 2 ? "kmvo_pulse_c2" : "kmvo_pulse_c4", mouth);
                                CvcSound("kame_pulse_c1", mouth);
                                g_cvc_rays.pulse_object = objects[k];
                                g_cvc_rays.image = image;
                                g_cvc_rays.pulse = 1; // its own charge
                                LOG_INFO(Core, "cvcbrawl: {} charges its pulse ({:.0f} apart)", k ? "B" : "A", closest);
                            } else if (other && other->walk && fighter[j].act == Act::Idle) {
                                play(j, other->walk, Act::Walk, 8000);
                                fighter[j].bait = true;
                                LOG_INFO(Core, "cvcbrawl: {} holds its ground, {} comes on ({:.0f} apart)", k ? "B" : "A",
                                         j ? "B" : "A", closest);
                            } else {
                                attacking = -1; // again shortly
                            }
                        } else if (moves->holds && moves->walk && [&] {
                                       const float* other = k ? (const float*)ra : (const float*)rb;
                                       const float* me = k ? (const float*)rb : (const float*)ra;
                                       return std::hypot(other[0] - me[0], other[2] - me[2]) > 26.0f;
                                   }()) {
                            // Its slam comes down about 22 ahead of its root: it walks on (its own walk)
                            // until the other is there; or, a third of the time its pulse is ready, fires that.
                            if (moves->pulse && Clock::now() > pulse_ready && rng() % 3 == 0) {
                                pulse_ready = Clock::now() + std::chrono::seconds(25);
                                play(k, moves->idle, Act::Pulse, 6000);
                                pulse_phase = 0, landed = false, attack_from = Clock::now();
                                float mouth[3]{};
                                mouth_of(k, mouth);
                                CvcEffect("npe/torch_spark.npe", mouth);
                                CvcEffect("npe/player_spark.npe", mouth);
                                CvcSound(rng() % 2 ? "kmvo_pulse_c2" : "kmvo_pulse_c4", mouth);
                                CvcSound("kame_pulse_c1", mouth);
                                g_cvc_rays.pulse_object = objects[k];
                                g_cvc_rays.image = image;
                                g_cvc_rays.pulse = 1; // its own charge
                                LOG_INFO(Core, "cvcbrawl: {} charges its pulse ({:.0f} apart)", k ? "B" : "A", closest);
                            } else {
                                play(k, moves->walk, Act::Walk, 6000);
                                LOG_INFO(Core, "cvcbrawl: {} comes on into its reach", k ? "B" : "A");
                            }
                        } else if (at_flank(k)) {
                            attack(k); // (beside its head: in reach; it turns onto it as it winds up)
                        } else if (closest >= moves->strike && moves->walk && !moves->holds) {
                            // (As long as the way takes: blows push the bodies apart now.)
                            play(k, moves->walk, Act::Walk, int(std::clamp(closest * 400.0f, 7000.0f, 20000.0f)));
                            LOG_INFO(Core, "cvcbrawl: {} closes in (bodies {:.0f} apart)", k ? "B" : "A", closest);
                        } else {
                            attack(k);
                        }
                    };
                    // Where an attacker's bones come down (test aid): each bone's lowest point during its
                    // attack, in its own frame (ahead, to its left) from its root; logged at the end.
                    if (attacking >= 0 && fighter[attacking].act == Act::Attack && !now[attacking].empty()) {
                        const int k = attacking;
                        const Moves* moves = kind_of(k);
                        const u64 mats = moves ? q(objects[k] + 0x120 + 0x198) : 0;
                        float m[12]{};
                        SIZE_T got = 0;
                        if (mats && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(mats), m, sizeof(m), &got)) {
                            const int axis = std::abs(moves->forward);
                            const float sign = moves->forward < 0 ? -1.0f : 1.0f;
                            float fx = sign * m[axis * 4], fz = sign * m[axis * 4 + 2];
                            const float n = std::max(std::hypot(fx, fz), 0.001f);
                            fx /= n, fz /= n;
                            const float* me = k ? (const float*)rb : (const float*)ra;
                            landing.resize(now[k].size(), {0.0f, 0.0f, 1e9f});
                            for (size_t i = 0; i < now[k].size(); i++) {
                                if (now[k][i][1] < landing[i][2]) {
                                    const float dx = now[k][i][0] - me[0], dz = now[k][i][2] - me[2];
                                    landing[i] = {dx * fx + dz * fz, fx * dz - fz * dx, now[k][i][1]};
                                }
                            }
                        }
                    }
                    if (!over && cvc_placed) {
                        // Both kept out of their AI (re-set: a slot can be replaced, cvckeep).
                        const s64 tick = Clock::now().time_since_epoch().count();
                        const bool free_a = tick < g_cvc_free_until[0].load(), free_b = tick < g_cvc_free_until[1].load();
                        *reinterpret_cast<volatile u64*>(image + 0x1593ae0) = free_a ? 0 : objects[0];
                        *reinterpret_cast<volatile u64*>(image + 0x1593b48) = free_b ? 0 : objects[1];
                        if (free_a || free_b) {
                            next_attack = Clock::now() + std::chrono::seconds(1); // nothing of ours meanwhile
                        }
                        static bool hooked = false;
                        if (!hooked) {
                            hooked = true;
                            g_cvc_rays.image = image;
                            *reinterpret_cast<volatile u64*>(image + 0x1593d70) =
                                reinterpret_cast<u64>(&CvcAiThunk); // contact_patch.py HELD
                            LOG_INFO(Core, "cvcbrawl: fight placed, both held; motion clock hook {}",
                                     CvcInstallMotionHook(image) ? "in" : "NOT in (unexpected code)");
                            stand(0), stand(1);
                        }
                        for (int k = 0; k < 2; k++) {
                            Fighter& f = fighter[k];
                            g_cvc_act_now[k] = int(f.act); // (the quality log; a flinch is set without play)
                            const Moves* moves = kind_of(k);
                            g_cvc_coming[k] = moves && moves->swing > 0.0f ? moves->swing : 40.0f;
                            if (!moves) {
                                continue;
                            }
                            float cosine = 0.0f, side = 0.0f;
                            const bool known = face(k, cosine, side);
                            if (f.act != Act::Attack) { // (stepping in belongs to a strike only)
                                std::scoped_lock guard{g_cvc_body[k].lock};
                                g_cvc_body[k].step[0] = g_cvc_body[k].step[1] = 0.0f;
                            }
                            // Locked into the other (over a unit inside it for 0.6 s): its body stops the
                            // blow or the step, so it breaks off and steps back (or stands). (Attacks went
                            // on through the other for seconds: the quality log's trapped bodies.)
                            {
                                // (And an attack leaning on the other 2.5 s, however shallow: Quadratus's
                                // stomps leant on Gaius for 8 s.)
                                static Clock::time_point deep_since[2]{}, touch_since[2]{}, quiet_until[2]{};
                                const auto at = Clock::now();
                                const bool deep = g_cvc_depth_now.load() > 1.0f;
                                const bool leaning = g_cvc_touch_now.load() && f.act == Act::Attack;
                                if (!deep) {
                                    deep_since[k] = {};
                                } else if (deep_since[k] == Clock::time_point{}) {
                                    deep_since[k] = at;
                                }
                                if (!leaning) {
                                    touch_since[k] = {};
                                } else if (touch_since[k] == Clock::time_point{}) {
                                    touch_since[k] = at;
                                }
                                const bool locked = deep_since[k] != Clock::time_point{} &&
                                                    at > deep_since[k] + std::chrono::milliseconds(600);
                                const bool pressed = touch_since[k] != Clock::time_point{} &&
                                                     at > touch_since[k] + std::chrono::milliseconds(3000);
                                // (Standing in it too: both stood 1.4 s 3 units inside each other. An attack
                                // only once it has leant on the other 3 s: a slam winds up in contact, and
                                // broken off then it never landed.)
                                if (at > quiet_until[k] && ((locked && (f.act == Act::Walk || f.act == Act::Turn ||
                                                                        f.act == Act::Idle)) ||
                                                            (pressed && f.act == Act::Attack))) {
                                    LOG_INFO(Core, "cvcbrawl: {} {} the other, breaks off", k ? "B" : "A",
                                             locked ? "locked into" : "leaning on");
                                    if (moves->retreat) {
                                        play(k, moves->retreat, Act::Retreat, 2500);
                                    } else {
                                        stand(k);
                                    }
                                    quiet_until[k] = at + std::chrono::seconds(2);
                                    deep_since[k] = touch_since[k] = {};
                                }
                            }
                            switch (f.act) {
                            case Act::Attack: {
                                // Turned onto the other while winding up, then the blow goes where it goes.
                                // (Basaran rears up for 1.6 s before its slam comes down: it can come on under it.)
                                const bool winding = Clock::now() < attack_from + std::chrono::milliseconds(moves->holds ? 1600 : 900);
                                if (winding) {
                                    steer(k, 0.3f);
                                } else {
                                    g_cvc_body[k].turn = 0.0f;
                                }
                                // (No stepping in during the wind-up: a root moved under an attack's own pose
                                // glides; it walks into reach first.)
                                if (played(k)) {
                                    // Its next part: already begun by the game, or begun here; none left: it
                                    // stands (from the attack's own end pose, crossfaded).
                                    const auto next = moves->follow.find(f.motion);
                                    if (next != moves->follow.end() && g_cvc_body[k].motion.load() == next->second) {
                                        f.motion = next->second, f.since = Clock::now();
                                        f.until = Clock::now() + std::chrono::seconds(6);
                                    } else if (next != moves->follow.end()) {
                                        CvcPlayMotion(image, objects[k], next->second, 0.2f);
                                        f.motion = next->second, f.since = Clock::now();
                                        f.until = Clock::now() + std::chrono::seconds(6);
                                    } else {
                                        stand(k);
                                    }
                                }
                                break;
                            }
                            case Act::Walk: {
                                if (f.bait) {
                                    // Onto the spot in front of the other's head, where its blows land; then
                                    // it is the other's move.
                                    float spot[3]{};
                                    const bool known_spot = front(1 - k, spot);
                                    steer(k, 0.45f, known_spot ? spot : nullptr);
                                    const float* me = k ? (const float*)rb : (const float*)ra;
                                    const bool there = known_spot && std::hypot(me[0] - spot[0], me[2] - spot[2]) < 6.0f;
                                    if (there || closest < 2.0f || g_cvc_touch_now.load()) {
                                        stand(k);
                                        engage(1 - k);
                                    } else if (Clock::now() > f.until) {
                                        stand(k);
                                        attacking = -1;
                                    }
                                    break;
                                }
                                // Against one that holds its ground: onto its place beside the other's head.
                                float flank_spot[3]{};
                                const bool flanking = flank(k, flank_spot);
                                // One that holds its ground (Basaran) comes on at an angle, its nose on a
                                // point 12 beside the other on the side its nose is already: the other ends
                                // up beside its head, where its side slams bring its head and hands down.
                                float angled[3]{};
                                const bool angling = moves->holds && [&] {
                                    float nose[3]{};
                                    if (!front(k, nose)) {
                                        return false;
                                    }
                                    const float* me = k ? (const float*)rb : (const float*)ra;
                                    const float* it = k ? (const float*)ra : (const float*)rb;
                                    float dx = it[0] - me[0], dz = it[2] - me[2];
                                    const float d = std::max(std::hypot(dx, dz), 0.001f);
                                    dx /= d, dz /= d;
                                    float px = -dz, pz = dx; // across the line to it
                                    if ((nose[0] - me[0]) * px + (nose[2] - me[2]) * pz < 0.0f) {
                                        px = -px, pz = -pz;
                                    }
                                    angled[0] = it[0] + px * 12.0f, angled[1] = it[1], angled[2] = it[2] + pz * 12.0f;
                                    return true;
                                }();
                                steer(k, 0.45f, flanking ? flank_spot : angling ? angled : nullptr);
                                if (flanking) {
                                    const float* me = k ? (const float*)rb : (const float*)ra;
                                    if (std::hypot(me[0] - flank_spot[0], me[2] - flank_spot[2]) < 5.0f || g_cvc_touch_now.load()) {
                                        stand(k);
                                        attack(k); // (it turns onto it as it winds up; engage would walk it on again)
                                    } else if (Clock::now() > f.until) {
                                        stand(k);
                                    }
                                    break;
                                }
                                if (moves->holds) { // (in until the other is about 22 ahead of it, or met)
                                    const float* other = k ? (const float*)ra : (const float*)rb;
                                    const float* me = k ? (const float*)rb : (const float*)ra;
                                    if (std::hypot(other[0] - me[0], other[2] - me[2]) <= 24.0f || g_cvc_touch_now.load()) {
                                        stand(k);
                                        attack(k);
                                    } else if (Clock::now() > f.until) {
                                        stand(k);
                                    }
                                    break;
                                }
                                if (g_cvc_touch_now.load()) {
                                    // (Met: walking on would only push it off, a glide. It strikes from here.)
                                    stand(k);
                                    attack(k);
                                } else if (closest < moves->strike) {
                                    engage(k); // square to it: the attack; else round first
                                } else if (closest < moves->strike + 2.0f && known && cosine > moves->aim - 0.1f) {
                                    attack(k); // as good as there, roughly facing it
                                } else if (Clock::now() > f.until && closest < moves->strike + 4.0f && known &&
                                           cosine > moves->aim - 0.1f) {
                                    attack(k); // as near as it gets (the bodies meet): strike from there
                                } else if (Clock::now() > f.until) {
                                    LOG_INFO(Core, "cvcbrawl: {} stops short ({:.0f} apart, facing {:.2f})", k ? "B" : "A",
                                             closest, cosine);
                                    stand(k);
                                }
                                break;
                            }
                            case Act::Turn:
                                steer(k, 0.5f);
                                if (known && cosine > std::min(moves->aim + 0.03f, 0.97f)) {
                                    LOG_INFO(Core, "cvcbrawl: {} turned to {:.2f}", k ? "B" : "A", cosine);
                                    engage(k);
                                } else if (!known || Clock::now() > f.since + std::chrono::seconds(6)) {
                                    LOG_INFO(Core, "cvcbrawl: {} gave up turning ({:.2f})", k ? "B" : "A", cosine);
                                    stand(k);
                                } else if (played(k)) {
                                    // Still round the other way: the turn again, on from where it is.
                                    CvcPlayMotion(image, objects[k], side > 0.0f ? moves->turn_left : moves->turn_right, 0.3f);
                                    f.motion = side > 0.0f ? moves->turn_left : moves->turn_right;
                                    f.until = Clock::now() + std::chrono::seconds(4);
                                }
                                break;
                            case Act::Flinch:
                                if (played(k)) {
                                    if (!moves->down.empty() && balance[k] > 1.8f) {
                                        balance[k] = 0.0f;
                                        // Knocked off its feet: its own fall, then getting up again.
                                        play(k, moves->down[0].first, Act::Down, moves->down[0].second ? moves->down[0].second : 8000);
                                        fighter[k].part = 0;
                                        LOG_INFO(Core, "cvcbrawl: {} goes down", k ? "B" : "A");
                                    } else if (f.heavy && moves->retreat) {
                                        play(k, moves->retreat, Act::Retreat, 3000);
                                        LOG_INFO(Core, "cvcbrawl: {} steps back", k ? "B" : "A");
                                    } else {
                                        stand(k);
                                    }
                                }
                                break;
                            case Act::Retreat:
                                if (played(k)) {
                                    stand(k);
                                }
                                break;
                            case Act::Down:
                                // A part given a time is held that long (a loop); the others to their end.
                                if (moves->down[f.part].second ? Clock::now() > f.since + std::chrono::milliseconds(
                                                                     moves->down[f.part].second)
                                                               : played(k)) {
                                    if (++f.part < moves->down.size()) {
                                        const auto [motion, ms] = moves->down[f.part];
                                        CvcPlayMotion(image, objects[k], motion, 0.3f);
                                        f.motion = motion, f.since = Clock::now();
                                        f.until = Clock::now() + std::chrono::milliseconds(ms ? ms + 1000 : 8000);
                                    } else {
                                        LOG_INFO(Core, "cvcbrawl: {} is up again", k ? "B" : "A");
                                        stand(k);
                                    }
                                }
                                break;
                            case Act::Pulse: {
                                // Charge 1.4 s, fire from its mouth along its facing at the other's chest
                                // (it hits when squarely aimed), the strike 0.25 s later, done after 1.5 s.
                                float mouth[3]{}, head[3]{};
                                mouth_of(k, mouth);
                                front(k, head);
                                const float* me = k ? (const float*)rb : (const float*)ra;
                                if (pulse_phase == 0 && Clock::now() < f.since + std::chrono::milliseconds(1400)) {
                                    steer(k, 0.4f);
                                } else if (pulse_phase == 0) {
                                    g_cvc_body[k].turn = 0.0f;
                                    // The other's chest: the middle of its bones.
                                    std::array<float, 3> chest{};
                                    for (const auto& b : now[1 - k]) {
                                        for (int c = 0; c < 3; c++) {
                                            chest[c] += b[c] / float(now[1 - k].size());
                                        }
                                    }
                                    pulse_on_target = known && cosine > 0.85f && !now[1 - k].empty();
                                    const float fx = head[0] - me[0], fz = head[2] - me[2];
                                    const float n = std::max(std::hypot(fx, fz), 0.001f);
                                    pulse_point = pulse_on_target ? chest
                                                                  : std::array<float, 3>{mouth[0] + fx / n * 70.0f, me[1] - 10.0f,
                                                                                         mouth[2] + fz / n * 70.0f};
                                    const float yaw = std::atan2(pulse_point[0] - mouth[0], pulse_point[2] - mouth[2]);
                                    CvcEffect("npe/bp_harpoon_impact.npe", mouth, yaw);
                                    CvcEffect("npe/arrow_hit_spark.npe", mouth, yaw);
                                    std::memcpy(pulse_from.data(), mouth, 12);
                                    pulse_drawn = 0.0f;
                                    // Its own projectile meets only the ground (not another colossus): aimed
                                    // at the ground under the other's chest, it bursts against its legs there.
                                    {
                                        float ground = pulse_point[1];
                                        for (const auto& bone : now[1 - k]) {
                                            ground = std::min(ground, bone[1]);
                                        }
                                        const float aim[3]{pulse_point[0], pulse_on_target ? ground - 2.0f : pulse_point[1], pulse_point[2]};
                                        std::memcpy(g_cvc_rays.pulse_target, aim, 12);
                                    }
                                    g_cvc_rays.pulse_object = objects[k];
                                    g_cvc_rays.pulse = 2; // its own shot
                                    CvcSound(rng() % 2 ? "kame_pulse_fire1" : "kame_pulse_fire2", mouth);
                                    CvcSound("kame_pulse_line1", pulse_point.data());
                                    CvcSound("kame_pulse_line2", mouth);
                                    pulse_phase = 1, pulse_fired = Clock::now();
                                    LOG_INFO(Core, "cvcbrawl: {} fires its pulse ({}, facing {:.2f})", k ? "B" : "A",
                                             pulse_on_target ? "at the other" : "past it", cosine);
                                } else if (pulse_phase == 1 && Clock::now() <= pulse_fired + std::chrono::milliseconds(250)) {
                                    // The bolt crossing: sparks laid along it every few units as it travels.
                                    const float reach = std::min(1.0f, std::chrono::duration<float>(Clock::now() - pulse_fired).count() / 0.25f);
                                    const float length = std::hypot(pulse_point[0] - pulse_from[0], pulse_point[1] - pulse_from[1],
                                                                    pulse_point[2] - pulse_from[2]);
                                    const float step = std::max(4.0f / std::max(length, 1.0f), 0.02f);
                                    const float yaw = std::atan2(pulse_point[0] - pulse_from[0], pulse_point[2] - pulse_from[2]);
                                    for (int n = 0; pulse_drawn + step <= reach && n < 6; n++) {
                                        pulse_drawn += step;
                                        const float at[3]{pulse_from[0] + (pulse_point[0] - pulse_from[0]) * pulse_drawn,
                                                          pulse_from[1] + (pulse_point[1] - pulse_from[1]) * pulse_drawn,
                                                          pulse_from[2] + (pulse_point[2] - pulse_from[2]) * pulse_drawn};
                                        CvcEffect(n % 2 ? "npe/player_spark.npe" : "npe/arrow_hit_spark.npe", at, yaw);
                                    }
                                } else if (pulse_phase == 1) {
                                    const float yaw = std::atan2(pulse_point[0] - mouth[0], pulse_point[2] - mouth[2]);
                                    if (pulse_on_target) {
                                        CvcEffect("npe/knight_shockwave_add.npe", pulse_point.data(), yaw);
                                        CvcEffect("npe/arrow_hit_spark.npe", pulse_point.data(), yaw);
                                        CvcEffect("npe/tts_pls_dst_smoke.npe", pulse_point.data(), yaw);
                                        CvcSound(rng() % 2 ? "kame_pulse_hit1" : "kame_pulse_hit3", pulse_point.data());
                                        pulse_hit_for = k; // the blow code below takes it
                                    } else {
                                        CvcEffect("npe/tts_pls_dst_smoke.npe", pulse_point.data(), yaw);
                                        CvcEffect("npe/gyz_gds_impact.npe", pulse_point.data(), yaw);
                                        CvcEffect("npe/player_stone_dust.npe", pulse_point.data(), yaw);
                                        CvcSound("kame_pulse_hit2", pulse_point.data());
                                    }
                                    pulse_phase = 2;
                                } else if (pulse_phase == 2 && Clock::now() > pulse_fired + std::chrono::milliseconds(1500)) {
                                    stand(k);
                                }
                                break;
                            }
                            case Act::Idle:
                                break;
                            }
                        }
                        // The next move: whoever's turn it is, when it stands and nobody attacks.
                        // Strictly in turn: the one hit answers once it has recovered; the other goes
                        // again only if that one cannot (more than 8 s on).
                        if (attacking < 0 && Clock::now() > next_attack) {
                            const bool waited = Clock::now() > next_attack + std::chrono::seconds(8);
                            for (int turn = 0; turn < (waited ? 2 : 1) && attacking < 0; turn++) {
                                const int k = (last_attacker + 1 + turn) % 2;
                                const Moves* moves = kind_of(k);
                                float cosine = 0.0f, side = 0.0f;
                                if (!moves || moves->attacks.empty() || fighter[k].act != Act::Idle ||
                                    std::hypot(ra[0] - rb[0], ra[2] - rb[2]) > moves->reach || !face(k, cosine, side)) {
                                    continue;
                                }
                                if (cosine > 0.8f) {
                                    engage(k);
                                } else if (moves->turn_left && moves->turn_right) {
                                    last_attacker = k, attacking = k;
                                    play(k, side > 0.0f ? moves->turn_left : moves->turn_right, Act::Turn, 4000);
                                    LOG_INFO(Core, "cvcbrawl: {} turns to face the other ({:.2f})", k ? "B" : "A", cosine);
                                }
                            }
                        }
                        // Two bodies that touch ease apart (a few units a second, on the game thread).
                        const float dx = ra[0] - rb[0], dz = ra[2] - rb[2], apart = std::hypot(dx, dz);
                        const float ease = 0.0f; // bodies are not pushed apart (reads as sliding)
                        (void)apart;
                        for (int k = 0; k < 2; k++) {
                            std::scoped_lock guard{g_cvc_body[k].lock};
                            g_cvc_body[k].apart[0] = (k ? -dx : dx) * ease;
                            g_cvc_body[k].apart[1] = (k ? -dz : dz) * ease;
                        }
                    } else if (over) {
                        for (int k = 0; k < 2; k++) {
                            g_cvc_body[k].turn = 0.0f;
                            std::scoped_lock guard{g_cvc_body[k].lock};
                            g_cvc_body[k].apart[0] = g_cvc_body[k].apart[1] = 0.0f;
                            g_cvc_body[k].step[0] = g_cvc_body[k].step[1] = 0.0f;
                        }
                    }
                    for (int k = 0; k < 2; k++) {
                        // The other one down without a blow of ours (the game's own damage, e.g. a
                        // native attack): the same knockout.
                        // Or already playing a death motion (the game's own death of a character,
                        // after native attacks): component +0x260 = object +0x380.
                        const u64 motion = q(objects[1 - k] + 0x380) - image;
                        const bool fell = (*reinterpret_cast<const volatile float*>(managers[1 - k] + 0x50) <= 0.0f &&
                                           *reinterpret_cast<const volatile float*>(managers[1 - k] + 0x4c) > 0.0f) ||
                                          motion == 0x1d39370 || motion == 0x1d395c0; // Knight_died_early, _loop
                        // A blow: the physics found k's body striking the other's, hard enough (any
                        // part, any move: a body driven into the other hurts it as hard as it lands).
                        int hit_bone = -1;
                        float hit_speed = 0.0f, impulse = 0.0f;
                        std::array<float, 3> contact{};
                        {
                            std::scoped_lock guard{g_cvc_blow.lock};
                            // (One swing through several of the other's parts is one blow: 0.5 s apart.)
                            static Clock::time_point last_blow[2]{};
                            if (g_cvc_blow.ready && g_cvc_blow.striker == k && Clock::now() < last_blow[k] + std::chrono::milliseconds(500)) {
                                g_cvc_blow.ready = false;
                            } else if (g_cvc_blow.ready && g_cvc_blow.striker == k && fighter[k].act != Act::Attack) {
                                g_cvc_blow.ready = false; // (bodies knocking together outside a strike push and lean, no harm)
                            } else if (g_cvc_blow.ready && g_cvc_blow.striker == k) {
                                last_blow[k] = Clock::now();
                                g_cvc_blow.ready = false;
                                hit_bone = g_cvc_blow.bone, hit_speed = g_cvc_blow.closing, impulse = g_cvc_blow.impulse;
                                std::memcpy(contact.data(), g_cvc_blow.point, 12);
                            }
                        }
                        const bool pulsed = pulse_hit_for == k;
                        if (pulsed) {
                            hit_bone = 0, hit_speed = 300.0f, contact = pulse_point; // its pulse struck
                            pulse_hit_for = -1;
                        }
                        if (hit_bone < 0 && !fell) {
                            continue;
                        }
                        auto* health = reinterpret_cast<float*>(managers[1 - k] + 0x50);
                        if (hit_bone >= 0) {
                            landed = true;
                            // The blow's impulse from the physics, against a full blow's (1); its pulse lands
                            // as a full blow. Damage goes with it: a graze does little, the heaviest blow
                            // twice a full one.
                            constexpr float full_blow = 30000.0f; // (Gaius's solid slam)
                            const float force = pulsed ? 1.0f : std::clamp(impulse / full_blow, 0.05f, 3.0f);
                            // Below 1: a share of the victim's full health for a full blow.
                            const float set = k == 0 ? dmg_a : dmg_b;
                            const float damage = (set < 1.0f ? set * *reinterpret_cast<const float*>(managers[1 - k] + 0x4c) : set) *
                                                 std::clamp(force > 1.0f ? 1.0f + 0.5f * (force - 1.0f) : force, 0.1f, 2.0f) /* (past a full blow, half as much again) */;
                            *health = std::max(0.0f, *health - damage);
                            balance[1 - k] += force;
                            // The impact: both freeze for a moment, the view shakes, then the one hit
                            // flinches (one of its own, by where the blow landed: the head, from
                            // behind, else the body) and is pushed away along the blow, and a heavy blow
                            // makes it step back after; the striker rocks back a little.
                            const Moves* victim_moves = kind_of(1 - k);
                            const Moves* striker_moves = kind_of(k);
                            const float strength = std::clamp(force, 0.3f, 1.6f); // (effects and sounds)
                            const bool heavy = *health > 0.0f && force > 0.9f;
                            const int stop_ms = 60; // the one hit reacts a beat after the blow
                            // (No hit-stop or camera shake: freezing only the motion clock, and a fast wobble
                            // filmed at 30 frames a second, both read as the bodies snapping.)
                            const float* vr = k ? (const float*)ra : (const float*)rb; // the victim's root
                            const float* sr = k ? (const float*)rb : (const float*)ra;
                            float dir[2]{vr[0] - sr[0], vr[2] - sr[2]};
                            const float span = std::max(std::hypot(dir[0], dir[1]), 0.001f);
                            dir[0] /= span, dir[1] /= span;
                            const std::array<float, 3> point = contact;
                            {
                                // The ground under the contact: the victim's lowest bone.
                                std::array<float, 3> ground = point;
                                for (const auto& b : now[1 - k]) {
                                    ground[1] = std::min(ground[1], b[1]);
                                }
                                const u64 entry = q(image + (k ? 0x1593b28 : 0x1593b30));
                                const bool armoured = entry >= table && (entry - table) / 0x1e8 == 2; // Gaius
                                CvcImpact(image, objects[1 - k], armoured, point.data(), ground.data(), strength, heavy);
                            }
                            // Its body gives under the blow, as much as the blow carries (and the striker's
                            // rocks back a little); losing its footing without a fall of its own (Gaius),
                            // it staggers far.
                            // (A body blow's lean and push came from the physics, there and then; the pulse
                            // meets no body there, so its push is given here: a heavy blow's impulse.)
                            const Moves* victim_kind = kind_of(1 - k);
                            const bool stagger = *health > 0.0f && balance[1 - k] > 1.8f && victim_kind && victim_kind->down.empty();
                            if (pulsed) {
                                CvcReactImpulse(1 - k, dir, point.data(), force);
                                const u64 entry = q(image + (k ? 0x1593b28 : 0x1593b30));
                                const float mass = Sotc::CvcPhysics::Mass(entry >= table ? int((entry - table) / 0x1e8) : -1);
                                if (mass > 0.0f) {
                                    std::scoped_lock guard{g_cvc_body[1 - k].lock};
                                    const float shove = std::min(force * full_blow / mass, 20.0f); // (a few units, as a blow)
                                    g_cvc_body[1 - k].push[0] += dir[0] * shove;
                                    g_cvc_body[1 - k].push[1] += dir[1] * shove;
                                }
                            }
                            if (stagger) { // past its balance: its motors give, the blow carries it over
                                {
                                    std::scoped_lock guard{g_cvc_react[1 - k].lock};
                                    g_cvc_react[1 - k].weak_until = std::chrono::duration<float>(
                                        std::chrono::steady_clock::now().time_since_epoch()).count() + 1.3f;
                                }
                                CvcReactImpulse(1 - k, dir, point.data(), force * 1.5f);
                                balance[1 - k] = 0.0f;
                                LOG_INFO(Core, "cvcbrawl: {} loses its footing", k ? "A" : "B");
                            }
                            // The striker's blow is stopped by the body it met: its attack ends there and it
                            // recovers into its stance (crossfaded) instead of cutting on through the other
                            // (blades 2-4 units inside for up to 0.7 s in the quality log).
                            if (fighter[k].act == Act::Attack && force > 0.2f) {
                                stand(k);
                            }
                            // The flinch (not from the knockout blow: the death follows that).
                            u64 flinch = 0;
                            // (A graze is only felt in its body, not a flinch; down, it lies on.)
                            if (*health > 0.0f && victim_moves && fighter[1 - k].act != Act::Down && force > 0.35f) {
                                float low = 1e9f, high = -1e9f;
                                for (const auto& b : now[1 - k]) {
                                    low = std::min(low, b[1]), high = std::max(high, b[1]);
                                }
                                float cosine = 0.0f, side = 0.0f;
                                const bool behind = face(1 - k, cosine, side) && cosine < -0.2f;
                                const bool on_head = point[1] > low + (high - low) * 0.75f;
                                const auto& pick = behind ? victim_moves->back : on_head ? victim_moves->high
                                                                                          : victim_moves->front;
                                flinch = pick[rng() % pick.size()];
                                fighter[1 - k] = {Act::Flinch, flinch, Clock::now() + std::chrono::milliseconds(stop_ms),
                                                  Clock::now() + std::chrono::milliseconds(stop_ms + 4000), heavy || stagger};
                                g_cvc_body[1 - k].turn = 0.0f;
                                LOG_INFO(Core, "cvcbrawl: {} flinches ({:#x}{}{}){}", k ? "A" : "B", flinch,
                                         behind ? ", from behind" : "", on_head ? ", head" : "", heavy ? ", heavy" : "");
                            }
                            const float shove = (14.0f + 16.0f * strength) / (victim_moves ? victim_moves->mass : 1.0f);
                            const float recoil = 4.0f / (striker_moves ? striker_moves->mass : 1.0f);
                            std::thread([image, victim = objects[1 - k], striker = k, flinch, stop_ms,
                                         push = std::array<float, 2>{dir[0] * shove, dir[1] * shove},
                                         back = std::array<float, 2>{-dir[0] * recoil, -dir[1] * recoil}] {
                                std::this_thread::sleep_for(std::chrono::milliseconds(stop_ms));
                                if (flinch) {
                                    CvcPlayMotion(image, victim, flinch, 0.25f);
                                }
                                (void)striker, (void)push, (void)back; // no scripted push: its own reaction moves it
                            }).detach();
                            (*reinterpret_cast<volatile u32*>(image + (k == 0 ? 0x1593ec8 : 0x1593ecc)))++;
                            LOG_INFO(Core,
                                     "cvcbrawl: {} hit {} (bone {}, {:.0f} units/s, impulse {:.0f}, force {:.2f}, balance {:.1f}, roots {:.0f}), health A {:.0f} B {:.0f} at {} ms",
                                     k == 0 ? "A" : "B", k == 0 ? "B" : "A", hit_bone, hit_speed, impulse, force, balance[1 - k],
                                     std::hypot(ra[0] - rb[0], ra[2] - rb[2]),
                                     *reinterpret_cast<const float*>(managers[0] + 0x50),
                                     *reinterpret_cast<const float*>(managers[1] + 0x50),
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch()).count());
                        } else {
                            LOG_INFO(Core, "cvcbrawl: {} down without a blow", k == 0 ? "B" : "A");
                        }
                        if (*health <= 0.0f) {
                            LOG_INFO(Core, "cvcbrawl: {} wins", k == 0 ? "A" : "B");
                            *reinterpret_cast<volatile u8*>(image + 0x1593efb) = u8(k + 1); // winner
                            // Knockout: the colossi stop targeting each other (the one-entry target
                            // lists hold Wander, who takes no damage), the loser's weak points
                            // all report damage, which makes its own script play its death, and
                            // once that has begun its slot is cleared so nothing calls into it while
                            // the game tears it down (the AI trampoline skips a cleared slot, the
                            // redirects ignore it, the contact test stops).
                            *reinterpret_cast<volatile u64*>(image + 0x1593f00) = PlayerObject(image);
                            *reinterpret_cast<volatile u64*>(image + 0x1593f40) = PlayerObject(image);
                            *reinterpret_cast<volatile u64*>(image + 0x1593dc0) = 0;
                            // The loser's own script keeps getting its boss entry once its slot is
                            // cleared (CUR4, re/cvc/target_patch.py): it plays the death.
                            *reinterpret_cast<volatile u64*>(image + 0x1593b40) =
                                *reinterpret_cast<const volatile u64*>(image + (k == 0 ? 0x1593b30 : 0x1593b28));
                            *reinterpret_cast<volatile u64*>(image + 0x1593b38) = objects[1 - k];
                            // Neither of the moves' AI holds stays on the loser: a host plays the
                            // level's own death (its AI runs it); a guest is held again below.
                            for (const u64 slot : {0x1593ae0ull, 0x1593b48ull}) {
                                auto* hold = reinterpret_cast<volatile u64*>(image + slot);
                                if (*hold == objects[1 - k] || *hold == objects[k]) {
                                    *hold = 0;
                                }
                            }
                            // The winner stays out of its AI (it would go after Wander, over the body):
                            // it finishes what it was doing, then stands.
                            *reinterpret_cast<volatile u64*>(image + 0x1593ae0) = objects[k];
                            {
                                const Moves* moves = kind_of(k);
                                const auto left = fighter[k].act == Act::Idle ? Clock::duration{}
                                                                              : std::max(Clock::duration{}, fighter[k].until - Clock::now());
                                if (moves && moves->idle) {
                                    std::thread([image, winner = objects[k], idle = moves->idle,
                                                 wait = std::min(left, Clock::duration(std::chrono::seconds(6)))] {
                                        std::this_thread::sleep_for(wait + std::chrono::milliseconds(200));
                                        CvcPlayMotion(image, winner, idle, 0.5f);
                                    }).detach();
                                }
                            }
                            // A guest colossus (not the level's own boss, 0x325e8a8) plays its own death
                            // motion with its AI no longer run (0x1593b48, contact_patch.py): the
                            // level's death sequence only collapses the level's own boss.
                            {
                                const u64 table = *reinterpret_cast<const u64*>(image + 0x16d21f8 + 0x20);
                                const u64 entry = *reinterpret_cast<const volatile u64*>(image + 0x1593b40);
                                const u64 level_boss = *reinterpret_cast<const volatile u64*>(image + 0x325e8a8);
                                // Static MotionKind records (component +0x260 during the native death).
                                // Captured with re/cvc_death_probe.sh (each killed in its own arena).
                                static const std::unordered_map<u64, u64> deaths{
                                    {0, 0x1d0c820},  // Valus: Minotaur_death_Introduction
                                    {1, 0x1d65f90},  // Quadratus: A_boss_death
                                    {2, 0x1d39370},  // Gaius: Knight_died_early
                                    {3, 0x1ce8ab0},  // Phaedra: Kirin_death_started
                                    {5, 0x1d00d70},  // Barba: Minotaur_B_death_early
                                    {8, 0x1d58b40},  // Basaran: Turtle_mortality_beginning_(from_falling)
                                    {10, 0x1d246d0}, // Celosia: Lion_mortality_beginning
                                    {13, 0x1cd1690}, // Cenobia: Cerberus_death_started
                                    {14, 0x1cf7190}, // Argus: Minotaur_C_died_early
                                };
                                const auto death = deaths.find(entry >= table ? (entry - table) / 0x1e8 : ~0ull);
                                if (entry != level_boss && death != deaths.end()) {
                                    *reinterpret_cast<volatile u64*>(image + 0x1593b48) = objects[1 - k];
                                    CvcPlayMotion(image, objects[1 - k], death->second);
                                }
                            }
                            const u64 loser_manager = managers[1 - k];
                            for (int step = 0; step < 600; step++) {
                                const u64 points = *reinterpret_cast<const volatile u64*>(loser_manager + 0x100);
                                const s32 count = *reinterpret_cast<const volatile s32*>(loser_manager + 0x68);
                                for (s32 i = 0; points && i < std::min(count, 64); i++) {
                                    *reinterpret_cast<volatile float*>(points + 0x28 + i * 8ull) = 50.0f;
                                }
                                *health = 0.0f;
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(900));
                            // The game's own death (manager update 0x6cb1d0 at health 0): manager
                            // +0x108 set, 0x82df90 stops the AI, character +0x240 = 1 and +0xb0d bit 2
                            // (object +0x360 / +0xc2d).
                            const u64 loser = objects[1 - k];
                            LOG_INFO(Core, "cvcbrawl: loser {:#x} death state: manager +0x108 {} health {:.0f}, "
                                     "+0x360 {}, +0xc2d {:#x}", loser,
                                     *reinterpret_cast<const volatile u8*>(loser_manager + 0x108),
                                     *reinterpret_cast<const volatile float*>(loser_manager + 0x50),
                                     *reinterpret_cast<const volatile u32*>(loser + 0x360),
                                     *reinterpret_cast<const volatile u8*>(loser + 0xc2d));
                            for (const u64 def : {0x1593b28ull, 0x1593b30ull}) {
                                const u64 entry = *reinterpret_cast<const volatile u64*>(image + def);
                                std::string found;
                                for (u32 at = 0; entry && at < 0x1e8; at += 8) {
                                    const u64 v = *reinterpret_cast<const volatile u64*>(entry + at);
                                    for (int m = 0; m < 2; m++) {
                                        if (v == managers[m] || v == objects[m]) {
                                            found += fmt::format(" +{:#x}={}{}", at, v == managers[m] ? "mgr" : "obj",
                                                                 m ? "B" : "A");
                                        }
                                    }
                                }
                                LOG_INFO(Core, "cvcbrawl: boss entry {} {:#x}:{}", def == 0x1593b28 ? "A" : "B", entry,
                                         found);
                            }
                            g_cvc_body[1 - k].object = 0;
                            *reinterpret_cast<volatile u64*>(image + (k == 0 ? 0x1593b10 : 0x1593b08)) = 0;
                            LOG_INFO(Core, "cvcbrawl: loser cleared from the fight");
                            for (int step = 0; step < 60; step++) {
                                const auto* at = reinterpret_cast<const volatile float*>(loser + 0x130);
                                Points body;
                                bones(loser, body);
                                float low = 1e9f, high = -1e9f;
                                for (const auto& b : body) {
                                    low = std::min(low, b[1]), high = std::max(high, b[1]);
                                }
                                LOG_INFO(Core, "cvcbrawl: loser motion {:#x} at {:.0f} {:.0f} {:.0f}, type {}, "
                                         "{} bones y {:.0f}..{:.0f}",
                                         *reinterpret_cast<const volatile u64*>(loser + 0x380) - image, at[0], at[1],
                                         at[2], *reinterpret_cast<const volatile u8*>(loser + 0xc), body.size(), low,
                                         high);
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            }
                            // 30 s after the knockout: the fight has been shown (a script can end the
                            // recording on this byte).
                            *reinterpret_cast<volatile u8*>(image + 0x1593b50) = 1;
                            g_cvc_body[0].object = 0, g_cvc_body[1].object = 0;
                            LOG_INFO(Core, "cvcbrawl: knockout shown (support-object guard stepped in {} times)",
                                     *reinterpret_cast<const volatile u32*>(image + 0x1593b70));
                            return;
                        }
                    }
                }
                g_cvc_body[0].object = 0, g_cvc_body[1].object = 0;
            }).detach();
        } else if (action.starts_with("cvchits=")) {
            // Test: colossus-vs-colossus damage from the game's own attack contact. With the
            // attack patches (re/cvc/attack_patch.py) the AI's attack check 0x82ede0 tests the
            // attack volume against the opponent and counts in the cave: checks by A/B at
            // 0x1593ec0/c4, hits by A/B at 0x1593ec8/cc. "cvchits=120:20:15:900": for 120 s each
            // new hit by A takes 20 off B's manager health (0x1593e08 -> +0x50), each by B 15 off
            // A's, at most one hit per attacker per 900 ms.
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            float seconds = 0, dmg_a = 0, dmg_b = 0, cooldown = 0;
            sscanf(action.c_str() + 8, "%f:%f:%f:%f", &seconds, &dmg_a, &dmg_b, &cooldown);
            std::thread([image, seconds, dmg_a, dmg_b, cooldown] {
                const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
                const auto u = [](u64 a) { return *reinterpret_cast<const volatile u32*>(a); };
                const auto now = [] { return std::chrono::steady_clock::now(); };
                const auto end = now() + std::chrono::milliseconds(u64(seconds * 1000));
                u32 seen_a = u(image + 0x1593ec8), seen_b = u(image + 0x1593ecc);
                auto ready_a = now(), ready_b = now(), report = now();
                while (now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const u64 ma = q(image + 0x1593e00), mb = q(image + 0x1593e08);
                    const u32 hits_a = u(image + 0x1593ec8), hits_b = u(image + 0x1593ecc);
                    if (now() >= report) {
                        report = now() + std::chrono::seconds(5);
                        LOG_INFO(Core, "cvchits: checks A {} B {}, hits A {} B {}",
                                 u(image + 0x1593ec0), u(image + 0x1593ec4), hits_a, hits_b);
                    }
                    if (!ma || !mb) {
                        continue;
                    }
                    auto* ha = reinterpret_cast<float*>(ma + 0x50);
                    auto* hb = reinterpret_cast<float*>(mb + 0x50);
                    if (hits_a != seen_a && now() >= ready_a) {
                        *hb = std::max(0.0f, *hb - dmg_a);
                        ready_a = now() + std::chrono::milliseconds(u64(cooldown));
                        LOG_INFO(Core, "cvchits: A hit B, A {:.0f} B {:.0f}", *ha, *hb);
                    }
                    if (hits_b != seen_b && now() >= ready_b) {
                        *ha = std::max(0.0f, *ha - dmg_b);
                        ready_b = now() + std::chrono::milliseconds(u64(cooldown));
                        LOG_INFO(Core, "cvchits: B hit A, A {:.0f} B {:.0f}", *ha, *hb);
                    }
                    seen_a = hits_a;
                    seen_b = hits_b;
                    if (*ha <= 0.0f || *hb <= 0.0f) {
                        LOG_INFO(Core, "cvchits: {} wins", *ha > 0.0f ? "A" : "B");
                        break;
                    }
                }
            }).detach();
        } else if (action.starts_with("cvcfight=")) {
            // Test: synthetic colossus-vs-colossus hits. "cvcfight=90:55:20:15:2500": for 90 s,
            // while colossi A and B (cave slots 0x1593b08/10) are within 55 units (x/z), every
            // 2.5 s A takes 15 and B takes 20 off their manager's health (0x1593e00/08 -> +0x50),
            // and A is pushed 6 units away from B (world copies and local transform).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            float seconds = 0, reach = 0, dmg_a = 0, dmg_b = 0, period = 0;
            sscanf(action.c_str() + 9, "%f:%f:%f:%f:%f", &seconds, &reach, &dmg_a, &dmg_b, &period);
            std::thread([image, seconds, reach, dmg_a, dmg_b, period] {
                const auto q = [](u64 a) { return *reinterpret_cast<const volatile u64*>(a); };
                const auto end = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(u64(seconds * 1000));
                auto next_hit = std::chrono::steady_clock::now();
                int hits = 0;
                while (std::chrono::steady_clock::now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    const u64 a = q(image + 0x1593b08), b = q(image + 0x1593b10);
                    const u64 ma = q(image + 0x1593e00), mb = q(image + 0x1593e08);
                    if (!a || !b || !ma || !mb) {
                        continue;
                    }
                    auto* pa = reinterpret_cast<float*>(a + 0x130);
                    const auto* pb = reinterpret_cast<const float*>(b + 0x130);
                    const float dx = pa[0] - pb[0], dz = pa[2] - pb[2];
                    const float d = std::sqrt(dx * dx + dz * dz);
                    if (d > reach || std::chrono::steady_clock::now() < next_hit) {
                        continue;
                    }
                    next_hit = std::chrono::steady_clock::now() + std::chrono::milliseconds(u64(period));
                    auto* ha = reinterpret_cast<float*>(ma + 0x50);
                    auto* hb = reinterpret_cast<float*>(mb + 0x50);
                    *hb = std::max(0.0f, *hb - dmg_a);
                    *ha = std::max(0.0f, *ha - dmg_b);
                    const float push = 0.0f / std::max(d, 0.001f);
                    for (const u64 at : {0x130, 0x140, 0x160, 0x750, 0x7b0, 0x7c0, 0x7f0, 0x1640}) {
                        auto* p = reinterpret_cast<float*>(a + at);
                        p[0] += dx * push;
                        p[2] += dz * push;
                    }
                    LOG_INFO(Core, "cvcfight hit {}: distance {:.1f}, A {:.0f}, B {:.0f}", ++hits, d,
                             *ha, *hb);
                    if (*ha <= 0.0f || *hb <= 0.0f) {
                        LOG_INFO(Core, "cvcfight: {} wins", *ha > 0.0f ? "A" : "B");
                        break;
                    }
                }
            }).detach();
        } else if (action.starts_with("findvec=")) {
            // Test: places in game memory holding three floats within r of x/y/z
            // ("findvec=-288/56/1388/300"), e.g. an actor's position. Read through
            // ReadProcessMemory so pages without backing cannot fault.
            float v[4]{};
            sscanf(action.c_str() + 8, "%f/%f/%f/%f", &v[0], &v[1], &v[2], &v[3]);
            if (action.starts_with("findvec=cam/")) {
                // "findvec=cam/0.5": around the gameplay camera's current position (camera+0x1740).
                const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
                const u64 image = module ? module->GetBaseAddress() : 0;
                const s32 slot = image ? *reinterpret_cast<const s32*>(image + 0x3259cf8) : -1;
                const u64 camera =
                    slot >= 0 && slot < 8 ? *reinterpret_cast<const u64*>(image + 0x3259d00 + slot * 8ull) : 0;
                if (camera) {
                    std::memcpy(v, reinterpret_cast<const float*>(camera + 0x1740), 12);
                }
                v[3] = std::stof(action.substr(12));
                LOG_INFO(Core, "findvec: camera at {} {} {}", v[0], v[1], v[2]);
            }
            std::vector<std::pair<VAddr, u64>> areas;
            Core::Memory::Instance()->ForEachReadable(
                [&](VAddr base, u64 size) { areas.emplace_back(base, size); });
            std::vector<float> chunk(0x10000 / 4 + 2);
            int hits = 0;
            static int scan = 0;
            std::ofstream out{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                              fmt::format("findvec_{}.txt", scan++)};
            for (const auto& [base, size] : areas) {
                for (u64 at = 0; at < size && hits < 200000; at += 0x10000) {
                    const u64 n = std::min<u64>(0x10000 + 8, size - at);
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(),
                                           reinterpret_cast<const void*>(base + at),
                                           chunk.data(), n, &got) ||
                        got < 12) {
                        continue;
                    }
                    for (u64 i = 0; i + 2 < got / 4 && i < 0x10000 / 4 && hits < 200000; i++) {
                        if (std::abs(chunk[i] - v[0]) < v[3] && std::abs(chunk[i + 1] - v[1]) < v[3] &&
                            std::abs(chunk[i + 2] - v[2]) < v[3]) {
                            out << fmt::format("{:x} {:.3f} {:.3f} {:.3f}\n", base + at + i * 4,
                                               chunk[i], chunk[i + 1], chunk[i + 2]);
                            hits++;
                        }
                    }
                }
            }
            LOG_INFO(Core, "findvec done: {} hits in {} areas", hits, areas.size());
        } else if (action.starts_with("dumpimg=")) {
            // Test: the game image's data section to <user>/imgdump_<label>.bin (pages that
            // are not mapped come out as zeros), for diffing game state between moments.
            constexpr u64 DataStart = 0x1604000, DataEnd = 0x3957000, Page = 0x1000;
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            std::ofstream out{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                                  ("imgdump_" + action.substr(8) + ".bin"),
                              std::ios::binary};
            static const std::vector<char> zeros(Page);
            for (u64 at = DataStart; module && at < DataEnd; at += Page) {
                const VAddr address = module->GetBaseAddress() + at;
                out.write(Core::Memory::Instance()->IsValidMapping(address, Page)
                              ? reinterpret_cast<const char*>(address)
                              : zeros.data(),
                          Page);
            }
        } else if (action.starts_with("poke=") || action.starts_with("peek=") ||
                   action.starts_with("pokef=") || action.starts_with("dumpat=") ||
                   action.starts_with("pokeb=") ||
                   action.starts_with("watch=")) {
            // Test: write or log a 32-bit word of the game's image ("poke=325f430:3" sets game
            // variable 8; offsets as the eboot image has them).
            // "peek=2ef8c08>80>6e8" follows pointers: the word at offset 0x6e8 of what the
            // pointer at 0x80 of what the pointer at image offset 0x2ef8c08 points to.
            // "pokef=<path>:<float>" writes a float; "dumpat=<path>:<hex size>" writes that many
            // bytes from there to <user>/dumpat_<n>.bin.
            const auto colon = action.find(':');
            const auto from = action.find('=') + 1;
            std::string path =
                action.substr(from, colon == std::string::npos ? colon : colon - from);
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            auto* memory = Core::Memory::Instance();
            const auto step = path.find('>');
            const u64 offset = std::stoull(path.substr(0, step), nullptr, 16);
            VAddr address = module ? module->GetBaseAddress() + offset : 0;
            for (auto at = step; address && at != std::string::npos;) {
                const auto next = path.find('>', at + 1);
                const u64 field = std::stoull(path.substr(at + 1, next - at - 1), nullptr, 16);
                address = memory->IsValidMapping(address, 8)
                              ? *reinterpret_cast<const u64*>(address) + field
                              : 0;
                at = next;
            }
            if (action.starts_with("watch=") && address && colon != std::string::npos) {
                Sotc::Watch::Arm(address, std::stoull(action.substr(colon + 1), nullptr, 16),
                                 module->GetBaseAddress(), 0x1600000, 8);
            } else if (action.starts_with("dumpat=") && address && colon != std::string::npos) {
                static int dumps = 0;
                const u64 size = std::stoull(action.substr(colon + 1), nullptr, 16);
                std::vector<char> bytes(size);
                SIZE_T got = 0;
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                  bytes.data(), size, &got);
                std::ofstream{Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
                                  fmt::format("dumpat_{}.bin", dumps),
                              std::ios::binary}
                    .write(bytes.data(), got);
                LOG_INFO(Core, "script {}: {:#x} bytes from {:#x} to dumpat_{}.bin", action, got,
                         address, dumps++);
            } else if (address && memory->IsValidMapping(address, 4)) {
                auto* word = reinterpret_cast<u32*>(address);
                if (action.starts_with("pokeb=") && colon != std::string::npos) {
                    // "pokeb=<path>:+1" adds to the byte there; "pokeb=<path>:5" sets it.
                    auto* byte = reinterpret_cast<u8*>(word);
                    const auto value = action.substr(colon + 1);
                    *byte = value.starts_with('+') ? u8(*byte + std::stoi(value.substr(1)))
                                                   : u8(std::stoi(value, nullptr, 0));
                } else if (action.starts_with("pokef=") && colon != std::string::npos) {
                    *reinterpret_cast<float*>(word) = std::stof(action.substr(colon + 1));
                } else if (action.starts_with("poke=") && colon != std::string::npos) {
                    *word = static_cast<u32>(std::stoll(action.substr(colon + 1), nullptr, 0));
                }
                LOG_INFO(Core, "script {}: {:#x} = {:#x} ({})", action, address, *word,
                         *reinterpret_cast<const float*>(word));
            }
        } else if (action.starts_with("ach=")) {
            // Test: unlock an achievement as the game would.
            Sotc::Achievements::Unlock(std::stoi(action.substr(4)), nullptr);
        } else if (action.starts_with("key=")) {
            // A keyboard key or mouse button by its binding-file name, sent as SDL events so it
            // goes through the same binding layer as a real device.
            // "key=w*2000" holds the key for 2000 ms; the default is 250 ms.
            auto name = action.substr(4);
            u64 hold_ms = 250;
            if (const auto star = name.find('*'); star != std::string::npos) {
                hold_ms = std::stoull(name.substr(star + 1));
                name = name.substr(0, star);
            }
            if (name == "wheelup" || name == "wheeldown") {
                // A real wheel notch, as the mouse sends it.
                SDL_Event wheel{};
                wheel.type = SDL_EVENT_MOUSE_WHEEL;
                wheel.wheel.y = name == "wheelup" ? 1.0f : -1.0f;
                SDL_PushEvent(&wheel);
            } else if (const auto key = Input::string_to_keyboard_key_map.find(name);
                key != Input::string_to_keyboard_key_map.end()) {
                const bool is_mouse = name.ends_with("button");
                SDL_Event key_event{};
                if (is_mouse) {
                    key_event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                    key_event.button.button = static_cast<Uint8>(key->second);
                    key_event.button.down = true;
                } else {
                    key_event.type = SDL_EVENT_KEY_DOWN;
                    key_event.key.windowID = g_script_window;
                    key_event.key.key = key->second;
                    key_event.key.scancode = SDL_GetScancodeFromKey(key->second, nullptr);
                    key_event.key.down = true;
                }
                SDL_PushEvent(&key_event);
                held_keys.push_back({key_event, now + hold_ms});
            }
        } else if (action.starts_with("pad=") && g_virtual_pad) {
            // "pad=south*300": gamepad button by its SDL name (south, east, west, north, start,
            // back, dpup, leftshoulder, ...). "pad=lefty:-1*3000": stick or trigger axis held
            // at a value from -1 to 1.
            auto name = action.substr(4);
            u64 hold_ms = 250;
            if (const auto star = name.find('*'); star != std::string::npos) {
                hold_ms = std::stoull(name.substr(star + 1));
                name = name.substr(0, star);
            }
            if (const auto colon = name.find(':'); colon != std::string::npos) {
                const auto axis = SDL_GetGamepadAxisFromString(name.substr(0, colon).c_str());
                const float value = std::stof(name.substr(colon + 1));
                if (axis != SDL_GAMEPAD_AXIS_INVALID) {
                    SDL_SetJoystickVirtualAxis(g_virtual_pad, axis, Sint16(value * 32767));
                    held_pad.push_back({int(axis), true, now + hold_ms});
                }
            } else if (const auto button = SDL_GetGamepadButtonFromString(name.c_str());
                       button != SDL_GAMEPAD_BUTTON_INVALID) {
                const bool ok = SDL_SetJoystickVirtualButton(g_virtual_pad, button, true);
                LOG_INFO(Input, "virtual pad button {} ({}) down: {}", name, int(button), ok);
                held_pad.push_back({int(button), false, now + hold_ms});
            } else {
                LOG_ERROR(Input, "virtual pad: unknown button {}", name);
            }
        } else if (action.starts_with("mouse=")) {
            // "mouse=20*1500": mouse moving right 20 counts per poll for 1500 ms (negative =
            // left), through the same mouse-to-stick conversion as a real mouse. "mouse=20/-10*800"
            // also moves it up 10 (positive = down).
            const auto star = action.find('*');
            const auto values = action.substr(6, star - 6);
            const auto slash = values.find('/');
            Input::test_mouse_delta[0] = std::stof(values.substr(0, slash));
            Input::test_mouse_delta[1] =
                slash == std::string::npos ? 0.0f : std::stof(values.substr(slash + 1));
            mouse_release_at =
                now + (star == std::string::npos ? 250 : std::stoull(action.substr(star + 1)));
        } else if (action == "fault") {
            // Test aid: a fault nothing handles, to see what the program does about one.
            volatile int* nowhere = nullptr;
            *nowhere = 1;
        } else if (action.starts_with("blank=")) {
            Sotc::Screens::SetSkipProbe(std::stoull(action.substr(6), nullptr, 16));
        } else if (action.starts_with("texuse=")) {
            Sotc::Screens::DumpUsed(action.substr(7));
        } else if (action.starts_with("click=")) {
            // "click=0.08/0.55": a left click at that place of the window (fractions of its
            // width and height), as the mouse sends it.
            const auto comma = action.find('/');
            int w = 0, h = 0;
            SDL_GetWindowSize(SDL_GetWindowFromID(g_script_window), &w, &h);
            if (comma != std::string::npos && w > 0 && h > 0) {
                SDL_Event click{};
                click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                click.button.windowID = g_script_window;
                click.button.button = SDL_BUTTON_LEFT;
                click.button.down = true;
                click.button.x = std::stof(action.substr(6, comma - 6)) * w;
                click.button.y = std::stof(action.substr(comma + 1)) * h;
                SDL_PushEvent(&click);
                held_keys.push_back({click, now + 120});
            }
        } else if (action.starts_with("hover=")) {
            // "hover=0.08/0.55": the pointer moved to that place of the window.
            const auto slash = action.find('/');
            if (slash != std::string::npos) {
                Sotc::MenuExt::TestPointer(std::stof(action.substr(6, slash - 6)),
                                           std::stof(action.substr(slash + 1)));
            }
        } else if (action.starts_with("mousey=")) {
            // The same, moving down (negative = up).
            const auto star = action.find('*');
            Input::test_mouse_delta[1] = std::stof(action.substr(7, star - 7));
            mouse_release_at =
                now + (star == std::string::npos ? 250 : std::stoull(action.substr(star + 1)));
        }
    }
    std::erase_if(held_pad, [now](const HeldPad& input) {
        if (now < input.release_at) {
            return false;
        }
        if (input.is_axis) {
            SDL_SetJoystickVirtualAxis(g_virtual_pad, input.index, 0);
        } else {
            SDL_SetJoystickVirtualButton(g_virtual_pad, input.index, false);
        }
        return true;
    });
    if (mouse_release_at && now >= mouse_release_at) {
        Input::test_mouse_delta[0] = 0.0f;
        Input::test_mouse_delta[1] = 0.0f;
        mouse_release_at = 0;
    }
    std::erase_if(held_keys, [now](HeldKey& key) {
        if (now < key.release_at) {
            return false;
        }
        const bool is_mouse = key.event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        key.event.type = is_mouse ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_KEY_UP;
        (is_mouse ? key.event.button.down : key.event.key.down) = false;
        SDL_PushEvent(&key.event);
        return true;
    });
    return interval;
}

static Uint32 SDLCALL PollControllerLightColour(void* userdata, SDL_TimerID timer_id,
                                                Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollLightColour();
    return interval;
}

WindowSDL::WindowSDL(s32 width_, s32 height_, Input::GameControllers* controllers_,
                     std::string_view window_title)
    : width{width_}, height{height_}, controllers{*controllers_} {
    if (!SDL_SetHint(SDL_HINT_APP_NAME, "Shadow of the Colossus")) {
        UNREACHABLE_MSG("Failed to set SDL window hint: {}", SDL_GetError());
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        UNREACHABLE_MSG("Failed to initialize SDL video subsystem: {}", SDL_GetError());
    }
    // On macOS, the future Intel compatibility environment does not include camera frameworks.
    // Just skip initializing it entirely, no point in splitting old vs new OS versions here.
#ifndef __APPLE__
    if (!SDL_Init(SDL_INIT_CAMERA)) {
        LOG_ERROR(Input, "Failed to initialize SDL camera subsystem: {}", SDL_GetError());
    }
#endif
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          std::string(window_title).c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, "flags", SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    // Creating the window directly in fullscreen avoids a visible windowed -> fullscreen
    // transition on startup. SDL sizes the window to the display and keeps the requested
    // width/height as the windowed size to restore when leaving fullscreen.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           EmulatorSettings.IsFullScreen());
    // SOTC_HEADLESS=1 keeps the window hidden so automated runs do not take over the desktop.
    const bool headless = std::getenv("SOTC_HEADLESS") != nullptr;
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, headless);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window == nullptr) {
        UNREACHABLE_MSG("Failed to create window handle: {}", SDL_GetError());
    }

    SDL_SetWindowMinimumSize(window, 640, 360);

    bool error = false;
    const SDL_DisplayID displayIndex = SDL_GetDisplayForWindow(window);
    if (displayIndex == 0) {
        LOG_ERROR(Frontend, "Error getting display index: {}", SDL_GetError());
        error = true;
    }
    const SDL_DisplayMode* displayMode;
    if ((displayMode = SDL_GetCurrentDisplayMode(displayIndex)) == 0) {
        LOG_ERROR(Frontend, "Error getting display mode: {}", SDL_GetError());
        error = true;
    }
    if (!error) {
        SDL_SetWindowFullscreenMode(
            window, EmulatorSettings.GetFullScreenMode() == "Fullscreen" ? displayMode : NULL);
    }
    SDL_SetWindowFullscreen(window, EmulatorSettings.IsFullScreen());
    SDL_SyncWindow(window);
    // The window geometry is only final once the fullscreen transition has settled; refresh
    // the cached size so the first swapchain and the splashscreen use the real drawable size.
    SDL_GetWindowSizeInPixels(window, &width, &height);

    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

#if defined(SDL_PLATFORM_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#elif defined(SDL_PLATFORM_LINUX) || defined(__FreeBSD__)
    // SDL doesn't have a platform define for FreeBSD AAAAAAAAAA
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        window_info.render_surface = (void*)SDL_GetNumberProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        window_info.render_surface = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    }
#elif defined(SDL_PLATFORM_MACOS)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#endif
    // input handler init-s
    Input::ControllerOutput::LinkJoystickAxes();
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));

    // A hidden test window never has focus, so its scripted gamepad needs background input.
    if (EmulatorSettings.IsBackgroundControllerInput() || std::getenv("SOTC_HEADLESS")) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::SetIcon(std::span<const u8> png_data) {
    if (png_data.empty()) {
        LOG_WARNING(Core, "No window icon data available, using default icon.");
        SetDefaultWindowIcon(window);
        return;
    }
    SetWindowIcon(window, std::vector<u8>(png_data.begin(), png_data.end()));
}

void WindowSDL::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    if (!SDL_WaitEvent(&event)) {
        return;
    }

    if (Libraries::Mouse::PushSDLEvent(event) || Libraries::Keyboard::PushSDLEvent(event)) {
        return;
    }

    if (ImGui::Core::ProcessEvent(&event)) {
        return;
    }

    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        OnResize();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_WHEEL_OFF:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Button prompts follow the device in use.
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            VideoCore::g_prompt_set = VideoCore::PromptSet::Keyboard;
            Sotc::Challenge::OnPlayerInput();
        }
        OnKeyboardMouseInput(&event);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        controllers.TryOpenSDLControllers();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(event.gaxis.value) > 16000)) {
            const SDL_JoystickID id = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
                                          ? event.gbutton.which
                                          : event.gaxis.which;
            Sotc::Challenge::OnPlayerInput();
            switch (SDL_GetGamepadTypeForID(id)) {
            case SDL_GAMEPAD_TYPE_PS3:
            case SDL_GAMEPAD_TYPE_PS4:
            case SDL_GAMEPAD_TYPE_PS5:
                VideoCore::g_prompt_set = VideoCore::PromptSet::PlayStation;
                break;
            default:
                VideoCore::g_prompt_set = VideoCore::PromptSet::Xbox;
                break;
            }
        }
        OnGamepadEvent(&event);
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    case SDL_EVENT_QUIT_DIALOG:
        Overlay::ToggleQuitWindow();
        break;
    case SDL_EVENT_TOGGLE_FULLSCREEN: {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(window, 0);
        } else {
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
        }
        break;
    }
    case SDL_EVENT_TOGGLE_PAUSE:
        if (DebugState.IsGuestThreadsPaused()) {
            LOG_INFO(Frontend, "Game Resumed");
            DebugState.ResumeGuestThreads();
        } else {
            LOG_INFO(Frontend, "Game Paused");
            DebugState.PauseGuestThreads();
        }
        break;
    case SDL_EVENT_CHANGE_CONTROLLER:
        UNREACHABLE_MSG("todo");
        break;
    case SDL_EVENT_TOGGLE_SIMPLE_FPS:
        Overlay::ToggleSimpleFps();
        break;
    case SDL_EVENT_TOGGLE_FRIENDS:
        // The friends hotkey (F2 by default) opens the PC menu in this build.
        if (Sotc::Achievements::IsPanelOpen()) {
            Sotc::Achievements::TogglePanel();
        } else {
            Sotc::Menu::HotkeyToggle();
        }
        break;
    case SDL_EVENT_RELOAD_INPUTS:
        Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
        break;
    case SDL_EVENT_MOUSE_TO_JOYSTICK:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Joystick));
        break;
    case SDL_EVENT_MOUSE_TO_GYRO:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Gyro));
        break;
    case SDL_EVENT_MOUSE_TO_TOUCHPAD:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Touchpad));
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(), false);
        break;
    case SDL_EVENT_ADD_VIRTUAL_USER:
        for (int i = 0; i < 4; i++) {
            if (controllers[i]->user_id == -1) {
                auto u = UserManagement.GetUserByPlayerIndex(i + 1);
                if (!u) {
                    break;
                }
                controllers[i]->user_id = u->user_id;
                controllers[i]->ConnectController(controllers[i]->m_sdl_gamepad);
                UserManagement.LoginUser(u, i + 1);
                break;
            }
        }
        break;
    case SDL_EVENT_REMOVE_VIRTUAL_USER:
        LOG_INFO(Input, "Remove user");
        for (int i = 3; i >= 0; i--) {
            if (controllers[i]->user_id != -1) {
                UserManagement.LogoutUser(UserManagement.GetUserByID(controllers[i]->user_id));
                controllers[i]->DisconnectController();
                controllers[i]->user_id = -1;
                break;
            }
        }
        break;
    case SDL_EVENT_RDOC_CAPTURE:
        if (VideoCore::IsRenderDocLoaded()) {
            VideoCore::TriggerCapture();
        } else {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        }
        break;
    case SDL_EVENT_SCREENSHOT_WITH_OVERLAYS:
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        break;
    default:
        break;
    }
}

void WindowSDL::InitTimers() {
    for (int i = 0; i < 4; ++i) {
        SDL_AddTimer(4, &PollController, controllers[i]);
    }
    SDL_AddTimer(8, Input::MousePolling, (void*)controllers[0]);
    Sotc::MenuExt::SetPad(controllers[0]);
    if (std::getenv("SOTC_SCRIPT")) {
        g_script_window = SDL_GetWindowID(window);
        if (std::getenv("SOTC_VPAD")) {
            AttachVirtualPad();
        }
        SDL_AddTimer(50, &ScriptTick, controllers[0]);
    }
    // Mouse look is on from the start; the mouse-to-joystick hotkey still toggles it. A hidden
    // test window must not capture the pointer.
    const bool capture = Input::ToggleMouseModeTo(Input::MouseMode::Joystick) &&
                         std::getenv("SOTC_HEADLESS") == nullptr;
    SDL_SetWindowRelativeMouseMode(window, capture);
}

void WindowSDL::RequestKeyboard() {
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StartTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
    keyboard_grab++;
}

void WindowSDL::ReleaseKeyboard() {
    ASSERT(keyboard_grab > 0);
    keyboard_grab--;
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StopTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
}

void WindowSDL::OnResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

Uint32 wheelOffCallback(void* og_event, Uint32 timer_id, Uint32 interval) {
    SDL_Event off_event = *(SDL_Event*)og_event;
    off_event.type = SDL_EVENT_MOUSE_WHEEL_OFF;
    SDL_PushEvent(&off_event);
    delete (SDL_Event*)og_event;
    return 0;
}

Uint32 menuKeyUpCallback(void* key, Uint32 timer_id, Uint32 interval) {
    SDL_Event up{};
    up.type = SDL_EVENT_KEY_UP;
    up.key.key = static_cast<SDL_Keycode>(reinterpret_cast<uintptr_t>(key));
    SDL_PushEvent(&up);
    return 0;
}

void WindowSDL::OnKeyboardMouseInput(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    if (Sotc::Menu::CaptureInput(event)) {
        return;
    }
    // An open overlay panel takes the keyboard; releases still pass so nothing stays held.
    // The menu hotkey itself goes through so it can close what it opened.
    if (Overlay::PanelBlocksMouse() &&
        ((event->type == SDL_EVENT_KEY_DOWN && event->key.key != SDLK_F2) ||
         event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_WHEEL)) {
        return;
    }

    // The weapon wheel takes the mouse buttons while it is open (the game runs on, slowed).
    if (Sotc::Wheel::IsOpen() &&
        (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_WHEEL)) {
        return;
    }

    // One step per turn of the wheel: a free-spinning wheel sends a burst of notches, and each
    // would change the weapon or move the selection again.
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        static u64 last_wheel_ms = 0;
        const u64 now_ms = SDL_GetTicks();
        if (now_ms - last_wheel_ms < 220) {
            return;
        }
        last_wheel_ms = now_ms;
    }

    const bool is_mouse =
        event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event->type == SDL_EVENT_MOUSE_WHEEL || event->type == SDL_EVENT_MOUSE_WHEEL_OFF;
    if (is_mouse && Overlay::MenuCursorActive()) {
        // An overlay panel owns the mouse while it is open; button releases still pass so
        // nothing stays held.
        if (Overlay::PanelBlocksMouse()) {
            if (event->type != SDL_EVENT_MOUSE_BUTTON_UP) {
                return;
            }
        } else if (Overlay::InTitleMenu() || Sotc::Screens::PauseMenu() ||
                   Sotc::Screens::GameOver()) {
            // The game's menus only know pad buttons: left click confirms, right click goes
            // back, the wheel moves the selection.
            SDL_Event key{};
            if (event->type == SDL_EVENT_MOUSE_WHEEL) {
                if (event->wheel.y == 0) {
                    return;
                }
                key.type = SDL_EVENT_KEY_DOWN;
                key.key.key = event->wheel.y > 0 ? SDLK_UP : SDLK_DOWN;
                key.key.down = true;
                SDL_AddTimer(60, menuKeyUpCallback,
                             reinterpret_cast<void*>(static_cast<uintptr_t>(key.key.key)));
            } else if (event->type == SDL_EVENT_MOUSE_WHEEL_OFF) {
                return;
            } else if (event->button.button == SDL_BUTTON_LEFT && event->button.down &&
                       [&] {
                           // A click on a menu line chooses that line.
                           int w = 0, h = 0;
                           SDL_GetWindowSize(window, &w, &h);
                           return w > 0 && h > 0 &&
                                  Sotc::MenuExt::OnClick(event->button.x / float(w),
                                                         event->button.y / float(h));
                       }()) {
                return;
            } else if (event->button.button == SDL_BUTTON_LEFT && event->button.down &&
                       [&] {
                           // Away from the chosen line a click does nothing.
                           int w = 0, h = 0;
                           SDL_GetWindowSize(window, &w, &h);
                           return w > 0 && h > 0 &&
                                  !Sotc::MenuExt::PointerOnCursor(event->button.x / float(w),
                                                                  event->button.y / float(h));
                       }()) {
                return;
            } else if (event->button.button == SDL_BUTTON_LEFT ||
                       event->button.button == SDL_BUTTON_RIGHT) {
                key.type = event->button.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                key.key.key =
                    event->button.button == SDL_BUTTON_LEFT ? SDLK_RETURN : SDLK_BACKSPACE;
                key.key.down = event->button.down;
            } else {
                return;
            }
            OnKeyboardMouseInput(&key);
            return;
        }
    }

    // get the event's id, if it's keyup or keydown
    const bool input_down = event->type == SDL_EVENT_KEY_DOWN ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event->type == SDL_EVENT_MOUSE_WHEEL;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // if it's a wheel event, make a timer that turns it off after a set time
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        const SDL_Event* copy = new SDL_Event(*event);
        SDL_AddTimer(33, wheelOffCallback, (void*)copy);
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    // update bindings
    if (inputs_changed) {
        Input::ActivateOutputsFromInputs();
    }
}

void WindowSDL::OnGamepadEvent(const SDL_Event* event) {
    bool input_down = event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
                      event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    // An open overlay panel takes the pad buttons too.
    if (Overlay::PanelBlocksMouse() && event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        return;
    }
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // the touchpad button shouldn't be rebound to anything else,
    // as it would break the entire touchpad handling
    // You can still bind other things to it though
    if (event->gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gbutton.which)]->Button(
            OrbisPadButtonDataOffset::TouchPad, input_down);
        return;
    }

    u8 gamepad;

    switch (event->type) {
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        switch ((SDL_SensorType)event->gsensor.sensor) {
        case SDL_SENSOR_GYRO:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateGyro(event->gsensor.data);
            }
            break;
        case SDL_SENSOR_ACCEL:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateAcceleration(event->gsensor.data);
            }
            break;
        default:
            break;
        }
        return;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gtouchpad.which)]
            ->SetTouchpadState(event->gtouchpad.finger,
                               event->type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP, event->gtouchpad.x,
                               event->gtouchpad.y);
        return;
    default:
        break;
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    if (inputs_changed) {
        // update bindings
        Input::ActivateOutputsFromInputs();
    }
}

#ifndef __APPLE__
void SetWindowIcon(SDL_Window* window, const std::vector<u8>& png) {
    int imageWidth = 0;
    int imageHeight = 0;
    constexpr int numChannels = 4;
    unsigned char* imageData = stbi_load_from_memory(png.data(), png.size(), &imageWidth,
                                                     &imageHeight, nullptr, numChannels);
    if (imageData == nullptr) {
        LOG_ERROR(Core, "Failed to load window icon image: {}", stbi_failure_reason());
        return;
    }
    SCOPE_EXIT {
        stbi_image_free(imageData);
    };

    SDL_Surface* surface = SDL_CreateSurfaceFrom(imageWidth, imageHeight, SDL_PIXELFORMAT_RGBA32,
                                                 imageData, imageWidth * numChannels);
    if (surface == nullptr) {
        LOG_ERROR(Core, "Failed to create SDL surface for window icon: {}", SDL_GetError());
    }
    if (!SDL_SetWindowIcon(window, surface)) {
        LOG_ERROR(Core, "Failed to set SDL window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(surface);
}
#endif

void SetDefaultWindowIcon(SDL_Window* window) {
    const auto resource = cmrc::res::get_filesystem();
    const auto file = resource.open("src/resources/shadps4.png");
    const std::vector<u8> texData = std::vector<u8>(file.begin(), file.end());
    SetWindowIcon(window, texData);
}

} // namespace Frontend
