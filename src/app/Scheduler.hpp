#pragma once

// 业务主体

#include "CgroupEscape.hpp"
#include "CrashHandler.hpp"
#include "Foreground.hpp"
#include "KernelEvents.hpp"
#include "ModeWatcher.hpp"
#include "../conf/Config.hpp"
#include "../cpu/CpuRuntime.hpp"
#include "../cpu/ProcessSampler.hpp"
#include "../frame/FrameSource.hpp"
#include "../inject/HookInjector.hpp"
#include "../sched/FasRuntime.hpp"
#include "../util/Logger.hpp"
#include "../util/Utils.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

class Scheduler {
public:
    Scheduler(const std::string& configPath, const bool checkOnly)
        : configPath_(configPath) {
        dataDirectoryReady();
        bringOutDefaultConfig();

        try {
            config_ = Config::Load(configPath_);
        } catch (const std::exception& error) {
            fprintf(stderr, "配置错误: %s\n", error.what());
            exitCode_ = 1;
            return;
        }

        if (checkOnly) {
            printSummary();
            return;
        }

        const bool replaced = stopOthers();

        logger_.setPath(config_.log);
        CrashHandler::install(config_.log);
        if (replaced) logger_.Info("旧实例已停止，本次重启完成");
        logger_.Info("fas v0.3.21 启动（Hook 帧源 • 异步 • 内核事件唤醒），配置 %s",
                     configPath_.c_str());
        if (!config_.enabled) {
            logger_.Error("配置 enabled=false，fas 不接管");
            return;
        }
        if (Utils::FindProcess("schedbalance") > 0)
            logger_.Warn("检测到 schedbalance 在跑：会与 fas 同时写频率上限，建议先停它");

        if (CgroupEscape::Escape() == CgroupEscape::Moved)
            logger_.Info("cgroup 已迁到 /system，避开临时子组清理");

        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);
        signal(SIGHUP, onSignal);
        signal(SIGPIPE, SIG_IGN);

        if (access(config_.modeFile.c_str(), F_OK) != 0) {
            if (FILE* mode = fopen(config_.modeFile.c_str(), "w")) {
                fputs("balance\n", mode);
                fclose(mode);
            }
        }

        cpu_ = std::make_unique<CpuRuntime>();
        if (!cpu_->init(logger_)) {
            exitCode_ = 1;
            return;
        }
        cpu_->setLockMin(config_.lockMin);
        cpu_->restore();

        sampler_ = std::make_unique<ProcessSampler>();
        sampler_->setTrackCores(config_.lockMin);
        fas_ = std::make_unique<FasRuntime>(config_, logger_, *cpu_, *sampler_);

        modes_ = std::make_unique<ModeWatcher>(logger_);
        modes_->attach(config_.modeFile);
        fas_->setMode(ModeWatcher::readMode(config_.modeFile));

        injector_ = std::make_unique<HookInjector>(logger_, Utils::SelfDirectory());
        injector_->start();

        frames_ = std::make_unique<FrameSource>(logger_);
        frames_->start();
        fas_->setSource(frames_.get());

        events_ = std::make_unique<KernelEvents>();
        if (events_->start())
            logger_.Info("内核事件源已启用（cgroup_attach_task，%d 个 CPU），空闲走事件唤醒",
                         events_->cpuCount());
        else
            logger_.Warn("内核事件源不可用，空闲退回 2s 轮询");

        watcher_ = std::make_unique<ForegroundWatcher>(config_);

