#pragma once

// 守护重启

#include "CgroupEscape.hpp"
#include "Scheduler.hpp"
#include "../util/Utils.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

class Supervisor {
public:
    Supervisor(std::string configPath, char* argv0)
        : configPath_(std::move(configPath)), argv0_(argv0) {
        prctl(PR_SET_NAME, "fas-super", 0, 0, 0);
        setProgramName("fas-super");
        signal(SIGPIPE, SIG_IGN);

        struct sigaction action {};
        action.sa_handler = &Supervisor::relaySignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        for (const int sig : {SIGTERM, SIGINT, SIGHUP})
            sigaction(sig, &action, nullptr);

        for (const pid_t pid : Utils::FindProcesses("fas-super"))
            if (pid != getpid()) kill(pid, SIGKILL);

        CgroupEscape::Escape();
    }

    void run() {
        uint64_t backoffSec = 1;
        for (;;) {
            if (access(kWatchdogOff, F_OK) == 0) {
                sleep(10);
                continue;
            }

            int pipes[2];
            if (pipe2(pipes, O_CLOEXEC) != 0) {
                sleep(5);
                continue;
            }

            const uint64_t startedNs = Utils::NowNs();
            const pid_t pid = fork();
            if (pid < 0) {
                close(pipes[0]);
                close(pipes[1]);
                sleep(5);
                continue;
            }

            if (pid == 0) {
                close(pipes[0]);
                childPid_ = 0;
                signal(SIGTERM, SIG_DFL);
                signal(SIGINT, SIG_DFL);
                signal(SIGHUP, SIG_DFL);
                prctl(PR_SET_NAME, "fas", 0, 0, 0);
                setProgramName("fas");
                _exit(Scheduler(configPath_, false).run());
            }

            close(pipes[1]);
            childPid_ = pid;

            char sink[1];
            while (read(pipes[0], sink, 1) < 0 && errno == EINTR) {
            }
            close(pipes[0]);
            childPid_ = 0;

            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            int code = 0;
            if (WIFEXITED(status)) code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);

            char line[192];
            char when[24];
            stampNow(when, sizeof(when));
            snprintf(line, sizeof(line), "%swatchdog: fas 退出 code=%d\n", when, code);
            appendWatchdog(line);

            if (code == 0) {
                snprintf(line, sizeof(line), "%swatchdog: 干净退出，停止守护\n", when);
                appendWatchdog(line);
                return;
            }

            if (Utils::NowNs() - startedNs > 60000000000ULL) backoffSec = 1;
            sleep(static_cast<unsigned>(backoffSec));
            backoffSec = backoffSec < 30 ? backoffSec * 2 : 30;
        }
    }

private:
    static constexpr const char* kWatchdogOff = "/sdcard/Android/fas-youran/watchdog.off";
    static constexpr const char* kWatchdogLog = "/sdcard/Android/fas-youran/watchdog.log";

    inline static volatile pid_t childPid_ = 0;

    // 信号转发
    static void relaySignal(int sig) {
        const pid_t child = childPid_;
        if (child > 0) kill(child, sig);
    }

    void setProgramName(const char* alias) {
        if (argv0_ == nullptr) return;
        const size_t room = strlen(argv0_);
        const size_t want = strlen(alias);
        if (room < want) return;
        memset(argv0_, 0, room + 1);
        memcpy(argv0_, alias, want);
    }

    static void stampNow(char* buffer, const size_t size) {
        const time_t now = time(nullptr);
        struct tm local {};
        localtime_r(&now, &local);
        strftime(buffer, size, "%Y-%m-%d %H:%M:%S ", &local);
    }

    static void appendWatchdog(const char* text) {
        const int fd = open(kWatchdogLog, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return;
        write(fd, text, strlen(text));
        close(fd);
    }

    std::string configPath_;
    char* argv0_ = nullptr;
};
