#pragma once

// 帧感知调度核心

#include "../conf/Config.hpp"
#include "../cpu/CpuRuntime.hpp"
#include "../cpu/ProcessSampler.hpp"
#include "../frame/FrameSource.hpp"
#include "../util/Logger.hpp"
#include "../util/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

class FasRuntime {
public:
    FasRuntime(const Config& config, Logger& logger, CpuRuntime& cpu,
               ProcessSampler& sampler)
        : config_(config), logger_(logger), cpu_(cpu), sampler_(sampler) {}

    void setSource(FrameSource* source) { source_ = source; }

    void setMode(const std::string& mode) { mode_ = mode; }
    bool active() const { return pid_ > 0; }

    bool controlling(const int pid, const std::string& package) const {
        return pid_ == pid && game_.package == package;
    }

    void refreshCandidates(const Config& config) {
        if (pid_ <= 0) return;
        Game game;
        if (!config.Find(game_.package, game)) return;
        candidates_ = game.fps;
    }

    // 接管
    void begin(const int pid, const Game& game) {
        pid_ = pid;
        game_ = game;
        candidates_ = game.fps;

        buffers_.clear();
        frameTimes_.clear();
        target_.reset();
        usable_ = Utils::NowNs() + kWarmNs;
        lastCalcNs_ = lastFrameNs_ = lastLogNs_ = lastDetailNs_ = 0;
        lastApplyNs_ = 0;
        lastJankLogNs_ = 0;
        lastFrameMs_ = additionalMs_ = utilOffset_ = maxFrameMs_ = 0;
        jankCount_ = bigJankCount_ = 0;
        janked_ = disabled_ = released_ = false;
        disabled_ = true;
        lastFrameNs_ = 0;

        cpu_.beginGame(true);
        sampler_.setPid(pid);

        std::string rates;
        for (const int fps : candidates_)
            rates += (rates.empty() ? "" : "/") + std::to_string(fps);
        logger_.Info("接管 %s（pid %d），帧率候选 %s，情景 %s",
                     game.package.c_str(), pid, rates.c_str(), mode_.c_str());
    }

    void end() {
        if (pid_ <= 0) return;
        cpu_.restore();
        sampler_.setPid(-1);
        logger_.Info("退出 %s，频率已恢复", game_.package.c_str());
        buffers_.clear();
        frameTimes_.clear();
        target_.reset();
        released_ = false;
        lastApplyNs_ = 0;
        pid_ = -1;
    }

    void onFrame(const uint64_t tsNs, const double ms, const int flags) {
        if (pid_ <= 0 || !std::isfinite(ms) || ms <= 0) return;
        if (released_) recover(tsNs);

        lastFrameNs_ = tsNs;
        lastFrameMs_ = ms;
        additionalMs_ = 0;
        if (flags) ++jankCount_;
        if (flags == 2) ++bigJankCount_;
        maxFrameMs_ = std::max(maxFrameMs_, ms);

        buffers_.push_back(ms);
        frameTimes_.push_back(tsNs);
        prune(tsNs);
        control(tsNs);
    }

    void tick(const uint64_t now) {
        if (pid_ <= 0 || lastFrameNs_ == 0) return;

        prune(now);
        const uint64_t ageNs = now > lastFrameNs_ ? now - lastFrameNs_ : 0;
        if (ageNs > kReleaseNs) {
            if (!released_) {
                released_ = true;
                cpu_.restore();
                logger_.Warn("超过 3.5 秒没有帧事件，恢复硬件频率停控");
            }
            return;
        }

        additionalMs_ = std::min(static_cast<double>(ageNs) / 1e6, kMaxGapMs);
        control(now);
    }

private:
    void recover(const uint64_t tsNs) {
        released_ = false;
        jankCount_ = bigJankCount_ = jankStreak_ = 0;
        maxFrameMs_ = 0;
        lastDetailNs_ = lastCalcNs_ = lastApplyNs_ = 0;
        additionalMs_ = 0;
        usable_ = tsNs + kWarmNs;
        cpu_.beginGame();
        logger_.Info("帧事件恢复，重新接管");
    }

