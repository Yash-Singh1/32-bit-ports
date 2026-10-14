#include "hitch_recorder.h"
#include "host_diagnostics.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <mach/mach_time.h>

/* Fixed storage; no allocation, driver queries, or file writes on a draw.
 * One render-thread producer, one log writer, and a small worker-event ring.
 * At most 128 reports per session, each with 32 preceding and 8 following
 * frames. A five-second cooldown prevents sustained low FPS flooding disk. */
enum { HISTORY = 32, AFTER = 8, TOP = 3, WORKERS = 16, REPORT_LIMIT = 128 };
struct event {
    const char *name;
    uint64_t start, ns;
    uint32_t caller, vp, fp, count;
};
struct frame {
    uint64_t swap, end, work, flush, pace, present;
    uint64_t counted_runtime;
    uint64_t count[HITCH_KIND_COUNT], ns[HITCH_KIND_COUNT];
    uint64_t gl_state_samples;
    struct event top[TOP];
    bool active;
};
struct report {
    struct frame frames[HISTORY + AFTER + 1];
    struct event workers[WORKERS];
    unsigned count, worker_count;
    uint64_t trigger;
};
int hitch_recorder_enabled;
int hitch_full_imports;
static FILE *output;
static pthread_t writer;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool stopping, queued;
static struct report pending, queue;
static struct frame history[HISTORY];
static _Thread_local struct frame current;
static pthread_mutex_t presentation_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t owner_token, next_owner_token, ownership_generation;
static _Thread_local uint64_t presenter_token;
static struct event workers[WORKERS];
static unsigned history_count, history_next, worker_next, worker_count;
static unsigned following, reports, skipped;
static uint64_t previous_end, last_trigger, threshold_ns;
static _Thread_local uint32_t vertex_program, fragment_program;
static _Thread_local struct hitch_scope *active_scope;
static _Thread_local uint64_t frame_generation;
static _Thread_local unsigned gl_state_sequence;
static bool is_current_presenter(void)
{
    return presenter_token && presenter_token == __atomic_load_n(&owner_token, __ATOMIC_ACQUIRE);
}

static mach_timebase_info_data_t hitch_timebase;
__attribute__((constructor)) static void initialize_hitch_clock(void)
{
    mach_timebase_info(&hitch_timebase);
}
uint64_t hitch_now(void)
{
    /* CLOCK_UPTIME_RAW uses this same clock. Rosetta's 1:1 timebase lets us
       avoid clock_gettime's dispatch/conversion on every timed import. */
    uint64_t ticks = mach_absolute_time();
    if (hitch_timebase.numer == hitch_timebase.denom) return ticks;
    return (uint64_t)((__uint128_t)ticks * hitch_timebase.numer / hitch_timebase.denom);
}

