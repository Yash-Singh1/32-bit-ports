/* Original i386 calls through dynamic QuickDraw thunks and the QuickTime movie
   imports. Optional media is supplied externally; no Valve binary is copied. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>

extern int32_t FSPathMakeRef(const uint8_t *, void *, uint8_t *);
extern int32_t FSGetCatalogInfo(const void *, uint32_t, void *, void *, void *, void *);
extern int16_t OpenMovieFile(const void *, int16_t *, int8_t);
extern int16_t CloseMovieFile(int16_t);
extern int16_t QTNewGWorld(uint32_t *, uint32_t, const int16_t *, uint32_t, uint32_t, uint32_t);
extern int16_t QTSetPixMapHandleGammaLevel(uint32_t, int32_t);
extern int16_t QTSetPixMapHandleRequestedGammaLevel(uint32_t, int32_t);
extern int16_t EnterMovies(void);
extern void ExitMovies(void);
extern int16_t NewMovieFromFile(uint32_t *, int16_t, int16_t *, const uint8_t *, int16_t, uint8_t *);
extern void DisposeMovie(uint32_t);
extern int16_t GetMoviesError(void);
extern int32_t GetMovieTimeScale(uint32_t);
extern int32_t GetMovieDuration(uint32_t);
extern void GetMovieNaturalBoundsRect(uint32_t, int16_t *);
extern void GetMovieNextInterestingTime(uint32_t, int16_t, int16_t, const uint32_t *, int32_t,
                                       int32_t, int32_t *, int32_t *);
extern void SetMovieGWorld(uint32_t, uint32_t, uint32_t);
extern void SetMovieTimeValue(uint32_t, int32_t);
extern void StartMovie(uint32_t);
extern void StopMovie(uint32_t);
extern int16_t UpdateMovie(uint32_t);
extern void MoviesTask(uint32_t, int32_t);

static int16_t open_file(const char *path, int16_t *file)
{
    struct { uint8_t value[80]; uint32_t guard; } ref = {{0}, 0xabcd1234};
    struct { uint8_t value[70]; uint8_t guard[4]; } spec = {{0}, {0x87,0x65,0x43,0x21}};
    struct { uint8_t value; uint8_t guard[3]; } directory = {0, {0x12,0x34,0x56}};
    struct { int16_t value; uint16_t guard; } opened = {0,0x789a};
    if (FSPathMakeRef((const uint8_t *)path, ref.value, &directory.value) ||
        ref.guard != 0xabcd1234 || directory.value || directory.guard[0] != 0x12 ||
        directory.guard[1] != 0x34 || directory.guard[2] != 0x56) return -1;
    if (FSGetCatalogInfo(ref.value, 0, 0, 0, spec.value, 0) ||
        spec.guard[0] != 0x87 || spec.guard[1] != 0x65 || spec.guard[2] != 0x43 || spec.guard[3] != 0x21)
        return -2;
    if (OpenMovieFile(spec.value, &opened.value, 1) || opened.value <= 0 || opened.guard != 0x789a)
        return -3;
    *file = opened.value;
    return 0;
}

static uint32_t pixel_signature(const unsigned char *pixels, int stride, int width, int height)
{
    uint32_t hash = 2166136261u;
    /* Sample the full image, including each component, rather than accepting
       alpha alone as evidence of a progressing movie. */
    for (int y = 0; y < height; y += height / 31 + 1)
        for (int x = 0; x < width; x += width / 63 + 1)
            for (unsigned c = 0; c < 4; ++c)
                hash = (hash ^ pixels[(size_t)y * stride + (size_t)x * 4 + c]) * 16777619u;
    return hash;
}

