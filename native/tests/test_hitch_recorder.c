#include "../src/hitch_recorder.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static uint64_t end = 1000000000ULL, swap;
static void frame(uint64_t duration, uint64_t target, bool active)
{
    end += duration;
    hitch_frame(++swap, end - 2000000, end - 1000000, end, target, active);
}
static pthread_mutex_t rendezvous = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t startup_ready = PTHREAD_COND_INITIALIZER;
static int phase;
static void *presenter(void *unused);
int main(void)
{
    /* Keep the recorder in the same nanosecond domain as frame pacing. */
    for (unsigned i = 0; i < 1000; ++i) {
        uint64_t before = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t measured = hitch_now();
        uint64_t after = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        assert(before <= measured && measured <= after);
    }
    assert(hitch_classify("glDrawRangeElements") == HITCH_DRAW);
    assert(hitch_classify("_glDrawArrays") == HITCH_DRAW);
    assert(hitch_classify("_glDrawBuffers") == HITCH_GL_STATE);
    assert(hitch_classify("_glCompressedTexSubImage2DARB") == HITCH_UPLOAD);
    assert(hitch_classify("_glProgramStringARB") == HITCH_SHADER);
    assert(hitch_classify("_cgCreateProgram") == HITCH_SHADER);
    assert(hitch_classify("_fread") == HITCH_IO);
    assert(hitch_classify("_FSReadFork") == HITCH_IO);
    assert(hitch_classify("_PBReadForkAsync") == HITCH_IO);
    assert(hitch_classify("_MPWaitOnSemaphore") == HITCH_WAIT);
    assert(hitch_classify("_MPWaitOnQueue") == HITCH_WAIT);
    assert(hitch_classify("_MPDelayUntil") == HITCH_WAIT);
    assert(hitch_classify("_usleep") == HITCH_WAIT);
    assert(hitch_classify("_lp32_steam_4_0") == HITCH_IO);
    assert(hitch_classify("_lp32_steam_4_16") == HITCH_IO);
    assert(hitch_classify("_lp32_steam_1_0") == HITCH_RUNTIME);
    assert(hitch_classify("_pthread_cond_wait$UNIX2003") == HITCH_WAIT);
    assert(hitch_classify("_glProgramEnvParameters4fvEXT") == HITCH_GL_STATE);
    assert(hitch_classify("_objc_msgSend") == HITCH_OBJC);
    assert(hitch_classify("_AudioUnitRender") == HITCH_AUDIO);
    assert(hitch_classify("_AudioQueueNewOutput") == HITCH_AUDIO);
    assert(hitch_classify("_AudioQueueDispose") == HITCH_AUDIO);
    assert(hitch_classify("_strlen") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("___tolower") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("_memcpy") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("_pthread_self") == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("_malloc") == HITCH_RUNTIME);
    assert(hitch_classify("_free") == HITCH_RUNTIME);
    const char *const cheap_helpers[] = {
        "_pthread_getspecific", "_logf", "_expf", "_floorf", "_ceilf", "_acosf",
        "_asinf", "_sinf", "_cosf", "_atanf", "_roundf", "_powf", "_atan2f", "_fmodf",
        "_floor", "_ceil", "_sin", "_cos", "_pow", "_atan2", "_fmod",
        "___udivdi3", "___umoddi3", "___divdi3", "___moddi3",
        "_OSAtomicCompareAndSwap32", "_OSAtomicCompareAndSwapInt",
        "_OSAtomicCompareAndSwap32Barrier", "_OSAtomicCompareAndSwapPtrBarrier",
        "_OSAtomicCompareAndSwap64", "_OSAtomicCompareAndSwap64Barrier",
        "_OSAtomicAdd64", "_pthread_mutex_trylock",
    };
    for (unsigned i = 0; i < sizeof(cheap_helpers) / sizeof(cheap_helpers[0]); ++i)
        assert(hitch_classify(cheap_helpers[i]) == HITCH_COUNTED_RUNTIME);
    assert(hitch_classify("__tlv_get_addr") == HITCH_RUNTIME);
    assert(hitch_classify("_pthread_once") == HITCH_RUNTIME);
    assert(hitch_classify("_pthread_mutex_unlock") == HITCH_RUNTIME);
    assert(hitch_classify("_nanosleep$UNIX2003") == HITCH_WAIT);
    assert(hitch_classify("_usleep$UNIX2003") == HITCH_WAIT);
    assert(hitch_classify("_OSAtomicAdd32Barrier") == HITCH_COUNTED_RUNTIME);
    assert(setenv("LP32_TRACE_ATOMIC", "0x1234", 1) == 0);
    assert(hitch_classify("_OSAtomicAdd32Barrier") == HITCH_RUNTIME);
    assert(unsetenv("LP32_TRACE_ATOMIC") == 0);
    assert(hitch_classify("_mach_absolute_time") == HITCH_COUNTED_RUNTIME);
    assert(setenv("LP32_TRACE_TIMING", "1", 1) == 0);
    assert(hitch_classify("_mach_absolute_time") == HITCH_RUNTIME);
    assert(unsetenv("LP32_TRACE_TIMING") == 0);
    assert(!hitch_recorder_enabled);
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lp32-hitch-test-%ld.log", (long)getpid());
    assert(!hitch_start(path, 25));
    const char *full = getenv("LP32_HITCH_FULL_IMPORTS");
    assert(!!hitch_full_imports == !!(full && full[0] && strcmp(full, "0")));
    assert(hitch_classify("_strlen") == (hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME));
    for (unsigned i = 0; i < sizeof(cheap_helpers) / sizeof(cheap_helpers[0]); ++i)
        assert(hitch_classify(cheap_helpers[i]) ==
               (hitch_full_imports ? HITCH_RUNTIME : HITCH_COUNTED_RUNTIME));
    assert(hitch_classify("_pthread_mutex_lock") == HITCH_WAIT);
    /* Startup itself presents once before Backend takes over. Its later
       imports and a scope spanning that handoff must not pollute Backend. */
    hitch_frame(0, end - 2000000, end - 1000000, end, 16666667, false);
    struct hitch_scope transferred;
    hitch_scope_begin(&transferred, end);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, presenter, NULL));
    pthread_mutex_lock(&rendezvous);
    while (phase != 1) pthread_cond_wait(&startup_ready, &rendezvous);
    /* The startup thread continues to make imports while Backend presents.
       It must never write Backend's current frame or runtime counters. */
    /* This earlier frame arrives after Backend's newer presentation. It
       must not reclaim ownership or underflow the shared frame clock. */
    struct hitch_scope late;
    hitch_scope_begin(&late, end);
    hitch_frame(999999, end - 5000000, end - 4000000, end - 3000000, 16666667, true);
    hitch_scope_end(&late, HITCH_SHADER, "late-presentation-scope", 0, end + 90000000, 0);
    hitch_scope_end(&transferred, HITCH_SHADER, "former-presenter-scope", 0, end + 90000000, 0);
    hitch_note(HITCH_WAIT, "former-presenter-wait", 0, end, end + 90000000, 0);
    hitch_note(HITCH_SHADER, "cgCreateProgram", 0x1234, end, end + 9000000, 0);
    hitch_note(HITCH_AUDIO, "AudioQueueNewOutput", 0x5678, end, end + 3000000, 0);
    hitch_count_runtime();
    phase = 2;
    pthread_cond_broadcast(&startup_ready);
    pthread_mutex_unlock(&rendezvous);
    pthread_join(thread, NULL);
    hitch_stop();
    FILE *f = fopen(path, "r"); assert(f);
    char text[65536]; size_t count = fread(text, 1, sizeof(text) - 1, f);
    text[count] = 0; fclose(f);
    assert(strstr(text, "hitch trigger=131 frames=41"));
    assert(strstr(text, " frame=99 "));
    assert(!strstr(text, " frame=98 "));
    assert(strstr(text, " frame=139 "));
    assert(strstr(text, "present=45.000 work=43.000 flush=1.000 pace=1.000"));
    assert(strstr(text, "draw=1/40.000"));
    assert(strstr(text, " counted_runtime=17\n"));
    assert(!strstr(text, " counted_runtime=18\n"));
    assert(strstr(text, hitch_full_imports ?
                  "glstate=65/70.000 objc=0/0.000 runtime=1/4.000" :
                  "glstate=65/8.000 objc=0/0.000 runtime=1/4.000"));
    assert(strstr(text, hitch_full_imports ?
                  "glstate_sampled_calls=65 glstate_sampled_ms=70.000" :
                  "glstate_sampled_calls=3 glstate_sampled_ms=8.000"));
    assert(strstr(text, hitch_full_imports ? "glstate_every=1;" : "glstate_every=32;"));
    assert(!strstr(text, "objc=1/"));
    assert(!strstr(text, "name=presentation"));
    assert(strstr(text, "host model="));
    assert(strstr(text, "rosetta="));
    assert(strstr(text, "caller=00abcdef vp=17 fp=28 count=600"));
    assert(strstr(text, "worker name=cgCreateProgram"));
    assert(strstr(text, "worker name=AudioQueueNewOutput"));
    assert(!strstr(text, "name=first-presentation"));
    assert(!strstr(text, "former-presenter-scope"));
    assert(!strstr(text, "late-presentation-scope"));
    assert(!strstr(text, "frame=999999"));
    assert(!strstr(text, "former-presenter-wait"));
    assert(!strstr(text, "shader=1/9.000"));
    assert(!strstr(text, "audio=1/3.000"));
    assert(strstr(text, "end reports=1 skipped=0"));
    unlink(path);
    /* Reusing the recorder in-process starts with a fresh writer and history. */
    assert(!hitch_start(path,25));
    hitch_frame(1,1000,2000,3000,16666667,true);
    hitch_stop();
    f = fopen(path,"r"); assert(f);
    count = fread(text,1,sizeof(text)-1,f); text[count] = 0; fclose(f);
    assert(strstr(text,"end reports=0 skipped=0"));
    assert(!strstr(text,"hitch trigger="));
    unlink(path);
    puts("hitch-recorder PASS (threshold, history, workers, cooldown, exclusive nesting, presentation boundaries, pthread presentation ownership, startup imports, host metadata)");
    return 0;
}

