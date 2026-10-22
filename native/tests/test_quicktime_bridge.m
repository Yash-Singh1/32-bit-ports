/* Native AVFoundation movie checks. The allocator models guarded i386 cells;
   the movie decoder, clock, audio tracks and QuickDraw storage are real. */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "quicktime_bridge.h"
#include "quicktime_support.h"
#include "compat_runtime.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)
static void pump(double seconds);
static void write_fixture(NSString *path);
static void verify_audio_track(NSString *path);

enum { arena_size = 64 * 1024 * 1024, max_allocations = 4096 };
static const uint32_t canary = 0xa1b2c3d4;
static unsigned char *arena;
static size_t arena_used;
static struct { uint32_t address; size_t bytes; bool freed; } allocations[max_allocations];
static unsigned allocation_count;
static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t compat_runtime32_allocate(size_t bytes, int clear)
{
    pthread_mutex_lock(&arena_lock);
    size_t start = (arena_used + 15) & ~(size_t)15;
    CHECK(allocation_count < max_allocations && bytes < arena_size - start - 32);
    unsigned char *p = arena + start + 16;
    memcpy(p - 4, &canary, 4); memcpy(p + bytes, &canary, 4);
    memset(p, clear ? 0 : 0xa5, bytes);
    CHECK((uintptr_t)p + bytes <= UINT32_MAX);
    uint32_t address = (uint32_t)(uintptr_t)p;
    allocations[allocation_count].address = address;
    allocations[allocation_count].bytes = bytes;
    ++allocation_count;
    arena_used = start + 16 + bytes + 4;
    pthread_mutex_unlock(&arena_lock);
    return address;
}

void compat_runtime32_deallocate(uint32_t address)
{
    if (!address) return;
    pthread_mutex_lock(&arena_lock);
    unsigned i;
    for (i = 0; i < allocation_count; ++i) if (allocations[i].address == address) break;
    CHECK(i < allocation_count && !allocations[i].freed);
    allocations[i].freed = true;
    memset((void *)(uintptr_t)address, 0xdd, allocations[i].bytes);
    pthread_mutex_unlock(&arena_lock);
}

uint32_t compat_runtime32_guest_callback(const char *name)
{
    /* Dynamic QuickDraw symbol creation is covered by the support suite. */
    CHECK(name && *name == '_');
    return 0x12345678;
}

static void *low(size_t bytes)
{
    return (void *)(uintptr_t)compat_runtime32_allocate(bytes, 1);
}

static uint32_t guest(const void *p)
{
    CHECK((uintptr_t)p <= UINT32_MAX);
    return (uint32_t)(uintptr_t)p;
}

static uint32_t qt(const char *name, const uint32_t *arguments)
{
    uint64_t result = UINT64_MAX;
    CHECK(quicktime_bridge32_dispatch(name, arguments, &result) == 1);
    CHECK(result <= UINT32_MAX);
    return (uint32_t)result;
}

static uint32_t qd(const char *name, const uint32_t *arguments)
{
    uint64_t result = UINT64_MAX;
    CHECK(quicktime_support32_dispatch(name, arguments, &result) == 1);
    CHECK(result <= UINT32_MAX);
    return (uint32_t)result;
}

static void check_guards(void)
{
    for (unsigned i = 0; i < allocation_count; ++i) {
        const unsigned char *p = (void *)(uintptr_t)allocations[i].address;
        uint32_t before, after;
        memcpy(&before, p - 4, 4); memcpy(&after, p + allocations[i].bytes, 4);
        CHECK(before == canary && after == canary);
    }
}

static int16_t open_file(NSString *path)
{
    const char *utf8 = path.fileSystemRepresentation;
    char *name = low(strlen(utf8) + 1); strcpy(name, utf8);
    void *reference = low(80), *spec = low(70);
    const uint32_t path_args[] = {guest(name), guest(reference), 0};
    CHECK(qd("_FSPathMakeRef", path_args) == 0);
    const uint32_t catalog[] = {guest(reference), 0, 0, 0, guest(spec), 0};
    CHECK(qd("_FSGetCatalogInfo", catalog) == 0);
    struct { int16_t value; uint16_t guard; } *file = low(sizeof(*file));
    file->guard = 0xbeef;
    const uint32_t args[] = {guest(spec), guest(file), 1};
    CHECK(qd("_OpenMovieFile", args) == 0 && file->value > 0 && file->guard == 0xbeef);
    check_guards();
    return file->value;
}

