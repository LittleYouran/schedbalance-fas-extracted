#pragma once

// cgroup 迁移

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace CgroupEscape {

enum Result : int {
    Failed = 0,
    AlreadyThere = 1,
    Moved = 2,
};

inline Result Escape() {
    char buffer[256] = {};
    FILE* self = fopen("/proc/self/cgroup", "r");
    if (!self) return Failed;
    const size_t length = fread(buffer, 1, sizeof(buffer) - 1, self);
    fclose(self);
    if (length == 0) return Failed;
    if (strstr(buffer, "0::/system\n") != nullptr) return AlreadyThere;

    FILE* target = fopen("/sys/fs/cgroup/system/cgroup.procs", "w");
    if (!target) return Failed;
    const bool written = fprintf(target, "%d\n", getpid()) > 0;
    fclose(target);
    return written ? Moved : Failed;
}

}
