#include "objc_legacy_bridge.h"
#include "compat_runtime.h"
#include "carbon_bridge.h"
#include "controller_bridge.h"
#include "game_profile.h"
#include "hid_bridge.h"
#include "macho_loader.h"
#include "objc_bridge.h"
#include "quicktime_bridge.h"
#include "hitch_recorder.h"
#include "host_diagnostics.h"
#include "steam_bridge.h"
#include "audio_bridge.h"
#include "crash_output.h"

#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libproc.h>
#include <limits.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uintptr_t guest_breakpoint_address;
static unsigned char guest_breakpoint_original_byte;
static volatile sig_atomic_t guest_breakpoint_stepping;
static volatile sig_atomic_t guest_breakpoint_count;
static unsigned guest_breakpoint_limit;
static unsigned guest_breakpoint_skip;
static int guest_diagnostic_fd = -1;
static unsigned guest_crash_active;
static int guest_trace_native_crash;
static struct { uint32_t address; unsigned count; } guest_crash_dumps[16];
static unsigned guest_crash_dump_count;
static unsigned char guest_signal_stack[128 * 1024] __attribute__((aligned(16)));

/* Finder launches do not retain stderr.  Mirror crash and breakpoint output
   into a persistent per-run file without changing normal console logging. */
#define GUEST_DIAGNOSTIC(...)                                              \
    do {                                                                   \
        dprintf(STDERR_FILENO, __VA_ARGS__);                               \
        if (guest_diagnostic_fd >= 0) {                                    \
            dprintf(guest_diagnostic_fd, __VA_ARGS__);                     \
        }                                                                  \
    } while (0)

#define CRASH_DIAGNOSTIC(...) do { \
    if (guest_diagnostic_fd >= 0) \
        lp32_crash_writef(guest_diagnostic_fd, __VA_ARGS__); \
    lp32_crash_writef(STDERR_FILENO, __VA_ARGS__); \
} while (0)

static void flush_guest_instruction(uintptr_t address);

/*
 * Complete Saga loads each level's shaders as separate files from
 * feralshaders/ (4,276 files, 17 MiB) and its hub data from game.dat.  After a
 * reboot these reads miss the page cache; the loader thread holds an engine
 * lock through them, and the render thread stalls behind it during the fade
 * into the cantina.  Read them once at utility priority so the first load
 * finds them cached.  Episode archives are left to the OS.
 */
static size_t prefetch_file(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return 0;
    size_t total = 0;
    ssize_t count;
    while ((count = read(descriptor, buffer, capacity)) > 0) total += (size_t)count;
    close(descriptor);
    return total;
}

static void *prefetch_saga_data(void *raw)
{
    char *root = raw;
    char *buffer = malloc(1 << 20);
    if (!buffer) { free(root); return NULL; }
    uint64_t start = hitch_now();
    size_t bytes = 0, files = 0;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/feralshaders", root);
    DIR *directory = opendir(path);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (entry->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/feralshaders/%s", root, entry->d_name);
            bytes += prefetch_file(path, buffer, 1 << 20);
            ++files;
        }
        closedir(directory);
    }
    snprintf(path, sizeof(path), "%s/game.dat", root);
    bytes += prefetch_file(path, buffer, 1 << 20);
    ++files;
    GUEST_DIAGNOSTIC("compat32: prefetched %zu data files (%.1f MiB) in %.0f ms\n",
                     files, bytes / 1048576.0, (hitch_now() - start) / 1e6);
    free(buffer);
    free(root);
    return NULL;
}

static void start_saga_data_prefetch(void)
{
    if (lp32_profile()->title != LP32_TITLE_COMPLETE_SAGA ||
        getenv("LP32_NO_DATA_PREFETCH")) return;
    char executable[PATH_MAX], resolved[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) != 0 ||
        !realpath(executable, resolved)) return;
    /* <parent>/<App>.app/Contents/MacOS/<loader>: the data sits in <parent>. */
    for (int level = 0; level < 4; ++level) {
        char *slash = strrchr(resolved, '/');
        if (!slash) return;
        *slash = 0;
    }
    char root[PATH_MAX];
    struct stat info;
    if (snprintf(root, sizeof(root), "%s/LEGOStarWarsSagaData", resolved) >=
            (int)sizeof(root) || stat(root, &info) != 0 || !S_ISDIR(info.st_mode)) {
        return;
    }
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_UTILITY, 0);
    pthread_t thread;
    char *copy = strdup(root);
    if (copy && pthread_create(&thread, &attributes, prefetch_saga_data, copy) != 0) {
        free(copy);
    }
    pthread_attr_destroy(&attributes);
}

static void open_guest_diagnostic_log(const char *executable_path, bool startup)
{
    /* A read-only storage probe must not truncate a player's session log. */
    if (getenv("LP32_STEAM_STORAGE_PROBE") || getenv("LP32_NO_DIAGNOSTIC_LOG")) return;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char directory[PATH_MAX];
    char path[PATH_MAX];
    int length = snprintf(directory, sizeof(directory),
                          "%s/Library/Logs/%s", home,
                          startup ? "LP32Compat" : lp32_profile()->log_directory);
    if (length < 0 || (size_t)length >= sizeof(directory)) return;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        perror("compat32: create diagnostic log directory");
        return;
    }
    length = snprintf(path, sizeof(path), "%s/%s.log", directory,
                      startup ? "startup" : "last-run");
    if (length < 0 || (size_t)length >= sizeof(path)) return;

    if (guest_diagnostic_fd >= 0) close(guest_diagnostic_fd);
    guest_diagnostic_fd = open(path,
        O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    if (guest_diagnostic_fd < 0) {
        perror("compat32: open diagnostic log");
        return;
    }
    lp32_crash_writef(guest_diagnostic_fd,
            "compat32: diagnostic session pid=%ld executable=%s\n",
            (long)getpid(), executable_path ? executable_path : "(unknown)");
    fprintf(stderr, "compat32: persistent diagnostic log: %s\n", path);

    /* A Finder or Dock launch has stderr on /dev/null, which loses every
       bridge message a user's run would need to be diagnosed; keep them in
       the session log instead.  Terminal launches keep their stderr. */
    struct stat stderr_stat, null_stat;
    if (!startup && fstat(STDERR_FILENO, &stderr_stat) == 0 &&
        stat("/dev/null", &null_stat) == 0 &&
        stderr_stat.st_dev == null_stat.st_dev &&
        stderr_stat.st_ino == null_stat.st_ino) {
        dup2(guest_diagnostic_fd, STDERR_FILENO);
        setvbuf(stderr, NULL, _IONBF, 0);
        /* GUEST_DIAGNOSTIC writes to both; stderr now is the log. */
        close(guest_diagnostic_fd);
        guest_diagnostic_fd = -1;
        fprintf(stderr, "compat32: stderr redirected to the session log\n");
    }
}

static void emit_u8(unsigned char *code, size_t *offset, unsigned char value)
{
    code[(*offset)++] = value;
}

static void emit_u32(unsigned char *code, size_t *offset, uint32_t value)
{
    memcpy(code + *offset, &value, sizeof(value));
    *offset += sizeof(value);
}

static void emit_rel32(unsigned char *code, size_t *offset,
                       uintptr_t code_address, uintptr_t target)
{
    intptr_t displacement = (intptr_t)target -
                            (intptr_t)(code_address + *offset + 4);
    int32_t relative = (int32_t)displacement;
    memcpy(code + *offset, &relative, sizeof(relative));
    *offset += sizeof(relative);
}

/*
 * During frontend state 2 the title maps any physical-input notification to
 * action 6 (the splash dismiss action).  The old input backend only emitted a
 * notification when a control changed.  Its device-independent polling path
 * emits the seven currently sampled controls every frame on this host, which
 * repeatedly restarts the dismiss fade and leaves the legal splash cycling.
 *
 * Keep the original first notification, latch only while state 2 is active,
 * and reset the latch in every other state.  The patch lives solely in the
 * recovered workspace image and the compatibility bridge; the source bundle
 * is never modified.
 */
