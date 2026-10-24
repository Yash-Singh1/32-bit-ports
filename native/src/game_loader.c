#include "objc_legacy_bridge.h"
#include "compat_runtime.h"
#include "audio_queue_bridge.h"
#include "cxx_exception_bridge.h"
#include "carbon_bridge.h"
#include "controller_bridge.h"
#include "game_profile.h"
#include "macho_loader.h"
#include "objc_bridge.h"
#include "objc_diagnostics.h"
#include "hitch_recorder.h"
#include "host_diagnostics.h"
#include "steam_bridge.h"
#include "guest_dyld.h"

#include <errno.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdio.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gl_backend.h"

static uintptr_t guest_breakpoint_address;
static unsigned char guest_breakpoint_original_byte;
static volatile sig_atomic_t guest_breakpoint_stepping;
static volatile sig_atomic_t guest_breakpoint_count;
static unsigned guest_breakpoint_limit;
static double guest_breakpoint_after;
static uint64_t guest_breakpoint_armed_ns;
static unsigned guest_breakpoint_skip;
/* LP32_TRACE_GUEST_MARKS: one-shot timing markers (first call only). */
enum { kGuestMarkCapacity = 16 };
static uintptr_t guest_mark_address[kGuestMarkCapacity];
static unsigned char guest_mark_original_byte[kGuestMarkCapacity];
static unsigned guest_mark_count;
static int guest_diagnostic_fd = -1;

/* Finder launches do not retain stderr.  Mirror crash and breakpoint output
   into a persistent per-run file without changing normal console logging. */
#define GUEST_DIAGNOSTIC(...)                                              \
    do {                                                                   \
        dprintf(STDERR_FILENO, __VA_ARGS__);                               \
        if (guest_diagnostic_fd >= 0) {                                    \
            dprintf(guest_diagnostic_fd, __VA_ARGS__);                     \
        }                                                                  \
    } while (0)

static void flush_guest_instruction(uintptr_t address);

/* Capture dyld metadata before guest execution. The signal handler only reads
   this snapshot; querying dyld while a fault interrupted its lock can hang. */
static struct native_image_diagnostic {
    uintptr_t start, end;
    char name[160], uuid[33];
} native_image_diagnostics[512];
static unsigned native_image_diagnostic_count;

static void capture_native_image_diagnostics(void)
{
    unsigned count = 0;
    for (uint32_t i = 0; i < _dyld_image_count() && count < 512; ++i) {
        const struct mach_header_64 *header = (const void *)_dyld_get_image_header(i);
        if (!header || header->magic != MH_MAGIC_64) continue;
        struct native_image_diagnostic *image = &native_image_diagnostics[count];
        const struct load_command *command = (const void *)(header + 1);
        for (uint32_t j = 0; j < header->ncmds; ++j) {
            if (command->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *segment = (const void *)command;
                if (!strcmp(segment->segname, "__TEXT")) {
                    image->start = segment->vmaddr + _dyld_get_image_vmaddr_slide(i);
                    image->end = image->start + segment->vmsize;
                }
            } else if (command->cmd == LC_UUID) {
                const struct uuid_command *uuid = (const void *)command;
                for (unsigned k = 0; k < 16; ++k)
                    snprintf(image->uuid + k * 2, 3, "%02x", uuid->uuid[k]);
            }
            command = (const void *)((const char *)command + command->cmdsize);
        }
        const char *name = _dyld_get_image_name(i);
        snprintf(image->name, sizeof(image->name), "%s", name ? name : "?");
        if (image->start) ++count;
    }
    native_image_diagnostic_count = count;
}

static void report_native_image(uintptr_t address)
{
    for (unsigned i = 0; i < native_image_diagnostic_count; ++i) {
        const struct native_image_diagnostic *image = &native_image_diagnostics[i];
        if (address >= image->start && address < image->end) {
            GUEST_DIAGNOSTIC("compat32: native image=%s base=0x%016llx offset=0x%llx uuid=%s\n",
                image->name, (unsigned long long)image->start,
                (unsigned long long)(address - image->start), image->uuid);
            return;
        }
    }
}

