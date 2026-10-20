// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>
#include <atomic>
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
    // A death motion to play on a knocked-out guest colossus (its host level's death sequence only
    // plays the level's own boss's): object, then its static MotionKind (e.g. Knight_died_early).
    std::atomic<u64> motion_object{0};
    u64 motion = 0;
};
static CvcRays g_cvc_rays;

extern "C" PS4_SYSV_ABI void CvcRayWork() {
    auto& r = g_cvc_rays;
    if (const u64 object = r.motion_object.exchange(0); object && r.image && r.motion) {
        // As the game's motion start 0x6179f0 does once it has resolved the motion (its callers
        // first mark the AI workspace's +0x1648): root motion 0x634600 / 0x634560, then 0x635740.
        const u64 component = object + 0x120;
        const u64 workspace = reinterpret_cast<u64 PS4_SYSV_ABI (*)(u64)>(r.image + 0x681ce0)(object);
        if (workspace) {
            *reinterpret_cast<volatile u8*>(workspace + 0x1648) = 1;
        }
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, u64, s32, float)>(r.image + 0x634600)(
            component + 0x10bc, object, r.motion, -1, 0.0f);
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, u64)>(r.image + 0x634560)(component, object, r.motion);
        reinterpret_cast<void PS4_SYSV_ABI (*)(u64, u64, float)>(r.image + 0x635740)(component, r.motion, 0.0f);
        LOG_INFO(Core, "cvc: death motion {:#x} on {:#x} (now {:#x})", r.motion - r.image, object,
                 *reinterpret_cast<const volatile u64*>(component + 0x260) - r.image);
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
static u32 MoveCharacter(u64 object, const float delta[3]) {
    for (const u64 at : {0x130, 0x140, 0x160, 0x750, 0x7b0, 0x7c0, 0x7f0, 0x1640}) {
        auto* p = reinterpret_cast<float*>(object + at);
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
        for (int k = 0; k < 3; k++) {
            p[k] += delta[k];
        }
    }
    return *reinterpret_cast<const u32*>(object + 0x1e0 + 0x30);
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
        if (text.starts_with('@')) {
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
        // is non-zero / zero (e.g. a cave slot that says the colossi are set up). May be chained.
        std::string action = queued;
        bool run = true;
        while (run && (action.starts_with("ifset=") || action.starts_with("ifzero="))) {
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
                LOG_INFO(Core, "cvctarget: A {:#x} (node {:#x}) <-> B {:#x} (node {:#x})",
                         *reinterpret_cast<u64*>(data + 0x08), na,
                         *reinterpret_cast<u64*>(data + 0x10), nb);
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
            if (picked) {
                sscanf(action.c_str() + 11, "%llx#%d:%f:%llx", &field, &pick, &seconds, &flag);
            } else {
                sscanf(action.c_str() + 11, "%llx:%f:%llx", &field, &seconds, &flag);
            }
            if (image) {
                DWORD old = 0;
                VirtualProtect(reinterpret_cast<void*>((image + flag) & ~u64(0xfff)), 0x1000,
                               PAGE_EXECUTE_READWRITE, &old);
            }
            std::thread([image, field, pick, seconds, flag] {
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
                        continue;
                    }
                    // It moves during its intro cinematic already, and a boss jump started then
                    // never finishes loading: also wait for 3 s without a cinematic (the
                    // letterbox amount, image 0x2dcdf04, see Screens::Letterboxed).
                    thread_local int calm = 0;
                    const float cinematic = *reinterpret_cast<const volatile float*>(image + 0x2dcdf04);
                    calm = cinematic == 0.0f ? calm + 1 : 0;
                    if (std::hypot(p[0] - first[0], p[2] - first[2]) > 2.0f && calm >= 15) {
                        *reinterpret_cast<volatile u8*>(image + flag) = 1;
                        LOG_INFO(Core, "awaitmoved: object {:#x} moved, flag {:#x} set", object, flag);
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
                            continue; // fine, or cleared on purpose (defeat)
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
                    if ((!a && !b) || !world) {
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
                                                : std::clamp(0.8f * sep + 30.0f, dist, 180.0f); // not out of the arena
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
                            // (from along it one colossus hides the other).
                            const float axis = std::atan2(pb[2] - pa[2], pb[0] - pa[0]);
                            // After a knockout: from the side away from the winner, which would
                            // otherwise stand between the camera and the body.
                            const bool after = winner == 1 || winner == 2;
                            const float* winner_at = winner == 1 ? pa : pb;
                            const float away = std::atan2(winner_at[2] - mid[2], winner_at[0] - mid[0]);
                            const auto score = [&](int k) {
                                const float at = tested_angle + offset(k);
                                return clear_part[k] - 0.2f * std::abs(offset(k)) -
                                       (after ? 0.8f * std::cos(at - away) : 0.5f * std::abs(std::cos(at - axis))) -
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
                                const float part = std::max(clear_part[0] - 12.0f / reach, 0.3f);
                                radius = reach * part, rise = 20.0f + lift * part;
                            } else if (clear_part[0] >= 0.6f && visible[0]) {
                                // In front of whatever blocks the far end of its own line.
                                const float part = clear_part[0] - 12.0f / reach; // 12 units off the hit
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
                        const float want_look[3]{mid[0], mid[1] + 12.0f, mid[2]};
                        const float ease = placed ? 0.03f : 1.0f;
                        for (int k = 0; k < 3; k++) {
                            look[k] += (want_look[k] - look[k]) * ease;
                        }
                        placed = true;
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
                        float dir[3]{look[0] - cam[0], look[1] - cam[1], look[2] - cam[2]};
                        const float length = std::max(std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] +
                                                                dir[2] * dir[2]), 0.001f);
                        for (float& c : dir) {
                            c /= length;
                        }
                        float* out = pose[which ^= 1];
                        const float values[15]{cam[0], cam[1], cam[2], 1.0f, look[0], look[1], look[2], 1.0f,
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
            // Test: colossus-vs-colossus hits from the two skeletons. "cvcbrawl=300:20:15:12:60:1200":
            // for 300 s, a bone of one colossus moving faster than 60 units/s that comes within 12
            // units of any bone of the other is a hit: A's hit takes 20 off B's manager health
            // (0x1593e08 -> +0x50), B's 15 off A's (0x1593e00), at most one per attacker per
            // 1200 ms. Bones: the world matrices of object+0x120 -> +0x198 (64 bytes, translation
            // at +0x30). Counts hits like cvchits= (0x1593ec8 by A, 0x1593ecc by B).
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const u64 image = module ? module->GetBaseAddress() : 0;
            float seconds = 0, dmg_a = 0, dmg_b = 0, radius = 0, speed = 0, cooldown = 0;
            sscanf(action.c_str() + 9, "%f:%f:%f:%f:%f:%f", &seconds, &dmg_a, &dmg_b, &radius, &speed,
                   &cooldown);
            std::thread([image, seconds, dmg_a, dmg_b, radius, speed, cooldown] {
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
                Clock::time_point ready[2]{Clock::now(), Clock::now()};
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
                    {
                        // Fight director: their AIs do not always commit (they can circle 80 units
                        // apart for minutes). Kept apart more than 45 units for 8 s, both are drawn
                        // toward each other, 5 units a second each, until within 30.
                        static Clock::time_point apart_since = Clock::now();
                        static bool drawing = false;
                        const auto* pa = reinterpret_cast<const volatile float*>(objects[0] + 0x130);
                        const auto* pb = reinterpret_cast<const volatile float*>(objects[1] + 0x130);
                        const float dx = pb[0] - pa[0], dz = pb[2] - pa[2];
                        const float gap = std::hypot(dx, dz);
                        if (gap < 1e4f) {
                            if (gap <= 45.0f) {
                                apart_since = Clock::now();
                            }
                            if (!drawing && Clock::now() - apart_since > std::chrono::seconds(8)) {
                                drawing = true;
                                LOG_INFO(Core, "cvcbrawl: drawing them together ({:.0f} apart)", gap);
                            }
                            if (drawing && gap < 30.0f) {
                                drawing = false;
                                apart_since = Clock::now();
                            }
                            if (drawing && gap > 1.0f) {
                                const float step = 5.0f * std::min(dt, 0.1f) / gap;
                                const float to_b[3]{dx * step, 0.0f, dz * step};
                                const float to_a[3]{-dx * step, 0.0f, -dz * step};
                                MoveCharacter(objects[0], to_b);
                                MoveCharacter(objects[1], to_a);
                            }
                            // Leash, for Basaran's valley (the only arena CvC runs in so far): the open
                            // geyser field around (950, -860); Gaius heads home under the rock arch,
                            // where the view is cramped. Their middle drifted over 70 units away: both
                            // nudged back, 3 units a second.
                            const float field[2]{950.0f, -860.0f};
                            const float mx = (pa[0] + pb[0]) / 2 - field[0], mz = (pa[2] + pb[2]) / 2 - field[1];
                            const float off = std::hypot(mx, mz);
                            if (off > 70.0f) {
                                const float step = 3.0f * std::min(dt, 0.1f) / off;
                                const float back[3]{-mx * step, 0.0f, -mz * step};
                                MoveCharacter(objects[0], back);
                                MoveCharacter(objects[1], back);
                            }
                        }
                    }
                    static int ticks = 0;
                    if (++ticks % 600 == 0) {
                        LOG_INFO(Core, "cvcbrawl: motions A {:#x} B {:#x}, health A {:.0f} B {:.0f}",
                                 q(objects[0] + 0x380) - image, q(objects[1] + 0x380) - image,
                                 *reinterpret_cast<const float*>(managers[0] + 0x50),
                                 *reinterpret_cast<const float*>(managers[1] + 0x50));
                    }
                    for (int k = 0; k < 2; k++) {
                        before[k].swap(now[k]);
                        bones(objects[k], now[k]);
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
                        if (!fell && (Clock::now() < ready[k] || now[k].size() != before[k].size() ||
                                      now[1 - k].empty() || dt <= 0.0f)) {
                            continue;
                        }
                        int hit_bone = -1;
                        float hit_speed = 0.0f, hit_gap = 0.0f;
                        for (size_t i = 0; i < now[k].size() && hit_bone < 0; i++) {
                            const float v = distance(now[k][i], before[k][i]) / dt;
                            if (v < speed || v > 2000.0f) { // over 2000: a teleport, not a blow
                                continue;
                            }
                            for (const auto& t : now[1 - k]) {
                                const float gap = distance(now[k][i], t);
                                if (gap < radius) {
                                    hit_bone = int(i), hit_speed = v, hit_gap = gap;
                                    break;
                                }
                            }
                        }
                        if (hit_bone < 0 && !fell) {
                            continue;
                        }
                        auto* health = reinterpret_cast<float*>(managers[1 - k] + 0x50);
                        if (hit_bone >= 0) {
                            ready[k] = Clock::now() + std::chrono::milliseconds(u64(cooldown));
                            const float damage = k == 0 ? dmg_a : dmg_b;
                            *health = std::max(0.0f, *health - damage);
                            // The blow's effect on the victim: shoved away from the attacker, 9 units
                            // over 300 ms. (Reporting weak-point damage here too made Basaran's script
                            // run its death early; that is kept for the knockout below.)
                            std::thread([victim = objects[1 - k], from = now[k][hit_bone]] {
                                const auto* pv = reinterpret_cast<const volatile float*>(victim + 0x130);
                                float push[3]{pv[0] - from[0], 0.0f, pv[2] - from[2]};
                                const float length = std::max(std::hypot(push[0], push[2]), 0.001f);
                                push[0] *= 0.6f / length, push[2] *= 0.6f / length;
                                for (int step = 0; step < 15; step++) {
                                    MoveCharacter(victim, push);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                }
                            }).detach();
                            (*reinterpret_cast<volatile u32*>(image + (k == 0 ? 0x1593ec8 : 0x1593ecc)))++;
                            LOG_INFO(Core,
                                     "cvcbrawl: {} hit {} (bone {}, {:.0f} units/s, {:.1f} apart), health A {:.0f} B {:.0f}",
                                     k == 0 ? "A" : "B", k == 0 ? "B" : "A", hit_bone, hit_speed, hit_gap,
                                     *reinterpret_cast<const float*>(managers[0] + 0x50),
                                     *reinterpret_cast<const float*>(managers[1] + 0x50));
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
                            // A guest colossus (not the level's own boss, 0x325e8a8) plays its own death
                            // motion with its AI no longer run (0x1593b48, contact_patch.py): the
                            // level's death sequence only collapses the level's own boss.
                            {
                                const u64 table = *reinterpret_cast<const u64*>(image + 0x16d21f8 + 0x20);
                                const u64 entry = *reinterpret_cast<const volatile u64*>(image + 0x1593b40);
                                const u64 level_boss = *reinterpret_cast<const volatile u64*>(image + 0x325e8a8);
                                // Static MotionKind records (component +0x260 during the native death).
                                static const std::unordered_map<u64, u64> deaths{{2, 0x1d39370}}; // Knight_died_early
                                const auto death = deaths.find(entry >= table ? (entry - table) / 0x1e8 : ~0ull);
                                if (entry != level_boss && death != deaths.end()) {
                                    *reinterpret_cast<volatile u64*>(image + 0x1593b48) = objects[1 - k];
                                    g_cvc_rays.image = image, g_cvc_rays.motion = image + death->second;
                                    g_cvc_rays.motion_object.store(objects[1 - k]);
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
                            // Colossi do not collide with each other: the winner (now after Wander)
                            // would walk through the body. Kept 30 units from its nearest bone.
                            std::thread([winner = objects[k], loser] {
                                const auto until = Clock::now() + std::chrono::seconds(40);
                                while (Clock::now() < until) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                                    u8 type = 0;
                                    SIZE_T got = 0;
                                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(loser + 0xc),
                                                           &type, 1, &got) || type != 9) {
                                        return;
                                    }
                                    // Its skeleton's middle (bone world matrices, translation at +0x30).
                                    u64 matrices = 0;
                                    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(loser + 0x120 + 0x198),
                                                      &matrices, 8, &got);
                                    const auto* w = reinterpret_cast<const volatile float*>(winner + 0x130);
                                    float c[2]{}, nearest = 1e9f;
                                    int count = 0;
                                    for (int i = 0; matrices && i < 256; i++) {
                                        float m[4]{};
                                        if (!ReadProcessMemory(GetCurrentProcess(),
                                                               reinterpret_cast<const void*>(matrices + i * 64 + 0x30), m,
                                                               sizeof(m), &got) ||
                                            m[3] != 1.0f) {
                                            break;
                                        }
                                        const float d = std::hypot(w[0] - m[0], w[2] - m[2]);
                                        if (d < nearest) {
                                            nearest = d, c[0] = m[0], c[1] = m[2];
                                        }
                                        count++;
                                    }
                                    if (count == 0) {
                                        continue;
                                    }
                                    const float dx = w[0] - c[0], dz = w[2] - c[1], gap = std::hypot(dx, dz);
                                    if (gap < 30.0f && gap > 0.001f) {
                                        const float push = std::min(30.0f - gap, 0.5f) / gap;
                                        const float delta[3]{dx * push, 0.0f, dz * push};
                                        MoveCharacter(winner, delta);
                                    }
                                }
                            }).detach();
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
                            LOG_INFO(Core, "cvcbrawl: knockout shown");
                            return;
                        }
                    }
                }
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