static uint32_t movie_file(NSString *path, int expected_error)
{
    int16_t file = open_file(path);
    struct { uint32_t value, guard; } *movie = low(sizeof(*movie));
    movie->value = UINT32_MAX; movie->guard = canary;
    const uint32_t args[] = {guest(movie), (uint32_t)(int32_t)file, 0, 0, 1, 0};
    int32_t status = (int32_t)qt("_NewMovieFromFile", args);
    if (status != expected_error) fprintf(stderr, "movie open %s returned %d, expected %d\n",
        path.lastPathComponent.UTF8String, status, expected_error);
    CHECK(status == expected_error && movie->guard == canary);
    CHECK((movie->value != 0) == (expected_error == 0));
    const uint32_t closed[] = {(uint32_t)(int32_t)file};
    CHECK(qd("_CloseMovieFile", closed) == 0);
    CHECK(quicktime_support32_file_path(file) == NULL);
    check_guards();
    return movie->value;
}

static uint32_t world_for_movie(uint32_t movie, int *width, int *height)
{
    struct { int16_t rect[4]; uint32_t guard; } *bounds = low(sizeof(*bounds));
    bounds->guard = canary;
    const uint32_t bound_args[] = {movie, guest(bounds)};
    CHECK(qt("_GetMovieNaturalBoundsRect", bound_args) == 0);
    CHECK(bounds->guard == canary && bounds->rect[0] == 0 && bounds->rect[1] == 0);
    *height = bounds->rect[2]; *width = bounds->rect[3];
    CHECK(*width > 0 && *height > 0);
    struct { uint32_t value, guard; } *world = low(sizeof(*world)); world->guard = canary;
    const uint32_t args[] = {guest(world), 0x42475241, guest(bounds), 0, 0, 0};
    CHECK(qd("_QTNewGWorld", args) == 0 && world->value && world->guard == canary);
    const uint32_t set[] = {movie, world->value, 0};
    CHECK(qt("_SetMovieGWorld", set) == 0);
    check_guards();
    return world->value;
}

static uint64_t world_hash(uint32_t world, int width, int height)
{
    const uint32_t world_args[] = {world};
    uint32_t pixmap = qd("_GetGWorldPixMap", world_args); CHECK(pixmap);
    const uint32_t map_args[] = {pixmap};
    CHECK(qd("_LockPixels", map_args) == 1);
    uint32_t address = qd("_GetPixBaseAddr", map_args), stride = qd("_GetPixRowBytes", map_args);
    CHECK(address && stride >= (uint32_t)width * 4);
    const unsigned char *pixels = (void *)(uintptr_t)address;
    uint64_t hash = 1469598103934665603ULL;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width * 4; ++x)
        hash = (hash ^ pixels[(size_t)y * stride + x]) * 1099511628211ULL;
    CHECK(qd("_UnlockPixels", map_args) == 0);
    return hash;
}

static void task(uint32_t movie)
{
    const uint32_t args[] = {movie, 0};
    int32_t status = (int32_t)qt("_MoviesTask", args);
    if (status) fprintf(stderr, "MoviesTask returned %d\n", status);
    CHECK(status == 0);
}

static void drive(uint32_t movie, double seconds)
{
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    do { pump(.005); task(movie); } while (end.timeIntervalSinceNow > 0);
}

static void seek(uint32_t movie, int32_t tick)
{
    const uint32_t args[] = {movie, (uint32_t)tick};
    CHECK(qt("_SetMovieTimeValue", args) == 0);
}

static void await_changed(uint32_t movie, uint32_t world, int width, int height,
                          uint64_t previous)
{
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:3];
    do {
        drive(movie, .02);
        if (world_hash(world, width, height) != previous) return;
    } while (deadline.timeIntervalSinceNow > 0);
    CHECK(world_hash(world, width, height) != previous);
}

