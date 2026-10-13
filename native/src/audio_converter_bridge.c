#include "audio_converter_bridge.h"
#include "compat_runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* L4D2 uses AudioConverter for HAL microphone PCM -> mono signed PCM,
   setting only prime method and sample-rate quality. Formats contain no
   pointers; converter references and AudioBufferList do. */
struct buffer32 { uint32_t channels, size, data; };
struct list32 { uint32_t count; struct buffer32 buffers[]; };
_Static_assert(sizeof(AudioStreamBasicDescription) == 40, "ASBD ABI");
_Static_assert(sizeof(struct buffer32) == 12, "i386 AudioBuffer ABI");
enum { MAX_BUFFERS = 64, MAX_CONVERTERS = 128, ALLOCATION_ERROR = -108 };
struct converter {
    AudioConverterRef host;
    AudioStreamBasicDescription input;
    uint32_t handle, scratch, buffer_capacity;
    uint32_t input_mirrors[MAX_BUFFERS];
    unsigned references;
    atomic_bool retired;
    pthread_mutex_t operation;
    struct converter *next;
};
static struct converter *converters;
static unsigned converter_count;
static uint32_t next_handle = 0xf9500001u;
static pthread_mutex_t converters_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct converter *active_callback;

static struct converter *acquire(uint32_t handle)
{
    pthread_mutex_lock(&converters_lock);
    struct converter *c = converters;
    while (c && c->handle != handle) c = c->next;
    if (c) ++c->references;
    pthread_mutex_unlock(&converters_lock);
    return c;
}

static void release(struct converter *c)
{
    pthread_mutex_lock(&converters_lock);
    bool destroy = !--c->references && atomic_load(&c->retired);
    pthread_mutex_unlock(&converters_lock);
    if (destroy) {
        for (unsigned i = 0; i < MAX_BUFFERS; ++i) {
            bool duplicate = false;
            for (unsigned j = 0; j < i; ++j)
                duplicate |= c->input_mirrors[i] == c->input_mirrors[j];
            if (!duplicate) compat_runtime32_deallocate(c->input_mirrors[i]);
        }
        compat_runtime32_deallocate(c->scratch);
        pthread_mutex_destroy(&c->operation);
        free(c);
    }
}

static OSStatus create(const uint32_t *a)
{
    if (!a[2]) return kAudio_ParamError;
    uint32_t *out = (void *)(uintptr_t)a[2];
    *out = 0;
    if (!a[0] || !a[1]) return kAudio_ParamError;
    AudioStreamBasicDescription input, output;
    memcpy(&input, (const void *)(uintptr_t)a[0], sizeof(input));
    memcpy(&output, (const void *)(uintptr_t)a[1], sizeof(output));
    if (input.mFormatID != kAudioFormatLinearPCM || output.mFormatID != kAudioFormatLinearPCM)
        return kAudioConverterErr_FormatNotSupported;
    if (!input.mChannelsPerFrame || input.mChannelsPerFrame > MAX_BUFFERS ||
        !output.mChannelsPerFrame || output.mChannelsPerFrame > MAX_BUFFERS)
        return kAudio_ParamError;
    struct converter *c = calloc(1, sizeof(*c));
    if (!c) return ALLOCATION_ERROR;
    c->input = input;
    c->buffer_capacity = input.mFormatFlags & kAudioFormatFlagIsNonInterleaved ?
        input.mChannelsPerFrame : 1;
    c->scratch = compat_runtime32_allocate(12 + c->buffer_capacity * sizeof(struct buffer32), 1);
    if (!c->scratch) { free(c); return ALLOCATION_ERROR; }
    pthread_mutex_init(&c->operation, NULL);
    atomic_init(&c->retired, false);
    OSStatus status = AudioConverterNew(&input, &output, &c->host);
    if (!status) {
        pthread_mutex_lock(&converters_lock);
        if (converter_count >= MAX_CONVERTERS || next_handle == 0xf9600000u) status = ALLOCATION_ERROR;
        else {
            c->handle = next_handle++;
            c->next = converters; converters = c; ++converter_count;
            *out = c->handle;
        }
        pthread_mutex_unlock(&converters_lock);
    }
    if (status) {
        if (c->host) AudioConverterDispose(c->host);
        compat_runtime32_deallocate(c->scratch);
        pthread_mutex_destroy(&c->operation);
        free(c);
    }
    return status;
}

