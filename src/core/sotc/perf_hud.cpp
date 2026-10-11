// SPDX-License-Identifier: GPL-2.0-or-later

// Performance readout for benchmark footage, see perf_hud.h.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include "core/sotc/perf_hud.h"
#include "core/sotc/ui.h"

#ifdef _WIN32
#include <windows.h>
typedef _Return_type_success_(return >= 0) LONG NTSTATUS;
#include <d3dkmthk.h>
#include <psapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "gdi32.lib")
#endif

namespace Sotc::PerfHud {

namespace {

constexpr size_t History = 600; // frames kept for the graph and the low figure

std::mutex mutex;
std::array<float, History> gaps{};
size_t gap_count = 0; // frames written so far (the ring wraps)

// Sampled once a second.
struct Sample {
    float cpu_all = 0.0f;     // whole machine, percent of all cores
    float cpu_process = 0.0f; // this program, percent of one core
    float cpu_thread = 0.0f;  // its busiest thread, percent of one core
    float gpu = -1.0f;        // busiest engine of the graphics card, percent; -1 = not known
    u64 ram = 0;              // working set, bytes
};
Sample sample;

#ifdef _WIN32
u64 Ticks(const FILETIME& time) {
    return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

/// How busy the graphics card is: the share of the last interval its busiest engine was
/// running, from the graphics kernel's own statistics (what Task Manager shows). It counts
/// every program's work on the card, not this one's alone. -1 when it cannot be read.
/// (Windows' performance counters for the same thing return nothing on some systems.)
float GpuLoad(float elapsed_100ns) {
    static std::map<std::pair<u64, u32>, u64> last_running;
    D3DKMT_ENUMADAPTERS2 list{};
    if (D3DKMTEnumAdapters2(&list) != 0 || list.NumAdapters == 0) {
        return -1.0f;
    }
    std::vector<D3DKMT_ADAPTERINFO> adapters(list.NumAdapters);
    list.pAdapters = adapters.data();
    if (D3DKMTEnumAdapters2(&list) != 0) {
        return -1.0f;
    }
    float busiest = -1.0f;
    for (u32 i = 0; i < list.NumAdapters; i++) {
        const LUID luid = adapters[i].AdapterLuid;
        D3DKMT_QUERYSTATISTICS about{};
        about.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        about.AdapterLuid = luid;
        if (D3DKMTQueryStatistics(&about) == 0) {
            const u32 nodes = about.QueryResult.AdapterInformation.NodeCount;
            for (u32 node = 0; node < nodes && node < 64; node++) {
                D3DKMT_QUERYSTATISTICS query{};
                query.Type = D3DKMT_QUERYSTATISTICS_NODE;
                query.AdapterLuid = luid;
                query.QueryNode.NodeId = node;
                if (D3DKMTQueryStatistics(&query) != 0) {
                    continue;
                }
                const u64 running = static_cast<u64>(
                    query.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart);
                const auto key = std::make_pair(
                    (static_cast<u64>(luid.HighPart) << 32) | luid.LowPart, node);
                const auto before = last_running.find(key);
                if (before != last_running.end() && elapsed_100ns > 0.0f && running >= before->second) {
                    busiest = std::max(
                        busiest, 100.0f * float(running - before->second) / elapsed_100ns);
                }
                last_running[key] = running;
            }
        }
        D3DKMT_CLOSEADAPTER close{adapters[i].hAdapter};
        D3DKMTCloseAdapter(&close);
    }
    return std::min(busiest, 100.0f);
}

void TakeSample() {
    static u64 last_idle = 0, last_kernel = 0, last_user = 0, last_process = 0;
    static u64 last_wall = 0;
    static std::map<DWORD, u64> last_threads;
    FILETIME idle, kernel, user, created, exited;
    Sample next;
    GetSystemTimes(&idle, &kernel, &user);
    const u64 total = (Ticks(kernel) - last_kernel) + (Ticks(user) - last_user);
    if (last_kernel != 0 && total != 0) {
        next.cpu_all = 100.0f * (1.0f - float(Ticks(idle) - last_idle) / float(total));
    }
    last_idle = Ticks(idle);
    last_kernel = Ticks(kernel);
    last_user = Ticks(user);

    FILETIME now_time;
    GetSystemTimeAsFileTime(&now_time);
    const u64 wall = Ticks(now_time);
    const float elapsed = last_wall != 0 ? float(wall - last_wall) : 0.0f;
    last_wall = wall;

    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const u64 process = Ticks(kernel) + Ticks(user);
    if (elapsed > 0.0f) {
        next.cpu_process = 100.0f * float(process - last_process) / elapsed;
    }
    last_process = process;

    // The busiest thread: the renderer's command thread is what limits the frame rate here.
    const DWORD pid = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        std::map<DWORD, u64> threads;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more;
             more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid) {
                continue;
            }
            const HANDLE thread =
                OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                const u64 used = Ticks(kernel) + Ticks(user);
                threads[entry.th32ThreadID] = used;
                const auto before = last_threads.find(entry.th32ThreadID);
                if (before != last_threads.end() && elapsed > 0.0f) {
                    next.cpu_thread =
                        std::max(next.cpu_thread, 100.0f * float(used - before->second) / elapsed);
                }
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        last_threads = std::move(threads);
    }

    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        next.ram = memory.WorkingSetSize;
    }