static int install_startup_input_latch_patch(void)
{
    const struct lp32_startup_latch_patch *layout = lp32_profile()->startup_latch;
    if (!layout) return 0;

    unsigned char expected[] = {
        0x84, 0xc0,             /* testb %al, %al */
        0x74, 0x00,             /* je original_frontend */
        0x83, 0xfe, 0x02,       /* cmpl $2, %esi */
    };
    expected[3] = (unsigned char)(layout->original_frontend - (layout->hook + 4));
    enum {
        kStubAddress = 0x7f00f000,
        kLatchAddress = 0x7f01fff0,
    };
    const uint32_t kHookAddress = layout->hook;
    const uint32_t kStateTrueAddress = layout->state_true;
    const uint32_t kOriginalFrontendAddress = layout->original_frontend;
    const uint32_t kFunctionEpilogueAddress = layout->function_epilogue;
    const uint32_t kStatePointerAddress = layout->state_pointer;

    unsigned char *hook = (void *)(uintptr_t)kHookAddress;
    unsigned char *stub = (void *)(uintptr_t)kStubAddress;
    volatile unsigned char *latch = (void *)(uintptr_t)kLatchAddress;
    if (memcmp(hook, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "compat32: startup input patch signature mismatch\n");
        return -1;
    }

    unsigned char code[96];
    size_t offset = 0;

    /* Preserve the overwritten state test and branch. */
    emit_u8(code, &offset, 0x84); emit_u8(code, &offset, 0xc0); /* test al,al */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x85); /* jne */
    size_t state_true_relative = offset;
    emit_u32(code, &offset, 0);

    /* The static pointer owns the frontend-state object used by 0x8e6430. */
    emit_u8(code, &offset, 0xa1); /* mov eax, [absolute] */
    emit_u32(code, &offset, kStatePointerAddress);
    emit_u8(code, &offset, 0x83); emit_u8(code, &offset, 0x38);
    emit_u8(code, &offset, 0x02); /* cmp dword ptr [eax], 2 */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x85); /* jne reset */
    size_t reset_relative = offset;
    emit_u32(code, &offset, 0);

    emit_u8(code, &offset, 0x80); emit_u8(code, &offset, 0x3d);
    emit_u32(code, &offset, kLatchAddress);
    emit_u8(code, &offset, 0x00); /* cmp byte ptr [latch], 0 */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x85); /* jne skip */
    size_t skip_relative = offset;
    emit_u32(code, &offset, 0);

    emit_u8(code, &offset, 0xc6); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, kLatchAddress);
    emit_u8(code, &offset, 0x01); /* mov byte ptr [latch], 1 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kOriginalFrontendAddress);

    size_t skip_offset = offset;
    emit_u8(code, &offset, 0xb8); emit_u32(code, &offset, 1); /* mov eax, 1 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kFunctionEpilogueAddress);

    size_t reset_offset = offset;
    emit_u8(code, &offset, 0xc6); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, kLatchAddress);
    emit_u8(code, &offset, 0x00); /* mov byte ptr [latch], 0 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kOriginalFrontendAddress);

    size_t state_true_offset = offset;
    emit_u8(code, &offset, 0x83); emit_u8(code, &offset, 0xfe);
    emit_u8(code, &offset, 0x02); /* cmp esi, 2 */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, kStateTrueAddress);

    int32_t relative = (int32_t)((intptr_t)(kStubAddress + state_true_offset) -
                                 (intptr_t)(kStubAddress +
                                            state_true_relative + 4));
    memcpy(code + state_true_relative, &relative, sizeof(relative));
    relative = (int32_t)((intptr_t)(kStubAddress + reset_offset) -
                         (intptr_t)(kStubAddress + reset_relative + 4));
    memcpy(code + reset_relative, &relative, sizeof(relative));
    relative = (int32_t)((intptr_t)(kStubAddress + skip_offset) -
                         (intptr_t)(kStubAddress + skip_relative + 4));
    memcpy(code + skip_relative, &relative, sizeof(relative));

    size_t page_size = (size_t)getpagesize();
    if (mprotect((void *)(uintptr_t)kStubAddress, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("compat32: mprotect startup input stub");
        return -1;
    }
    memcpy(stub, code, offset);
    flush_guest_instruction(kStubAddress);
    if (mprotect((void *)(uintptr_t)kStubAddress, page_size,
                 PROT_READ | PROT_EXEC) != 0) {
        perror("compat32: protect startup input stub");
        return -1;
    }
    *latch = 0;

    uintptr_t hook_page = kHookAddress & ~(page_size - 1);
    if (mprotect((void *)hook_page, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("compat32: mprotect startup input hook");
        return -1;
    }
    unsigned char patch[sizeof(expected)] = {0xe9};
    int32_t hook_relative =
        (int32_t)((intptr_t)kStubAddress - (intptr_t)(kHookAddress + 5));
    memcpy(patch + 1, &hook_relative, sizeof(hook_relative));
    patch[5] = 0x90;
    patch[6] = 0x90;
    memcpy(hook, patch, sizeof(patch));
    __builtin___clear_cache((char *)hook, (char *)hook + sizeof(patch));
    if (mprotect((void *)hook_page, page_size, PROT_READ | PROT_EXEC) != 0) {
        perror("compat32: protect startup input hook");
        return -1;
    }
    fprintf(stderr, "compat32: installed startup input repeat latch\n");
    return 0;
}

static int write_guest_code(uintptr_t address, const void *bytes, size_t length,
                            const char *what)
{
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = address & ~(page_size - 1);
    size_t span = (address + length - page + page_size - 1) & ~(page_size - 1);
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "compat32: mprotect %s: %s\n", what, strerror(errno));
        return -1;
    }
    memcpy((void *)address, bytes, length);
    __builtin___clear_cache((char *)address, (char *)(address + length));
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "compat32: protect %s: %s\n", what, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Single SaveCore thread (see lp32_save_worker_patch in game_profile.h).
 * The worker constructor publishes the run flag already set, and the bare
 * thread-spawn call inside the worker's start method goes through a stub that
 * forwards only the first caller (atomic test-and-set on a latch byte); the
 * rest of the start method still runs for every worker object.
 */
static int install_save_worker_patch(void)
{
    const struct lp32_save_worker_patch *layout = lp32_profile()->save_worker;
    if (!layout) return 0;
    enum {
        kStubAddress = 0x7f00f800,
        kLatchAddress = 0x7f01ffe0,
    };

    const struct lp32_code_signature *ctor = &layout->ctor_running_flag;
    unsigned char *ctor_code = (void *)(uintptr_t)ctor->address;
    unsigned char *call_site = (void *)(uintptr_t)layout->start_call;
    unsigned char expected_call[5] = {0xe8};
    int32_t relative = (int32_t)((intptr_t)layout->thread_starter -
                                 (intptr_t)(layout->start_call + 5));
    memcpy(expected_call + 1, &relative, sizeof(relative));
    if (memcmp(ctor_code, ctor->expected, ctor->length) != 0 ||
        memcmp(call_site, expected_call, sizeof(expected_call)) != 0) {
        fprintf(stderr, "compat32: save worker patch signature mismatch\n");
        return -1;
    }

    unsigned char code[32];
    size_t offset = 0;
    emit_u8(code, &offset, 0xb0); emit_u8(code, &offset, 0x01); /* mov al, 1 */
    emit_u8(code, &offset, 0x86); emit_u8(code, &offset, 0x05);
    emit_u32(code, &offset, kLatchAddress);   /* xchg al, byte ptr [latch] */
    emit_u8(code, &offset, 0x84); emit_u8(code, &offset, 0xc0); /* test al, al */
    emit_u8(code, &offset, 0x75); emit_u8(code, &offset, 0x05); /* jne skip */
    emit_u8(code, &offset, 0xe9);                                /* jmp starter */
    emit_rel32(code, &offset, kStubAddress, layout->thread_starter);
    emit_u8(code, &offset, 0x31); emit_u8(code, &offset, 0xc0); /* skip: xor eax, eax */
    emit_u8(code, &offset, 0xc3);                                /* ret (no thread) */

    *(volatile unsigned char *)(uintptr_t)kLatchAddress = 0;
    if (write_guest_code(kStubAddress, code, offset, "save worker stub") != 0) {
        return -1;
    }
    unsigned char ctor_patch[sizeof(ctor->expected)];
    memcpy(ctor_patch, ctor->expected, ctor->length);
    ctor_patch[ctor->length - 1] = 0x01;    /* mov byte [reg+d8], 1 */
    if (write_guest_code(ctor->address, ctor_patch, ctor->length,
                         "save worker constructor") != 0) {
        return -1;
    }
    unsigned char call_patch[5] = {0xe8};
    relative = (int32_t)((intptr_t)kStubAddress -
                         (intptr_t)(layout->start_call + 5));
    memcpy(call_patch + 1, &relative, sizeof(relative));
    if (write_guest_code(layout->start_call, call_patch, sizeof(call_patch),
                         "save worker start call") != 0) {
        return -1;
    }
    fprintf(stderr, "compat32: installed single SaveCore thread patch\n");
    return 0;
}

