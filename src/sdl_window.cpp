// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/singleton.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/sotc/achievements.h"
#include "core/sotc/challenge.h"
#include "core/sotc/menu.h"
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

static Uint32 SDLCALL ScriptTick(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    using Libraries::Pad::OrbisPadButtonDataOffset;
    struct Step {
        double at;
        std::string action;
    };
    static const std::vector<Step> steps = [] {
        std::vector<Step> out;
        std::stringstream ss{std::getenv("SOTC_SCRIPT")};
        for (std::string item; std::getline(ss, item, ',');) {
            const auto colon = item.find(':');
            out.push_back({std::stod(item.substr(0, colon)), item.substr(colon + 1)});
        }
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
    if (next < steps.size() && (now - start) / 1000.0 >= steps[next].at) {
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
    for (const std::string& action : due) {
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
        } else if (action.starts_with("poke=") || action.starts_with("peek=")) {
            // Test: write or log a 32-bit word of the game's image ("poke=325f430:3" sets game
            // variable 8; offsets as the eboot image has them).
            const auto colon = action.find(':');
            const u64 offset = std::stoull(action.substr(5, colon - 5), nullptr, 16);
            const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
            const VAddr address = module ? module->GetBaseAddress() + offset : 0;
            if (address && Core::Memory::Instance()->IsValidMapping(address, 4)) {
                auto* word = reinterpret_cast<u32*>(address);
                if (action.starts_with("poke=") && colon != std::string::npos) {
                    *word = static_cast<u32>(std::stoll(action.substr(colon + 1), nullptr, 0));
                }
                LOG_INFO(Core, "script {}: {:#x} = {:#x}", action, offset, *word);
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
