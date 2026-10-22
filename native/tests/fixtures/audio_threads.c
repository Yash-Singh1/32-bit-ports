#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach.h>
#include <CoreAudio/CoreAudio.h>
extern int GetCurrentProcess(void *);
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static volatile int worker_ran, returned_buffer, listener_ran;
static int *worker_errno;
static uint32_t self_suspend_port, self_suspend_ready, self_suspend_completed;
static kern_return_t self_suspend_status;
static AudioQueueRef expected_queue;
static AudioQueueBufferRef expected_buffer;

static void *worker(void *info)
{
    worker_errno = &errno;
    *worker_errno = EACCES;
    worker_ran = (uintptr_t)info == 0x12345 ? 1 : -1;
    return (void *)0x56789;
}
static void *self_suspending_worker(void *unused)
{
    (void)unused;
    self_suspend_port = pthread_mach_thread_np(pthread_self());
    __atomic_store_n(&self_suspend_ready, 1, __ATOMIC_RELEASE);
    self_suspend_status = thread_suspend(self_suspend_port);
    __atomic_store_n(&self_suspend_completed, 1, __ATOMIC_RELEASE);
    return (void *)0x34567;
}
static void buffer_done(void *info, AudioQueueRef q, AudioQueueBufferRef b)
{
    returned_buffer = (uintptr_t)info == 0xabc && q == expected_queue && b == expected_buffer &&
        b->mUserData == (void *)0xdef && b->mAudioDataBytesCapacity >= 17640 ? 1 : -1;
}
static void state_changed(void *info, AudioQueueRef q, AudioQueuePropertyID property)
{
    if ((uintptr_t)info != 0x987 || q != expected_queue || property != kAudioQueueProperty_IsRunning)
        listener_ran = -100;
    else ++listener_ran;
}

static volatile int burst_error;
static void burst_done(void *info, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    if (info != (void *)0xbeef || !queue || !buffer || !buffer->mAudioData ||
        buffer->mAudioDataBytesCapacity != 1024 || buffer->mAudioDataByteSize > 1024)
        burst_error = 1;
}

static int check_map_load_queues(const AudioStreamBasicDescription *format)
{
    /* The report's exact allocation shape, inside the loader rather than a
       standalone native reproducer. PCM is zero throughout (silent). */
    for (int cycle = 0; cycle < 3; ++cycle) {
        AudioQueueRef queue = NULL;
        AudioQueueBufferRef buffers[128];
        if (AudioQueueNewOutput(format, burst_done, (void *)0xbeef, NULL, NULL, 0, &queue)) return -276;
        for (int i = 0; i < 128; ++i) {
            if (AudioQueueAllocateBuffer(queue, 1024, &buffers[i])) return -277;
            memset(buffers[i]->mAudioData, 0, 1024);
            buffers[i]->mAudioDataByteSize = 1024;
        }
        for (int burst = 0; burst < 8; ++burst) {
            for (int i = 0; i < 128; ++i)
                if (AudioQueueEnqueueBuffer(queue, buffers[i], 0, NULL)) return -278;
            if (AudioQueueStart(queue, NULL)) return -279;
            usleep(5000);
            if (AudioQueueStop(queue, true) || AudioQueueStop(queue, true) ||
                AudioQueueStop(queue, true) || burst_error) return -280;
            /* Source also restarts the same queue after multiple immediate
               stops, before its mixer has re-enqueued the returned buffers. */
            if (AudioQueueStart(queue, NULL) || AudioQueueStop(queue, true)) return -282;
        }
        if (AudioQueueDispose(queue, true)) return -281;
    }
    return 0;
}