/*
 * Empty sampler slot guard (see lp32_texture_bind_guard in game_profile.h).
 * The five-byte "mov ecx, [eax+14h]; test ecx, ecx" becomes a jump to a stub
 * that yields ecx = 0 when eax (the bound record) is NULL, so the jnz that
 * follows takes the same skip path the game uses for a record whose field is
 * zero.
 */
static int install_texture_bind_guard(void)
{
    const struct lp32_texture_bind_guard *guard = lp32_profile()->texture_bind_guard;
    if (!guard) return 0;
    enum { kStubAddress = 0x7f00fc00 };

    const struct lp32_code_signature *load = &guard->load;
    if (memcmp((void *)(uintptr_t)load->address, load->expected, load->length) != 0 ||
        load->length != 5 || guard->resume != load->address + 5) {
        fprintf(stderr, "compat32: texture bind guard signature mismatch\n");
        return -1;
    }

    unsigned char code[16];
    size_t offset = 0;
    emit_u8(code, &offset, 0x31); emit_u8(code, &offset, 0xc9); /* xor ecx, ecx */
    emit_u8(code, &offset, 0x85); emit_u8(code, &offset, 0xc0); /* test eax, eax */
    emit_u8(code, &offset, 0x74); emit_u8(code, &offset, 0x03); /* jz skip */
    emit_u8(code, &offset, 0x8b); emit_u8(code, &offset, 0x48);
    emit_u8(code, &offset, 0x14);                                /* mov ecx, [eax+14h] */
    emit_u8(code, &offset, 0x85); emit_u8(code, &offset, 0xc9); /* skip: test ecx, ecx */
    emit_u8(code, &offset, 0xe9);                                /* jmp resume */
    emit_rel32(code, &offset, kStubAddress, guard->resume);
    if (write_guest_code(kStubAddress, code, offset, "texture bind guard stub") != 0) {
        return -1;
    }
    unsigned char hook[5] = {0xe9};
    int32_t relative = (int32_t)((intptr_t)kStubAddress -
                                 (intptr_t)(load->address + 5));
    memcpy(hook + 1, &relative, sizeof(relative));
    if (write_guest_code(load->address, hook, sizeof(hook),
                         "texture bind guard hook") != 0) {
        return -1;
    }
    fprintf(stderr, "compat32: installed empty sampler slot guard\n");
    return 0;
}

/* Validate the addresses used by the display bridge before guest startup. */
static int install_screen_aspect_patch(void)
{
    const struct lp32_screen_aspect_patch *patch =
        lp32_profile()->screen_aspect_patch;
    if (!patch) return 0;
    const struct lp32_code_signature *store = &patch->mode_ratio_store;
    const struct lp32_code_signature *hud = &patch->hud_scale_load;
    const struct lp32_code_signature *panel = &patch->panel_scale_select;
    float original_hud_scale;
    float panel_scales[4];
    float panel_coin_y, panel_heart_y;
    memcpy(&original_hud_scale,
           (const void *)(uintptr_t)patch->hud_default_scale_address,
           sizeof(original_hud_scale));
    memcpy(panel_scales, (const void *)(uintptr_t)patch->panel_scale_table,
           sizeof(panel_scales));
    memcpy(&panel_coin_y, (const void *)(uintptr_t)patch->panel_coin_y,
           sizeof(panel_coin_y));
    memcpy(&panel_heart_y, (const void *)(uintptr_t)patch->panel_heart_y,
           sizeof(panel_heart_y));
    if (store->length != 8 ||
        memcmp((const void *)(uintptr_t)store->address,
               store->expected, store->length) != 0 ||
        hud->length != 8 ||
        memcmp((const void *)(uintptr_t)hud->address,
               hud->expected, hud->length) != 0 ||
        panel->length != 8 ||
        memcmp((const void *)(uintptr_t)panel->address,
               panel->expected, panel->length) != 0 ||
        fabsf(panel_scales[0] - 0.60556465f) > 0.000001f ||
        fabsf(panel_scales[1] - 0.52373159f) > 0.000001f ||
        fabsf(panel_scales[2] - 0.33842796f) > 0.000001f ||
        fabsf(panel_scales[3] - 0.39126638f) > 0.000001f ||
        fabsf(panel_coin_y - 0.045f) > 0.000001f ||
        fabsf(panel_heart_y + 0.045f) > 0.000001f ||
        original_hud_scale != 9.0f / 16.0f) {
        fprintf(stderr, "compat32: screen aspect data signature mismatch\n");
        return -1;
    }
    fprintf(stderr, "compat32: verified Saga aspect and panel scale data addresses\n");
    return 0;
}

/* Keep the original disabled-achievements return (eax = 0) when Steam's
   stats object is NULL. Do not invent an interface, claim an unlock, or
   alter Steam's initialization/relaunch result. All non-NULL calls retain
   the original SetAchievement/StoreStats path and failure handling. */
static int install_steam_achievement_guard(void)
{
    const struct lp32_steam_achievement_guard *guard =
        lp32_profile()->steam_achievement_guard;
    if (!guard) return 0;
    enum { kStubAddress = 0x7f00f400 };
    const struct lp32_code_signature *check = &guard->enabled_check;
    const struct lp32_code_signature *load = &guard->stats_load;
    const struct lp32_code_signature *skip = &guard->skip_return;
    const unsigned char *branch = (void *)(uintptr_t)(check->address + 7);
    const unsigned char zero_eax[] = {0x31, 0xc0};
    if (check->length != 7 || load->length != 8 || skip->length != 6 ||
        memcmp((void *)(uintptr_t)check->address, check->expected, 7) ||
        memcmp((void *)(uintptr_t)load->address, load->expected, 8) ||
        memcmp((void *)(uintptr_t)skip->address, skip->expected, 6) ||
        memcmp((void *)(uintptr_t)(check->address - 2), zero_eax, 2) ||
        branch[0] != 0x74 ||
        check->address + 9 + (int8_t)branch[1] != skip->address) {
        fprintf(stderr, "compat32: Steam achievement guard signature mismatch\n");
        return -1;
    }
    unsigned char code[32];
    size_t offset = 0;
    emit_u8(code, &offset, 0x83); emit_u8(code, &offset, 0x3d);
    emit_u32(code, &offset, guard->stats_pointer);
    emit_u8(code, &offset, 0); /* cmp dword ptr [stats], 0 */
    emit_u8(code, &offset, 0x0f); emit_u8(code, &offset, 0x84);
    emit_rel32(code, &offset, kStubAddress, skip->address); /* je return */
    memcpy(code + offset, check->expected, check->length);
    offset += check->length;
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kStubAddress, check->address + check->length);
    if (write_guest_code(kStubAddress, code, offset, "Steam achievement stub"))
        return -1;
    unsigned char hook[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    int32_t relative = (int32_t)(kStubAddress - (check->address + 5));
    memcpy(hook + 1, &relative, sizeof(relative));
    if (write_guest_code(check->address, hook, sizeof(hook), "Steam achievement hook"))
        return -1;
    fprintf(stderr, "compat32: installed unavailable Steam stats achievement guard\n");
    return 0;
}

