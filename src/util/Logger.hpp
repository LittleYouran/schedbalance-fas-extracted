#pragma once

// 日志

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static constexpr int kLineMax = 1024;
    static constexpr int64_t kRotateSize = 8LL * 1024 * 1024;

    static constexpr const char* kLevelTexts[] = {
        "调试 ->", "信息 ->", "警告 ->", "错误 ->",
    };

    void setPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        if (path_.empty()) return;
        char stamp[24];
        Stamp(stamp, sizeof(stamp));
        if (FILE* file = fopen(path_.c_str(), "w")) {
            fprintf(file, "-------- %sfas 启动 --------\n", stamp);
            fclose(file);
        }
    }

    void Debug(const char* format, ...) {
        va_list args;
        va_start(args, format);
        Emit(LogLevel::Debug, format, args);
        va_end(args);
    }

    void Info(const char* format, ...) {
        va_list args;
        va_start(args, format);
        Emit(LogLevel::Info, format, args);
        va_end(args);
    }

    void Warn(const char* format, ...) {
        va_list args;
        va_start(args, format);
        Emit(LogLevel::Warn, format, args);
        va_end(args);
    }

    void Error(const char* format, ...) {
        va_list args;
        va_start(args, format);
        Emit(LogLevel::Error, format, args);
        va_end(args);
    }

private:
    void Emit(const LogLevel level, const char* format, va_list args) {
        char stamp[24];
        Stamp(stamp, sizeof(stamp));

        char line[kLineMax];
        const int head = snprintf(
            line, sizeof(line), "%s%s ", stamp, kLevelTexts[static_cast<int>(level)]);
        if (head < 0) return;
        vsnprintf(line + head, sizeof(line) - static_cast<size_t>(head), format, args);

        std::lock_guard<std::mutex> lock(mutex_);
        if (level >= LogLevel::Warn || path_.empty())
            fprintf(stderr, "%s\n", line);
        Submit(line);
    }

    void Submit(const char* line) {
        if (path_.empty()) return;

        struct stat info {};
        if (stat(path_.c_str(), &info) == 0 && info.st_size > kRotateSize)
            rename(path_.c_str(), (path_ + ".old").c_str());

        FILE* file = fopen(path_.c_str(), "a");
        if (!file) return;
        fwrite(line, 1, strlen(line), file);
        fputc('\n', file);
        fclose(file);
    }

    static void Stamp(char* buffer, const size_t size) {
        const time_t now = time(nullptr);
        struct tm local {};
        localtime_r(&now, &local);
        strftime(buffer, size, "%Y-%m-%d %H:%M:%S ", &local);
    }

    std::mutex mutex_;
    std::string path_;
};


