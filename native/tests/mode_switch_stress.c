/*
 * Rosetta mode-switch stress test.
 *
 * Reproduces the wrong-mode landing that crashed the game: threads doing
 * i386 <-> x86_64 round trips through far jumps while another thread keeps
 * Rosetta translating fresh code (which makes it flush its translation
 * cache every few seconds).  Threads that are in the middle of a far jump
 * during a flush resume on the far side in their old mode.
 *
 *   mode_switch_stress guarded=0 threads=12 seconds=30 churn=1
 *       plain pads, as the loader used before 2026-09-04: dies within
 *       seconds with SIGBUS at pad+6 (the 64-bit gateway pad decoded as
 *       i386) and/or SIGSEGV in a thunk (i386 decoded as x86_64).
 *   mode_switch_stress guarded=1 threads=12 seconds=30 churn=1
 *       the loader's guarded pads and landing trampoline: survives, and
 *       reports how many wrong-mode landings were recovered.
 *   mode_switch_stress guarded=1 test32=1 threads=1 seconds=1 iters=1000
 *       deliberately enters the 64-bit pad in i386 mode on every call to
 *       exercise the recovery path deterministically.
 *
 * Build: make -C native mode-switch-stress
 */
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>

extern int lp32_native_segments;
extern uint32_t run_compat32(uint32_t eip, uint32_t esp, uint16_t cs32);
extern void stress_gateway_plain(void);
extern void stress_gateway_guarded(void);
uint16_t stress_cs32;
uint32_t stress_landing32;

enum {
    kCode32 = 0x20000000,
    kCode64 = 0x20001000,
    kData = 0x20002000,
    kStacks = 0x21000000,
    kExitThunk = kCode32 + 0x000,
    kThunk = kCode32 + 0x100,
    kTestStub = kCode32 + 0x140,
    kLoop = kCode32 + 0x200,
    kLoopTest = kCode32 + 0x240,
    kLanding = kCode32 + 0x300,
    kLandingAlt = kCode32 + 0x340,
    kReturnPad = kCode64 + 0x000,
    kGatewayPad = kCode64 + 0x040,
    kReturnPadAlt = kCode64 + 0x080,
    kGatewayPadAlt = kCode64 + 0x0c0,
    kExitFar = kData + 0x00,
    kGatewayFar = kData + 0x10,
    kCounter = kData + 0x100,
    kWrongTo64 = kData + 0x110,
    kWrongTo32 = kData + 0x114,
    kFailed = kData + 0x118,
};

struct __attribute__((packed)) far_ptr32 {
    uint32_t offset;
    uint16_t selector;
};

static int guarded, threads = 4, seconds = 10, churn, test32;
static uint32_t iterations = 100000;
static _Atomic uint64_t total_calls;
static volatile int stop;

static void *map_low(size_t size, uintptr_t address)
{
    void *m = mmap((void *)address, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return m;
}

static uint8_t *emit(uint8_t *p, const void *bytes, size_t n)
{
    memcpy(p, bytes, n);
    return p + n;
}
static uint8_t *emit8(uint8_t *p, uint8_t v) { *p = v; return p + 1; }
static uint8_t *emit16(uint8_t *p, uint16_t v) { return emit(p, &v, 2); }
static uint8_t *emit32(uint8_t *p, uint32_t v) { return emit(p, &v, 4); }
static uint8_t *emit64(uint8_t *p, uint64_t v) { return emit(p, &v, 8); }

static uint32_t counter(uint32_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}

static void crash_handler(int sig, siginfo_t *info, void *opaque)
{
    ucontext_t *uc = opaque;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "\nFATAL signal %d addr=%p rip=%#llx cs=%#llx rsp=%#llx rax=%#llx "
        "r14=%#llx wrong_to64=%u wrong_to32=%u failed=%u calls=%llu\n",
        sig, info->si_addr,
        (unsigned long long)uc->uc_mcontext->__ss.__rip,
        (unsigned long long)uc->uc_mcontext->__ss.__cs,
        (unsigned long long)uc->uc_mcontext->__ss.__rsp,
        (unsigned long long)uc->uc_mcontext->__ss.__rax,
        (unsigned long long)uc->uc_mcontext->__ss.__r14,
        counter(kWrongTo64), counter(kWrongTo32), counter(kFailed),
        (unsigned long long)atomic_load(&total_calls));
    write(2, buf, n);
    _exit(1);
}

/* i386 recovery tail: lock incl counter; ljmp $cs64:retry, or int3. */
static uint8_t *emit_recovery32(uint8_t *p, uint32_t count, uint32_t retry,
                                uint16_t cs64)
{
    p = emit(p, (uint8_t[]){0xf0, 0xff, 0x05}, 3);
    p = emit32(p, count);
    if (!retry) return emit8(p, 0xcc);
    p = emit8(p, 0xea);
    p = emit32(p, retry);
    return emit16(p, cs64);
}

