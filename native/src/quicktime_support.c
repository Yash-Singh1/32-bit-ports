#include "quicktime_support.h"
#include "compat_runtime.h"

#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The exercised Mac Source movie path uses opaque FSRef/PixMap values, the
   70-byte packed FSSpec, 8-byte QuickDraw Rect, signed16 movie file refs and
   guest32 output pointers. Never pass those records to a pointer-sized host
   QuickTime API. The real decoder is the separate AVFoundation movie bridge. */
enum { parameter_error = -50, file_not_found = -43, memory_full = -108,
       path_limit = 1024, file_limit = 64, world_limit = 64 };
static const char framework_path[] =
    "/System/Library/Frameworks/QuickTime.framework/Versions/A/QuickTime";
static const char *const dynamic_functions[] = {
    "DisposeGWorld", "GetGWorldPixMap", "LockPixels", "UnlockPixels",
    "GetPixBaseAddr", "GetPixRowBytes", "SetGWorld"
};
static pthread_mutex_t support_lock = PTHREAD_MUTEX_INITIALIZER;
static char *paths[path_limit];
static unsigned path_count;
static struct { int16_t reference; unsigned path; int descriptor; } files[file_limit];
static uint32_t next_file_reference = 1, next_world = 1;
struct world {
    uint32_t token, pixmap, pixels;
    int width, height;
    size_t row_bytes;
    unsigned locks;
    bool disposed;
    int32_t gamma, requested_gamma;
};
static struct world worlds[world_limit];
static _Thread_local uint32_t current_world;

static bool guest_range(uint32_t address, size_t size)
{
    return address && (uint64_t)address + size <= UINT64_C(0x100000000);
}

uint32_t quicktime_support32_open(const char *path)
{
    return path && !strcmp(path, framework_path) ? LP32_QUICKTIME_FRAMEWORK_HANDLE : 0;
}

uint32_t quicktime_support32_symbol(uint32_t handle, const char *name)
{
    if (handle != LP32_QUICKTIME_FRAMEWORK_HANDLE || !name) return 0;
    for (unsigned i = 0; i < sizeof(dynamic_functions) / sizeof(dynamic_functions[0]); ++i) {
        if (strcmp(name, dynamic_functions[i])) continue;
        char import[64];
        import[0] = '_';
        strcpy(import + 1, name);
        return compat_runtime32_guest_callback(import);
    }
    return 0;
}

static int path_reference(const uint32_t *a)
{
    if (!guest_range(a[0], 1) || !guest_range(a[1], 80) ||
        (a[2] && !guest_range(a[2], 1))) return parameter_error;
    const char *path = (const void *)(uintptr_t)a[0];
    size_t capacity = UINT64_C(0x100000000) - a[0];
    if (capacity > PATH_MAX) capacity = PATH_MAX;
    if (strnlen(path, capacity) == capacity) return parameter_error;
    char resolved[PATH_MAX];
    struct stat info;
    if (!realpath(path, resolved) || stat(resolved, &info)) return file_not_found;
    pthread_mutex_lock(&support_lock);
    unsigned index;
    for (index = 0; index < path_count; ++index)
        if (!strcmp(paths[index], resolved)) break;
    if (index == path_count) {
        if (path_count == path_limit || !(paths[index] = strdup(resolved))) {
            pthread_mutex_unlock(&support_lock); return memory_full;
        }
        ++path_count;
    }
    /* FSRef is an opaque 80-byte identifier. These internal tokens are valid
       only for this scoped FSGetCatalogInfo/OpenMovieFile path. */
    uint32_t reference[20] = {UINT32_C(0x4c503246), index + 1};
    memcpy((void *)(uintptr_t)a[1], reference, sizeof(reference));
    if (a[2]) *(uint8_t *)(uintptr_t)a[2] = S_ISDIR(info.st_mode);
    pthread_mutex_unlock(&support_lock);
    return 0;
}

static int catalog_info(const uint32_t *a)
{
    if (!guest_range(a[0], 80) || a[1] || a[2] || a[3] || a[5] ||
        !guest_range(a[4], 70)) return parameter_error;
    uint32_t reference[20]; memcpy(reference, (const void *)(uintptr_t)a[0], sizeof(reference));
    if (reference[0] != UINT32_C(0x4c503246) || !reference[1]) return parameter_error;
    for (unsigned i = 2; i < 20; ++i) if (reference[i]) return parameter_error;
    pthread_mutex_lock(&support_lock);
    unsigned index = reference[1] - 1;
    if (index >= path_count) { pthread_mutex_unlock(&support_lock); return parameter_error; }
    const char *name = strrchr(paths[index], '/');
    name = name ? name + 1 : paths[index];
    size_t length = strlen(name);
    if (length > 63) length = 63;
    unsigned char spec[70] = {0};
    int16_t volume = INT16_MIN;
    memcpy(spec, &volume, sizeof(volume));
    memcpy(spec + 2, &reference[1], sizeof(reference[1]));
    spec[6] = (unsigned char)length;
    memcpy(spec + 7, name, length);
    memcpy((void *)(uintptr_t)a[4], spec, sizeof(spec));
    pthread_mutex_unlock(&support_lock);
    return 0;
}