        ready_ = true;
    }

    // 主循环
    int run() {
        if (!ready_) return exitCode_;

        std::string activeMode = ModeWatcher::readMode(config_.modeFile);
        uint64_t nextModeNs = 0;
        int emptyStreak = 0;
        uint64_t lastBeatNs = 0;
        uint64_t nextForeground = 0;
        uint64_t nextConfigNs = 0;
        uint64_t beginNs = 0;
        uint64_t configStamp = Utils::FileMtimeNs(configPath_.c_str());
        bool frameSeen = false;
        bool frameWarned = false;

        while (!stop_) {
            uint64_t now = Utils::NowNs();

            if (lastBeatNs != 0 && now - lastBeatNs > kBeatStallNs)
                logger_.Warn("主循环停摆约 %llu 秒，疑似被冻结器或系统介入暂停",
                             static_cast<unsigned long long>((now - lastBeatNs) /
                                                             1000000000ULL));
            lastBeatNs = now;

            FrameSource::Frame frame;
            while (frames_->pop(frame)) {
                frameSeen = true;
                fas_->onFrame(frame.tsNs, frame.ms, frame.flags);
            }

            if (!fas_->active() && events_->available()) {
                uint64_t deadline = nextConfigNs;
                if (nextForeground < deadline) deadline = nextForeground;
                uint64_t waitNs = deadline > now ? deadline - now : 0;
                if (waitNs > kIdleWaitMaxNs) waitNs = kIdleWaitMaxNs;
                int timeoutMs = static_cast<int>(waitNs / 1000000ULL);
                if (timeoutMs < 1) timeoutMs = 1;
                if (events_->wait(timeoutMs)) nextForeground = 0;
                now = Utils::NowNs();
            }

            if (now >= nextForeground) {
                nextForeground = now +
                    (fas_->active() ? kActiveForegroundNs
                                    : (events_->available() ? kEventForegroundNs
                                                            : kIdleForegroundNs));
                const Foreground fg = watcher_->find();
                if (fg.empty()) {
                    if (++emptyStreak >= 2) {
                        emptyStreak = 0;
                        if (fas_->active()) fas_->end();
                    }
                } else {
                    emptyStreak = 0;
                    Game game;
                    if (config_.enabled && config_.Find(fg.package, game) &&
                        !fas_->controlling(fg.pid, fg.package)) {
                        fas_->end();
                        fas_->begin(fg.pid, game);
                        beginNs = now;
                        frameSeen = false;
                        frameWarned = false;
                    }
                }
            }

            frames_->setPaused(!fas_->active());

            if (now >= nextConfigNs) {
                nextConfigNs = now + 1000000000ULL;
                const uint64_t stamp = Utils::FileMtimeNs(configPath_.c_str());
                if (stamp != 0 && stamp != configStamp) {
                    configStamp = stamp;
                    Config next;
                    std::string error;
                    if (Config::TryLoad(configPath_, next, error)) {
                        config_ = next;
                        watcher_->resetCache();
                        cpu_->setLockMin(config_.lockMin);
                        sampler_->setTrackCores(config_.lockMin);
                        fas_->refreshCandidates(config_);
                        if (!config_.enabled) fas_->end();
                        logger_.Info("配置热重载生效");
                    } else {
                        logger_.Warn("配置热重载失败，沿用旧配置：%s", error.c_str());
                    }
                }
            }

            fas_->tick(now);

            if (!frameSeen && !frameWarned && fas_->active() &&
                beginNs != 0 && now - beginNs > kWarnGapNs) {
                frameWarned = true;
                logger_.Error("已接管但 2 秒内没有帧事件，"
                              "检查 libfas_hook.so 是否注入、点位是否匹配");
            }

            if (modes_->changed() || now >= nextModeNs) {
                nextModeNs = now + 500000000ULL;
                const std::string next = ModeWatcher::readMode(config_.modeFile);
                if (next != activeMode) {
                    activeMode = next;
                    fas_->setMode(next);
                    logger_.Info("情景模式切换：%s", next.c_str());
                }
            }
            injector_->check(now);

            if (fas_->active())
                usleep(kActiveSleepUs);
            else if (!events_->available())
                usleep(kIdleSleepUs);
        }

        fas_->end();
        frames_->stop();
        logger_.Info("fas 已退出");
        return 0;
    }

private:
    static constexpr const char* kDataDirectory = "/sdcard/Android/fas-youran";
    static constexpr const char* kDefaultConfig = "/sdcard/Android/fas-youran/fas.conf";

    static constexpr useconds_t kActiveSleepUs = 100000;
    static constexpr useconds_t kIdleSleepUs = 2000000;
    static constexpr uint64_t kActiveForegroundNs = 500000000ULL;
    static constexpr uint64_t kIdleForegroundNs = 2000000000ULL;
    static constexpr uint64_t kEventForegroundNs = 4000000000ULL;
    static constexpr uint64_t kIdleWaitMaxNs = 4000000000ULL;
    static constexpr uint64_t kWarnGapNs = 2000000000ULL;
    static constexpr uint64_t kBeatStallNs = 5000000000ULL;

    inline static volatile sig_atomic_t stop_ = 0;
    static void onSignal(int) { stop_ = 1; }

    // 启动准备
    static void dataDirectoryReady() {
        for (int step = 0; step < 30; ++step) {
            if (mkdir(kDataDirectory, 0775) == 0 || errno == EEXIST) return;
            usleep(500000);
        }
    }

    static void bringOutDefaultConfig() {
        if (access(kDefaultConfig, F_OK) != 0)
            Utils::CopyFile(Utils::SelfDirectory() + "/fas.conf", kDefaultConfig);
    }

    static bool stopOthers() {
        const std::vector<pid_t> others = Utils::FindProcesses("fas");
        bool replaced = false;
        for (const pid_t pid : others) {
            if (pid == getpid()) continue;
            kill(pid, SIGTERM);
            replaced = true;
        }
        if (!replaced) return false;

        for (int step = 0; step < 10; ++step) {
            bool alive = false;
            for (const pid_t pid : others)
                if (pid != getpid() && kill(pid, 0) == 0) alive = true;
            if (!alive) return true;
            usleep(100000);
        }
        for (const pid_t pid : others)
            if (pid != getpid()) kill(pid, SIGKILL);
        return true;
    }

    void printSummary() const {
        printf("enabled=%s games=%zu kp=%g util_target=%g lock_min=%s\n",
               config_.enabled ? "true" : "false", config_.games.size(), config_.kp,
               config_.utilTarget, config_.lockMin ? "true" : "false");
        for (const auto& entry : config_.games) {
            printf("%s:", entry.first.c_str());
            for (const int fps : entry.second.fps) printf(" %d", fps);
            printf("\n");
        }
        for (const auto& margin : config_.margins)
            printf("[%s] margin_fps = %g\n", margin.first.c_str(), margin.second);
    }

    std::string configPath_;
    Config config_;
    Logger logger_;

    std::unique_ptr<CpuRuntime> cpu_;
    std::unique_ptr<ProcessSampler> sampler_;
    std::unique_ptr<FasRuntime> fas_;
    std::unique_ptr<ModeWatcher> modes_;
    std::unique_ptr<HookInjector> injector_;
    std::unique_ptr<FrameSource> frames_;
    std::unique_ptr<KernelEvents> events_;
    std::unique_ptr<ForegroundWatcher> watcher_;

    bool ready_ = false;
    int exitCode_ = 0;
};