static void timeline(uint32_t movie, unsigned expected_samples)
{
    const uint32_t args[] = {movie};
    int32_t scale = (int32_t)qt("_GetMovieTimeScale", args);
    const uint32_t bad_rect[] = {movie, 0xfffffffcu};
    CHECK((int32_t)qt("_GetMovieNaturalBoundsRect", bad_rect) == -50);
    const uint32_t negative_time[] = {movie, UINT32_MAX};
    CHECK((int32_t)qt("_SetMovieTimeValue", negative_time) == -2015);
    const uint32_t past_end[] = {movie, (uint32_t)qt("_GetMovieDuration", args) + 1};
    CHECK((int32_t)qt("_SetMovieTimeValue", past_end) == -2015);
    int32_t length = (int32_t)qt("_GetMovieDuration", args);
    CHECK(scale > 0 && length > 0 && qt("_GetMoviePreferredRate", args) == 0x10000);
    struct { int32_t value; uint32_t guard; } *tick = low(sizeof(*tick)), *duration = low(sizeof(*duration));
    tick->guard = duration->guard = canary;
    uint32_t *types = low(4); *types = 0x65796573;
    int32_t previous = -1;
    unsigned found = 0;
    for (;;) {
        uint32_t next[] = {movie, 1, 1, guest(types), (uint32_t)previous, 0x10000, guest(tick), guest(duration)};
        int32_t status = (int32_t)qt("_GetMovieNextInterestingTime", next);
        CHECK(tick->guard == canary && duration->guard == canary);
        if (status == -2015) { CHECK(tick->value == -1 && duration->value == 0); break; }
        CHECK(status == 0 && tick->value > previous && tick->value < length && duration->value > 0);
        previous = tick->value;
        CHECK(++found <= 10000);
    }
    CHECK(found == expected_samples && (int32_t)qt("_GetMoviesError", args) == -2015);
    uint32_t first[] = {movie, 1 | 0x4000, 1, guest(types), 0, 0x10000, guest(tick), 0};
    CHECK(qt("_GetMovieNextInterestingTime", first) == 0 && tick->value == 0);
    uint32_t invalid[] = {movie, 1, 2, guest(types), 0, 0x10000, guest(tick), guest(duration)};
    CHECK((int32_t)qt("_GetMovieNextInterestingTime", invalid) == -50);
    CHECK(tick->guard == canary && duration->guard == canary);
    check_guards();
}