static int open_movie_file(const uint32_t *a)
{
    if (!guest_range(a[0], 70) || !guest_range(a[1], 2) || a[2] != 1) return parameter_error;
    const unsigned char *spec = (const void *)(uintptr_t)a[0];
    int16_t volume; uint32_t path;
    memcpy(&volume, spec, sizeof(volume)); memcpy(&path, spec + 2, sizeof(path));
    if (volume != INT16_MIN || !path) return parameter_error;
    pthread_mutex_lock(&support_lock);
    unsigned slot;
    for (slot = 0; slot < file_limit; ++slot) if (!files[slot].reference) break;
    if (path > path_count || slot == file_limit || next_file_reference > INT16_MAX) {
        pthread_mutex_unlock(&support_lock); return path > path_count ? parameter_error : memory_full;
    }
    const char *name = strrchr(paths[path - 1], '/');
    name = name ? name + 1 : paths[path - 1];
    size_t length = strlen(name);
    if (length > 63) length = 63;
    if (spec[6] != length || memcmp(spec + 7, name, length)) {
        pthread_mutex_unlock(&support_lock); return parameter_error;
    }
    struct stat info;
    if (stat(paths[path - 1], &info) || !S_ISREG(info.st_mode)) {
        pthread_mutex_unlock(&support_lock); return file_not_found;
    }
    int descriptor = open(paths[path - 1], O_RDONLY);
    if (descriptor < 0) {
        int status = errno == EACCES ? -54 : file_not_found;
        pthread_mutex_unlock(&support_lock); return status;
    }
    int16_t reference = (int16_t)next_file_reference++;
    files[slot].reference = reference; files[slot].path = path - 1;
    files[slot].descriptor = descriptor;
    memcpy((void *)(uintptr_t)a[1], &reference, sizeof(reference));
    pthread_mutex_unlock(&support_lock);
    return 0;
}

char *quicktime_support32_file_path(int16_t reference)
{
    char *path = NULL;
    pthread_mutex_lock(&support_lock);
    for (unsigned i = 0; i < file_limit; ++i)
        if (reference > 0 && files[i].reference == reference) {
            path = strdup(paths[files[i].path]); break;
        }
    pthread_mutex_unlock(&support_lock);
    return path;
}

static int close_movie_file(int16_t reference)
{
    int status = parameter_error;
    pthread_mutex_lock(&support_lock);
    for (unsigned i = 0; i < file_limit; ++i)
        if (reference > 0 && files[i].reference == reference) {
            close(files[i].descriptor);
            files[i].reference = 0; status = 0; break;
        }
    pthread_mutex_unlock(&support_lock);
    return status;
}

static struct world *find_world(uint32_t token, bool pixmap)
{
    for (unsigned i = 0; i < world_limit; ++i)
        if (worlds[i].token && (pixmap ? worlds[i].pixmap : worlds[i].token) == token)
            return worlds + i;
    return NULL;
}

static void release_world(struct world *world)
{
    compat_runtime32_deallocate(world->pixels);
    memset(world, 0, sizeof(*world));
}

static int new_world(const uint32_t *a)
{
    if (!guest_range(a[0], 4) || a[1] != UINT32_C(0x42475241) ||
        !guest_range(a[2], 8) || a[3] || a[4] || a[5]) return parameter_error;
    int16_t rect[4]; memcpy(rect, (const void *)(uintptr_t)a[2], sizeof(rect));
    int width = rect[3] - rect[1], height = rect[2] - rect[0];
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return parameter_error;
    size_t stride = (size_t)width * 4, bytes = stride * (size_t)height;
    pthread_mutex_lock(&support_lock);
    unsigned slot;
    for (slot = 0; slot < world_limit; ++slot) if (!worlds[slot].token) break;
    if (slot == world_limit || next_world >= UINT32_C(0x100000)) {
        pthread_mutex_unlock(&support_lock); return memory_full;
    }
    uint32_t pixels = compat_runtime32_allocate(bytes, 1);
    if (!pixels) { pthread_mutex_unlock(&support_lock); return memory_full; }
    if (!guest_range(pixels, bytes)) {
        compat_runtime32_deallocate(pixels); pthread_mutex_unlock(&support_lock); return memory_full;
    }
    uint32_t serial = next_world++;
    struct world world = {UINT32_C(0xf9100000) + serial, UINT32_C(0xf9200000) + serial,
        pixels, width, height, stride, 0, false, 0x23333, 0x23333};
    worlds[slot] = world;
    memcpy((void *)(uintptr_t)a[0], &world.token, sizeof(world.token));
    pthread_mutex_unlock(&support_lock);
    return 0;
}