static void emit_gateway_pad(uint8_t *p, uint64_t target64, uint32_t retry,
                             uint16_t cs64)
{
    if (guarded) {
        p = emit(p, (uint8_t[]){0x31, 0xc0, 0x40, 0x90, 0x85, 0xc0, 0x75, 0x0c}, 8);
    }
    p = emit(p, (uint8_t[]){0x48, 0xb8}, 2);
    p = emit64(p, target64);
    p = emit(p, (uint8_t[]){0xff, 0xe0}, 2);
    if (guarded) emit_recovery32(p, retry ? kWrongTo64 : kFailed, retry, cs64);
}

static void emit_return_pad(uint8_t *p, uint32_t retry, uint16_t cs64)
{
    static const uint8_t return64[] = {
        0x4c, 0x87, 0xf4,
        /* Only native Intel needs selector restoration. */
        0x83, 0x7c, 0x24, 0x0c, 0x00, 0x74, 0x0e,
        0x66, 0x8b, 0x4c, 0x24, 0x08, 0x8e, 0xd9,
        0x66, 0x8b, 0x4c, 0x24, 0x0a, 0x8e, 0xc1, 0x48, 0x83, 0xc4, 0x18, 0x41, 0x5f, 0x41, 0x5e,
        0x41, 0x5d, 0x41, 0x5c, 0x5b, 0x5d, 0xc3,
    };
    if (guarded) {
        p = emit(p, (uint8_t[]){0x31, 0xc9, 0x41, 0xff, 0xc1, 0x85, 0xc9, 0x75, sizeof(return64)}, 9);
    }
    p = emit(p, return64, sizeof(return64));
    if (guarded) emit_recovery32(p, retry ? kWrongTo64 : kFailed, retry, cs64);
}

static void emit_landing32(uint8_t *p, uint32_t retry)
{
    p = emit(p, (uint8_t[]){0x31, 0xc9, 0x41, 0xff, 0xc1, 0x85, 0xc9, 0x74, 0x01, 0xc3}, 10);
    /* x86_64 recovery: lock incl counter; movl $retry,(%r14); ljmp *(%r14) */
    p = emit(p, (uint8_t[]){0xf0, 0xff, 0x04, 0x25}, 4);
    p = emit32(p, retry ? kWrongTo32 : kFailed);
    if (!retry) {
        emit8(p, 0xcc);
        return;
    }
    p = emit(p, (uint8_t[]){0x41, 0xc7, 0x06}, 3);
    p = emit32(p, retry);
    emit(p, (uint8_t[]){0x41, 0xff, 0x2e}, 3);
}

/* mov $N,%esi; L: push $counter; push $1; call callee; add $8,%esp;
   dec %esi; jnz L; ljmp *exit_far */
static void emit_loop(uint8_t *p, uint32_t base, uint32_t callee)
{
    uint8_t *start = p;
    p = emit8(p, 0xbe);
    p = emit32(p, iterations);
    uint8_t *loop = p;
    p = emit8(p, 0x68);
    p = emit32(p, kCounter);
    p = emit(p, (uint8_t[]){0x6a, 0x01}, 2);
    p = emit8(p, 0xe8);
    p = emit32(p, callee - (base + (uint32_t)(p - start) + 4));
    p = emit(p, (uint8_t[]){0x83, 0xc4, 0x08}, 3);
    p = emit8(p, 0x4e);
    p = emit8(p, 0x75);
    *p = (uint8_t)(int8_t)(loop - (p + 1));
    ++p;
    p = emit(p, (uint8_t[]){0xff, 0x2d}, 2);
    emit32(p, kExitFar);
}

static void *worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    uint32_t esp = (uint32_t)(kStacks + (uintptr_t)tid * 0x100000) + 0x10000 - 64;
    uint32_t *guest_stack = (uint32_t *)(uintptr_t)esp;
    uint32_t loop = test32 ? kLoopTest : kLoop;
    while (!stop) {
        uint32_t eip;
        if (guarded) {
            guest_stack[0] = loop;
            guest_stack[1] = kExitThunk;
            eip = kLanding;
        } else {
            guest_stack[0] = kExitThunk;
            eip = loop;
        }
        run_compat32(eip, esp, stress_cs32);
        atomic_fetch_add(&total_calls, iterations);
    }
    return NULL;
}

/* Generate, execute and discard 8 MiB of unique x86_64 code per cycle. */
static void *churner(void *arg)
{
    (void)arg;
    const size_t size = 8u << 20;
    unsigned cycles = 0;
    while (!stop) {
        uint8_t *block = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
        if (block == MAP_FAILED) {
            perror("churn mmap");
            return NULL;
        }
        for (size_t i = 0; i < size / 16; ++i) {
            uint8_t *f = block + i * 16;
            f[0] = 0xb8;
            memcpy(f + 1, &i, 4);
            f[5] = 0xc3;
            memset(f + 6, 0xcc, 10);
        }
        if (mprotect(block, size, PROT_READ | PROT_EXEC)) {
            perror("churn mprotect");
            return NULL;
        }
        volatile uint32_t sink = 0;
        for (size_t i = 0; i < size / 16 && !stop; ++i) {
            sink += ((uint32_t (*)(void))(block + i * 16))();
        }
        (void)sink;
        munmap(block, size);
        ++cycles;
    }
    fprintf(stderr, "churn cycles: %u\n", cycles);
    return NULL;
}