#include "steam_achievement_selftest.h"

enum { kSubtitlePanelStub = 0x7f00ee00, kSubtitlePanelConstants = 0x7f00ef00 };

static int install_subtitle_panel_patch(void)
{
    const struct lp32_subtitle_panel_patch *patch = lp32_profile()->subtitle_panel_patch;
    if (!patch) return 0;
    const struct lp32_code_signature *call = &patch->draw_call;
    int32_t target;
    memcpy(&target, call->expected + 1, sizeof(target));
    if (call->length != 5 || call->expected[0] != 0xe8 ||
        call->address + 5 + target != patch->draw_function ||
        memcmp((void *)(uintptr_t)call->address, call->expected, 5)) {
        fprintf(stderr, "compat32: subtitle panel signature mismatch\n");
        return -1;
    }
    /* The original drawer puts the panel at depth 1/FOV. Perspective then
       scales its width by FOV/tan(FOV/2). Cancel that variation relative to
       the normal cinematic camera. Text, wrapping, height and position are
       untouched. Skip the reference FOV exactly, preserving normal scenes.
       This call site belongs only to the subtitle background. */
    float constants[] = {0.5f, patch->reference_fov / tanf(patch->reference_fov * 0.5f)};
    uint32_t reference_bits;
    memcpy(&reference_bits, &patch->reference_fov, 4);
    unsigned char code[96];
    size_t offset = 0;
    const unsigned char prologue[] = {
        0x50, 0x51,                  /* push eax; push ecx */
        0x8b,0x44,0x24,0x0c,        /* mov eax,[esp+12]: camera */
        0x8b,0x4c,0x24,0x18,        /* mov ecx,[esp+24]: size */
        0x81,0xb8,                  /* cmp dword [eax+fov],reference */
    };
    memcpy(code, prologue, sizeof(prologue)); offset = sizeof(prologue);
    emit_u32(code, &offset, patch->camera_fov_offset);
    emit_u32(code, &offset, reference_bits);
    emit_u8(code, &offset, 0x74);
    size_t skip = offset++;
    emit_u8(code, &offset, 0xd9); emit_u8(code, &offset, 0x80); /* fld [eax+fov] */
    emit_u32(code, &offset, patch->camera_fov_offset);
    emit_u8(code, &offset, 0xd8); emit_u8(code, &offset, 0x0d); /* fmul [half] */
    emit_u32(code, &offset, kSubtitlePanelConstants);
    emit_u8(code, &offset, 0xd9); emit_u8(code, &offset, 0xf2); /* fptan */
    emit_u8(code, &offset, 0xdd); emit_u8(code, &offset, 0xd8); /* fstp st(0): pop 1 */
    emit_u8(code, &offset, 0xd8); emit_u8(code, &offset, 0xb0); /* fdiv [eax+fov] */
    emit_u32(code, &offset, patch->camera_fov_offset);
    emit_u8(code, &offset, 0xd8); emit_u8(code, &offset, 0x0d); /* fmul [reference ratio] */
    emit_u32(code, &offset, kSubtitlePanelConstants + 4);
    emit_u8(code, &offset, 0xd8); emit_u8(code, &offset, 0x09); /* fmul [ecx]: width */
    emit_u8(code, &offset, 0xd9); emit_u8(code, &offset, 0x19); /* fstp [ecx] */
    code[skip] = (unsigned char)(offset - skip - 1);
    emit_u8(code, &offset, 0x59); emit_u8(code, &offset, 0x58); /* pop ecx; pop eax */
    emit_u8(code, &offset, 0xe9);
    emit_rel32(code, &offset, kSubtitlePanelStub, patch->draw_function);
    if (write_guest_code(kSubtitlePanelConstants, constants, sizeof(constants), "subtitle panel constants") ||
        write_guest_code(kSubtitlePanelStub, code, offset, "subtitle panel stub")) return -1;
    unsigned char hook[5] = {0xe8};
    int32_t relative = (int32_t)(kSubtitlePanelStub - (call->address + 5));
    memcpy(hook + 1, &relative, 4);
    if (write_guest_code(call->address, hook, sizeof(hook), "subtitle panel hook")) return -1;
    fprintf(stderr, "compat32: installed subtitle panel FOV correction\n");
    return 0;
}

#include "subtitle_panel_selftest.h"

static void flush_guest_instruction(uintptr_t address)
{
    __builtin___clear_cache((char *)address, (char *)(address + 1));
}

static const char *vm_tag_name(unsigned tag)
{
    switch (tag) {
    case 0: return "untagged";
    case VM_MEMORY_MALLOC: return "malloc";
    case VM_MEMORY_MALLOC_SMALL: return "malloc-small";
    case VM_MEMORY_MALLOC_LARGE: return "malloc-large";
    case VM_MEMORY_MALLOC_HUGE: return "malloc-huge";
    case VM_MEMORY_MALLOC_TINY: return "malloc-tiny";
    case VM_MEMORY_MALLOC_LARGE_REUSABLE: return "malloc-large-reusable";
    case VM_MEMORY_MALLOC_LARGE_REUSED: return "malloc-large-reused";
    case VM_MEMORY_MALLOC_NANO: return "malloc-nano";
    case VM_MEMORY_MALLOC_MEDIUM: return "malloc-medium";
    case VM_MEMORY_MACH_MSG: return "mach-msg";
    case VM_MEMORY_IOKIT: return "iokit";
    case VM_MEMORY_STACK: return "stack";
    case VM_MEMORY_GUARD: return "guard";
    case VM_MEMORY_SHARED_PMAP: return "shared-cache";
    case VM_MEMORY_DYLIB: return "dylib";
    case VM_MEMORY_APPKIT: return "appkit";
    case VM_MEMORY_FOUNDATION: return "foundation";
    case VM_MEMORY_COREGRAPHICS: return "coregraphics";
    case VM_MEMORY_LAYERKIT: return "coreanimation";
    case VM_MEMORY_DYLD: return "dyld";
    case VM_MEMORY_DYLD_MALLOC: return "dyld-malloc";
    case VM_MEMORY_GLSL: return "glsl";
    case VM_MEMORY_OS_ALLOC_ONCE: return "os-alloc-once";
    case VM_MEMORY_LIBDISPATCH: return "libdispatch";
    case VM_MEMORY_IOSURFACE: return "iosurface";
    case VM_MEMORY_AUDIO: return "audio";
    case VM_MEMORY_IOACCELERATOR: return "ioaccelerator";
    case VM_MEMORY_COLORSYNC: return "colorsync";
    case VM_MEMORY_ROSETTA: return "rosetta";
    case VM_MEMORY_ROSETTA_THREAD_CONTEXT: return "rosetta-thread-context";
    case VM_MEMORY_ROSETTA_EXECUTABLE_HEAP: return "rosetta-executable-heap";
    case VM_MEMORY_ROSETTA_ARENA: return "rosetta-arena";
    default: return "?";
    }
}