struct fill_context { struct converter *converter; uint32_t function, info; };
static OSStatus input_ready(AudioConverterRef host, UInt32 *packets, AudioBufferList *data,
                            AudioStreamPacketDescription **descriptions, void *opaque)
{
    (void)host;
    struct fill_context *f = opaque;
    struct converter *c = f->converter;
    uint32_t requested = *packets;
    if (!data || !data->mNumberBuffers || data->mNumberBuffers > c->buffer_capacity)
        return kAudio_ParamError;
    uint32_t *words = (void *)(uintptr_t)c->scratch;
    struct list32 *guest = (void *)(words + 2);
    uint32_t mirrors[MAX_BUFFERS] = {0};
    words[0] = requested; words[1] = 0;
    guest->count = data->mNumberBuffers;
    for (unsigned i = 0; i < guest->count; ++i) {
        const AudioBuffer *b = data->mBuffers + i;
        uint32_t pointer = 0;
        if ((uintptr_t)b->mData > UINT32_MAX && b->mDataByteSize) {
            /* The converter may offer its own input storage. Mirror it below
               4 GB; the callback can fill or replace this buffer. Retain the
               mirror until the next callback, even across Fill returns. */
            pointer = compat_runtime32_allocate(b->mDataByteSize, 0);
            if (!pointer) {
                for (unsigned j = 0; j < i; ++j) {
                    bool old = false;
                    for (unsigned k = 0; k < MAX_BUFFERS; ++k)
                        old |= mirrors[j] && mirrors[j] == c->input_mirrors[k];
                    if (!old) compat_runtime32_deallocate(mirrors[j]);
                }
                return ALLOCATION_ERROR;
            }
            memcpy((void *)(uintptr_t)pointer, b->mData, b->mDataByteSize);
            mirrors[i] = pointer;
        } else if ((uintptr_t)b->mData <= UINT32_MAX) {
            pointer = (uint32_t)(uintptr_t)b->mData;
            for (unsigned j = 0; j < MAX_BUFFERS; ++j)
                if (pointer && pointer == c->input_mirrors[j]) mirrors[i] = pointer;
        }
        guest->buffers[i] = (struct buffer32){b->mNumberChannels, b->mDataByteSize, pointer};
    }
    for (unsigned i = 0; i < MAX_BUFFERS; ++i) {
        bool reused = false, duplicate = false;
        for (unsigned j = 0; j < MAX_BUFFERS; ++j)
            reused |= c->input_mirrors[i] && c->input_mirrors[i] == mirrors[j];
        for (unsigned j = 0; j < i; ++j)
            duplicate |= c->input_mirrors[i] == c->input_mirrors[j];
        if (!reused && !duplicate) compat_runtime32_deallocate(c->input_mirrors[i]);
    }
    memcpy(c->input_mirrors, mirrors, sizeof(mirrors));
    uint32_t args[] = {c->handle, c->scratch, c->scratch + 8,
                       descriptions ? c->scratch + 4 : 0, f->info};
    struct converter *previous = active_callback;
    active_callback = c;
    OSStatus status = (int32_t)compat_runtime32_call(f->function, args, 5);
    active_callback = previous;
    if (compat_runtime32_last_call_trapped()) return kAudioConverterErr_UnspecifiedError;
    if (!guest->count || guest->count > data->mNumberBuffers)
        return kAudioConverterErr_InvalidInputSize;
    if (words[1]) return kAudioConverterErr_OperationNotSupported;
    for (unsigned i = 0; i < guest->count; ++i) {
        const struct buffer32 *b = guest->buffers + i;
        if ((words[0] && b->size && !b->data) ||
            (uint64_t)words[0] * c->input.mBytesPerPacket > b->size ||
            (uint64_t)b->data + b->size > UINT64_C(0x100000000))
            return kAudioConverterErr_InvalidInputSize;
        data->mBuffers[i] = (AudioBuffer){b->channels, b->size, (void *)(uintptr_t)b->data};
    }
    data->mNumberBuffers = guest->count;
    *packets = words[0];
    if (descriptions) *descriptions = NULL;
    return status;
}

