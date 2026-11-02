#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/resource.h>
#include <math.h>
#include <stdarg.h>
#include <wchar.h>
#include <locale.h>
#include <stdlib.h>

extern uint64_t UpTime(void);
extern uint64_t AbsoluteToNanoseconds(uint64_t);
extern uint64_t NanosecondsToAbsolute(uint64_t);
extern int finite(double);
extern void GetGlobalMouse(int16_t *);
extern uint32_t CGMainDisplayID(void);
extern int CGDisplayHideCursor(uint32_t);
extern int CGDisplayShowCursor(uint32_t);
extern int CGCursorIsVisible(void);

static __attribute__((noinline)) void unwind(jmp_buf state, int value, int depth)
{
    volatile uint32_t frame[32];
    frame[depth] = 0x12345678;
    if (depth) unwind(state, value, depth - 1);
    longjmp(state, value + (frame[depth] != 0x12345678));
}

static int format_wide(wchar_t *output, size_t size, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vswprintf(output, size, format, args);
    va_end(args);
    return result;
}

/* A Source rendering frame can exceed the bridge's former 64 KiB
   callback stride. Neither comparator entry may overwrite suspended locals. */
static __attribute__((noinline)) int compare_stack_int(const void *a, const void *b)
{
    volatile uint32_t scratch[64];
    for (unsigned i = 0; i < 64; ++i) scratch[i] = 0xabc00000u + i;
    int result = (*(const int *)a > *(const int *)b) -
                 (*(const int *)a < *(const int *)b);
    for (unsigned i = 0; i < 64; ++i)
        if (scratch[i] != 0xabc00000u + i) abort();
    return result;
}

static __attribute__((noinline)) int compare_nested_stack_int(const void *a, const void *b)
{
    volatile uint32_t scratch[64];
    for (unsigned i = 0; i < 64; ++i) scratch[i] = 0xdef00000u + i;
    const int search[] = {1, 3, 9}, key = 3;
    const int *found = bsearch(&key, search, 3, sizeof(int), compare_stack_int);
    if (!found || *found != key) abort();
    for (unsigned i = 0; i < 64; ++i)
        if (scratch[i] != 0xdef00000u + i) abort();
    return compare_stack_int(a, b);
}

static __attribute__((noinline)) int check_deep_callback_stack(void)
{
    volatile unsigned char suspended[96 * 1024];
    for (unsigned i = 0; i < sizeof(suspended); ++i)
        suspended[i] = (unsigned char)(i * 17u + 3u);
    int values[] = {9, 1, 7, 3};
    qsort(values, 4, sizeof(int), compare_nested_stack_int);
    for (unsigned i = 0; i < sizeof(suspended); ++i)
        if (suspended[i] != (unsigned char)(i * 17u + 3u)) return -234;
    if (values[0] != 1 || values[1] != 3 || values[2] != 7 || values[3] != 9)
        return -235;
    return 0;
}