static void fixture_playback(NSString *path)
{
    uint32_t movie = movie_file(path, 0);
    timeline(movie, 12);
    const uint32_t args[] = {movie};
    int32_t scale = (int32_t)qt("_GetMovieTimeScale", args);
    const uint32_t muted[] = {movie, 0}; CHECK(qt("_SetMovieVolume", muted) == 0);
    int width, height; uint32_t world = world_for_movie(movie, &width, &height);
    CHECK(width == 64 && height == 48);
    const uint32_t bad_world[] = {movie, UINT32_MAX, 0};
    CHECK((int32_t)qt("_SetMovieGWorld", bad_world) == -50);
    const uint32_t bad_audio[] = {movie, UINT32_MAX};
    CHECK((int32_t)qt("_SetMovieAudioContext", bad_audio) == -50);
    uint64_t empty = world_hash(world, width, height);
    seek(movie, scale / 4); drive(movie, .3);
    uint64_t quarter = world_hash(world, width, height); CHECK(quarter != empty);
    /* Paused tasks must retain the last image if the output has no new frame. */
    drive(movie, .1); CHECK(world_hash(world, width, height) == quarter);
    seek(movie, 3 * scale / 4); drive(movie, .3);
    uint64_t late = world_hash(world, width, height); CHECK(late != quarter);
    seek(movie, scale / 4); drive(movie, .3); CHECK(world_hash(world, width, height) == quarter);
    CHECK(qt("_StartMovie", args) == 0); await_changed(movie, world, width, height, quarter);
    CHECK(qt("_StopMovie", args) == 0); drive(movie, .1);
    uint64_t stopped = world_hash(world, width, height);
    drive(movie, .15); CHECK(world_hash(world, width, height) == stopped);
    /* Source starts once, then loops by seeking without another StartMovie.
       Native AVPlayer pauses at EOF, so verify both rewind and continued
       frame advancement rather than only a changed first frame. */
    seek(movie, 0); drive(movie, .3);
    uint64_t beginning = world_hash(world, width, height); CHECK(beginning != late);
    CHECK(qt("_StartMovie", args) == 0); drive(movie, 1.25);
    task(movie); uint64_t end = world_hash(world, width, height); CHECK(end != beginning);
    seek(movie, 0); await_changed(movie, world, width, height, end);
    uint64_t rewound = world_hash(world, width, height);
    await_changed(movie, world, width, height, rewound);
    CHECK(qt("_StopMovie", args) == 0);
    seek(movie, 0); drive(movie, .3); CHECK(world_hash(world, width, height) == beginning);
    /* A busy pixel map pins the previous image; the pending frame is retried. */
    const uint32_t world_args[] = {world}; uint32_t map = qd("_GetGWorldPixMap", world_args);
    const uint32_t map_args[] = {map}; CHECK(qd("_LockPixels", map_args) == 1);
    seek(movie, scale / 2); pump(.25);
    CHECK((int32_t)qt("_MoviesTask", muted) == -50);
    CHECK(qd("_UnlockPixels", map_args) == 0); drive(movie, .1);
    CHECK(world_hash(world, width, height) != beginning);
    struct { uint32_t value, guard; } *context = low(sizeof(*context)); context->guard = canary;
    const uint32_t audio[] = {0, 0, 0, guest(context)};
    CHECK(qt("_QTAudioContextCreateForAudioDevice", audio) == 0 && context->value && context->guard == canary);
    const uint32_t bad_context[] = {0, 0, 0, 0xfffffffeu};
    CHECK((int32_t)qt("_QTAudioContextCreateForAudioDevice", bad_context) == -50);
    struct { uint32_t value, guard; } *invalid_context = low(sizeof(*invalid_context));
    invalid_context->value = UINT32_MAX; invalid_context->guard = canary;
    const uint32_t device_options[] = {0, 1, 0, guest(invalid_context)};
    CHECK((int32_t)qt("_QTAudioContextCreateForAudioDevice", device_options) == -50);
    CHECK(!invalid_context->value && invalid_context->guard == canary);
    const uint32_t attach[] = {movie, context->value}; CHECK(qt("_SetMovieAudioContext", attach) == 0);
    const uint32_t release[] = {context->value}; CHECK(qt("_QTAudioContextRelease", release) == 0);
    CHECK((int32_t)qt("_SetMovieAudioContext", attach) == -50);
    CHECK((int32_t)qt("_QTAudioContextRelease", release) == -50);
    CHECK(qt("_DisposeMovie", args) == 0);
    CHECK((int32_t)qt("_GetMovieDuration", args) == -50);
    CHECK((int32_t)qt("_DisposeMovie", args) == -50);
    uint32_t replacement = movie_file(path, 0); CHECK(replacement != movie);
    /* The native seek completion may outlive a disposed movie. */
    seek(replacement, scale / 2);
    const uint32_t replacement_args[] = {replacement}; CHECK(qt("_DisposeMovie", replacement_args) == 0);
    pump(.1);
    CHECK(qd("_DisposeGWorld", world_args) == 0);
    check_guards();
}

