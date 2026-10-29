#pragma once

// 情景模式

#include "../util/Logger.hpp"
#include "../util/Utils.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/inotify.h>
#include <unistd.h>

class ModeWatcher {
public:
    explicit ModeWatcher(Logger& logger) : logger_(logger) {}

    bool attach(const std::string& modeFile) {
        file_ = baseName(modeFile);
        const std::string watchDir = realDirectory(directoryOf(modeFile));

        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) {
            logger_.Warn("初始化 inotify 失败: %s", strerror(errno));
            return false;
        }

        const uint32_t mask =
            IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE | IN_MODIFY |
            IN_DELETE | IN_MOVED_FROM | IN_ATTRIB;
        if (inotify_add_watch(fd_, watchDir.c_str(), mask) < 0) {
            logger_.Warn("无法监听 %s：%s", watchDir.c_str(), strerror(errno));
            close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    bool changed() {
        if (fd_ < 0) return false;

        bool dirty = false;
        char events[4096];
        for (;;) {
            const ssize_t length = read(fd_, events, sizeof(events));
            if (length <= 0) break;

            for (ssize_t remaining = length;
                 remaining >= static_cast<ssize_t>(sizeof(inotify_event));) {
                const inotify_event* event =
                    reinterpret_cast<const inotify_event*>(events + length - remaining);
                dirty = dirty || (event->mask & IN_Q_OVERFLOW) != 0 ||
                        event->len == 0 || file_ == event->name;
                remaining -= static_cast<ssize_t>(sizeof(inotify_event)) + event->len;
            }
        }
        return dirty;
    }

    static std::string readMode(const std::string& path) {
        std::string value;
        if (std::ifstream input(path); input && std::getline(input, value))
            value = trim(value);
        for (const char* mode : {"powersave", "balance", "performance", "fast"})
            if (value == mode) return mode;
        return "balance";
    }

private:
    static std::string trim(const std::string& value) {
        size_t begin = 0;
        size_t end = value.size();
        while (begin < end && isspace(static_cast<unsigned char>(value[begin])))
            ++begin;
        while (end > begin &&
               isspace(static_cast<unsigned char>(value[end - 1])))
            --end;
        return value.substr(begin, end - begin);
    }

    static std::string directoryOf(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? "." : path.substr(0, slash);
    }

    static std::string baseName(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    static std::string realDirectory(const std::string& directory) {
        static const std::string kSdcard = "/sdcard";
        static const std::string kEmulated = "/storage/emulated/0";
        static const std::string kReal = "/data/media/0";
        if (directory == kSdcard || directory == kEmulated) return kReal;
        if (directory.rfind(kSdcard + "/", 0) == 0)
            return kReal + directory.substr(kSdcard.size());
        if (directory.rfind(kEmulated + "/", 0) == 0)
            return kReal + directory.substr(kEmulated.size());
        return directory;
    }

    Logger& logger_;
    std::string file_;
    int fd_ = -1;
};
