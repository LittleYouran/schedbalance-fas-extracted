#define _GNU_SOURCE 1

// 帧源 Hook

#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dobby/dobby.h>

#define TAG_HOOK  "FAS_HOOK"
#define TAG_BATCH "FAS_BATCH"
#define TAG_TRACE "FAS_TRACE"

#define LOGI(tag, ...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define LOGE(tag, ...) __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)

// 批量上报
#define BATCH_MAX_FRAMES 8
#define BATCH_MAX_NS     (50ULL * 1000000ULL)
#define BATCH_TEXT       1024

static char batchText[BATCH_TEXT];
static size_t batchLen;
static int batchFrames;
static uint64_t batchStartNs;

typedef void (*TraceBeginFn)(uint64_t category, const char* name);

static TraceBeginFn realTraceBegin;
static uint64_t (*realTags)(void);
static void (*realBeginBody)(const char*);

static pthread_mutex_t stateMutex = PTHREAD_MUTEX_INITIALIZER;

static char marker[64];
static int started;
static uint64_t lastNs;
static unsigned sequence;
static int recent[3] = {40, 40, 40};

#define SAMPLE_N 16
#define CAND_MAX 8
struct Cand {
    char name[56];
    uint64_t lastNs;
    uint64_t win[SAMPLE_N];
    int count;
    int banned;
};
static struct Cand cand[CAND_MAX];
static int candCount;

#define PROBE_MAX 60
static char probe[PROBE_MAX][56];
static int probeCount;
static uint64_t probeWindowStart;

static const char* markers[] = {
    "composite ",
    "GameEdr_frameEnd",
    "commit ",
    "postComposition",
    "onPostComposition",
    "postCompositionLocked",
    "onMessageRefresh",
    "doComposition",
};