int check_context(void)
{
    int stack_error = check_deep_callback_stack();
    if (stack_error) return stack_error;
    /* The guest must observe its own balanced cursor state even when the
       test process is not the foreground application. Nested users must not
       reveal each other's cursor, and unmatched shows must not underflow. */
    uint32_t display = CGMainDisplayID();
    int cursor_error = !CGCursorIsVisible();
    cursor_error |= CGDisplayHideCursor(display);
    cursor_error |= CGCursorIsVisible();
    cursor_error |= CGDisplayHideCursor(display);
    cursor_error |= CGCursorIsVisible();
    cursor_error |= CGDisplayShowCursor(display);
    cursor_error |= CGCursorIsVisible();
    cursor_error |= CGDisplayShowCursor(display);
    cursor_error |= !CGCursorIsVisible();
    cursor_error |= CGDisplayShowCursor(display);
    cursor_error |= !CGCursorIsVisible();
    if (cursor_error) return -233;
    struct { uint32_t before; int16_t point[2]; uint32_t after; } mouse =
        {0x12345678, {0, 0}, 0x87654321};
    GetGlobalMouse(mouse.point);
    if (mouse.before != 0x12345678 || mouse.after != 0x87654321) return -232;
    if (!setlocale(LC_CTYPE, "en_US.UTF-8")) return -231;
    struct { wchar_t text[128]; uint32_t guard; } formatted = {{0}, 0xdeadbeef};
    struct { long count; uint32_t guard; } written = {0, 0x12345678};
    const wchar_t *expected = L"Ω ok -7 1099511627776 1.25 9";
    int length = format_wide(formatted.text, 128, L"%ls %s %ld %lld %.*f %zu%ln",
        L"Ω", "ok", -7L, 1099511627776LL, 2, 1.25, (size_t)9, &written.count);
    if (length != 28 || written.count != 28 || wcscmp(formatted.text, expected) ||
        formatted.guard != 0xdeadbeef || written.guard != 0x12345678) return -229;
    struct { wchar_t text[3]; uint32_t guard; } small = {{0}, 0x87654321};
    if (swprintf(small.text, 3, L"%ls", L"abcd") >= 0 || small.guard != 0x87654321) return -230;
    setlocale(LC_CTYPE, "C");
    volatile float fraction = 0.5f, whole = 100000.0f;
    float angle = asinf(fraction), slope = atanf(fraction);
    if (angle < 0.5235f || angle > 0.5237f || slope < 0.4635f || slope > 0.4638f ||
        (uint64_t)whole != 100000 || !finite(1.25) || finite(INFINITY) || finite(NAN)) return -228;
    struct { struct rusage usage; uint32_t guard; } resources = {{0}, 0x98765432};
    if (sizeof(resources.usage) != 72 || getrusage(RUSAGE_SELF, &resources.usage) ||
        resources.guard != 0x98765432 || resources.usage.ru_maxrss <= 0 ||
        resources.usage.ru_utime.tv_usec < 0 || resources.usage.ru_utime.tv_usec >= 1000000) return -227;
    uint64_t start = UpTime();
    usleep(1000);
    uint64_t elapsed = UpTime() - start;
    uint64_t ns = AbsoluteToNanoseconds(elapsed);
    uint64_t ticks = NanosecondsToAbsolute(ns);
    if (ns < 100000 || ticks > elapsed || elapsed - ticks > 1) return -225;
    volatile int64_t dividend = -(INT64_C(1) << 50) + 7, divisor = 7;
    if (dividend / divisor != -INT64_C(160842843834659) || dividend % divisor != -4) return -226;
    struct { jmp_buf state; uint32_t guard; } saved;
    saved.guard = 0xabc123;
    volatile int count = 0;
    int value = setjmp(saved.state);
    if (!value) { ++count; unwind(saved.state, 0, 10); }
    if (value != 1 || count != 1 || saved.guard != 0xabc123) return -220;
    value = setjmp(saved.state);
    if (!value) unwind(saved.state, 42, 6);
    if (value != 42) return -221;

    sigset_t original, blocked = 1u << (SIGUSR1 - 1), current;
    if (sigprocmask(SIG_BLOCK, &blocked, &original)) return -222;
    sigjmp_buf signal_state;
    value = sigsetjmp(signal_state, 1);
    if (!value) {
        sigprocmask(SIG_UNBLOCK, &blocked, 0);
        siglongjmp(signal_state, 7);
    }
    sigprocmask(SIG_SETMASK, 0, &current);
    if (value != 7 || !(current & blocked)) return -223;
    value = _setjmp(saved.state);
    if (!value) { sigprocmask(SIG_UNBLOCK, &blocked, 0); _longjmp(saved.state, 8); }
    sigprocmask(SIG_SETMASK, 0, &current);
    sigprocmask(SIG_SETMASK, &original, 0);
    if (value != 8 || (current & blocked)) return -224;
    return 0;
}