    // Graphics load: Windows' own counter for this program's use of the 3D engines.
    next.gpu = GpuLoad(elapsed);
    std::scoped_lock lock{mutex};
    sample = next;
}
#else
void TakeSample() {}
#endif

/// Listing every thread of the machine takes long enough to show in the frame times, so the
/// sampling has a thread of its own.
void SamplerLoop() {
    for (;;) {
        TakeSample();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

/// A line of text from the environment; underscores stand for spaces, so it can be passed
/// through scripts that split on spaces.
std::string EnvText(const char* name) {
    const char* value = std::getenv(name);
    std::string text = value ? value : "";
    std::ranges::replace(text, '_', ' ');
    return text;
}

} // namespace

bool Enabled() {
    static const bool enabled = std::getenv("SOTC_PERFHUD") != nullptr;
    return enabled;
}

void OnFrame(double gap_ms) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{mutex};
    gaps[gap_count % History] = static_cast<float>(gap_ms);
    ++gap_count;
}

void Draw() {
    if (!Enabled()) {
        return;
    }
    static const bool started = [] {
        std::thread{SamplerLoop}.detach();
        return true;
    }();
    // The frames in time order, newest last.
    std::vector<float> frames;
    Sample sample;
    {
        std::scoped_lock lock{mutex};
        sample = PerfHud::sample;
        const size_t count = std::min(gap_count, History);
        frames.reserve(count);
        for (size_t i = 0; i < count; i++) {
            frames.push_back(gaps[(gap_count - count + i) % History]);
        }
    }
    float now_fps = 0.0f, average = 0.0f, low = 0.0f;
    if (!frames.empty()) {
        // "Now" is the last half second, so the figure can be read.
        float time = 0.0f;
        size_t recent = 0;
        for (auto it = frames.rbegin(); it != frames.rend() && time < 500.0f; ++it) {
            time += *it;
            ++recent;
        }
        now_fps = time > 0.0f ? 1000.0f * recent / time : 0.0f;
        float total = 0.0f;
        for (const float gap : frames) {
            total += gap;
        }
        average = total > 0.0f ? 1000.0f * frames.size() / total : 0.0f;
        // 1% low: the frame rate over the slowest hundredth of the frames.
        std::vector<float> sorted = frames;
        std::ranges::sort(sorted, std::greater{});
        const size_t worst = std::max<size_t>(sorted.size() / 100, 1);
        float worst_time = 0.0f;
        for (size_t i = 0; i < worst; i++) {
            worst_time += sorted[i];
        }
        low = worst_time > 0.0f ? 1000.0f * worst / worst_time : 0.0f;
    }

    const float scale = Ui::Scale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetForegroundDrawList();
    const float width = 430.0f * scale;
    const float left = display.x - width - 28.0f * scale;
    const float top = 28.0f * scale;
    const float pad = 14.0f * scale;
    const std::string label = EnvText("SOTC_PERFHUD_LABEL");
    const std::string note = EnvText("SOTC_PERFHUD_NOTE");
    const float line = 26.0f * scale;
    const float graph_height = 70.0f * scale;
    const float height = pad * 2 + (label.empty() ? 0.0f : line) + 46.0f * scale + line * 4 +
                         graph_height + 10.0f * scale + (note.empty() ? 0.0f : line);
    draw->AddRectFilled({left, top}, {left + width, top + height}, IM_COL32(8, 8, 8, 176),
                        6.0f * scale);
    constexpr ImU32 Bright = IM_COL32(240, 238, 228, 255);
    constexpr ImU32 Plain = IM_COL32(190, 188, 178, 255);
    constexpr ImU32 Green = IM_COL32(140, 214, 132, 255);
    ImFont* font = Ui::Serif();
    const auto text = [&](float x, float y, float size, ImU32 color, const std::string& value) {
        draw->AddText(font, size * scale, {x, y}, color, value.c_str());
    };
    float y = top + pad;
    const float x = left + pad;
    if (!label.empty()) {
        text(x, y, 20.0f, Plain, label);
        y += line;
    }
    text(x, y, 40.0f, Green, fmt::format("{:.0f} FPS", now_fps));
    text(x + 190.0f * scale, y + 2.0f * scale, 19.0f, Bright,
         fmt::format("average {:.0f}", average));
    text(x + 190.0f * scale, y + 22.0f * scale, 19.0f, Bright, fmt::format("1% low {:.0f}", low));
    y += 46.0f * scale;
    const float last_gap = frames.empty() ? 0.0f : frames.back();
    text(x, y, 19.0f, Plain,
         fmt::format("frame {:.1f} ms     {} x {}", last_gap, int(display.x), int(display.y)));
    y += line;
    text(x, y, 19.0f, Plain,
         fmt::format("CPU {:.0f}%  game {:.0f}%  busiest thread {:.0f}%", sample.cpu_all,
                     sample.cpu_process, sample.cpu_thread));
    y += line;
    text(x, y, 19.0f, Plain,
         // "Allocated": what the renderer has taken from the card, which is more than a frame
         // needs (it keeps textures and buffers it may want again).
         // The kernel reports no running time at all on some systems (a steady 0): then the
         // line goes without the figure rather than show a wrong one.
         sample.gpu > 0.5f ? fmt::format("GPU {:.0f}%     video memory allocated {:.1f} GB", sample.gpu,
                                          device_memory.load() / 1073741824.0)
                            : fmt::format("video memory allocated {:.1f} GB",
                                          device_memory.load() / 1073741824.0));
    y += line;
    text(x, y, 19.0f, Plain, fmt::format("RAM {:.1f} GB", sample.ram / 1073741824.0));
    y += line;
    // Frame times, newest at the right; the top of the graph is 50 ms, lines at 16.7 and 33.3.
    const float graph_left = x;
    const float graph_right = left + width - pad;
    const float graph_bottom = y + graph_height;
    draw->AddRectFilled({graph_left, y}, {graph_right, graph_bottom}, IM_COL32(0, 0, 0, 120));
    for (const float mark : {16.7f, 33.3f}) {
        const float mark_y = graph_bottom - graph_height * mark / 50.0f;
        draw->AddLine({graph_left, mark_y}, {graph_right, mark_y}, IM_COL32(255, 255, 255, 46));
    }
    if (frames.size() > 1) {
        const size_t shown = std::min<size_t>(frames.size(), 360);
        const float step = (graph_right - graph_left) / float(shown - 1);
        ImVec2 before{};
        for (size_t i = 0; i < shown; i++) {
            const float gap = std::min(frames[frames.size() - shown + i], 50.0f);
            const ImVec2 point{graph_left + step * float(i), graph_bottom - graph_height * gap / 50.0f};
            if (i != 0) {
                draw->AddLine(before, point, Green, 1.5f * scale);
            }
            before = point;
        }
    }
    y = graph_bottom + 10.0f * scale;
    if (!note.empty()) {
        text(x, y, 17.0f, Plain, note);
    }
}

} // namespace Sotc::PerfHud