static void installed_movie(NSString *path)
{
    uint32_t movie = movie_file(path, 0);
    const uint32_t args[] = {movie}, muted[] = {movie, 0};
    CHECK(qt("_SetMovieVolume", muted) == 0);
    int width, height; uint32_t world = world_for_movie(movie, &width, &height);
    uint64_t empty = world_hash(world, width, height);
    int32_t scale = (int32_t)qt("_GetMovieTimeScale", args);
    seek(movie, scale * 3); drive(movie, .4);
    uint64_t first = world_hash(world, width, height); CHECK(first != empty);
    CHECK(qt("_StartMovie", args) == 0); await_changed(movie, world, width, height, first);
    CHECK(qt("_StopMovie", args) == 0 && qt("_DisposeMovie", args) == 0);
    const uint32_t world_args[] = {world}; CHECK(qd("_DisposeGWorld", world_args) == 0);
    printf("QuickTime installed movie: PASS (%s %dx%d)\n", path.lastPathComponent.UTF8String, width, height);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        arena = mmap((void *)(uintptr_t)0x40000000, arena_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(arena != MAP_FAILED && (uintptr_t)arena + arena_size <= UINT32_MAX);
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"l4d2-movie-fixture-%@.mov", NSUUID.UUID.UUIDString]];
        write_fixture(path); verify_audio_track(path);
        const uint32_t none[] = {0}; CHECK(qt("_EnterMovies", none) == 0);
        struct { uint32_t value, guard; } *bad = low(sizeof(*bad)); bad->guard = canary;
        const uint32_t invalid[] = {guest(bad), 0xffff, 0, 0, 1, 0};
        CHECK((int32_t)qt("_NewMovieFromFile", invalid) == -43 && !bad->value && bad->guard == canary);
        fixture_playback(path);
        NSString *invalid_path = [path stringByAppendingString:@".txt"];
        CHECK([@"not a movie" writeToFile:invalid_path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        movie_file(invalid_path, -2010);
        CHECK([[NSFileManager defaultManager] removeItemAtPath:invalid_path error:nil]);
        for (int i = 1; i < argc; ++i) installed_movie([NSString stringWithUTF8String:argv[i]]);
        uint32_t outstanding = movie_file(path, 0);
        const uint32_t outstanding_args[] = {outstanding};
        seek(outstanding, 0); CHECK(qt("_ExitMovies", none) == 0); pump(.1);
        CHECK((int32_t)qt("_GetMovieDuration", outstanding_args) == -50);
        CHECK(qt("_ExitMovies", none) == 0);
        uint64_t untouched = UINT64_C(0x1234567887654321);
        CHECK(!quicktime_bridge32_dispatch("_QuickTimeUnsupportedFunction", none, &untouched));
        CHECK(untouched == UINT64_C(0x1234567887654321));
        check_guards();
        CHECK([[NSFileManager defaultManager] removeItemAtPath:path error:nil]);
        CHECK(munmap(arena, arena_size) == 0);
        puts("QuickTime movies: PASS (real H264/PCM decoding, guarded guest cells, timestamps, pause/seek/loop, stale handles, GWorld retries, audio-context lifecycle)");
    }
    return 0;
}
static void pump(double seconds)
{
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    do {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
    } while (end.timeIntervalSinceNow > 0);
}

