#include "objc_diagnostics.h"
#include "compat_runtime.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef LP32_OBJC_DIAGNOSTICS_DEFAULT
#define LP32_OBJC_DIAGNOSTICS_DEFAULT 0
#endif
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "Objective-C crash diagnostics need lock-free atomics");

enum { HISTORY = 128, LIFETIME_HISTORY = 64 };
struct slot { _Atomic bool busy; struct objc_diag_record record; };
static struct slot history[HISTORY], lifetime[LIFETIME_HISTORY];
static _Atomic unsigned history_next, lifetime_next;
static _Atomic int enabled = -1;
static _Thread_local struct objc_diag_record active;
static _Thread_local _Atomic bool active_valid;
static _Thread_local uint32_t thread_id;
static _Thread_local uint32_t pool_depth;

static bool recording(void)
{
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        const char *setting = getenv("LP32_OBJC_DIAGNOSTICS");
        value = setting ? strcmp(setting, "0") != 0 : LP32_OBJC_DIAGNOSTICS_DEFAULT;
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value != 0;
}

static bool read_memory(uint64_t source, void *target, size_t size)
{
    mach_vm_size_t copied = 0;
    return source && mach_vm_read_overwrite(mach_task_self(), source, size,
        (mach_vm_address_t)target, &copied) == KERN_SUCCESS && copied == size;
}

static void copy_text(char output[80], const char *input)
{
    memset(output, 0, 80);
    if (!input) return;
    /* A short selector can be at the end of a mapped page. Do not require
       bytes from the next page merely to capture its terminating NUL. */
    for (size_t offset = 0; offset < 79;) {
        uintptr_t address = (uintptr_t)input + offset;
        size_t count = 4096 - (address & 4095);
        if (count > 79 - offset) count = 79 - offset;
        if (!read_memory(address, output + offset, count)) break;
        if (memchr(output + offset, 0, count)) break;
        offset += count;
    }
    output[79] = 0;
}

static struct objc_diag_record new_record(enum objc_diag_kind kind)
{
    struct guest_resume_context frame;
    if (!thread_id) thread_id = pthread_mach_thread_np(pthread_self());
    return (struct objc_diag_record){.kind = kind, .thread = thread_id,
        .time = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), .depth = pool_depth,
        .caller = compat_runtime32_import_frame(&frame) ? frame.eip : 0};
}

static void append(struct slot *slots, unsigned capacity, _Atomic unsigned *next,
                    struct objc_diag_record record)
{
    unsigned index = atomic_fetch_add_explicit(next, 1, memory_order_relaxed);
    struct slot *slot = &slots[index % capacity];
    bool expected = false;
    /* Drop a contended record. Neither a writer nor the crash handler may
       wait for a thread that a signal could have interrupted. */
    if (!atomic_compare_exchange_strong_explicit(&slot->busy, &expected, true,
            memory_order_acquire, memory_order_relaxed)) return;
    record.sequence = (uint64_t)index + 1;
    slot->record = record;
    atomic_store_explicit(&slot->busy, false, memory_order_release);
}

void objc_diag_begin(struct objc_diag_scope *scope, const char *import,
                     const uint32_t *arguments, uint32_t depth)
{
    memset(scope, 0, sizeof(*scope));
    if (!recording() || strncmp(import, "_objc_msgSend", 13)) return;
    scope->enabled = true;
    scope->previous_valid = atomic_load_explicit(&active_valid, memory_order_acquire);
    if (scope->previous_valid) scope->previous = active;
    struct objc_diag_record record = new_record(OBJC_DIAG_ENTER);
    pool_depth = depth;
    record.depth = depth;
    read_memory((uintptr_t)arguments, record.arguments, sizeof(record.arguments));
    bool stret = !strcmp(import, "_objc_msgSend_stret");
    record.token = record.arguments[stret ? 1 : 0];
    if (!strcmp(import, "_objc_msgSendSuper")) {
        uint32_t super[2] = {0};
        if (read_memory(record.token, super, sizeof(super))) {
            record.token = super[0];
            record.klass = super[1];
        }
    }
    copy_text(record.text, (const char *)(uintptr_t)record.arguments[stret ? 2 : 1]);
    atomic_store_explicit(&active_valid, false, memory_order_release);
    active = record;
    atomic_store_explicit(&active_valid, true, memory_order_release);
    append(history, HISTORY, &history_next, record);
}

void objc_diag_end(struct objc_diag_scope *scope)
{
    if (!scope->enabled) return;
    struct objc_diag_record record = active;
    record.kind = OBJC_DIAG_LEAVE;
    append(history, HISTORY, &history_next, record);
    atomic_store_explicit(&active_valid, false, memory_order_release);
    if (scope->previous_valid) {
        active = scope->previous;
        active.depth = pool_depth;
        atomic_store_explicit(&active_valid, true, memory_order_release);
    }
}

