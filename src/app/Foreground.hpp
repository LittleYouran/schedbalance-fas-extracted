#pragma once

// 前台识别

#include "../conf/Config.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <utility>
#include <vector>

struct Foreground {
    std::string package;
    int pid = -1;

    bool empty() const { return package.empty() || pid <= 0; }
};

class ForegroundWatcher {
public:
    explicit ForegroundWatcher(const Config& config) : config_(config) {}

    Foreground find() {
        if (!last_.empty() && config_.games.count(last_.package) &&
            stillTop(last_.pid, last_.package))
            return last_;

        std::string raw;
        std::vector<int> pids;
        if (!readTop("/dev/cpuset/top-app/cgroup.procs", raw, pids)) {
            last_ = {};
            lastTopRaw_.clear();
            return last_;
        }

        // 名单缓存
        if (last_.empty() && raw == lastTopRaw_ && ++cacheHits_ < 5)
            return last_;
        cacheHits_ = 0;

        Foreground best;
        bool bestMain = false;
        for (const int pid : pids) {
            const std::string process = readProcess(pid);
            if (process.empty()) continue;

            const size_t suffix = process.find(':');
            const bool main = suffix == std::string::npos;
            const std::string package =
                main ? process : process.substr(0, suffix);
            if (!config_.games.count(package)) continue;

            if (best.empty() || (main && !bestMain) ||
                (main == bestMain && pid > best.pid)) {
                best.package = package;
                best.pid = pid;
                bestMain = main;
            }
        }
        lastTopRaw_ = std::move(raw);
        last_ = best;
        return best;
    }

    void resetCache() {
        last_ = {};
        lastTopRaw_.clear();
        cacheHits_ = 0;
    }

private:
    static bool stillTop(const int pid, const std::string& package) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
        char buffer[1024] = {};
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (length <= 0 || !strstr(buffer, "top-app")) return false;

        const std::string process = readProcess(pid);
        return process == package;
    }

    static bool readTop(const char* path, std::string& raw,
                        std::vector<int>& pids) {
        char buffer[8192] = {};
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (length < 0) return false;
        if (length == 0) return true;

        raw.assign(buffer, static_cast<size_t>(length));
        const char* cursor = buffer;
        const char* end = buffer + length;
        while (cursor < end) {
            char* next = nullptr;
            const long value = strtol(cursor, &next, 10);
            if (next == cursor) break;
            if (value > 0) pids.push_back(static_cast<int>(value));
            cursor = next;
        }
        return true;
    }

    static std::string readProcess(const int pid) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        char buffer[512] = {};
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return {};
        const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (length <= 0) return {};

        const std::string process(buffer, strnlen(buffer, length));
        if (process.find('.') == std::string::npos ||
            process.find_first_of("/ \t\r\n") != std::string::npos ||
            !std::isalpha(static_cast<unsigned char>(process.front())))
            return {};
        return process;
    }

    const Config& config_;
    Foreground last_;
    std::string lastTopRaw_;
    int cacheHits_ = 0;
};
