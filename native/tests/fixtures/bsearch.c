#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Deliberately malformed calls bypass the SDK's nonnull annotation while
   still using the exact guest import, so compiler diagnostics stay strict. */
extern void *raw_bsearch(const void *, const void *, size_t, size_t,
                         int (*)(const void *, const void *)) __asm("_bsearch");

struct element { int value; uint32_t tag; };
struct array {
    uint32_t before;
    struct element values[9];
    uint32_t after;
};
struct key {
    uint32_t before;
    int value;
    struct key *self;
    const struct element *base;
    unsigned count, nested, calls, failure;
    uint32_t after;
};
static const struct array original = {0x1234abcd,
    {{-5,1},{-2,2},{0,3},{0,4},{0,5},{3,6},{9,7},{15,8},{42,9}},0x9876fedc};
static const uint16_t nested_values[] = {1,5,9,12,20};
static uint32_t started;
static const char *expected_route;

static int compare_u16(const void *key, const void *element)
{
    uint16_t wanted = *(const uint16_t *)key, actual = *(const uint16_t *)element;
    return wanted == actual ? 0 : wanted < actual ? INT_MIN : INT_MAX;
}

static int compare_records(const void *left, const void *right)
{
    struct key *key = (void *)left;
    /* A swapped key/element call fails before reading beyond an element. */
    if (key->before != 0x71524354) return INT_MIN;
    if (key->self != key || key->after != 0xabcdef01) { key->failure = 1; return INT_MIN; }
    uintptr_t element = (uintptr_t)right, first = (uintptr_t)key->base;
    if (element < first || element >= first + key->count * sizeof(struct element) ||
        (element - first) % sizeof(struct element)) { key->failure = 2; return INT_MIN; }
    ++key->calls;
    if (key->nested) {
        uint16_t wanted = 12;
        void *found = bsearch(&wanted,nested_values,5,sizeof(uint16_t),compare_u16);
        if (found != nested_values + 3) { key->failure = 3; return INT_MIN; }
        wanted = 13;
        if (bsearch(&wanted,nested_values,5,sizeof(uint16_t),compare_u16)) {
            key->failure = 4; return INT_MIN;
        }
        if (strcmp(strerror(65),expected_route)) { key->failure = 5; return INT_MIN; }
        /* End a nested callback in a front-module bridge. The outer core
           dispatch must retain its name/match state on the next search. */
        struct { char name[256]; uint32_t guard; } hostname = {{0},0x4567abcd};
        if (gethostname(hostname.name,sizeof(hostname.name)) ||
            hostname.guard != 0x4567abcd) { key->failure = 6; return INT_MIN; }
    }
    int actual = ((const struct element *)right)->value;
    return key->value == actual ? 0 : key->value < actual ? INT_MIN : INT_MAX;
}

static int search(struct array *array, int value, unsigned nested)
{
    struct key key = {0x71524354,value,NULL,array->values,9,nested,0,0,0xabcdef01};
    key.self = &key;
    struct element *result = bsearch(&key,array->values,9,sizeof(array->values[0]),compare_records);
    if (key.failure || !key.calls || key.before != 0x71524354 || key.after != 0xabcdef01)
        return -991;
    int present = 0;
    for (unsigned i = 0; i < 9; ++i) if (array->values[i].value == value) present = 1;
    if (present) {
        if (!result || result < array->values || result >= array->values + 9 ||
            result->value != value || result->tag < 1 || result->tag > 9) return -992;
    } else if (result) return -993;
    return !memcmp(array,&original,sizeof(*array)) ? 0 : -994;
}

struct worker { unsigned nested; int failed; uint32_t guard; };
static void *search_worker(void *opaque)
{
    struct worker *worker = opaque;
    struct array array = original;
    const int values[] = {-6,-5,-2,0,1,3,8,9,15,40,42,43};
    while (!__atomic_load_n(&started,__ATOMIC_ACQUIRE)) usleep(100);
    for (unsigned round = 0; round < 16 && !worker->failed; ++round)
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            worker->failed = search(&array,values[i],worker->nested);
            if (worker->failed) break;
        }
    return worker;
}

int check_bsearch(void)
{
    expected_route = getenv("LP32_STRERROR_0");
    if (!expected_route) return -1000;
    struct array array = original;
    const int values[] = {-6,-5,-2,0,1,3,8,9,15,40,42,43};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        int error = search(&array,values[i],1); if (error) return error;
    }
    /* Match the observed server call's count41 / width2 shape. */
    struct { uint32_t before; uint16_t values[41]; uint16_t padding; uint32_t after; } words;
    words.before = 0x13579bdf; words.after = 0x2468ace0; words.padding = 0xabcd;
    for (unsigned i = 0; i < 41; ++i) words.values[i] = (uint16_t)(i * 2);
    uint16_t wanted = 36;
    if (bsearch(&wanted,words.values,41,2,compare_u16) != words.values + 18) return -995;
    wanted = 37;
    if (bsearch(&wanted,words.values,41,2,compare_u16)) return -996;
    wanted = 80;
    if (bsearch(&wanted,words.values,41,2,compare_u16) != words.values + 40 ||
        bsearch(&wanted,words.values + 40,1,2,compare_u16) != words.values + 40) return -997;
    if (words.before != 0x13579bdf || words.after != 0x2468ace0 || words.padding != 0xabcd) return -998;
    for (unsigned i = 0; i < 41; ++i) if (words.values[i] != i * 2) return -999;

    struct key key = {0x71524354,0,NULL,array.values,9,0,0,0,0xabcdef01}; key.self = &key;
    errno = EDOM;
    if (raw_bsearch(NULL,NULL,0,UINT32_MAX,NULL) || errno != EDOM || key.calls) return -1001;
    if (bsearch(&key,(void *)0xfffffff0u,8,4,compare_records) || key.calls) return -1002;
    if (bsearch(&key,array.values,UINT32_MAX,UINT32_MAX,compare_records) || key.calls) return -1003;
    if (bsearch(&key,array.values,9,0,compare_records) || key.calls) return -1004;
    if (raw_bsearch(&key,array.values,9,sizeof(array.values[0]),NULL) || key.calls) return -1005;
    if (bsearch(NULL,array.values,9,sizeof(array.values[0]),compare_records) || key.calls) return -1006;

    enum { THREADS = 4 };
    struct worker workers[THREADS];
    struct { pthread_t value; uint32_t guard; } threads[THREADS];
    __atomic_store_n(&started,0,__ATOMIC_RELEASE);
    for (unsigned i = 0; i < THREADS; ++i) {
        workers[i] = (struct worker){i % 2,0,0xabcdefab};
        threads[i].guard = 0x98765432;
        if (pthread_create(&threads[i].value,NULL,search_worker,workers + i)) return -1007;
    }
    __atomic_store_n(&started,1,__ATOMIC_RELEASE);
    for (unsigned i = 0; i < THREADS; ++i) {
        struct { void *value; uint32_t guard; } joined = {NULL,0xfedcba98};
        if (pthread_join(threads[i].value,&joined.value) || joined.value != workers + i ||
            joined.guard != 0xfedcba98 || threads[i].guard != 0x98765432 || workers[i].guard != 0xabcdefab)
            return -1008;
        if (workers[i].failed) return workers[i].failed;
    }
    return 0;
}