void objc_diag_resolved(uint32_t token, const void *native, uint32_t klass)
{
    if (!atomic_load_explicit(&active_valid, memory_order_acquire) || active.token != token) return;
    atomic_store_explicit(&active_valid, false, memory_order_release);
    active.native = (uintptr_t)native;
    if (klass) active.klass = klass;
    struct objc_diag_record record = active;
    record.kind = OBJC_DIAG_RESOLVED;
    atomic_store_explicit(&active_valid, true, memory_order_release);
    append(history, HISTORY, &history_next, record);
}

void objc_diag_method(uint32_t token, uint32_t imp, const char *encoding)
{
    if (!atomic_load_explicit(&active_valid, memory_order_acquire) || active.token != token) return;
    atomic_store_explicit(&active_valid, false, memory_order_release);
    active.value = imp;
    copy_text(active.encoding, encoding);
    struct objc_diag_record record = active;
    record.kind = OBJC_DIAG_METHOD;
    atomic_store_explicit(&active_valid, true, memory_order_release);
    append(history, HISTORY, &history_next, record);
}

void objc_diag_note(enum objc_diag_kind kind, uint32_t token, const void *native,
                    uint32_t klass, const char *text, uint32_t depth)
{
    if (!recording()) return;
    struct objc_diag_record record = new_record(kind);
    record.token = token; record.native = (uintptr_t)native;
    record.klass = klass; record.depth = depth == UINT32_MAX ? pool_depth : depth;
    if (kind == OBJC_DIAG_POOL_PUSH || kind == OBJC_DIAG_POOL_POP) {
        pool_depth = depth;
        if (atomic_load_explicit(&active_valid, memory_order_acquire)) {
            atomic_store_explicit(&active_valid, false, memory_order_release);
            active.depth = depth;
            atomic_store_explicit(&active_valid, true, memory_order_release);
        }
    }
    copy_text(record.text, text);
    append(history, HISTORY, &history_next, record);
    if (kind == OBJC_DIAG_CREATE || kind == OBJC_DIAG_DEALLOC || kind == OBJC_DIAG_RETIRE)
        append(lifetime, LIFETIME_HISTORY, &lifetime_next, record);
}

static void report_record(int fd, int extra_fd, const char *prefix, struct objc_diag_record record)
{
    static const char *names[] = {"enter", "resolved", "method", "leave", "create",
        "dealloc", "retire", "pool-push", "pool-pop", "autorelease"};
    char line[512];
    int count = snprintf(line, sizeof(line), "compat32: objc %s %llu %s thread=%u "
        "guest=%08x native=%016llx class=%08x caller=%08x pool=%u method=%08x "
        "text=%s encoding=%s args=%08x,%08x,%08x,%08x,%08x,%08x\n",
        prefix, (unsigned long long)record.sequence,
        record.kind < sizeof(names) / sizeof(names[0]) ? names[record.kind] : "unknown",
        record.thread, record.token, (unsigned long long)record.native, record.klass,
        record.caller, record.depth, record.value, record.text, record.encoding,
        record.arguments[0], record.arguments[1], record.arguments[2],
        record.arguments[3], record.arguments[4], record.arguments[5]);
    if (count <= 0) return;
    size_t size = (size_t)count < sizeof(line) ? (size_t)count : sizeof(line) - 1;
    if (fd >= 0) write(fd, line, size);
    if (extra_fd >= 0) write(extra_fd, line, size);
}

static void report_ring(int fd, int extra_fd, const char *name, struct slot *slots,
                        unsigned capacity, _Atomic unsigned *next)
{
    unsigned end = atomic_load_explicit(next, memory_order_relaxed);
    unsigned count = end < capacity ? end : capacity;
    for (unsigned i = end - count; i != end; ++i) {
        struct slot *slot = &slots[i % capacity];
        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(&slot->busy, &expected, true,
                memory_order_acquire, memory_order_relaxed)) continue;
        struct objc_diag_record record = slot->record;
        atomic_store_explicit(&slot->busy, false, memory_order_release);
        if (record.sequence == (uint64_t)i + 1) report_record(fd, extra_fd, name, record);
    }
}

void objc_diag_crash_report(int fd, int extra_fd)
{
    if (atomic_load_explicit(&enabled, memory_order_relaxed) != 1) return;
    if (atomic_load_explicit(&active_valid, memory_order_acquire))
        report_record(fd, extra_fd, "active", active);
    report_ring(fd, extra_fd, "history", history, HISTORY, &history_next);
    report_ring(fd, extra_fd, "lifetime", lifetime, LIFETIME_HISTORY, &lifetime_next);
}
