#include "quicktime_bridge.h"
#include "quicktime_support.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* L4D2's original IQuickTime001 implementation still owns the Source
   material, texture, looping and presentation code. This adapter replaces
   its removed Movie Toolbox decoder with AVPlayer, preserving native audio
   playback and copying BGRA frames into its guest QuickDraw world. */
enum { PARAM_ERROR = -50, MEMORY_ERROR = -108, FILE_ERROR = -43,
       MOVIE_ERROR = -2010, INVALID_TIME = -2015, MAX_MOVIES = 32,
       MAX_SAMPLE_TIMES = 4000000 };
struct seek_state { atomic_uint references; atomic_bool done, succeeded; };
struct movie {
    uint32_t handle, world, audio_context;
    int width, height;
    int32_t time_scale, duration;
    int32_t *sample_times;
    size_t sample_count;
    AVPlayer *player;
    AVPlayerItem *item;
    AVPlayerItemVideoOutput *output;
    CVPixelBufferRef last_pixels;
    struct seek_state *seek;
    bool logged_frame, started;
    struct movie *next;
};
struct audio_context { uint32_t handle; struct audio_context *next; };
static struct movie *movies;
static struct audio_context *audio_contexts;
static unsigned movie_count;
static uint32_t next_movie = 0xf9300001u, next_audio = 0xf9400001u;
/* No guest callbacks or main-queue sync hops occur under this lock. QuickDraw
   support never takes the movie lock, so movie -> world is the sole order. */
static pthread_mutex_t movies_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int16_t last_error;

static void *guest_range(uint32_t address, size_t bytes)
{
    if (!address || bytes > UINT32_MAX ||
        (uint64_t)address + bytes > UINT64_C(0x100000000)) return NULL;
    return (void *)(uintptr_t)address;
}

static struct movie *find_movie(uint32_t handle)
{
    struct movie *m = movies;
    while (m && m->handle != handle) m = m->next;
    return m;
}

static bool find_audio(uint32_t handle)
{
    for (struct audio_context *a = audio_contexts; a; a = a->next)
        if (a->handle == handle) return true;
    return false;
}

static void release_seek(struct seek_state *seek)
{
    if (seek && atomic_fetch_sub(&seek->references, 1) == 1) free(seek);
}

static void destroy_movie(struct movie *m)
{
    [m->player pause];
    [m->player replaceCurrentItemWithPlayerItem:nil];
    if (m->item && m->output) [m->item removeOutput:m->output];
    if (m->last_pixels) CVPixelBufferRelease(m->last_pixels);
    release_seek(m->seek);
    [m->output release];
    [m->item release];
    [m->player release];
    free(m->sample_times);
    free(m);
}

static int compare_times(const void *left, const void *right)
{
    int32_t a = *(const int32_t *)left, b = *(const int32_t *)right;
    return (a > b) - (a < b);
}

