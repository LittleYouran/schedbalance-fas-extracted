#pragma once

// 常用工具

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace Utils {

inline uint64_t NowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

inline uint64_t FileMtimeNs(const char* path) {
    struct stat info {};
    if (stat(path, &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_mtim.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(info.st_mtim.tv_nsec);
}

inline bool FileWrite(const char* path, const char* content, const size_t length) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        chmod(path, 0644);
        fd = open(path, O_WRONLY | O_CLOEXEC);
    }
    if (fd < 0) return false;

    const ssize_t written = write(fd, content, length);
    close(fd);
    return written == static_cast<ssize_t>(length);
}

inline bool FileWrite(const char* path, const int value) {
    char text[16];
    const int length = snprintf(text, sizeof(text), "%d", value);
    return FileWrite(path, text, static_cast<size_t>(length));
}

inline int ReadInt(const char* path, const int fallback) {
    char buffer[32] = {};
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fallback;

    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) return fallback;

    char* end = nullptr;
    const long value = strtol(buffer, &end, 10);
    if (end == buffer) return fallback;
    return static_cast<int>(value);
}

inline std::string ReadText(const char* path) {
    char buffer[64] = {};
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};

    const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) return {};

    ssize_t text = length;
    while (text > 0 && (buffer[text - 1] == '\n' || buffer[text - 1] == '\r' ||
                        buffer[text - 1] == ' '))
        --text;
    buffer[text] = '\0';
    return buffer;
}

inline FILE* OpenText(const char* path, const char* mode) {
    return fopen(path, mode);
}

inline bool FilesDiffer(const std::string& a, const std::string& b) {
    FILE* fa = fopen(a.c_str(), "rb");
    if (!fa) return true;
    FILE* fb = fopen(b.c_str(), "rb");
    if (!fb) {
        fclose(fa);
        return true;
    }

    fseek(fa, 0, SEEK_END);
    const long la = ftell(fa);
    fseek(fa, 0, SEEK_SET);
    fseek(fb, 0, SEEK_END);
    const long lb = ftell(fb);
    fseek(fb, 0, SEEK_SET);

    bool differ = la != lb;
    for (long offset = 0; !differ && offset < la; offset += 65536) {
        char bufA[65536];
        char bufB[65536];
        const size_t chunk = static_cast<size_t>(
            offset + 65536 <= la ? 65536 : la - offset);
        if (fread(bufA, 1, chunk, fa) != chunk ||
            fread(bufB, 1, chunk, fb) != chunk) {
            differ = true;
            break;
        }
        if (memcmp(bufA, bufB, chunk) != 0) differ = true;
    }

    fclose(fa);
    fclose(fb);
    return differ;
}

inline std::vector<pid_t> FindProcesses(const char* name) {
    std::vector<pid_t> found;
    DIR* proc = opendir("/proc");
    if (!proc) return found;

    dirent* entry = nullptr;
    while ((entry = readdir(proc))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE* file = fopen(path, "rb");
        if (!file) continue;

        char comm[32] = {};
        const bool ok = fgets(comm, sizeof(comm), file) != nullptr;
        fclose(file);
        if (!ok) continue;

        comm[strcspn(comm, "\r\n")] = '\0';
        if (strcmp(comm, name) == 0) found.push_back(atoi(entry->d_name));
    }
    closedir(proc);
    return found;
}

inline pid_t FindProcess(const char* name) {
    const std::vector<pid_t> all = FindProcesses(name);
    return all.empty() ? 0 : all.front();
}

inline bool CopyFile(const std::string& from, const std::string& to) {
    FILE* source = fopen(from.c_str(), "rb");
    if (!source) return false;

    FILE* target = fopen(to.c_str(), "wb");
    if (!target) {
        fclose(source);
        return false;
    }

    char buffer[16384];
    size_t length = 0;
    while ((length = fread(buffer, 1, sizeof(buffer), source)) > 0)
        fwrite(buffer, 1, length, target);

    const bool ok = !ferror(target) && !ferror(source);
    fclose(target);
    fclose(source);
    return ok;
}

inline bool HasFlag(const std::string& value, const char* flag) {
    return value.find(flag) != std::string::npos;
}

inline std::string SelfDirectory() {
    char path[PATH_MAX] = {};
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return ".";
    path[length] = '\0';
    const char* slash = strrchr(path, '/');
    return slash ? std::string(path, slash - path) : std::string(".");
}

}