unsigned hitch_classify(const char *name)
{
    if (*name == '_') ++name;
    /* Timing millions of tiny libc calls can itself extend a loading frame
       by hundreds of milliseconds. Count these separately by default;
       LP32_HITCH_FULL_IMPORTS=1 restores per-call timings for diagnosis.
       Keep allocation, locks, file access, graphics and audio fully timed. */
    if (!strcmp(name, "strlen") || !strcmp(name, "strcmp") ||
        !strcmp(name, "memcmp") || !strcmp(name, "strcpy") ||
        !strcmp(name, "strncpy") || !strcmp(name, "memcpy") ||
        !strcmp(name, "memmove") || !strcmp(name, "memset") ||
        !strcmp(name, "__tolower") || !strcmp(name, "pthread_self"))
        return hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME;
    /* These direct handlers only read thread-local state or compute scalar
       arithmetic. TLS resolver calls can allocate and run constructors, so
       they deliberately retain full timings. */
    static const char *const cheap_helpers[] = {
        "pthread_getspecific", "logf", "expf", "floorf", "ceilf", "acosf",
        "asinf", "sinf", "cosf", "atanf", "roundf", "powf", "atan2f", "fmodf",
        "floor", "ceil", "sin", "cos", "pow", "atan2", "fmod",
        "__udivdi3", "__umoddi3", "__divdi3", "__moddi3",
        "OSAtomicCompareAndSwap32", "OSAtomicCompareAndSwapInt",
        "OSAtomicCompareAndSwap32Barrier", "OSAtomicCompareAndSwapPtrBarrier",
        "OSAtomicCompareAndSwap64", "OSAtomicCompareAndSwap64Barrier",
        "OSAtomicAdd64", "pthread_mutex_trylock",
    };
    for (unsigned i = 0; i < sizeof(cheap_helpers) / sizeof(cheap_helpers[0]); ++i)
        if (!strcmp(name, cheap_helpers[i]))
            return hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME;
    /* Atomic-add tracing performs file I/O. Its cold parser also remains
       timed because the import dispatcher times every uncached handler. */
    if ((!strcmp(name, "OSAtomicAdd32") || !strcmp(name, "OSAtomicAdd32Barrier")) &&
        !getenv("LP32_TRACE_ATOMIC"))
        return hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME;
    if (!strcmp(name, "mach_absolute_time") && !getenv("LP32_TRACE_TIMING"))
        return hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME;
    /* Steam RemoteStorage uses generated C++ vtable thunks rather than libc
     * file imports, so include these in I/O attribution too. */
    if (!strncmp(name, "lp32_steam_4_", 13)) return HITCH_IO;
    if (!strncmp(name, "glDraw", 6) &&
        (strstr(name, "Elements") || strstr(name, "Arrays"))) return HITCH_DRAW;
    if (strstr(name, "TexImage") || strstr(name, "TexSubImage") ||
        strstr(name, "BufferData") || strstr(name, "BufferSubData") ||
        strstr(name, "MapBuffer") || strstr(name, "UnmapBuffer") ||
        strstr(name, "GenerateMipmap")) return HITCH_UPLOAD;
    if (!strcmp(name, "cgCreateProgram") || !strcmp(name, "cgCompileProgram") ||
        !strcmp(name, "glProgramStringARB") || !strcmp(name, "glCompileShader") ||
        !strcmp(name, "glLinkProgram")) return HITCH_SHADER;
    if (!strcmp(name, "fread") || !strcmp(name, "fwrite") ||
        !strncmp(name, "read$", 5) || !strcmp(name, "read") ||
        !strncmp(name, "pread", 5) || !strncmp(name, "pwrite", 6) ||
        !strcmp(name, "write") || !strncmp(name, "write$", 6) ||
        !strcmp(name, "fopen") || !strcmp(name, "fclose") ||
        !strcmp(name, "FSReadFork") || !strcmp(name, "FSWriteFork") ||
        !strcmp(name, "FSRead") || !strcmp(name, "FSWrite") ||
        !strcmp(name, "PBReadForkAsync") || !strcmp(name, "PBReadForkSync")) return HITCH_IO;
    if (!strncmp(name, "pthread_mutex_lock", 18) ||
        !strncmp(name, "pthread_cond_wait", 17) ||
        !strncmp(name, "pthread_cond_timedwait", 22) ||
        !strcmp(name, "glFinish") || !strcmp(name, "glFlush") ||
        strstr(name, "WaitSync") || !strcmp(name, "MPWaitOnSemaphore") ||
        !strcmp(name, "MPWaitOnQueue") || !strcmp(name, "MPDelayUntil") ||
        !strcmp(name, "usleep") || !strncmp(name, "usleep$", 7) ||
        !strcmp(name, "nanosleep") || !strncmp(name, "nanosleep$", 10)) return HITCH_WAIT;
    if (!strncmp(name, "AudioQueue", 10) || !strncmp(name, "AudioUnit", 9) ||
        !strncmp(name, "AudioConverter", 14) ||
        !strncmp(name, "AudioFile", 9) || !strncmp(name, "ExtAudioFile", 12) ||
        !strncmp(name, "Snd", 3)) return HITCH_AUDIO;
    if (!strncmp(name, "gl", 2) || !strncmp(name, "CGL", 3)) return HITCH_GL_STATE;
    if (!strncmp(name, "objc_", 5)) return HITCH_OBJC;
    return HITCH_RUNTIME;
}

