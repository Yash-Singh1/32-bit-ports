/* Real AudioToolbox conversions with a small, guarded i386 address arena.
   Only guest allocation and callback entry are stubbed; no decoder is faked. */
#include "audio_converter_bridge.h"
#include "compat_runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
enum { ARENA_SIZE = 8 * 1024 * 1024, CALLBACK_ID = 0x13579, MAX_ALLOCATIONS = 512 };
static const uint32_t guard = 0xa1b2c3d4;
struct allocation { uint32_t address; size_t size; bool freed; };
static struct allocation allocations[MAX_ALLOCATIONS];
static unsigned allocation_count, owned_count, free_count;
static unsigned char *arena;
static size_t arena_used;
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int callback_trapped;

uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    pthread_mutex_lock(&allocation_lock);
    size_t start = (arena_used + 15) & ~(size_t)15;
    CHECK(allocation_count < MAX_ALLOCATIONS && size < ARENA_SIZE - start - 32);
    unsigned char *p = arena + start + 16;
    memcpy(p - 4, &guard, 4);
    memcpy(p + size, &guard, 4);
    memset(p, clear ? 0 : 0xa5, size);
    CHECK((uintptr_t)p + size <= UINT32_MAX);
    uint32_t result = (uint32_t)(uintptr_t)p;
    allocations[allocation_count++] = (struct allocation){result, size, false};
    arena_used = start + 16 + size + 4;
    pthread_mutex_unlock(&allocation_lock);
    return result;
}

void compat_runtime32_deallocate(uint32_t address)
{
    if (!address) return;
    pthread_mutex_lock(&allocation_lock);
    unsigned i;
    for (i = 0; i < allocation_count; ++i)
        if (allocations[i].address == address) break;
    CHECK(i < allocation_count && !allocations[i].freed);
    allocations[i].freed = true;
    memset((void *)(uintptr_t)address, 0xdd, allocations[i].size);
    ++free_count;
    pthread_mutex_unlock(&allocation_lock);
}

static void *low_alloc(size_t size)
{
    uint32_t address = compat_runtime32_allocate(size, 1);
    ++owned_count;
    return (void *)(uintptr_t)address;
}

static uint32_t guest(const void *p)
{
    CHECK((uintptr_t)p <= UINT32_MAX);
    return (uint32_t)(uintptr_t)p;
}

static int32_t dispatch(const char *name, const uint32_t *a)
{
    uint64_t result = UINT64_MAX;
    CHECK(audio_converter_bridge32_dispatch(name, a, &result) == 1);
    CHECK(result <= UINT32_MAX);
    return (int32_t)result;
}

struct buffer32 { uint32_t channels, size, data; };
struct list32 { uint32_t count; struct buffer32 buffers[2]; };
_Static_assert(sizeof(AudioStreamBasicDescription) == 40, "ASBD ABI");
_Static_assert(sizeof(struct buffer32) == 12, "i386 AudioBuffer ABI");
enum callback_mode { NORMAL, ERROR, TRAP, OVERPACKETS, NULL_DATA, PREFILLED_INPUT,
                     EXTRA_PACKETS, REENTRANT_DISPOSE, CROSS_CONVERTER, BLOCK };
struct callback_state {
    uint32_t converter, samples[2], frames, position, channels, bytes_per_packet;
    uint32_t other_converter;
    unsigned calls;
    enum callback_mode mode;
    int32_t error;
    bool planar;
};
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool entered, unblock, dispose_started, dispose_finished;
} gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false, false};

