#pragma once

// 频率执行层

#include "../util/Logger.hpp"
#include "../util/Utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iterator>
#include <set>
#include <string>
#include <vector>

class CpuRuntime {
public:
    struct Policy {
        int id = 0;
        std::string dir;
        std::string governor;
        std::vector<int> freqs;
        std::vector<int> cpus;
        int hwMin = 0;
        int hwMax = 0;
        int target = 0;
        int realCur = 0;
        double load = -1;
        int lastMin = -1;
        int lastMax = -1;
    };

    bool init(Logger& logger) {
        logger_ = &logger;
        DIR* dir = opendir("/sys/devices/system/cpu/cpufreq");
        if (!dir) {
            logger_->Error("无法打开 cpufreq 目录");
            return false;
        }

        dirent* entry = nullptr;
        while ((entry = readdir(dir))) {
            if (strncmp(entry->d_name, "policy", 6) != 0) continue;

            Policy policy;
            policy.id = atoi(entry->d_name + 6);
            policy.dir =
                std::string("/sys/devices/system/cpu/cpufreq/") + entry->d_name;
            ReadFreqs(policy.dir + "/scaling_available_frequencies",
                      policy.freqs);
            policy.governor = ReadText(policy.dir + "/scaling_governor");
            if (!ReadCpus(policy.dir + "/affected_cpus", policy.cpus) ||
                policy.freqs.empty())
                continue;
            policy.hwMin = policy.freqs.front();
            policy.hwMax = policy.freqs.back();
            policy.target = policy.hwMax;
            hwMax_ = std::max(hwMax_, policy.hwMax);
            policies_.push_back(std::move(policy));
        }
        closedir(dir);

        std::sort(policies_.begin(), policies_.end(),
                  [](const Policy& a, const Policy& b) { return a.id < b.id; });
        if (policies_.empty()) {
            logger_->Error("没有可用的 cpufreq policy");
            return false;
        }

        for (const Policy& policy : policies_)
            logger_->Info("policy%d cpus=%s governor=%s 硬件 %d~%d kHz",
                          policy.id, Join(policy.cpus).c_str(),
                          policy.governor.c_str(), policy.hwMin, policy.hwMax);
        return true;
    }

    void setLockMin(const bool value) { lockMin_ = value; }

    // 接管
    void beginGame(const bool free = false) {
        loads_.clear();
        busy_.clear();
        total_.clear();
        lastLoadNs_ = 0;
        lastActualNs_ = 0;
        for (Policy& policy : policies_) {
            WriteMin(policy, policy.hwMin);
            const int cur =
                std::clamp(RealCur(policy), policy.hwMin, policy.hwMax);
            WriteMax(policy, free ? policy.hwMax : Resolve(policy, cur, 0));
            policy.target = cur;
            policy.realCur = cur;
            policy.load = -1;
        }
    }

    void restore() {
        for (Policy& policy : policies_) {
            WriteMin(policy, policy.hwMin);
            WriteMax(policy, policy.hwMax);
            policy.target = std::clamp(RealCur(policy), policy.hwMin, policy.hwMax);
            policy.realCur = policy.target;
            policy.load = -1;
            policy.lastMin = policy.hwMin;
            policy.lastMax = policy.hwMax;
        }
    }

    // 调度
    void schedule(const double control, const bool janked, const double util,
                  const double utilTarget, const std::set<int>& cores) {
        if (control > 0) refreshLoads();

        const uint64_t now = Utils::NowNs();
        if (lastActualNs_ == 0 || now - lastActualNs_ >= kActualNs) {
            lastActualNs_ = now;
            for (Policy& policy : policies_)
                policy.realCur = ReadInt(
                    policy.dir + "/scaling_cur_freq", policy.realCur);
        }

        int fasTarget = 0;
        int realMax = 0;
        for (Policy& policy : policies_) {
            fasTarget = std::max(fasTarget, policy.target);
            realMax = std::max(realMax, policy.realCur);
        }

        int goal = std::clamp(fasTarget + static_cast<int>(std::lround(control)),
                              0, hwMax_);
        if (!janked && util > 0 && utilTarget > 0)
            goal = std::min(
                goal,
                std::clamp(static_cast<int>(realMax * util / utilTarget), 0,
                           hwMax_));

        for (Policy& policy : policies_) {
            int frequency = goal;
            if (control > 0 && policy.load >= 0 && goal > policy.target) {
                if (policy.load < 8.0)
                    frequency = policy.target;
                else if (policy.load < 25.0)
                    frequency = policy.target +
                                static_cast<int>((goal - policy.target) *
                                                 (policy.load - 8.0) / 17.0);
            }

            const int resolved = Resolve(policy, frequency, policy.target);
            Write(policy, resolved,
                  lockMin_ && !cores.empty() && Covers(policy.cpus, cores));
            policy.target = resolved;
        }
    }

    int hwMax() const { return hwMax_; }

    int target() const {
        int value = 0;
        for (const Policy& policy : policies_)
            value = std::max(value, policy.target);
        return value;
    }

    std::string describe() const {
        std::string text;
        for (const Policy& policy : policies_) {
            if (!text.empty()) text += " | ";
            text += "p" + std::to_string(policy.id) + " " +
                    std::to_string(policy.target) + "/" +
                    std::to_string(policy.realCur) + "kHz";
            if (policy.load >= 0)
                text += " " + std::to_string(static_cast<int>(policy.load + 0.5)) +
                        "%";
        }
        return text;
    }

private:
    static constexpr uint64_t kLoadWindowNs = 300000000ULL;
    static constexpr uint64_t kActualNs = 500000000ULL;

