// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <imgui.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/sotc/first_run.h"
#include "core/sotc/lang.h"
#include "core/sotc/ui.h"

#ifdef _WIN32
#include <windows.h>

#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace Sotc::FirstRun {

namespace {

std::filesystem::path UserDir() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir);
}

#ifdef _WIN32
struct Machine {
    std::wstring gpu_name;
    u64 gpu_memory_mb = 0; // dedicated memory of the strongest adapter
    u64 ram_mb = 0;
    u32 cores = 0;
    int display_width = 0;
    int display_height = 0;
};

Machine Detect() {
    Machine machine;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            const u64 memory_mb = desc.DedicatedVideoMemory / (1024 * 1024);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && memory_mb >= machine.gpu_memory_mb) {
                machine.gpu_memory_mb = memory_mb;
                machine.gpu_name = desc.Description;
            }
            adapter->Release();
        }
        factory->Release();
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        machine.ram_mb = memory.ullTotalPhys / (1024 * 1024);
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    machine.cores = info.dwNumberOfProcessors;
    // Real pixels of the main display, whatever the desktop scaling.
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        machine.display_width = static_cast<int>(mode.dmPelsWidth);
        machine.display_height = static_cast<int>(mode.dmPelsHeight);
    }
    return machine;
}
#endif

} // namespace

bool ConfigureForThisPc(bool new_player) {
#ifdef _WIN32
    std::error_code ec;
    const auto marker = UserDir() / "sotc_first_run_settings.txt";
    if (std::filesystem::exists(marker, ec)) {
        return false;
    }
    // Only a new player's first start gets settings chosen for them. Anyone who has played
    // before keeps every setting they have.
    if (!new_player) {
        std::ofstream(marker) << "This install had been played before the first-run check; "
                                 "nothing was changed.\n";
        return false;
    }
    const auto script = std::filesystem::current_path(ec) / "Settings.ps1";
    if (!std::filesystem::exists(script, ec)) {
        return false; // try again on a later start
    }
    const Machine machine = Detect();
    // Detail by graphics memory, which tracks GPU class well enough for a first guess. The game
    // itself needs about 3 GB of it at 1080p.
    const char* preset = machine.gpu_memory_mb >= 11000  ? "ultra"
                         : machine.gpu_memory_mb >= 7000 ? "high"
                         : machine.gpu_memory_mb >= 5000 ? "medium"
                                                         : "low";
    if (machine.ram_mb < 15000 || machine.cores < 8) {
        // Little memory or few CPU threads: stay at or below medium.
        if (std::string_view{preset} == "ultra" || std::string_view{preset} == "high") {
            preset = "medium";
        }
    }
    // The game draws 1080p; larger displays get the window at their size with FSR upscaling.
    const char* window = machine.display_height >= 2160   ? "4k"
                         : machine.display_height >= 1440 ? "1440p"
                         : machine.display_height >= 1080 ? "1080p"
                                                          : "720p";
    const bool upscale = machine.display_height > 1080;
    std::wstring command = L"pwsh.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
                           script.wstring() + L"\" -Preset " +
                           std::wstring(preset, preset + std::strlen(preset)) + L" -Window " +
                           std::wstring(window, window + std::strlen(window)) +
                           L" -Fullscreen on -Fps uncapped -VSync on -Upscaling " + (upscale ? L"on" : L"off");
    if (const char* game = std::getenv("SOTC_GAME_DIR")) {
        command += L" -GameDir \"" + std::filesystem::path(game).wstring() + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        return false;
    }
    std::ofstream out(marker);
    out << "Chosen on the first start for this PC. Change them any time in Settings.\n"
        << "GPU memory " << machine.gpu_memory_mb << " MB, RAM " << machine.ram_mb << " MB, "
        << machine.cores << " CPU threads, display " << machine.display_width << "x"
        << machine.display_height << "\n"
        << "Preset " << preset << ", window " << window
        << ", fullscreen on, frame rate uncapped with VSync on, upscaling "
        << (upscale ? "on" : "off") << "\n";
    LOG_INFO(Config, "First-run settings: preset {}, window {}", preset, window);
    return true;
#else
    return false;
#endif
}

namespace {
enum class State { Unknown, Showing, Done };
std::atomic<State> state{State::Unknown};
} // namespace

bool OpeningActive() {
    return state == State::Showing;
}

void DrawOpeningMessages() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point start;
    if (state == State::Done) {
        return;
    }
    if (state == State::Unknown) {
        std::error_code ec;
        const auto marker = UserDir() / "sotc_opening_shown.txt";
        // Test runs ask for it with SOTC_OPENING=1 and are otherwise left alone.
        const char* forced = std::getenv("SOTC_OPENING");
        const bool wanted = forced ? std::atoi(forced) != 0
                                   : !std::filesystem::exists(marker, ec) &&
                                         std::getenv("SOTC_HEADLESS") == nullptr;
        if (!wanted) {
            state = State::Done;
            return;
        }
        // Marked before showing: a start that fails must not replay it for ever.
        std::ofstream(marker) << "The opening messages were shown.\n";
        state = State::Showing;
        start = Clock::now();
        // The game waits behind the black screen and begins its own intros afterwards.
        DebugState.PauseGuestThreads();
    }

    static constexpr std::array<const char*, 2> Lines{
        "We waited, yet no one answered.",
        "Sometimes, we have to do things ourselves.",
    };
    constexpr float FadeIn = 1.5f, Hold = 5.0f, FadeOut = 1.5f, Gap = 0.8f;
    constexpr float Each = FadeIn + Hold + FadeOut + Gap;
    const float t = std::chrono::duration<float>(Clock::now() - start).count() - 0.8f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({0.0f, 0.0f}, display, IM_COL32(0, 0, 0, 255));
    if (t >= Each * Lines.size()) {
        state = State::Done;
        DebugState.ResumeGuestThreads();
        return;
    }
    if (t < 0.0f) {
        return;
    }
    const size_t index = static_cast<size_t>(t / Each);
    const float local = t - index * Each;
    float alpha = 0.0f;
    if (local < FadeIn) {
        alpha = local / FadeIn;
    } else if (local < FadeIn + Hold) {
        alpha = 1.0f;
    } else if (local < FadeIn + Hold + FadeOut) {
        alpha = 1.0f - (local - FadeIn - Hold) / FadeOut;
    }
    // Eased, so the text breathes in and out instead of ramping.
    alpha = alpha * alpha * (3.0f - 2.0f * alpha);
    const float size = 64.0f * Ui::Scale();
    const std::string text = Lang::Tr(Lines[index]);
    const ImVec2 extent = Ui::TextSize(size, text);
    draw->AddText(Ui::Serif(), size,
                  {(display.x - extent.x) * 0.5f, (display.y - extent.y) * 0.5f},
                  IM_COL32(236, 233, 222, static_cast<int>(255.0f * alpha)), text.c_str());
}

} // namespace Sotc::FirstRun
