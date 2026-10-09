// SPDX-License-Identifier: GPL-2.0-or-later

// Startup watchdog: when no frame has been shown some time after launch, writes the call stack of
// every thread to hang_stacks.txt in the log folder, so a stalled start can be diagnosed from the
// user's machine without a debugger.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <thread>

#include "common/path_util.h"
#include "common/types.h"
#include "core/sotc/watchdog.h"

#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

extern std::atomic<u64> g_first_flip_ms;

namespace Sotc::Watchdog {

#ifdef _WIN32
namespace {

void DumpAllThreads(const std::filesystem::path& path, int waited_seconds) {
    FILE* out = _wfopen(path.c_str(), L"w");
    if (!out) {
        return;
    }
    std::fprintf(out, "No frame %d s after launch. Call stacks of all threads:\n", waited_seconds);
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (snapshot == INVALID_HANDLE_VALUE || !Thread32First(snapshot, &entry)) {
        std::fclose(out);
        return;
    }
    do {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                       FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        char name[128] = "";
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr);
            LocalFree(description);
        }
        // Frames are collected while the thread is stopped and printed after it runs again, so
        // nothing here waits on a lock the stopped thread may hold.
        DWORD64 frames[48]{};
        int count = 0;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                while (count < 48 &&
                       StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                                   nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                                   nullptr) &&
                       frame.AddrPC.Offset != 0) {
                    frames[count++] = frame.AddrPC.Offset;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);

        std::fprintf(out, "\nThread %lu [%s]\n", entry.th32ThreadID, name);
        for (int i = 0; i < count; i++) {
            char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            const bool named = SymFromAddr(process, frames[i], &displacement, symbol);
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            const bool has_module = SymGetModuleInfo64(process, frames[i], &module);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement = 0;
            const bool has_line = SymGetLineFromAddr64(process, frames[i], &line_displacement, &line);
            std::fprintf(out, "  %s+0x%llx  %s", has_module ? module.ModuleName : "?",
                         has_module ? frames[i] - module.BaseOfImage : frames[i],
                         named ? symbol->Name : "");
            if (has_line) {
                std::fprintf(out, "  (%s:%lu)", line.FileName, line.LineNumber);
            }
            std::fprintf(out, "\n");
        }
        std::fflush(out);
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    std::fclose(out);
}

} // namespace
#endif

void WriteCrashStack(void* context_record, unsigned long long fault_address) {
#ifdef _WIN32
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "crash_stack.txt";
    FILE* out = _wfopen(path.c_str(), L"w");
    if (!out) {
        return;
    }
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *static_cast<CONTEXT*>(context_record);
    std::fprintf(out, "Crash at %llx, access to %llx. Call stack of the faulting thread:\n",
                 static_cast<unsigned long long>(context.Rip), fault_address);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; i++) {
        if (i > 0 &&
            (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
             frame.AddrPC.Offset == 0)) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        char buffer[sizeof(SYMBOL_INFO) + 512]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, address, &displacement, symbol);
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const bool has_module = SymGetModuleInfo64(process, address, &module);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        const bool has_line = SymGetLineFromAddr64(process, address, &line_displacement, &line);
        std::fprintf(out, "  %s+0x%llx  %s", has_module ? module.ModuleName : "?",
                     has_module ? address - module.BaseOfImage : address,
                     named ? symbol->Name : "");
        if (has_line) {
            std::fprintf(out, "  (%s:%lu)", line.FileName, line.LineNumber);
        }
        std::fprintf(out, "\n");
    }
    std::fclose(out);
#endif
}

#ifdef _WIN32
namespace {

/// Profiling aid (SOTC_THREADSTAT=<file>): every 5 s, the CPU time each thread used in that
/// interval, busiest first, by thread name.
void ThreadStatLoop(std::string path) {
    struct Seen {
        ULONGLONG cpu = 0;
    };
    std::map<DWORD, Seen> seen;
    FILE* out = std::fopen(path.c_str(), "w");
    if (!out) {
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        std::vector<std::pair<double, std::string>> rows;
        double total = 0.0;
        if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != pid) {
                    continue;
                }
                const HANDLE thread =
                    OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread) {
                    continue;
                }
                FILETIME created, exited, kernel, user;
                if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                    const ULONGLONG cpu =
                        (ULONGLONG(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                        (ULONGLONG(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                    Seen& before = seen[entry.th32ThreadID];
                    const double percent = (cpu - before.cpu) / 1e7 / 5.0 * 100.0;
                    before.cpu = cpu;
                    char name[128] = "";
                    PWSTR description = nullptr;
                    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
                        WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name),
                                            nullptr, nullptr);
                        LocalFree(description);
                    }
                    total += percent;
                    if (percent >= 2.0) {
                        rows.emplace_back(percent, name[0] ? name : "(unnamed)");
                    }
                }
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }
        if (snapshot != INVALID_HANDLE_VALUE) {
            CloseHandle(snapshot);
        }
        std::sort(rows.begin(), rows.end(), std::greater<>());
        std::fprintf(out, "total %.0f%%:", total);
        for (const auto& [percent, name] : rows) {
            std::fprintf(out, "  %s %.0f%%", name.c_str(), percent);
        }
        std::fprintf(out, "\n");
        std::fflush(out);
    }
}