    // 写频
    static void Write(Policy& policy, const int frequency,
                      const bool critical) {
        if (!critical) {
            WriteMin(policy, policy.hwMin);
            WriteMax(policy, frequency);
            return;
        }
        if (frequency >= policy.target) {
            WriteMax(policy, frequency);
            WriteMin(policy, frequency);
        } else {
            WriteMin(policy, frequency);
            WriteMax(policy, frequency);
        }
    }

    // 贴档
    static int Resolve(const Policy& policy, const int target,
                       const int current) {
        const std::vector<int>& freqs = policy.freqs;
        if (freqs.empty()) return policy.hwMax;

        if (target < current) {
            const auto upper =
                std::upper_bound(freqs.begin(), freqs.end(), target);
            return upper == freqs.begin() ? freqs.front() : *std::prev(upper);
        }
        const auto lower =
            std::lower_bound(freqs.begin(), freqs.end(), target);
        return lower == freqs.end() ? freqs.back() : *lower;
    }

    static bool Covers(const std::vector<int>& cpus, const std::set<int>& cores) {
        for (const int cpu : cpus)
            if (cores.count(cpu)) return true;
        return false;
    }

    static void ReadFreqs(const std::string& path, std::vector<int>& freqs) {
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) return;
        int value = 0;
        while (fscanf(file, "%d", &value) == 1)
            if (value > 0) freqs.push_back(value);
        fclose(file);
        std::sort(freqs.begin(), freqs.end());
        freqs.erase(std::unique(freqs.begin(), freqs.end()), freqs.end());
    }

    static bool ReadCpus(const std::string& path, std::vector<int>& cpus) {
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) return false;
        int value = 0;
        while (fscanf(file, "%d", &value) == 1) cpus.push_back(value);
        fclose(file);
        return !cpus.empty();
    }

    static std::string Join(const std::vector<int>& values) {
        std::string text;
        for (const int value : values) {
            if (!text.empty()) text += ",";
            text += std::to_string(value);
        }
        return text;
    }

    static std::string ReadText(const std::string& path) {
        char buffer[64] = {};
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) return {};
        const size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        if (length == 0) return {};
        buffer[length] = '\0';
        size_t text = length;
        while (text > 0 && (buffer[text - 1] == '\n' || buffer[text - 1] == '\r' ||
                            buffer[text - 1] == ' '))
            --text;
        buffer[text] = '\0';
        return buffer;
    }

    static int ReadInt(const std::string& path, const int fallback) {
        int value = fallback;
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) return fallback;
        if (fscanf(file, "%d", &value) != 1) value = fallback;
        fclose(file);
        return value;
    }

    static int RealCur(const Policy& policy) {
        const int cur = ReadInt(policy.dir + "/scaling_cur_freq", 0);
        return cur > 0 ? cur : policy.hwMax;
    }

    static void WriteMin(Policy& policy, const int value) {
        if (policy.lastMin == value) return;
        const std::string path = policy.dir + "/scaling_min_freq";
        if (Utils::FileWrite(path.c_str(), value)) policy.lastMin = value;
    }

    static void WriteMax(Policy& policy, const int value) {
        if (policy.lastMax == value) return;
        const std::string path = policy.dir + "/scaling_max_freq";
        if (Utils::FileWrite(path.c_str(), value)) policy.lastMax = value;
    }

    // 分簇负载
    void refreshLoads() {
        const uint64_t now = Utils::NowNs();
        if (lastLoadNs_ != 0 && now - lastLoadNs_ < kLoadWindowNs) return;
        lastLoadNs_ = now;

        FILE* file = fopen("/proc/stat", "rb");
        if (!file) return;

        std::vector<double> loads;
        std::vector<uint64_t> busy;
        std::vector<uint64_t> total;
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), file)) {
            if (strncmp(buffer, "cpu", 3) != 0) break;
            if (!isdigit(static_cast<unsigned char>(buffer[3]))) continue;

            int id = -1;
            unsigned long long user = 0, nice = 0, system = 0, idle = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (sscanf(buffer, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                       &id, &user, &nice, &system, &idle, &iowait, &irq,
                       &softirq, &steal) != 9 || id < 0)
                continue;

            const uint64_t busyTicks =
                user + nice + system + irq + softirq + steal;
            const uint64_t totalTicks = busyTicks + idle + iowait;
            if (static_cast<size_t>(id) >= loads.size()) {
                loads.resize(id + 1, -1.0);
                busy.resize(id + 1, 0);
                total.resize(id + 1, 0);
            }

            if (busy_.size() > static_cast<size_t>(id) &&
                total_[id] > 0 && totalTicks > total_[id])
                loads[id] = static_cast<double>(busyTicks - busy_[id]) /
                            static_cast<double>(totalTicks - total_[id]) * 100.0;
            busy[id] = busyTicks;
            total[id] = totalTicks;
        }
        fclose(file);

        loads_.swap(loads);
        busy_.swap(busy);
        total_.swap(total);
        for (Policy& policy : policies_) policy.load = PolicyLoad(policy);
    }

    double PolicyLoad(const Policy& policy) const {
        double sum = 0;
        int count = 0;
        for (const int cpu : policy.cpus) {
            if (cpu < 0 || static_cast<size_t>(cpu) >= loads_.size() ||
                loads_[cpu] < 0)
                continue;
            sum += loads_[cpu];
            ++count;
        }
        return count > 0 ? sum / count : -1;
    }

    Logger* logger_ = nullptr;
    std::vector<Policy> policies_;
    std::vector<double> loads_;
    std::vector<uint64_t> busy_;
    std::vector<uint64_t> total_;
    int hwMax_ = 0;
    bool lockMin_ = false;
    uint64_t lastLoadNs_ = 0;
    uint64_t lastActualNs_ = 0;
};