int quicktime_support32_world_info(uint32_t token, int *width, int *height, size_t *stride)
{
    pthread_mutex_lock(&support_lock);
    struct world *world = find_world(token, false);
    int status = world && !world->disposed ? 0 : parameter_error;
    if (!status) {
        if (width) *width = world->width;
        if (height) *height = world->height;
        if (stride) *stride = world->row_bytes;
    }
    pthread_mutex_unlock(&support_lock);
    return status;
}

int quicktime_support32_world_write(uint32_t token, const void *pixels,
                                     size_t source_stride, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / 4 ||
        source_stride < (size_t)width * 4 || (size_t)height > SIZE_MAX / source_stride)
        return parameter_error;
    pthread_mutex_lock(&support_lock);
    struct world *world = find_world(token, false);
    int status = !world || world->disposed || world->locks ||
        world->width != width || world->height != height ? parameter_error : 0;
    if (!status) {
        for (int y = 0; y < height; ++y)
            memcpy((unsigned char *)(uintptr_t)world->pixels + (size_t)y * world->row_bytes,
                    (const unsigned char *)pixels + (size_t)y * source_stride, world->row_bytes);
    }
    pthread_mutex_unlock(&support_lock);
    return status;
}

int quicktime_support32_dispatch(const char *name, const uint32_t *a, uint64_t *result)
{
    if (!strcmp(name, "_FSPathMakeRef")) { *result = (uint32_t)path_reference(a); return 1; }
    if (!strcmp(name, "_FSGetCatalogInfo")) { *result = (uint32_t)catalog_info(a); return 1; }
    if (!strcmp(name, "_OpenMovieFile")) { *result = (uint32_t)open_movie_file(a); return 1; }
    if (!strcmp(name, "_CloseMovieFile")) { *result = (uint32_t)close_movie_file((int16_t)a[0]); return 1; }
    if (!strcmp(name, "_QTNewGWorld")) { *result = (uint32_t)new_world(a); return 1; }
    bool dispose = !strcmp(name, "_DisposeGWorld");
    bool get_map = !strcmp(name, "_GetGWorldPixMap");
    bool lock = !strcmp(name, "_LockPixels"), unlock = !strcmp(name, "_UnlockPixels");
    bool base = !strcmp(name, "_GetPixBaseAddr"), stride = !strcmp(name, "_GetPixRowBytes");
    bool gamma = !strcmp(name, "_QTSetPixMapHandleGammaLevel");
    bool requested_gamma = !strcmp(name, "_QTSetPixMapHandleRequestedGammaLevel");
    if (!strcmp(name, "_SetGWorld")) {
        pthread_mutex_lock(&support_lock);
        struct world *world = a[0] ? find_world(a[0], false) : NULL;
        if (!a[1] && (!a[0] || (world && !world->disposed))) current_world = a[0];
        pthread_mutex_unlock(&support_lock); *result = 0; return 1;
    }
    if (!(dispose || get_map || lock || unlock || base || stride || gamma || requested_gamma)) return 0;
    pthread_mutex_lock(&support_lock);
    struct world *world = find_world(a[0], !(dispose || get_map));
    *result = (gamma || requested_gamma) ? (uint32_t)parameter_error : 0;
    if (world) {
        if (dispose) {
            world->disposed = true;
            if (current_world == a[0]) current_world = 0;
            if (!world->locks) release_world(world);
        } else if (get_map && !world->disposed) *result = world->pixmap;
        else if (lock && !world->disposed && world->locks != UINT_MAX) {
            ++world->locks; *result = 1;
        } else if (unlock && world->locks) {
            if (!--world->locks && world->disposed) release_world(world);
        } else if (base && (!world->disposed || world->locks)) *result = world->pixels;
        else if (stride && (!world->disposed || world->locks)) *result = world->row_bytes;
        /* AVFoundation supplies the exercised transfer-encoded BGRA path.
           Support the audited 2.2 metadata request; a different transfer curve
           would require an actual pixel conversion, so reject it honestly. */
        else if ((gamma || requested_gamma) && !world->disposed && a[1] == 0x23333) {
            if (gamma) world->gamma = (int32_t)a[1]; else world->requested_gamma = (int32_t)a[1];
            *result = 0;
        }
    }
    pthread_mutex_unlock(&support_lock);
    return 1;
}