int main(int argc, char **argv)
{
    int translated = 0;
    size_t translated_size = sizeof(translated);
    sysctlbyname("sysctl.proc_translated", &translated, &translated_size, NULL, 0);
    lp32_native_segments = !translated;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "guarded=", 8)) guarded = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "threads=", 8)) threads = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "seconds=", 8)) seconds = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "churn=", 6)) churn = atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "test32=", 7)) test32 = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "iters=", 6)) iterations = (uint32_t)strtoul(argv[i] + 6, NULL, 0);
    }
    if (threads < 1) threads = 1;
    if (threads > 60) threads = 60;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    ldt_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.code.limit00 = 0xffff;
    entry.code.type = DESC_CODE_READ;
    entry.code.dpl = 3;
    entry.code.present = 1;
    entry.code.limit16 = 0x0f;
    entry.code.opsz = DESC_CODE_32B;
    entry.code.granular = DESC_GRAN_PAGE;
    if (i386_set_ldt(32, &entry, 1) < 0) {
        perror("i386_set_ldt");
        return 1;
    }
    stress_cs32 = (32 << 3) | 7;
    stress_landing32 = kLanding;
    uint16_t cs64;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs64));

    uint8_t *code32 = map_low(0x1000, kCode32);
    uint8_t *code64 = map_low(0x1000, kCode64);
    uint8_t *data = map_low(0x1000, kData);
    map_low((size_t)threads * 0x100000, kStacks);
    memset(code32, 0xcc, 0x1000);
    memset(code64, 0xcc, 0x1000);

    struct far_ptr32 exit_far = {kReturnPad, cs64};
    struct far_ptr32 gateway_far = {kGatewayPad, cs64};
    memcpy(data + 0x00, &exit_far, sizeof(exit_far));
    memcpy(data + 0x10, &gateway_far, sizeof(gateway_far));

    uint64_t gateway = (uint64_t)(uintptr_t)(guarded ? stress_gateway_guarded
                                                     : stress_gateway_plain);
    emit_return_pad(code64 + 0x000, kReturnPadAlt, cs64);
    emit_gateway_pad(code64 + 0x040, gateway, kGatewayPadAlt, cs64);
    emit_return_pad(code64 + 0x080, 0, cs64);
    emit_gateway_pad(code64 + 0x0c0, gateway, 0, cs64);

    uint8_t *p = code32 + 0x000;                       /* exit thunk */
    p = emit(p, (uint8_t[]){0xff, 0x2d}, 2);
    emit32(p, kExitFar);
    p = code32 + 0x100;                                /* import thunk */
    p = emit8(p, 0x68);
    p = emit32(p, 7);
    p = emit(p, (uint8_t[]){0xff, 0x2d}, 2);
    emit32(p, kGatewayFar);
    p = code32 + 0x140;                                /* wrong-mode test stub */
    p = emit8(p, 0x68);
    p = emit32(p, 7);
    p = emit8(p, 0xe9);
    emit32(p, kGatewayPad - (kTestStub + 10));
    emit_loop(code32 + 0x200, kLoop, kThunk);
    emit_loop(code32 + 0x240, kLoopTest, kTestStub);
    emit_landing32(code32 + 0x300, kLandingAlt);
    emit_landing32(code32 + 0x340, 0);

    if (mprotect(code32, 0x1000, PROT_READ | PROT_EXEC) ||
        mprotect(code64, 0x1000, PROT_READ | PROT_EXEC)) {
        perror("mprotect");
        return 1;
    }

    printf("guarded=%d threads=%d seconds=%d churn=%d test32=%d iters=%u\n",
           guarded, threads, seconds, churn, test32, iterations);
    pthread_t tids[64], churn_tid;
    double t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    for (int t = 0; t < threads; ++t) {
        pthread_create(&tids[t], NULL, worker, (void *)(intptr_t)t);
    }
    if (churn) pthread_create(&churn_tid, NULL, churner, NULL);
    for (int s = 0; s < seconds; ++s) {
        sleep(1);
        printf("  t=%2ds calls=%llu wrong_to64=%u wrong_to32=%u\n", s + 1,
               (unsigned long long)atomic_load(&total_calls),
               counter(kWrongTo64), counter(kWrongTo32));
        fflush(stdout);
    }
    stop = 1;
    for (int t = 0; t < threads; ++t) pthread_join(tids[t], NULL);
    if (churn) pthread_join(churn_tid, NULL);
    double t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    uint64_t calls = atomic_load(&total_calls);
    printf("done: %llu round trips in %.1fs (%.1f ns/call/thread) counter=%u "
           "wrong_to64=%u wrong_to32=%u failed=%u\n",
           (unsigned long long)calls, t1 - t0,
           (t1 - t0) / ((double)calls / threads) * 1e9, counter(kCounter),
           counter(kWrongTo64), counter(kWrongTo32), counter(kFailed));
    return counter(kCounter) == calls ? 0 : 2;
}
