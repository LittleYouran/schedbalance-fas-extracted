#pragma once

// 配置解析

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct Game {
    std::string package;
    std::vector<int> fps;
};

struct Config {
    bool enabled = false;
    std::string log = "/sdcard/Android/fas-youran/fas.log";
    std::string modeFile = "/sdcard/Android/fas-youran/switch.txt";
    double kp = 0.0003;
    double utilTarget = 0.5;
    bool lockMin = false;
    std::map<std::string, Game> games;
    std::map<std::string, double> margins;

    static Config Load(const std::string& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("无法打开配置: " + path);

        Config config;
        std::string section;
        std::string line;
        while (std::getline(input, line)) {
            const std::string text = Trim(line);
            if (text.empty() || text.front() == '#' || text.front() == ';') continue;

            if (text.front() == '[') {
                if (text.back() != ']')
                    throw std::runtime_error("配置节格式错误: " + text);
                section = Trim(text.substr(1, text.size() - 2));
                continue;
            }

            const size_t eq = text.find('=');
            if (eq == std::string::npos)
                throw std::runtime_error("配置项缺少 '=': " + text);
            const std::string key = Trim(text.substr(0, eq));
            const std::string value = Trim(text.substr(eq + 1));

            if (section == "config")
                ParseConfig(config, key, value);
            else if (section == "games")
                ParseGame(config, key, value);
            else if (!section.empty())
                ParseMargin(config, section, key, value);
            else
                throw std::runtime_error("配置项不在任何节内: " + text);
        }

        Validate(config);
        return config;
    }

    static bool TryLoad(const std::string& path, Config& out, std::string& error) {
        try {
            out = Load(path);
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    bool Find(const std::string& package, Game& out) const {
        const auto it = games.find(package);
        if (it == games.end()) return false;
        out = it->second;
        return true;
    }

    double Margin(const std::string& mode) const {
        const auto it = margins.find(mode);
        return it == margins.end() ? 1.0 : it->second;
    }

private:
    static void ParseConfig(Config& config, const std::string& key,
                            const std::string& value) {
        if (key == "enabled") config.enabled = Boolean(value);
        else if (key == "log") config.log = value;
        else if (key == "mode_file") config.modeFile = value;
        else if (key == "kp") config.kp = Number(value);
        else if (key == "util_target") config.utilTarget = Number(value);
        else if (key == "lock_min") config.lockMin = Boolean(value);
        else throw std::runtime_error("未知配置项: " + key);
    }

    static void ParseGame(Config& config, const std::string& key,
                          const std::string& value) {
        if (!ValidPackage(key)) throw std::runtime_error("包名无效: " + key);

        Game game;
        game.package = key;
        size_t begin = 0;
        while (begin < value.size()) {
            const size_t end = value.find_first_of(" \t", begin);
            const std::string token = value.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            const int fps = std::atoi(token.c_str());
            if (fps < 15 || fps > 240)
                throw std::runtime_error("帧率必须是 15~240 的整数: " + key);
            if (std::find(game.fps.begin(), game.fps.end(), fps) != game.fps.end())
                throw std::runtime_error("帧率重复: " + key);
            game.fps.push_back(fps);

            if (end == std::string::npos) break;
            begin = value.find_first_not_of(" \t", end);
            if (begin == std::string::npos) break;
        }
        if (game.fps.empty())
            throw std::runtime_error("游戏必须填写至少一个帧率: " + key);

        std::sort(game.fps.begin(), game.fps.end());
        config.games[key] = std::move(game);
    }

    static void ParseMargin(Config& config, const std::string& section,
                            const std::string& key, const std::string& value) {
        if (key != "margin_fps")
            throw std::runtime_error("未知模式参数: " + key);
        const double margin = Number(value);
        if (margin < 0 || margin > 10)
            throw std::runtime_error("margin_fps 超出范围: " + value);
        config.margins[section] = margin;
    }

    static void Validate(const Config& config) {
        if (config.games.empty()) throw std::runtime_error("配置缺少 [games] 游戏列表");
        if (config.kp <= 0 || config.kp > 0.01)
            throw std::runtime_error("kp 超出范围 (0, 0.01]");
        if (config.utilTarget <= 0.05 || config.utilTarget > 1)
            throw std::runtime_error("util_target 超出范围 (0.05, 1]");
    }

    static std::string Trim(const std::string& value) {
        const size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return {};
        const size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    static bool Boolean(const std::string& value) {
        if (value == "true") return true;
        if (value == "false") return false;
        throw std::runtime_error("布尔值只能填写 true 或 false: " + value);
    }

    static double Number(const std::string& value) {
        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(value.c_str(), &end);
        if (errno || end == value.c_str() || *end != '\0')
            throw std::runtime_error("无效数值: " + value);
        return parsed;
    }

    static bool ValidPackage(const std::string& name) {
        if (name.empty() || name.find('.') == std::string::npos) return false;
        return std::none_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0 || ch == '/' || ch == ':';
        });
    }
};