    void control(const uint64_t now) {
        if (additionalMs_ >= kQuietGapMs) return;
        refreshTarget(now);
        if (!target_) return;

        if (disabled_) {
            if (now < usable_) return;
            disabled_ = false;
            cpu_.beginGame();
        }
        if (buffers_.size() < kApplyFrames || now < usable_) return;
        if (lastApplyNs_ != 0 && now >= lastApplyNs_ &&
            now - lastApplyNs_ < kApplyNs)
            return;
        lastApplyNs_ = now;
        apply(now);
    }

    // 定档
    void refreshTarget(const uint64_t now) {
        if (lastCalcNs_ != 0 && now - lastCalcNs_ < kCalcIntervalNs) return;
        lastCalcNs_ = now;
        if (now < usable_ || frameTimes_.size() < kTargetFrames) return;

        const double fps = currentFps();
        std::optional<int> next;
        if (!candidates_.empty() &&
            fps >= static_cast<double>(candidates_.front() - 10)) {
            next = candidates_.back();
            for (const int candidate : candidates_) {
                if (fps <= static_cast<double>(candidate) + 3.0) {
                    next = candidate;
                    break;
                }
            }
        }

        if (next == target_) return;

        if (next) {
            target_ = next;
            logger_.Info("目标帧率 -> %d（实测 %.1f）", *target_, fps);
        } else {
            target_.reset();
            disabled_ = true;
            cpu_.restore();
            logger_.Info("实测 %.1f 低于最低档，释放频率控制", fps);
        }
        buffers_.clear();
        frameTimes_.clear();
        additionalMs_ = 0;
        jankStreak_ = 0;
        lastApplyNs_ = 0;
        usable_ = now + kCooldownNs;
    }

    // 积分控制
    void apply(const uint64_t now) {
        const double target = static_cast<double>(*target_);
        const double margin = config_.Margin(mode_) * target / 60.0;

        double util = util_;
        std::set<int> cores = cores_;
        if (sampler_.sample(util, cores)) {
            util_ = util;
            cores_ = cores;
            updateUtilOffset(now);
        }

        const double adjusted =
            std::clamp(target - margin + utilOffset_, 1.0, target);

        const double fps = currentFps();
        const double tolerance = std::max(2.0, margin + 1.0);
        const bool janked = fps < target - tolerance;

        const double frameMs =
            std::max(janked ? lastFrameMs_ : recentAverage(kBurstFrames),
                     additionalMs_);
        const double normalizedMs = frameMs * adjusted;
        const double limit = static_cast<double>(cpu_.hwMax());
        double control = std::clamp((normalizedMs * 1e6 - 1e9) * config_.kp,
                                    -limit, limit);
        if (janked) {
            ++jankStreak_;
            control = std::min(
                control, limit * std::min(
                             0.25 + 0.25 * static_cast<double>(jankStreak_ - 1),
                             1.0));
        } else {
            jankStreak_ = 0;
            if (control > 0) control = std::min(control, limit * 0.10);
        }

        cpu_.schedule(control, janked, util_, config_.utilTarget, cores_);
        lastControl_ = control;

        if (janked != janked_) {
            janked_ = janked;
            if (lastJankLogNs_ == 0 || now - lastJankLogNs_ >= kLogThrottleNs) {
                lastJankLogNs_ = now;
                logger_.Info(janked ? "检测到掉帧，临时升频" : "帧率恢复");
            }
        }

        observe(now);
    }

    void observe(const uint64_t now) {
        if (lastDetailNs_ == 0 || now - lastDetailNs_ >= kDetailNs) {
            lastDetailNs_ = now;
            unsigned long long lagMs = 0;
            int depth = 0;
            if (source_) {
                lagMs = pid_ > 0 && lastFrameNs_ != 0 &&
                                now > lastFrameNs_
                            ? (now - lastFrameNs_) / 1000000ULL
                            : 0;
                depth = source_->depth();
            }
            logger_.Info("线程 %s | CPU %s | 帧龄 %llu 队 %d",
                         sampler_.describe().c_str(), cpu_.describe().c_str(),
                         lagMs, depth);
        }

        if (lastLogNs_ == 0 || now - lastLogNs_ < kLogNs) return;
        lastLogNs_ = now;
        logger_.Info("目标 %d | 实测 %.1f | 控制 %+.0f kHz | util %.2f | "
                     "util 偏移 %.1f | fas %d kHz | 最大帧 %.1fms | "
                     "jank %d big %d",
                     *target_, currentFps(), lastControl_, util_, utilOffset_,
                     cpu_.target(), maxFrameMs_, jankCount_, bigJankCount_);
        jankCount_ = bigJankCount_ = 0;
        maxFrameMs_ = 0;
    }