static void *presenter(void *unused)
{
    (void)unused;
    /* The first presentation also invalidates scopes begun before ownership. */
    struct hitch_scope first;
    hitch_scope_begin(&first, end);
    /* Warm-up, ring wrap, intentional 30 FPS, and inactive frames are quiet. */
    frame(16666667, 16666667, true);
    hitch_scope_end(&first, HITCH_OBJC, "first-presentation", 0, end, 0);
    for (int i = 1; i < 50; ++i) frame(16666667, 16666667, true);
    for (int i = 0; i < 40; ++i) frame(33333333, 33333333, true);
    for (int i = 0; i < 40; ++i) frame(50000000, 16666667, false);
    pthread_mutex_lock(&rendezvous);
    phase = 1;
    pthread_cond_broadcast(&startup_ready);
    while (phase != 2) pthread_cond_wait(&startup_ready, &rendezvous);
    pthread_mutex_unlock(&rendezvous);
    for (int i = 0; i < 17; ++i) hitch_count_runtime();
    struct hitch_scope outer, inner, presentation;
    assert(hitch_sample_gl_state());
    hitch_scope_begin(&outer, end);
    outer.counted = true;
    hitch_scope_begin(&inner, end + 2000000);
    hitch_scope_end(&inner, HITCH_RUNTIME, "nested", 0, end + 6000000, 0);
    hitch_scope_end(&outer, HITCH_GL_STATE, "outer", 0, end + 10000000, 0);
    /* State counts include skipped samples, while durations include only
       sampled scopes. The outer scope above still excludes its nested child. */
    unsigned sampled = 0;
    for (unsigned i = 0; i < 64; ++i) {
        if (hitch_sample_gl_state()) {
            struct hitch_scope state;
            hitch_scope_begin(&state, end);
            state.counted = true;
            hitch_scope_end(&state, HITCH_GL_STATE, "sampled-state", 0,
                            end + 1000000, 0);
            ++sampled;
        }
    }
    assert(sampled == (hitch_full_imports ? 64 : 2));
    hitch_program(0x8620, 17); hitch_program(0x8804, 28);
    hitch_note(HITCH_DRAW, "glDrawRangeElements", 0xabcdef, end, end + 40000000, 600);
    hitch_scope_begin(&presentation, end);
    frame(45000000, 16666667, true);
    hitch_scope_end(&presentation, HITCH_OBJC, "presentation", 0, end, 0);
    for (int i = 0; i < 8; ++i) frame(16666667, 16666667, true);
    /* Another hitch within five seconds must not generate another report. */
    frame(45000000, 16666667, true);
    usleep(20000);
    /* Benchmark selected-call timing without driver work or per-call logging. */
    uint64_t before = hitch_now();
    for (int i = 0; i < 100000; ++i) {
        uint64_t start = hitch_now();
        hitch_note(HITCH_DRAW, "glDrawArrays", 0, start, hitch_now(), 3);
    }
    printf("recorder selected-call overhead: %.0f ns/call\n", (hitch_now() - before) / 100000.0);
    before = hitch_now();
    for (int i = 0; i < 100000; ++i) {
        struct hitch_scope scope;
        hitch_scope_begin(&scope, hitch_now());
        hitch_scope_end(&scope, HITCH_RUNTIME, "benchmark", 0, hitch_now(), 0);
    }
    printf("recorder exclusive-call overhead: %.0f ns/call\n", (hitch_now() - before) / 100000.0);
    before = hitch_now();
    unsigned samples = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        if (hitch_sample_gl_state()) {
            struct hitch_scope state;
            hitch_scope_begin(&state, hitch_now());
            state.counted = true;
            hitch_scope_end(&state, HITCH_GL_STATE, "gl-state-benchmark", 0,
                            hitch_now(), 0);
            ++samples;
        }
    }
    printf("recorder GL state overhead: %.0f ns/call, %u/100000 timed\n",
           (hitch_now() - before) / 100000.0, samples);
    return NULL;
}