static void open_guest_diagnostic_log(const char *executable_path)
{
    /* A read-only storage probe must not truncate a player's session log. */
    if (getenv("LP32_STEAM_STORAGE_PROBE") || getenv("LP32_NO_DIAGNOSTIC_LOG")) return;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char directory[PATH_MAX];
    char path[PATH_MAX];
    int length = snprintf(directory, sizeof(directory),
                          "%s/Library/Logs/%s", home,
                          lp32_profile()->log_directory);
    if (length < 0 || (size_t)length >= sizeof(directory)) return;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        perror("compat32: create diagnostic log directory");
        return;
    }
    length = snprintf(path, sizeof(path), "%s/last-run.log", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return;

    guest_diagnostic_fd = open(path,
        O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    if (guest_diagnostic_fd < 0) {
        perror("compat32: open diagnostic log");
        return;
    }
    dprintf(guest_diagnostic_fd,
            "compat32: diagnostic session pid=%ld executable=%s\n",
            (long)getpid(), executable_path ? executable_path : "(unknown)");
    fprintf(stderr, "compat32: persistent diagnostic log: %s\n", path);

    /* A Finder or Dock launch has stderr on /dev/null, which loses every
       bridge message a user's run would need to be diagnosed; keep them in
       the session log instead.  Terminal launches keep their stderr. */
    struct stat stderr_stat, null_stat;
    if (fstat(STDERR_FILENO, &stderr_stat) == 0 &&
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
/* Apply the profile's verified byte patches; all signatures are checked
   before any byte changes, and a mismatch (another build) skips them all. */
static int install_code_patches(void)
{
    const struct lp32_code_patch *patches = lp32_profile()->code_patches;
    if (!patches || getenv("LP32_NO_CODE_PATCHES")) return 0; /* A/B measurements */
    for (const struct lp32_code_patch *p = patches; p->address; ++p) {
        if (p->length > sizeof(p->expected) ||
            memcmp((const void *)(uintptr_t)p->address, p->expected, p->length)) {
            fprintf(stderr, "compat32: code patch signature mismatch at %08x; skipping patches\n",
                    p->address);
            return 0;
        }
    }
    for (const struct lp32_code_patch *p = patches; p->address; ++p) {
        if (write_guest_code(p->address, p->replacement, p->length, p->reason)) return -1;
        fprintf(stderr, "compat32: patched %08x: %s\n", p->address, p->reason);
    }
    return 0;
}

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

/* SV_InitGameProgs can compile scripts and spawn entities for nearly a second
   without reaching SCR_UpdateLoadScreen. The database worker keeps advancing,
   but the cinematic badge and progress bar retain the previous frame.

   Yield at script-load/entity-parse entry and at the VM's existing loop timer
   check. These are main-thread boundaries outside string/database locks, not
   arbitrary libc imports inside a critical section. Loading already permits
   SCR_UpdateLoadScreen from DB_FindXAssetHeader while resolving script assets.
   Keep the game's redraw guard, 33 ms throttle, cinematic timing, and real
   progress counters. Never draw gameplay from inside script execution. */
static int install_loading_screen_patch(void)
{
    const struct lp32_loading_screen_patch *layout = lp32_profile()->loading_screen;
    if (!layout) return 0;

    /* Unused tail of the i386 landing-pad page: after e000/e040, before the
       guest context routines at f000. Do not place i386 code on the d000
       page containing the 64-bit gateway landing pads. */
    enum { kStubAddress = 0x7f00e800, kStubStride = 128 };
    const struct lp32_code_signature *signatures[] = {
        &layout->redraw, &layout->is_main_thread, &layout->milliseconds,
        &layout->entry_points[0], &layout->entry_points[1],
    };
    unsigned char original_call[5] = {0xe8};
    int32_t relative = (int32_t)(layout->milliseconds.address -
                                 (layout->vm_loop_timer_call + 5));
    memcpy(original_call + 1, &relative, sizeof(relative));
    /* Validate the entire layout before modifying any guest instructions. */
    for (size_t i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i) {
        const struct lp32_code_signature *signature = signatures[i];
        if (signature->length != 6 ||
            memcmp((void *)(uintptr_t)signature->address, signature->expected, 6)) {
            fprintf(stderr, "compat32: loading screen patch signature mismatch at %08x\n",
                    signature->address);
            return -1;
        }
    }
    if (memcmp((void *)(uintptr_t)layout->vm_loop_timer_call, original_call, 5)) {
        fprintf(stderr, "compat32: loading screen VM timer signature mismatch\n");
        return -1;
    }

    for (unsigned i = 0; i < 3; ++i) {
        bool entry = i < 2;
        const struct lp32_code_signature *point = entry ? &layout->entry_points[i] : NULL;
        uint32_t site = entry ? point->address : layout->vm_loop_timer_call;
        uint32_t base = kStubAddress + i * kStubStride;
        unsigned char code[kStubStride];
        size_t offset = 0;
        /* cdecl frame: leave the original stack arguments untouched and align
           esp to 16 bytes before calling the no-argument guest functions. */
        const unsigned char prologue[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08};
        memcpy(code, prologue, sizeof(prologue)); offset += sizeof(prologue);
        emit_u8(code, &offset, 0xa1); /* mov eax, [cls pointer] */
        emit_u32(code, &offset, layout->client_state_pointer);
        emit_u8(code, &offset, 0x85); emit_u8(code, &offset, 0xc0); /* test eax, eax */
        emit_u8(code, &offset, 0x74);
        size_t null_jump = offset; emit_u8(code, &offset, 0);
        emit_u8(code, &offset, 0x83); emit_u8(code, &offset, 0x78);
        emit_u8(code, &offset, 0x0c); emit_u8(code, &offset, 3); /* cmp [eax+12], 3 */
        emit_u8(code, &offset, 0x75);
        size_t state_jump = offset; emit_u8(code, &offset, 0);
        emit_u8(code, &offset, 0xe8);
        emit_rel32(code, &offset, base, layout->is_main_thread.address);
        emit_u8(code, &offset, 0x84); emit_u8(code, &offset, 0xc0); /* test al, al */
        emit_u8(code, &offset, 0x74);
        size_t thread_jump = offset; emit_u8(code, &offset, 0);
        emit_u8(code, &offset, 0xe8);
        emit_rel32(code, &offset, base, layout->redraw.address);
        code[null_jump] = (uint8_t)(offset - null_jump - 1);
        code[state_jump] = (uint8_t)(offset - state_jump - 1);
        code[thread_jump] = (uint8_t)(offset - thread_jump - 1);
        emit_u8(code, &offset, 0xc9); /* leave: original stack and ebp */
        if (entry) {
            memcpy(code + offset, point->expected, point->length);
            offset += point->length;
        }
        emit_u8(code, &offset, 0xe9);
        emit_rel32(code, &offset, base, entry ? site + point->length :
                                              layout->milliseconds.address);
        if (write_guest_code(base, code, offset, "loading screen redraw stub")) return -1;
        unsigned char hook[6] = {entry ? 0xe9 : 0xe8, 0, 0, 0, 0, 0x90};
        relative = (int32_t)(base - (site + 5));
        memcpy(hook + 1, &relative, sizeof(relative));
        if (write_guest_code(site, hook, entry ? 6 : 5, "loading screen redraw hook")) return -1;
    }
    fprintf(stderr, "compat32: installed cooperative mission loading screen redraws\n");
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

static void flush_guest_instruction(uintptr_t address)
{
    __builtin___clear_cache((char *)address, (char *)(address + 1));
}

static void guest_crash_diagnostic(int signal_number, siginfo_t *info,
                                   void *opaque_context)
{
    ucontext_t *context = opaque_context;
    uint64_t rip = context->uc_mcontext->__ss.__rip;
    uint64_t rsp = context->uc_mcontext->__ss.__rsp;
    for (unsigned index = 0; signal_number == SIGTRAP && index < guest_mark_count; ++index) {
        if (rip != guest_mark_address[index] + 1) continue;
        *(volatile unsigned char *)guest_mark_address[index] = guest_mark_original_byte[index];
        flush_guest_instruction(guest_mark_address[index]);
        context->uc_mcontext->__ss.__rip = guest_mark_address[index];
        char line[96];
        int length = snprintf(line, sizeof(line), "compat32: guest mark 0x%08lx t=%.3f\n",
                              (unsigned long)guest_mark_address[index],
                              clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);
        if (length > 0) write(STDERR_FILENO, line, (size_t)length);
        return;
    }
    if (signal_number == SIGTRAP && guest_breakpoint_address) {
        if (guest_breakpoint_stepping) {
            *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
            flush_guest_instruction(guest_breakpoint_address);
            context->uc_mcontext->__ss.__rflags &= ~UINT64_C(0x100);
            guest_breakpoint_stepping = 0;
            return;
        }
        if (rip == guest_breakpoint_address + 1) {
            if (getenv("LP32_TEST_EXIT_AT_GUEST_BREAKPOINT")) {
                GUEST_DIAGNOSTIC("compat32: test reached guest breakpoint 0x%08lx\n",
                                 (unsigned long)guest_breakpoint_address);
                _Exit(EXIT_SUCCESS);
            }
            /* LP32_TRACE_GUEST_AFTER=<seconds> ignores hits until then. */
            bool early = guest_breakpoint_after > 0 &&
                (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - guest_breakpoint_armed_ns) / 1e9 <
                    guest_breakpoint_after;
            unsigned count = early ? guest_breakpoint_count : ++guest_breakpoint_count;
            if (early) {
                *(volatile unsigned char *)guest_breakpoint_address = guest_breakpoint_original_byte;
                flush_guest_instruction(guest_breakpoint_address);
                context->uc_mcontext->__ss.__rip = guest_breakpoint_address;
                context->uc_mcontext->__ss.__rflags |= UINT64_C(0x100);
                guest_breakpoint_stepping = 1;
                return;
            }
            const uint32_t *words = (const void *)(uintptr_t)rsp;
            static long every = -1;
            if (every < 0) {
                const char *text = getenv("LP32_TRACE_GUEST_EVERY");
                every = text ? strtol(text, NULL, 0) : 1;
                if (every < 1) every = 1;
            }
            if (count > guest_breakpoint_skip && count % (unsigned)every == 0) {
            GUEST_DIAGNOSTIC(
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
            if (rsp >= UINT32_C(0x1000) && rsp < UINT64_C(0xfffff000)) {
                for (unsigned index = 0; index < 12; ++index) {
                    GUEST_DIAGNOSTIC(
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
                        GUEST_DIAGNOSTIC(
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
                        const char *words_text = getenv("LP32_TRACE_GUEST_DUMP_WORDS");
                        unsigned words = words_text ? (unsigned)strtoul(words_text, NULL, 0) : 16;
                        if (words > 16384) words = 16384;
                        for (unsigned index = 0; index < words; ++index) {
                            GUEST_DIAGNOSTIC(
                                    "compat32: breakpoint mem[%08llx]=0x%08x\n",
                                    (unsigned long long)(dump_address + index * 4),
                                    dump[index]);
                        }
                        uintptr_t indirect = dump[0];
                        if (indirect >= UINT32_C(0x1000) &&
                            indirect < UINT32_C(0x80000000)) {
                            const uint32_t *object = (const void *)indirect;
                            for (unsigned index = 0; index < 32; ++index) {
                                GUEST_DIAGNOSTIC(
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
                            GUEST_DIAGNOSTIC(
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
    GUEST_DIAGNOSTIC(
            "compat32: signal %d code=%d address=%p "
            "rip=0x%016llx rsp=0x%016llx\n",
            signal_number, info->si_code, info->si_addr, rip, rsp);
    if (rip > UINT32_MAX) {
        uintptr_t address = rip, frame = context->uc_mcontext->__ss.__rbp;
        for (unsigned i = 0; i < 16; ++i) {
            GUEST_DIAGNOSTIC("compat32: native return[%u]=0x%016llx\n",
                            i, (unsigned long long)address);
            report_native_image(address);
            /* Symbol lookup can acquire dyld locks. Keep it opt-in; the
               bounded raw frame walk is sufficient for offline symbolication. */
            if (getenv("LP32_TRACE_NATIVE_CRASH")) {
            Dl_info symbol = {0};
            dladdr((void *)address, &symbol);
            GUEST_DIAGNOSTIC("compat32: native frame %u %p %s + %llu (%s)\n", i,
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
    GUEST_DIAGNOSTIC(
            "compat32: crash cs=0x%llx mode-recoveries to64=%u to32=%u\n",
            (unsigned long long)context->uc_mcontext->__ss.__cs,
            mode_to64, mode_to32);
    GUEST_DIAGNOSTIC(
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
    uint64_t native_words[8];
    mach_vm_size_t native_count = 0;
    if (rsp > UINT32_MAX && mach_vm_read_overwrite(mach_task_self(), rsp,
            sizeof(native_words), (mach_vm_address_t)native_words, &native_count) == KERN_SUCCESS &&
            native_count == sizeof(native_words)) {
        for (unsigned i = 0; i < 8; ++i)
            GUEST_DIAGNOSTIC("compat32: native stack[%u]=0x%016llx\n", i,
                            (unsigned long long)native_words[i]);
    }
    objc_diag_crash_report(STDERR_FILENO, guest_diagnostic_fd);
    /* An allocator trap (caulk's maybe_create_free_node) leaves the bad
       block in rsi/rax: say whether it is one of the bridge's audio buffers. */
    const uint64_t suspects[] = {context->uc_mcontext->__ss.__rsi, context->uc_mcontext->__ss.__rax,
                                 context->uc_mcontext->__ss.__rdi, context->uc_mcontext->__ss.__rbx};
    audio_queue_bridge32_crash_report(STDERR_FILENO, guest_diagnostic_fd, suspects, 4);
    if (rsp >= UINT32_C(0x1000) && rsp < UINT64_C(0xfffff000)) {
        const uint32_t *words = (const void *)(uintptr_t)rsp;
        for (unsigned index = 0; index < 16; ++index) {
            GUEST_DIAGNOSTIC("compat32: crash stack[%u]=0x%08x\n",
                             index, words[index]);
        }
    }
    /* LP32_CRASH_DUMP=0xaddr:dwords[,...] appends guest cells to the report. */
    const char *dump = getenv("LP32_CRASH_DUMP");
    if (dump && dump[0]) {
        char buffer[256];
        strlcpy(buffer, dump, sizeof(buffer));
        char *cursor = buffer;
        for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
            char *colon = strchr(item, ':');
            uint32_t address = (uint32_t)strtoul(item, NULL, 0);
            unsigned count = colon ? (unsigned)strtoul(colon + 1, NULL, 0) : 8;
            if (address < 0x1000 || address >= 0x7f000000 || count > 64) continue;
            const uint32_t *words = (const void *)(uintptr_t)address;
            for (unsigned index = 0; index < count; ++index) {
                GUEST_DIAGNOSTIC("compat32: crash mem[0x%08x]=0x%08x\n",
                                 address + 4 * index, words[index]);
            }
        }
    }
    fsync(guest_diagnostic_fd >= 0 ? guest_diagnostic_fd : STDERR_FILENO);
    _exit(128 + signal_number);
}

static void install_guest_crash_diagnostics(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guest_crash_diagnostic;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &action, NULL);
}

/* LP32_TRACE_GUEST_MARKS=addr[,addr...] logs the first call of each guest
   function with a timestamp, then runs it unmodified (no re-arming). */
static int configure_guest_marks(const struct macho_image32 *image)
{
    const char *text = getenv("LP32_TRACE_GUEST_MARKS");
    if (!text || !text[0]) return 0;
    char buffer[512];
    strlcpy(buffer, text, sizeof(buffer));
    char *cursor = buffer;
    size_t page_size = (size_t)getpagesize();
    for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
        char *end = NULL;
        unsigned long long address = strtoull(item, &end, 0);
        if (!end || *end || address < image->min_address || address >= image->max_address ||
            guest_mark_count == kGuestMarkCapacity) {
            fprintf(stderr, "game_loader: invalid LP32_TRACE_GUEST_MARKS entry: %s\n", item);
            return -1;
        }
        uintptr_t page = (uintptr_t)address & ~(page_size - 1);
        if (mprotect((void *)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            perror("game_loader: mprotect guest mark page");
            return -1;
        }
        guest_mark_address[guest_mark_count] = (uintptr_t)address;
        guest_mark_original_byte[guest_mark_count] = *(const unsigned char *)(uintptr_t)address;
        *(volatile unsigned char *)(uintptr_t)address = 0xcc;
        flush_guest_instruction((uintptr_t)address);
        ++guest_mark_count;
    }
    fprintf(stderr, "compat32: armed %u guest marks\n", guest_mark_count);
    return 0;
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
    const char *after_text = getenv("LP32_TRACE_GUEST_AFTER");
    guest_breakpoint_after = after_text ? strtod(after_text, NULL) : 0;
    guest_breakpoint_armed_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
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
    if (guest_breakpoint_after > 0) {
        /* Insert the breakpoint only once the delay has passed, so earlier
           hits cost nothing. */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(guest_breakpoint_after * 1e9)),
                       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
            flush_guest_instruction(guest_breakpoint_address);
        });
        guest_breakpoint_after = 0;
    } else {
        *(volatile unsigned char *)guest_breakpoint_address = 0xcc;
        flush_guest_instruction(guest_breakpoint_address);
    }
    fprintf(stderr,
            "compat32: armed guest breakpoint 0x%08llx original=0x%02x "
            "skip=%u limit=%u\n",
            parsed, guest_breakpoint_original_byte, guest_breakpoint_skip,
            guest_breakpoint_limit);
    return 0;
}

/* LP32_TRACE_CONSOLE_PRINT=0xaddr:N replaces the guest function at addr with
   a jump to _lp32_trace_print, which logs its Nth stack argument as a string
   (for example the game's console print) and returns 0. Diagnostic only: the
   original function no longer runs. */
static void configure_console_print_trace(const struct macho_image32 *image)
{
    const char *text = getenv("LP32_TRACE_CONSOLE_PRINT");
    if (!text || !text[0]) return;
    char *end = NULL;
    unsigned long address = strtoul(text, &end, 0);
    unsigned argument = end && *end == ':' ? (unsigned)strtoul(end + 1, NULL, 0) : 0;
    if (address < image->min_address || address + 5 > image->max_address || argument > 16) {
        fprintf(stderr, "game_loader: invalid LP32_TRACE_CONSOLE_PRINT: %s\n", text);
        return;
    }
    compat_runtime32_set_trace_print_argument(argument);
    uint32_t thunk = compat_runtime32_guest_callback("_lp32_trace_print");
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = address & ~(page_size - 1);
    if (!thunk || mprotect((void *)page, page_size * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
    uint8_t *code = (uint8_t *)(uintptr_t)address;
    int32_t displacement = (int32_t)(thunk - (uint32_t)(address + 5));
    code[0] = 0xe9;
    memcpy(code + 1, &displacement, 4);
    flush_guest_instruction(address);
    fprintf(stderr, "compat32: console print trace at 0x%08lx argument %u\n", address, argument);
}

/* MW2 campaign: SV_AddServerCommand ends the game with "Reliable command
   buffer overflow" when more than 4 KB of server commands, or 128 of them,
   wait for the client. Route its two Com_Error calls through the loader so
   the log shows which commands filled it; the game then errors as before. */
static void configure_command_overflow_dump(const struct macho_image32 *image)
{
    if (lp32_profile()->title != LP32_TITLE_MW2) return;
    uint32_t com_error = macho_image32_find_symbol(image, "__Z9Com_Error11errorParm_tPKcz");
    static const uint32_t sites[] = {0x002ee211, 0x002ee24e};
    if (!com_error) return;
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        const uint8_t *code = (const uint8_t *)(uintptr_t)sites[i];
        int32_t displacement;
        memcpy(&displacement, code + 1, 4);
        if (code[0] != 0xe8 || sites[i] + 5 + (uint32_t)displacement != com_error) return;
    }
    uint32_t thunk = compat_runtime32_guest_callback("_lp32_mw2_command_overflow");
    size_t page_size = (size_t)getpagesize();
    uintptr_t page = sites[0] & ~(page_size - 1);
    if (!thunk || mprotect((void *)page, page_size * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
    compat_runtime32_set_command_overflow_target(com_error);
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        int32_t displacement = (int32_t)(thunk - (sites[i] + 5));
        memcpy((uint8_t *)(uintptr_t)sites[i] + 1, &displacement, 4);
        flush_guest_instruction(sites[i]);
    }
    mprotect((void *)page, page_size * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "compat32: server command overflow dump installed\n");
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
    static const char *const candidates[] = {"LEGOPirates", "LEGOCloneWars", "LEGOMarvel", "LEGOCompleteSaga", "COD4", "COD4MP", "COD2", "COD2MP", "MW2", "MW2MP", "MW3", "MW3MP"};
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

int main(int argc, char **argv)
{
    /* A write to a socket whose peer has gone (e.g. the Steam client IPC)
       must fail with EPIPE, not silently kill the game. Launched from Finder,
       MW2 otherwise dies of SIGPIPE seconds after its first frame. */
    compat_runtime32_reserve_guest_arena();
    signal(SIGPIPE, SIG_IGN);
    install_guest_crash_diagnostics();
    capture_native_image_diagnostics();
    compat_runtime32_set_diagnostic_sink(runtime_diagnostic_line);
    if (getenv("LP32_FOCUS_SELFTEST")) {
        int expected = atoi(getenv("LP32_FOCUS_SELFTEST"));
        return objc_bridge32_run_focus_self_test(expected) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_CRASH_DIAGNOSTIC_SELFTEST")) {
        raise(SIGSEGV);
        return EXIT_FAILURE;
    }
    if (getenv("LP32_POINTER_SELFTEST")) {
        return objc_bridge32_run_pointer_self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_GAMMA_SELFTEST")) {
        return objc_bridge32_run_gamma_self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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

    struct macho_image32 image;
    if (macho_image32_load(image_path, &image) != 0) return EXIT_FAILURE;
    if (lp32_profile_select(&image, image_path) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    lp32_gl_backend_select(lp32_profile()->prefers_lp32gl ? LP32_GL_BACKEND_MESA : LP32_GL_BACKEND_APPLE);
    open_guest_diagnostic_log(argc > 0 ? argv[0] : NULL);
    char host[768];
    lp32_host_description(host, sizeof(host));
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

    if (configure_guest_marks(&image) != 0 || configure_guest_breakpoint(&image) != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    configure_console_print_trace(&image);
    configure_command_overflow_dump(&image);

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
    if (getenv("LP32_PURE_ATOMIC_SELFTEST")) {
        int result = compat_runtime32_run_pure_atomic_self_test();
        macho_image32_unload(&image);
        return result ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (getenv("LP32_ONCE_SELFTEST")) {
        int result = compat_runtime32_run_sync_self_test();
        macho_image32_unload(&image);
        return result ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (getenv("LP32_DYLD_FIXTURE_SELFTEST")) {
        int result = guest_dyld32_initialize(image_path);
        if (!result) result = guest_dyld32_self_test();
        macho_image32_unload(&image);
        return result ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (lp32_title_is_cod_carbon(lp32_profile()->title)) {
        /* The renderer accepts only an exact 24-bit depth-buffer capability.
           Current Apple GPUs offer 32-bit depth. Accept both; the original
           AGL request still chooses a real format with at least 24 bits. */
        const struct lp32_code_signature *check = &lp32_profile()->depth_capability_check;
        if (check->length) {
            uint8_t *code = (void *)(uintptr_t)check->address;
            if (memcmp(code, check->expected, check->length) ||
                mprotect((void *)(uintptr_t)(check->address & ~4095u), 4096, PROT_READ | PROT_WRITE | PROT_EXEC)) {
                fprintf(stderr, "game_loader: %s depth capability signature mismatch or protection error\n",
                        lp32_profile()->name);
                return EXIT_FAILURE;
            }
            code[check->length - 1] = 0x18;
            flush_guest_instruction(check->address);
        }
        if (guest_dyld32_initialize(image_path) || guest_dyld32_bind_main_cxx(&image))
            return EXIT_FAILURE;
        char bink_path[PATH_MAX];
        int length = snprintf(bink_path, sizeof(bink_path), "%s/libBinkMachOx86.dylib", guest_dyld32_game_root());
        unsigned bink_imports = 0;
        for (unsigned i = 0; i < image.import_count; ++i)
            if (!strncmp(image.imports[i].name, "_Bink", 5)) ++bink_imports;
        uint32_t bink = bink_imports && length > 0 && (size_t)length < sizeof(bink_path) ? guest_dyld32_open(bink_path, RTLD_NOW) : 0;
        if (bink_imports && !bink) {
            fprintf(stderr, "game_loader: cannot load original Bink library: %s\n", (char *)(uintptr_t)guest_dyld32_error());
            return EXIT_FAILURE;
        }
        unsigned bound = 0;
        for (unsigned i = 0; i < image.import_count; ++i) {
            const struct macho_import32 *import = &image.imports[i];
            if (strncmp(import->name, "_Bink", 5)) continue;
            uint32_t address = guest_dyld32_symbol(bink, import->name + 1);
            if (!address || macho_image32_bind_import(import, address)) return EXIT_FAILURE;
            ++bound;
        }
        if (bink_imports) fprintf(stderr, "compat32: bound %u Bink imports to the original i386 library\n", bound);
    }
    if (lp32_title_is_mw_sdl(lp32_profile()->title)) {
        if (guest_dyld32_initialize(image_path)) return EXIT_FAILURE;
        /* MW3 multiplayer links Bink 2; its exports keep the _Bink prefix. */
        const char *libraries[] = {
            lp32_profile()->title == LP32_TITLE_MW3_MP ? "libBink2Macx86.dylib" : "libBinkMacx86.dylib",
            "libMilesX86.dylib"};
        /* Imports are matched by prefix; _RADSS_CAInstallDriver is Miles'
           CoreAudio sound-system driver (Bink exports its own copy); _RIB_ is
           its provider registry (MW3 multiplayer). */
        const char *prefixes[][3] = {{"_Bink", NULL, NULL}, {"_AIL_", "_RADSS_", "_RIB_"}};
        for (unsigned library = 0; library < 2; ++library) {
            char path[PATH_MAX];
            int length = snprintf(path, sizeof(path), "%s/%s", guest_dyld32_game_root(), libraries[library]);
            if (length < 0 || (size_t)length >= sizeof(path)) return EXIT_FAILURE;
            uint32_t handle = guest_dyld32_open(path, RTLD_NOW);
            if (!handle) {
                fprintf(stderr, "game_loader: cannot load %s %s: %s\n", lp32_profile()->name, libraries[library],
                        (char *)(uintptr_t)guest_dyld32_error());
                return EXIT_FAILURE;
            }
            unsigned bound = 0;
            for (unsigned i = 0; i < image.import_count; ++i) {
                const struct macho_import32 *import = &image.imports[i];
                bool matched = false;
                for (unsigned p = 0; p < 3 && prefixes[library][p]; ++p)
                    matched |= !strncmp(import->name, prefixes[library][p], strlen(prefixes[library][p]));
                if (!matched) continue;
                uint32_t address = guest_dyld32_symbol(handle, import->name + 1);
                if (!address || macho_image32_bind_import(import, address)) return EXIT_FAILURE;
                ++bound;
            }
            fprintf(stderr, "compat32: bound %u %s imports to %s\n", bound, lp32_profile()->name, libraries[library]);
        }
    }
    if (lp32_profile()->title == LP32_TITLE_MW2_MP) {
        /* The playlist download creates its file before any data arrives.
           Quitting mid-download leaves an empty file that the next launch
           loads, and the fastfile reader waits forever for more data. */
        char pattern[PATH_MAX];
        int length = snprintf(pattern, sizeof(pattern), "%s/../GameData/zone/*/mp_playlists*.ff",
                              guest_dyld32_game_root());
        glob_t found = {0};
        if (length > 0 && (size_t)length < sizeof(pattern) && glob(pattern, 0, NULL, &found) == 0) {
            for (size_t i = 0; i < found.gl_pathc; ++i) {
                struct stat info;
                if (!stat(found.gl_pathv[i], &info) && info.st_size == 0 && !unlink(found.gl_pathv[i]))
                    fprintf(stderr, "compat32: removed empty playlist %s\n", found.gl_pathv[i]);
            }
        }
        globfree(&found);
    }
    if (getenv("LP32_BUNDLE_SELFTEST")) {
        int result = objc_bridge32_run_bundle_self_test();
        macho_image32_unload(&image);
        return result ? EXIT_FAILURE : EXIT_SUCCESS;
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
    if (getenv("LP32_CARBON_INPUT_SELFTEST")) {
        int result = objc_bridge32_run_carbon_input_self_test();
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
    if (getenv("LP32_EXCEPTION_SELFTEST")) {
        int result = cxx_exception_bridge32_run_self_test();
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
    if (getenv("LP32_CARBON_FILE_SELFTEST")) {
        return carbon_bridge32_run_file_self_test() ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (getenv("LP32_CARBON_DISPATCH_SELFTEST")) {
        int result = carbon_bridge32_run_dispatch_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_IMPORT_RETURN_SELFTEST")) {
        int result = compat_runtime32_run_import_return_self_test();
        macho_image32_unload(&image);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (getenv("LP32_COD4_DOWNLOAD_SELFTEST")) {
        int result = objc_legacy32_run_download_self_test();
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

    if (install_code_patches() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }
    if (install_loading_screen_patch() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    if (objc_bridge32_prepare_shader_cache() != 0) {
        macho_image32_unload(&image);
        return EXIT_FAILURE;
    }

    /* Lightweight, bounded reports; no full import profiling or draw dumps.
       COD4 deliberately redraws some loading screens at 30 Hz, so its default
       threshold is 50 ms rather than treating each loading frame as a hitch. */
    bool cod_hitches = lp32_title_is_cod_carbon(lp32_profile()->title);
    bool modern_warfare_hitches = lp32_profile()->title == LP32_TITLE_MW2 ||
                                  lp32_profile()->title == LP32_TITLE_MW2_MP ||
                                  lp32_profile()->title == LP32_TITLE_MW3 ||
                                  lp32_profile()->title == LP32_TITLE_MW3_MP;
    const char *hitch_option = getenv("LP32_HITCH_LOG");
    if ((hitch_option && strcmp(hitch_option, "0")) ||
        (!hitch_option && (lp32_profile()->title == LP32_TITLE_MARVEL ||
                           cod_hitches || modern_warfare_hitches))) {
        char path[PATH_MAX];
        const char *home = getenv("HOME");
        int length = snprintf(path, sizeof(path), "%s/Library/Logs/%s/hitches-%ld-%llu.log",
                              home ? home : "/tmp", lp32_profile()->log_directory,
                              (long)getpid(), (unsigned long long)hitch_now());
        const char *destination = hitch_option && strchr(hitch_option, '/') ?
                                  hitch_option : path;
        const char *threshold = getenv("LP32_HITCH_MS");
        if (length > 0 && (size_t)length < sizeof(path) &&
            hitch_start(destination, threshold ? strtod(threshold, NULL) :
                        (cod_hitches ? 50.0 : 25.0)) == 0) {
            GUEST_DIAGNOSTIC("compat32: automatic hitch recorder: %s\n", destination);
        } else {
            perror("compat32: hitch recorder unavailable");
        }
    }

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
        if (getenv("LP32_OBJC_FACTORY_TEST_CLASS")) {
            return objc_legacy32_run_factory_self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        if (getenv("LP32_INITIALIZERS_SELFTEST")) {
            macho_image32_unload(&image);
            return EXIT_SUCCESS;
        }

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
        /* Aspyr's documented-in-code -g option enters the game directly
           instead of opening the obsolete embedded Game Guide browser. */
        if (!extra_argument_text && lp32_title_is_cod_carbon(lp32_profile()->title))
            extra_argument_text = "-g";
        /* Space-separated, so tests can pass e.g. "-g +map killhouse". */
        enum { kMaxExtraArguments = 16 };
        uint32_t extra_arguments[kMaxExtraArguments];
        uint32_t extra_count = 0;
        for (const char *cursor = extra_argument_text; cursor && *cursor && extra_count < kMaxExtraArguments;) {
            while (*cursor == ' ') ++cursor;
            size_t length = strcspn(cursor, " ");
            if (!length) break;
            char argument[256];
            snprintf(argument, sizeof argument, "%.*s", (int)length, cursor);
            extra_arguments[extra_count] = compat_runtime32_copy_cstring(argument);
            if (!extra_arguments[extra_count]) break;
            ++extra_count;
            cursor += length;
        }
        uint32_t guest_argc = 1 + extra_count;
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
        for (uint32_t i = 0; i < extra_count; ++i) guest_argv[1 + i] = extra_arguments[i];

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
        initialization_trapped = compat_runtime32_last_call_trapped();
    }
    macho_image32_unload(&image);
    return initialization_trapped ? EXIT_FAILURE : EXIT_SUCCESS;
}
