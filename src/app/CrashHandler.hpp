#pragma once

// 崩溃处理

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

class CrashHandler {
public:
    static void install(const std::string& logPath) {
        if (logPath.empty()) return;
        fd_ = open(logPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (fd_ < 0) return;

        stack_t stack {};
        stack.ss_sp = altStack_;
        stack.ss_size = sizeof(altStack_);
        stack.ss_flags = 0;
        sigaltstack(&stack, nullptr);

        struct sigaction action {};
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = &CrashHandler::onCrash;
        action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (const int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE})
            sigaction(sig, &action, nullptr);
    }

private:
    static constexpr int kMaxFrames = 40;

    struct Frames {
        uintptr_t address[kMaxFrames];
        int count;
    };

    // 回溯帧
    static _Unwind_Reason_Code collectFrame(struct _Unwind_Context* context,
                                            void* arg) {
        auto* state = static_cast<Frames*>(arg);
        if (state->count >= kMaxFrames) return _URC_END_OF_STACK;
        const uintptr_t pc = static_cast<uintptr_t>(_Unwind_GetIP(context));
        if (pc == 0) return _URC_END_OF_STACK;
        state->address[state->count++] = pc;
        return _URC_NO_REASON;
    }

    static void writeRaw(const char* text, size_t length) {
        if (fd_ < 0) return;
        while (length > 0) {
            const ssize_t written = write(fd_, text, length);
            if (written <= 0) return;
            text += written;
            length -= static_cast<size_t>(written);
        }
    }

    static void dumpFrames() {
        Frames state {};
        _Unwind_Backtrace(collectFrame, &state);
        char buffer[kMaxFrames * 12 + 64];
        int offset = snprintf(buffer, sizeof(buffer), "\n[crash-frames]");
        for (int i = 0; i < state.count && offset > 0; ++i) {
            if (static_cast<size_t>(offset) + 16 >= sizeof(buffer)) break;
            const int written =
                snprintf(buffer + offset, sizeof(buffer) - static_cast<size_t>(offset),
                         " %lx", static_cast<unsigned long>(state.address[i]));
            if (written <= 0) break;
            offset += written;
        }
        if (offset > 0) writeRaw(buffer, static_cast<size_t>(offset));
        writeRaw("\n[/crash-frames]\n", 17);
    }

    static void dumpMaps() {
        const int source = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (source < 0) return;
        writeRaw("\n[crash-maps]\n", 14);
        char buffer[2048];
        ssize_t length = 0;
        while ((length = read(source, buffer, sizeof(buffer))) > 0)
            writeRaw(buffer, static_cast<size_t>(length));
        close(source);
        writeRaw("\n[/crash-maps]\n", 15);
    }

    static void onCrash(int sig, siginfo_t* info, void* context) {
        if (fd_ >= 0) {
            uintptr_t pc = 0;
            uintptr_t lr = 0;
            uintptr_t sp = 0;
            if (context != nullptr) {
                const auto* uc = static_cast<const ucontext_t*>(context);
                pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
                lr = static_cast<uintptr_t>(uc->uc_mcontext.regs[30]);
                sp = static_cast<uintptr_t>(uc->uc_mcontext.sp);
            }
            char buffer[288];
            const int length = snprintf(buffer, sizeof(buffer),
                                        "\n[crash] signal=%d code=%d addr=%p pid=%d"
                                        " pc=%lx lr=%lx sp=%lx\n",
                                        sig, info ? info->si_code : 0,
                                        info ? info->si_addr : nullptr,
                                        static_cast<int>(getpid()),
                                        static_cast<unsigned long>(pc),
                                        static_cast<unsigned long>(lr),
                                        static_cast<unsigned long>(sp));
            if (length > 0) {
                const size_t size = static_cast<size_t>(length) < sizeof(buffer)
                                        ? static_cast<size_t>(length)
                                        : sizeof(buffer) - 1;
                writeRaw(buffer, size);
            }
            dumpFrames();
            dumpMaps();
        }
        raise(sig);
    }

    inline static int fd_ = -1;
    alignas(16) inline static char altStack_[64 * 1024];
};
