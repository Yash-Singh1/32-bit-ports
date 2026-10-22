/* Exercise the exact guest32 records and real file lifecycle. The decoder
   belongs to quicktime_bridge; this test covers its guest pixel/file boundary. */
#include "quicktime_support.h"
#include "compat_runtime.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned char *arena;
static size_t used;
static unsigned frees;
static const uint32_t guard = 0x1234abcd;
static char callback_name[64];
struct allocation { uint32_t address; size_t size; bool freed; };
static struct allocation allocations[512];
static unsigned allocation_count;

uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    size_t offset = (used + 15) & ~(size_t)15;
    CHECK(offset + size + 32 < 8 * 1024 * 1024 && allocation_count < 512);
    unsigned char *p = arena + offset + 16;
    memcpy(p - 4, &guard, 4); memcpy(p + size, &guard, 4);
    memset(p, clear ? 0 : 0x5a, size);
    used = offset + size + 20;
    uint32_t address = (uint32_t)(uintptr_t)p;
    CHECK((uintptr_t)p + size <= UINT32_MAX);
    allocations[allocation_count++] = (struct allocation){address, size, false};
    return address;
}

void compat_runtime32_deallocate(uint32_t address)
{
    for (unsigned i = 0; i < allocation_count; ++i) {
        if (allocations[i].address != address) continue;
        CHECK(!allocations[i].freed);
        allocations[i].freed = true;
        memset((void *)(uintptr_t)address, 0xdd, allocations[i].size);
        ++frees; return;
    }
    CHECK(false);
}

uint32_t compat_runtime32_guest_callback(const char *name)
{
    CHECK(strlen(name) < sizeof(callback_name));
    strcpy(callback_name, name);
    return 0x13579;
}

static uint32_t low(size_t size) { return compat_runtime32_allocate(size, 1); }
static int32_t call(const char *name, const uint32_t *a)
{
    uint64_t result = UINT64_MAX;
    CHECK(quicktime_support32_dispatch(name, a, &result) && result <= UINT32_MAX);
    return (int32_t)result;
}

static uint32_t world(int width, int height)
{
    uint32_t output = low(8), rectangle = low(8);
    uint32_t *out = (void *)(uintptr_t)output; out[1] = guard;
    int16_t rect[] = {0, 0, (int16_t)height, (int16_t)width};
    memcpy((void *)(uintptr_t)rectangle, rect, sizeof(rect));
    uint32_t a[] = {output, 0x42475241, rectangle, 0, 0, 0};
    CHECK(!call("_QTNewGWorld", a) && out[0] && out[1] == guard);
    return out[0];
}

static void files(void)
{
    char name[] = "/private/tmp/lp32-quicktime-support-XXXXXX";
    int descriptor = mkstemp(name); CHECK(descriptor >= 0); CHECK(!close(descriptor));
    uint32_t path = low(strlen(name) + 1), ref = low(84), spec = low(74), isdir = low(5), out = low(6);
    strcpy((void *)(uintptr_t)path, name);
    memcpy((void *)(uintptr_t)(ref + 80), &guard, 4);
    memcpy((void *)(uintptr_t)(spec + 70), &guard, 4);
    memcpy((void *)(uintptr_t)(isdir + 1), &guard, 4);
    memcpy((void *)(uintptr_t)(out + 2), &guard, 4);
    uint32_t a[] = {path, ref, isdir};
    CHECK(!call("_FSPathMakeRef", a) && !*(uint8_t *)(uintptr_t)isdir);
    CHECK(!memcmp((void *)(uintptr_t)(ref + 80), &guard, 4));
    CHECK(!memcmp((void *)(uintptr_t)(isdir + 1), &guard, 4));
    uint32_t catalog[] = {ref, 0, 0, 0, spec, 0};
    CHECK(!call("_FSGetCatalogInfo", catalog));
    CHECK(!memcmp((void *)(uintptr_t)(spec + 70), &guard, 4));
    uint32_t open[] = {spec, out, 1};
    CHECK(!call("_OpenMovieFile", open));
    CHECK(!memcmp((void *)(uintptr_t)(out + 2), &guard, 4));
    int16_t first; memcpy(&first, (void *)(uintptr_t)out, 2);
    CHECK(first > 0);
    char *copy = quicktime_support32_file_path(first);
    char resolved[4096]; CHECK(realpath(name, resolved));
    CHECK(copy && !strcmp(copy, resolved)); free(copy);
    uint32_t close_args[] = {(uint32_t)(int32_t)first};
    CHECK(!call("_CloseMovieFile", close_args));
    CHECK(!quicktime_support32_file_path(first));
    CHECK(call("_CloseMovieFile", close_args) == -50);
    CHECK(!call("_OpenMovieFile", open));
    int16_t second; memcpy(&second, (void *)(uintptr_t)out, 2);
    CHECK(second > first && !quicktime_support32_file_path(first));
    close_args[0] = (uint32_t)(int32_t)second;
    CHECK(!call("_CloseMovieFile", close_args));
    open[2] = 2; CHECK(call("_OpenMovieFile", open) == -50); open[2] = 1;
    ((uint8_t *)(uintptr_t)spec)[7] ^= 1;
    CHECK(call("_OpenMovieFile", open) == -50);
    ((uint8_t *)(uintptr_t)spec)[7] ^= 1;
    catalog[1] = 1; CHECK(call("_FSGetCatalogInfo", catalog) == -50); catalog[1] = 0;
    catalog[4] = 0xfffffff0; CHECK(call("_FSGetCatalogInfo", catalog) == -50);
    open[1] = 0xffffffff; CHECK(call("_OpenMovieFile", open) == -50);
    a[1] = 0xfffffff0; CHECK(call("_FSPathMakeRef", a) == -50);
    CHECK(!unlink(name));
    a[1] = ref; CHECK(call("_FSPathMakeRef", a) == -43);
}