static OSStatus fill(struct converter *c, const uint32_t *a)
{
    if (!a[1] || !a[3] || !a[4]) return kAudio_ParamError;
    /* Packet descriptions belong to compressed streams, outside this PCM
       surface. Reject them before calling native code or touching outputs. */
    if (a[5]) return kAudioConverterErr_OperationNotSupported;
    struct list32 *guest = (void *)(uintptr_t)a[4];
    uint32_t count = guest->count;
    if (!count || count > MAX_BUFFERS) return kAudio_ParamError;
    AudioBufferList *native = calloc(1, offsetof(AudioBufferList, mBuffers) + count * sizeof(AudioBuffer));
    if (!native) return ALLOCATION_ERROR;
    native->mNumberBuffers = count;
    for (unsigned i = 0; i < count; ++i) {
        const struct buffer32 *b = guest->buffers + i;
        if ((b->size && !b->data) ||
            (uint64_t)b->data + b->size > UINT64_C(0x100000000)) {
            free(native); return kAudio_ParamError;
        }
        native->mBuffers[i] = (AudioBuffer){b->channels, b->size, (void *)(uintptr_t)b->data};
    }
    UInt32 packets = *(uint32_t *)(uintptr_t)a[3];
    uint32_t capacity = packets;
    struct fill_context context = {c, a[1], a[2]};
    OSStatus status = AudioConverterFillComplexBuffer(c->host, input_ready, &context, &packets, native, NULL);
    bool valid = packets <= capacity && native->mNumberBuffers <= count;
    for (unsigned i = 0; valid && i < native->mNumberBuffers; ++i) {
        const AudioBuffer *b = native->mBuffers + i;
        valid = b->mData == (void *)(uintptr_t)guest->buffers[i].data &&
                b->mDataByteSize <= guest->buffers[i].size;
    }
    if (valid) {
        *(uint32_t *)(uintptr_t)a[3] = packets;
        guest->count = native->mNumberBuffers;
        for (unsigned i = 0; i < guest->count; ++i) {
            guest->buffers[i].channels = native->mBuffers[i].mNumberChannels;
            guest->buffers[i].size = native->mBuffers[i].mDataByteSize;
        }
    } else status = kAudioConverterErr_InvalidOutputSize;
    free(native);
    return status;
}

int audio_converter_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    bool new_converter = !strcmp(name, "_AudioConverterNew");
    bool dispose = !strcmp(name, "_AudioConverterDispose");
    bool set_property = !strcmp(name, "_AudioConverterSetProperty");
    bool fill_buffer = !strcmp(name, "_AudioConverterFillComplexBuffer");
    if (!new_converter && !dispose && !set_property && !fill_buffer) return 0;
    OSStatus status = kAudio_ParamError;
    if (new_converter) status = create(a);
    else {
        struct converter *c = acquire(a[0]);
        if (c) {
            /* Input callbacks may supply data, but cannot synchronously
               operate on any converter. This prevents nested A -> B -> A
               waits and two callback threads acquiring opposite locks. */
            if (active_callback) { release(c); goto done; }
            if (dispose) {
                pthread_mutex_lock(&converters_lock);
                struct converter **link = &converters;
                while (*link && *link != c) link = &(*link)->next;
                if (*link) { *link = c->next; --converter_count; atomic_store(&c->retired, true); }
                else dispose = false;
                pthread_mutex_unlock(&converters_lock);
            }
            pthread_mutex_lock(&c->operation);
            if (dispose) {
                status = AudioConverterDispose(c->host);
                if (status) {
                    /* A failed native disposal leaves the converter usable.
                       Restore its token rather than orphaning native state. */
                    pthread_mutex_lock(&converters_lock);
                    atomic_store(&c->retired, false);
                    c->next = converters; converters = c; ++converter_count;
                    pthread_mutex_unlock(&converters_lock);
                }
            }
            else if (!atomic_load(&c->retired)) {
                if (set_property) {
                    if (a[1] != kAudioConverterPrimeMethod && a[1] != kAudioConverterSampleRateConverterQuality)
                        status = kAudioConverterErr_PropertyNotSupported;
                    else if (a[2] != sizeof(UInt32)) status = kAudioConverterErr_BadPropertySizeError;
                    else if (a[3]) {
                        UInt32 value; memcpy(&value, (const void *)(uintptr_t)a[3], sizeof(value));
                        status = AudioConverterSetProperty(c->host, a[1], sizeof(value), &value);
                    }
                } else if (fill_buffer) status = fill(c, a);
            }
            pthread_mutex_unlock(&c->operation);
            release(c);
        }
    }
done:
    *result = (uint32_t)status;
    return 1;
}