/* These asynchronous key-loading APIs remain the macOS 11-compatible path.
   Read metadata only after loading; do not trigger implicit synchronous I/O. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static int load_movie(const char *path, struct movie **out)
{
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
        options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
    dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
    dispatch_retain(loaded);
    NSArray *keys = @[@"tracks", @"duration", @"playable"];
    [asset loadValuesAsynchronouslyForKeys:keys completionHandler:^{
        dispatch_semaphore_signal(loaded);
        dispatch_release(loaded);
    }];
    /* The callback owns an explicit reference, including after timeout. */
    long timed_out = dispatch_semaphore_wait(loaded,
        dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    dispatch_release(loaded);
    if (timed_out) { [asset cancelLoading]; return MOVIE_ERROR; }
    for (NSString *key in keys) {
        NSError *error = nil;
        if ([asset statusOfValueForKey:key error:&error] != AVKeyValueStatusLoaded)
            return MOVIE_ERROR;
    }
    NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    if (!asset.playable || tracks.count != 1) return MOVIE_ERROR;
    AVAssetTrack *track = [tracks objectAtIndex:0];
    CGSize size = track.naturalSize;
    double seconds = CMTimeGetSeconds(asset.duration);
    int32_t scale = track.naturalTimeScale;
    if (scale <= 0 || !isfinite(seconds) || seconds <= 0 ||
        seconds * scale >= INT32_MAX || size.width < 1 || size.width > INT16_MAX ||
        size.height < 1 || size.height > INT16_MAX ||
        !CGAffineTransformIsIdentity(track.preferredTransform)) return MOVIE_ERROR;

    struct movie *m = calloc(1, sizeof(*m));
    if (!m) return MEMORY_ERROR;
    m->width = (int)size.width; m->height = (int)size.height;
    m->time_scale = scale;
    m->duration = (int32_t)CMTimeConvertScale(asset.duration, scale,
        kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;

    /* Use real presentation timestamps, including fractional/VFR cadence.
       Reading compressed samples here performs no video/audio playback. */
    NSError *error = nil;
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    AVAssetReaderTrackOutput *timing = [[AVAssetReaderTrackOutput alloc]
        initWithTrack:track outputSettings:nil];
    timing.alwaysCopiesSampleData = NO;
    if (!reader || ![reader canAddOutput:timing]) {
        [timing release]; [reader release]; destroy_movie(m); return MOVIE_ERROR;
    }
    [reader addOutput:timing];
    if (![reader startReading]) {
        [timing release]; [reader release]; destroy_movie(m); return MOVIE_ERROR;
    }
    size_t capacity = 0;
    int status = 0;
    CMSampleBufferRef sample;
    while ((sample = [timing copyNextSampleBuffer])) {
        /* Compressed reader outputs include marker-only buffers with no
           samples and possibly invalid timestamps. They are not frames. */
        if (!CMSampleBufferGetNumSamples(sample)) { CFRelease(sample); continue; }
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        int64_t tick = CMTIME_IS_NUMERIC(pts) ? CMTimeConvertScale(pts, scale,
            kCMTimeRoundingMethod_RoundHalfAwayFromZero).value : -1;
        CFRelease(sample);
        if (tick < 0 || tick >= INT32_MAX) { status = MOVIE_ERROR; break; }
        if (m->sample_count == MAX_SAMPLE_TIMES) { status = MEMORY_ERROR; break; }
        if (m->sample_count == capacity) {
            size_t larger = capacity ? capacity * 2 : 256;
            if (larger > MAX_SAMPLE_TIMES) larger = MAX_SAMPLE_TIMES;
            int32_t *times = realloc(m->sample_times, larger * sizeof(*times));
            if (!times) { status = MEMORY_ERROR; break; }
            m->sample_times = times; capacity = larger;
        }
        m->sample_times[m->sample_count++] = (int32_t)tick;
    }
    if (!status && (reader.status != AVAssetReaderStatusCompleted || !m->sample_count))
        status = MOVIE_ERROR;
    [reader cancelReading]; [timing release]; [reader release];
    if (status) { destroy_movie(m); return status; }
    qsort(m->sample_times, m->sample_count, sizeof(*m->sample_times), compare_times);
    size_t unique = 1;
    for (size_t i = 1; i < m->sample_count; ++i)
        if (m->sample_times[i] != m->sample_times[unique - 1])
            m->sample_times[unique++] = m->sample_times[i];
    m->sample_count = unique;
    m->item = [[AVPlayerItem alloc] initWithAsset:asset];
    m->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:
        @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
    if (!m->item || !m->output) { destroy_movie(m); return MEMORY_ERROR; }
    /* Only video rendering is suppressed. AVPlayer still plays the movie's
       audio, including L4D2's PCM and ALAC tracks, on the default device. */
    m->output.suppressesPlayerRendering = YES;
    [m->item addOutput:m->output];
    m->player = [[AVPlayer alloc] initWithPlayerItem:m->item];
    m->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    m->player.automaticallyWaitsToMinimizeStalling = NO;
    if (!m->player) { destroy_movie(m); return MEMORY_ERROR; }
    *out = m;
    return 0;
}
#pragma clang diagnostic pop

static int new_movie(const uint32_t *a)
{
    uint32_t *out = guest_range(a[0], 4);
    if (!out) return PARAM_ERROR;
    *out = 0;
    /* newMovieActive, no resource IDs/names, and no data-reference output. */
    if (a[2] || a[3] || a[4] != 1 || a[5]) return PARAM_ERROR;
    char *path = quicktime_support32_file_path((int16_t)a[1]);
    if (!path) return FILE_ERROR;
    struct movie *m = NULL;
    int status = load_movie(path, &m);
    if (!status) {
        pthread_mutex_lock(&movies_lock);
        if (movie_count == MAX_MOVIES || next_movie == 0xf9400000u) status = MEMORY_ERROR;
        else {
            m->handle = next_movie++;
            m->next = movies; movies = m; ++movie_count;
            *out = m->handle;
            const char *file = strrchr(path, '/');
            fprintf(stderr, "compat32: QuickTime shim opened %s %dx%d samples=%zu duration=%.3fs\n",
                file ? file + 1 : path, m->width, m->height, m->sample_count,
                (double)m->duration / m->time_scale);
        }
        pthread_mutex_unlock(&movies_lock);
        if (status) destroy_movie(m);
    }
    free(path);
    return status;
}

static int copy_frame(struct movie *m)
{
    if (m->seek) {
        if (!atomic_load(&m->seek->done)) return 0;
        bool succeeded = atomic_load(&m->seek->succeeded);
        release_seek(m->seek); m->seek = NULL;
        if (!succeeded) return MOVIE_ERROR;
        /* QuickTime keeps a started movie running across a loop seek.
           AVPlayer pauses at EOF, so restore the requested playback state
           after the seek completes; an explicit StopMovie still wins. */
        if (m->started) [m->player play];
    }
    if (m->item.status == AVPlayerItemStatusFailed || m->player.status == AVPlayerStatusFailed)
        return MOVIE_ERROR;
    if (!m->world) return 0;
    CMTime time = m->player.currentTime;
    CVPixelBufferRef pixels = [m->output copyPixelBufferForItemTime:time itemTimeForDisplay:NULL];
    if (pixels) {
        if (m->last_pixels) CVPixelBufferRelease(m->last_pixels);
        m->last_pixels = pixels;
    }
    /* Absence of a newly decoded frame is not EOF. Keep the last visible
       frame during decoder startup, frame repeats and a paused movie. */
    if (!m->last_pixels) return 0;
    if (CVPixelBufferGetPixelFormatType(m->last_pixels) != kCVPixelFormatType_32BGRA ||
        CVPixelBufferGetWidth(m->last_pixels) != (size_t)m->width ||
        CVPixelBufferGetHeight(m->last_pixels) != (size_t)m->height)
        return MOVIE_ERROR;
    CVReturn locked = CVPixelBufferLockBaseAddress(m->last_pixels, kCVPixelBufferLock_ReadOnly);
    if (locked) return MOVIE_ERROR;
    int result = quicktime_support32_world_write(m->world,
        CVPixelBufferGetBaseAddress(m->last_pixels),
        CVPixelBufferGetBytesPerRow(m->last_pixels), m->width, m->height);
    CVPixelBufferUnlockBaseAddress(m->last_pixels, kCVPixelBufferLock_ReadOnly);
    if (!result && !m->logged_frame) {
        fprintf(stderr, "compat32: QuickTime shim decoded BGRA movie=%08x frame=%dx%d\n",
            m->handle, m->width, m->height);
        m->logged_frame = true;
    }
    return result;
}

static int interesting_time(struct movie *m, const uint32_t *a)
{
    int32_t *out = guest_range(a[6], 4);
    int32_t *duration = a[7] ? guest_range(a[7], 4) : NULL;
    const uint32_t *types = guest_range(a[3], 4);
    if (!out || (a[7] && !duration) || !types) return PARAM_ERROR;
    *out = -1; if (duration) *duration = 0;
    uint16_t flags = (uint16_t)a[1];
    if (a[2] != 1 || *types != 0x65796573u || (int32_t)a[5] <= 0 ||
        !(flags & (1 | 16)) || (flags & ~(0x4000 | 1 | 16))) return PARAM_ERROR;
    bool inclusive = (flags & 0x4000) != 0;
    int32_t from = (int32_t)a[4];
    size_t lo = 0, hi = m->sample_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->sample_times[mid] < from || (!inclusive && m->sample_times[mid] == from))
            lo = mid + 1;
        else hi = mid;
    }
    if (lo == m->sample_count) return INVALID_TIME;
    *out = m->sample_times[lo];
    if (duration) *duration = (lo + 1 < m->sample_count ? m->sample_times[lo + 1] : m->duration) - *out;
    return 0;
}

