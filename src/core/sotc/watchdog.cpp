// SPDX-License-Identifier: GPL-2.0-or-later

// Startup watchdog: when no frame has been shown some time after launch, writes the call stack of
// every thread to hang_stacks.txt in the log folder, so a stalled start can be diagnosed from the
// user's machine without a debugger.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

void Start() {
#ifdef _WIN32
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