static void write_event(const char *label, const struct event *e)
{
    fprintf(output, "  %s name=%s start=%.3f ms=%.3f caller=%08x vp=%u fp=%u count=%u\n",
            label, e->name, e->start / 1e9, e->ns / 1e6,
            e->caller, e->vp, e->fp, e->count);
}
static void write_report(const struct report *r)
{
    fprintf(output, "hitch trigger=%llu frames=%u\n", (unsigned long long)r->trigger, r->count);
    for (unsigned i = 0; i < r->count; ++i) {
        const struct frame *f = &r->frames[i];
        fprintf(output, " frame=%llu end=%.3f active=%d present=%.3f work=%.3f flush=%.3f pace=%.3f",
                (unsigned long long)f->swap, f->end / 1e9, f->active,
                f->present / 1e6, f->work / 1e6, f->flush / 1e6, f->pace / 1e6);
        static const char *names[] = {"unknown", "none", "counted_runtime", "draw", "upload", "shader", "io", "wait", "audio", "glstate", "objc", "runtime"};
        for (unsigned k = HITCH_DRAW; k < HITCH_KIND_COUNT; ++k)
            fprintf(output, " %s=%llu/%.3f", names[k], (unsigned long long)f->count[k], f->ns[k] / 1e6);
        fprintf(output, " glstate_sampled_calls=%llu glstate_sampled_ms=%.3f",
                (unsigned long long)f->gl_state_samples, f->ns[HITCH_GL_STATE] / 1e6);
        fprintf(output, " counted_runtime=%llu\n", (unsigned long long)f->counted_runtime);
        for (unsigned j = 0; j < TOP; ++j)
            if (f->top[j].name) write_event("call", &f->top[j]);
    }
    for (unsigned i = 0; i < r->worker_count; ++i) write_event("worker", &r->workers[i]);
    fflush(output);
}
static void *write_loop(void *unused)
{
    (void)unused;
    struct report local;
    for (;;) {
        pthread_mutex_lock(&mutex);
        while (!queued && !stopping) pthread_cond_wait(&ready, &mutex);
        if (!queued && stopping) { pthread_mutex_unlock(&mutex); break; }
        local = queue;
        queued = false;
        pthread_mutex_unlock(&mutex);
        write_report(&local);
    }
    return NULL;
}
static void enqueue(void)
{
    pthread_mutex_lock(&mutex);
    if (!queued) {
        pending.worker_count = 0;
        uint64_t oldest = pending.frames[0].end - pending.frames[0].present;
        uint64_t newest = pending.frames[pending.count - 1].end;
        for (unsigned i = 0; i < worker_count; ++i) {
            const struct event *e = &workers[(worker_next + WORKERS - worker_count + i) % WORKERS];
            if (e->start <= newest && e->start + e->ns >= oldest)
                pending.workers[pending.worker_count++] = *e;
        }
        queue = pending;
        queued = true;
        ++reports;
        pthread_cond_signal(&ready);
    } else ++skipped;
    pthread_mutex_unlock(&mutex);
    pending.count = 0;
}
int hitch_start(const char *path, double threshold_ms)
{
    if (output) { errno = EALREADY; return -1; }
    output = fopen(path, "wx");
    if (!output) return -1;
    fchmod(fileno(output), 0600);
    threshold_ns = (threshold_ms >= 1 && threshold_ms <= 10000) ?
        (uint64_t)(threshold_ms * 1e6) : 25000000;
    /* Start/stop are called with guest recording quiescent. A new session
       must not inherit queued reports, frame history or timestamps. */
    memset(history, 0, sizeof(history));
    memset(&current, 0, sizeof(current));
    memset(&pending, 0, sizeof(pending));
    memset(&queue, 0, sizeof(queue));
    memset(workers, 0, sizeof(workers));
    history_count = history_next = worker_next = worker_count = 0;
    following = reports = skipped = 0;
    previous_end = last_trigger = 0;
    stopping = queued = false;
    gl_state_sequence = 0;
    __atomic_add_fetch(&ownership_generation, 1, __ATOMIC_ACQ_REL);
    /* Startup may run on the application thread while a guest Backend
       thread presents. Ownership is established at the first presentation. */
    __atomic_store_n(&owner_token, 0, __ATOMIC_RELEASE);
    const char *full = getenv("LP32_HITCH_FULL_IMPORTS");
    hitch_full_imports = full && full[0] && strcmp(full, "0");
    fprintf(output, "hitch-recorder v4 pid=%ld wall=%lld monotonic=%.3f threshold_ms=%.3f history=%d after=%d limit=%d full_imports=%d\n"
        "CPU wall timings only; draw time can include driver compilation or waiting, not GPU execution time.\n"
        "Category values are call-count/exclusive-ms. Work includes untimed guest work and gateway overhead; flush excludes pacing.\n"
        "Timed nested imports are excluded from parent timings; calls spanning presentation are omitted.\n",
        (long)getpid(), (long long)time(NULL), hitch_now() / 1e9, threshold_ns / 1e6,
        HISTORY, AFTER, REPORT_LIMIT, hitch_full_imports);
    fprintf(output, "glstate_every=%u; glstate count includes every call, its ms and glstate_sampled_ms cover only glstate_sampled_calls, without extrapolation. Untimed state calls remain in work and enclosing timed scopes. Full-import mode times every GL state call.\n", hitch_full_imports ? 1 : 32);
    fprintf(output, "counted_runtime contains untimed libc/string/thread-ID/scalar-math/TLS-value/atomic/cached-trylock calls; their time remains in work and any enclosing timed import.\n");
    fprintf(output, "Inline i386 atomic helpers bypass import/category counters; LP32_NO_PURE_IMPORTS=1 restores gateway accounting, and full-import/profiling/atomic-trace modes select the gateway automatically.\n");
    char host[768]; lp32_host_description(host, sizeof(host));
    fprintf(output, "%s\n", host);
    fflush(output);
    int error = pthread_create(&writer, NULL, write_loop, NULL);
    if (error) { fclose(output); output = NULL; errno = error; return -1; }
    hitch_recorder_enabled = 1;
    return 0;
}
void hitch_stop(void)
{
    if (!hitch_recorder_enabled) return;
    /* Called after guest execution stops; preserve a partially collected hitch. */
    if (pending.count) enqueue();
    pthread_mutex_lock(&mutex);
    stopping = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&mutex);
    pthread_join(writer, NULL);
    fprintf(output, "end reports=%u skipped=%u\n", reports, skipped);
    fclose(output);
    output = NULL;
    hitch_recorder_enabled = 0;
    __atomic_store_n(&owner_token, 0, __ATOMIC_RELEASE);
}
void hitch_program(uint32_t target, uint32_t program)
{
    if (target == 0x8620) vertex_program = program;
    if (target == 0x8804) fragment_program = program;
}
static void note_import(unsigned kind, const char *name, uint32_t caller,
                        uint64_t start, uint64_t end, uint32_t count,
                        bool count_call);