static uint64_t nowNs(void) {
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void batchPush(const unsigned seq, const uint64_t ts,
                      const uint64_t frameUs, const int flags) {
    char entry[64];
    const int length = snprintf(entry, sizeof(entry), "%u %llu %llu %d", seq,
                                (unsigned long long)ts,
                                (unsigned long long)frameUs, flags);
    if (length <= 0 || length >= (int)sizeof(entry)) return;

    const size_t need = (size_t)length + (batchFrames ? 1 : 0);
    if (batchLen + need + 1 > sizeof(batchText)) return;

    if (batchFrames) batchText[batchLen++] = ';';
    memcpy(batchText + batchLen, entry, (size_t)length);
    batchLen += (size_t)length;
    batchText[batchLen] = '\0';
    ++batchFrames;
}

static int batchFlush(char* out, const size_t size) {
    if (batchFrames == 0) return 0;
    const size_t copy = batchLen < size - 1 ? batchLen : size - 1;
    memcpy(out, batchText, copy);
    out[copy] = '\0';

    const int frames = batchFrames;
    batchText[0] = '\0';
    batchLen = 0;
    batchFrames = 0;
    batchStartNs = 0;
    return frames;
}

static int nameMatch(const char* name, const char* key) {
    const size_t length = strlen(key);
    if (length == 0 || strncmp(name, key, length) != 0) return 0;
    const char next = name[length];
    if (next == '\0') return 1;
    if (key[length - 1] == ' ') return 1;
    return next == ' ' || next == '\n' || next == ':';
}

static void probeLog(const char* name) {
    const uint64_t now = nowNs();
    if (!probeWindowStart) probeWindowStart = now;
    if (now - probeWindowStart > 600000000000ULL) {
        probeCount = 0;
        probeWindowStart = now;
    }
    if (probeCount >= PROBE_MAX) return;
    char trimmed[56];
    snprintf(trimmed, sizeof(trimmed), "%s", name);
    for (char* p = trimmed; *p; ++p)
        if (*p >= '0' && *p <= '9') *p = '#';
    for (int i = 0; i < probeCount; ++i)
        if (!strcmp(probe[i], trimmed)) return;
    snprintf(probe[probeCount], sizeof(probe[0]), "%s", trimmed);
    LOGI(TAG_TRACE, "probe[%02d] tid=%d %s", probeCount, gettid(),
         probe[probeCount]);
    ++probeCount;
}

static void winSort(uint64_t* win, const int n) {
    for (int i = 1; i < n; ++i) {
        const uint64_t value = win[i];
        int j = i - 1;
        while (j >= 0 && win[j] > value) {
            win[j + 1] = win[j];
            --j;
        }
        win[j + 1] = value;
    }
}

// 验收
static void judge(struct Cand* c) {
    if (c->count < SAMPLE_N || c->banned) return;

    uint64_t win[SAMPLE_N];
    memcpy(win, c->win, sizeof(win));
    winSort(win, SAMPLE_N);
    const uint64_t lo = win[3];
    const uint64_t hi = win[12];
    const uint64_t med = (win[7] + win[8]) / 2;
    const int stable =
        med >= 2000000ULL && med <= 40000000ULL && hi <= lo * 2;

    if (stable) {
        snprintf(marker, sizeof(marker), "%s", c->name);
        started = 0;
        sequence = 0;
        recent[0] = recent[1] = recent[2] = 40;
        LOGI(TAG_HOOK, "marker=%s 验收通过 med=%lluus", marker,
             (unsigned long long)(med / 1000ULL));
        return;
    }

    c->banned = 1;
    int live = 0;
    for (int i = 0; i < candCount; ++i)
        if (!cand[i].banned) live = 1;
    if (!live && !marker[0]) {
        snprintf(marker, sizeof(marker), "%s", c->name);
        started = 0;
        LOGE(TAG_HOOK, "候选全部验收失败，退回保底 marker=%s", marker);
    }
}

static struct Cand* candGet(const char* key) {
    for (int i = 0; i < candCount; ++i)
        if (!strcmp(cand[i].name, key)) return &cand[i];
    if (candCount >= CAND_MAX) return NULL;
    struct Cand* c = &cand[candCount++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", key);
    return c;
}

static void candSample(struct Cand* c, const uint64_t now) {
    if (c->banned) return;
    if (c->lastNs == 0) {
        c->lastNs = now;
        return;
    }
    const uint64_t delta = now - c->lastNs;
    c->lastNs = now;
    if (delta < 1000000ULL || delta > 120000000ULL) return;
    c->win[c->count % SAMPLE_N] = delta;
    ++c->count;
    if (c->count >= SAMPLE_N) {
        c->count = SAMPLE_N;
        judge(c);
    }
}

#define LOCKED_N 32
static uint64_t lockedWin[LOCKED_N];
static int lockedCount;

// 自检
static void lockedHealth(const uint64_t delta) {
    lockedWin[lockedCount % LOCKED_N] = delta;
    ++lockedCount;
    if (lockedCount < LOCKED_N || lockedCount % LOCKED_N != 0) return;

    uint64_t win[LOCKED_N];
    memcpy(win, lockedWin, sizeof(win));
    winSort(win, LOCKED_N);
    const uint64_t lo = win[3];
    const uint64_t hi = win[28];
    const uint64_t med = (win[15] + win[16]) / 2;
    if (med >= 1500000ULL && med <= 130000000ULL && hi <= lo * 6) return;

    LOGE(TAG_HOOK, "marker=%s 节奏失稳 med=%lluus，重开验收", marker,
         (unsigned long long)(med / 1000ULL));
    marker[0] = '\0';
    started = 0;
    lockedCount = 0;
    batchText[0] = '\0';
    batchLen = 0;
    batchFrames = 0;
    batchStartNs = 0;
    for (int i = 0; i < candCount; ++i) {
        cand[i].count = 0;
        cand[i].lastNs = 0;
        cand[i].banned = 0;
    }
}

static void onTraceName(const char* name) {
    if (!name) return;

    const char* key = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i)
        if (nameMatch(name, markers[i])) {
            key = markers[i];
            break;
        }

    char flushText[BATCH_TEXT];
    int flushFrames = 0;

    pthread_mutex_lock(&stateMutex);
    probeLog(name);
    const uint64_t now = nowNs();
    if (marker[0]) {
        if (key && nameMatch(name, marker)) {
            if (!started) {
                started = 1;
                lastNs = now;
            } else {
                const uint64_t frameUs = (now - lastNs) / 1000ULL;
                lastNs = now;
                if (frameUs >= 1000 && frameUs <= 120000) {
                    lockedHealth(frameUs * 1000ULL);

                    const int frameMs = (int)(frameUs / 1000ULL);
                    int flags = 0;
                    const int average = (recent[0] + recent[1] + recent[2]) / 3;
                    if (frameMs > average * 2) flags = frameMs > 41 ? 2 : 1;
                    if (frameMs < 41) {
                        recent[0] = recent[1];
                        recent[1] = recent[2];
                        recent[2] = frameMs;
                    }
                    if (batchFrames == 0) batchStartNs = now;
                    batchPush(sequence++, now, frameUs, flags);
                    if (batchFrames >= BATCH_MAX_FRAMES ||
                        now - batchStartNs >= BATCH_MAX_NS)
                        flushFrames = batchFlush(flushText, sizeof(flushText));
                }
            }
        }
        pthread_mutex_unlock(&stateMutex);
        if (flushFrames > 0) LOGI(TAG_BATCH, "%s", flushText);
        return;
    }
    if (key) candSample(candGet(key), now);
    pthread_mutex_unlock(&stateMutex);
}

static void hookTraceBegin(uint64_t category, const char* name) {
    if (realTraceBegin) realTraceBegin(category, name);
    onTraceName(name);
}

static void hookBeginBody(const char* name) {
    if (realBeginBody) realBeginBody(name);
    onTraceName(name);
}

static uint64_t hookTags(void) {
    return (realTags ? realTags() : 0) | (1ULL << 1);
}

static void* resolvePerfettoTraceBegin(void) {
    if (android_get_device_api_level() < 35) return NULL;

    static const char* symbol = "_ZN16tracing_perfetto10traceBeginEmPKc";
    void* address = DobbySymbolResolver(NULL, symbol);
    if (address) return address;

    void* handle = dlopen("libtracing_perfetto.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen("libtracing_perfetto.so", RTLD_NOW);
    if (!handle) return NULL;
    return dlsym(handle, symbol);
}

// 注入入口
__attribute__((visibility("default")))
int fas_hook_init(void) {
    static int hookReady = 0;
    if (hookReady) return 0;

    void* begin = resolvePerfettoTraceBegin();
    if (begin) {
        DobbyHook(begin, (void*)hookTraceBegin, (void**)&realTraceBegin);
        LOGI(TAG_HOOK, "ready mode=perfetto traceBegin=%p", begin);
        hookReady = 1;
        return 0;
    }

    void* tags = DobbySymbolResolver(NULL, "atrace_get_enabled_tags");
    begin = DobbySymbolResolver(NULL, "atrace_begin_body");
    if (!tags || !begin) {
        LOGE(TAG_HOOK, "resolve failed tags=%p begin=%p", tags, begin);
        return 1;
    }
    DobbyHook(tags, (void*)hookTags, (void**)&realTags);
    DobbyHook(begin, (void*)hookBeginBody, (void**)&realBeginBody);
    LOGI(TAG_HOOK, "ready mode=atrace tags=%p begin=%p", tags, begin);
    hookReady = 1;
    return 0;
}