int quicktime_bridge32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    static const char *const supported[] = {
        "EnterMovies", "ExitMovies", "NewMovieFromFile", "DisposeMovie",
        "GetMovieDuration", "GetMovieNaturalBoundsRect", "GetMoviePreferredRate",
        "GetMovieTimeScale", "GetMovieNextInterestingTime", "GetMoviesError",
        "MoviesTask", "UpdateMovie", "SetMovieTimeValue", "SetMovieVolume",
        "StartMovie", "StopMovie", "SetMovieGWorld", "Fix2Long",
        "QTAudioContextCreateForAudioDevice", "QTAudioContextRelease", "SetMovieAudioContext"
    };
    if (*name == '_') ++name;
    bool found = false;
    for (size_t i = 0; i < sizeof(supported) / sizeof(*supported); ++i)
        if (!strcmp(name, supported[i])) { found = true; break; }
    if (!found) return 0;
    @autoreleasepool {
        /* The legacy API cooperatively services movie work. AVPlayer's
           readiness and seek notifications also need a live main run loop.
           Pump before taking our lock because an event may reenter a bridge. */
        if (!strcmp(name, "MoviesTask") && [NSThread isMainThread])
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        *result = 0;
        int status = 0;
        if (!strcmp(name, "GetMoviesError")) {
            *result = (uint32_t)(int32_t)last_error; return 1;
        }
        if (!strcmp(name, "EnterMovies")) {
            /* Success means the replacement media service is available,
               not that a missing legacy QuickTime framework initialized. */
            status = NSClassFromString(@"AVPlayer") ? 0 : MOVIE_ERROR;
        } else if (!strcmp(name, "NewMovieFromFile")) status = new_movie(a);
        else if (!strcmp(name, "Fix2Long")) {
            *result = (uint32_t)(((int64_t)(int32_t)a[0] + 0x8000) >> 16);
        } else {
            pthread_mutex_lock(&movies_lock);
            if (!strcmp(name, "ExitMovies")) {
                while (movies) {
                    struct movie *m = movies; movies = m->next; destroy_movie(m);
                }
                movie_count = 0;
                while (audio_contexts) {
                    struct audio_context *c = audio_contexts;
                    audio_contexts = c->next; free(c);
                }
            } else if (!strcmp(name, "QTAudioContextCreateForAudioDevice")) {
                uint32_t *out = guest_range(a[3], 4);
                if (!out) status = PARAM_ERROR;
                else {
                    *out = 0;
                    if (a[0] || a[1] || a[2]) status = PARAM_ERROR;
                    else {
                        struct audio_context *c = calloc(1, sizeof(*c));
                        if (!c || next_audio == 0xf9500000u) { free(c); status = MEMORY_ERROR; }
                        else {
                            c->handle = next_audio++; c->next = audio_contexts;
                            audio_contexts = c; *out = c->handle;
                        }
                    }
                }
            } else if (!strcmp(name, "QTAudioContextRelease")) {
                struct audio_context **link = &audio_contexts;
                while (*link && (*link)->handle != a[0]) link = &(*link)->next;
                if (*link) {
                    struct audio_context *c = *link; *link = c->next; free(c);
                    for (struct movie *m = movies; m; m = m->next)
                        if (m->audio_context == a[0]) m->audio_context = 0;
                } else if (a[0]) status = PARAM_ERROR;
            } else {
                struct movie *m = find_movie(a[0]);
                if (!m) status = PARAM_ERROR;
                else if (!strcmp(name, "DisposeMovie")) {
                    struct movie **link = &movies;
                    while (*link != m) link = &(*link)->next;
                    *link = m->next; --movie_count; destroy_movie(m);
                } else if (!strcmp(name, "GetMovieDuration")) *result = (uint32_t)m->duration;
                else if (!strcmp(name, "GetMovieTimeScale")) *result = (uint32_t)m->time_scale;
                else if (!strcmp(name, "GetMoviePreferredRate")) *result = 0x10000;
                else if (!strcmp(name, "GetMovieNaturalBoundsRect")) {
                    int16_t *rect = guest_range(a[1], 8);
                    if (!rect) status = PARAM_ERROR;
                    else { rect[0] = rect[1] = 0; rect[2] = m->height; rect[3] = m->width; }
                } else if (!strcmp(name, "GetMovieNextInterestingTime")) status = interesting_time(m, a);
                else if (!strcmp(name, "SetMovieGWorld")) {
                    int width, height; size_t stride;
                    if (a[2] || (a[1] && (quicktime_support32_world_info(a[1], &width, &height, &stride) ||
                        width != m->width || height != m->height))) status = PARAM_ERROR;
                    else m->world = a[1];
                } else if (!strcmp(name, "SetMovieAudioContext")) {
                    if (a[1] && !find_audio(a[1])) status = PARAM_ERROR;
                    else m->audio_context = a[1];
                } else if (!strcmp(name, "SetMovieVolume")) {
                    float volume = (int16_t)a[1] / 256.0f;
                    m->player.volume = fminf(1, fmaxf(0, volume));
                } else if (!strcmp(name, "StartMovie")) { m->started = true; [m->player play]; }
                else if (!strcmp(name, "StopMovie")) { m->started = false; [m->player pause]; }
                else if (!strcmp(name, "SetMovieTimeValue")) {
                    int32_t tick = (int32_t)a[1];
                    if (tick < 0 || tick > m->duration) status = INVALID_TIME;
                    else {
                        struct seek_state *seek = calloc(1, sizeof(*seek));
                        if (!seek) status = MEMORY_ERROR;
                        else {
                            atomic_init(&seek->references, 2);
                            atomic_init(&seek->done, false);
                            atomic_init(&seek->succeeded, false);
                            release_seek(m->seek); m->seek = seek;
                            if (m->last_pixels) { CVPixelBufferRelease(m->last_pixels); m->last_pixels = NULL; }
                            /* Until completion, video output may still offer
                               a pre-seek frame. Do not copy it into the world.
                               Callback state outlives movie disposal. */
                            [m->player seekToTime:CMTimeMake(tick, m->time_scale)
                                toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
                                completionHandler:^(BOOL finished) {
                                    atomic_store(&seek->succeeded, finished);
                                    atomic_store(&seek->done, true);
                                    release_seek(seek);
                                }];
                        }
                    }
                } else if (!strcmp(name, "UpdateMovie") || !strcmp(name, "MoviesTask")) status = copy_frame(m);
            }
            pthread_mutex_unlock(&movies_lock);
        }
        last_error = (int16_t)status;
        if (status) *result = (uint32_t)(int32_t)status;
        return 1;
    }
}
