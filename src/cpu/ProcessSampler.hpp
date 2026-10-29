#pragma once

// 线程采样

#include "../util/Utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sched.h>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

class ProcessSampler {
public:
    struct Thread {
        int tid = 0;
        std::string name;
        uint64_t prevTicks = 0;
        double usage = 0;
        int cpu = -1;
    };

    static constexpr int kTopThreads = 8;
    static constexpr uint64_t kSampleNs = 300000000ULL;
    static constexpr uint64_t kRescanNs = 3000000000ULL;
    static constexpr uint64_t kSlowRescanNs = 12000000000ULL;

    ProcessSampler() : clockHz_(sysconf(_SC_CLK_TCK) > 0 ?
                                static_cast<double>(sysconf(_SC_CLK_TCK)) : 100) {}

    void setPid(const int pid) {
        pid_ = pid;
        threads_.clear();
        cores_.clear();
        util_ = 0;
        lastScanNs_ = 0;
        lastSampleNs_ = 0;
        if (pid > 0) rescan(Utils::NowNs());
    }

    void setTrackCores(const bool value) { trackCores_ = value; }

    bool sample(double& util, std::set<int>& cores) {
        if (pid_ <= 0) return false;

        const uint64_t now = Utils::NowNs();
        if (lastScanNs_ == 0 || now - lastScanNs_ >= rescanGap_) rescan(now);
        if (lastSampleNs_ == 0 || now - lastSampleNs_ < kSampleNs) return false;

        const double seconds = static_cast<double>(now - lastSampleNs_) / 1e9;
        lastSampleNs_ = now;

        double best = 0;
        bool live = false;
        cores.clear();
        for (Thread& thread : threads_) {
            uint64_t ticks = 0;
            int cpu = -1;
            if (!readStat(thread.tid, ticks, &cpu)) continue;

            const uint64_t delta =
                ticks > thread.prevTicks ? ticks - thread.prevTicks : 0;
            thread.usage = seconds > 0 ?
                static_cast<double>(delta) / (seconds * clockHz_) : 0;
            thread.prevTicks = ticks;
            if (cpu >= 0) thread.cpu = cpu;
            best = std::max(best, thread.usage);
            live = true;

            if (trackCores_) {
                cpu_set_t mask;
                CPU_ZERO(&mask);
                if (sched_getaffinity(thread.tid, sizeof(mask), &mask) == 0) {
                    for (int core = 0; core < CPU_SETSIZE; ++core)
                        if (CPU_ISSET(core, &mask)) cores.insert(core);
                }
            }
        }
        if (!live) {
            rescan(now);
            return false;
        }

        util_ = best;
        util = util_;
        cores_ = cores;
        return true;
    }

    double util() const { return util_; }
    const std::set<int>& cores() const { return cores_; }

    std::string describe() const {
        std::string text;
        for (const Thread& thread : threads_) {
            if (thread.usage <= 0) continue;
            if (!text.empty()) text += " / ";
            text += thread.name.empty() ? "tid" + std::to_string(thread.tid)
                                        : thread.name;
            if (thread.cpu >= 0) text += "@c" + std::to_string(thread.cpu);
            text += " " + std::to_string(static_cast<int>(thread.usage * 100)) + "%";
        }
        return text;
    }

private:
    void rescan(const uint64_t now) {
        lastScanNs_ = now;
        lastSampleNs_ = now;

        const std::string path = "/proc/" + std::to_string(pid_) + "/task";
        DIR* dir = opendir(path.c_str());
        if (!dir) return;

        threads_.clear();
        dirent* entry = nullptr;
        uint64_t allTicks = 0;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;

            Thread thread;
            if (!readStat(atoi(entry->d_name), thread.prevTicks, &thread.cpu,
                          &thread.name))
                continue;
            thread.tid = atoi(entry->d_name);
            allTicks += thread.prevTicks;
            threads_.push_back(std::move(thread));
        }
        closedir(dir);

        std::sort(threads_.begin(), threads_.end(),
                  [](const Thread& a, const Thread& b) {
                      return a.prevTicks > b.prevTicks;
                  });
        if (threads_.size() > kTopThreads) threads_.resize(kTopThreads);

        uint64_t topTicks = 0;
        for (const Thread& thread : threads_) topTicks += thread.prevTicks;
        rescanGap_ = (allTicks > 0 && topTicks * 4 >= allTicks * 3)
                         ? kSlowRescanNs
                         : kRescanNs;
    }

    bool readStat(const int tid, uint64_t& ticks, int* cpu,
                  std::string* name = nullptr) const {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid_, tid);
        FILE* file = fopen(path, "rb");
        if (!file) return false;

        char buffer[1024] = {};
        const size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        if (length == 0) return false;
        buffer[length] = '\0';

        const char* open = strchr(buffer, '(');
        const char* close = strrchr(buffer, ')');
        if (!open || !close || close <= open) return false;
        if (name) name->assign(open + 1, close - open - 1);

        const char* cursor = close + 1;
        uint64_t total = 0;
        int field = 2;
        while (*cursor && field < 39) {
            cursor += strspn(cursor, " \t");
            if (!*cursor) break;

            ++field;
            if (field == 14 || field == 15)
                total += strtoull(cursor, nullptr, 10);
            cursor += strcspn(cursor, " \t\n");
        }

        cursor += strspn(cursor, " \t");
        if (cpu)
            *cpu = *cursor ? static_cast<int>(strtol(cursor, nullptr, 10)) : -1;

        ticks = total;
        return true;
    }

    int pid_ = -1;
    double clockHz_ = 100;
    std::vector<Thread> threads_;
    std::set<int> cores_;
    double util_ = 0;
    bool trackCores_ = true;
    uint64_t lastScanNs_ = 0;
    uint64_t lastSampleNs_ = 0;
    uint64_t rescanGap_ = kRescanNs;
};
