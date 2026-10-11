// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>
#include <SDL3/SDL_timer.h>
#include <fmt/format.h>
#if defined(_WIN32)
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#endif
#include "core/memory.h"
#include "core/sotc/mem_trace.h"
#include "core/sotc/menu.h"
#include "core/sotc/watchdog.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

namespace Core {

#if defined(_WIN32)

static LONG SignalHandlerBody(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    const auto* signals = Signals::Instance();

    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const bool report_unhandled =
        use_static_windows_guest_red_zone_protection ? static_protection_exception : true;
    if (report_unhandled) {
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        // Host stack of the faulting thread, to symbolize crashes without a debugger.
        void* frames[40];
        const USHORT count = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
        std::string trace;
        for (USHORT i = 0; i < count; ++i) {
            trace += fmt::format("{} ", frames[i]);
        }
        LOG_CRITICAL(Debug, "access {:#x} rsp {:#x} stack: {}",
                     pExp->ExceptionRecord->ExceptionInformation[1], pExp->ContextRecord->Rsp,
                     trace);
        // What the registers held and what lies around the addresses in them: when the game's
        // allocator trips over a damaged block, the bytes that damaged it are right there and
        // often say who wrote them. Read through the system, so a bad address cannot fault.
        {
            const CONTEXT& c = *pExp->ContextRecord;
            LOG_CRITICAL(Debug,
                         "rax {:#x} rbx {:#x} rcx {:#x} rdx {:#x} rsi {:#x} rdi {:#x} r8 {:#x} "
                         "r9 {:#x} r10 {:#x} r11 {:#x} r12 {:#x} r13 {:#x} r14 {:#x} r15 {:#x}",
                         c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.R10, c.R11, c.R12,
                         c.R13, c.R14, c.R15);
            const auto dump = [](const char* name, u64 address) {
                u8 bytes[192];
                SIZE_T got = 0;
                const u64 from = (address & ~0xFULL) - 96;
                if (address < 0x10000 ||
                    !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(from), bytes,
                                       sizeof(bytes), &got) ||
                    got != sizeof(bytes)) {
                    return;
                }
                for (u32 row = 0; row < sizeof(bytes); row += 32) {
                    std::string hex, text;
                    for (u32 i = 0; i < 32; i++) {
                        hex += fmt::format("{:02x}{}", bytes[row + i], i % 8 == 7 ? " " : "");
                        text += bytes[row + i] >= 32 && bytes[row + i] < 127
                                    ? static_cast<char>(bytes[row + i])
                                    : '.';
                    }
                    LOG_CRITICAL(Debug, "{} {:#x}: {} {}", name, from + row, hex, text);
                }
            };
            // Changes to the address space that touched a megabyte around any of those
            // addresses, and the last few of all.
            {
                const u64 watched[] = {c.Rax, c.Rbx, c.Rsi, c.Rdi, c.R8, c.R10, c.R12, c.R15};
                const u64 newest = Sotc::MemTrace::written.load();
                u64 slot = newest > Sotc::MemTrace::Capacity ? newest - Sotc::MemTrace::Capacity : 0;
                Sotc::MemTrace::ForEach([&](const Sotc::MemTrace::Entry& entry) {
                    bool near_one = false;
                    for (const u64 address : watched) {
                        near_one |= address + 0x100000 > entry.address &&
                                    address < entry.address + entry.size + 0x100000;
                    }
                    if (near_one || slot + 24 >= newest) {
                        LOG_CRITICAL(Debug, "memory {} {:#x} size {:#x} at {} ms{}", entry.kind,
                                     entry.address, entry.size, entry.ms, near_one ? " NEAR" : "");
                    }
                    ++slot;
                });
                LOG_CRITICAL(Debug, "now {} ms", SDL_GetTicks());
                // Where those addresses live, and every recent zeroing of backing memory (Z,
                // by backing address) and page reset (X), to set against that.
                auto* memory = Core::Memory::Instance();
                const char* names[] = {"rax", "rbx", "rsi", "rdi", "r8", "r10", "r12", "r15"};
                for (size_t i = 0; i < std::size(watched); i++) {
                    if (watched[i] >= 0x10000) {
                        LOG_CRITICAL(Debug, "{} {:#x}: {}", names[i], watched[i],
                                     memory->DescribeAddress(watched[i]));
                    }
                }
                // The game's allocator keeps free blocks on a ring through a head that sits
                // 128 bytes into its state; the fault is usually a block on that ring whose link
                // has been wiped. Walk the ring from the first block (r10 in the search routine)
                // and say where the last good block and the bad link live.
                for (const u64 head : {c.R9, c.Rdi}) {
                    u64 first = 0;
                    SIZE_T got = 0;
                    if (head < 0x10000 ||
                        !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(head),
                                           &first, sizeof(first), &got) ||
                        got != sizeof(first)) {
                        continue;
                    }
                    u64 node = first, previous = head;
                    int steps = 0;
                    for (; steps < 50000 && node >= 0x10000 && node != head; steps++) {
                        u64 links[2]{};
                        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(node),
                                               links, sizeof(links), &got) ||
                            got != sizeof(links)) {
                            LOG_CRITICAL(Debug, "ring from {:#x}: block {:#x} (after {:#x}, {} steps) "
                                                "cannot be read: {}",
                                         head, node, previous, steps, memory->DescribeAddress(node));
                            break;
                        }
                        if (links[1] < 0x10000) {
                            LOG_CRITICAL(Debug, "ring from {:#x}: block {:#x} after {} steps has link "
                                                "{:#x}: {}",
                                         head, node, steps, links[1], memory->DescribeAddress(node));
                            LOG_CRITICAL(Debug, "ring: the block before it, {:#x}: {}", previous,
                                         memory->DescribeAddress(previous));
                            dump("bad", node);
                            dump("bad+", node + 192);
                            dump("bad-", node - 192);
                            break;
                        }
                        previous = node;
                        node = links[1];
                    }
                    LOG_CRITICAL(Debug, "ring from {:#x}: walked {} blocks, ended at {:#x}", head,
                                 steps, node);
                }
                Sotc::MemTrace::ForEach([&](const Sotc::MemTrace::Entry& entry) {
                    if ((entry.kind == 'Z' || entry.kind == 'X') &&
                        SDL_GetTicks() - entry.ms < 3000) {
                        LOG_CRITICAL(Debug, "recent {} {:#x} size {:#x} at {} ms", entry.kind,
                                     entry.address, entry.size, entry.ms);
                    }
                });
            }
            dump("rax", c.Rax);
            dump("rcx", c.Rcx);
            dump("rbx", c.Rbx);
            dump("r8", c.R8);
            dump("r9", c.R9);
            dump("r10", c.R10);
            dump("r10+", c.R10 + 192);
            dump("r10-", c.R10 - 192);
            dump("r11", c.R11);
            dump("rdi", c.Rdi);
            dump("rdi+", c.Rdi + 192);
            dump("rsi", c.Rsi);
        }
        Sotc::Watchdog::WriteCrashStack(pExp->ContextRecord,
                                        pExp->ExceptionRecord->ExceptionInformation[1]);
        // The game's own code faults now and then while it loads (seen inside its memory
        // allocator); rather than just close, the game is started again.
        if (Sotc::Menu::RecoverFromCrash()) {
            LOG_CRITICAL(Debug, "Restarting the game after the fault");
        }
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// The handler's own work is done on a stack of the runtime's, not on the stack of the thread
// that faulted. The game runs much of its code on fibers with stacks of 16 to 64 KiB that it
// takes from its heap, and writes to memory the GPU side watches fault all the time: handling
// one goes many calls deep (page tracking, buffer and texture caches). On a small fiber stack
// that ran off its end into whatever the heap held below it, which showed up later as faults
// in the game's allocator and job code. What Windows itself puts on the faulting stack before
// the handler is called (the exception record and context, a few KiB) cannot be moved.
constexpr size_t HandlerStackSize = 1024 * 1024;

