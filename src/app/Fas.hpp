#pragma once

// 入口分发

#include "Scheduler.hpp"
#include "Supervisor.hpp"

#include <cstdio>
#include <memory>
#include <string>

class Fas {
public:
    Fas(int argc, char** argv) : argv0_(argc > 0 ? argv[0] : nullptr) {
        bool valid = true;
        for (int i = 1; i < argc && valid; ++i) {
            const std::string argument = argv[i];
            if (argument == "--check") {
                checkOnly_ = true;
            } else if (argument == "--no-supervise") {
                supervise_ = false;
            } else if (argument == "--config" && i + 1 < argc) {
                configPath_ = argv[++i];
            } else {
                fprintf(stderr, "用法: %s [--config 路径] [--check] [--no-supervise]\n",
                        argv0_ ? argv0_ : "fas");
                valid = false;
            }
        }
        if (!valid) {
            usageError_ = true;
            return;
        }

        if (checkOnly_ || !supervise_)
            scheduler_ = std::make_unique<Scheduler>(configPath_, checkOnly_);
        else
            supervisor_ = std::make_unique<Supervisor>(configPath_, argv0_);
    }

    int wait() {
        if (usageError_) return 1;
        if (scheduler_) return scheduler_->run();
        if (supervisor_) supervisor_->run();
        return 0;
    }

private:
    static constexpr const char* kDefaultConfig = "/sdcard/Android/fas-youran/fas.conf";

    std::string configPath_ = kDefaultConfig;
    char* argv0_ = nullptr;
    bool checkOnly_ = false;
    bool supervise_ = true;
    bool usageError_ = false;

    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<Supervisor> supervisor_;
};