/* Finds the region containing address; false if it is unmapped. */
static bool crash_region_at(mach_vm_address_t address, mach_vm_address_t *start,
                            mach_vm_size_t *size,
                            vm_region_extended_info_data_t *region)
{
    mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t object = MACH_PORT_NULL;
    *start = address;
    kern_return_t result = mach_vm_region(mach_task_self(), start, size,
        VM_REGION_EXTENDED_INFO, (vm_region_info_t)region, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return result == KERN_SUCCESS && *start <= address;
}

static void crash_print_region(const char *label, const char *relation,
                               mach_vm_address_t start, mach_vm_size_t size,
                               const vm_region_extended_info_data_t *region)
{
    char path[PATH_MAX] = "";
    proc_regionfilename(getpid(), start, path, sizeof(path));
    CRASH_DIAGNOSTIC(
        "compat32: crash region %s %s [0x%llx-0x%llx) prot=%c%c%c "
        "tag=%u(%s) share=%u%s%s\n",
        label, relation, (unsigned long long)start,
        (unsigned long long)(start + size),
        region->protection & VM_PROT_READ ? 'r' : '-',
        region->protection & VM_PROT_WRITE ? 'w' : '-',
        region->protection & VM_PROT_EXECUTE ? 'x' : '-',
        region->user_tag, vm_tag_name(region->user_tag), region->share_mode,
        path[0] ? " " : "", path);
}

/*
 * Heap corruption is usually detected far from the write that caused it, so
 * for each pointer-like value name the mapping it points into, the mapping
 * just below it (the likely source of an overrun into a region's first
 * bytes), and dump the surrounding writable memory.
 */
static void crash_describe_address(const char *label, uint64_t value, bool dump)
{
    if (value < 0x10000 || value >= UINT64_C(0x800000000000)) return;
    mach_vm_address_t start;
    mach_vm_size_t size;
    vm_region_extended_info_data_t region;
    if (!crash_region_at(value, &start, &size, &region)) {
        CRASH_DIAGNOSTIC("compat32: crash region %s=0x%llx unmapped\n",
                         label, (unsigned long long)value);
        return;
    }
    char relation[64];
    lp32_crash_format(relation, sizeof(relation), "=0x%llx +0x%llx in",
             (unsigned long long)value, (unsigned long long)(value - start));
    crash_print_region(label, relation, start, size, &region);
    mach_vm_address_t below_start;
    mach_vm_size_t below_size;
    vm_region_extended_info_data_t below;
    if (start > 0 && crash_region_at(start - 1, &below_start, &below_size, &below)) {
        crash_print_region(label, "below", below_start, below_size, &below);
    }
    if (!dump || !(region.protection & VM_PROT_WRITE) ||
        region.user_tag == VM_MEMORY_STACK) return;
    mach_vm_address_t line = (value & ~(mach_vm_address_t)15) - 48;
    for (unsigned index = 0; index < 7; ++index, line += 16) {
        uint64_t words[2];
        mach_vm_size_t count = 0;
        if (mach_vm_read_overwrite(mach_task_self(), line, sizeof(words),
                                   (mach_vm_address_t)words, &count) ||
            count != sizeof(words)) {
            CRASH_DIAGNOSTIC("compat32: crash mem %s 0x%llx: unreadable\n",
                             label, (unsigned long long)line);
            continue;
        }
        CRASH_DIAGNOSTIC("compat32: crash mem %s 0x%llx: %016llx %016llx\n",
                         label, (unsigned long long)line,
                         (unsigned long long)words[0],
                         (unsigned long long)words[1]);
    }
}

static void crash_describe_state(siginfo_t *info, const ucontext_t *context)
{
    mach_port_t thread = mach_thread_self();
    CRASH_DIAGNOSTIC("compat32: crash thread port=%u\n", thread);
    mach_port_deallocate(mach_task_self(), thread);
    const __typeof__(context->uc_mcontext->__ss) *state = &context->uc_mcontext->__ss;
    CRASH_DIAGNOSTIC(
        "compat32: crash r8=%016llx r9=%016llx r10=%016llx r11=%016llx "
        "r12=%016llx r13=%016llx r15=%016llx rflags=%016llx\n",
        state->__r8, state->__r9, state->__r10, state->__r11, state->__r12,
        state->__r13, state->__r15, state->__rflags);
    const struct { const char *name; uint64_t value; bool dump; } values[] = {
        {"fault", (uint64_t)(uintptr_t)info->si_addr, true},
        {"rip", state->__rip, false},
        {"rsp", state->__rsp, false},
        {"rax", state->__rax, true}, {"rbx", state->__rbx, true},
        {"rcx", state->__rcx, true}, {"rdx", state->__rdx, true},
        {"rsi", state->__rsi, true}, {"rdi", state->__rdi, true},
        {"rbp", state->__rbp, false}, {"r8", state->__r8, true},
        {"r9", state->__r9, true}, {"r10", state->__r10, true},
        {"r11", state->__r11, true}, {"r12", state->__r12, true},
        {"r13", state->__r13, true}, {"r14", state->__r14, true},
        {"r15", state->__r15, true},
    };
    const size_t value_count = sizeof(values) / sizeof(values[0]);
    for (size_t index = 0; index < value_count; ++index) {
        bool repeated = false;
        for (size_t earlier = 0; earlier < index && !repeated; ++earlier) {
            repeated = values[earlier].value == values[index].value;
        }
        if (!repeated) {
            crash_describe_address(values[index].name, values[index].value,
                                   values[index].dump);
        }
    }
    audio_bridge32_write_crash_report(STDERR_FILENO);
    if (guest_diagnostic_fd >= 0) audio_bridge32_write_crash_report(guest_diagnostic_fd);
}

static void guest_crash_diagnostic(int signal_number, siginfo_t *info,
                                   void *opaque_context)
{
    ucontext_t *context = opaque_context;
    uint64_t rip = context->uc_mcontext->__ss.__rip;
    uint64_t rsp = context->uc_mcontext->__ss.__rsp;
    if (signal_number == SIGTRAP && guest_breakpoint_address) {
        if (guest_breakpoint_stepping) {
            *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
            flush_guest_instruction(guest_breakpoint_address);
            context->uc_mcontext->__ss.__rflags &= ~UINT64_C(0x100);
            guest_breakpoint_stepping = 0;
            return;
        }
        if (rip == guest_breakpoint_address + 1) {
            unsigned count = ++guest_breakpoint_count;
            const uint32_t *words = (const void *)(uintptr_t)rsp;
            if (count > guest_breakpoint_skip) {
            CRASH_DIAGNOSTIC(
                    "compat32: guest breakpoint 0x%08llx hit %u "
                    "eax=%08llx ebx=%08llx ecx=%08llx edx=%08llx "
                    "esi=%08llx edi=%08llx ebp=%08llx esp=%08llx\n",
                    (unsigned long long)guest_breakpoint_address, count,
                    context->uc_mcontext->__ss.__rax & UINT32_MAX,
                    context->uc_mcontext->__ss.__rbx & UINT32_MAX,
                    context->uc_mcontext->__ss.__rcx & UINT32_MAX,
                    context->uc_mcontext->__ss.__rdx & UINT32_MAX,
                    context->uc_mcontext->__ss.__rsi & UINT32_MAX,
                    context->uc_mcontext->__ss.__rdi & UINT32_MAX,
                    context->uc_mcontext->__ss.__rbp & UINT32_MAX,
                    rsp & UINT32_MAX);
            if (rsp >= UINT32_C(0x1000) && rsp < UINT32_C(0x80000000)) {
                for (unsigned index = 0; index < 12; ++index) {
                    CRASH_DIAGNOSTIC(
                            "compat32: breakpoint stack[%u]=0x%08x\n",
                            index, words[index]);
                }
                const char *dump_argument_text =
                    getenv("LP32_TRACE_GUEST_DUMP_ARGUMENT");
                unsigned dump_argument = dump_argument_text ?
                    (unsigned)strtoul(dump_argument_text, NULL, 0) :
                    (getenv("LP32_TRACE_GUEST_DUMP_ARG1") ? 1u : 0u);
                if (dump_argument > 0 && dump_argument < 12 &&
                    words[dump_argument] >= UINT32_C(0x1000) &&
                    words[dump_argument] < UINT32_C(0x80000000)) {
                    const uint32_t *object =
                        (const void *)(uintptr_t)words[dump_argument];
                    for (unsigned index = 0; index < 24; ++index) {
                        CRASH_DIAGNOSTIC(
                                "compat32: breakpoint arg%u+0x%02x=0x%08x\n",
                                dump_argument, index * 4, object[index]);
                    }
                }
                const char *dump_address_text =
                    getenv("LP32_TRACE_GUEST_DUMP_ADDRESS");
                if (dump_address_text && dump_address_text[0]) {
                    uintptr_t dump_address =
                        (uintptr_t)strtoull(dump_address_text, NULL, 0);
                    if (dump_address >= UINT32_C(0x1000) &&
                        dump_address < UINT32_C(0x80000000)) {
                        const uint32_t *dump = (const void *)dump_address;
                        for (unsigned index = 0; index < 16; ++index) {
                            CRASH_DIAGNOSTIC(
                                    "compat32: breakpoint mem[%08llx]=0x%08x\n",
                                    (unsigned long long)(dump_address + index * 4),
                                    dump[index]);
                        }
                        uintptr_t indirect = dump[0];
                        if (indirect >= UINT32_C(0x1000) &&
                            indirect < UINT32_C(0x80000000)) {
                            const uint32_t *object = (const void *)indirect;
                            for (unsigned index = 0; index < 32; ++index) {
                                CRASH_DIAGNOSTIC(
                                        "compat32: breakpoint indirect[%08llx]="
                                        "0x%08x\n",
                                        (unsigned long long)(indirect + index * 4),
                                        object[index]);
                            }
                        }
                    }
                }
                if (getenv("LP32_TRACE_GUEST_DUMP_ESI")) {
                    uintptr_t dump_address =
                        context->uc_mcontext->__ss.__rsi & UINT32_MAX;
                    if (dump_address >= UINT32_C(0x1000) &&
                        dump_address < UINT32_C(0x80000000)) {
                        const uint32_t *dump = (const void *)dump_address;
                        for (unsigned index = 0; index < 20; ++index) {
                            CRASH_DIAGNOSTIC(
                                    "compat32: breakpoint esi+0x%02x="
                                    "0x%08x\n",
                                    index * 4, dump[index]);
                        }
                    }
                }
            }
            }
            *(volatile unsigned char *)guest_breakpoint_address =
                guest_breakpoint_original_byte;
            flush_guest_instruction(guest_breakpoint_address);
            context->uc_mcontext->__ss.__rip = guest_breakpoint_address;
            if (count < guest_breakpoint_limit) {
                context->uc_mcontext->__ss.__rflags |= UINT64_C(0x100);
                guest_breakpoint_stepping = 1;
            }
            return;
        }
    }
    /* A second fault must terminate, and a blocked output fd or optional
       symbol lookup must not leave a dead game consuming CPU indefinitely. */
    if (__atomic_exchange_n(&guest_crash_active, 1, __ATOMIC_RELAXED))
        _exit(128 + signal_number);
    struct sigaction timeout = {0};
    timeout.sa_handler = SIG_DFL;
    sigemptyset(&timeout.sa_mask);
    sigaction(SIGALRM, &timeout, NULL);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
    alarm(2);
    CRASH_DIAGNOSTIC(
            "compat32: signal %d code=%d address=%p "
            "rip=0x%016llx rsp=0x%016llx\n",
            signal_number, info->si_code, info->si_addr, rip, rsp);
    if (rip > UINT32_MAX) {
        uintptr_t address = rip, frame = context->uc_mcontext->__ss.__rbp;
        for (unsigned i = 0; i < 16; ++i) {
            CRASH_DIAGNOSTIC("compat32: native return[%u]=0x%016llx\n",
                            i, (unsigned long long)address);
            /* Symbol lookup can acquire dyld locks. Keep it opt-in; the
               bounded raw frame walk is sufficient for offline symbolication. */
            if (guest_trace_native_crash) {
            Dl_info symbol = {0};
            dladdr((void *)address, &symbol);
            CRASH_DIAGNOSTIC("compat32: native frame %u %p %s + %llu (%s)\n", i,
                (void *)address, symbol.dli_sname ? symbol.dli_sname : "?",
                (unsigned long long)(address - (uintptr_t)symbol.dli_saddr),
                symbol.dli_fname ? symbol.dli_fname : "?");
            }
            uint64_t words[2]; mach_vm_size_t count = 0;
            if (frame < rsp || frame - rsp > 0x100000 ||
                mach_vm_read_overwrite(mach_task_self(), frame, sizeof(words),
                    (mach_vm_address_t)words, &count) || count != sizeof(words)) break;
            if (words[0] <= frame) break;
            frame = words[0]; address = words[1];
        }
    }
    uint32_t mode_to64 = 0, mode_to32 = 0;
    compat_runtime32_mode_guard_counts(&mode_to64, &mode_to32);
    CRASH_DIAGNOSTIC(
            "compat32: crash cs=0x%llx mode-recoveries to64=%u to32=%u\n",
            (unsigned long long)context->uc_mcontext->__ss.__cs,
            mode_to64, mode_to32);
    CRASH_DIAGNOSTIC(
            "compat32: crash rax=%016llx rbx=%016llx rcx=%016llx "
            "rdx=%016llx rsi=%016llx rdi=%016llx rbp=%016llx r14=%016llx\n",
            context->uc_mcontext->__ss.__rax,
            context->uc_mcontext->__ss.__rbx,
            context->uc_mcontext->__ss.__rcx,
            context->uc_mcontext->__ss.__rdx,
            context->uc_mcontext->__ss.__rsi,
            context->uc_mcontext->__ss.__rdi,
            context->uc_mcontext->__ss.__rbp,
            context->uc_mcontext->__ss.__r14);
    crash_describe_state(info, context);
    if (rsp >= UINT32_C(0x1000) && rsp < UINT32_C(0x80000000)) {
        uint32_t words[16]; mach_vm_size_t count = 0;
        if (mach_vm_read_overwrite(mach_task_self(), rsp, sizeof(words),
                (mach_vm_address_t)words, &count) == KERN_SUCCESS) {
            for (unsigned index = 0; index < count / sizeof(words[0]); ++index)
                CRASH_DIAGNOSTIC("compat32: crash stack[%u]=0x%08x\n", index, words[index]);
        } else CRASH_DIAGNOSTIC("compat32: crash stack unreadable\n");
    }
    for (unsigned item = 0; item < guest_crash_dump_count; ++item) {
        uint32_t words[64]; mach_vm_size_t count = 0;
        uint32_t address = guest_crash_dumps[item].address;
        if (mach_vm_read_overwrite(mach_task_self(), address,
                guest_crash_dumps[item].count * sizeof(words[0]),
                (mach_vm_address_t)words, &count) != KERN_SUCCESS) {
            CRASH_DIAGNOSTIC("compat32: crash mem[0x%08x] unreadable\n", address);
            continue;
        }
        for (unsigned index = 0; index < count / sizeof(words[0]); ++index)
            CRASH_DIAGNOSTIC("compat32: crash mem[0x%08x]=0x%08x\n",
                             address + 4 * index, words[index]);
    }
    _exit(128 + signal_number);
}

static void install_guest_crash_diagnostics(void)
{
    guest_trace_native_crash = getenv("LP32_TRACE_NATIVE_CRASH") != NULL;
    const char *dump = getenv("LP32_CRASH_DUMP");
    if (dump) {
        char buffer[256];
        strlcpy(buffer, dump, sizeof(buffer));
        char *cursor = buffer;
        for (char *item = strsep(&cursor, ","); item && guest_crash_dump_count < 16;
             item = strsep(&cursor, ",")) {
            char *colon = strchr(item, ':');
            uint32_t address = (uint32_t)strtoul(item, NULL, 0);
            unsigned count = colon ? (unsigned)strtoul(colon + 1, NULL, 0) : 8;
            if (address < 0x1000 || address >= 0x7f000000 || count > 64) continue;
            guest_crash_dumps[guest_crash_dump_count].address = address;
            guest_crash_dumps[guest_crash_dump_count++].count = count;
        }
    }
    stack_t stack = {.ss_sp = guest_signal_stack, .ss_size = sizeof(guest_signal_stack)};
    sigaltstack(&stack, NULL);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guest_crash_diagnostic;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGTRAP, &action, NULL);
}

