#include "guest_once.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Previous runtime algorithm, with its guest callback replaced by a no-op. */
static pthread_mutex_t old_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static struct guest_once_control *completed[256];
static unsigned completed_count;
static struct guest_once_control controls[128];
static int initialize(void *unused) { (void)unused; return 0; }

static int old_once(struct guest_once_control *control)
{
    pthread_mutex_lock(&old_lock);
    for (unsigned i = 0; i < completed_count; ++i) {
        if (completed[i] == control) {
            pthread_mutex_unlock(&old_lock);
            return 0;
        }
    }
    if (completed_count < 256) completed[completed_count++] = control;
    initialize(NULL);
    pthread_mutex_unlock(&old_lock);
    return 0;
}

struct job { int old; unsigned controls; unsigned iterations; };
static void *run(void *opaque)
{
    struct job *job = opaque;
    for (unsigned i = 0; i < job->iterations; ++i) {
        struct guest_once_control *control = &controls[i % job->controls];
        if (job->old) old_once(control);
        else guest_once_run(control, initialize, NULL);
    }
    return NULL;
}

static double now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

int main(void)
{
    for (unsigned i = 0; i < 128; ++i) {
        old_once(&controls[i]);
        guest_once_run(&controls[i], initialize, NULL);
    }
    for (unsigned threads = 1; threads <= 4; threads *= 4) {
        for (unsigned count = 1; count <= 128; count *= 128) {
            for (int old = 1; old >= 0; --old) {
                struct job job = { old, count, 1000000 };
                pthread_t workers[4];
                double start = now();
                for (unsigned i = 0; i < threads; ++i) pthread_create(&workers[i], NULL, run, &job);
                for (unsigned i = 0; i < threads; ++i) pthread_join(workers[i], NULL);
                double elapsed = now() - start;
                printf("%s, %u threads, %u controls: %.2f ns/call\n",
                       old ? "previous" : "current", threads, count,
                       elapsed * 1e9 / (threads * job.iterations));
            }
        }
    }
}
