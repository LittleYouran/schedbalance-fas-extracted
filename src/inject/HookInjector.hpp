#pragma once

// Hook 注入

#include "../util/Logger.hpp"
#include "../util/Utils.hpp"

#include <android/api-level.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class HookInjector {
public:
    HookInjector(Logger& logger, const std::string& directory)
        : logger_(logger), directory_(directory) {}

    void start() {
        sfPid_ = waitSurfaceFlinger();
        if (sfPid_ <= 0) {
            logger_.Error("找不到 surfaceflinger，跳过 Hook 注入");
            return;
        }
        inject(sfPid_);
    }

    // 检查 SF
    void check(const uint64_t now) {
        if (now < nextCheckNs_) return;

        if (injected_ && sfPid_ > 0 && live(sfPid_)) {
            nextCheckNs_ = now + kCheckNs;
            return;
        }
        if (failCount_ >= 5) {
            nextCheckNs_ = now + kCheckNs * 10;
            return;
        }
        nextCheckNs_ = now + (kCheckNs << (failCount_ < 4 ? failCount_ : 4));

        const int sf = Utils::FindProcess("surfaceflinger");
        if (sf <= 0) return;
        if (sfPid_ > 0 && sf != sfPid_) {
            logger_.Warn("SurfaceFlinger 已重启（%d -> %d），重新注入 Hook", sfPid_, sf);
            injected_ = false;
            failCount_ = 0;
        }
        sfPid_ = sf;
        inject(sf);
    }

private:
    static constexpr uint64_t kCheckNs = 3000000000ULL;
    std::string hookPath_ = "/data/local/tmp/fas-hook.so";

    static bool live(const int pid) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE* file = fopen(path, "rb");
        if (!file) return false;

        char comm[32] = {};
        const bool ok = fgets(comm, sizeof(comm), file) != nullptr;
        fclose(file);
        if (!ok) return false;
        comm[strcspn(comm, "\r\n")] = '\0';
        return strcmp(comm, "surfaceflinger") == 0;
    }

    static bool hasModule(const int pid, const char* needle) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/maps", pid);
        FILE* file = fopen(path, "rb");
        if (!file) return false;

        bool found = false;
        char line[1024];
        while (!found && fgets(line, sizeof(line), file))
            found = strstr(line, needle) != nullptr;
        fclose(file);
        return found;
    }

    template <typename Fn>
    static bool waitFor(Fn&& ready, const int attempts, const int intervalMs) {
        for (int i = 0; i < attempts; ++i) {
            if (ready()) return true;
            usleep(intervalMs * 1000);
        }
        return false;
    }

    static int waitSurfaceFlinger() {
        int pid = 0;
        if (!waitFor([&] { return (pid = Utils::FindProcess("surfaceflinger")) > 0; },
                     30, 500))
            return 0;
        if (android_get_device_api_level() >= 35)
            waitFor([&] { return hasModule(pid, "libtracing_perfetto.so"); }, 12, 500);
        return pid;
    }

    static std::string withSuffix(const std::string& base, const int suffix) {
        const size_t dot = base.rfind(".so");
        if (dot == std::string::npos)
            return base + "-" + std::to_string(suffix);
        return base.substr(0, dot) + "-" + std::to_string(suffix) +
               base.substr(dot);
    }

    std::string deploy(const std::string& sourceName,
                       const std::string& destBase) {
        const std::string source = directory_ + "/" + sourceName;
        std::string dest = destBase;
        if (access(dest.c_str(), F_OK) == 0 &&
            !Utils::FilesDiffer(source, dest)) {
            logger_.Info("复用已有库 %s", dest.c_str());
            return dest;
        }
        if (access(dest.c_str(), F_OK) == 0) {
            for (int suffix = 2;; ++suffix) {
                dest = withSuffix(destBase, suffix);
                if (access(dest.c_str(), F_OK) != 0) break;
                if (!Utils::FilesDiffer(source, dest)) {
                    logger_.Info("复用已有库 %s", dest.c_str());
                    return dest;
                }
            }
        }
        if (!Utils::CopyFile(source, dest)) {
            logger_.Error("复制库失败: %s", source.c_str());
            return {};
        }
        chmod(dest.c_str(), 0644);
        logger_.Info("部署库 %s", dest.c_str());
        return dest;
    }

    bool runInjector(const int pid, const std::string& library,
                     const char* label, const char* entry) {
        std::string command =
            "'" + directory_ + "/fas-injector' -p " + std::to_string(pid) +
            " -l '" + library + "'";
        if (entry && *entry) command += std::string(" -s '") + entry + "'";
        command += " 2>&1";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            logger_.Error("无法启动 fas-injector");
            return false;
        }

        char line[512];
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\r\n")] = '\0';
            logger_.Info("注入: %s", line);
        }
        const int status = pclose(pipe);
        if (status == 0) {
            logger_.Info("%s 注入成功（surfaceflinger pid=%d）", label, pid);
            return true;
        }
        logger_.Error("%s 注入失败（status=%d）", label, status);
        return false;
    }

    void inject(const int pid) {
        // 执行注入
        const std::string path = deploy("libfas_hook.so", hookPath_);
        if (path.empty()) return;
        if (runInjector(pid, path, "Hook", "fas_hook_init")) {
            injected_ = true;
            failCount_ = 0;
            dropDeployed(path);
            return;
        }
        ++failCount_;
    }

    void dropDeployed(const std::string& loaded) {
        unlink(loaded.c_str());
        for (int suffix = 2; suffix <= 9; ++suffix) {
            const std::string stale = withSuffix(hookPath_, suffix);
            if (access(stale.c_str(), F_OK) == 0) unlink(stale.c_str());
        }
        logger_.Info("已清理落地库 %s（SF 已持有映射）", loaded.c_str());
    }

    Logger& logger_;
    std::string directory_;
    int sfPid_ = 0;
    uint64_t nextCheckNs_ = 0;
    bool injected_ = false;
    int failCount_ = 0;
};