static int configure_guest_breakpoint(const struct macho_image32 *image)
{
    const char *address_text = getenv("LP32_TRACE_GUEST_ADDRESS");
    if (!address_text || !address_text[0]) return 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(address_text, &end, 0);
    if (!end || *end || parsed < image->min_address ||
        parsed >= image->max_address) {
        fprintf(stderr, "game_loader: invalid LP32_TRACE_GUEST_ADDRESS: %s\n",
                address_text);
        return -1;
    }
    const char *limit_text = getenv("LP32_TRACE_GUEST_LIMIT");
    unsigned long parsed_limit = limit_text ? strtoul(limit_text, NULL, 0) : 16;
    guest_breakpoint_limit = parsed_limit > 0 && parsed_limit <= UINT_MAX ?
        (unsigned)parsed_limit : 16;
    const char *skip_text = getenv("LP32_TRACE_GUEST_SKIP");
    unsigned long parsed_skip = skip_text ? strtoul(skip_text, NULL, 0) : 0;
    guest_breakpoint_skip = parsed_skip < guest_breakpoint_limit ?
        (unsigned)parsed_skip : 0;
    guest_breakpoint_address = (uintptr_t)parsed;
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = guest_breakpoint_address & ~(page_size - 1);
    if (mprotect((void *)page, page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("game_loader: mprotect guest breakpoint page");
        guest_breakpoint_address = 0;
        return -1;
    }
    guest_breakpoint_original_byte =
        *(const unsigned char *)guest_breakpoint_address;
    *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
    flush_guest_instruction(guest_breakpoint_address);
    fprintf(stderr,
            "compat32: armed guest breakpoint 0x%08llx original=0x%02x "
            "skip=%u limit=%u\n",
            parsed, guest_breakpoint_original_byte, guest_breakpoint_skip,
            guest_breakpoint_limit);
    return 0;
}

static void runtime_diagnostic_line(const char *line)
{
    GUEST_DIAGNOSTIC("%s", line);
}

/* Bundled launches carry the image in Contents/SharedSupport; pick whichever
   known title's image is present next to the loader. */
static const char *default_image_path(const char *argv0, char *buffer,
                                      size_t size)
{
    static const char *const candidates[] = {"LEGOPirates", "LEGOCloneWars", "LEGOMarvel", "LEGOCompleteSaga", "LEGOBatman3", "LEGOMovie", "LEGOHobbit"};
    const char *slash = strrchr(argv0, '/');
    size_t directory_length = slash ? (size_t)(slash - argv0) : 0;
    for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        const struct lp32_game_profile *profile = lp32_profile_named(candidates[index]);
        int length = snprintf(buffer, size, "%.*s%s../SharedSupport/%s",
                              (int)directory_length, argv0, slash ? "/" : "",
                              profile->image_file);
        if (length < 0 || (size_t)length >= size) {
            fprintf(stderr, "game_loader: executable path is too long\n");
            return NULL;
        }
        if (access(buffer, R_OK) == 0) return buffer;
    }
    fprintf(stderr, "game_loader: no game image found in Contents/SharedSupport\n");
    return NULL;
}

