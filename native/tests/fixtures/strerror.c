#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *legacy_strerror(int) __asm("_strerror");

enum { CASES = 8, THREADS = 8 };
static const int errors[CASES] = {65, ENOENT, EACCES, 0, -1, INT_MIN, INT_MAX, 123456789};
static const char *expectations[CASES];
static uint32_t start;
struct worker {
    uint32_t before;
    unsigned index;
    char *messages[CASES];
    int failure;
    uint32_t after;
};

static int check_message(int index, char **saved)
{
    errno = EDOM;
    char *message = strerror(errors[index]);
    int expected_errno = index >= 4 ? EINVAL : EDOM;
    if (!message || errno != expected_errno ||
        (uintptr_t)message + strlen(expectations[index]) + 1 > UINT32_MAX ||
        strcmp(message, expectations[index])) return -981;
    if (*saved && (*saved != message || strcmp(*saved, expectations[index]))) return -982;
    *saved = message;
    struct { char text[256]; uint32_t guard; } copy = {{0}, 0x1234abcd};
    size_t size = strlen(message) + 1;
    if (size > sizeof(copy.text)) return -983;
    for (size_t i = 0; i < size; ++i) copy.text[i] = message[i];
    return copy.guard == 0x1234abcd && !strcmp(copy.text,expectations[index]) ? 0 : -984;
}

static void *strerror_worker(void *context)
{
    struct worker *worker = context;
    while (!__atomic_load_n(&start,__ATOMIC_ACQUIRE)) usleep(100);
    for (unsigned round = 0; round < 64 && !worker->failure; ++round)
        for (unsigned i = 0; i < CASES; ++i) {
            unsigned index = (worker->index + i) % CASES;
            worker->failure = check_message((int)index,worker->messages + index);
            if (worker->failure) break;
        }
    return worker;
}

int check_strerror(void)
{
    const char *names[CASES] = {"LP32_STRERROR_0","LP32_STRERROR_1","LP32_STRERROR_2","LP32_STRERROR_3",
        "LP32_STRERROR_4","LP32_STRERROR_5","LP32_STRERROR_6","LP32_STRERROR_7"};
    for (unsigned i = 0; i < CASES; ++i) {
        expectations[i] = getenv(names[i]);
        if (!expectations[i]) return -985;
    }
    char *retained = NULL;
    int error = check_message(0,&retained); if (error) return error;
    struct worker workers[THREADS];
    struct { pthread_t thread; uint32_t guard; } threads[THREADS];
    memset(workers,0,sizeof(workers));
    __atomic_store_n(&start,0,__ATOMIC_RELEASE);
    for (unsigned i = 0; i < THREADS; ++i) {
        workers[i].before = 0xabcddcba; workers[i].after = 0x98766789; workers[i].index = i;
        threads[i].guard = 0x87654321;
        if (pthread_create(&threads[i].thread,NULL,strerror_worker,workers + i)) return -986;
    }
    __atomic_store_n(&start,1,__ATOMIC_RELEASE);
    for (unsigned i = 0; i < THREADS; ++i) {
        struct { void *worker; uint32_t guard; } result = {NULL,0x13579bdf};
        if (pthread_join(threads[i].thread,&result.worker) || result.worker != workers + i ||
            result.guard != 0x13579bdf || threads[i].guard != 0x87654321 ||
            workers[i].before != 0xabcddcba || workers[i].after != 0x98766789) return -987;
        if (workers[i].failure) return workers[i].failure;
    }
    for (unsigned i = 0; i < CASES; ++i) {
        char *message = workers[0].messages[i];
        if (!message || strcmp(message,expectations[i])) return -988;
        for (unsigned thread = 1; thread < THREADS; ++thread)
            if (workers[thread].messages[i] != message) return -989;
        error = check_message((int)i,&message); if (error) return error;
    }
    return !strcmp(retained,expectations[0]) && legacy_strerror(65) == retained ? 0 : -990;
}
