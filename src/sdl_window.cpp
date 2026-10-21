// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
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
    float from[40][4], to[40][4], hit[40][4]; // hit: where the line stops (to when clear)
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
static u64 CvcFindEffect(u64 image, std::string_view name, bool list = false) {
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
    const u64 live = CvcFindEffect(image, name);
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

static void CvcContactWork(u64 image, float dt) {
    auto& c = g_cvc_contact;
    std::vector<std::array<float, 3>> now[2];
    for (int k = 0; k < 2; k++) {
        const u64 object = g_cvc_body[k].object.load();
        if (!object || *reinterpret_cast<const volatile u8*>(object + 0xc) != 9) {
            c.before[k].clear();
            continue;
        }
        const auto* at = reinterpret_cast<const volatile float*>(object + 0x130);
        const u64 matrices = *reinterpret_cast<const volatile u64*>(object + 0x120 + 0x198);
        for (int i = 0; matrices && i < 128 && Core::Memory::Instance()->IsValidMapping(matrices + i * 64, 64); i++) {
            const auto* m = reinterpret_cast<const volatile float*>(matrices + i * 64 + 0x30);
            if (m[3] != 1.0f || std::hypot(m[0] - at[0], m[2] - at[2]) > 400.0f) {
                break;
            }
            now[k].push_back({m[0], m[1], m[2]});
        }
    }
    const int k = c.attacker.load();
    const u64 filter = *reinterpret_cast<const volatile u64*>(image + 0x1593ed8);
    if (k >= 0 && !c.found.load() && filter && dt > 0.0f && now[k].size() == c.before[k].size() && !now[1 - k].empty()) {
        using LineTest = u8 PS4_SYSV_ABI (*)(void* hit, const float* from, const float* to, u64 filter, u32 single,
                                             u64 ignore);
        const auto test = reinterpret_cast<LineTest>(image + 0x7a7e40);
        const auto nearest = [](const std::vector<std::array<float, 3>>& bones, const float* p) {
            float best = 1e9f;
            for (const auto& b : bones) {
                best = std::min(best, std::hypot(b[0] - p[0], b[1] - p[1], b[2] - p[2]));
            }
            return best;
        };
        // Not its root and spine (0-5): a lunging body is not a blow; limbs, head and weapon are.
        for (size_t i = 6; i < now[k].size() && !c.found.load(); i++) {
            const auto& a = c.before[k][i];
            const auto& b = now[k][i];
            const float travel = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            if (travel / dt < c.least.load() || travel / dt > 2000.0f) {
                continue;
            }
            {
                float best = 1e9f;
                for (const auto& v : now[1 - k]) {
                    best = std::min(best, std::hypot(b[0] - v[0], b[1] - v[1], b[2] - v[2]));
                }
                if (best < c.closest.load()) {
                    c.closest = best, c.closest_bone = int(i);
                }
            }
            // Against its body: the sweep's nearest approach to any of its bones.
            {
                const float d[3]{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const float len2 = std::max(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1e-6f);
                for (const auto& v : now[1 - k]) {
                    const float t = std::clamp(((v[0] - a[0]) * d[0] + (v[1] - a[1]) * d[1] + (v[2] - a[2]) * d[2]) / len2,
                                               0.0f, 1.0f);
                    const float p[3]{a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t};
                    if (std::hypot(p[0] - v[0], p[1] - v[1], p[2] - v[2]) < c.body.load()) {
                        // Where it meets the body's surface, from that bone toward the sweep.
                        const float n = std::max(std::hypot(p[0] - v[0], p[1] - v[1], p[2] - v[2]), 0.001f);
                        for (int q = 0; q < 3; q++) {
                            c.point[q] = v[q] + (p[q] - v[q]) / n * std::min(n, c.body.load());
                        }
                        c.speed = travel / dt, c.bone = int(i);
                        c.found = true;
                        LOG_INFO(Core, "cvccontact: {} bone {} touched {}'s body at {:.0f} {:.0f} {:.0f}", k ? "B" : "A", i,
                                 k ? "A" : "B", c.point[0], c.point[1], c.point[2]);
                        break;
                    }
                }
                if (c.found.load()) {
                    break;
                }
            }
            alignas(16) const float from[4]{a[0], a[1], a[2], 1.0f};
            alignas(16) const float to[4]{b[0], b[1], b[2], 1.0f};
            for (const u32 single : {1u, 0u}) {
                alignas(16) float hit[12]{};
                // The attacker's own body left out (the test's last value); the hit names the object
                // it met (hit +0x20), which must be the other colossus.
                if (test(hit, from, to, filter, single, g_cvc_body[k].object.load())) {
                    continue; // clear
                }
                const u64 met = *reinterpret_cast<const u64*>(hit + 8);
                const float on_victim = nearest(now[1 - k], hit);
                if (met == g_cvc_body[1 - k].object.load()) {
                    std::memcpy(c.point, hit, 12);
                    c.speed = travel / dt, c.bone = int(i);
                    c.found = true;
                    LOG_INFO(Core, "cvccontact: {} bone {} met {} at {:.0f} {:.0f} {:.0f} ({:.1f} from its bones), hit words "
                             "{:#x} {:#x} {:#x} {:#x}", k ? "B" : "A", i, k ? "A" : "B", hit[0], hit[1], hit[2], on_victim,
                             *reinterpret_cast<const u32*>(hit + 4), *reinterpret_cast<const u32*>(hit + 8),
                             *reinterpret_cast<const u64*>(hit + 8), *reinterpret_cast<const u64*>(hit + 10));
                    break;
                }
            }
        }
    }
    c.before[0].swap(now[0]);
    c.before[1].swap(now[1]);
}

static void CvcBodyWork(u64 image) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point before = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(Clock::now() - before).count(), 0.0f, 0.05f);
    before = Clock::now();
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
        body.frames = motion ? float(reinterpret_cast<s32 PS4_SYSV_ABI (*)(u64)>(image + 0x6049e0)(motion + 0x128))
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
                delta[c * 2] = (body.push[c] + body.apart[c]) * dt;
                body.push[c] *= keep;
                if (std::abs(body.push[c]) < 0.05f) {
                    body.push[c] = 0.0f;
                }
            }
        }
        if (delta[0] != 0.0f || delta[2] != 0.0f) {
            MoveCharacter(object, delta);
        }
        // Turning eased toward the rate asked for (up to 2 rad/s² either way): no sudden swing.
        const float want = body.turn.load();
        body.turning += std::clamp(want - body.turning, -2.0f * dt, 2.0f * dt);
        if (std::abs(body.turning) > 0.001f) {
            RotateCharacter(object, body.turning * dt);
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
    CvcContactWork(image, dt);
}