/// Sampling profiler (SOTC_PROFILE=<file>, optional SOTC_PROFILE_THREAD=<name>,
/// SOTC_PROFILE_AFTER=<seconds>): interrupts one thread every 2 ms for 20 s, records its call
/// stack, and writes where the time went, by function, exclusive and inclusive.
void ProfileLoop(std::string path) {
    const char* wanted_env = std::getenv("SOTC_PROFILE_THREAD");
    const std::string wanted = wanted_env ? wanted_env : "shadPS4:GpuCommandProcessor";
    const char* after_env = std::getenv("SOTC_PROFILE_AFTER");
    std::this_thread::sleep_for(std::chrono::seconds(after_env ? std::atoi(after_env) : 75));

    HANDLE target = nullptr;
    const DWORD pid = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) {
                continue;
            }
            const HANDLE thread =
                OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            char name[128] = "";
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
                WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr,
                                    nullptr);
                LocalFree(description);
            }
            if (wanted == name && !target) {
                target = thread;
            } else {
                CloseHandle(thread);
            }
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) {
        CloseHandle(snapshot);
    }
    if (!target) {
        return;
    }

    constexpr int MaxDepth = 32;
    constexpr int Samples = 10000;
    // Filled without allocating while the thread is stopped: it may hold the heap lock.
    std::vector<std::array<DWORD64, MaxDepth>> stacks(Samples);
    std::vector<int> depths(Samples, 0);
    for (int n = 0; n < Samples; n++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (SuspendThread(target) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(target, &context)) {
            int depth = 0;
            while (depth < MaxDepth && context.Rip != 0) {
                stacks[n][depth++] = context.Rip;
                DWORD64 image_base = 0;
                const PRUNTIME_FUNCTION function =
                    RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
                if (!function) {
                    // A leaf: the return address is on top of the stack. Take one step, then
                    // stop if that leads nowhere known.
                    if (depth != 1 || IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), 8)) {
                        break;
                    }
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                                 &handler_data, &establisher, nullptr);
            }
            depths[n] = depth;
        }
        ResumeThread(target);
    }
    CloseHandle(target);

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::map<DWORD64, std::string> names;
    const auto name_of = [&](DWORD64 address) -> const std::string& {
        char buffer[sizeof(SYMBOL_INFO) + 512]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string name;
        if (SymFromAddr(process, address, &displacement, symbol)) {
            name = symbol->Name;
            // Symbols of the driver are export names far from the real code: keep the module.
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            if (SymGetModuleInfo64(process, address, &module) &&
                std::string_view{module.ModuleName} != "shadps4" &&
                std::string_view{module.ModuleName} != "ShadowOfTheColossus") {
                name = std::string("[") + module.ModuleName + "]";
            }
        } else {
            name = "[unknown]";
        }
        if (name.size() > 110) {
            name.resize(110);
        }
        return names.emplace(address, std::move(name)).first->second;
    };
    std::map<std::string, int> exclusive, inclusive, charged;
    int taken = 0;
    for (int n = 0; n < Samples; n++) {
        if (depths[n] == 0) {
            continue;
        }
        taken++;
        std::set<std::string> once;
        bool charged_done = false;
        for (int d = 0; d < depths[n]; d++) {
            const auto found = names.find(stacks[n][d]);
            const std::string& name = found != names.end() ? found->second : name_of(stacks[n][d]);
            if (d == 0) {
                exclusive[name]++;
            }
            // Time spent in the driver or the system, charged to the function of ours that
            // called into it, together with the module it ended up in.
            if (!charged_done && name[0] != '[') {
                charged_done = true;
                const auto top = names.find(stacks[n][0]);
                const std::string where = d == 0 ? "" : " -> " + top->second;
                charged[name + where]++;
            }
            if (once.insert(name).second) {
                inclusive[name]++;
            }
        }
    }
    FILE* out = std::fopen(path.c_str(), "w");
    if (!out) {
        return;
    }
    const auto dump = [&](const char* title, const std::map<std::string, int>& counts, int rows) {
        std::vector<std::pair<int, std::string>> sorted;
        for (const auto& [name, count] : counts) {
            sorted.emplace_back(count, name);
        }
        std::sort(sorted.begin(), sorted.end(), std::greater<>());
        std::fprintf(out, "\n%s\n", title);
        for (int i = 0; i < rows && i < static_cast<int>(sorted.size()); i++) {
            std::fprintf(out, "%5.1f%%  %s\n", 100.0 * sorted[i].first / std::max(taken, 1),
                         sorted[i].second.c_str());
        }
    };
    std::fprintf(out, "Thread %s, %d samples\n", wanted.c_str(), taken);
    dump("Exclusive (time in the function itself)", exclusive, 45);
    dump("Inclusive (time in the function and what it calls)", inclusive, 70);
    dump("By our function, with the outside module the time was spent in", charged, 60);
    std::fclose(out);
}

} // namespace
#endif

void Start() {
#ifdef _WIN32
    if (const char* profile_path = std::getenv("SOTC_PROFILE")) {
        std::thread{ProfileLoop, std::string(profile_path)}.detach();
    }
    if (const char* stat_path = std::getenv("SOTC_THREADSTAT")) {
        std::thread{ThreadStatLoop, std::string(stat_path)}.detach();
    }
    int seconds = 120;
    if (const char* env = std::getenv("SOTC_WATCHDOG_S")) {
        seconds = std::atoi(env);
    }
    if (seconds <= 0) {
        return;
    }
    std::thread{[seconds] {
        for (int i = 0; i < seconds; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_first_flip_ms != 0) {
                return;
            }
        }
        DumpAllThreads(Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "hang_stacks.txt",
                       seconds);
    }}.detach();
#endif
}

} // namespace Sotc::Watchdog