static void pixels(void)
{
    uint32_t token = world(3, 2), a[] = {token};
    uint32_t pixmap = (uint32_t)call("_GetGWorldPixMap", a); CHECK(pixmap);
    a[0] = pixmap;
    uint32_t address = (uint32_t)call("_GetPixBaseAddr", a); CHECK(address);
    CHECK(call("_GetPixRowBytes", a) == 12);
    int width = 0, height = 0; size_t stride = 0;
    CHECK(!quicktime_support32_world_info(token, &width, &height, &stride));
    CHECK(width == 3 && height == 2 && stride == 12);
    uint32_t gamma[] = {pixmap,0x23333};
    CHECK(!call("_QTSetPixMapHandleGammaLevel", gamma));
    CHECK(!call("_QTSetPixMapHandleRequestedGammaLevel", gamma));
    gamma[1] = 0x10000; CHECK(call("_QTSetPixMapHandleGammaLevel", gamma) == -50);
    uint8_t source[32]; for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i + 1);
    CHECK(!quicktime_support32_world_write(token, source, 16, 3, 2));
    CHECK(!memcmp((void *)(uintptr_t)address, source, 12));
    CHECK(!memcmp((void *)(uintptr_t)(address + 12), source + 16, 12));
    CHECK(quicktime_support32_world_write(token, source, 11, 3, 2) == -50);
    CHECK(quicktime_support32_world_write(token, source, 16, 2, 2) == -50);
    CHECK(call("_LockPixels", a) == 1 && call("_LockPixels", a) == 1);
    CHECK(quicktime_support32_world_write(token, source, 16, 3, 2) == -50);
    unsigned before = frees; uint32_t dispose[] = {token};
    CHECK(!call("_DisposeGWorld", dispose) && frees == before);
    CHECK(quicktime_support32_world_info(token, NULL, NULL, NULL) == -50);
    CHECK(!call("_LockPixels", a));
    CHECK((uint32_t)call("_GetPixBaseAddr", a) == address);
    CHECK(!call("_UnlockPixels", a) && frees == before);
    CHECK(!call("_UnlockPixels", a) && frees == before + 1);
    CHECK(!call("_GetPixBaseAddr", a) && !call("_GetPixRowBytes", a));
    CHECK(!call("_DisposeGWorld", dispose) && frees == before + 1);
    uint32_t replacement = world(3, 2); CHECK(replacement != token);
    CHECK(!call("_GetGWorldPixMap", dispose));
    dispose[0] = replacement; CHECK(!call("_DisposeGWorld", dispose));
    uint32_t output = low(8), rectangle = low(8);
    int16_t rect[] = {0,0,2,3}; memcpy((void *)(uintptr_t)rectangle, rect, sizeof(rect));
    uint32_t invalid[] = {output,0x42475241,rectangle,0,0,0};
    invalid[0] = 0xffffffff; CHECK(call("_QTNewGWorld", invalid) == -50); invalid[0] = output;
    invalid[1] = 0; CHECK(call("_QTNewGWorld", invalid) == -50); invalid[1] = 0x42475241;
    invalid[5] = 1; CHECK(call("_QTNewGWorld", invalid) == -50); invalid[5] = 0;
    rect[3] = 0; memcpy((void *)(uintptr_t)rectangle, rect, sizeof(rect));
    CHECK(call("_QTNewGWorld", invalid) == -50);
}

int main(void)
{
    arena = mmap((void *)(uintptr_t)0x40000000, 8 * 1024 * 1024,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    CHECK(arena != MAP_FAILED);
    uint32_t handle = quicktime_support32_open("/System/Library/Frameworks/QuickTime.framework/Versions/A/QuickTime");
    CHECK(handle == LP32_QUICKTIME_FRAMEWORK_HANDLE);
    CHECK(!quicktime_support32_open("/usr/lib/QuickTime"));
    const char *symbols[] = {"DisposeGWorld","GetGWorldPixMap","LockPixels","UnlockPixels",
                             "GetPixBaseAddr","GetPixRowBytes","SetGWorld"};
    for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
        CHECK(quicktime_support32_symbol(handle, symbols[i]) == 0x13579);
        CHECK(callback_name[0] == '_' && !strcmp(callback_name + 1, symbols[i]));
    }
    CHECK(!quicktime_support32_symbol(handle, "unsupported"));
    CHECK(!quicktime_support32_symbol(1, "LockPixels"));
    files(); pixels();
    for (unsigned i = 0; i < allocation_count; ++i) {
        CHECK(!memcmp((void *)(uintptr_t)(allocations[i].address - 4), &guard, 4));
        CHECK(!memcmp((void *)(uintptr_t)(allocations[i].address + allocations[i].size), &guard, 4));
    }
    CHECK(!munmap(arena, 8 * 1024 * 1024));
    puts("quicktime-support self-test: PASS");
    return 0;
}
