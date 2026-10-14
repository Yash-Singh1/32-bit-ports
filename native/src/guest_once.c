#include "guest_once.h"

#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>

_Static_assert(sizeof(struct guest_once_control) == 8, "Darwin i386 once size");
_Static_assert(offsetof(struct guest_once_control, state) == 4, "Darwin i386 once state");

struct once_entry {
    struct guest_once_control *control;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int running;
    struct once_entry *next;
};

/* Entries never move or disappear while another thread can use them. */
static struct once_entry *entries[1024];
static pthread_mutex_t entries_lock = PTHREAD_MUTEX_INITIALIZER;
/* Stable entries let repeated resets avoid registry locking too. */
static _Thread_local struct once_entry *recent[8];

static void unlock_entry(void *opaque)
{
    struct once_entry *entry = opaque;
    pthread_mutex_unlock(&entry->lock);
}

static void abandon_initializer(void *opaque)
{
    struct once_entry *entry = opaque;
    pthread_mutex_lock(&entry->lock);
    entry->running = 0;
    pthread_cond_broadcast(&entry->changed);
    pthread_mutex_unlock(&entry->lock);
}

int guest_once_run(struct guest_once_control *control, int (*initialize)(void *), void *context)
{
    if (!control || !initialize) return EINVAL;
    if (__atomic_load_n(&control->state, __ATOMIC_ACQUIRE) == UINT32_MAX) return 0;
    uintptr_t address = (uintptr_t)control;
    unsigned bucket = ((address >> 3) ^ (address >> 13)) % 1024;
    unsigned slot = bucket % 8;
    struct once_entry *entry = recent[slot];
    if (!entry || entry->control != control) {
        pthread_mutex_lock(&entries_lock);
        entry = entries[bucket];
        while (entry && entry->control != control) entry = entry->next;
        if (!entry) {
            entry = malloc(sizeof(*entry));
            if (!entry) {
                pthread_mutex_unlock(&entries_lock);
                return ENOMEM;
            }
            *entry = (struct once_entry){ .control = control,
                                         .lock = PTHREAD_MUTEX_INITIALIZER,
                                         .changed = PTHREAD_COND_INITIALIZER,
                                         .next = entries[bucket] };
            entries[bucket] = entry;
        }
        pthread_mutex_unlock(&entries_lock);
        recent[slot] = entry;
    }

    int complete = 0;
    pthread_mutex_lock(&entry->lock);
    pthread_cleanup_push(unlock_entry, entry);
    while (entry->running) pthread_cond_wait(&entry->changed, &entry->lock);
    complete = __atomic_load_n(&control->state, __ATOMIC_ACQUIRE) == UINT32_MAX;
    if (!complete) entry->running = 1;
    pthread_cleanup_pop(1);
    if (complete) return 0;

    int result;
    /* Cancellation or pthread_exit must make this control retryable. */
    pthread_cleanup_push(abandon_initializer, entry);
    result = initialize(context);
    pthread_mutex_lock(&entry->lock);
    if (result == 0) __atomic_store_n(&control->state, UINT32_MAX, __ATOMIC_RELEASE);
    entry->running = 0;
    pthread_cond_broadcast(&entry->changed);
    pthread_mutex_unlock(&entry->lock);
    pthread_cleanup_pop(0);
    return result;
}