/// Asks for a motion to be started on a colossus at the next camera update (game thread).
/// `blend`: seconds it crossfades from the pose it had (the game's own blend, component +0x980..;
/// 0: it snaps to the new motion's first frame).
// Guards the motion requests: one waiting per colossus, the newest (two started in the same
// frame, the second blending over the first's first frame, crashed the game's object update at
// 0x61af2f).
static std::mutex g_cvc_motion_lock;

static void CvcPlayMotion(u64 image, u64 object, u64 motion, float blend = 0.0f) {
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
                        const float y = pa[1];
                        if (y < floor_y) {
                            const float lift[3]{0.0f, floor_y - y, 0.0f};
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
                            missing[slot == 0x1593b10] = 0;
                            continue; // fine, or cleared on purpose (defeat)
                        }
                        if (current && ++missing[slot == 0x1593b10] < 30) {
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
                                         "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} (angle {:.2f}, reach {:.0f})",
                                         clear_part[0], clear_part[1], clear_part[2], clear_part[3],
                                         clear_part[4], clear_part[5], clear_part[6], clear_part[7],
                                         clear_part[8], clear_part[9], clear_part[10], clear_part[11],
                                         tested_angle, reach);
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
                                       (visible[k] ? 0.0f : 1.5f);
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
                            rays.count = 38, rays.image = image, tested_angle = angle;
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
                        // Never inside or against a colossus: at least 22 units from every bone.
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
                                               m, sizeof(m), &got) ||
                            m[3] != 1.0f || std::hypot(m[0] - pos[0], m[2] - pos[2]) > 400.0f) {
                            break;
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
                    };
                    static const std::unordered_map<int, Moves> kinds{
                        // Gaius: Knight_attack_1 (slam), _2; chest / belly, head / face, back / neck
                        // damage; Knight_walk; Knight_Standing_left/right_rotation; Knight_Standing;
                        // Knight_retreat.
                        {2, {{{0x1d3a5f0, 4500}, {0x1d3b620, 6000}},
                             {0x1d3cd40, 0x1d3d1e0}, {0x1d3c650, 0x1d3c8a0}, {0x1d3cf90, 0x1d3caf0},
                             250.0f, 0x1d38340, 8.0f, 1, 0x1d37c50, 0x1d37a00, 0.0f, 0x1d377b0, 0x1d387e0, 1.0f, 0.85f, 8.0f, false, 0, 0,
                             // attack_1_Introduction > attack_one_stop_(success); attack_2_Introduction >
                             // _loop > _quit
                             {{0x1d3a5f0, 0x1d3a840}, {0x1d3b620, 0x1d3b870}, {0x1d3b870, 0x1d3bac0}}, false,
                             // (no knockdown: Knight_fall's own effects crash the game's effect update,
                             // 0x693f57); Knight_Search
                             {}, {0x1d38ed0}}}, // a wide sword sweep
                        // Basaran: hand attacks (slow slams); Turtle_shudder; Turtle_walking_1;
                        // Turtle_Standing_left/right_rotation_1; Turtle_Standing; Turtle_retreat_1.
                        {8, {{{0x1d59230, 3500}, {0x1d59480, 3000}},
                             {0x1d5a4b0}, {0x1d5a4b0}, {0x1d5a4b0},
                             250.0f, 0x1d553c0, 3.0f, -2, 0x1d54f20, 0x1d54a80, 30.0f, 0x1d51e90, 0x1d561a0, 1.7f, 0.9f, 10.0f, true, 0x1d59dc0, 0x1d59920,
                             // both_hands_attack_1 > hands_attack_2 (middle); each hands_attack_2 > its _3
                             {{0x1d59230, 0x1d59480}, {0x1d59480, 0x1d596d0}, {0x1d59920, 0x1d59b70},
                              {0x1d59dc0, 0x1d5a010}}, true,
                             // Turtle_overturn_the_beginning_(right) > turned_over_the_first_two > turned_over >
                             // overturned_loop (3 s on its back) > flip_over_stop_1 > _2; Turtle_Standing_1, _2
                             {{0x1d56640, 0}, {0x1d56890, 0}, {0x1d56f80, 0}, {0x1d571d0, 3000}, {0x1d57420, 0}, {0x1d57670, 0}},
                             {0x1d545e0, 0x1d54830}}},
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
                        // (Its hands come down about 20 ahead of its root, measured; the other's
                        // body, 8 around its root, reaches back over that from 30 ahead.)
                        const float* me = k ? (const float*)rb : (const float*)ra;
                        spot[0] = me[0] + fx * 30.0f, spot[1] = me[1], spot[2] = me[2] + fz * 30.0f;
                        return true;
                    };
                    // Crossfade per move: standing, walking and turning ease in; an attack and a
                    // flinch come in quicker.
                    const auto play = [&](int k, u64 motion, Act act, int ms) {
                        const float blend = act == Act::Attack ? 0.4f : act == Act::Flinch ? 0.25f
                                          : act == Act::Idle ? 0.6f : 0.35f;
                        CvcPlayMotion(image, objects[k], motion, blend);
                        fighter[k] = {act, motion, Clock::now(), Clock::now() + std::chrono::milliseconds(ms)};
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
                                LOG_INFO(Core, "cvcbrawl: {} missed (its moving bone {} came within {:.1f} of the other's bones)",
                                         k ? "B" : "A", g_cvc_contact.closest_bone.load(), g_cvc_contact.closest.load());
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
                        if (moves->strike_left && face(k, cosine, side) && cosine < 0.97f) {
                            motion = side > 0.0f ? moves->strike_left : moves->strike_right, ms = 3000;
                        }
                        (void)ms; // it ends when its last part does (Act::Attack below); 12 s at most
                        play(k, motion, Act::Attack, 12000);
                        attacking = k, landed = false, attack_from = Clock::now();
                        g_cvc_contact.closest = 1e9f, g_cvc_contact.closest_bone = -1;
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
                        } else if (closest >= moves->strike && moves->holds && [&] {
                                       // (The other already on the spot in front of its head: in reach.)
                                       float spot[3]{};
                                       const float* other = k ? (const float*)ra : (const float*)rb;
                                       return !front(k, spot) || std::hypot(other[0] - spot[0], other[2] - spot[2]) >= 8.0f;
                                   }()) {
                            // It waits; the other comes on into its reach (then it strikes). Or, from
                            // where it is, it fires its pulse at it.
                            const int j = 1 - k;
                            const Moves* other = kind_of(j);
                            if (moves->pulse && (closest > 20.0f || rng() % 100 < 45)) {
                                play(k, moves->idle, Act::Pulse, 6000);
                                pulse_phase = 0, landed = false, attack_from = Clock::now();
                                float head[3]{};
                                front(k, head);
                                const float* me = k ? (const float*)rb : (const float*)ra;
                                const float mouth[3]{(head[0] + me[0] * 2.0f) / 3.0f, me[1] + 14.0f, (head[2] + me[2] * 2.0f) / 3.0f};
                                // (Its own tts_pls_* charge and bolt effects are not resident in this arena, and
                                // Wander's sword effects s_* crash outside his hands: general sparks stand in.)
                                CvcEffect("npe/torch_spark.npe", mouth);
                                CvcEffect("npe/player_spark.npe", mouth);
                                CvcSound(rng() % 2 ? "kmvo_pulse_c2" : "kmvo_pulse_c4", mouth);
                                CvcSound("kame_pulse_c1", mouth);
                                LOG_INFO(Core, "cvcbrawl: {} charges its pulse ({:.0f} apart)", k ? "B" : "A", closest);
                            } else if (other && other->walk && fighter[j].act == Act::Idle) {
                                play(j, other->walk, Act::Walk, 8000);
                                fighter[j].bait = true;
                                LOG_INFO(Core, "cvcbrawl: {} holds its ground, {} comes on ({:.0f} apart)", k ? "B" : "A",
                                         j ? "B" : "A", closest);
                            } else {
                                attacking = -1; // again shortly
                            }
                        } else if (closest >= moves->strike && moves->walk && !moves->holds) {
                            play(k, moves->walk, Act::Walk, 7000);
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
                            const Moves* moves = kind_of(k);
                            if (!moves) {
                                continue;
                            }
                            float cosine = 0.0f, side = 0.0f;
                            const bool known = face(k, cosine, side);
                            switch (f.act) {
                            case Act::Attack:
                                // Turned onto the other while winding up, then the blow goes where it goes.
                                if (Clock::now() < attack_from + std::chrono::milliseconds(900)) {
                                    steer(k, 0.5f);
                                } else {
                                    g_cvc_body[k].turn = 0.0f;
                                }
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
                            case Act::Walk:
                                if (f.bait) {
                                    // Onto the spot in front of the other's head, where its blows land; then
                                    // it is the other's move.
                                    float spot[3]{};
                                    const bool known_spot = front(1 - k, spot);
                                    steer(k, 0.45f, known_spot ? spot : nullptr);
                                    const float* me = k ? (const float*)rb : (const float*)ra;
                                    const bool there = known_spot && std::hypot(me[0] - spot[0], me[2] - spot[2]) < 6.0f;
                                    if (there || closest < 2.0f) {
                                        stand(k);
                                        engage(1 - k);
                                    } else if (Clock::now() > f.until) {
                                        stand(k);
                                        attacking = -1;
                                    }
                                    break;
                                }
                                steer(k, 0.45f);
                                if (closest < moves->strike) {
                                    engage(k); // square to it: the attack; else round first
                                } else if (closest < moves->strike + 2.0f && known && cosine > moves->aim - 0.1f) {
                                    attack(k); // as good as there, roughly facing it
                                } else if (Clock::now() > f.until) {
                                    LOG_INFO(Core, "cvcbrawl: {} stops short ({:.0f} apart, facing {:.2f})", k ? "B" : "A",
                                             closest, cosine);
                                    stand(k);
                                }
                                break;
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
                                    if (f.heavy && !moves->down.empty() && rng() % 100 < 35) {
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
                                float head[3]{};
                                front(k, head);
                                const float* me = k ? (const float*)rb : (const float*)ra;
                                const float mouth[3]{(head[0] + me[0] * 2.0f) / 3.0f, me[1] + 14.0f,
                                                     (head[2] + me[2] * 2.0f) / 3.0f};
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
                        // A blow: k is attacking, past the wind-up, not landed yet, and one of its
                        // bones, moving fast and toward the other's body, reaches it.
                        // (Not while it walks up or turns: its feet are not a blow.)
                        const bool swinging = attacking == k && fighter[k].act == Act::Attack && !landed &&
                                              Clock::now() > attack_from + std::chrono::milliseconds(400);
                        if (!swinging && g_cvc_contact.attacker.load() == k) {
                            g_cvc_contact.attacker = -1;
                            g_cvc_contact.found = false;
                        }
                        const bool pulsed = pulse_hit_for == k;
                        if (!fell && !pulsed && (!swinging || now[k].size() != before[k].size() || now[1 - k].empty() ||
                                                 dt <= 0.0f)) {
                            continue;
                        }
                        // The blow: its swing meets the other's body in the game's own collision
                        // (CvcContactWork sweeps the moving bones on the game thread).
                        int hit_bone = -1;
                        float hit_speed = 0.0f, hit_gap = 0.0f;
                        std::array<float, 3> contact{};
                        if (pulsed) {
                            hit_bone = 0, hit_speed = 300.0f, contact = pulse_point; // its pulse struck
                            pulse_hit_for = -1;
                        } else if (swinging) {
                            const Moves* striker = kind_of(k);
                            g_cvc_contact.least = striker && striker->swing > 0.0f ? striker->swing : speed;
                            g_cvc_contact.body = kind_of(1 - k) ? kind_of(1 - k)->body : 8.0f;
                            g_cvc_contact.attacker = k;
                            if (g_cvc_contact.found.load()) {
                                hit_bone = g_cvc_contact.bone, hit_speed = g_cvc_contact.speed;
                                std::memcpy(contact.data(), g_cvc_contact.point, 12);
                                g_cvc_contact.attacker = -1;
                                g_cvc_contact.found = false;
                            }
                        }
                        if (hit_bone < 0 && !fell) {
                            continue;
                        }
                        auto* health = reinterpret_cast<float*>(managers[1 - k] + 0x50);
                        if (hit_bone >= 0) {
                            landed = true;
                            // Below 1: a share of the victim's full health (any pair, same number of blows).
                            const float set = k == 0 ? dmg_a : dmg_b;
                            const float damage =
                                set < 1.0f ? set * *reinterpret_cast<const float*>(managers[1 - k] + 0x4c) : set;
                            *health = std::max(0.0f, *health - damage);
                            // The impact: both freeze for a moment, the view shakes, then the one hit
                            // flinches (one of its own, by where the blow landed: the head, from
                            // behind, else the body) and is pushed away along the blow, and a heavy blow
                            // makes it step back after; the striker rocks back a little.
                            const Moves* victim_moves = kind_of(1 - k);
                            const Moves* striker_moves = kind_of(k);
                            const float strength = std::clamp(hit_speed / 120.0f, 0.5f, 1.6f);
                            const bool heavy = *health > 0.0f && (strength > 1.1f || rng() % 100 < 50);
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
                            // The flinch (not from the knockout blow: the death follows that).
                            u64 flinch = 0;
                            if (*health > 0.0f && victim_moves && fighter[1 - k].act != Act::Down) { // (down: it lies on)
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
                                                  Clock::now() + std::chrono::milliseconds(stop_ms + 4000), heavy};
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
                                     "cvcbrawl: {} hit {} (bone {}, {:.0f} units/s, {:.1f} apart, roots {:.0f}), health A {:.0f} B {:.0f} at {} ms",
                                     k == 0 ? "A" : "B", k == 0 ? "B" : "A", hit_bone, hit_speed, hit_gap,
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