void hitch_scope_begin(struct hitch_scope *scope, uint64_t now)
{
    *scope = (struct hitch_scope){now, 0, frame_generation, active_scope,
        __atomic_load_n(&ownership_generation, __ATOMIC_ACQUIRE), false};
    active_scope = scope;
}
void hitch_scope_end(struct hitch_scope *scope, unsigned kind, const char *name,
                     uint32_t caller, uint64_t now, uint32_t count)
{
    uint64_t elapsed = now - scope->start;
    active_scope = scope->parent;
    if (active_scope) active_scope->children += elapsed;
    if (scope->generation != frame_generation || scope->ownership_generation !=
        __atomic_load_n(&ownership_generation, __ATOMIC_ACQUIRE)) return;
    uint64_t exclusive = elapsed > scope->children ? elapsed - scope->children : 0;
    note_import(kind, name, caller, scope->start, scope->start + exclusive, count,
                !scope->counted);
}
static void note_import(unsigned kind, const char *name, uint32_t caller,
                        uint64_t start, uint64_t end, uint32_t draw_count,
                        bool count_call)
{
    if (!hitch_recorder_enabled || kind < HITCH_DRAW || kind >= HITCH_KIND_COUNT) return;
    uint64_t ns = end - start;
    struct event e = {name, start, ns, caller, vertex_program, fragment_program, draw_count};
    if (!is_current_presenter()) {
        if (ns < 2000000 || kind == HITCH_WAIT) return;
        pthread_mutex_lock(&mutex);
        workers[worker_next++ % WORKERS] = e;
        if (worker_count < WORKERS) ++worker_count;
        pthread_mutex_unlock(&mutex);
        return;
    }
    if (count_call) ++current.count[kind];
    current.ns[kind] += ns;
    if (kind == HITCH_GL_STATE) ++current.gl_state_samples;
    for (unsigned i = 0; i < TOP; ++i) {
        if (ns > current.top[i].ns) {
            for (unsigned j = TOP - 1; j > i; --j) current.top[j] = current.top[j - 1];
            current.top[i] = e; break;
        }
    }
}
void hitch_note(unsigned kind, const char *name, uint32_t caller,
                uint64_t start, uint64_t end, uint32_t draw_count)
{
    note_import(kind, name, caller, start, end, draw_count, true);
}

