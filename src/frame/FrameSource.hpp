#pragma once

// 帧源

#include "../util/Logger.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

class FrameSource {
public:
    struct Frame {
        uint64_t tsNs = 0;
        double ms = 0;
        int flags = 0;
    };

    static constexpr int kCapacity = 256;

    explicit FrameSource(Logger& logger) : logger_(logger) {}
    ~FrameSource() { stop(); }

    void start() {
        if (worker_.joinable()) return;
        stopped_ = false;
        worker_ = std::thread([this] { run(); });
        logger_.Info("异步帧源已启动");
    }

    void stop() {
        if (!worker_.joinable()) return;
        stopped_ = true;
        const pid_t pid = logcat_.load();
        if (pid > 0) kill(pid, SIGKILL);
        pauseCv_.notify_all();
        worker_.join();
    }

    bool pop(Frame& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (head_ == tail_) return false;
        out = ring_[tail_];
        tail_ = next(tail_);
        return true;
    }

    void setPaused(const bool paused) {
        if (paused_.exchange(paused) == paused) return;
        if (paused && logcat_.load() > 0) kill(logcat_.load(), SIGKILL);
        pauseCv_.notify_all();
    }

    uint64_t droppedCount() const { return dropped_; }

    int depth() const {
        std::lock_guard<std::mutex> lock(mutex_);
        int d = head_ - tail_;
        if (d < 0) d += kCapacity;
        return d;
    }

private:
    void run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(pauseMutex_);
                pauseCv_.wait(lock, [this] {
                    return stopped_.load() || !paused_.load();
                });
            }
            if (stopped_) break;

            if (stream_ < 0 && !spawn()) {
                rest();
                continue;
            }
            if (stopped_) break;
            if (paused_) {
                shutdown();
                continue;
            }

            char chunk[4096];
            const ssize_t length = read(stream_, chunk, sizeof(chunk));
            if (length > 0) {
                feed(chunk, static_cast<size_t>(length));
                continue;
            }
            if (length < 0 && errno == EINTR) continue;

            shutdown();
            if (!stopped_ && !paused_) rest();
        }
        shutdown();
    }

    bool spawn() {
        int pipes[2];
        if (pipe(pipes) != 0) {
            logger_.Error("创建帧管道失败: %s", strerror(errno));
            return false;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            close(pipes[0]);
            close(pipes[1]);
            logger_.Error("fork logcat 失败: %s", strerror(errno));
            return false;
        }

        if (pid == 0) {
            dup2(pipes[1], STDOUT_FILENO);
            close(pipes[0]);
            close(pipes[1]);
            execlp("logcat", "logcat", "-b", "main", "-v", "tag", "-s",
                   "FAS_FRAME:I", "FAS_BATCH:I", "FAS_HOOK:I", "FAS_TRACE:I",
                   "-T", "1", static_cast<char*>(nullptr));
            _exit(127);
        }

        close(pipes[1]);
        logcat_ = pid;
        stream_ = pipes[0];
        pending_.clear();
        return true;
    }

    void shutdown() {
        if (stream_ >= 0) {
            close(stream_);
            stream_ = -1;
        }
        const pid_t pid = logcat_.exchange(-1);
        if (pid > 0) {
            kill(pid, SIGKILL);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        pending_.clear();
    }

    static void rest() { usleep(500000); }

    void feed(const char* data, const size_t length) {
        pending_.append(data, length);
        size_t line = 0;
        for (;;) {
            const size_t end = pending_.find('\n', line);
            if (end == std::string::npos) break;
            parse(pending_.substr(line, end - line));
            line = end + 1;
        }
        if (line > 0) pending_.erase(0, line);
        if (pending_.size() > 65536) pending_.clear();
    }

    void parse(const std::string& text) {
        const size_t batch = text.find("FAS_BATCH:");
        if (batch != std::string::npos) {
            const char* cursor = text.c_str() + batch + 10;
            while (*cursor) {
                unsigned long long seq = 0;
                unsigned long long ts = 0;
                unsigned long long us = 0;
                int flags = 0;
                int used = 0;
                if (sscanf(cursor, "%llu %llu %llu %d%n",
                           &seq, &ts, &us, &flags, &used) != 4 ||
                    used <= 0)
                    break;
                if (us >= 1000)
                    push(Frame{ts, static_cast<double>(us) / 1000.0, flags});
                cursor += used;
                if (*cursor != ';') break;
                ++cursor;
            }
            return;
        }

        const size_t frame = text.find("FAS_FRAME:");
        if (frame != std::string::npos) {
            unsigned long long ts = 0;
            unsigned long long us = 0;
            unsigned long long seq = 0;
            int flags = 0;
            if (sscanf(text.c_str() + frame + 10, "%llu %llu %llu %d",
                       &seq, &ts, &us, &flags) != 4 ||
                us < 1000)
                return;

            push(Frame{ts, static_cast<double>(us) / 1000.0, flags});
            return;
        }

        const size_t hook = text.find("FAS_HOOK:");
        if (hook != std::string::npos)
            logger_.Info("Hook: %s", text.c_str() + hook + 9);

        const size_t trace = text.find("FAS_TRACE:");
        if (trace != std::string::npos)
            logger_.Debug("Trace: %s", text.c_str() + trace + 10);
    }

    void push(const Frame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (next(head_) == tail_) {
            ++dropped_;
            return;
        }
        ring_[head_] = frame;
        head_ = next(head_);
    }

    static int next(const int index) { return (index + 1) % kCapacity; }

    Logger& logger_;
    std::thread worker_;
    std::atomic<bool> stopped_{false};
    std::atomic<uint64_t> dropped_{0};

    mutable std::mutex mutex_;
    Frame ring_[kCapacity] = {};
    int head_ = 0;
    int tail_ = 0;

    std::atomic<pid_t> logcat_{-1};
    int stream_ = -1;
    std::string pending_;

    std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
    std::atomic<bool> paused_{false};
};
