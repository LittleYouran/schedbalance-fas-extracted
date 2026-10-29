#pragma once

// 事件源

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <linux/perf_event.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

class KernelEvents {
public:
    static constexpr int kMaxCpu = 16;
    static constexpr int kDataPages = 8;

    KernelEvents() = default;
    ~KernelEvents() { close(); }

    KernelEvents(const KernelEvents&) = delete;
    KernelEvents& operator=(const KernelEvents&) = delete;

    bool start() {
        close();
        const int id = tracepointId();
        if (id < 0) return false;

        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) return false;
        page_ = static_cast<size_t>(page);
        length_ = page_ * (1 + kDataPages);

        const long online = sysconf(_SC_NPROCESSORS_ONLN);
        if (online <= 0) return false;
        const int cpus = online < kMaxCpu ? static_cast<int>(online) : kMaxCpu;

        rings_.reserve(static_cast<size_t>(cpus));
        for (int cpu = 0; cpu < cpus; ++cpu) {
            if (!openRing(cpu, id)) {
                close();
                return false;
            }
        }
        return !rings_.empty();
    }

    bool available() const { return !rings_.empty(); }

    int cpuCount() const { return static_cast<int>(rings_.size()); }

    bool wait(const int timeoutMs) {
        if (rings_.empty()) return false;

        struct pollfd fds[kMaxCpu];
        const size_t count = rings_.size();
        for (size_t i = 0; i < count; ++i) {
            fds[i].fd = rings_[i].fd;
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }

        const int ready = poll(fds, static_cast<nfds_t>(count), timeoutMs);
        if (ready <= 0) return false;

        bool hit = false;
        for (size_t i = 0; i < count; ++i) {
            if (fds[i].revents & POLLIN) hit = drain(rings_[i]) || hit;
        }
        return hit;
    }

    void close() {
        for (auto& ring : rings_) {
            if (ring.base && ring.base != MAP_FAILED) munmap(ring.base, length_);
            if (ring.fd >= 0) ::close(ring.fd);
        }
        rings_.clear();
        length_ = 0;
    }

private:
    struct Ring {
        int fd = -1;
        void* base = nullptr;
        struct perf_event_mmap_page* meta = nullptr;
        uint64_t tail = 0;
        size_t dataOffset = 0;
        size_t dataSize = 0;
    };

    static int tracepointId() {
        static const char* paths[] = {
            "/sys/kernel/tracing/events/cgroup/cgroup_attach_task/id",
            "/sys/kernel/debug/tracing/events/cgroup/cgroup_attach_task/id",
        };
        for (const char* path : paths) {
            FILE* file = fopen(path, "r");
            if (!file) continue;
            int id = -1;
            const int read = fscanf(file, "%d", &id);
            fclose(file);
            if (read == 1 && id >= 0) return id;
        }
        return -1;
    }

    bool openRing(const int cpu, const int id) {
        struct perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_TRACEPOINT;
        attr.size = sizeof(attr);
        attr.config = static_cast<uint64_t>(id);
        attr.sample_period = 1;
        attr.sample_type = PERF_SAMPLE_RAW;
        attr.wakeup_events = 1;
        attr.disabled = 1;

        const int fd = static_cast<int>(syscall(
            __NR_perf_event_open, &attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC));
        if (fd < 0) return false;

        void* base = mmap(nullptr, length_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            ::close(fd);
            return false;
        }

        Ring ring;
        ring.fd = fd;
        ring.base = base;
        ring.meta = static_cast<struct perf_event_mmap_page*>(base);
        ring.tail = 0;
        if (ring.meta->version >= 1) {
            ring.dataOffset = static_cast<size_t>(ring.meta->data_offset);
            ring.dataSize = static_cast<size_t>(ring.meta->data_size);
        }
        rings_.push_back(ring);

        if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) return false;
        return true;
    }

    // 环形缓冲
    bool drain(Ring& ring) {
        const uint64_t head =
            __atomic_load_n(&ring.meta->data_head, __ATOMIC_ACQUIRE);
        if (ring.tail >= head) {
            __atomic_store_n(&ring.meta->data_tail, ring.tail, __ATOMIC_RELEASE);
            return false;
        }

        const size_t dataOffset = ring.dataOffset ? ring.dataOffset : page_;
        const size_t area = ring.dataSize ? ring.dataSize : (length_ - page_);
        if (area < sizeof(struct perf_event_header)) {
            ring.tail = head;
            __atomic_store_n(&ring.meta->data_tail, ring.tail, __ATOMIC_RELEASE);
            return false;
        }

        if (head - ring.tail > area) ring.tail = head;

        const uint8_t* base = static_cast<const uint8_t*>(ring.base) + dataOffset;
        bool hit = false;

        while (ring.tail < head) {
            const size_t offset = static_cast<size_t>(ring.tail % area);

            struct perf_event_header header {};
            const size_t room = area - offset;
            const size_t first =
                room < sizeof(header) ? room : sizeof(header);
            memcpy(&header, base + offset, first);
            if (first < sizeof(header))
                memcpy(reinterpret_cast<uint8_t*>(&header) + first, base,
                       sizeof(header) - first);

            if (header.size < sizeof(header) ||
                ring.tail + header.size > head)
                break;

            if (header.size > area) {
                ring.tail = head;
                break;
            }
            if (offset + header.size > area) {
                ring.tail += header.size;
                continue;
            }

            if (header.type == PERF_RECORD_SAMPLE &&
                header.size >= sizeof(header) + sizeof(uint32_t)) {
                const uint8_t* sample =
                    base + offset + sizeof(struct perf_event_header);
                uint32_t rawSize = 0;
                memcpy(&rawSize, sample, sizeof(rawSize));
                const uint32_t roomBytes =
                    header.size - sizeof(header) - sizeof(rawSize);
                if (rawSize > roomBytes) rawSize = roomBytes;
                if (isTopApp(sample + sizeof(rawSize), rawSize)) hit = true;
            }
            ring.tail += header.size;
        }

        __atomic_store_n(&ring.meta->data_tail, ring.tail, __ATOMIC_RELEASE);
        return hit;
    }

    // 匹配路径
    static bool isTopApp(const uint8_t* raw, const uint32_t size) {
        constexpr uint32_t kFixed = 36;
        if (size < kFixed + 2) return false;

        uint32_t pathLoc = 0;
        memcpy(&pathLoc, raw + 28, sizeof(pathLoc));
        const uint32_t offset = pathLoc & 0xffff;
        if (offset < kFixed || offset >= size) return false;

        const char* path = reinterpret_cast<const char*>(raw + offset);
        const uint32_t room = size - offset;
        static const char kLeaf[] = "/top-app";
        const uint32_t leaf = sizeof(kLeaf) - 1;
        if (room < leaf) return false;
        if (memcmp(path, kLeaf, leaf) != 0) return false;
        return room == leaf || path[leaf] == '\0' || path[leaf] == '/' ||
               path[leaf] == '\n';
    }

    std::vector<Ring> rings_;
    size_t page_ = 0;
    size_t length_ = 0;
};