bool hitch_sample_gl_state(void)
{
    if (!hitch_recorder_enabled) return false;
    if (is_current_presenter()) ++current.count[HITCH_GL_STATE];
    return hitch_full_imports || !(gl_state_sequence++ & 31);
}

void hitch_count_runtime(void)
{
    if (hitch_recorder_enabled && is_current_presenter()) ++current.counted_runtime;
}
void hitch_frame(uint64_t swap, uint64_t work_end, uint64_t flush_end,
                 uint64_t present_end, uint64_t target_ns, bool active)
{
    if (!hitch_recorder_enabled) return;
    /* One presentation producer owns current and its counters. Imports
       before its first presentation remain worker events. */
    pthread_mutex_lock(&presentation_mutex);
    /* Threads capture their timestamps before taking this lock. A delayed
       presentation must neither move the clock backwards nor take ownership.
       Its enclosing import still crosses a presentation boundary. */
    if ((previous_end && present_end <= previous_end) ||
        work_end > flush_end || flush_end > present_end) {
        ++frame_generation;
        memset(&current, 0, sizeof(current));
        pthread_mutex_unlock(&presentation_mutex);
        return;
    }
    if (work_end < previous_end) work_end = previous_end;
    if (flush_end < work_end) flush_end = work_end;
    if (!presenter_token) presenter_token = __atomic_add_fetch(&next_owner_token, 1, __ATOMIC_RELAXED);
    if (!is_current_presenter()) {
        memset(&current, 0, sizeof(current));
        __atomic_add_fetch(&ownership_generation, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&owner_token, presenter_token, __ATOMIC_RELEASE);
    }
    ++frame_generation;
    current.swap = swap; current.end = present_end; current.active = active;
    current.present = previous_end ? present_end - previous_end : 0;
    current.work = previous_end ? work_end - previous_end : 0;
    current.flush = flush_end - work_end;
    current.pace = present_end - flush_end;
    uint64_t limit = target_ns * 3 / 2;
    if (limit < threshold_ns) limit = threshold_ns;
    if (pending.count) {
        pending.frames[pending.count++] = current;
        if (!--following) enqueue();
    } else if (active && history_count == HISTORY && current.present > limit &&
               (!last_trigger || present_end - last_trigger >= 5000000000ULL) && reports < REPORT_LIMIT) {
        pending.trigger = swap;
        for (unsigned i = 0; i < history_count; ++i)
            pending.frames[pending.count++] = history[(history_next + HISTORY - history_count + i) % HISTORY];
        pending.frames[pending.count++] = current;
        following = AFTER;
        last_trigger = present_end;
    }
    history[history_next++ % HISTORY] = current;
    if (history_count < HISTORY) ++history_count;
    previous_end = present_end;
    memset(&current, 0, sizeof(current));
    gl_state_sequence = 0;
    pthread_mutex_unlock(&presentation_mutex);
}