uint32_t compat_runtime32_call(uint32_t function, const uint32_t *a, size_t count)
{
    CHECK(function == CALLBACK_ID && count == 5 && a[1] && a[2] && a[4]);
    callback_trapped = 0;
    struct callback_state *s = (void *)(uintptr_t)a[4];
    uint32_t *packets = (void *)(uintptr_t)a[1];
    struct list32 *list = (void *)(uintptr_t)a[2];
    CHECK(a[0] == s->converter);
    CHECK(list->count == (s->planar ? s->channels : 1));
    for (unsigned i = 0; i < list->count; ++i)
        CHECK(list->buffers[i].channels == (s->planar ? 1 : s->channels));
    if (a[3]) CHECK(*(uint32_t *)(uintptr_t)a[3] == 0);
    ++s->calls;
    if (s->mode == REENTRANT_DISPOSE) {
        const uint32_t dispose[] = {a[0]};
        CHECK(dispatch("_AudioConverterDispose", dispose) == kAudio_ParamError);
    }
    if (s->mode == CROSS_CONVERTER) {
        const uint32_t dispose[] = {s->other_converter};
        const uint32_t property[] = {s->other_converter, kAudioConverterPrimeMethod, 4, a[1]};
        const uint32_t nested_fill[] = {s->other_converter, CALLBACK_ID, a[4], a[1], a[2], 0};
        CHECK(dispatch("_AudioConverterDispose", dispose) == kAudio_ParamError);
        CHECK(dispatch("_AudioConverterSetProperty", property) == kAudio_ParamError);
        CHECK(dispatch("_AudioConverterFillComplexBuffer", nested_fill) == kAudio_ParamError);
    }
    if (s->mode == BLOCK) {
        pthread_mutex_lock(&gate.lock);
        gate.entered = true;
        pthread_cond_broadcast(&gate.changed);
        while (!gate.unblock) pthread_cond_wait(&gate.changed, &gate.lock);
        pthread_mutex_unlock(&gate.lock);
    }
    uint32_t available = s->frames - s->position;
    uint32_t n = *packets < available ? *packets : available;
    if (s->mode == ERROR || s->mode == TRAP) n = 0;
    if (s->mode == EXTRA_PACKETS) n = available;
    for (unsigned i = 0; i < list->count; ++i) {
        /* Source's input callback leaves the provided count/channels alone. */
        if (s->mode == PREFILLED_INPUT && n) {
            CHECK(list->buffers[i].data && list->buffers[i].size >= n * s->bytes_per_packet);
            memcpy((void *)(uintptr_t)list->buffers[i].data,
                   (void *)(uintptr_t)(s->samples[i] + s->position * s->bytes_per_packet),
                   n * s->bytes_per_packet);
        } else list->buffers[i].data = n ? s->samples[i] + s->position * s->bytes_per_packet : 0;
        list->buffers[i].size = n * s->bytes_per_packet;
    }
    s->position += n;
    if (s->mode == OVERPACKETS) { ++*packets; return 0; }
    *packets = n;
    if (s->mode == NULL_DATA) list->buffers[0].data = 0;
    if (s->mode == TRAP) callback_trapped = 1;
    if (s->mode == ERROR) return (uint32_t)s->error;
    return n ? 0 : (uint32_t)-1;
}

int compat_runtime32_last_call_trapped(void) { return callback_trapped; }