int check_quicktime(void)
{
    if (!getenv("LP32_FIXTURE_QUICKTIME")) return 0;
    const char *framework = "/System/Library/Frameworks/QuickTime.framework/Versions/A/QuickTime";
    void *handle = dlopen(framework, RTLD_NOW);
    if (!handle || dlopen(framework, RTLD_NOW) != handle) return -951;
    void (*dispose)(uint32_t) = dlsym(handle, "DisposeGWorld");
    uint32_t (*get_map)(uint32_t) = dlsym(handle, "GetGWorldPixMap");
    uint8_t (*lock)(uint32_t) = dlsym(handle, "LockPixels");
    void (*unlock)(uint32_t) = dlsym(handle, "UnlockPixels");
    unsigned char *(*get_base)(uint32_t) = dlsym(handle, "GetPixBaseAddr");
    int32_t (*get_stride)(uint32_t) = dlsym(handle, "GetPixRowBytes");
    void (*set_world)(uint32_t,uint32_t) = dlsym(handle, "SetGWorld");
    if (!dispose || !get_map || !lock || !unlock || !get_base || !get_stride || !set_world)
        return -952;
    if (dlsym(handle, "unsupportedQuickDrawSymbol") || !dlerror() || dlerror()) return -953;
    int16_t file;
    if (open_file("quicktime-file.mov", &file) || CloseMovieFile(file) || !CloseMovieFile(file)) return -954;
    struct { uint32_t value, guard; } world = {0,0x12345678};
    int16_t rect[] = {0,0,3,5};
    if (QTNewGWorld(&world.value,0x42475241,rect,0,0,0) || !world.value || world.guard != 0x12345678)
        return -955;
    uint32_t map = get_map(world.value);
    if (!map || QTSetPixMapHandleGammaLevel(map,0x23333) ||
        QTSetPixMapHandleRequestedGammaLevel(map,0x23333) || get_stride(map) != 20) return -956;
    if (!lock(map) || !lock(map)) return -957;
    unsigned char *pixels = get_base(map);
    if (!pixels || (uintptr_t)pixels > UINT32_MAX - 60) return -958;
    for (unsigned i = 0; i < 60; ++i) if (pixels[i]) return -959;
    memset(pixels,0x5a,60);
    dispose(world.value);
    if (lock(map) || get_map(world.value) || get_base(map) != pixels) return -960;
    unlock(map);
    if (get_base(map) != pixels || pixels[59] != 0x5a) return -961;
    unlock(map);
    if (get_base(map) || get_stride(map) || lock(map)) return -962;
    dispose(world.value);
    set_world(0,0);
    if (dlclose(handle)) return -963;

    const char *media = getenv("LP32_QUICKTIME_TEST_MOVIE");
    if (!media || !*media) return 0;
    if (EnterMovies() || open_file(media,&file)) return -964;
    struct { uint32_t value, guard; } movie = {0,0x87654321};
    int16_t created = NewMovieFromFile(&movie.value,file,0,0,1,0);
    if (created) {
        printf("QuickTime fixture NewMovieFromFile status=%d lastError=%d file=%d\n",
                created,GetMoviesError(),file);
        return -965;
    }
    if (!movie.value || movie.guard != 0x87654321 || CloseMovieFile(file)) return -975;
    if (GetMovieTimeScale(movie.value) <= 0 || GetMovieDuration(movie.value) <= 0) return -966;
    GetMovieNaturalBoundsRect(movie.value,rect);
    int width = rect[3] - rect[1], height = rect[2] - rect[0];
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return -967;
    uint32_t type = 0x65796573;
    struct { int32_t value; uint32_t guard; } first = {-1,0xabcdef01}, next = {-1,0x10fedcba};
    GetMovieNextInterestingTime(movie.value,0x4001,1,&type,0,0x10000,&first.value,0);
    if (GetMoviesError() || first.value < 0 || first.guard != 0xabcdef01) return -968;
    GetMovieNextInterestingTime(movie.value,1,1,&type,first.value,0x10000,&next.value,0);
    if (GetMoviesError() || next.value <= first.value || next.guard != 0x10fedcba) return -969;
    world.value = 0;
    if (QTNewGWorld(&world.value,0x42475241,rect,0,0,0) || !world.value ||
        world.guard != 0x12345678) return -970;
    map = get_map(world.value);
    if (!lock(map)) return -972;
    pixels = get_base(map);
    int32_t stride = get_stride(map);
    if (!pixels || stride < width * 4) return -973;
    uint32_t empty = pixel_signature(pixels,stride,width,height);
    unlock(map);
    SetMovieGWorld(movie.value,world.value,0);
    SetMovieTimeValue(movie.value,0);
    StartMovie(movie.value);
    uint32_t first_frame = 0;
    int saw_frame = 0, progressed = 0;
    for (unsigned attempt = 0; !progressed && attempt < 200; ++attempt) {
        if (UpdateMovie(movie.value)) return -971;
        MoviesTask(movie.value,10);
        if (!lock(map)) return -972;
        pixels = get_base(map);
        stride = get_stride(map);
        if (!pixels || stride < width * 4) return -973;
        uint32_t hash = pixel_signature(pixels,stride,width,height);
        if (hash != empty) {
            if (!saw_frame) { first_frame = hash; saw_frame = 1; }
            else if (hash != first_frame) progressed = 1;
        }
        unlock(map);
        if (!progressed) usleep(20000);
    }
    StopMovie(movie.value);
    DisposeMovie(movie.value);
    dispose(world.value);
    ExitMovies();
    if (!progressed) printf("QuickTime fixture frozen pixels: sawFrame=%d firstHash=%08x\n",saw_frame,first_frame);
    return progressed ? 0 : -974;
}