using StackCall = LONG (*)(LONG (*)(EXCEPTION_POINTERS*) noexcept, EXCEPTION_POINTERS*, void*);

static StackCall MakeStackCall() {
    using namespace Xbyak::util;
    static Xbyak::CodeGenerator code{4096};
    const auto entry = code.getCurr<StackCall>();
    // rcx = function, rdx = its argument, r8 = top of the stack to run it on.
    code.push(rbp);
    code.mov(rbp, rsp);
    code.mov(rsp, r8);
    code.and_(rsp, ~0xFULL);
    code.sub(rsp, 32);
    code.mov(rax, rcx);
    code.mov(rcx, rdx);
    code.call(rax);
    code.mov(rsp, rbp);
    code.pop(rbp);
    code.ret();
    return entry;
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    static const bool own_stack = std::getenv("SOTC_NO_ALTSTACK") == nullptr;
    static const StackCall stack_call = MakeStackCall();
    static thread_local u8* handler_stack = nullptr;
    if (!own_stack) {
        return SignalHandlerBody(pExp);
    }
    if (handler_stack == nullptr) {
        handler_stack = static_cast<u8*>(
            VirtualAlloc(nullptr, HandlerStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    const u8* here = reinterpret_cast<const u8*>(&pExp);
    // No stack to be had, or a fault inside the handler itself: carry on where we are.
    if (handler_stack == nullptr ||
        (here >= handler_stack && here < handler_stack + HandlerStackSize)) {
        return SignalHandlerBody(pExp);
    }
    // Windows checks stack addresses against these bounds when it unwinds.
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    void* const base = tib->StackBase;
    void* const limit = tib->StackLimit;
    tib->StackBase = handler_stack + HandlerStackSize;
    tib->StackLimit = handler_stack;
    const LONG result =
        stack_call(SignalHandlerBody, pExp, handler_stack + HandlerStackSize - 64);
    tib->StackBase = base;
    tib->StackLimit = limit;
    return result;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = (void*)context.uc_mcontext.mc_rip;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        const bool is_exec = Common::IsExecuteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address),
                            is_write  ? "Write to"
                            : is_exec ? "Executed from"
                                      : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
}

void SignalDispatch::RemoveHandlers() {
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        std::quick_exit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        std::quick_exit(1);
    }
#endif
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