static AudioStreamBasicDescription pcm(double rate, unsigned channels, bool floating,
                                       bool planar)
{
    unsigned bytes = floating ? sizeof(float) : sizeof(int16_t);
    return (AudioStreamBasicDescription){rate, kAudioFormatLinearPCM,
        (floating ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
        kAudioFormatFlagIsPacked | (planar ? kAudioFormatFlagIsNonInterleaved : 0),
        bytes * (planar ? 1 : channels), 1, bytes * (planar ? 1 : channels),
        channels, bytes * 8, 0};
}

static uint32_t create(AudioStreamBasicDescription input, AudioStreamBasicDescription output)
{
    AudioStreamBasicDescription *in = low_alloc(sizeof(*in)), *out = low_alloc(sizeof(*out));
    *in = input; *out = output;
    struct { uint32_t value, guard; } *handle = low_alloc(sizeof(*handle));
    handle->guard = guard;
    const uint32_t args[] = {guest(in), guest(out), guest(handle)};
    CHECK(dispatch("_AudioConverterNew", args) == noErr);
    CHECK(handle->value && handle->guard == guard);
    CHECK(!memcmp(in, &input, sizeof(input)) && !memcmp(out, &output, sizeof(output)));
    return handle->value;
}

static void dispose(uint32_t converter)
{
    const uint32_t args[] = {converter};
    CHECK(dispatch("_AudioConverterDispose", args) == noErr);
    CHECK(dispatch("_AudioConverterDispose", args) == kAudio_ParamError);
}

static int32_t set_property(uint32_t converter, uint32_t property, uint32_t size,
                            uint32_t value)
{
    uint32_t *data = low_alloc(4);
    *data = value;
    const uint32_t args[] = {converter, property, size, guest(data)};
    return dispatch("_AudioConverterSetProperty", args);
}

struct fill_state {
    uint32_t packets, packet_guard;
    struct list32 output;
    uint32_t list_guard;
    struct callback_state input;
};

static struct fill_state *prepare_fill(uint32_t converter, unsigned frames,
                                       unsigned channels, bool planar)
{
    struct fill_state *f = low_alloc(sizeof(*f));
    f->packets = frames; f->packet_guard = f->list_guard = guard;
    f->output.count = planar ? channels : 1;
    f->input = (struct callback_state){.converter = converter, .frames = frames,
        .channels = channels, .bytes_per_packet = sizeof(float) * (planar ? 1 : channels),
        .planar = planar};
    for (unsigned i = 0; i < f->output.count; ++i) {
        f->input.samples[i] = guest(low_alloc(frames * f->input.bytes_per_packet));
        f->output.buffers[i] = (struct buffer32){planar ? 1 : channels,
            frames * (uint32_t)sizeof(int16_t) * (planar ? 1 : channels),
            guest(low_alloc(frames * sizeof(int16_t) * (planar ? 1 : channels)))};
    }
    return f;
}

static int32_t fill(struct fill_state *f, uint32_t descriptions)
{
    const uint32_t args[] = {f->input.converter, CALLBACK_ID, guest(&f->input),
        guest(&f->packets), guest(&f->output), descriptions};
    return dispatch("_AudioConverterFillComplexBuffer", args);
}

static void check_guards(const struct fill_state *f)
{
    CHECK(f->packet_guard == guard && f->list_guard == guard);
    for (unsigned i = 0; i < allocation_count; ++i) {
        uint32_t before, after;
        const unsigned char *p = (void *)(uintptr_t)allocations[i].address;
        memcpy(&before, p - 4, 4); memcpy(&after, p + allocations[i].size, 4);
        CHECK(before == guard && after == guard);
    }
}

static void exact_pcm(bool planar, bool provided_input)
{
    unsigned channels = planar ? 2 : 1;
    uint32_t c = create(pcm(48000, channels, true, planar), pcm(48000, channels, false, planar));
    /* A format-only native converter need not expose resampler properties. */
    CHECK(set_property(c, kAudioConverterPrimeMethod, 8, 2) == kAudioConverterErr_BadPropertySizeError);
    CHECK(set_property(c, kAudioConverterDecompressionMagicCookie, 4, 0) == kAudioConverterErr_PropertyNotSupported);
    struct fill_state *f = prepare_fill(c, 8, channels, planar);
    if (provided_input) f->input.mode = PREFILLED_INPUT;
    for (unsigned channel = 0; channel < f->output.count; ++channel) {
        float *in = (void *)(uintptr_t)f->input.samples[channel];
        for (unsigned i = 0; i < 8; ++i) in[i] = ((int)i - 4) / 4.0f;
    }
    struct list32 original = f->output;
    struct { uint32_t value, guard; } *descriptions = low_alloc(sizeof(*descriptions));
    descriptions->value = 0x11223344; descriptions->guard = guard;
    CHECK(fill(f, guest(descriptions)) == kAudioConverterErr_OperationNotSupported);
    CHECK(f->packets == 8 && !memcmp(&f->output, &original, sizeof(original)) &&
          descriptions->value == 0x11223344 && descriptions->guard == guard && !f->input.calls);
    int32_t converted = fill(f, 0);
    if (converted) fprintf(stderr, "PCM conversion returned %d; callbacks=%u packets=%u\n",
                           converted, f->input.calls, f->packets);
    CHECK(converted == noErr && f->packets == 8 && f->input.calls);
    CHECK(f->output.count == original.count);
    for (unsigned channel = 0; channel < f->output.count; ++channel) {
        CHECK(f->output.buffers[channel].channels == original.buffers[channel].channels &&
              f->output.buffers[channel].size == 16 &&
              f->output.buffers[channel].data == original.buffers[channel].data);
        const int16_t *out = (void *)(uintptr_t)f->output.buffers[channel].data;
        for (unsigned i = 0; i < 8; ++i) CHECK(out[i] == ((int)i - 4) * 8192);
    }
    /* EOF is the status returned by L4D2's callback, not manufactured success. */
    f->packets = 8;
    for (unsigned i = 0; i < f->output.count; ++i) f->output.buffers[i].size = 16;
    CHECK(fill(f, 0) == -1 && f->packets == 0);
    check_guards(f);
    dispose(c);
    uint32_t replacement = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
    CHECK(replacement != c);
    CHECK(set_property(c, kAudioConverterPrimeMethod, 4, 2) == kAudio_ParamError);
    dispose(replacement);
}

static void resample(void)
{
    uint32_t c = create(pcm(48000, 1, true, false), pcm(11025, 1, false, false));
    CHECK(set_property(c, kAudioConverterPrimeMethod, 4, kConverterPrimeMethod_None) == noErr);
    CHECK(set_property(c, kAudioConverterSampleRateConverterQuality, 4, 64) == noErr);
    struct fill_state *f = prepare_fill(c, 4096, 1, false);
    float *in = (void *)(uintptr_t)f->input.samples[0];
    for (unsigned i = 0; i < 4096; ++i) in[i] = (float)(0.5 * sin(i * 2 * M_PI * 440 / 48000));
    f->packets = 512; f->output.buffers[0].size = 512 * sizeof(int16_t);
    uint32_t data = f->output.buffers[0].data;
    CHECK(fill(f, 0) == noErr && f->packets == 512 && f->input.calls);
    CHECK(f->output.buffers[0].data == data && f->output.buffers[0].size == 1024);
    const int16_t *out = (void *)(uintptr_t)data;
    double sum = 0;
    unsigned crossings = 0;
    for (unsigned i = 32; i < 512; ++i) {
        double value = out[i] / 32768.0;
        sum += value * value;
        if (out[i - 1] <= 0 && out[i] > 0) ++crossings;
    }
    CHECK(sqrt(sum / 480) > 0.30 && sqrt(sum / 480) < 0.40);
    CHECK(crossings >= 18 && crossings <= 20);
    check_guards(f);
    dispose(c);
}

static void callback_failures(void)
{
    const enum callback_mode modes[] = {ERROR, TRAP, OVERPACKETS, NULL_DATA, REENTRANT_DISPOSE};
    const int32_t statuses[] = {-12345, kAudioConverterErr_UnspecifiedError,
        kAudioConverterErr_InvalidInputSize, kAudioConverterErr_InvalidInputSize, noErr};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        uint32_t c = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
        struct fill_state *f = prepare_fill(c, 8, 1, false);
        f->input.mode = modes[i]; f->input.error = -12345;
        CHECK(fill(f, 0) == statuses[i] && f->input.calls);
        check_guards(f);
        dispose(c);
    }
}