static void write_fixture(NSString *path)
{
    NSError *error = nil;
    AVAssetWriter *writer = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:path]
        fileType:AVFileTypeQuickTimeMovie error:&error];
    CHECK(writer && !error);
    AVAssetWriterInput *video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
        outputSettings:@{AVVideoCodecKey:AVVideoCodecTypeH264, AVVideoWidthKey:@64,
                         AVVideoHeightKey:@48}];
    AVAssetWriterInputPixelBufferAdaptor *adaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:video
        sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
                                     (id)kCVPixelBufferWidthKey:@64, (id)kCVPixelBufferHeightKey:@48}];
    AVAssetWriterInput *audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
        outputSettings:@{AVFormatIDKey:@(kAudioFormatLinearPCM), AVSampleRateKey:@48000,
            AVNumberOfChannelsKey:@2, AVLinearPCMBitDepthKey:@16, AVLinearPCMIsFloatKey:@NO,
            AVLinearPCMIsBigEndianKey:@NO, AVLinearPCMIsNonInterleaved:@NO}];
    CHECK([writer canAddInput:video] && [writer canAddInput:audio]);
    [writer addInput:video]; [writer addInput:audio];
    CHECK([writer startWriting]); [writer startSessionAtSourceTime:kCMTimeZero];
    for (unsigned frame = 0; frame < 12; ++frame) {
        while (!video.readyForMoreMediaData) { CHECK(writer.status == AVAssetWriterStatusWriting); pump(.005); }
        CVPixelBufferRef pixel = NULL;
        CHECK(CVPixelBufferCreate(NULL, 64, 48, kCVPixelFormatType_32BGRA, NULL, &pixel) == 0);
        CHECK(CVPixelBufferLockBaseAddress(pixel, 0) == 0);
        unsigned char *data = CVPixelBufferGetBaseAddress(pixel);
        size_t stride = CVPixelBufferGetBytesPerRow(pixel);
        for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x) {
            unsigned char *p = data + y * stride + x * 4;
            p[0] = (unsigned char)(16 + frame * 15);
            p[1] = (unsigned char)(32 + y * 3);
            p[2] = (unsigned char)(48 + x * 2);
            p[3] = 255;
        }
        CHECK(CVPixelBufferUnlockBaseAddress(pixel, 0) == 0);
        CHECK([adaptor appendPixelBuffer:pixel withPresentationTime:CMTimeMake(frame, 12)]);
        CVPixelBufferRelease(pixel);
    }
    [video markAsFinished];
    AudioStreamBasicDescription format = {48000, kAudioFormatLinearPCM,
        kAudioFormatFlagIsPacked | kAudioFormatFlagIsSignedInteger,
        4, 1, 4, 2, 16, 0};
    CMAudioFormatDescriptionRef description = NULL;
    CHECK(CMAudioFormatDescriptionCreate(NULL, &format, 0, NULL, 0, NULL, NULL, &description) == 0);
    CMBlockBufferRef block = NULL;
    CHECK(CMBlockBufferCreateWithMemoryBlock(NULL, NULL, 48000 * 4, NULL, NULL, 0,
        48000 * 4, 0, &block) == 0);
    CHECK(CMBlockBufferAssureBlockMemory(block) == 0);
    char *storage = NULL; size_t contiguous = 0, total = 0;
    CHECK(CMBlockBufferGetDataPointer(block, 0, &contiguous, &total, &storage) == 0);
    CHECK(contiguous == total && total == 48000 * 4);
    int16_t *samples = (void *)storage;
    for (unsigned frame = 0; frame < 48000; ++frame) {
        int16_t value = (int16_t)(sin(frame * 2 * M_PI * 440 / 48000) * 6553);
        samples[frame * 2] = samples[frame * 2 + 1] = value;
    }
    CMSampleBufferRef sample = NULL;
    CHECK(CMAudioSampleBufferCreateWithPacketDescriptions(NULL, block, true, NULL, NULL,
        description, 48000, kCMTimeZero, NULL, &sample) == 0);
    while (!audio.readyForMoreMediaData) { CHECK(writer.status == AVAssetWriterStatusWriting); pump(.005); }
    CHECK([audio appendSampleBuffer:sample]); [audio markAsFinished];
    CFRelease(sample); CFRelease(block); CFRelease(description);
    __block atomic_bool finished = false;
    [writer finishWritingWithCompletionHandler:^{ atomic_store(&finished, true); }];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:10];
    while (!atomic_load(&finished)) { CHECK(deadline.timeIntervalSinceNow > 0); pump(.005); }
    if (writer.status != AVAssetWriterStatusCompleted)
        fprintf(stderr, "fixture writer: %s\n", writer.error.description.UTF8String);
    CHECK(writer.status == AVAssetWriterStatusCompleted);
}

static void verify_audio_track(NSString *path)
{
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
    CHECK(tracks.count == 1);
    NSError *error = nil;
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    CHECK(reader && !error);
    AVAssetReaderTrackOutput *output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tracks[0]
        outputSettings:@{AVFormatIDKey:@(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey:@16,
            AVLinearPCMIsFloatKey:@NO, AVLinearPCMIsBigEndianKey:@NO,
            AVLinearPCMIsNonInterleaved:@NO}];
    CHECK([reader canAddOutput:output]); [reader addOutput:output]; CHECK([reader startReading]);
    CMSampleBufferRef sample = [output copyNextSampleBuffer]; CHECK(sample);
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t bytes = CMBlockBufferGetDataLength(block); CHECK(bytes >= 4096);
    int16_t *pcm = malloc(bytes); CHECK(pcm);
    CHECK(CMBlockBufferCopyDataBytes(block, 0, bytes, pcm) == 0);
    double energy = 0;
    for (size_t i = 0; i < bytes / 2; ++i) { double value = pcm[i] / 32768.0; energy += value * value; }
    CHECK(sqrt(energy / (bytes / 2)) > .10);
    free(pcm); CFRelease(sample); [reader cancelReading];
}
