// Native crash reporter for bug reports. Android's own tombstone is not always reachable (many handhelds' ROMs keep none),
// so on SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT this writes the signal, fault address and a backtrace (module + offset, ready
// for addr2line against the matching unstripped libmain.so) to logcat and to <files>/Logs/crash-<pid>.txt, then hands the
// signal on to the previous handler (Android's debuggerd), so the process still ends the way it would have.
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

namespace
{
constexpr int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
constexpr int kMaxFrames = 48;

struct sigaction g_old[NSIG];
char g_path[512] = {};
volatile sig_atomic_t g_inHandler = 0;

struct Trace
{
    void* frames[kMaxFrames];
    int count = 0;
};

_Unwind_Reason_Code UnwindStep(_Unwind_Context* ctx, void* arg)
{
    Trace* t = static_cast<Trace*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc == 0)
        return _URC_NO_REASON;
    if (t->count >= kMaxFrames)
        return _URC_END_OF_STACK;
    t->frames[t->count++] = reinterpret_cast<void*>(pc);
    return _URC_NO_REASON;
}

void Emit(int fd, const char* text, size_t len)
{
    __android_log_write(ANDROID_LOG_FATAL, "cane", text);
    if (fd >= 0)
        (void)!write(fd, text, len);
}

void Handler(int sig, siginfo_t* info, void* context)
{
    if (g_inHandler == 0)
    {
        g_inHandler = 1;
        int fd = g_path[0] != 0 ? open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
        char line[400];
        int n = snprintf(line, sizeof(line), "*** native crash: signal %d (%s) code %d fault addr %p tid %d pid %d\n", sig,
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "SIGABRT",
            info != nullptr ? info->si_code : 0, info != nullptr ? info->si_addr : nullptr, static_cast<int>(gettid()),
            static_cast<int>(getpid()));
        Emit(fd, line, static_cast<size_t>(n));

        Trace trace;
        _Unwind_Backtrace(UnwindStep, &trace);
        for (int i = 0; i < trace.count; ++i)
        {
            Dl_info di;
            if (dladdr(trace.frames[i], &di) != 0 && di.dli_fname != nullptr)
            {
                const char* base = strrchr(di.dli_fname, '/');
                base = base != nullptr ? base + 1 : di.dli_fname;
                const uintptr_t off = reinterpret_cast<uintptr_t>(trace.frames[i]) - reinterpret_cast<uintptr_t>(di.dli_fbase);
                n = snprintf(line, sizeof(line), "  #%02d pc %p  %s+0x%lx%s%s\n", i, trace.frames[i], base,
                    static_cast<unsigned long>(off), di.dli_sname != nullptr ? "  " : "", di.dli_sname != nullptr ? di.dli_sname : "");
            }
            else
            {
                n = snprintf(line, sizeof(line), "  #%02d pc %p\n", i, trace.frames[i]);
            }
            Emit(fd, line, static_cast<size_t>(n));
        }
        if (fd >= 0)
            close(fd);
    }

    // Hand over to whatever was installed before us (debuggerd), so the crash ends as it normally would.
    struct sigaction& old = g_old[sig];
    if (old.sa_flags & SA_SIGINFO && old.sa_sigaction != nullptr)
    {
        old.sa_sigaction(sig, info, context);
    }
    else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler != nullptr)
    {
        old.sa_handler(sig);
    }
    else
    {
        signal(sig, SIG_DFL);
        if (sig == SIGABRT)
            raise(sig);   // synchronous faults simply re-fault into the default action on return
    }
}
}

extern "C" void cane_install_crash_handler(const char* logsDir)
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    if (logsDir != nullptr && logsDir[0] != 0)
        snprintf(g_path, sizeof(g_path), "%s/crash-%d.txt", logsDir, static_cast<int>(getpid()));

    // A fault on a stack overflow needs its own stack to run the handler on.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);

    for (int sig : kSignals)
    {
        struct sigaction sa{};
        sa.sa_sigaction = Handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &g_old[sig]);
    }
}