static void extra_packets(void)
{
    uint32_t c = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
    struct fill_state *f = prepare_fill(c, 16, 1, false);
    f->input.mode = EXTRA_PACKETS;
    float *in = (void *)(uintptr_t)f->input.samples[0];
    for (unsigned i = 0; i < 16; ++i) in[i] = ((int)i - 8) / 16.0f;
    /* The callback request is a minimum: delivering a larger valid packet
       batch is permitted, and the converter retains the unconsumed input. */
    f->packets = 8; f->output.buffers[0].size = 16;
    CHECK(fill(f, 0) == noErr && f->packets == 8 && f->input.position == 16);
    const int16_t *out = (void *)(uintptr_t)f->output.buffers[0].data;
    for (unsigned i = 0; i < 8; ++i) CHECK(out[i] == ((int)i - 8) * 2048);
    unsigned calls = f->input.calls;
    f->packets = 8; f->output.buffers[0].size = 16;
    CHECK(fill(f, 0) == noErr && f->packets == 8 && f->input.calls == calls);
    for (unsigned i = 0; i < 8; ++i) CHECK(out[i] == (int)i * 2048);
    check_guards(f);
    dispose(c);
}

static void cross_converter_reentry(void)
{
    uint32_t a = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
    uint32_t b = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
    struct fill_state *first = prepare_fill(a, 8, 1, false);
    first->input.mode = CROSS_CONVERTER;
    first->input.other_converter = b;
    CHECK(fill(first, 0) == noErr && first->input.calls);
    /* Callback rejection must not retire B or prevent ordinary operations. */
    CHECK(set_property(b, kAudioConverterPrimeMethod, 8, 2) == kAudioConverterErr_BadPropertySizeError);
    struct fill_state *second = prepare_fill(b, 8, 1, false);
    CHECK(fill(second, 0) == noErr && second->input.calls);
    check_guards(first); check_guards(second);
    dispose(a); dispose(b);
}

static void invalid_output_range(void)
{
    uint32_t c = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
    struct fill_state *f = prepare_fill(c, 8, 1, false);
    f->output.buffers[0].data = 0xfffffff0u;
    f->output.buffers[0].size = 32;
    struct list32 original = f->output;
    CHECK(fill(f, 0) == kAudio_ParamError && !f->input.calls && f->packets == 8);
    CHECK(!memcmp(&f->output, &original, sizeof(original)));
    check_guards(f);
    dispose(c);
}

