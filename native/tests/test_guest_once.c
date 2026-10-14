#include "guest_once.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct guest_once_control controls[4104];
static int entered, release_initializer;
static atomic_int calls, returned;
static int published[4104];

static int blocking_initializer(void *unused)
{
    int *value = unused;
    atomic_fetch_add(&calls, 1);
    pthread_mutex_lock(&lock);
    ++entered;
    pthread_cond_broadcast(&changed);
    while (!release_initializer) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    *value = 42;
    return 0;
}

static void *run_once(void *key)
{
    struct guest_once_control *control = key;
    int *value = &published[control - controls];
    assert(guest_once_run(control, blocking_initializer, value) == 0);
    assert(*value == 42);
    atomic_fetch_add(&returned, 1);
    return NULL;
}

static void await_entered(int count)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&lock);
    while (entered < count) assert(pthread_cond_timedwait(&changed, &lock, &deadline) == 0);
    pthread_mutex_unlock(&lock);
}

static void release(void)
{
    pthread_mutex_lock(&lock);
    release_initializer = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static int count_initializer(void *count)
{
    ++*(int *)count;
    return 0;
}

static int nested_initializer(void *count)
{
    assert(guest_once_run(&controls[3], count_initializer, count) == 0);
    ++*(int *)count;
    return 0;
}

static int failed_initializer(void *unused)
{
    (void)unused;
    return ECANCELED;
}

static int exiting_initializer(void *unused)
{
    (void)unused;
    pthread_exit(NULL);
}

static void *exit_once(void *unused)
{
    (void)unused;
    guest_once_run(&controls[5], exiting_initializer, NULL);
    assert(0 && "initializer exit must leave the host thread");
    return NULL;
}

static int cancelled_initializer(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lock);
    entered = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (;;) {
        struct timespec pause = { .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
        pthread_testcancel();
    }
}

static void *cancel_once(void *unused)
{
    (void)unused;
    guest_once_run(&controls[7], cancelled_initializer, NULL);
    assert(0 && "initializer must be cancelled");
    return NULL;
}

int main(void)
{
    assert(guest_once_run(0, count_initializer, NULL) == EINVAL);
    assert(guest_once_run(&controls[0], NULL, NULL) == EINVAL);
    pthread_t first, second;
    /* Separate controls must enter callbacks together before either finishes. */
    assert(pthread_create(&first, NULL, run_once, &controls[0]) == 0);
    await_entered(1);
    assert(pthread_create(&second, NULL, run_once, &controls[1]) == 0);
    await_entered(2);
    assert(atomic_load(&returned) == 0);
    release();
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    assert(atomic_load(&calls) == 2);

    entered = release_initializer = published[2] = 0;
    atomic_store(&calls, 0);
    atomic_store(&returned, 0);
    pthread_t waiters[16];
    for (unsigned i = 0; i < 16; ++i)
        assert(pthread_create(&waiters[i], NULL, run_once, &controls[2]) == 0);
    await_entered(1);
    struct timespec pause = { .tv_nsec = 50000000 };
    nanosleep(&pause, NULL);
    assert(atomic_load(&calls) == 1);
    assert(atomic_load(&returned) == 0);
    release();
    for (unsigned i = 0; i < 16; ++i) pthread_join(waiters[i], NULL);
    assert(atomic_load(&calls) == 1);
    assert(atomic_load(&returned) == 16);

    int count = 0;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < 4096; ++i)
            assert(guest_once_run(&controls[8 + i], count_initializer, &count) == 0);
    assert(count == 4096);
    count = 0;
    assert(guest_once_run(&controls[4], nested_initializer, &count) == 0);
    assert(count == 2);
    assert(guest_once_run(&controls[4], nested_initializer, &count) == 0);
    assert(count == 2);
    count = 0;
    assert(guest_once_run(&controls[6], failed_initializer, NULL) == ECANCELED);
    assert(controls[6].state == 0);
    assert(guest_once_run(&controls[6], count_initializer, &count) == 0);
    assert(count == 1);
    assert(pthread_create(&first, NULL, exit_once, NULL) == 0);
    pthread_join(first, NULL);
    assert(controls[5].state == 0);
    assert(guest_once_run(&controls[5], count_initializer, &count) == 0);
    assert(count == 2);
    entered = 0;
    assert(pthread_create(&first, NULL, cancel_once, NULL) == 0);
    await_entered(1);
    assert(pthread_cancel(first) == 0);
    void *cancel_result;
    pthread_join(first, &cancel_result);
    assert(cancel_result == PTHREAD_CANCELED);
    assert(controls[7].state == 0);
    assert(guest_once_run(&controls[7], count_initializer, &count) == 0);
    assert(count == 3);
    struct guest_once_control recycled = { .signature = 0x30b1bcba };
    assert(guest_once_run(&recycled, count_initializer, &count) == 0);
    assert(count == 4);
    assert(recycled.signature == 0x30b1bcba);
    assert(recycled.state == UINT32_MAX);
    recycled = (struct guest_once_control){ .signature = 0x30b1bcba };
    assert(guest_once_run(&recycled, count_initializer, &count) == 0);
    assert(count == 5);
    assert(recycled.signature == 0x30b1bcba);
    puts("guest once: independent concurrency, completion visibility, contention, 4096 controls, nesting, abort/exit/cancel retry, reset/reuse, and invalid arguments passed");
}