    // util 棘轮
    void updateUtilOffset(const uint64_t now) {
        if (lastUtilNs_ != 0 && now - lastUtilNs_ < kUtilNs) return;
        lastUtilNs_ = now;
        if (util_ <= 0.1) utilOffset_ = 0;
        else if (util_ <= 0.55) utilOffset_ -= 0.1;
        else if (util_ >= 0.65) utilOffset_ += 0.1;
        utilOffset_ = std::clamp(utilOffset_, -3.0, 0.0);
    }

    double currentFps() const {
        if (buffers_.empty()) return additionalMs_ > 0 ? 1000.0 / additionalMs_ : 0;
        double total = additionalMs_;
        for (const double ms : buffers_) total += ms;
        return 1000.0 / (total / static_cast<double>(buffers_.size()));
    }

    double recentAverage(const int count) const {
        if (count >= static_cast<int>(buffers_.size())) return lastFrameMs_;
        double total = 0;
        for (auto it = buffers_.end() - count; it != buffers_.end(); ++it)
            total += *it;
        return total / count;
    }

    // 3s 窗口
    void prune(const uint64_t now) {
        while (!frameTimes_.empty() &&
               frameTimes_.front() + kPruneNs < now) {
            frameTimes_.pop_front();
            buffers_.pop_front();
        }
    }

    static constexpr uint64_t kWarmNs = 1000000000ULL;
    static constexpr uint64_t kCooldownNs = 600000000ULL;
    static constexpr uint64_t kReleaseNs = 3500000000ULL;
    static constexpr uint64_t kPruneNs = 3000000000ULL;
    static constexpr uint64_t kCalcIntervalNs = 100000000ULL;
    static constexpr uint64_t kApplyNs = 50000000ULL;
    static constexpr uint64_t kUtilNs = 1000000000ULL;
    static constexpr uint64_t kDetailNs = 1000000000ULL;
    static constexpr uint64_t kLogNs = 5000000000ULL;
    static constexpr uint64_t kLogThrottleNs = 1000000000ULL;
    static constexpr int kTargetFrames = 20;
    static constexpr int kApplyFrames = 24;
    static constexpr int kBurstFrames = 5;
    static constexpr double kMaxGapMs = 100.0;
    static constexpr double kQuietGapMs = 100.0;

    const Config& config_;
    Logger& logger_;
    CpuRuntime& cpu_;
    ProcessSampler& sampler_;
    FrameSource* source_ = nullptr;

    int pid_ = -1;
    Game game_;
    std::vector<int> candidates_;
    std::string mode_ = "balance";
    std::deque<double> buffers_;
    std::deque<uint64_t> frameTimes_;
    std::optional<int> target_;

    uint64_t usable_ = 0;
    uint64_t lastCalcNs_ = 0;
    uint64_t lastApplyNs_ = 0;
    uint64_t lastFrameNs_ = 0;
    double lastFrameMs_ = 0;
    double additionalMs_ = 0;
    double utilOffset_ = 0;
    double util_ = 0;
    std::set<int> cores_;
    uint64_t lastUtilNs_ = 0;
    uint64_t lastLogNs_ = 0;
    uint64_t lastDetailNs_ = 0;
    uint64_t lastJankLogNs_ = 0;
    double lastControl_ = 0;
    bool janked_ = false;
    bool disabled_ = true;
    bool released_ = false;
    int jankCount_ = 0;
    int bigJankCount_ = 0;
    int jankStreak_ = 0;
    double maxFrameMs_ = 0;
};