struct worker { struct fill_state *fill; int32_t status; };
static void *fill_worker(void *opaque)
{
    struct worker *w = opaque;
    w->status = fill(w->fill, 0);
    return NULL;
}

static void *dispose_worker(void *opaque)
{
    struct worker *w = opaque;
    pthread_mutex_lock(&gate.lock);
    gate.dispose_started = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);
    const uint32_t args[] = {w->fill->input.converter};
    w->status = dispatch("_AudioConverterDispose", args);
    pthread_mutex_lock(&gate.lock);
    gate.dispose_finished = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);
    return NULL;
}

static void concurrent_dispose(void)
{
    uint32_t c = create(pcm(48000, 1, true, false), pcm(48000, 1, false, false));
    struct fill_state *f = prepare_fill(c, 8, 1, false);
    f->input.mode = BLOCK;
    struct worker filling = {f, -999}, disposing = {f, -999};
    pthread_t fill_thread, dispose_thread;
    CHECK(!pthread_create(&fill_thread, NULL, fill_worker, &filling));
    pthread_mutex_lock(&gate.lock);
    while (!gate.entered) pthread_cond_wait(&gate.changed, &gate.lock);
    pthread_mutex_unlock(&gate.lock);
    CHECK(!pthread_create(&dispose_thread, NULL, dispose_worker, &disposing));
    pthread_mutex_lock(&gate.lock);
    while (!gate.dispose_started) pthread_cond_wait(&gate.changed, &gate.lock);
    struct timespec until;
    CHECK(!clock_gettime(CLOCK_REALTIME, &until));
    until.tv_nsec += 100 * 1000 * 1000;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    int status = 0;
    while (!gate.dispose_finished && !status)
        status = pthread_cond_timedwait(&gate.changed, &gate.lock, &until);
    CHECK(status == ETIMEDOUT && !gate.dispose_finished);
    gate.unblock = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);
    CHECK(!pthread_join(fill_thread, NULL) && !pthread_join(dispose_thread, NULL));
    CHECK(filling.status == noErr && disposing.status == noErr);
    const uint32_t stale[] = {c};
    CHECK(dispatch("_AudioConverterDispose", stale) == kAudio_ParamError);
    check_guards(f);
}

static void rejected_creation(void)
{
    AudioStreamBasicDescription *in = low_alloc(sizeof(*in)), *out = low_alloc(sizeof(*out));
    *in = pcm(48000, 1, true, false); *out = pcm(48000, 1, false, false);
    struct { uint32_t value, guard; } *handle = low_alloc(sizeof(*handle));
    handle->value = 0xffffffff; handle->guard = guard;
    const uint32_t args[] = {guest(in), guest(out), guest(handle)};
    in->mFormatID = kAudioFormatMPEGLayer3;
    CHECK(dispatch("_AudioConverterNew", args) == kAudioConverterErr_FormatNotSupported);
    CHECK(!handle->value && handle->guard == guard);
    *in = pcm(48000, 0, true, false); handle->value = 0xffffffff;
    CHECK(dispatch("_AudioConverterNew", args) == kAudio_ParamError);
    CHECK(!handle->value && handle->guard == guard);
    uint64_t untouched = 0x1234567887654321;
    CHECK(!audio_converter_bridge32_dispatch("_AudioConverterUnknown", args, &untouched));
    CHECK(untouched == 0x1234567887654321);
}

int main(void)
{
    /* A reduced __PAGEZERO at link time leaves this guest-sized mapping free. */
    arena = mmap((void *)(uintptr_t)0x40000000, ARENA_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(arena != MAP_FAILED && (uintptr_t)arena + ARENA_SIZE <= UINT32_MAX);
    rejected_creation();
    exact_pcm(false, false);
    exact_pcm(true, false);
    exact_pcm(false, true);
    exact_pcm(true, true);
    resample();
    callback_failures();
    extra_packets();
    cross_converter_reentry();
    invalid_output_range();
    concurrent_dispose();
    CHECK(free_count == allocation_count - owned_count);
    CHECK(!munmap(arena, ARENA_SIZE));
    puts("AudioConverter: PASS (real PCM conversion/resampling, i386 layouts, guards, errors, EOF, stale handles, concurrent disposal)");
    return 0;
}