__attribute__((noreturn, noinline)) static void crash_without_stack(void)
{
    __asm__ volatile("xorq %rsp, %rsp; ud2");
    __builtin_unreachable();
}

int main(int argc, char **argv)
{
    objc_bridge32_initialize_test_mode();
    if (getenv("LP32_SILENT_TEST_SELFTEST")) {
        const char *mute = getenv("LP32_MUTE_AUDIO");
        const char *background = getenv("LP32_BACKGROUND_TEST");
        int ok = mute && !strcmp(mute, "1") && background && !strcmp(background, "1");
        fprintf(stderr, "silent-test startup: %s (before guest/audio initialization)\n", ok ? "PASS" : "FAIL");
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    install_guest_crash_diagnostics();
    compat_runtime32_set_diagnostic_sink(runtime_diagnostic_line);
    if (getenv("LP32_FOCUS_SELFTEST")) {
        int expected = atoi(getenv("LP32_FOCUS_SELFTEST"));
        return objc_bridge32_run_focus_self_test(expected) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CRASH_DIAGNOSTIC_SELFTEST")) {
        const char *mode = getenv("LP32_CRASH_DIAGNOSTIC_SELFTEST");
        if (!strcmp(mode, "stdio")) {
            /* Fault inside libc output, as in the Sonoma CPU report. */
            setvbuf(stdout, NULL, _IONBF, 0);
            stdout->_write = NULL;
            fprintf(stdout, "compat32: stdio crash self-test %d\n", argc);
        } else if (!strcmp(mode, "stack")) {
            crash_without_stack();
        } else if (!strcmp(mode, "unmapped")) {
            munmap((void *)0x1000, 4096);
        }
        raise(SIGSEGV);
        return EXIT_FAILURE;
    }
    if (getenv("LP32_DISPATCH_FAST_SELFTEST")) {
        return objc_bridge32_run_dispatch_fast_self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_POINTER_SELFTEST")) {
        return objc_bridge32_run_pointer_self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_OBJC_PROXY_SELFTEST")) {
        return objc_bridge32_run_proxy_self_test() == 0 ?
            EXIT_SUCCESS : EXIT_FAILURE;
    }
    char image_path_buffer[PATH_MAX];
    const char *image_path = NULL;
    if (argc >= 2 && argv[1][0] != '-') {
        image_path = argv[1];
    } else {
        image_path = default_image_path(argv[0], image_path_buffer,
                                        sizeof(image_path_buffer));
        if (!image_path) return EXIT_FAILURE;
    }

    if (getenv("LP32_PAUSE_FOR_VMMAP")) sleep(30);

    open_guest_diagnostic_log(argc > 0 ? argv[0] : NULL, true);
    char host[768];
    lp32_host_description(host, sizeof(host));
    CRASH_DIAGNOSTIC("compat32: %s\n", host);
    CRASH_DIAGNOSTIC("compat32: loading image %s\n", image_path);
    struct macho_image32 image;
    if (macho_image32_load(image_path, &image) != 0) return EXIT_FAILURE;
    if (lp32_profile_select(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    open_guest_diagnostic_log(argc > 0 ? argv[0] : NULL, false);
    GUEST_DIAGNOSTIC("compat32: %s\n", host);
    const struct mach_header_64 *loader_header =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (loader_header && loader_header->magic == MH_MAGIC_64) {
        const struct load_command *command = (const void *)(loader_header + 1);
        for (uint32_t i = 0; i < loader_header->ncmds; ++i) {
            if (command->cmd == LC_UUID) {
                const struct uuid_command *uuid = (const void *)command;
                GUEST_DIAGNOSTIC("compat32: loader base=%p uuid=", (const void *)loader_header);
                for (unsigned j = 0; j < 16; ++j) GUEST_DIAGNOSTIC("%02x", uuid->uuid[j]);
                GUEST_DIAGNOSTIC("\n");
                break;
            }
            command = (const void *)((const char *)command + command->cmdsize);
        }
    }

    if (configure_guest_breakpoint(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    printf("image: segments=%" PRIu32 " range=0x%08" PRIx32 "-0x%08" PRIx32
           " entry=0x%08" PRIx32 " initializers=%" PRIu32 " imports=%" PRIu32 "\n",
           image.segment_count, image.min_address, image.max_address,
           image.entry_eip, image.initializer_count, image.import_count);

    const struct mach_header *mapped_header = image.header;
    if (mapped_header->magic != MH_MAGIC || mapped_header->cputype != CPU_TYPE_I386) {
        fprintf(stderr, "game_loader: mapped header validation failed\n");
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    puts("mapped-image validation: PASS");

    if (compat_runtime32_initialize(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (getenv("LP32_OBJC_LIFETIME_SELFTEST")) {
        int result=objc_legacy32_run_lifetime_self_test();
        macho_image32_unload(&image);
        return result==0?EXIT_SUCCESS:EXIT_FAILURE;
    }
    if (getenv("LP32_HEAP_SELFTEST")) {
        int result = compat_runtime32_run_heap_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CARBON_GEOMETRY_SELFTEST")) {
        int result = carbon_bridge32_run_geometry_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_FILE_SELFTEST")) {
        int result = compat_runtime32_run_file_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CTYPE_SELFTEST")) {
        int result = compat_runtime32_run_ctype_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_STEAM_STORAGE_PROBE")) {
        int result = steam_bridge32_probe_storage();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CG_SELFTEST")) {
        int result = compat_runtime32_run_cg_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_SYNC_SELFTEST")) {
        int result = compat_runtime32_run_sync_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GL_PARAMETER_SELFTEST")) {
        int result = objc_bridge32_run_gl_parameter_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CARBON_DISPATCH_SELFTEST")) {
        int result = carbon_bridge32_run_dispatch_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_MEDIA_SELFTEST")) {
        int result=quicktime_bridge32_run_media_self_test();
        macho_image32_unload(&image);
        return result==0?EXIT_SUCCESS:EXIT_FAILURE;
    }
    if (getenv("LP32_DATE_FORMATTER_SELFTEST")) {
        int result=objc_bridge32_run_date_formatter_self_test();
        macho_image32_unload(&image);
        return result==0?EXIT_SUCCESS:EXIT_FAILURE;
    }
    if (getenv("LP32_APP_EVENT_SELFTEST")) {
        int result = objc_bridge32_run_app_event_delivery_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_EVENT_MONITOR_SELFTEST")) {
        int result = objc_bridge32_run_event_monitor_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_IMPORT_RETURN_SELFTEST")) {
        int result = compat_runtime32_run_import_return_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_HID_MAP_SELFTEST")) {
        int result = hid_bridge32_run_ds4_map_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GL_BUFFER_SELFTEST")) {
        int result = objc_bridge32_run_gl_buffer_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GL_TEXTURE_SELFTEST")) {
        int result = objc_bridge32_run_gl_texture_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_STEAM_ACHIEVEMENT_SELFTEST")) {
        int result = steam_achievement_selftest();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_SUBTITLE_PANEL_SELFTEST")) {
        int result = subtitle_panel_selftest();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_SCREEN_ASPECT_SELFTEST")) {
        int result = install_screen_aspect_patch();
        if (result == 0 && lp32_profile()->screen_aspect_patch) {
            const struct lp32_screen_aspect_patch *patch =
                lp32_profile()->screen_aspect_patch;
            uint32_t renderer = *(const uint32_t *)(uintptr_t)patch->renderer_pointer_slot;
            uint32_t option = *(const uint32_t *)(uintptr_t)patch->option_pointer_slot;
            if (renderer < image.min_address ||
                renderer > image.max_address - 0x634 ||
                option < image.min_address ||
                option > image.max_address - 0x428) result = -1;
            if (result == 0 &&
                objc_bridge32_run_panel_scale_self_test(patch) != 0) result = -1;
        }
        fprintf(stderr, "compat32: screen aspect data check %s\n",
                result == 0 ? "PASS" : "FAIL");
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (steam_bridge32_prepare_environment(lp32_profile()->steam_app_id) != 0 ||
        install_steam_achievement_guard() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (controller_bridge32_install() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (getenv("LP32_CONTROLLER_SELFTEST")) {
        int result = controller_bridge32_run_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_STARTUP_INPUT_LATCH") &&
        install_startup_input_latch_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_SAVE_WORKER_PATCH") &&
        install_save_worker_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_TEXTURE_BIND_GUARD") &&
        install_texture_bind_guard() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (!getenv("LP32_DISABLE_SCREEN_ASPECT_PATCH") &&
        install_screen_aspect_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (objc_bridge32_prepare_shader_cache() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (install_subtitle_panel_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    /* Automatic for the newer Feral ports and Complete Saga; no full import profiling or draw dumps. */
    const char *hitch_option = getenv("LP32_HITCH_LOG");
    if ((hitch_option && strcmp(hitch_option, "0")) ||
        (!hitch_option && (lp32_profile()->title == LP32_TITLE_MARVEL ||
                           lp32_profile()->title == LP32_TITLE_BATMAN3 ||
                           lp32_profile()->title == LP32_TITLE_MOVIE ||
                           lp32_profile()->title == LP32_TITLE_HOBBIT ||
                           lp32_profile()->title == LP32_TITLE_COMPLETE_SAGA))) {
        char path[PATH_MAX];
        const char *home = getenv("HOME");
        int length = snprintf(path, sizeof(path), "%s/Library/Logs/%s/hitches-%ld-%llu.log",
                              home ? home : "/tmp", lp32_profile()->log_directory,
                              (long)getpid(), (unsigned long long)hitch_now());
        const char *destination = hitch_option && strchr(hitch_option, '/') ?
                                  hitch_option : path;
        const char *threshold = getenv("LP32_HITCH_MS");
        if (length > 0 && (size_t)length < sizeof(path) &&
            hitch_start(destination, threshold ? strtod(threshold, NULL) : 25.0) == 0) {
            GUEST_DIAGNOSTIC("compat32: automatic hitch recorder: %s\n", destination);
        } else {
            perror("compat32: hitch recorder unavailable");
        }
    }
    start_saga_data_prefetch();

    const uint32_t *initializers =
        (const void *)(uintptr_t)image.initializer_address;
    int initialization_trapped = 0;
    for (uint32_t index = 0; index < image.initializer_count; ++index) {
        if (index == 0 || index + 1 == image.initializer_count ||
            (index % 32) == 0) {
            printf("initializer[%" PRIu32 "]: 0x%08" PRIx32 "\n",
                   index, initializers[index]);
        }
        fflush(stdout);
        uint32_t result = compat_runtime32_call(initializers[index], NULL, 0);
        if (compat_runtime32_last_call_trapped()) {
            printf("controlled guest exit after initializer[%" PRIu32
                   "] result=0x%08" PRIx32 "\n", index, result);
            initialization_trapped = 1;
            break;
        }
    }

    if (!initialization_trapped) {
        printf("initializers completed: %" PRIu32 "/%" PRIu32 "\n",
               image.initializer_count, image.initializer_count);

        char host_executable_path[PATH_MAX];
        uint32_t host_executable_path_size = sizeof(host_executable_path);
        if (_NSGetExecutablePath(host_executable_path,
                                 &host_executable_path_size) != 0) {
            fprintf(stderr, "game_loader: executable path is too long\n");
            macho_image32_unload(&image);
            return EXIT_FAILURE;
        }
        uint32_t executable_path =
            compat_runtime32_copy_cstring(host_executable_path);
        const char *extra_argument_text = getenv("LP32_GUEST_EXTRA_ARGUMENT");
        uint32_t extra_argument = extra_argument_text && extra_argument_text[0] ?
            compat_runtime32_copy_cstring(extra_argument_text) : 0;
        uint32_t guest_argc = extra_argument ? 2 : 1;
        uint32_t argv_address = compat_runtime32_allocate(
            (guest_argc + 1) * sizeof(uint32_t), 1);
        uint32_t empty_vector = compat_runtime32_allocate(sizeof(uint32_t), 1);
        if (!executable_path || !argv_address || !empty_vector) {
            fprintf(stderr, "game_loader: could not construct guest process arguments\n");
            macho_image32_unload(&image);
            return EXIT_FAILURE;
        }
        uint32_t *guest_argv = (void *)(uintptr_t)argv_address;
        guest_argv[0] = executable_path;
        if (extra_argument) guest_argv[1] = extra_argument;

        const uint32_t main_arguments[] = {
            guest_argc, argv_address, empty_vector, empty_vector,
        };
        uint32_t main_address = lp32_profile()->main_address;
        if (!main_address) main_address = lp32_profile_main_address(&image);
        if (!main_address) {
            fprintf(stderr, "game_loader: could not locate the game's main\n");
            macho_image32_unload(&image);
            return EXIT_FAILURE;
        }
        printf("entering game main: 0x%08" PRIx32 "\n", main_address);
        fflush(stdout);
        uint32_t result = compat_runtime32_call(
            main_address, main_arguments,
            sizeof(main_arguments) / sizeof(main_arguments[0]));
        printf("game main returned/escaped: 0x%08" PRIx32 " trapped=%d\n",
               result, compat_runtime32_last_call_trapped());
    }
    macho_image32_unload(&image);
    return EXIT_SUCCESS;
}