int check_audio_threads(void)
{
    struct { uint32_t hi, lo, guard; } psn = {0, 0, 0x1234abcd};
    if (GetCurrentProcess(&psn) || psn.guard != 0x1234abcd || (!psn.hi && !psn.lo)) return -220;
    AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    struct { AudioDeviceID id; uint32_t guard; } device = {0, 0x1234abcd};
    UInt32 size = sizeof(device.id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device.id) ||
        size != sizeof(device.id) || device.guard != 0x1234abcd) return -221;
    address.mSelector = 0xffffffff;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device.id) !=
        kAudioHardwareUnknownPropertyError || device.guard != 0x1234abcd) return -222;
    worker_ran = returned_buffer = listener_ran = 0;
    errno = EDOM;
    int *main_errno = &errno;
    struct { pthread_t thread; uint32_t guard; } t = {0, 0x12345678};
    if (pthread_create_suspended_np(&t.thread, NULL, worker, (void *)0x12345)) return -200;
    usleep(10000);
    if (worker_ran || t.guard != 0x12345678) return -201;
    mach_port_t port = pthread_mach_thread_np(t.thread);
    if (!port || thread_suspend(port) || thread_resume(port)) return -202;
    /* One resume balances the extra suspend; the creation suspend remains.
       A fake successful thread_suspend would let the worker run here. */
    usleep(10000);
    if (worker_ran || thread_resume(port)) return -223;
    struct { void *value; uint32_t guard; } joined = {0, 0x87654321};
    if (pthread_join(t.thread, &joined.value) || worker_ran != 1 ||
        joined.value != (void *)0x56789 || joined.guard != 0x87654321 ||
        worker_errno == main_errno || !worker_errno || *worker_errno != EACCES) return -203;
    if (thread_suspend(MACH_PORT_NULL) == KERN_SUCCESS) return -224;
    self_suspend_port = self_suspend_ready = self_suspend_completed = 0;
    self_suspend_status = KERN_FAILURE;
    if (pthread_create(&t.thread, NULL, self_suspending_worker, NULL)) return -225;
    unsigned attempts = 0, successful_resumes = 0;
    while (!__atomic_load_n(&self_suspend_ready, __ATOMIC_ACQUIRE) && attempts++ < 10000)
        usleep(100);
    if (!__atomic_load_n(&self_suspend_ready, __ATOMIC_ACQUIRE) || !self_suspend_port) return -226;
    /* Publishing readiness precedes entering the kernel. Early resumes must
       report the native error; retry until the actual self-suspend is resumed.
       A no-op suspend cannot produce a successful native resume. */
    attempts = 0;
    while (!__atomic_load_n(&self_suspend_completed, __ATOMIC_ACQUIRE) && attempts++ < 10000) {
        if (thread_resume(self_suspend_port) == KERN_SUCCESS) ++successful_resumes;
        usleep(100);
    }
    if (!__atomic_load_n(&self_suspend_completed, __ATOMIC_ACQUIRE) ||
        !successful_resumes || self_suspend_status != KERN_SUCCESS)
        return -227;
    if (pthread_join(t.thread, &joined.value) || joined.value != (void *)0x34567 ||
        t.guard != 0x12345678 || joined.guard != 0x87654321) return -228;

    AudioStreamBasicDescription format = {44100, kAudioFormatLinearPCM,
        kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    struct { AudioQueueRef queue; uint32_t guard; } output = {0, 0x12345678};
    if (AudioQueueNewOutput(&format, buffer_done, (void *)0xabc, NULL, NULL, 0, &output.queue)) return -204;
    expected_queue = output.queue;
    if (!output.queue || output.guard != 0x12345678) return -205;
    if (AudioQueueAddPropertyListener(output.queue, kAudioQueueProperty_IsRunning, state_changed, (void *)0x987)) return -206;
    struct { AudioQueueBufferRef buffer; uint32_t guard; } allocation = {0, 0x98765432};
    if (AudioQueueAllocateBuffer(output.queue, 17640, &allocation.buffer)) return -207;
    expected_buffer = allocation.buffer;
    if (!allocation.buffer || allocation.guard != 0x98765432 || !allocation.buffer->mAudioData ||
        allocation.buffer->mAudioDataBytesCapacity < 17640) return -208;
    memset(allocation.buffer->mAudioData, 0, 17640);
    allocation.buffer->mAudioDataByteSize = 17640;
    allocation.buffer->mUserData = (void *)0xdef;
    if (AudioQueueSetParameter(output.queue, kAudioQueueParam_Volume, 0.5f)) return -209;
    float volume = 0;
    if (AudioQueueGetParameter(output.queue, kAudioQueueParam_Volume, &volume) || volume != 0.5f) return -210;
    if (AudioQueueEnqueueBuffer(output.queue, allocation.buffer, 0, NULL) || AudioQueueStart(output.queue, NULL)) return -211;
    for (int i = 0; i < 300 && !returned_buffer; ++i) usleep(10000);
    if (returned_buffer != 1 || listener_ran < 0) return -212;
    if (AudioQueueStop(output.queue, true)) return -213;
    if (AudioQueueRemovePropertyListener(output.queue, kAudioQueueProperty_IsRunning, state_changed, (void *)0x987)) return -214;
    if (AudioQueueFreeBuffer(output.queue, allocation.buffer) || AudioQueueDispose(output.queue, true)) return -215;
    /* The registry is reusable after native teardown. */
    for (int i = 0; i < 65; ++i) {
        if (AudioQueueNewOutput(&format, buffer_done, NULL, NULL, NULL, 0, &output.queue) ||
            AudioQueueDispose(output.queue, true)) return -216;
    }
    return check_map_load_queues(&format);
}
