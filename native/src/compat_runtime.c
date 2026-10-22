#include "compat_runtime.h"
#include "tlv_bridge.h"
#include "hitch_recorder.h"
#include "audio_bridge.h"
#include "audio_converter_bridge.h"
#include "sound_manager_bridge.h"
#include "time_manager_bridge.h"
#include "audio_queue_bridge.h"
#include "openal_bridge.h"
#include "curl_bridge.h"
#include "controller_bridge.h"
#include "asphalt6_controller.h"
#include "game_profile.h"
#include "steam_bridge.h"
#include "resource_bridge.h"
#include "carbon_bridge.h"
#include "crypto_bridge.h"
#include "quicktime_bridge.h"
#include "stl_bridge.h"
#include "libcpp_bridge.h"
#include "libcpp_future_bridge.h"
#include "blocks_bridge.h"
#include "rtti_bridge.h"
#include "regex_bridge.h"
#include "iconv_bridge.h"
#include "zlib_bridge.h"
#include "sdl_bridge.h"
#include <CommonCrypto/CommonDigest.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/utsname.h>
#include <wchar.h>
#include "socket_bridge.h"
#include "dlfcn_bridge.h"
#include "guest_dyld.h"
#include "objc_bridge.h"
#include "cxx_exception_bridge.h"
#include "name_match.h"
#include "guest_once.h"

#include <architecture/i386/table.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/CoreAudio.h>
#include <copyfile.h>
#include <IOKit/IOKitLib.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <i386/user_ldt.h>
#include <limits.h>
#include <locale.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <netdb.h>
#include <zlib.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>
#include <unistd.h>

extern uint64_t run_compat32(uint32_t eip, uint32_t esp, uint16_t cs32);
extern void lp32_import_gateway(void);
extern const uint8_t lp32_context_start[], lp32_context_end[];
extern const uint8_t lp32_context_setjmp[], lp32_context_fast_setjmp[], lp32_context_sigsetjmp[];
extern const uint8_t lp32_context_longjmp[], lp32_context_fast_longjmp[];
extern const uint8_t lp32_context_save_helper[], lp32_context_restore_helper[];
extern const uint8_t lp32_context_atomic_add32[], lp32_context_atomic_cas32[];
extern const uint8_t lp32_context_strcmp[], lp32_context_tolower[], lp32_context_lower_table[];
static uint32_t guest_context_symbol(const char *name);

uint16_t lp32_cs32;
int lp32_callee_pops_struct_return;
_Thread_local int lp32_leave_guest;
_Thread_local int lp32_return_fp_kind;
_Thread_local union {
    float f;
    double d;
    uint64_t bits;
} lp32_fp_result;

enum {
    kBridgeCodeBase = 0x7f000000,
    kBridgeCodeSize = 0x00010000,
    kBridgeDataBase = 0x7f010000,
    kBridgeDataSize = 0x00010000,
    kLegacyGuestStackBase = 0x7c000000,
    /* Where 32-bit macOS placed secondary thread stacks. */
    kNativeGuestStackBase = 0xb0000000,
    kGuestStackSize = 0x03000000,
    kGuestStackPerThread = 0x00100000,
    /* Pages 0x0000-0xcfff hold only i386 code and far-pointer data; the
       64-bit landing pads live on their own page and the i386 landing
       trampolines on another.  Rosetta normally translates each region in
       the mode it is executed in, but see the mode guards below: a thread
       can arrive at either kind of pad in the wrong mode. */
    kExitThunkOffset = 0x000,
    kExitFarPointerOffset = 0x010,
    kGatewayFarPointerOffset = 0x040,
    kImportThunksOffset = 0x1000,
    kInlineImportCapacity = 1024,
    kOverflowImportBase = 0x7f020000,
    kOverflowImportCapacity = 3072,
    kOverflowImportSize = kOverflowImportCapacity * 16,
    /* Preserve the first 4096 thunk addresses and the file/audio/controller
       token ranges at 0x7f030000..0x7f08ffff when expanding for newer images. */
    kExtendedImportBase = 0x7f100000,
    kExtendedImportSize = (MACHO_IMAGE32_MAX_IMPORTS - kInlineImportCapacity -
                          kOverflowImportCapacity) * 16,
    kImportThunkSize = 16,
    kDynamicThunksOffset = 0x5000,
    kDynamicThunkCapacity = 2048,
    kHost64PadsOffset = 0xd000,
    kReturn64Offset = kHost64PadsOffset + 0x000,
    kGateway64Offset = kHost64PadsOffset + 0x040,
    kReturn64AltOffset = kHost64PadsOffset + 0x080,
    kGateway64AltOffset = kHost64PadsOffset + 0x0c0,
    /* Further retry pads after the alternates: one 0x80 stride holds a
       return and a gateway pad, 0x40 holds one landing trampoline. */
    kHost64RetryOffset = kHost64PadsOffset + 0x100,
    kLanding32Offset = 0xe000,
    kLanding32AltOffset = 0xe040,
    kLanding32RetryOffset = 0xe080,
    kModeRetryPads = 28,
    kGuestContextOffset = 0xf000,
    kGuestLowercaseOffset = 0x7000,
    /* Mode-guard event counters: the bridge data region holds import data
       cells from 0x0000, the rune locale from 0x6000, Cocoa proxy slots from
       0x8000 (objc_bridge) and the
       startup input latch at 0xfff0 and the SaveCore start latch at 0xffe0
       (game_loader); the cells stop short of this. */
    kModeGuardCountersOffset = 0x7ff0,
    kWrongModeTo64Counter = kBridgeDataBase + kModeGuardCountersOffset + 0,
    kWrongModeTo32Counter = kBridgeDataBase + kModeGuardCountersOffset + 4,
    kModeGuardFailedCounter = kBridgeDataBase + kModeGuardCountersOffset + 8,
};

_Static_assert(kHost64RetryOffset + kModeRetryPads * 0x80 <= kLanding32Offset,
               "64-bit retry pads overflow their page");
/* game_loader places COD4's loading-screen stubs at 0x7f00e800. */
_Static_assert(kLanding32RetryOffset + kModeRetryPads * 0x40 <= 0xe800,
               "i386 retry trampolines overlap game_loader's stubs at 0xe800");
_Static_assert(kImportThunksOffset + kInlineImportCapacity * kImportThunkSize <=
               kDynamicThunksOffset, "static import thunks overlap dynamic thunks");
_Static_assert(kDynamicThunksOffset + kDynamicThunkCapacity * kImportThunkSize <=
               kHost64PadsOffset, "dynamic thunks overlap 64-bit landing pads");

/* Where the 64-bit gateway sends a thread back into i386 code. */
uint32_t lp32_landing32 = kBridgeCodeBase + kLanding32Offset;

/* Stale stack words are read by MW2 code with mismatched varargs (e.g.
   Scr_Objective_Position formats int zeros with %f). At 0x7c000000+ a stale
   stack pointer read as a double's high word is ~1e300 and printed as a
   300-digit number; at 32-bit macOS's 0xb0000000 it is a tiny negative value
   and prints "-0.00", as the game shipped with. compat_runtime32_reserve_
   guest_arena moves stacks there when the range is free. */
static uint32_t guest_stack_base = kLegacyGuestStackBase;


struct __attribute__((packed)) far_ptr32 {
    uint32_t offset;
    uint16_t selector;
};

static struct macho_image32 *current_image;
int compat_runtime32_pointer_import_matches(uint32_t address, const char *name) {
    if (!current_image || !address) return 0;
    for (uint32_t i=0;i<current_image->import_count;++i) {
        const struct macho_import32 *import=&current_image->imports[i];
        if (import->kind==MACHO_IMPORT32_POINTER && !strcmp(import->name,name) &&
            *(const uint32_t *)(uintptr_t)import->address==address) return 1;
    }
    return 0;
}
int compat_runtime32_guest_symbol_matches(uint32_t address, const char *name) {
    return current_image && address && macho_image32_find_symbol(current_image, name) == address;
}
static _Thread_local int last_call_trapped;
/* edx of the last guest call: the high half of an i386 pair return. */
static _Thread_local uint32_t last_call_edx;
static uint32_t keymgr_slots[64];

enum {
    kGuestHeapMinimum = 0x02000000,
    kGuestHeapEnd = 0x70000000,
    kGuestHeapHeaderSize = 16,
    kGuestHeapBinCount = 64,
    kGuestHeapSiteCapacity = 512,
    kGuestHeapLiveMagic = 0x4c503332, /* LP32 */
    kGuestHeapFreeMagic = 0x46524545, /* FREE */
    kGuestHeapHostSite = 0xff000001,
    kGuestHeapStringSite = 0xff000002,
    kGuestHeapRenderPoolSite = 0xff000003,
};

struct guest_heap_site_stats {
    uint32_t site;
    uint64_t allocations;
    uint64_t frees;
    uint64_t allocated_bytes;
    uint64_t freed_bytes;
    uint64_t live_bytes;
    uint64_t peak_live_bytes;
};

struct guest_heap_stats {
    uint64_t allocation_calls;
    uint64_t bump_allocations;
    uint64_t reused_allocations;
    uint64_t split_blocks;
    uint64_t failed_allocations;
    uint64_t free_calls;
    uint64_t valid_frees;
    uint64_t null_frees;
    uint64_t invalid_frees;
    uint64_t double_frees;
    uint64_t realloc_calls;
    uint64_t realloc_in_place;
    uint64_t realloc_moved;
    uint64_t live_blocks;
    uint64_t live_requested;
    uint64_t live_capacity;
    uint64_t peak_live_requested;
    uint64_t peak_live_capacity;
    uint64_t reusable_capacity;
    uint64_t abandoned_capacity;
    uintptr_t high_water;
};

static uintptr_t guest_heap_base = kGuestHeapMinimum;
static uintptr_t guest_heap_cursor = kGuestHeapMinimum;
static uint32_t guest_heap_free_bins[kGuestHeapBinCount];
static struct guest_heap_site_stats guest_heap_sites[kGuestHeapSiteCapacity];
static unsigned guest_heap_site_count;
static struct guest_heap_stats guest_heap_statistics;
static pthread_mutex_t guest_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static bool guest_heap_reuse = true;
static bool guest_heap_poison;
static bool guest_heap_trace;
static bool timing_trace;
static uint64_t guest_heap_report_swap_interval = 600;
static uint32_t guest_errno_address;
static _Thread_local uint32_t guest_thread_errno_address;
static _Thread_local char *guest_strtok_state;
static uint32_t guest_pthread_next_key;
static _Thread_local uint32_t guest_pthread_values[128];
/*
 * Multiprocessing Services semaphores.  Handles are (generation << 16 | slot)
 * so a deleted semaphore's slot can be reused while any stale handle, or a
 * waiter that has not yet observed the deletion, is still rejected.  The
 * former implementation handed out 255 handles once and never reused them.
 */
enum {
    kGuestSemaphoreCapacity = 4096,
    kGuestSemaphoreSlotMask = 0xffff,
    kGuestSemaphoreGenerationShift = 16,
};

struct guest_semaphore {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int32_t count;
    int32_t maximum;
    uint32_t generation;
    bool initialized;
    bool constructed;
};

static struct guest_semaphore guest_semaphores[kGuestSemaphoreCapacity];
static uint32_t guest_semaphore_high_water = 1;
static uint32_t guest_semaphore_live_count;
static pthread_mutex_t guest_semaphore_table_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t guest_semaphore_handle(uint32_t slot, uint32_t generation)
{
    return ((generation & kGuestSemaphoreSlotMask) <<
            kGuestSemaphoreGenerationShift) | slot;
}

/* Returns the semaphore with its mutex held, or NULL for a stale handle. */
static struct guest_semaphore *guest_semaphore_acquire(uint32_t handle)
{
    uint32_t slot = handle & kGuestSemaphoreSlotMask;
    uint32_t generation = handle >> kGuestSemaphoreGenerationShift;
    if (!slot || slot >= kGuestSemaphoreCapacity) return NULL;
    struct guest_semaphore *semaphore = &guest_semaphores[slot];
    if (!semaphore->constructed) return NULL;
    pthread_mutex_lock(&semaphore->mutex);
    if (!semaphore->initialized ||
        (semaphore->generation & kGuestSemaphoreSlotMask) != generation) {
        pthread_mutex_unlock(&semaphore->mutex);
        return NULL;
    }
    return semaphore;
}

static uint32_t guest_semaphore_create(int32_t maximum, int32_t initial)
{
    pthread_mutex_lock(&guest_semaphore_table_lock);
    uint32_t slot = 0;
    for (uint32_t candidate = 1; candidate < guest_semaphore_high_water;
         ++candidate) {
        if (!guest_semaphores[candidate].initialized) {
            slot = candidate;
            break;
        }
    }
    if (!slot) {
        if (guest_semaphore_high_water >= kGuestSemaphoreCapacity) {
            pthread_mutex_unlock(&guest_semaphore_table_lock);
            fprintf(stderr, "compat32: MP semaphore table exhausted\n");
            return 0;
        }
        slot = guest_semaphore_high_water++;
    }
    struct guest_semaphore *semaphore = &guest_semaphores[slot];
    if (!semaphore->constructed) {
        pthread_mutex_init(&semaphore->mutex, NULL);
        pthread_cond_init(&semaphore->condition, NULL);
        semaphore->constructed = true;
    }
    pthread_mutex_lock(&semaphore->mutex);
    ++semaphore->generation;
    if ((semaphore->generation & kGuestSemaphoreSlotMask) == 0) {
        ++semaphore->generation;
    }
    semaphore->maximum = maximum;
    semaphore->count = initial;
    semaphore->initialized = true;
    uint32_t handle = guest_semaphore_handle(slot, semaphore->generation);
    pthread_mutex_unlock(&semaphore->mutex);
    __atomic_fetch_add(&guest_semaphore_live_count, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&guest_semaphore_table_lock);
    return handle;
}
static _Thread_local uint32_t guest_call_depth;
static _Thread_local uint32_t guest_thread_slot = UINT32_MAX;
static pthread_mutex_t guest_stack_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t guest_stack_slots_in_use;
static pthread_key_t guest_stack_slot_key;
static pthread_once_t guest_stack_slot_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t guest_execution_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool guest_execution_lock_held;

enum {
    kGuestStdinHandle = 0x7f030000,
    kGuestStdoutHandle = 0x7f030010,
    kGuestStderrHandle = 0x7f030020,
    kGuestFileHandleBase = 0x7f030100,
    kGuestFileHandleStride = 16,
    kGuestFileCapacity = 1024,
    kGuestFileGenerationStride = kGuestFileCapacity * kGuestFileHandleStride,
    kGuestFileGenerationCount = 3,
};

struct guest_file_entry {
    uint32_t handle;
    FILE *file;
};

static struct guest_file_entry guest_files[kGuestFileCapacity];
static uint32_t guest_file_next_index;
static uint32_t guest_file_active_count;
static uint32_t guest_file_peak_count;
static uint64_t guest_file_exhaustion_count;
static pthread_mutex_t guest_file_lock = PTHREAD_MUTEX_INITIALIZER;

enum {
    kGuestDirectoryHandleBase = 0x7f070000,
    kGuestDirectoryHandleStride = 16,
    kGuestDirectoryCapacity = 256,
};

/* This title was built against Darwin's pre-ino64 i386 dirent ABI.  Its
   directory iterator reads d_name at byte offset 8 (confirmed at 0xd91cb),
   whereas the current x86_64 SDK puts it at byte offset 21. */
struct __attribute__((packed, aligned(4))) guest_dirent32 {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type;
    uint8_t d_namlen;
    char d_name[256];
};

_Static_assert(offsetof(struct guest_dirent32, d_name) == 8,
               "legacy Darwin i386 dirent layout");

struct __attribute__((packed, aligned(4))) guest_dirent_inode64 {
    uint64_t inode, seek_offset;
    uint16_t record_length, name_length;
    uint8_t type;
    char name[1024];
};
_Static_assert(offsetof(struct guest_dirent_inode64, name) == 21 &&
               sizeof(struct guest_dirent_inode64) == 1048, "i386 inode64 dirent ABI");

struct guest_directory_entry {
    uint32_t handle;
    DIR *directory;
    uint32_t guest_dirent;
};

static struct guest_directory_entry guest_directories[kGuestDirectoryCapacity];
static pthread_mutex_t guest_directory_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Guest pthread mutexes and condition variables are keyed by the address of
 * the guest's own (opaque, never dereferenced) object.  Host objects are
 * allocated individually so the lookup tables can grow without moving a
 * mutex that another thread is currently blocked on, and destroy calls
 * release the entry so a long campaign cannot exhaust the registry.  A host
 * object that is still busy at destroy time (the guest destroyed a locked
 * mutex or a waited-on condition, which is undefined for it as well) is kept
 * rather than freed underneath its waiters.
 */
struct guest_mutex_entry {
    uint32_t address;
    pthread_mutex_t *mutex;
};

struct guest_cond_entry {
    uint32_t address;
    pthread_cond_t *condition;
};

static struct guest_mutex_entry *guest_mutexes;
static struct guest_cond_entry *guest_conditions;
static uint32_t guest_mutex_count;
static uint32_t guest_mutex_capacity;
static uint32_t guest_cond_count;
static uint32_t guest_cond_capacity;
static uint64_t guest_sync_released_count;
static pthread_mutex_t guest_sync_table_lock = PTHREAD_MUTEX_INITIALIZER;
static char dynamic_symbol_names[kDynamicThunkCapacity][256];
static uint32_t dynamic_symbol_count;

/*
 * Cg is still present in the game bundle as an x86_64 slice.  Cg 3.0 does not
 * hand out pointers for CGcontext/CGprogram/CGparameter: every handle is a
 * small integer allocated from a monotonically increasing per-process counter
 * (the bundled runtime returns 0x1 for the first context, 0x9 for the first
 * program, and roughly eighteen values per compiled program).  Those values
 * are passed to the guest unchanged, exactly as the original i386 Cg did.
 *
 * The previous design interned every handle in a fixed 4,096-entry table and
 * silently returned NULL once it filled.  A single session compiles several
 * hundred programs, so the table saturated part way through the campaign and
 * from then on every newly compiled material either lost its program
 * (cgCreateProgram == NULL) or its parameters (cgGetFirstParameter == NULL).
 * The renderer then drew those materials with whichever ARB program and
 * constants happened to be bound last, producing the flat saturated skies,
 * oceans, cards, and stretched props that appeared in whichever level was
 * loaded latest.
 *
 * A growable table is retained purely as a safety net for the theoretical
 * case of a Cg build that returns real pointers above the i386 range.
 */
enum {
    kGuestCgHandleBase = 0x7f040000,
    kGuestCgHandleStride = 16,
};

static void *cg_library;
static void **cg_objects;
static uint32_t cg_object_count;
static uint32_t cg_object_capacity;
static bool cg_object_table_reported;
static pthread_mutex_t cg_object_lock = PTHREAD_MUTEX_INITIALIZER;

/* Guest copies of strings owned by the Cg runtime.  Cg interns parameter
   names, so identical host pointers are returned for the life of the process;
   the guest copy is keyed by that pointer and refreshed only when the content
   changes (for example a recycled compiled-program buffer).  This keeps the
   guest heap bounded instead of allocating a new copy on every call. */
struct cg_string_entry {
    const char *host;
    uint32_t owner;
    uint32_t guest;
    uint32_t capacity;
};

static struct cg_string_entry *cg_strings;
static uint32_t cg_string_count;
static uint32_t cg_string_capacity;
static pthread_mutex_t cg_string_lock = PTHREAD_MUTEX_INITIALIZER;

/* Session counters reported at exit.  A non-zero null count for programs or
   first parameters is exactly the symptom that used to produce flat
   saturated materials, so it is the number to check after a long session. */
static uint32_t cg_programs_created;
static uint32_t cg_programs_destroyed;
static uint32_t cg_null_programs;
static uint32_t cg_null_first_parameters;

static void cg_report(const char *reason)
{
    if (!cg_library) return;
    fprintf(stderr,
            "compat32: Cg %s: programs created=%" PRIu32 " destroyed=%" PRIu32
            " null-programs=%" PRIu32 " null-first-parameters=%" PRIu32
            " pointer-handles=%" PRIu32 " guest-strings=%" PRIu32 "\n",
            reason, __atomic_load_n(&cg_programs_created, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_programs_destroyed, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_null_programs, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_null_first_parameters, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_object_count, __ATOMIC_RELAXED),
            __atomic_load_n(&cg_string_count, __ATOMIC_RELAXED));
}

/* HIToolbox; declared here to avoid pulling Carbon headers in. */
extern OSStatus SetSystemUIMode(UInt32 mode, UInt32 options);

struct guest_thread_context {
    uint32_t token;
    uint32_t function;
    uint32_t argument;
};

static _Thread_local uint32_t current_thread_token;
/* pthread_t is a pointer even in i386. Public pthread_cleanup_push/pop
   macros access its cleanup-stack word at offset four. */
static uint32_t allocate_thread_token(void) {
    uint32_t token = compat_runtime32_allocate(4096, 1);
    if (token) *(uint32_t *)(uintptr_t)token = 0x54485244;
    return token;
}
struct thread_token { uint32_t token; pthread_t host; bool finished, detached; struct thread_token *next; };
static struct thread_token *thread_tokens;
static pthread_mutex_t thread_tokens_lock=PTHREAD_MUTEX_INITIALIZER;
static void remember_thread(struct thread_token *record, uint32_t token, pthread_t host) {
    record->token=token;record->host=host;record->finished=false;record->detached=false;
    pthread_mutex_lock(&thread_tokens_lock);
    record->next=thread_tokens;thread_tokens=record;
    pthread_mutex_unlock(&thread_tokens_lock);
}
static uint32_t guest_pthread_self(void) {
    if (!current_thread_token) {
        struct thread_token *record=malloc(sizeof(*record));
        if(!record)return 0;
        current_thread_token=allocate_thread_token();
        if(!current_thread_token){free(record);return 0;}
        remember_thread(record,current_thread_token,pthread_self());
    }
    return current_thread_token;
}
static void forget_thread(uint32_t token) {
    pthread_mutex_lock(&thread_tokens_lock);
    for (struct thread_token **p = &thread_tokens; *p; p = &(*p)->next) {
        if ((*p)->token != token) continue;
        struct thread_token *record = *p; *p = record->next; free(record); compat_runtime32_deallocate(token); break;
    }
    pthread_mutex_unlock(&thread_tokens_lock);
}
static void thread_finished(uint32_t token, bool detached) {
    pthread_mutex_lock(&thread_tokens_lock);
    for (struct thread_token **p = &thread_tokens; *p; p = &(*p)->next) {
        struct thread_token *record = *p;
        if (record->token != token) continue;
        if (detached) record->detached = true; else record->finished = true;
        if (record->finished && record->detached) { *p = record->next; free(record); compat_runtime32_deallocate(token); }
        break;
    }
    pthread_mutex_unlock(&thread_tokens_lock);
}
static pthread_t host_thread(uint32_t token) {
    if(token && token==current_thread_token)return pthread_self();
    pthread_t host=NULL;
    pthread_mutex_lock(&thread_tokens_lock);
    for(struct thread_token *r=thread_tokens;r;r=r->next)if(r->token==token){host=r->host;break;}
    pthread_mutex_unlock(&thread_tokens_lock);
    return host;
}

static uint32_t guest_allocate(size_t size, bool clear);
static uint32_t guest_allocate_at(size_t size, bool clear, uint32_t site);
static bool guest_deallocate(uint32_t pointer);

/* guest_file_lock must be held while the returned FILE is in use.  This is
   important even for reads: another guest thread may close the same opaque
   handle concurrently, and libc FILE objects are invalid immediately after
   fclose. */
static FILE *host_file_for_guest_locked(uint32_t handle)
{
    if (handle == kGuestStdinHandle) return stdin;
    if (handle == kGuestStdoutHandle) return stdout;
    if (handle == kGuestStderrHandle) return stderr;
    if (handle < kGuestFileHandleBase) return NULL;
    uint32_t offset = handle - kGuestFileHandleBase;
    uint32_t slot_offset = offset % kGuestFileGenerationStride;
    if (slot_offset % kGuestFileHandleStride) return NULL;
    uint32_t index = slot_offset / kGuestFileHandleStride;
    if (index >= kGuestFileCapacity) return NULL;
    struct guest_file_entry *entry = &guest_files[index];
    return entry->file && entry->handle == handle ? entry->file : NULL;
}

static FILE *lock_host_file_for_guest(uint32_t handle)
{
    pthread_mutex_lock(&guest_file_lock);
    FILE *file = host_file_for_guest_locked(handle);
    if (!file) {
        pthread_mutex_unlock(&guest_file_lock);
        errno = EBADF;
    }
    return file;
}

static void unlock_host_file(void)
{
    pthread_mutex_unlock(&guest_file_lock);
}

static uint32_t guest_handle_for_file(FILE *file)
{
    if (!file) return 0;
    pthread_mutex_lock(&guest_file_lock);
    for (uint32_t offset = 0; offset < kGuestFileCapacity; ++offset) {
        uint32_t index = (guest_file_next_index + offset) % kGuestFileCapacity;
        struct guest_file_entry *entry = &guest_files[index];
        if (entry->file) continue;

        /* Keep every opaque FILE value 16-byte aligned, as the original table
           did, while rotating through three non-overlapping generations below
           the Cg handle range.  Round-robin slot selection therefore rejects
           a recently closed stale handle for at least 3,072 subsequent opens. */
        uint32_t generation = 0;
        if (entry->handle) {
            generation = ((entry->handle - kGuestFileHandleBase) /
                          kGuestFileGenerationStride + 1) %
                         kGuestFileGenerationCount;
        }
        uint32_t handle = kGuestFileHandleBase +
                          generation * kGuestFileGenerationStride +
                          index * kGuestFileHandleStride;
        entry->handle = handle;
        entry->file = file;
        guest_file_next_index = (index + 1) % kGuestFileCapacity;
        ++guest_file_active_count;
        if (guest_file_active_count > guest_file_peak_count) {
            guest_file_peak_count = guest_file_active_count;
        }
        pthread_mutex_unlock(&guest_file_lock);
        return handle;
    }

    ++guest_file_exhaustion_count;
    uint64_t exhaustion = guest_file_exhaustion_count;
    uint32_t active = guest_file_active_count;
    pthread_mutex_unlock(&guest_file_lock);

    /* Ownership transfers to this function even on exhaustion.  The former
       implementation leaked this FILE and its descriptor whenever its
       monotonic 1,024-entry table filled. */
    fclose(file);
    errno = EMFILE;
    if (exhaustion == 1 || getenv("LP32_TRACE_FILES")) {
        fprintf(stderr,
                "compat32: guest FILE table exhausted active=%" PRIu32
                " capacity=%u occurrence=%" PRIu64 "\n",
                active, kGuestFileCapacity, exhaustion);
    }
    return 0;
}

static int guest_close_file(uint32_t handle)
{
    pthread_mutex_lock(&guest_file_lock);
    FILE *file = host_file_for_guest_locked(handle);
    if (!file) {
        pthread_mutex_unlock(&guest_file_lock);
        errno = EBADF;
        return EOF;
    }

    if (handle != kGuestStdinHandle && handle != kGuestStdoutHandle &&
        handle != kGuestStderrHandle) {
        uint32_t index = ((handle - kGuestFileHandleBase) %
                          kGuestFileGenerationStride) /
                         kGuestFileHandleStride;
        guest_files[index].file = NULL;
        if (guest_file_active_count) --guest_file_active_count;
    }
    int result = fclose(file);
    pthread_mutex_unlock(&guest_file_lock);
    return result;
}

static uint32_t guest_handle_for_directory(DIR *directory)
{
    if (!directory) return 0;
    pthread_mutex_lock(&guest_directory_lock);
    for (uint32_t index = 0; index < kGuestDirectoryCapacity; ++index) {
        struct guest_directory_entry *entry = &guest_directories[index];
        if (entry->directory) continue;
        if (!entry->guest_dirent) {
            entry->guest_dirent = guest_allocate(sizeof(struct guest_dirent_inode64), true);
            if (!entry->guest_dirent) break;
        }
        entry->handle = kGuestDirectoryHandleBase +
                        index * kGuestDirectoryHandleStride;
        entry->directory = directory;
        uint32_t handle = entry->handle;
        pthread_mutex_unlock(&guest_directory_lock);
        return handle;
    }
    pthread_mutex_unlock(&guest_directory_lock);
    closedir(directory);
    errno = EMFILE;
    return 0;
}

static struct guest_directory_entry *guest_directory_for_handle(uint32_t handle)
{
    if (handle < kGuestDirectoryHandleBase) return NULL;
    uint32_t offset = handle - kGuestDirectoryHandleBase;
    if (offset % kGuestDirectoryHandleStride) return NULL;
    uint32_t index = offset / kGuestDirectoryHandleStride;
    if (index >= kGuestDirectoryCapacity) return NULL;
    struct guest_directory_entry *entry = &guest_directories[index];
    return entry->directory && entry->handle == handle ? entry : NULL;
}

/* Constant doubles exported as data. MW3 reads these through its imports;
   AppKit reports the 10.9 release the game was linked against. */
static bool guest_double_import(const char *name, double *value)
{
    if (!strcmp(name, "_kCFAbsoluteTimeIntervalSince1970")) *value = kCFAbsoluteTimeIntervalSince1970;
    else if (!strcmp(name, "_NSAppKitVersionNumber")) *value = 1265.0;
    else return false;
    return true;
}

static uint32_t guest_standard_file_import(const char *name)
{
    if (strcmp(name, "___stdinp") == 0) return kGuestStdinHandle;
    if (strcmp(name, "___stdoutp") == 0) return kGuestStdoutHandle;
    if (strcmp(name, "___stderrp") == 0) return kGuestStderrHandle;
    return 0;
}

/* Released host objects are recycled rather than freed so that a guest
   thread racing a destroy with a lock (undefined for the guest as well) can
   never dereference unmapped host memory. */
struct host_sync_free_node {
    struct host_sync_free_node *next;
};

static struct host_sync_free_node *free_host_mutexes;
static struct host_sync_free_node *free_host_conditions;
static void recycle_host_mutex(pthread_mutex_t *mutex);

static int initialize_host_mutex(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    int status = pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return status;
}

/*
 * Lock-free guest mutex lookup.  Every guest lock, trylock and unlock must
 * find the host mutex for a guest address; doing that under
 * guest_sync_table_lock serialized all guest threads (MW2's renderer makes
 * millions of lock calls per second).  Host mutexes live in an arena that
 * never moves or shrinks, so a 32-bit arena index names one, and each map
 * slot packs (guest address << 32 | index + 1) into a single atomic word.
 * Readers probe without locking; inserts and removals happen under
 * guest_sync_table_lock, and removals leave a tombstone so probes continue.
 * A miss simply falls back to the locked table, which stays authoritative.
 */
enum {
    kHostMutexArenaCapacity = 1u << 20,
    kGuestMutexMapBits = 20,
    kGuestMutexMapSize = 1u << kGuestMutexMapBits,
};
static uint8_t *host_mutex_arena;
static uint32_t host_mutex_arena_used;
static _Atomic uint64_t guest_mutex_map[kGuestMutexMapSize];
static const uint64_t kGuestMutexMapTombstone = UINT64_C(0xffffffff) << 32;

static uint32_t guest_mutex_map_home(uint32_t address)
{
    return (uint32_t)(((address >> 2) * UINT32_C(2654435761)) >> (32 - kGuestMutexMapBits));
}

static pthread_mutex_t *guest_mutex_map_find(uint32_t address)
{
    if (!address || address == UINT32_MAX || !host_mutex_arena) return NULL;
    uint32_t slot = guest_mutex_map_home(address);
    for (uint32_t probe = 0; probe < kGuestMutexMapSize; ++probe) {
        uint64_t entry = atomic_load_explicit(&guest_mutex_map[(slot + probe) & (kGuestMutexMapSize - 1)],
                                              memory_order_acquire);
        if (!entry) return NULL;
        if ((uint32_t)(entry >> 32) == address)
            return (pthread_mutex_t *)(void *)(host_mutex_arena +
                (size_t)((uint32_t)entry - 1) * sizeof(pthread_mutex_t));
    }
    return NULL;
}

/* guest_sync_table_lock held. */
static void guest_mutex_map_insert(uint32_t address, pthread_mutex_t *mutex)
{
    if (!address || address == UINT32_MAX) return;
    uint32_t index = (uint32_t)(((uint8_t *)mutex - host_mutex_arena) / sizeof(pthread_mutex_t));
    uint32_t slot = guest_mutex_map_home(address);
    for (uint32_t probe = 0; probe < kGuestMutexMapSize / 2; ++probe) {
        _Atomic uint64_t *cell = &guest_mutex_map[(slot + probe) & (kGuestMutexMapSize - 1)];
        uint64_t entry = atomic_load_explicit(cell, memory_order_relaxed);
        if (entry && entry != kGuestMutexMapTombstone && (uint32_t)(entry >> 32) != address) continue;
        atomic_store_explicit(cell, (uint64_t)address << 32 | (index + 1), memory_order_release);
        return;
    }
}

/* guest_sync_table_lock held. */
static void guest_mutex_map_remove(uint32_t address)
{
    if (!address || address == UINT32_MAX) return;
    uint32_t slot = guest_mutex_map_home(address);
    for (uint32_t probe = 0; probe < kGuestMutexMapSize; ++probe) {
        _Atomic uint64_t *cell = &guest_mutex_map[(slot + probe) & (kGuestMutexMapSize - 1)];
        uint64_t entry = atomic_load_explicit(cell, memory_order_relaxed);
        if (!entry) return;
        if ((uint32_t)(entry >> 32) == address) {
            atomic_store_explicit(cell, kGuestMutexMapTombstone, memory_order_release);
            return;
        }
    }
}

static pthread_mutex_t *create_host_mutex(void)
{
    pthread_mutex_t *mutex;
    if (free_host_mutexes) {
        struct host_sync_free_node *node = free_host_mutexes;
        free_host_mutexes = node->next;
        mutex = (pthread_mutex_t *)node;
    } else {
        if (!host_mutex_arena) {
            void *arena = mmap(NULL, (size_t)kHostMutexArenaCapacity * sizeof(pthread_mutex_t),
                               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (arena == MAP_FAILED) return NULL;
            host_mutex_arena = arena;
        }
        if (host_mutex_arena_used == kHostMutexArenaCapacity) return NULL;
        mutex = (pthread_mutex_t *)(void *)(host_mutex_arena +
            (size_t)host_mutex_arena_used++ * sizeof(pthread_mutex_t));
    }
    if (initialize_host_mutex(mutex) != 0) {
        recycle_host_mutex(mutex);
        return NULL;
    }
    return mutex;
}

static void recycle_host_mutex(pthread_mutex_t *mutex)
{
    struct host_sync_free_node *node = (struct host_sync_free_node *)mutex;
    node->next = free_host_mutexes;
    free_host_mutexes = node;
}

static pthread_cond_t *create_host_condition(void)
{
    pthread_cond_t *condition;
    if (free_host_conditions) {
        struct host_sync_free_node *node = free_host_conditions;
        free_host_conditions = node->next;
        condition = (pthread_cond_t *)node;
    } else {
        condition = malloc(sizeof(*condition) >
                           sizeof(struct host_sync_free_node) ?
                           sizeof(*condition) :
                           sizeof(struct host_sync_free_node));
        if (!condition) return NULL;
    }
    if (pthread_cond_init(condition, NULL) != 0) {
        free(condition);
        return NULL;
    }
    return condition;
}

static void recycle_host_condition(pthread_cond_t *condition)
{
    struct host_sync_free_node *node = (struct host_sync_free_node *)condition;
    node->next = free_host_conditions;
    free_host_conditions = node;
}

/* Direct-mapped guess at a guest mutex's table index.  A miss (stale after a
   destroy compacted the table) just falls back to the scan, so entries are
   never invalidated. */
enum { kGuestMutexIndexCacheSize = 1024 };
static uint32_t guest_mutex_index_cache[kGuestMutexIndexCacheSize];

static pthread_mutex_t *host_mutex_for_guest(uint32_t address, bool create)
{
    pthread_mutex_t *mapped = guest_mutex_map_find(address);
    if (mapped) return mapped;
    uint32_t *cached = &guest_mutex_index_cache[
        (address >> 3) % kGuestMutexIndexCacheSize];
    pthread_mutex_lock(&guest_sync_table_lock);
    if (*cached < guest_mutex_count && guest_mutexes[*cached].address == address) {
        pthread_mutex_t *mutex = guest_mutexes[*cached].mutex;
        pthread_mutex_unlock(&guest_sync_table_lock);
        return mutex;
    }
    for (uint32_t index = 0; index < guest_mutex_count; ++index) {
        if (guest_mutexes[index].address == address) {
            pthread_mutex_t *mutex = guest_mutexes[index].mutex;
            *cached = index;
            pthread_mutex_unlock(&guest_sync_table_lock);
            return mutex;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    if (guest_mutex_count == guest_mutex_capacity) {
        uint32_t grown = guest_mutex_capacity ? guest_mutex_capacity * 2 : 1024;
        struct guest_mutex_entry *table =
            realloc(guest_mutexes, (size_t)grown * sizeof(*table));
        if (!table) {
            pthread_mutex_unlock(&guest_sync_table_lock);
            return NULL;
        }
        guest_mutexes = table;
        guest_mutex_capacity = grown;
    }
    pthread_mutex_t *mutex = create_host_mutex();
    if (!mutex) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    *cached = guest_mutex_count;
    struct guest_mutex_entry *entry = &guest_mutexes[guest_mutex_count++];
    entry->address = address;
    entry->mutex = mutex;
    guest_mutex_map_insert(address, mutex);
    pthread_mutex_unlock(&guest_sync_table_lock);
    return mutex;
}

/* pthread_mutex_init on an address that already has a host mutex means the
   guest recycled that memory (or re-initialized a static).  Replace the host
   object so no stale lock state leaks into the new mutex; if the old one is
   still held it is unrecoverable for the guest too, so keep it instead. */
static uint32_t guest_mutex_initialize(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_mutex_count; ++index) {
        struct guest_mutex_entry *entry = &guest_mutexes[index];
        if (entry->address != address) continue;
        if (pthread_mutex_destroy(entry->mutex) == 0) {
            initialize_host_mutex(entry->mutex);
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return 0;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    return host_mutex_for_guest(address, true) ? 0 : (uint32_t)ENOMEM;
}

static uint32_t guest_mutex_destroy(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_mutex_count; ++index) {
        struct guest_mutex_entry *entry = &guest_mutexes[index];
        if (entry->address != address) continue;
        int status = pthread_mutex_destroy(entry->mutex);
        if (status == 0) {
            guest_mutex_map_remove(address);
            recycle_host_mutex(entry->mutex);
            *entry = guest_mutexes[--guest_mutex_count];
            ++guest_sync_released_count;
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return (uint32_t)status;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    /* Never created on the host: statically initialized but never locked. */
    return 0;
}

static pthread_cond_t *host_cond_for_guest(uint32_t address, bool create)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_cond_count; ++index) {
        if (guest_conditions[index].address == address) {
            pthread_cond_t *condition = guest_conditions[index].condition;
            pthread_mutex_unlock(&guest_sync_table_lock);
            return condition;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    if (guest_cond_count == guest_cond_capacity) {
        uint32_t grown = guest_cond_capacity ? guest_cond_capacity * 2 : 1024;
        struct guest_cond_entry *table =
            realloc(guest_conditions, (size_t)grown * sizeof(*table));
        if (!table) {
            pthread_mutex_unlock(&guest_sync_table_lock);
            return NULL;
        }
        guest_conditions = table;
        guest_cond_capacity = grown;
    }
    pthread_cond_t *condition = create_host_condition();
    if (!condition) {
        pthread_mutex_unlock(&guest_sync_table_lock);
        return NULL;
    }
    struct guest_cond_entry *entry = &guest_conditions[guest_cond_count++];
    entry->address = address;
    entry->condition = condition;
    pthread_mutex_unlock(&guest_sync_table_lock);
    return condition;
}

static uint32_t guest_cond_initialize(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_cond_count; ++index) {
        struct guest_cond_entry *entry = &guest_conditions[index];
        if (entry->address != address) continue;
        pthread_cond_broadcast(entry->condition);
        if (pthread_cond_destroy(entry->condition) == 0) {
            pthread_cond_init(entry->condition, NULL);
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return 0;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    return host_cond_for_guest(address, true) ? 0 : (uint32_t)ENOMEM;
}

/* macOS reports success from pthread_cond_destroy even while a thread is
   blocked in pthread_cond_wait on the object, so a busy check cannot protect
   waiters.  Wake them first: a spurious wakeup is permitted by POSIX and the
   guest re-tests its predicate, whereas a waiter left on a recycled object
   would sleep forever. */
static uint32_t guest_cond_destroy(uint32_t address)
{
    pthread_mutex_lock(&guest_sync_table_lock);
    for (uint32_t index = 0; index < guest_cond_count; ++index) {
        struct guest_cond_entry *entry = &guest_conditions[index];
        if (entry->address != address) continue;
        pthread_cond_broadcast(entry->condition);
        int status = pthread_cond_destroy(entry->condition);
        if (status == 0) {
            recycle_host_condition(entry->condition);
            *entry = guest_conditions[--guest_cond_count];
            ++guest_sync_released_count;
        }
        pthread_mutex_unlock(&guest_sync_table_lock);
        return (uint32_t)status;
    }
    pthread_mutex_unlock(&guest_sync_table_lock);
    return 0;
}

static int runtime_error(const char *what)
{
    fprintf(stderr, "compat32: %s: %s\n", what, strerror(errno));
    return -1;
}

static bool process_is_translated(void)
{
    int translated = 0;
    size_t size = sizeof(translated);
    return sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) == 0 &&
           translated == 1;
}

static int allocate_cs32(void)
{
    ldt_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.code.limit00 = 0xffff;
    entry.code.type = DESC_CODE_READ;
    entry.code.dpl = 3;
    entry.code.present = 1;
    entry.code.limit16 = 0x0f;
    entry.code.opsz = DESC_CODE_32B;
    entry.code.granular = DESC_GRAN_PAGE;

    const int index = 32;
    if (i386_set_ldt(index, &entry, 1) < 0) return runtime_error("i386_set_ldt(cs32)");
    lp32_cs32 = (uint16_t)((index << 3) | 7);
    return 0;
}

static void emit_u32(uint8_t *destination, uint32_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static void emit_u64(uint8_t *destination, uint64_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static uint8_t *emit_bytes(uint8_t *destination, const uint8_t *bytes, size_t size)
{
    memcpy(destination, bytes, size);
    return destination + size;
}

/*
 * Mode guards.
 *
 * Every transition between i386 and x86_64 code is a far jump through a
 * selector.  Under Rosetta a thread that is in the middle of such a jump
 * while the translator flushes its code cache can resume on the far side in
 * its *old* mode: the 64-bit gateway pad is then decoded as i386 (the
 * observed crash: `movabs $gateway,%rax` becomes `dec eax; mov eax,imm32;
 * add [eax],eax`, a write into the game's text), and an i386 thunk can
 * likewise be decoded as x86_64.  A standalone stress test reproduces both
 * within seconds when another thread keeps Rosetta translating fresh code.
 *
 * Each pad therefore starts with a probe whose bytes are valid in both
 * modes but compute a different value: 0x40 is `inc eax` in i386 and a REX
 * prefix in x86_64, and 0x41 is `inc ecx` versus REX.B.  When the probe
 * shows the wrong mode the pad counts the event and redoes the far jump to
 * an alternate pad; the alternate pad traps if that fails too, so a stuck
 * thread never loops.  Registers used by the probes are scratch at every
 * site: eax at an import call, ecx at a return.
 */
static bool mode_guards_disabled;

/* i386 tail: lock incl counter; ljmp $cs64:retry (or int3 when retry==0). */
static uint8_t *emit_recovery32(uint8_t *destination, uint32_t counter,
                                uint32_t retry, uint16_t cs64)
{
    static const uint8_t lock_inc[] = {0xf0, 0xff, 0x05};
    uint8_t *p = emit_bytes(destination, lock_inc, sizeof(lock_inc));
    emit_u32(p, counter);
    p += 4;
    if (!retry) {
        *p++ = 0xcc;
        return p;
    }
    *p++ = 0xea;
    emit_u32(p, retry);
    p += 4;
    memcpy(p, &cs64, sizeof(cs64));
    return p + 2;
}

/* 64-bit pad reached from import thunks: jump to lp32_import_gateway. */
static void emit_gateway64_pad(uint8_t *destination, uint32_t retry,
                               uint16_t cs64)
{
    uint8_t *p = destination;
    if (!mode_guards_disabled) {
        /* xorl %eax,%eax; inc-eax-or-REX nop; testl %eax,%eax; jnz recover */
        static const uint8_t probe[] = {0x31, 0xc0, 0x40, 0x90,
                                        0x85, 0xc0, 0x75, 0x0c};
        p = emit_bytes(p, probe, sizeof(probe));
    }
    /* movabs $lp32_import_gateway,%rax; jmp *%rax */
    p[0] = 0x48;
    p[1] = 0xb8;
    emit_u64(p + 2, (uint64_t)(uintptr_t)lp32_import_gateway);
    p[10] = 0xff;
    p[11] = 0xe0;
    p += 12;
    if (!mode_guards_disabled) {
        emit_recovery32(p, retry ? kWrongModeTo64Counter : kModeGuardFailedCounter,
                        retry, cs64);
    }
}

/* 64-bit pad reached from the exit thunk: restore the host ABI frame
   established by run_compat32 and return to it with the guest's edx:eax
   packed into rax (i386 returns 8-byte values, NSPoint among them, in the
   pair). */
static void emit_return64_pad(uint8_t *destination, uint32_t retry,
                              uint16_t cs64)
{
    static const uint8_t return64[] = {
        0x4c, 0x87, 0xf4,
        0x48, 0x83, 0xc4, 0x18,
        0x41, 0x5f,
        0x41, 0x5e,
        0x41, 0x5d,
        0x41, 0x5c,
        0x5b,
        0x5d,
        0x48, 0xc1, 0xe2, 0x20,     /* shlq $32, %rdx */
        0x89, 0xc0,                 /* movl %eax, %eax */
        0x48, 0x09, 0xd0,           /* orq %rdx, %rax */
        0xc3,
    };
    uint8_t *p = destination;
    if (!mode_guards_disabled) {
        /* xorl %ecx,%ecx; inc-ecx-twice-or-inc-r9d; testl %ecx,%ecx; jnz */
        static const uint8_t probe[] = {0x31, 0xc9, 0x41, 0xff, 0xc1,
                                        0x85, 0xc9, 0x75, 0x1b};
        _Static_assert(sizeof(return64) == 0x1b, "probe skips the return sequence");
        p = emit_bytes(p, probe, sizeof(probe));
    }
    p = emit_bytes(p, return64, sizeof(return64));
    if (!mode_guards_disabled) {
        emit_recovery32(p, retry ? kWrongModeTo64Counter : kModeGuardFailedCounter,
                        retry, cs64);
    }
}

/* i386 landing trampoline: the 64-bit side leaves the i386 destination on
   top of the guest stack and far-jumps here; `ret` pops it.  Decoded as
   x86_64 (wrong mode) the probe falls into the recovery tail instead, which
   counts the event, redirects the far pointer still addressed by r14 at the
   alternate trampoline and redoes the jump. */
static void emit_landing32(uint8_t *destination, uint32_t retry)
{
    static const uint8_t probe[] = {0x31, 0xc9, 0x41, 0xff, 0xc1,
                                    0x85, 0xc9, 0x74, 0x01, 0xc3};
    uint8_t *p = emit_bytes(destination, probe, sizeof(probe));
    static const uint8_t lock_inc_abs[] = {0xf0, 0xff, 0x04, 0x25};
    p = emit_bytes(p, lock_inc_abs, sizeof(lock_inc_abs));
    emit_u32(p, retry ? kWrongModeTo32Counter : kModeGuardFailedCounter);
    p += 4;
    if (!retry) {
        *p = 0xcc;
        return;
    }
    static const uint8_t store_r14[] = {0x41, 0xc7, 0x06};
    p = emit_bytes(p, store_r14, sizeof(store_r14));
    emit_u32(p, retry);
    p += 4;
    static const uint8_t ljmp_r14[] = {0x41, 0xff, 0x2e};
    emit_bytes(p, ljmp_r14, sizeof(ljmp_r14));
}

void compat_runtime32_mode_guard_counts(uint32_t *recovered_to64,
                                        uint32_t *recovered_to32)
{
    *recovered_to64 = *(volatile uint32_t *)(uintptr_t)kWrongModeTo64Counter;
    *recovered_to32 = *(volatile uint32_t *)(uintptr_t)kWrongModeTo32Counter;
}

static void (*diagnostic_sink)(const char *line);

void compat_runtime32_set_diagnostic_sink(void (*sink)(const char *line))
{
    diagnostic_sink = sink;
}

static int game_config_missing;

int compat_runtime32_game_config_missing(void)
{
    return game_config_missing;
}

static bool path_is_game_config(const char *path)
{
    static const char suffix[] = "/pcconfig.txt";
    size_t length = strlen(path);
    return length >= sizeof(suffix) - 1 &&
           strcasecmp(path + length - (sizeof(suffix) - 1), suffix) == 0;
}

/* Called once per presented frame: report newly recovered wrong-mode
   landings so the persistent run log shows how often Rosetta did this. */
void compat_runtime32_check_mode_guards(uint64_t swap_count)
{
    static uint32_t reported_to64, reported_to32;
    uint32_t to64, to32;
    compat_runtime32_mode_guard_counts(&to64, &to32);
    if (to64 == reported_to64 && to32 == reported_to32) return;
    char line[160];
    snprintf(line, sizeof(line),
             "compat32: recovered wrong-mode landing(s) at swap %llu: "
             "to64=%u (+%u) to32=%u (+%u)\n",
             (unsigned long long)swap_count, to64, to64 - reported_to64,
             to32, to32 - reported_to32);
    reported_to64 = to64;
    reported_to32 = to32;
    if (diagnostic_sink) {
        diagnostic_sink(line);
    } else {
        fputs(line, stderr);
    }
}

/*
 * Test aid (LP32_TEST_CODE_CHURN=<mib>): keep generating and executing fresh
 * x86_64 code so Rosetta has to translate continuously and flush its cache
 * every few seconds.  This is what makes wrong-mode landings frequent enough
 * to observe; without guards the game dies within seconds under it.
 */
static void *code_churn_thread(void *opaque)
{
    size_t size = (size_t)(uintptr_t)opaque << 20;
    for (;;) {
        uint8_t *block = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
        if (block == MAP_FAILED) return NULL;
        for (size_t index = 0; index < size / 16; ++index) {
            uint8_t *function = block + index * 16;
            function[0] = 0xb8;                 /* movl $index,%eax */
            emit_u32(function + 1, (uint32_t)index);
            function[5] = 0xc3;                 /* ret */
            memset(function + 6, 0xcc, 10);
        }
        if (mprotect(block, size, PROT_READ | PROT_EXEC) != 0) return NULL;
        volatile uint32_t sink = 0;
        for (size_t index = 0; index < size / 16; ++index) {
            sink += ((uint32_t (*)(void))(block + index * 16))();
        }
        munmap(block, size);
    }
}

static void start_code_churn_if_requested(void)
{
    const char *text = getenv("LP32_TEST_CODE_CHURN");
    if (!text || !text[0]) return;
    unsigned mib = (unsigned)strtoul(text, NULL, 0);
    if (!mib) mib = 8;
    pthread_t thread;
    if (pthread_create(&thread, NULL, code_churn_thread,
                       (void *)(uintptr_t)mib) == 0) {
        pthread_detach(thread);
        fprintf(stderr, "compat32: test code churn active (%u MiB cycles)\n", mib);
    }
}

/* Darwin's inline ctype helpers read this exported object directly. The
 * 64-bit host has wider function pointers ahead of the cached tables, so
 * copying its native layout (or a placeholder data cell) is not sufficient. */
struct guest_rune_locale32 {
    char magic[8];
    char encoding[32];
    uint32_t sgetrune, sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256], mapupper[256];
    struct { int32_t count; uint32_t ranges; } extensions[3];
    uint32_t variable;
    int32_t variable_length, class_count;
    uint32_t classes;
};
enum { kGuestRuneLocaleOffset = 0x6000 };
_Static_assert(offsetof(struct guest_rune_locale32, runetype) == 0x34,
               "Darwin i386 inline ctype table offset");
_Static_assert(kGuestRuneLocaleOffset + sizeof(struct guest_rune_locale32) <=
               kModeGuardCountersOffset, "rune locale precedes counters and proxies");

static void initialize_guest_rune_locale(void)
{
    struct guest_rune_locale32 *locale =
        (void *)(uintptr_t)(kBridgeDataBase + kGuestRuneLocaleOffset);
    memcpy(locale->magic, _DefaultRuneLocale.__magic, sizeof(locale->magic));
    memcpy(locale->encoding, _DefaultRuneLocale.__encoding, sizeof(locale->encoding));
    locale->invalid_rune = _DefaultRuneLocale.__invalid_rune;
    memcpy(locale->runetype, _DefaultRuneLocale.__runetype, sizeof(locale->runetype));
    memcpy(locale->maplower, _DefaultRuneLocale.__maplower, sizeof(locale->maplower));
    memcpy(locale->mapupper, _DefaultRuneLocale.__mapupper, sizeof(locale->mapupper));
}

_Static_assert(kGuestRuneLocaleOffset + sizeof(struct guest_rune_locale32) <=
               kGuestLowercaseOffset, "lowercase table overlaps rune locale");
_Static_assert(kGuestLowercaseOffset + 256 * sizeof(uint32_t) <=
               kModeGuardCountersOffset, "lowercase table overlaps counters");
static void update_guest_lowercase_table(void)
{
    uint32_t *table = (void *)(uintptr_t)(kBridgeDataBase + kGuestLowercaseOffset);
    for (unsigned i = 0; i < 256; ++i) table[i] = (uint32_t)tolower((unsigned char)i);
}

static uint32_t pure_import_symbol(const char *name)
{
    const char *full = getenv("LP32_HITCH_FULL_IMPORTS");
    if (getenv("LP32_NO_PURE_IMPORTS") || getenv("LP32_NO_FAST_IMPORTS") || getenv("LP32_GUEST_LOCK") ||
        getenv("LP32_FRAME_STATS") || getenv("LP32_TRACE_ATOMIC") ||
        (full && full[0] && strcmp(full, "0"))) return 0;
    const uint8_t *entry = NULL;
    if (!strcmp(name, "_OSAtomicAdd32") || !strcmp(name, "_OSAtomicAdd32Barrier"))
        entry = lp32_context_atomic_add32;
    else if (!strcmp(name, "_OSAtomicCompareAndSwap32") ||
             !strcmp(name, "_OSAtomicCompareAndSwapInt") ||
             !strcmp(name, "_OSAtomicCompareAndSwap32Barrier") ||
             !strcmp(name, "_OSAtomicCompareAndSwapPtrBarrier"))
        entry = lp32_context_atomic_cas32;
    return entry ? kBridgeCodeBase + kGuestContextOffset +
                   (uint32_t)(entry - lp32_context_start) : 0;
}

static int build_transition_bridge(void)
{
    uint8_t *code = (void *)(uintptr_t)kBridgeCodeBase;
    memset(code, 0xcc, kBridgeCodeSize);
    memset((void *)(uintptr_t)kOverflowImportBase, 0xcc, kOverflowImportSize);
    memset((void *)(uintptr_t)kExtendedImportBase, 0xcc, kExtendedImportSize);
    mode_guards_disabled = getenv("LP32_NO_MODE_GUARDS") != NULL;
    if (mode_guards_disabled) {
        fprintf(stderr, "compat32: mode guards disabled by LP32_NO_MODE_GUARDS\n");
    }
    start_code_churn_if_requested();

    uint16_t cs64 = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs64));

    /* i386 exit thunk: ljmp *exit_far_pointer */
    code[kExitThunkOffset + 0] = 0xff;
    code[kExitThunkOffset + 1] = 0x2d;
    emit_u32(code + kExitThunkOffset + 2,
             kBridgeCodeBase + kExitFarPointerOffset);

    const struct far_ptr32 exit_far = {
        .offset = kBridgeCodeBase + kReturn64Offset,
        .selector = cs64,
    };
    memcpy(code + kExitFarPointerOffset, &exit_far, sizeof(exit_far));

    /* Each wrong-mode landing retries through the next pad in its chain
       (primary, alternate, then kModeRetryPads more); the last one traps.
       MW2's render workers make enough import calls during translation
       churn to miss twice in a row, which a single alternate turned into a
       crash. */
    emit_return64_pad(code + kReturn64Offset,
                      kBridgeCodeBase + kReturn64AltOffset, cs64);
    emit_return64_pad(code + kReturn64AltOffset,
                      kBridgeCodeBase + kHost64RetryOffset, cs64);
    for (uint32_t pad = 0; pad < kModeRetryPads; ++pad) {
        uint32_t next = pad + 1 < kModeRetryPads ?
            kBridgeCodeBase + kHost64RetryOffset + (pad + 1) * 0x80 : 0;
        emit_return64_pad(code + kHost64RetryOffset + pad * 0x80, next, cs64);
    }

    const struct far_ptr32 gateway_far = {
        .offset = kBridgeCodeBase + kGateway64Offset,
        .selector = cs64,
    };
    memcpy(code + kGatewayFarPointerOffset, &gateway_far, sizeof(gateway_far));

    emit_gateway64_pad(code + kGateway64Offset,
                       kBridgeCodeBase + kGateway64AltOffset, cs64);
    emit_gateway64_pad(code + kGateway64AltOffset,
                       kBridgeCodeBase + kHost64RetryOffset + 0x40, cs64);
    for (uint32_t pad = 0; pad < kModeRetryPads; ++pad) {
        uint32_t next = pad + 1 < kModeRetryPads ?
            kBridgeCodeBase + kHost64RetryOffset + (pad + 1) * 0x80 + 0x40 : 0;
        emit_gateway64_pad(code + kHost64RetryOffset + pad * 0x80 + 0x40, next, cs64);
    }

    emit_landing32(code + kLanding32Offset, kBridgeCodeBase + kLanding32AltOffset);
    emit_landing32(code + kLanding32AltOffset, kBridgeCodeBase + kLanding32RetryOffset);
    for (uint32_t pad = 0; pad < kModeRetryPads; ++pad) {
        uint32_t next = pad + 1 < kModeRetryPads ?
            kBridgeCodeBase + kLanding32RetryOffset + (pad + 1) * 0x40 : 0;
        emit_landing32(code + kLanding32RetryOffset + pad * 0x40, next);
    }

    for (uint32_t index = 0; index < current_image->import_count; ++index) {
        const struct macho_import32 *import = &current_image->imports[index];
        if (import->kind == MACHO_IMPORT32_POINTER && !import->target) continue;
        uint8_t *thunk = index < kInlineImportCapacity ?
            code + kImportThunksOffset + index * kImportThunkSize :
            index < kInlineImportCapacity + kOverflowImportCapacity ?
            (uint8_t *)(uintptr_t)kOverflowImportBase +
                (index - kInlineImportCapacity) * kImportThunkSize :
            (uint8_t *)(uintptr_t)kExtendedImportBase +
                (index - kInlineImportCapacity - kOverflowImportCapacity) * kImportThunkSize;
        uint32_t context = pure_import_symbol(import->name);
        if (!context) context = guest_context_symbol(import->name);
        if (context) {
            thunk = (void *)(uintptr_t)context;
        } else if (import->target) {
            thunk = (void *)(uintptr_t)import->target;
        } else {
            /* push $import_index; ljmp *gateway_far_pointer */
            thunk[0] = 0x68;
            emit_u32(thunk + 1, index);
            thunk[5] = 0xff;
            thunk[6] = 0x2d;
            emit_u32(thunk + 7, kBridgeCodeBase + kGatewayFarPointerOffset);
        }

        if (import->kind != MACHO_IMPORT32_STUB) {
            uint32_t *slot = (void *)(uintptr_t)current_image->imports[index].address;
            *slot = (uint32_t)(uintptr_t)thunk;
            continue;
        }
        uint8_t *stub = (void *)(uintptr_t)current_image->imports[index].address;
        int64_t displacement = (int64_t)(uintptr_t)thunk -
                               ((int64_t)(uintptr_t)stub + 5);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            errno = ERANGE;
            return runtime_error("i386 import thunk displacement");
        }
        uintptr_t page = (uintptr_t)stub & ~(uintptr_t)0xfff;
        size_t span = (((uintptr_t)stub + 5 + 0xfff) & ~(uintptr_t)0xfff) - page;
        if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            return runtime_error("mprotect i386 import stub");
        }
        stub[0] = 0xe9;
        emit_u32(stub + 1, (uint32_t)(int32_t)displacement);
    }

    for (uint32_t index = 0; index < kDynamicThunkCapacity; ++index) {
        uint8_t *thunk = code + kDynamicThunksOffset + index * kImportThunkSize;
        thunk[0] = 0x68;
        emit_u32(thunk + 1, UINT32_C(0x80000000) | index);
        thunk[5] = 0xff;
        thunk[6] = 0x2d;
        emit_u32(thunk + 7, kBridgeCodeBase + kGatewayFarPointerOffset);
    }

    uint32_t *data_cell = (void *)(uintptr_t)kBridgeDataBase;
    size_t data_cells = 0;
    memset(data_cell, 0, kBridgeDataSize);
    initialize_guest_rune_locale();
    update_guest_lowercase_table();
    for (uint32_t index = 0; index < current_image->import_count; ++index) {
        if (current_image->imports[index].target) continue;
        if (current_image->imports[index].kind != MACHO_IMPORT32_POINTER) continue;
        if ((data_cells + 4) * sizeof(*data_cell) > kGuestRuneLocaleOffset) {
            errno = ENOSPC;
            return runtime_error("i386 import data cells");
        }
        uint32_t cell_address = kBridgeDataBase +
                                (uint32_t)(data_cells * sizeof(*data_cell));
        uint32_t *slot = (void *)(uintptr_t)current_image->imports[index].address;
        *slot = cell_address;
        if (strcmp(current_image->imports[index].name, "__DefaultRuneLocale") == 0) {
            *slot = kBridgeDataBase + kGuestRuneLocaleOffset;
            continue;
        }
        if (strcmp(current_image->imports[index].name, "__CurrentRuneLocale") == 0) {
            data_cell[data_cells] = kBridgeDataBase + kGuestRuneLocaleOffset;
        }
        if (strcmp(current_image->imports[index].name, "_errno") == 0) {
            guest_errno_address = cell_address;
        }
        if (strcmp(current_image->imports[index].name, "_mach_task_self_") == 0) {
            data_cell[data_cells] = mach_task_self();
        }
        if (strcmp(current_image->imports[index].name, "_vm_page_size") == 0)
            data_cell[data_cells] = (uint32_t)vm_page_size;
        double constant;
        if (guest_double_import(current_image->imports[index].name, &constant))
            memcpy(&data_cell[data_cells], &constant, sizeof(constant));
        uint32_t objc_pointer =
            objc_bridge32_pointer_import(current_image->imports[index].name);
        if (objc_pointer) data_cell[data_cells] = objc_pointer;
        uint32_t standard_file =
            guest_standard_file_import(current_image->imports[index].name);
        if (standard_file) data_cell[data_cells] = standard_file;
        data_cells += 4;
    }

    size_t context_size = (uintptr_t)lp32_context_end - (uintptr_t)lp32_context_start;
    if (context_size > kBridgeCodeSize - kGuestContextOffset)
        return runtime_error("guest context page overflow");
    memcpy(code + kGuestContextOffset, lp32_context_start, context_size);
    emit_u32(code + kGuestContextOffset + (lp32_context_lower_table - lp32_context_start),
             kBridgeDataBase + kGuestLowercaseOffset);
    uint32_t helper = compat_runtime32_guest_callback("_lp32_context_signal_mask");
    emit_u32(code + kGuestContextOffset + (lp32_context_save_helper - lp32_context_start), helper);
    emit_u32(code + kGuestContextOffset + (lp32_context_restore_helper - lp32_context_start), helper);
    if (mprotect((void *)(uintptr_t)kOverflowImportBase, kOverflowImportSize,
                 PROT_READ | PROT_EXEC) != 0) return runtime_error("mprotect overflow imports");
    if (mprotect((void *)(uintptr_t)kExtendedImportBase, kExtendedImportSize,
                 PROT_READ | PROT_EXEC) != 0) return runtime_error("mprotect extended imports");
    if (mprotect(code, kBridgeCodeSize, PROT_READ | PROT_EXEC) != 0) {
        return runtime_error("mprotect transition bridge");
    }
    return 0;
}

static uint32_t guest_context_symbol(const char *name)
{
    if (!lp32_title_is_cod_carbon(lp32_profile()->title) &&
        !lp32_title_is_mw_sdl(lp32_profile()->title) &&
        !lp32_title_is_gameloft(lp32_profile()->title)) return 0;
    const uint8_t *entry = NULL;
    if (!strcmp(name, "_setjmp")) entry = lp32_context_setjmp;
    else if (!strcmp(name, "__setjmp")) entry = lp32_context_fast_setjmp;
    else if (!strcmp(name, "_sigsetjmp")) entry = lp32_context_sigsetjmp;
    else if (!strcmp(name, "_longjmp") || !strcmp(name, "_siglongjmp")) entry = lp32_context_longjmp;
    else if (!strcmp(name, "__longjmp")) entry = lp32_context_fast_longjmp;
    else if (lp32_title_is_mw_sdl(lp32_profile()->title) &&
             !getenv("LP32_NO_GUEST_LIBC")) {
        if (!strcmp(name, "_strcmp")) entry = lp32_context_strcmp;
        else if (!strcmp(name, "___tolower")) entry = lp32_context_tolower;
    }
    return entry ? kBridgeCodeBase + kGuestContextOffset + (uint32_t)(entry - lp32_context_start) : 0;
}

static uint32_t guest_thunk_for_dynamic_symbol(const char *name)
{
    uint32_t context = pure_import_symbol(name);
    if (!context) context = guest_context_symbol(name);
    if (context) return context;
    for (uint32_t index = 0; index < dynamic_symbol_count; ++index) {
        if (strcmp(dynamic_symbol_names[index], name) == 0) {
            return kBridgeCodeBase + kDynamicThunksOffset + index * kImportThunkSize;
        }
    }
    if (dynamic_symbol_count >= kDynamicThunkCapacity || strlen(name) >= sizeof(dynamic_symbol_names[0])) return 0;
    uint32_t index = dynamic_symbol_count++;
    strcpy(dynamic_symbol_names[index], name);
    return kBridgeCodeBase + kDynamicThunksOffset + index * kImportThunkSize;
}

uint32_t compat_runtime32_guest_callback(const char *name)
{
    return guest_thunk_for_dynamic_symbol(name);
}

uint32_t compat_runtime32_resolve_symbol(const char *name, int data_hint)
{
    (void)data_hint;
    if (!strcmp(name, "__DefaultRuneLocale")) {
        /* Darwin's cached rune arrays are identical; the two obsolete function
           pointers before them are 32-bit in the guest, so offsets differ. */
        static uint32_t locale;
        if (!locale) {
            locale = compat_runtime32_allocate(3164, 1);
            if (!locale) return 0;
            uint8_t *bytes = (void *)(uintptr_t)locale;
            memcpy(bytes, &_DefaultRuneLocale, 40);
            memcpy(bytes + 52, _DefaultRuneLocale.__runetype, 1024);
            memcpy(bytes + 1076, _DefaultRuneLocale.__maplower, 1024);
            memcpy(bytes + 2100, _DefaultRuneLocale.__mapupper, 1024);
        }
        return locale;
    }
    uint32_t value = guest_standard_file_import(name);
    if (!value) value = objc_bridge32_pointer_import(name);
    double constant;
    bool is_double = guest_double_import(name, &constant);
    bool data = value || is_double || !strcmp(name, "_errno") ||
        !strcmp(name, "_mach_task_self_") || !strcmp(name, "_vm_page_size") || !strcmp(name, "_environ") ||
        !strcmp(name, "___stack_chk_guard") || !strcmp(name, "___mb_cur_max") || !strcmp(name, "___CFConstantStringClassReference") ||
        !strcmp(name, "_kCFAllocatorDefault") || !strcmp(name, "_kIOMasterPortDefault") ||
        !strncmp(name, "__ZTV", 5) || !strncmp(name, "__ZTI", 5) ||
        !strcmp(name, "__ZNSs4_Rep20_S_empty_rep_storageE") ||
        !strncmp(name, ".objc_class_name_", 17);
    if (!data) return guest_thunk_for_dynamic_symbol(name);
    struct data_symbol { char *name; uint32_t address; };
    static struct data_symbol cells[256];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (!strcmp(cells[i].name, name)) return cells[i].address;
    if (count == sizeof(cells) / sizeof(cells[0])) return 0;
    uint32_t address = compat_runtime32_allocate(128, 1);
    char *copy = strdup(name);
    if (!address || !copy) { free(copy); return 0; }
    cells[count++] = (struct data_symbol){copy, address};
    if (!strcmp(name, "_mach_task_self_")) value = mach_task_self();
    if (!strcmp(name, "_vm_page_size")) value = (uint32_t)vm_page_size;
    if (!strcmp(name, "___stack_chk_guard")) value = UINT32_C(0x6b71329a);
    if (!strcmp(name, "___mb_cur_max")) value = (uint32_t)MB_CUR_MAX;
    *(uint32_t *)(uintptr_t)address = value;
    if (is_double) memcpy((void *)(uintptr_t)address, &constant, sizeof(constant));
    if (!strcmp(name, "_errno")) guest_errno_address = address;
    return address;
}

static bool ensure_cg_library(void)
{
    if (cg_library) return true;
    char executable_path[PATH_MAX];
    uint32_t executable_path_size = sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &executable_path_size) != 0) {
        fprintf(stderr, "compat32: executable path is too long for Cg lookup\n");
        return false;
    }
    char *last_slash = strrchr(executable_path, '/');
    if (!last_slash) return false;
    *last_slash = '\0';
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path),
                          "%s/../Library/Frameworks/Cg.framework/Cg",
                          executable_path);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "compat32: bundled Cg path is too long\n");
        return false;
    }
    cg_library = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!cg_library) {
        fprintf(stderr, "compat32: unable to load Cg framework: %s\n", dlerror());
        return false;
    }
    fprintf(stderr, "compat32: using bundled x86_64 Cg framework\n");
    return true;
}

static void *cg_symbol(const char *name)
{
    if (!ensure_cg_library()) return NULL;
    void *result = dlsym(cg_library, name);
    if (!result) fprintf(stderr, "compat32: Cg symbol %s is unavailable\n", name);
    return result;
}

static uint32_t guest_handle_for_cg_object(void *object)
{
    if (!object) return 0;
    uintptr_t value = (uintptr_t)object;
    if (value < kGuestCgHandleBase) return (uint32_t)value;

    pthread_mutex_lock(&cg_object_lock);
    if (!cg_object_table_reported) {
        fprintf(stderr,
                "compat32: Cg returned a pointer-sized handle %p; using the "
                "translation table for it\n", object);
        cg_object_table_reported = true;
    }
    for (uint32_t index = 0; index < cg_object_count; ++index) {
        if (cg_objects[index] == object) {
            pthread_mutex_unlock(&cg_object_lock);
            return kGuestCgHandleBase + index * kGuestCgHandleStride;
        }
    }
    if (cg_object_count == cg_object_capacity) {
        uint32_t grown = cg_object_capacity ? cg_object_capacity * 2 : 1024;
        void **table = realloc(cg_objects, (size_t)grown * sizeof(*table));
        if (!table) {
            pthread_mutex_unlock(&cg_object_lock);
            fprintf(stderr, "compat32: Cg handle table growth failed\n");
            return 0;
        }
        cg_objects = table;
        cg_object_capacity = grown;
    }
    uint32_t index = cg_object_count++;
    cg_objects[index] = object;
    pthread_mutex_unlock(&cg_object_lock);
    return kGuestCgHandleBase + index * kGuestCgHandleStride;
}

static void *cg_object_for_guest(uint32_t handle)
{
    if (!handle) return NULL;
    if (handle < kGuestCgHandleBase) return (void *)(uintptr_t)handle;
    uint32_t offset = handle - kGuestCgHandleBase;
    if (offset % kGuestCgHandleStride) return NULL;
    uint32_t index = offset / kGuestCgHandleStride;
    pthread_mutex_lock(&cg_object_lock);
    void *result = index < cg_object_count ? cg_objects[index] : NULL;
    pthread_mutex_unlock(&cg_object_lock);
    return result;
}

/* `owner` is the guest handle of the Cg program whose lifetime bounds the
   string (compiled listings), or 0 for strings Cg interns for the whole
   process (parameter names, semantics, profile strings). */
static uint32_t guest_cg_string_owned(uint32_t owner, const char *string)
{
    if (!string) return 0;
    size_t size = strlen(string) + 1;
    if (size > UINT32_MAX) return 0;

    pthread_mutex_lock(&cg_string_lock);
    struct cg_string_entry *entry = NULL;
    for (uint32_t index = 0; index < cg_string_count; ++index) {
        if (cg_strings[index].host == string &&
            cg_strings[index].owner == owner) {
            entry = &cg_strings[index];
            break;
        }
    }
    if (entry && entry->guest && entry->capacity >= size &&
        memcmp((const void *)(uintptr_t)entry->guest, string, size) == 0) {
        uint32_t guest = entry->guest;
        pthread_mutex_unlock(&cg_string_lock);
        return guest;
    }
    if (!entry) {
        if (cg_string_count == cg_string_capacity) {
            uint32_t grown = cg_string_capacity ? cg_string_capacity * 2 : 1024;
            struct cg_string_entry *table =
                realloc(cg_strings, (size_t)grown * sizeof(*table));
            if (!table) {
                pthread_mutex_unlock(&cg_string_lock);
                return compat_runtime32_copy_cstring(string);
            }
            cg_strings = table;
            cg_string_capacity = grown;
        }
        entry = &cg_strings[cg_string_count++];
        memset(entry, 0, sizeof(*entry));
        entry->host = string;
        entry->owner = owner;
    }
    if (entry->capacity < size) {
        uint32_t guest = compat_runtime32_reallocate(entry->guest, size);
        if (!guest) {
            pthread_mutex_unlock(&cg_string_lock);
            return 0;
        }
        entry->guest = guest;
        entry->capacity = (uint32_t)size;
    }
    memcpy((void *)(uintptr_t)entry->guest, string, size);
    uint32_t guest = entry->guest;
    pthread_mutex_unlock(&cg_string_lock);
    return guest;
}

static uint32_t guest_cg_string(const char *string)
{
    return guest_cg_string_owned(0, string);
}

/* Drop every guest string that belonged to a destroyed program.  Its host
   buffer is about to be freed and may be handed to the next program. */
static void release_cg_program_strings(uint32_t owner)
{
    if (!owner) return;
    pthread_mutex_lock(&cg_string_lock);
    for (uint32_t index = 0; index < cg_string_count;) {
        struct cg_string_entry *entry = &cg_strings[index];
        if (entry->owner != owner) {
            ++index;
            continue;
        }
        if (entry->guest) compat_runtime32_deallocate(entry->guest);
        *entry = cg_strings[--cg_string_count];
    }
    pthread_mutex_unlock(&cg_string_lock);
}

static uint32_t guest_copy_nullable_cstring(const char *string)
{
    return guest_cg_string(string);
}

uint32_t compat_runtime32_cg_object_count(void)
{
    pthread_mutex_lock(&cg_object_lock);
    uint32_t count = cg_object_count;
    pthread_mutex_unlock(&cg_object_lock);
    return count;
}

uint32_t compat_runtime32_cg_string_count(void)
{
    pthread_mutex_lock(&cg_string_lock);
    uint32_t count = cg_string_count;
    pthread_mutex_unlock(&cg_string_lock);
    return count;
}

static size_t guest_heap_capacity_for_size(size_t size)
{
    if (size == 0) size = 1;
    if (size > UINT32_MAX - 31) return 0;
    return (size + 31) & ~(size_t)15;
}

static unsigned guest_heap_bin_for_capacity(size_t capacity)
{
    if (capacity <= 512) {
        unsigned index = (unsigned)(capacity / 16);
        return index ? index - 1 : 0;
    }
    unsigned index = 32;
    size_t upper_bound = 1024;
    while (capacity > upper_bound && index + 1 < kGuestHeapBinCount) {
        upper_bound <<= 1;
        ++index;
    }
    return index;
}

static struct guest_heap_site_stats *guest_heap_site_locked(uint32_t site,
                                                             bool create)
{
    if (!site) site = kGuestHeapHostSite;
    for (unsigned index = 0; index < guest_heap_site_count; ++index) {
        if (guest_heap_sites[index].site == site) return &guest_heap_sites[index];
    }
    if (!create || guest_heap_site_count >= kGuestHeapSiteCapacity) return NULL;
    struct guest_heap_site_stats *entry =
        &guest_heap_sites[guest_heap_site_count++];
    memset(entry, 0, sizeof(*entry));
    entry->site = site;
    return entry;
}

static void guest_heap_record_allocation_locked(uint32_t site,
                                                 size_t requested,
                                                 size_t capacity)
{
    struct guest_heap_stats *stats = &guest_heap_statistics;
    ++stats->allocation_calls;
    ++stats->live_blocks;
    stats->live_requested += requested;
    stats->live_capacity += capacity;
    if (stats->live_requested > stats->peak_live_requested) {
        stats->peak_live_requested = stats->live_requested;
    }
    if (stats->live_capacity > stats->peak_live_capacity) {
        stats->peak_live_capacity = stats->live_capacity;
    }
    struct guest_heap_site_stats *entry = guest_heap_site_locked(site, true);
    if (entry) {
        ++entry->allocations;
        entry->allocated_bytes += requested;
        entry->live_bytes += requested;
        if (entry->live_bytes > entry->peak_live_bytes) {
            entry->peak_live_bytes = entry->live_bytes;
        }
    }
}

static void guest_heap_record_free_locked(uint32_t site, size_t requested,
                                           size_t capacity)
{
    struct guest_heap_stats *stats = &guest_heap_statistics;
    ++stats->valid_frees;
    if (stats->live_blocks) --stats->live_blocks;
    stats->live_requested = requested <= stats->live_requested ?
        stats->live_requested - requested : 0;
    stats->live_capacity = capacity <= stats->live_capacity ?
        stats->live_capacity - capacity : 0;
    struct guest_heap_site_stats *entry = guest_heap_site_locked(site, false);
    if (entry) {
        ++entry->frees;
        entry->freed_bytes += requested;
        entry->live_bytes = requested <= entry->live_bytes ?
            entry->live_bytes - requested : 0;
    }
}

static bool guest_heap_validate_header_locked(uint32_t pointer,
                                              uint32_t **metadata_out,
                                              bool *already_free)
{
    if (already_free) *already_free = false;
    if (pointer < guest_heap_base + kGuestHeapHeaderSize ||
        (pointer & 15) != 0 || pointer > guest_heap_cursor) {
        return false;
    }
    uintptr_t header_address = pointer - kGuestHeapHeaderSize;
    if (header_address < guest_heap_base ||
        header_address + kGuestHeapHeaderSize > guest_heap_cursor) {
        return false;
    }
    uint32_t *metadata = (void *)header_address;
    if (metadata[1] == kGuestHeapFreeMagic) {
        if (already_free) *already_free = true;
        return false;
    }
    if (metadata[1] != kGuestHeapLiveMagic) return false;
    uint32_t capacity = metadata[2];
    if (capacity < 32 || (capacity & 15) != 0 ||
        (uintptr_t)pointer + capacity > guest_heap_cursor ||
        metadata[0] > capacity) {
        return false;
    }
    if (metadata_out) *metadata_out = metadata;
    return true;
}

static void guest_heap_insert_free_locked(uint32_t header_address)
{
    uint32_t *metadata = (void *)(uintptr_t)header_address;
    unsigned bin = guest_heap_bin_for_capacity(metadata[2]);
    uint32_t *footer = (void *)(uintptr_t)(header_address +
        kGuestHeapHeaderSize + metadata[2] - 8);
    footer[0] = kGuestHeapFreeMagic;
    footer[1] = metadata[2];
    metadata[3] = guest_heap_free_bins[bin];
    guest_heap_free_bins[bin] = header_address;
    guest_heap_statistics.reusable_capacity += metadata[2];
}

static bool guest_heap_remove_free_locked(uint32_t header_address)
{
    const uint32_t *target = (const void *)(uintptr_t)header_address;
    if (target[1] != kGuestHeapFreeMagic || target[2] < 32 ||
        (target[2] & 15) != 0) {
        return false;
    }
    unsigned bin = guest_heap_bin_for_capacity(target[2]);
    uint32_t *link = &guest_heap_free_bins[bin];
    uint64_t maximum_nodes = guest_heap_statistics.valid_frees +
                             guest_heap_statistics.split_blocks + 1;
    for (uint64_t count = 0; *link && count < maximum_nodes; ++count) {
        uint32_t current = *link;
        uint32_t *metadata = (void *)(uintptr_t)current;
        if (current == header_address) {
            *link = metadata[3];
            guest_heap_statistics.reusable_capacity -= metadata[2];
            metadata[3] = 0;
            return true;
        }
        link = &metadata[3];
    }
    return false;
}

static uint32_t guest_heap_coalesce_free_locked(uint32_t header_address)
{
    uint32_t *metadata = (void *)(uintptr_t)header_address;
    uint32_t capacity = metadata[2];
    uintptr_t next_address = (uintptr_t)header_address +
                             kGuestHeapHeaderSize + capacity;
    if (next_address + kGuestHeapHeaderSize <= guest_heap_cursor) {
        uint32_t *next = (void *)next_address;
        uint32_t next_capacity = next[2];
        if (next[1] == kGuestHeapFreeMagic && next_capacity >= 32 &&
            (next_capacity & 15) == 0 &&
            next_address + kGuestHeapHeaderSize + next_capacity <=
                guest_heap_cursor &&
            guest_heap_remove_free_locked((uint32_t)next_address)) {
            capacity += kGuestHeapHeaderSize + next_capacity;
            metadata[2] = capacity;
        }
    }

    if (header_address >= guest_heap_base + kGuestHeapHeaderSize + 32) {
        const uint32_t *previous_footer =
            (const void *)(uintptr_t)(header_address - 8);
        uint32_t previous_capacity = previous_footer[1];
        if (previous_footer[0] == kGuestHeapFreeMagic &&
            previous_capacity >= 32 && (previous_capacity & 15) == 0 &&
            previous_capacity <= header_address - guest_heap_base -
                                 kGuestHeapHeaderSize) {
            uint32_t previous_address = header_address -
                kGuestHeapHeaderSize - previous_capacity;
            uint32_t *previous = (void *)(uintptr_t)previous_address;
            if (previous[1] == kGuestHeapFreeMagic &&
                previous[2] == previous_capacity &&
                guest_heap_remove_free_locked(previous_address)) {
                previous[2] = previous_capacity + kGuestHeapHeaderSize +
                              capacity;
                metadata = previous;
                header_address = previous_address;
                capacity = metadata[2];
            }
        }
    }

    if ((uintptr_t)header_address + kGuestHeapHeaderSize + capacity ==
        guest_heap_cursor) {
        guest_heap_cursor = header_address;
        return 0;
    }
    metadata[0] = 0;
    metadata[1] = kGuestHeapFreeMagic;
    metadata[3] = 0;
    return header_address;
}

static uint32_t guest_heap_take_free_locked(size_t needed_capacity)
{
    unsigned first_bin = guest_heap_bin_for_capacity(needed_capacity);
    for (unsigned bin = first_bin; bin < kGuestHeapBinCount; ++bin) {
        uint32_t *link = &guest_heap_free_bins[bin];
        uint64_t traversed = 0;
        while (*link && traversed++ < guest_heap_statistics.valid_frees +
                                      guest_heap_statistics.split_blocks + 1) {
            uint32_t header_address = *link;
            if (header_address < guest_heap_base ||
                (header_address & 15) != 0 ||
                (uintptr_t)header_address + kGuestHeapHeaderSize >
                    guest_heap_cursor) {
                fprintf(stderr,
                        "compat32: corrupt guest heap free-list address "
                        "0x%08" PRIx32 "\n", header_address);
                ++guest_heap_statistics.invalid_frees;
                *link = 0;
                break;
            }
            uint32_t *metadata = (void *)(uintptr_t)header_address;
            uint32_t next = metadata[3];
            size_t capacity = metadata[2];
            if (metadata[1] != kGuestHeapFreeMagic || capacity < 32 ||
                (capacity & 15) != 0 ||
                (uintptr_t)header_address + kGuestHeapHeaderSize + capacity >
                    guest_heap_cursor) {
                fprintf(stderr,
                        "compat32: corrupt guest heap free block "
                        "0x%08" PRIx32 "\n", header_address);
                ++guest_heap_statistics.invalid_frees;
                *link = next;
                continue;
            }
            if (capacity < needed_capacity) {
                link = &metadata[3];
                continue;
            }
            *link = next;
            guest_heap_statistics.reusable_capacity -= capacity;
            if (capacity >= needed_capacity + kGuestHeapHeaderSize + 32) {
                uint32_t remainder_address = header_address +
                    kGuestHeapHeaderSize + (uint32_t)needed_capacity;
                uint32_t *remainder = (void *)(uintptr_t)remainder_address;
                remainder[0] = 0;
                remainder[1] = kGuestHeapFreeMagic;
                remainder[2] = (uint32_t)(capacity - needed_capacity -
                                           kGuestHeapHeaderSize);
                remainder[3] = 0;
                metadata[2] = (uint32_t)needed_capacity;
                guest_heap_insert_free_locked(remainder_address);
                ++guest_heap_statistics.split_blocks;
            }
            return header_address;
        }
    }
    return 0;
}

static uint32_t guest_allocate_at(size_t size, bool clear, uint32_t site)
{
    if (size == 0) size = 1;
    size_t needed_capacity = guest_heap_capacity_for_size(size);
    if (!needed_capacity) return 0;

    pthread_mutex_lock(&guest_heap_lock);
    uint32_t header_address = guest_heap_reuse ?
        guest_heap_take_free_locked(needed_capacity) : 0;
    bool reused = header_address != 0;
    if (reused) {
        ++guest_heap_statistics.reused_allocations;
    } else {
        uintptr_t next = guest_heap_cursor + kGuestHeapHeaderSize +
                         needed_capacity;
        if (next < guest_heap_cursor || next > kGuestHeapEnd) {
            ++guest_heap_statistics.failed_allocations;
            fprintf(stderr,
                    "compat32: guest heap exhausted request=%zu cursor=0x%08" PRIxPTR
                    " end=0x%08x\n", size, guest_heap_cursor, kGuestHeapEnd);
            pthread_mutex_unlock(&guest_heap_lock);
            return 0;
        }
        header_address = (uint32_t)guest_heap_cursor;
        guest_heap_cursor = next;
        if (guest_heap_cursor > guest_heap_statistics.high_water) {
            guest_heap_statistics.high_water = guest_heap_cursor;
        }
        ++guest_heap_statistics.bump_allocations;
    }
    uint32_t *metadata = (void *)(uintptr_t)header_address;
    metadata[0] = (uint32_t)size;
    metadata[1] = kGuestHeapLiveMagic;
    if (!reused) metadata[2] = (uint32_t)needed_capacity;
    metadata[3] = site ? site : kGuestHeapHostSite;
    size_t capacity = metadata[2];
    guest_heap_record_allocation_locked(metadata[3], size, capacity);
    pthread_mutex_unlock(&guest_heap_lock);

    uint32_t result = header_address + kGuestHeapHeaderSize;
    if (clear) memset((void *)(uintptr_t)result, 0, capacity);
    return result;
}

static uint32_t guest_allocate(size_t size, bool clear)
{
    return guest_allocate_at(size, clear, kGuestHeapHostSite);
}

static bool guest_deallocate(uint32_t pointer)
{
    pthread_mutex_lock(&guest_heap_lock);
    ++guest_heap_statistics.free_calls;
    if (!pointer) {
        ++guest_heap_statistics.null_frees;
        pthread_mutex_unlock(&guest_heap_lock);
        return true;
    }
    uint32_t *metadata = NULL;
    bool already_free = false;
    if (!guest_heap_validate_header_locked(pointer, &metadata, &already_free)) {
        if (already_free) {
            ++guest_heap_statistics.double_frees;
        } else {
            ++guest_heap_statistics.invalid_frees;
        }
        uint64_t failures = guest_heap_statistics.invalid_frees +
                            guest_heap_statistics.double_frees;
        if (failures <= 32) {
            fprintf(stderr, "compat32: ignored %s guest free 0x%08" PRIx32
                    "\n", already_free ? "double" : "invalid", pointer);
        }
        pthread_mutex_unlock(&guest_heap_lock);
        return false;
    }
    uint32_t requested = metadata[0];
    uint32_t capacity = metadata[2];
    uint32_t site = metadata[3];
    guest_heap_record_free_locked(site, requested, capacity);
    metadata[0] = 0;
    metadata[1] = kGuestHeapFreeMagic;
    metadata[3] = 0;
    if (guest_heap_poison) {
        memset((void *)(uintptr_t)pointer, 0xdd, capacity);
    }
    if (guest_heap_reuse) {
        uint32_t free_header = guest_heap_coalesce_free_locked(
            (uint32_t)(uintptr_t)metadata);
        if (free_header) guest_heap_insert_free_locked(free_header);
    } else {
        guest_heap_statistics.abandoned_capacity += capacity;
    }
    pthread_mutex_unlock(&guest_heap_lock);
    return true;
}

static const char *guest_heap_site_label(uint32_t site)
{
    switch (site) {
        case kGuestHeapHostSite: return "host-bridge";
        case kGuestHeapStringSite: return "compat-string";
        case kGuestHeapRenderPoolSite: return "render-pool";
        default: return "guest-call";
    }
}

void compat_runtime32_heap_report(const char *reason)
{
    pthread_mutex_lock(&guest_heap_lock);
    const struct guest_heap_stats *stats = &guest_heap_statistics;
    fprintf(stderr,
            "compat32: guest heap [%s] reuse=%s alloc=%llu bump=%llu "
            "reused=%llu split=%llu free=%llu/%llu live=%llu "
            "requested=%.2fMiB capacity=%.2fMiB peak=%.2f/%.2fMiB "
            "reusable=%.2fMiB abandoned=%.2fMiB cursor=0x%08" PRIxPTR
            " high=0x%08" PRIxPTR " invalid=%llu double=%llu failed=%llu "
            "realloc=%llu(in-place=%llu moved=%llu)\n",
            reason ? reason : "report", guest_heap_reuse ? "on" : "off",
            (unsigned long long)stats->allocation_calls,
            (unsigned long long)stats->bump_allocations,
            (unsigned long long)stats->reused_allocations,
            (unsigned long long)stats->split_blocks,
            (unsigned long long)stats->valid_frees,
            (unsigned long long)stats->free_calls,
            (unsigned long long)stats->live_blocks,
            (double)stats->live_requested / (1024.0 * 1024.0),
            (double)stats->live_capacity / (1024.0 * 1024.0),
            (double)stats->peak_live_requested / (1024.0 * 1024.0),
            (double)stats->peak_live_capacity / (1024.0 * 1024.0),
            (double)stats->reusable_capacity / (1024.0 * 1024.0),
            (double)stats->abandoned_capacity / (1024.0 * 1024.0),
            guest_heap_cursor, stats->high_water,
            (unsigned long long)stats->invalid_frees,
            (unsigned long long)stats->double_frees,
            (unsigned long long)stats->failed_allocations,
            (unsigned long long)stats->realloc_calls,
            (unsigned long long)stats->realloc_in_place,
            (unsigned long long)stats->realloc_moved);

    if (getenv("LP32_HEAP_REPORT_SITES")) {
        bool selected[kGuestHeapSiteCapacity] = {false};
        for (unsigned rank = 0; rank < 12; ++rank) {
            unsigned best = UINT_MAX;
            for (unsigned index = 0; index < guest_heap_site_count; ++index) {
                if (selected[index] || !guest_heap_sites[index].live_bytes) {
                    continue;
                }
                if (best == UINT_MAX || guest_heap_sites[index].live_bytes >
                                        guest_heap_sites[best].live_bytes) {
                    best = index;
                }
            }
            if (best == UINT_MAX) break;
            selected[best] = true;
            const struct guest_heap_site_stats *entry = &guest_heap_sites[best];
            fprintf(stderr,
                    "compat32: guest heap site[%u] 0x%08" PRIx32
                    " %s live=%.2fMiB peak=%.2fMiB alloc/free=%llu/%llu\n",
                    rank + 1, entry->site, guest_heap_site_label(entry->site),
                    (double)entry->live_bytes / (1024.0 * 1024.0),
                    (double)entry->peak_live_bytes / (1024.0 * 1024.0),
                    (unsigned long long)entry->allocations,
                    (unsigned long long)entry->frees);
        }
    }
    pthread_mutex_unlock(&guest_heap_lock);
}

void compat_runtime32_heap_frame(uint64_t swap_count)
{
    if (!guest_heap_trace || !guest_heap_report_swap_interval) return;
    if (swap_count == 1 || swap_count % guest_heap_report_swap_interval == 0) {
        char reason[48];
        snprintf(reason, sizeof(reason), "swap-%llu",
                 (unsigned long long)swap_count);
        compat_runtime32_heap_report(reason);
        cg_report(reason);
    }
}

int lp32_apply_thread_return_state(void)
{
    /* Consumed here rather than cleared at every dispatch entry: only the
       few float-returning imports ever set it, and each thread-local access
       is a tlv_get_addr call on this ABI. */
    int kind = lp32_return_fp_kind;
    if (kind) {
        lp32_return_fp_kind = 0;
        if (kind == 1) {
            __asm__ volatile("flds %0" : : "m"(lp32_fp_result.f));
        } else {
            __asm__ volatile("fldl %0" : : "m"(lp32_fp_result.d));
        }
    }
    return lp32_leave_guest;
}

static uint32_t guest_reallocate(uint32_t old_pointer, size_t new_size,
                                 uint32_t site)
{
    pthread_mutex_lock(&guest_heap_lock);
    ++guest_heap_statistics.realloc_calls;
    pthread_mutex_unlock(&guest_heap_lock);
    if (!old_pointer) return guest_allocate_at(new_size, false, site);
    if (!new_size) {
        guest_deallocate(old_pointer);
        return 0;
    }

    size_t needed_capacity = guest_heap_capacity_for_size(new_size);
    if (!needed_capacity) return 0;
    pthread_mutex_lock(&guest_heap_lock);
    uint32_t *metadata = NULL;
    bool already_free = false;
    if (!guest_heap_validate_header_locked(old_pointer, &metadata,
                                           &already_free)) {
        ++guest_heap_statistics.invalid_frees;
        pthread_mutex_unlock(&guest_heap_lock);
        return 0;
    }
    uint32_t old_size = metadata[0];
    uint32_t old_capacity = metadata[2];
    uint32_t old_site = metadata[3];
    if (needed_capacity <= old_capacity) {
        metadata[0] = (uint32_t)new_size;
        if (new_size >= old_size) {
            guest_heap_statistics.live_requested += new_size - old_size;
        } else {
            guest_heap_statistics.live_requested -= old_size - new_size;
        }
        if (guest_heap_statistics.live_requested >
            guest_heap_statistics.peak_live_requested) {
            guest_heap_statistics.peak_live_requested =
                guest_heap_statistics.live_requested;
        }
        struct guest_heap_site_stats *entry =
            guest_heap_site_locked(old_site, false);
        if (entry) {
            if (new_size >= old_size) entry->live_bytes += new_size - old_size;
            else entry->live_bytes -= old_size - new_size;
            if (entry->live_bytes > entry->peak_live_bytes) {
                entry->peak_live_bytes = entry->live_bytes;
            }
        }
        ++guest_heap_statistics.realloc_in_place;
        pthread_mutex_unlock(&guest_heap_lock);
        return old_pointer;
    }
    uintptr_t old_end = (uintptr_t)old_pointer + old_capacity;
    if (old_end == guest_heap_cursor) {
        uintptr_t new_end = (uintptr_t)old_pointer + needed_capacity;
        if (new_end >= old_end && new_end <= kGuestHeapEnd) {
            guest_heap_cursor = new_end;
            if (guest_heap_cursor > guest_heap_statistics.high_water) {
                guest_heap_statistics.high_water = guest_heap_cursor;
            }
            metadata[0] = (uint32_t)new_size;
            metadata[2] = (uint32_t)needed_capacity;
            guest_heap_statistics.live_requested += new_size - old_size;
            guest_heap_statistics.live_capacity +=
                needed_capacity - old_capacity;
            if (guest_heap_statistics.live_requested >
                guest_heap_statistics.peak_live_requested) {
                guest_heap_statistics.peak_live_requested =
                    guest_heap_statistics.live_requested;
            }
            if (guest_heap_statistics.live_capacity >
                guest_heap_statistics.peak_live_capacity) {
                guest_heap_statistics.peak_live_capacity =
                    guest_heap_statistics.live_capacity;
            }
            struct guest_heap_site_stats *entry =
                guest_heap_site_locked(old_site, false);
            if (entry) {
                entry->live_bytes += new_size - old_size;
                if (entry->live_bytes > entry->peak_live_bytes) {
                    entry->peak_live_bytes = entry->live_bytes;
                }
            }
            ++guest_heap_statistics.realloc_in_place;
            pthread_mutex_unlock(&guest_heap_lock);
            return old_pointer;
        }
    }
    pthread_mutex_unlock(&guest_heap_lock);

    uint32_t result = guest_allocate_at(new_size, false, site);
    if (!result) return 0;
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy((void *)(uintptr_t)result, (const void *)(uintptr_t)old_pointer, copy_size);
    guest_deallocate(old_pointer);
    pthread_mutex_lock(&guest_heap_lock);
    ++guest_heap_statistics.realloc_moved;
    pthread_mutex_unlock(&guest_heap_lock);
    return result;
}

static void replenish_legacy_render_pool_if_empty(void)
{
    const struct lp32_render_pool *pool = lp32_profile()->render_pool;
    if (!pool) return;
    uint32_t *free_count = (void *)(uintptr_t)pool->free_count;
    uint32_t *free_head = (void *)(uintptr_t)pool->free_head;
    if (*free_head) return;

    const uint32_t object_size = pool->object_size;
    const uint32_t object_count = pool->object_count;
    uint32_t block = guest_allocate_at(object_size * object_count, true,
                                       kGuestHeapRenderPoolSite);
    if (!block) return;
    for (uint32_t index = 0; index < object_count; ++index) {
        uint32_t object = block + index * object_size;
        uint32_t next = index + 1 < object_count ? object + object_size : 0;
        *(uint32_t *)(uintptr_t)(object + 0x28) = next;
    }
    *free_head = block;
    *free_count += object_count;
    fprintf(stderr, "compat32: extended legacy renderer pool by %u objects\n",
            object_count);
}

/* GCC 4's 32-bit COW std::string stores one pointer to character data.  The
   three 32-bit _Rep fields immediately preceding it are length, capacity and
   reference count.  Compatibility-created strings normally have independent
   reps; honoring a positive reference count also makes disposal safe if guest
   inline code happens to share one. */
static uint32_t guest_string_make(const char *bytes, size_t length,
                                  uint32_t site)
{
    if (length > UINT32_MAX - 13) return 0;
    uint32_t allocation = guest_allocate_at(12 + length + 1, false,
                                            site ? site : kGuestHeapStringSite);
    if (!allocation) return 0;
    uint32_t *rep = (void *)(uintptr_t)allocation;
    rep[0] = (uint32_t)length;
    rep[1] = (uint32_t)length;
    rep[2] = 0;
    char *data = (void *)(uintptr_t)(allocation + 12);
    if (length) memcpy(data, bytes, length);
    data[length] = '\0';
    return allocation + 12;
}

static const char *guest_string_data(uint32_t object)
{
    if (!object) return "";
    uint32_t data = *(const uint32_t *)(uintptr_t)object;
    return data ? (const char *)(uintptr_t)data : "";
}

static size_t guest_string_length(uint32_t object)
{
    if (!object) return 0;
    uint32_t data = *(const uint32_t *)(uintptr_t)object;
    if (!data) return 0;
    return *(const uint32_t *)(uintptr_t)(data - 12);
}

static void guest_string_dispose_rep(uint32_t rep)
{
    if (!rep || rep < guest_heap_base + kGuestHeapHeaderSize) {
        return; /* Includes libstdc++'s mapped static empty representation. */
    }
    pthread_mutex_lock(&guest_heap_lock);
    uint32_t *metadata = NULL;
    bool already_free = false;
    if (!guest_heap_validate_header_locked(rep, &metadata, &already_free) ||
        metadata[0] < 12) {
        pthread_mutex_unlock(&guest_heap_lock);
        return;
    }
    int32_t *reference_count = (void *)(uintptr_t)(rep + 8);
    if (*reference_count > 0) {
        --*reference_count;
        pthread_mutex_unlock(&guest_heap_lock);
        return;
    }
    pthread_mutex_unlock(&guest_heap_lock);
    guest_deallocate(rep);
}

static void guest_string_dispose_object(uint32_t object)
{
    if (!object) return;
    uint32_t *data_pointer = (void *)(uintptr_t)object;
    uint32_t data = *data_pointer;
    *data_pointer = 0;
    if (data >= 12) guest_string_dispose_rep(data - 12);
}

static uint32_t guest_string_construct(uint32_t object, const char *bytes,
                                       size_t length, uint32_t site)
{
    if (!object) return 0;
    uint32_t data = guest_string_make(bytes ? bytes : "", bytes ? length : 0,
                                      site);
    if (!data) return 0;
    *(uint32_t *)(uintptr_t)object = data;
    return object;
}

static uint32_t guest_string_assign(uint32_t object, const char *bytes,
                                    size_t length, uint32_t site)
{
    if (!object) return 0;
    uint32_t old_data = *(const uint32_t *)(uintptr_t)object;
    uint32_t data = guest_string_make(bytes ? bytes : "", bytes ? length : 0,
                                      site);
    if (!data) return 0;
    *(uint32_t *)(uintptr_t)object = data;
    if (old_data >= 12 && old_data != data) {
        guest_string_dispose_rep(old_data - 12);
    }
    return object;
}

struct __attribute__((packed, aligned(4))) guest_stat32 {
    int32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t st_rdev;
    int32_t st_atime_sec;
    int32_t st_atime_nsec;
    int32_t st_mtime_sec;
    int32_t st_mtime_nsec;
    int32_t st_ctime_sec;
    int32_t st_ctime_nsec;
    int64_t st_size;
    int64_t st_blocks;
    int32_t st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
};

_Static_assert(sizeof(struct guest_stat32) == 96, "Darwin i386 stat layout");

struct __attribute__((packed, aligned(4))) guest_stat_inode64 {
    int32_t device; uint16_t mode, links; uint64_t inode;
    uint32_t uid, gid; int32_t rdevice;
    int32_t access[2], modify[2], change[2], birth[2];
    int64_t size, blocks; int32_t block_size;
    uint32_t flags, generation; int32_t spare; int64_t qspare[2];
};
_Static_assert(sizeof(struct guest_stat_inode64)==108,"i386 inode64 stat size");

/* Darwin's legacy i386 ABI uses four-byte longs and pointers in struct tm.
   The current host structure has eight-byte tm_gmtoff and tm_zone fields. */
struct __attribute__((packed, aligned(4))) guest_tm32 {
    int32_t tm_sec;
    int32_t tm_min;
    int32_t tm_hour;
    int32_t tm_mday;
    int32_t tm_mon;
    int32_t tm_year;
    int32_t tm_wday;
    int32_t tm_yday;
    int32_t tm_isdst;
    int32_t tm_gmtoff;
    uint32_t tm_zone;
};

struct __attribute__((packed, aligned(4))) guest_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
};

_Static_assert(sizeof(struct guest_tm32) == 44,
               "Darwin i386 struct tm layout");
_Static_assert(sizeof(struct guest_timeval32) == 8,
               "Darwin i386 timeval layout");

static uint32_t copy_host_tm_to_guest(const struct tm *host,
                                      struct guest_tm32 *guest)
{
    if (!host || !guest) return 0;
    memset(guest, 0, sizeof(*guest));
    guest->tm_sec = host->tm_sec;
    guest->tm_min = host->tm_min;
    guest->tm_hour = host->tm_hour;
    guest->tm_mday = host->tm_mday;
    guest->tm_mon = host->tm_mon;
    guest->tm_year = host->tm_year;
    guest->tm_wday = host->tm_wday;
    guest->tm_yday = host->tm_yday;
    guest->tm_isdst = host->tm_isdst;
    guest->tm_gmtoff = (int32_t)host->tm_gmtoff;
    if (host->tm_zone) {
        static pthread_mutex_t zone_lock = PTHREAD_MUTEX_INITIALIZER;
        static struct zone_entry {uint32_t text; struct zone_entry *next;} *zones;
        pthread_mutex_lock(&zone_lock);
        struct zone_entry *zone = zones;
        while (zone && strcmp((const char *)(uintptr_t)zone->text, host->tm_zone)) zone = zone->next;
        if (!zone) {
            zone = malloc(sizeof(*zone));
            if (zone) {
                zone->text = compat_runtime32_copy_cstring(host->tm_zone);
                if (zone->text) {zone->next = zones; zones = zone;}
                else {free(zone); zone = NULL;}
            }
        }
        guest->tm_zone = zone ? zone->text : 0;
        pthread_mutex_unlock(&zone_lock);
    }
    return (uint32_t)(uintptr_t)guest;
}

static void append_formatted(char *output, size_t capacity, size_t *length,
                             const char *piece, size_t piece_length)
{
    if (capacity) {
        size_t available = *length < capacity - 1 ? capacity - 1 - *length : 0;
        size_t copy = piece_length < available ? piece_length : available;
        if (copy) memcpy(output + *length, piece, copy);
        output[(*length + copy < capacity) ? *length + copy : capacity - 1] = '\0';
    }
    *length += piece_length;
}

static int guest_vformat(char *output, size_t capacity, const char *format,
                         const uint32_t *words)
{
    size_t length = 0;
    size_t word = 0;
    if (capacity) output[0] = '\0';
    while (*format) {
        if (*format != '%') {
            const char *start = format;
            while (*format && *format != '%') ++format;
            append_formatted(output, capacity, &length, start,
                             (size_t)(format - start));
            continue;
        }
        const char *start = format++;
        if (*format == '%') {
            append_formatted(output, capacity, &length, "%", 1);
            ++format;
            continue;
        }

        int stars[2] = {0, 0};
        unsigned star_count = 0;
        while (*format && !strchr("diouxXfFeEgGaAcspn", *format)) {
            if (*format == '*' && star_count < 2) stars[star_count++] = (int)words[word++];
            ++format;
        }
        if (!*format) break;
        char conversion = *format++;
        size_t spec_length = (size_t)(format - start);
        if (spec_length >= 64) return -1;
        char spec[64];
        memcpy(spec, start, spec_length);
        spec[spec_length] = '\0';

        bool wide_integer = strstr(spec, "ll") || strchr(spec, 'j');
        char piece[4096];
        int count = 0;
#define FORMAT_VALUE(value) do { \
            if (star_count == 2) count = snprintf(piece, sizeof(piece), spec, stars[0], stars[1], (value)); \
            else if (star_count == 1) count = snprintf(piece, sizeof(piece), spec, stars[0], (value)); \
            else count = snprintf(piece, sizeof(piece), spec, (value)); \
        } while (0)
        if (conversion == 's') {
            const char *value = (const char *)(uintptr_t)words[word++];
            FORMAT_VALUE(value ? value : "(null)");
        } else if (conversion == 'c') {
            int value = (int)words[word++];
            FORMAT_VALUE(value);
        } else if (conversion == 'p') {
            void *value = (void *)(uintptr_t)words[word++];
            FORMAT_VALUE(value);
        } else if (strchr("fFeEgGaA", conversion)) {
            double value;
            memcpy(&value, words + word, sizeof(value));
            word += 2;
            FORMAT_VALUE(value);
        } else if (conversion == 'n') {
            uint32_t address = words[word++];
            if (address) *(int32_t *)(uintptr_t)address = (int32_t)length;
            continue;
        } else if (wide_integer) {
            uint64_t bits;
            memcpy(&bits, words + word, sizeof(bits));
            word += 2;
            if (conversion == 'd' || conversion == 'i') {
                FORMAT_VALUE((long long)(int64_t)bits);
            } else {
                FORMAT_VALUE((unsigned long long)bits);
            }
        } else if (conversion == 'd' || conversion == 'i') {
            int32_t value = (int32_t)words[word++];
            FORMAT_VALUE(value);
        } else {
            uint32_t value = words[word++];
            FORMAT_VALUE(value);
        }
#undef FORMAT_VALUE
        if (count < 0) return count;
        size_t piece_length = (size_t)count;
        if (piece_length >= sizeof(piece)) piece_length = sizeof(piece) - 1;
        append_formatted(output, capacity, &length, piece, piece_length);
    }
    return length > INT_MAX ? -1 : (int)length;
}

static int guest_vscan_consumed(const char *input, const char *format,
                                const uint32_t *destinations,
                                size_t *consumed);

static int guest_vscan(const char *input, const char *format,
                       const uint32_t *destinations)
{
    return guest_vscan_consumed(input, format, destinations, NULL);
}

/*
 * fscanf over a guest stream.  The pending input is scanned from a snapshot
 * of the stream and the file position is moved past exactly what the
 * conversions consumed, which is how the title's config/ini readers use it
 * (short whitespace-delimited tokens per call).
 */
static int guest_fscan(FILE *file, const char *format,
                       const uint32_t *destinations)
{
    enum { snapshot_size = 16384 };
    char *snapshot = malloc(snapshot_size + 1);
    if (!snapshot) return EOF;
    long position = ftell(file);
    if (position < 0) {
        free(snapshot);
        return EOF;
    }
    size_t available = fread(snapshot, 1, snapshot_size, file);
    if (available == 0) {
        free(snapshot);
        return EOF;
    }
    snapshot[available] = '\0';
    size_t consumed = 0;
    int assigned = guest_vscan_consumed(snapshot, format, destinations,
                                        &consumed);
    if (consumed > available) consumed = available;
    fseek(file, position + (long)consumed, SEEK_SET);
    free(snapshot);
    if (assigned == 0 && consumed == available) return EOF;
    return assigned;
}

static int guest_vscan_consumed(const char *input, const char *format,
                                const uint32_t *destinations,
                                size_t *consumed)
{
    const char *start = input;
    size_t destination = 0;
    int assigned = 0;
    while (*format) {
        if (isspace((unsigned char)*format)) {
            while (isspace((unsigned char)*format)) ++format;
            while (isspace((unsigned char)*input)) ++input;
            continue;
        }
        if (*format != '%') {
            if (*input != *format) break;
            ++input;
            ++format;
            continue;
        }
        ++format;
        if (*format == '%') {
            if (*input != '%') break;
            ++input;
            ++format;
            continue;
        }

        bool suppress = false;
        if (*format == '*') { suppress = true; ++format; }
        size_t width = 0;
        while (isdigit((unsigned char)*format)) {
            width = width * 10 + (unsigned)(*format++ - '0');
        }
        enum { length_default, length_hh, length_h, length_l, length_ll } length =
            length_default;
        if (*format == 'h') {
            ++format;
            length = *format == 'h' ? (++format, length_hh) : length_h;
        } else if (*format == 'l') {
            ++format;
            length = *format == 'l' ? (++format, length_ll) : length_l;
        }
        char conversion = *format++;
        if (!conversion) break;

        if (conversion != 'c' && conversion != '[' && conversion != 'n') {
            while (isspace((unsigned char)*input)) ++input;
        }
        char *end = NULL;
        uint32_t destination_address = suppress ? 0 : destinations[destination++];
        /* Numeric field widths cap the input item, just as they do for
         * strings. Reading an entire hex blob for %2lx corrupts every byte
         * of the legacy registry's binary preferences on reload. */
        const char *numeric_input = input;
        char numeric_small[128];
        char *numeric_large = NULL;
        if (width && strchr("diouxXfFeEgGaA", conversion)) {
            size_t count = strnlen(input, width);
            char *copy = count < sizeof(numeric_small) ? numeric_small :
                (numeric_large = malloc(count + 1));
            if (!copy) break;
            memcpy(copy, input, count); copy[count] = 0; numeric_input = copy;
        }
        if (strchr("diouxX", conversion)) {
            int base = conversion == 'i' ? 0 :
                       (conversion == 'o' ? 8 :
                        (conversion == 'x' || conversion == 'X' ? 16 : 10));
            bool signed_value = conversion == 'd' || conversion == 'i';
            uint64_t value = signed_value ? (uint64_t)strtoll(numeric_input, &end, base) :
                                            strtoull(numeric_input, &end, base);
            size_t scanned = (size_t)(end - numeric_input);
            free(numeric_large);
            if (!scanned) break;
            if (!suppress && destination_address) {
                void *output = (void *)(uintptr_t)destination_address;
                if (length == length_hh) *(uint8_t *)output = (uint8_t)value;
                else if (length == length_h) *(uint16_t *)output = (uint16_t)value;
                else if (length == length_ll) *(uint64_t *)output = value;
                else *(uint32_t *)output = (uint32_t)value;
                ++assigned;
            }
            input += scanned;
        } else if (strchr("fFeEgGaA", conversion)) {
            double value = strtod(numeric_input, &end);
            size_t scanned = (size_t)(end - numeric_input);
            free(numeric_large);
            if (!scanned) break;
            if (!suppress && destination_address) {
                if (length == length_l || length == length_ll) {
                    *(double *)(uintptr_t)destination_address = value;
                } else {
                    *(float *)(uintptr_t)destination_address = (float)value;
                }
                ++assigned;
            }
            input += scanned;
        } else if (conversion == 's') {
            size_t count = 0;
            size_t maximum = width ? width : SIZE_MAX;
            while (input[count] && !isspace((unsigned char)input[count]) &&
                   count < maximum) ++count;
            if (!count) break;
            if (!suppress && destination_address) {
                char *output = (void *)(uintptr_t)destination_address;
                memcpy(output, input, count);
                output[count] = '\0';
                ++assigned;
            }
            input += count;
        } else if (conversion == 'c') {
            size_t count = width ? width : 1;
            size_t available = strlen(input);
            if (available < count) break;
            if (!suppress && destination_address) {
                memcpy((void *)(uintptr_t)destination_address, input, count);
                ++assigned;
            }
            input += count;
        } else if (conversion == 'n') {
            if (!suppress && destination_address) {
                *(uint32_t *)(uintptr_t)destination_address =
                    (uint32_t)(input - start);
            }
        } else if (conversion == '[') {
            /* Scanset: %[set] / %[^set], ']' first is literal, a-z ranges.
               The engine's config reader opens pcconfig.txt with
               "FileVersion%*[ \n\t]%d"; failing this conversion left the
               version at 0, which the game took as an outdated file, so it
               deleted the config and rewrote defaults on every launch. */
            bool negate = false;
            if (*format == '^') { negate = true; ++format; }
            bool set[256] = {false};
            const char *set_start = format;
            while (*format && (*format != ']' || format == set_start)) {
                unsigned char low = (unsigned char)*format;
                if (format[1] == '-' && format[2] && format[2] != ']') {
                    unsigned char high = (unsigned char)format[2];
                    for (unsigned c = low; c <= high; ++c) set[c] = true;
                    format += 3;
                } else {
                    set[low] = true;
                    ++format;
                }
            }
            if (*format != ']') break;
            ++format;
            size_t count = 0;
            size_t maximum = width ? width : SIZE_MAX;
            while (input[count] && count < maximum &&
                   set[(unsigned char)input[count]] != negate) ++count;
            if (!count) break;
            if (!suppress && destination_address) {
                char *output = (void *)(uintptr_t)destination_address;
                memcpy(output, input, count);
                output[count] = '\0';
                ++assigned;
            }
            input += count;
        } else {
            break;
        }
    }
    if (consumed) *consumed = (size_t)(input - start);
    return assigned;
}

/* Per-control synchronization preserves the Darwin i386 signature word. */
static bool guest_once_thread_exiting(void);

static int guest_once_initialize(void *opaque)
{
    compat_runtime32_call((uint32_t)(uintptr_t)opaque, NULL, 0);
    return last_call_trapped || guest_once_thread_exiting() ? ECANCELED : 0;
}

static int guest_pthread_once(uint32_t control, uint32_t initializer)
{
    if (!initializer) return EINVAL;
    return guest_once_run((struct guest_once_control *)(uintptr_t)control,
                          guest_once_initialize,
                          (void *)(uintptr_t)initializer);
}

/*
 * Import dispatch is a long if-chain over the import name.  The name length
 * is computed once per dispatch and every candidate is rejected by length
 * before any bytes are compared.  import_is also records that the runtime
 * chain matched the name at all: a name that reaches the audio/Objective-C
 * stages without any runtime match is memoized per import id so later calls
 * skip the runtime chain entirely.  A runtime handler that matches and then
 * deliberately falls through keeps the full chain, so behaviour is unchanged.
 */
static _Thread_local size_t dispatch_name_length;
static _Thread_local bool dispatch_name_matched;

#define import_is(name, expected) \
    (LP32_NAME_IS((name), dispatch_name_length, (expected)) ? \
     (dispatch_name_matched = true) : false)

enum {
    kImportStageUnknown = 0,
    kImportStageRuntime = 1,
    kImportStageAudio = 2,
    kImportStageObjC = 3,
};

static uint8_t *import_stage_table;
static uint8_t dynamic_import_stage_table[kDynamicThunkCapacity];

/*
 * Direct-handler memo, one slot per import id.  NULL: not yet resolved;
 * kFastImportNone: resolved, no direct handler, always use the chains;
 * otherwise the handler.  Resolution happens after the first chained
 * dispatch of that id, so the chains still see every import at least once
 * (they also own the side effects of first use, such as dlsym thunk
 * creation).  LP32_NO_FAST_IMPORTS keeps every call on the chains.
 */
#define kFastImportNone ((lp32_fast_import_fn)(uintptr_t)1)
static lp32_fast_import_fn *fast_import_table;
static lp32_fast_import_fn dynamic_fast_import_table[kDynamicThunkCapacity];
static bool fast_imports_disabled;
static uint8_t import_return_kinds[MACHO_IMAGE32_MAX_IMPORTS];
static uint8_t dynamic_return_kinds[kDynamicThunkCapacity];

/* Per-import time accounting for LP32_FRAME_STATS (racy across threads by
   design; it is diagnostic only). */
struct import_profile_entry {
    const char *name;
    uint64_t calls;
    uint64_t ns;
};
static struct import_profile_entry *import_profile_table;
static struct import_profile_entry dynamic_import_profile_table[kDynamicThunkCapacity];

static struct import_profile_entry *import_profile_slot(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        return dynamic_id < kDynamicThunkCapacity ?
            &dynamic_import_profile_table[dynamic_id] : NULL;
    }
    if (import_profile_table && current_image &&
        import_id < current_image->import_count) {
        return &import_profile_table[import_id];
    }
    return NULL;
}

static uint8_t *import_stage_slot(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        return dynamic_id < kDynamicThunkCapacity ?
            &dynamic_import_stage_table[dynamic_id] : NULL;
    }
    if (import_stage_table && current_image &&
        import_id < current_image->import_count) {
        return &import_stage_table[import_id];
    }
    return NULL;
}

/*
 * Rosetta's undocumented compatibility-mode path can mis-decode a low thunk
 * as x86_64 when two threads execute i386 code concurrently.  Serialize only
 * the guest instruction intervals.  The assembly import gateway drops this
 * lock before calling any host API (including blocking pthread operations)
 * and reacquires it immediately before returning to i386 code.
 */
int compat_runtime32_frame_profile_enabled;
static _Thread_local struct compat_runtime32_frame_profile frame_profile;

static inline uint64_t profile_now(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void compat_runtime32_take_frame_profile(
    struct compat_runtime32_frame_profile *out)
{
    *out = frame_profile;
    memset(&frame_profile, 0, sizeof(frame_profile));
}

/* Long uninterrupted guest intervals on worker threads stall the render
   thread at its next import.  With profiling enabled, holds longer than this
   are attributed to the import that ended them. */
static _Thread_local uint64_t guest_hold_start_ns;
static _Thread_local uint64_t guest_last_hold_ns;
static bool guest_execution_lock_disabled;
enum { kLongGuestHoldNs = 4000000 };

void lp32_guest_execution_enter(void)
{
    /* The plain global is tested first: the thread-local costs a
       tlv_get_addr call, and this runs twice per import. */
    if (guest_execution_lock_disabled || guest_execution_lock_held) return;
    if (compat_runtime32_frame_profile_enabled) {
        if (pthread_mutex_trylock(&guest_execution_lock) != 0) {
            uint64_t start = profile_now();
            pthread_mutex_lock(&guest_execution_lock);
            frame_profile.lock_wait_ns += profile_now() - start;
            ++frame_profile.lock_waits;
        }
        guest_hold_start_ns = profile_now();
    } else {
        pthread_mutex_lock(&guest_execution_lock);
    }
    guest_execution_lock_held = true;
}

void lp32_guest_execution_leave(void)
{
    if (guest_execution_lock_disabled || !guest_execution_lock_held) return;
    guest_execution_lock_held = false;
    if (compat_runtime32_frame_profile_enabled && guest_hold_start_ns) {
        guest_last_hold_ns = profile_now() - guest_hold_start_ns;
    }
    pthread_mutex_unlock(&guest_execution_lock);
}

static float guest_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double guest_double(const uint32_t *words)
{
    double value;
    memcpy(&value, words, sizeof(value));
    return value;
}

static uint64_t return_guest_float(float value)
{
    lp32_fp_result.f = value;
    lp32_return_fp_kind = 1;
    return 0;
}

static uint64_t return_guest_double(double value)
{
    lp32_fp_result.d = value;
    lp32_return_fp_kind = 2;
    return 0;
}

uint64_t compat_runtime32_return_double(double value) { return return_guest_double(value); }
uint64_t compat_runtime32_return_float(float value) { return return_guest_float(value); }

/* Set by the guest's pthread_exit, which leaves guest code like a trapped
   import does; run_guest_thread then finishes the host thread normally. */
static _Thread_local bool guest_thread_started, guest_thread_exiting;

static bool guest_once_thread_exiting(void)
{
    return guest_thread_exiting;
}

static _Thread_local uint32_t guest_thread_exit_value;

static void *run_guest_thread(void *opaque)
{
    struct guest_thread_context context = *(struct guest_thread_context *)opaque;
    free(opaque);
    current_thread_token = context.token;
    guest_thread_started = true;
    if (getenv("LP32_TRACE_THREADS")) {
        uint64_t thread_id = 0;
        pthread_threadid_np(NULL, &thread_id);
        uint32_t record = lp32_profile()->thread_argument_is_direct ?
            context.argument : *(uint32_t *)(uintptr_t)context.argument;
        fprintf(stderr,
                "compat32: guest thread tid=%llu entry=0x%08x record=0x%08x "
                "fn=0x%08x name=%.31s\n",
                (unsigned long long)thread_id, context.function, record,
                record ? *(uint32_t *)(uintptr_t)record : 0,
                record ? (const char *)(uintptr_t)(record + 8) : "");
    }
    uint32_t result = compat_runtime32_call(context.function, &context.argument, 1);
    if (guest_thread_exiting) result = guest_thread_exit_value;
    if (getenv("LP32_TRACE_THREADS")) {
        uint64_t thread_id = 0;
        pthread_threadid_np(NULL, &thread_id);
        fprintf(stderr, "compat32: guest thread tid=%llu exited\n",
                (unsigned long long)thread_id);
    }
    if (!lp32_profile()->thread_argument_is_direct) guest_deallocate(context.argument);
    thread_finished(context.token, false);
    return (void *)(uintptr_t)result;
}

static int guest_qsort_compare(uint32_t comparator, const void *left,
                               const void *right)
{
    const uint32_t arguments[2] = {
        (uint32_t)(uintptr_t)left,
        (uint32_t)(uintptr_t)right,
    };
    return (int32_t)compat_runtime32_call(comparator, arguments, 2);
}

static void guest_qsort_swap(uint8_t *left, uint8_t *right, size_t width)
{
    uint8_t buffer[256];
    while (width) {
        size_t chunk = width < sizeof(buffer) ? width : sizeof(buffer);
        memcpy(buffer, left, chunk);
        memcpy(left, right, chunk);
        memcpy(right, buffer, chunk);
        left += chunk;
        right += chunk;
        width -= chunk;
    }
}

/*
 * The host libc qsort hands the comparator pointers into a scratch copy of
 * the array for some sizes.  That copy lives in the 64-bit host heap, and the
 * guest comparator only sees the low 32 bits.  Sort in place instead
 * (heapsort: every comparison addresses the guest's own array).
 */
static void guest_qsort(uint8_t *base, size_t count, size_t width,
                        uint32_t comparator)
{
#define GUEST_QSORT_ELEMENT(index) (base + (index) * width)
    for (size_t start = count / 2; start-- > 0;) {
        size_t root = start;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= count) break;
            if (child + 1 < count &&
                guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(child),
                                    GUEST_QSORT_ELEMENT(child + 1)) < 0) {
                ++child;
            }
            if (guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(root),
                                    GUEST_QSORT_ELEMENT(child)) >= 0) {
                break;
            }
            guest_qsort_swap(GUEST_QSORT_ELEMENT(root),
                             GUEST_QSORT_ELEMENT(child), width);
            root = child;
        }
    }
    for (size_t end = count; end-- > 1;) {
        guest_qsort_swap(GUEST_QSORT_ELEMENT(0), GUEST_QSORT_ELEMENT(end), width);
        size_t root = 0;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= end) break;
            if (child + 1 < end &&
                guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(child),
                                    GUEST_QSORT_ELEMENT(child + 1)) < 0) {
                ++child;
            }
            if (guest_qsort_compare(comparator, GUEST_QSORT_ELEMENT(root),
                                    GUEST_QSORT_ELEMENT(child)) >= 0) {
                break;
            }
            guest_qsort_swap(GUEST_QSORT_ELEMENT(root),
                             GUEST_QSORT_ELEMENT(child), width);
            root = child;
        }
    }
#undef GUEST_QSORT_ELEMENT
}

const struct macho_image32 *compat_runtime32_image(void)
{
    return current_image;
}

int compat_runtime32_initialize(struct macho_image32 *image)
{
    if (!process_is_translated()) {
        errno = ENOTSUP;
        return runtime_error("x86_64 host is not running through Rosetta");
    }
    current_image = image;
    cxx_exception_bridge32_register_image(image->text_start, image->text_end,
                                          image->unwind_info_start, image->unwind_info_end);
    rtti_bridge32_initialize(image);
    /* Keep small titles at the original base; large BSS images need the heap
       above their last mapped page, never overlapping guest globals. */
    guest_heap_base = image->max_address > kGuestHeapMinimum ?
        ((uintptr_t)image->max_address + 0xffff) & ~(uintptr_t)0xffff : kGuestHeapMinimum;
    guest_heap_cursor = guest_heap_base;
    lp32_callee_pops_struct_return = lp32_profile()->callee_pops_struct_return;
    import_stage_table = calloc(image->import_count ? image->import_count : 1,
                                sizeof(*import_stage_table));
    import_profile_table = calloc(image->import_count ? image->import_count : 1,
                                  sizeof(*import_profile_table));
    fast_import_table = calloc(image->import_count ? image->import_count : 1,
                               sizeof(*fast_import_table));
    fast_imports_disabled = getenv("LP32_NO_FAST_IMPORTS") != NULL;
    memset(import_return_kinds, 0, sizeof(import_return_kinds));
    memset(dynamic_return_kinds, 0, sizeof(dynamic_return_kinds));
    const char *reuse_text = getenv("LP32_GUEST_HEAP_REUSE");
    guest_heap_reuse = !(reuse_text && strcmp(reuse_text, "0") == 0);
    guest_heap_poison = getenv("LP32_GUEST_HEAP_POISON") != NULL;
    guest_heap_trace = getenv("LP32_TRACE_GUEST_HEAP") != NULL;
    timing_trace = getenv("LP32_TRACE_TIMING") != NULL;
    compat_runtime32_frame_profile_enabled = getenv("LP32_FRAME_STATS") != NULL;
    /* The global guest lock guarded against Rosetta mis-decoding a page that
       held both 32-bit thunks and 64-bit landing pads.  Those now live on
       separate pages, and the lock cost the render thread up to a third of
       its time waiting behind worker threads, so it is off unless asked
       for.  LP32_NO_GUEST_LOCK is still accepted as the explicit form. */
    guest_execution_lock_disabled = getenv("LP32_GUEST_LOCK") == NULL;
    const char *report_interval_text = getenv("LP32_HEAP_REPORT_SWAPS");
    if (report_interval_text && report_interval_text[0]) {
        guest_heap_report_swap_interval =
            strtoull(report_interval_text, NULL, 0);
    }
    guest_heap_statistics.high_water = guest_heap_cursor;
    if (allocate_cs32() != 0) return -1;
    if (build_transition_bridge() != 0) return -1;
    if (tlv_bridge32_initialize(image) != 0) return runtime_error("initialize guest TLV");
    printf("compat32: cs32=0x%04x imports=%" PRIu32 " bridge=0x%08x\n",
           lp32_cs32, image->import_count, kBridgeCodeBase);
    fprintf(stderr,
            "compat32: guest heap validation enabled, reuse=%s poison=%s\n",
            guest_heap_reuse ? "on" : "off",
            guest_heap_poison ? "on" : "off");
    if (getenv("LP32_TRACE_IMPORT_TABLE")) {
        for (uint32_t index = 0; index < image->import_count; ++index) {
            fprintf(stderr, "compat32: import[%" PRIu32 "] 0x%08" PRIx32
                    " %s\n", index, image->imports[index].address,
                    image->imports[index].name);
        }
    }
    return 0;
}

static uint64_t dispatch_named_import(uint32_t import_id, const char *name,
                                      const uint32_t *arguments,
                                      uint32_t return_address);

static lp32_fast_import_fn *fast_import_slot(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        return dynamic_id < kDynamicThunkCapacity ?
            &dynamic_fast_import_table[dynamic_id] : NULL;
    }
    if (fast_import_table && current_image &&
        import_id < current_image->import_count) {
        return &fast_import_table[import_id];
    }
    return NULL;
}

static lp32_fast_import_fn runtime_fast_import(const char *name);

static const char *import_name_for_id(uint32_t import_id)
{
    if ((import_id & UINT32_C(0x80000000)) != 0) {
        uint32_t dynamic_id = import_id & UINT32_C(0x7fffffff);
        if (dynamic_id < dynamic_symbol_count) {
            return dynamic_symbol_names[dynamic_id];
        }
    } else if (current_image && import_id < current_image->import_count) {
        return current_image->imports[import_id].name;
    }
    return "<invalid import>";
}

enum import_return_kind { kReturnUnknown, kReturnOrdinary, kReturnStruct,
                          kReturnSteamDynamic };
static unsigned import_return_kind(const char *name)
{
    if (strcmp(name, "__ZNKSt3__18ios_base6getlocEv") == 0 ||
        strcmp(name, "__ZNSt3__19to_stringEm") == 0 ||
        strcmp(name, "__ZNSt3__17promiseIvE10get_futureEv") == 0 ||
        strcmp(name, "__ZSt17current_exceptionv") == 0 ||
        strcmp(name, "_CFAbsoluteTimeGetGregorianDate") == 0 ||
        strcmp(name, "_CTFontGetBoundingBox") == 0 ||
        strcmp(name, "_CFUUIDGetUUIDBytes") == 0 ||
        strcmp(name, "_CTFontGetBoundingRectsForGlyphs") == 0 ||
        strcmp(name, "_CTLineGetImageBounds") == 0 ||
        strcmp(name, "_CGDisplayBounds") == 0 ||
        strcmp(name, "_CVDisplayLinkGetNominalOutputVideoRefreshPeriod") == 0 ||
        strcmp(name, "_objc_msgSend_stret") == 0) return kReturnStruct;
    /* Steam's user-ID call selects its ABI from the arguments and interface
       state. Cache only that it needs a dynamic check, never its result. */
    if (strncmp(name, "_lp32_steam_", 12) == 0) return kReturnSteamDynamic;
    return kReturnOrdinary;
}

/* Clang's i386 callers compensate for the callee popping the hidden sret
   pointer. Older GCC ports retain their existing caller-cleanup convention.
   Like the fast dispatcher, memoize immutable properties by import ID: this
   runs after every import, including thousands of ordinary GL calls/frame. */
uint32_t *lp32_adjust_import_stack(uint32_t *stack)
{
    uint32_t id = stack[0], index = id & UINT32_C(0x7fffffff);
    uint8_t *slot = id & UINT32_C(0x80000000) ?
        (index < kDynamicThunkCapacity ? &dynamic_return_kinds[index] : NULL) :
        (index < MACHO_IMAGE32_MAX_IMPORTS ? &import_return_kinds[index] : NULL);
    unsigned kind = slot ? __atomic_load_n(slot, __ATOMIC_RELAXED) : kReturnUnknown;
    if (kind == kReturnUnknown) {
        kind = import_return_kind(import_name_for_id(id));
        if (slot) __atomic_store_n(slot, kind, __ATOMIC_RELAXED);
    }
    if (kind == kReturnStruct || (kind == kReturnSteamDynamic &&
        steam_bridge32_call_uses_sret(import_name_for_id(id), stack + 2))) {
        stack[2] = stack[1];
        return stack + 1;
    }
    return stack;
}

static uint64_t dispatch_import_chained(uint32_t import_id, const char *name,
                                        const uint32_t *arguments,
                                        uint32_t return_address,
                                        lp32_fast_import_fn *fast_slot)
{
    uint64_t result = dispatch_named_import(import_id, name, arguments,
                                            return_address);
    if (fast_slot && !*fast_slot && !fast_imports_disabled) {
        lp32_fast_import_fn handler = runtime_fast_import(name);
        if (!handler) handler = libcpp_bridge32_fast_import(name);
        if (!handler) handler = objc_bridge32_fast_import(name);
        *fast_slot = handler ? handler : kFastImportNone;
    }
    return result;
}

static uint32_t *uncached_import_return_for_test(uint32_t *stack)
{
    const char *name = import_name_for_id(stack[0]);
    unsigned kind = import_return_kind(name);
    if (kind == kReturnStruct || (kind == kReturnSteamDynamic &&
        steam_bridge32_call_uses_sret(name, stack + 2))) {
        stack[2] = stack[1]; return stack + 1;
    }
    return stack;
}

int compat_runtime32_run_import_return_self_test(void)
{
    static const struct { const char *name; unsigned kind; } cases[] = {
        {"__ZNKSt3__18ios_base6getlocEv", kReturnStruct},
        {"_CFAbsoluteTimeGetGregorianDate", kReturnStruct},
        {"_CTFontGetBoundingBox", kReturnStruct},
        {"_CFUUIDGetUUIDBytes", kReturnStruct},
        {"_CTFontGetBoundingRectsForGlyphs", kReturnStruct},
        {"_CTLineGetImageBounds", kReturnStruct},
        {"_CGDisplayBounds", kReturnStruct},
        {"_CVDisplayLinkGetNominalOutputVideoRefreshPeriod", kReturnStruct},
        {"_objc_msgSend_stret", kReturnStruct},
        {"_glDrawRangeElements", kReturnOrdinary},
        {"_OSAtomicAdd32Barrier", kReturnOrdinary},
        {"_objc_msgSend", kReturnOrdinary},
        {"_lp32_steam_0_2", kReturnSteamDynamic},
    };
    uint32_t ordinary_id = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!compat_runtime32_guest_callback(cases[i].name)) return -1;
        uint32_t index;
        for (index = 0; index < dynamic_symbol_count; ++index)
            if (!strcmp(dynamic_symbol_names[index], cases[i].name)) break;
        if (index == dynamic_symbol_count) return -1;
        uint32_t id = UINT32_C(0x80000000) | index;
        if (cases[i].kind == kReturnOrdinary) ordinary_id = id;
        for (unsigned pass = 0; pass < 2; ++pass) {
            uint32_t stack[] = {id, 0x12345678, 0x456789ab, 0xabcdefff};
            uint32_t expected[] = {id, 0x12345678, 0x456789ab, 0xabcdefff};
            uint32_t *adjusted = lp32_adjust_import_stack(stack);
            uint32_t *reference = uncached_import_return_for_test(expected);
            if ((adjusted - stack) != (reference - expected) ||
                memcmp(stack, expected, sizeof(stack)) ||
                dynamic_return_kinds[index] != cases[i].kind) return -1;
            if (cases[i].kind == kReturnStruct &&
                (adjusted != stack + 1 || stack[2] != 0x12345678)) return -1;
            if (cases[i].kind == kReturnOrdinary && adjusted != stack) return -1;
        }
    }
    /* Static Mach-O IDs use a separate table and must make the same decision. */
    for (uint32_t i = 0; i < current_image->import_count; ++i) {
        uint32_t a[] = {i, 123, 456, 789}, b[] = {i, 123, 456, 789};
        if (lp32_adjust_import_stack(a) - a != uncached_import_return_for_test(b) - b ||
            memcmp(a, b, sizeof(a))) return -1;
    }
    uint32_t stack[] = {ordinary_id, 0, 0, 0};
    uint32_t *(*volatile functions[])(uint32_t *) = {
        uncached_import_return_for_test, lp32_adjust_import_stack};
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t start = hitch_now();
        for (unsigned i = 0; i < 1000000; ++i)
            if (functions[pass](stack) != stack) return -1;
        fprintf(stderr, "import-return-selftest: %s %.1f ns/call\n",
                pass ? "cached" : "uncached", (hitch_now() - start) / 1e6);
    }
    puts("import-return-selftest: PASS (static/dynamic IDs, structure return stack, ordinary calls, Steam dynamic classification)");
    return 0;
}

static uint8_t hitch_import_kinds[MACHO_IMAGE32_MAX_IMPORTS];
static uint8_t hitch_dynamic_kinds[kDynamicThunkCapacity];

static void record_hitch_import(unsigned kind, const char *name,
                                const uint32_t *args, uint32_t caller,
                                struct hitch_scope *scope, uint64_t end)
{
    uint32_t count = 0;
    if (kind == HITCH_DRAW) {
        const char *draw = name + (name[0] == '_');
        count = strstr(draw, "RangeElements") ? args[3] :
                strstr(draw, "Arrays") ? args[2] : args[1];
    }
    hitch_scope_end(scope, kind, name, caller, end, count);
}

extern int libcpp_stream_bridge32_dispatch(const char *, const uint32_t *, uint64_t *);

/* The guest frame that called the chained import running on this thread;
   saved_registers is the gateway's {edi, esi, ebp, ebx} (64-bit slots). */
struct import_frame {
    const uint64_t *saved_registers;
    const uint32_t *arguments;
    uint32_t return_address;
};
static _Thread_local struct import_frame current_import_frame;
static _Thread_local struct guest_resume_context pending_resume;
static _Thread_local bool resume_pending;
/* Threads with a pending resume; the gateway only asks when nonzero. */
int lp32_resumes_pending;

bool compat_runtime32_import_frame(struct guest_resume_context *frame)
{
    const struct import_frame *current = &current_import_frame;
    if (!current->saved_registers) return false;
    *frame = (struct guest_resume_context){
        .edi = (uint32_t)current->saved_registers[0],
        .esi = (uint32_t)current->saved_registers[1],
        .ebp = (uint32_t)current->saved_registers[2],
        .ebx = (uint32_t)current->saved_registers[3],
        .esp = (uint32_t)(uintptr_t)current->arguments,
        .eip = current->return_address,
    };
    return true;
}

void compat_runtime32_resume_guest(const struct guest_resume_context *context)
{
    pending_resume = *context;
    if (!resume_pending) __atomic_add_fetch(&lp32_resumes_pending, 1, __ATOMIC_RELAXED);
    resume_pending = true;
}

const struct guest_resume_context *lp32_take_resume_context(void)
{
    if (!resume_pending) return NULL;
    resume_pending = false;
    __atomic_sub_fetch(&lp32_resumes_pending, 1, __ATOMIC_RELAXED);
    return &pending_resume;
}

static uint64_t fast_pthread_mutex_trylock(const uint32_t *arguments,
                                           uint32_t return_address);

uint64_t lp32_dispatch_import(uint32_t import_id, const uint32_t *arguments,
                              uint32_t return_address,
                              const uint64_t *saved_registers)
{
    lp32_fast_import_fn *fast_slot = fast_import_slot(import_id);
    lp32_fast_import_fn fast = fast_slot ? *fast_slot : NULL;
    unsigned hitch_kind = HITCH_NONE;
    const char *hitch_name = NULL;
    struct hitch_scope hitch_scope;
    if (hitch_recorder_enabled) {
        uint32_t slot = import_id & UINT32_C(0x7fffffff);
        uint8_t *kind = (import_id & UINT32_C(0x80000000)) ?
            (slot < kDynamicThunkCapacity ? &hitch_dynamic_kinds[slot] : NULL) :
            (slot < MACHO_IMAGE32_MAX_IMPORTS ? &hitch_import_kinds[slot] : NULL);
        if (kind) {
            hitch_kind = __atomic_load_n(kind, __ATOMIC_RELAXED);
            if (!hitch_kind) {
                hitch_kind = hitch_classify(import_name_for_id(import_id));
                __atomic_store_n(kind, hitch_kind, __ATOMIC_RELAXED);
            }
        }
        /* Keep cold dispatch and lazy host mutex allocation visible. The
           cached trylock operation itself never waits for another owner. */
        if (hitch_kind == HITCH_COUNTED_RUNTIME &&
            ((uintptr_t)fast <= 1 ||
             (fast == fast_pthread_mutex_trylock &&
              !guest_mutex_map_find(arguments[0]))))
            hitch_kind = HITCH_RUNTIME;
        if (hitch_kind == HITCH_COUNTED_RUNTIME) hitch_count_runtime();
        bool timed = hitch_kind != HITCH_GL_STATE || hitch_sample_gl_state();
        if (hitch_kind >= HITCH_DRAW && timed) {
            hitch_name = import_name_for_id(import_id);
            hitch_scope_begin(&hitch_scope, hitch_now());
            hitch_scope.counted = hitch_kind == HITCH_GL_STATE;
        }
    }
    if (!compat_runtime32_frame_profile_enabled) {
        uint64_t result;
        if ((uintptr_t)fast > 1) {
            result = fast(arguments, return_address);
        } else {
            struct import_frame outer = current_import_frame;
            current_import_frame = (struct import_frame){saved_registers, arguments, return_address};
            result = dispatch_import_chained(import_id, import_name_for_id(import_id),
                                             arguments, return_address, fast_slot);
            current_import_frame = outer;
        }
        if (hitch_name) record_hitch_import(hitch_kind, hitch_name, arguments,
                                          return_address, &hitch_scope, hitch_now());
        return result;
    }
    const char *name = import_name_for_id(import_id);
    if (guest_last_hold_ns >= kLongGuestHoldNs) {
        fprintf(stderr,
                "compat32: guest hold %.1fms on thread %#llx ended by %s "
                "from 0x%08" PRIx32 "\n",
                (double)guest_last_hold_ns / 1e6,
                (unsigned long long)pthread_mach_thread_np(pthread_self()),
                name, return_address);
        guest_last_hold_ns = 0;
    }
    uint64_t start = profile_now();
    struct import_frame outer = current_import_frame;
    current_import_frame = (struct import_frame){saved_registers, arguments, return_address};
    uint64_t result = (uintptr_t)fast > 1 ?
        fast(arguments, return_address) :
        dispatch_import_chained(import_id, name, arguments, return_address,
                                fast_slot);
    current_import_frame = outer;
    uint64_t elapsed = profile_now() - start;
    if (hitch_name) record_hitch_import(hitch_kind, hitch_name, arguments,
                                      return_address, &hitch_scope, hitch_now());
    frame_profile.dispatch_ns += elapsed;
    ++frame_profile.calls;
    struct import_profile_entry *entry = import_profile_slot(import_id);
    if (entry) {
        entry->name = name;
        entry->calls += 1;
        entry->ns += elapsed;
    }
    /* LP32_SLOW_IMPORT_MS=<n>: report any import on any thread that took
       longer than n ms, to find where a worker (audio decoder, loader)
       stalls. */
    static int64_t slow_import_threshold_ns = -1;
    if (slow_import_threshold_ns < 0) {
        const char *text = getenv("LP32_SLOW_IMPORT_MS");
        slow_import_threshold_ns = text && text[0] ?
            (int64_t)(strtod(text, NULL) * 1e6) : 0;
    }
    if (slow_import_threshold_ns > 0 && (int64_t)elapsed >= slow_import_threshold_ns) {
        fprintf(stderr,
                "compat32: slow import %s %.1fms on thread %#llx from 0x%08" PRIx32
                " args=%08x %08x %08x\n",
                name, (double)elapsed / 1e6,
                (unsigned long long)pthread_mach_thread_np(pthread_self()),
                return_address, arguments[0], arguments[1], arguments[2]);
    }
    return result;
}

void compat_runtime32_report_import_profile(unsigned top)
{
    struct import_profile_entry *entries[2] = {
        import_profile_table, dynamic_import_profile_table,
    };
    size_t counts[2] = {
        current_image ? current_image->import_count : 0,
        kDynamicThunkCapacity,
    };
    uint64_t total_ns = 0;
    uint64_t total_calls = 0;
    for (unsigned table = 0; table < 2; ++table) {
        for (size_t index = 0; index < counts[table]; ++index) {
            total_ns += entries[table][index].ns;
            total_calls += entries[table][index].calls;
        }
    }
    fprintf(stderr, "compat32: import profile total calls=%llu time=%.1fms\n",
            (unsigned long long)total_calls, (double)total_ns / 1e6);
    for (unsigned rank = 0; rank < top; ++rank) {
        struct import_profile_entry *best = NULL;
        for (unsigned table = 0; table < 2; ++table) {
            for (size_t index = 0; index < counts[table]; ++index) {
                struct import_profile_entry *entry = &entries[table][index];
                if (entry->calls && (!best || entry->ns > best->ns)) best = entry;
            }
        }
        if (!best) break;
        fprintf(stderr, "compat32:   %-40s calls=%-8llu time=%7.2fms avg=%6.0fns\n",
                best->name ? best->name : "?", (unsigned long long)best->calls,
                (double)best->ns / 1e6, (double)best->ns / (double)best->calls);
        best->calls = 0;
        best->ns = 0;
    }
    for (unsigned table = 0; table < 2; ++table) {
        for (size_t index = 0; index < counts[table]; ++index) {
            entries[table][index].calls = 0;
            entries[table][index].ns = 0;
        }
    }
}

static uint64_t dispatch_bridge_stages(uint32_t import_id, const char *name,
                                       const uint32_t *arguments,
                                       uint32_t return_address,
                                       uint8_t known_stage);

/*
 * Direct handlers for the runtime imports the game issues thousands of
 * times per frame.  The named chain below calls the same functions, so a
 * memoized direct call and a chained call behave identically.
 */
/* LP32_TRACE_ATOMIC=0xaddr[,0xaddr...] logs every OSAtomicAdd32 on the listed
   guest cells with the calling thread and guest return address (diagnostic
   for lock-free queues whose indices go out of step). */
static uint32_t atomic_trace_cells[8];
static unsigned atomic_trace_count;
static bool atomic_trace_parsed;

static void parse_atomic_trace(void)
{
    atomic_trace_parsed = true;
    const char *spec = getenv("LP32_TRACE_ATOMIC");
    if (!spec || !spec[0]) return;
    char buffer[256];
    strlcpy(buffer, spec, sizeof(buffer));
    char *cursor = buffer;
    for (char *item = strsep(&cursor, ","); item; item = strsep(&cursor, ",")) {
        if (atomic_trace_count == sizeof(atomic_trace_cells) /
                                  sizeof(atomic_trace_cells[0])) break;
        uint32_t address = (uint32_t)strtoul(item, NULL, 0);
        if (address) atomic_trace_cells[atomic_trace_count++] = address;
    }
}

static uint64_t fast_OSAtomicAdd32(const uint32_t *arguments,
                                   uint32_t return_address)
{
    int32_t *value = (void *)(uintptr_t)arguments[1];
    int32_t result = __atomic_add_fetch(value, (int32_t)arguments[0],
                                        __ATOMIC_SEQ_CST);
    if (!atomic_trace_parsed) parse_atomic_trace();
    for (unsigned index = 0; index < atomic_trace_count; ++index) {
        if (atomic_trace_cells[index] != arguments[1]) continue;
        uint64_t thread_id = 0;
        pthread_threadid_np(NULL, &thread_id);
        fprintf(stderr,
                "compat32: atomic-add tid=%llu ra=0x%08x [0x%08x] %d -> %d\n",
                (unsigned long long)thread_id, return_address, arguments[1],
                result - (int32_t)arguments[0], result);
        break;
    }
    return (uint32_t)result;
}

static uint64_t fast_OSAtomicCompareAndSwap32(const uint32_t *arguments,
                                              uint32_t return_address)
{
    (void)return_address;
    int32_t expected = (int32_t)arguments[0];
    int32_t *value = (void *)(uintptr_t)arguments[2];
    return __atomic_compare_exchange_n(value, &expected, (int32_t)arguments[1],
                                       false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

static uint64_t fast_memmove(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    memmove((void *)(uintptr_t)arguments[0], (const void *)(uintptr_t)arguments[1],
            arguments[2]);
    return arguments[0];
}

static uint64_t fast_memset(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    memset((void *)(uintptr_t)arguments[0], (int)arguments[1], arguments[2]);
    return arguments[0];
}

/* COD4's asset loader calls these millions of times between loading-screen
 * redraws. Reuse the ordinary implementations through memoized handlers,
 * without repeating every bridge's name dispatch for each byte/string. */
static uint64_t fast_strlen(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return (uint32_t)strlen((const char *)(uintptr_t)a[0]);
}
static uint64_t fast_strcmp(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return (uint32_t)strcmp((const char *)(uintptr_t)a[0], (const char *)(uintptr_t)a[1]);
}
static uint64_t fast_memcmp(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return (uint32_t)memcmp((const void *)(uintptr_t)a[0], (const void *)(uintptr_t)a[1], a[2]);
}
/* Quake-derived engines shift strings left in place with strcpy (for example
 * Info_RemoveKey on objective configstrings).  The i386 libc they shipped
 * against copied forward a byte at a time; the host's vectorized strcpy
 * corrupts overlapping copies. */
static uint64_t fast_strcpy(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    const char *source = (const char *)(uintptr_t)a[1];
    memmove((char *)(uintptr_t)a[0], source, strlen(source) + 1);
    return a[0];
}
static uint64_t fast_strncpy(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    strncpy((char *)(uintptr_t)a[0], (const char *)(uintptr_t)a[1], a[2]);
    return a[0];
}
static uint64_t fast_tolower(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return a[0] <= UCHAR_MAX ? (uint32_t)tolower((unsigned char)a[0]) : a[0];
}
static uint64_t fast_pthread_self(const uint32_t *a, uint32_t caller)
{
    (void)a; (void)caller;
    return guest_pthread_self();
}
static uint64_t fast_pthread_getspecific(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return a[0] < 128 ? guest_pthread_values[a[0]] : 0;
}
static uint64_t fast_dynamic_cast(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return rtti_bridge32_cast(a[0], a[1], a[2]);
}

static uint64_t fast_logf(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    return return_guest_float(logf(guest_float(arguments[0])));
}

static uint64_t fast_expf(const uint32_t *arguments, uint32_t return_address)
{
    (void)return_address;
    return return_guest_float(expf(guest_float(arguments[0])));
}

/* TEMPORARY contention profile (LP32_DEBUG_MUTEX_HOT). */
static struct { uint32_t mutex, calls, contended; uint64_t wait_ns; uint32_t trace[6]; } hot_mutexes[4096];
static void hot_mutex_report(void)
{
    for (int rank = 0; rank < 8; ++rank) {
        int best = -1;
        for (int i = 0; i < 4096; ++i)
            if (hot_mutexes[i].calls && (best < 0 || hot_mutexes[i].wait_ns > hot_mutexes[best].wait_ns)) best = i;
        if (best < 0) break;
        fprintf(stderr, "HOT mutex=%08x calls=%u contended=%u wait=%.1fms trace=%08x %08x %08x %08x %08x %08x\n",
                hot_mutexes[best].mutex, hot_mutexes[best].calls, hot_mutexes[best].contended,
                hot_mutexes[best].wait_ns / 1e6, hot_mutexes[best].trace[0], hot_mutexes[best].trace[1],
                hot_mutexes[best].trace[2], hot_mutexes[best].trace[3], hot_mutexes[best].trace[4], hot_mutexes[best].trace[5]);
        hot_mutexes[best].calls = 0;
    }
    memset(hot_mutexes, 0, sizeof(hot_mutexes));
}

/* Darwin handles the contended handoff. A custom pause/clock polling loop
 * under Rosetta consumes CPU on every waiter and can starve the lock owner. */
static uint64_t fast_pthread_mutex_lock(const uint32_t *arguments,
                                        uint32_t return_address)
{
    pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], true);
    if (!mutex) return (uint32_t)ENOMEM;
    static int hot = -1;
    if (hot < 0) hot = getenv("LP32_DEBUG_MUTEX_HOT") != NULL;
    if (hot) {
        uint32_t slot = (arguments[0] >> 3) % 4096;
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        bool contended = pthread_mutex_trylock(mutex) != 0;
        int status = contended ? pthread_mutex_lock(mutex) : 0;
        hot_mutexes[slot].mutex = arguments[0];
        ++hot_mutexes[slot].calls;
        if (contended) {
            ++hot_mutexes[slot].contended;
            hot_mutexes[slot].wait_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
            if (!hot_mutexes[slot].trace[0])
                for (unsigned i = 0, n = 0; i < 1024 && n < 6; ++i) {
                    uint32_t v = arguments[i];
                    if (v > 0x1005 && v < 0x3d4000 && *(const uint8_t *)(uintptr_t)(v - 5) == 0xe8)
                        hot_mutexes[slot].trace[n++] = v;
                }
        }
        static uint64_t last_report;
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (!last_report) last_report = now;
        if (now - last_report > 10000000000ull) { last_report = now; hot_mutex_report(); }
        return (uint32_t)status;
    }
    int status = pthread_mutex_lock(mutex);
    const struct lp32_render_pool *pool = lp32_profile()->render_pool;
    if (!pool) return (uint32_t)status;
    if (return_address == pool->return_address &&
        getenv("LP32_TRACE_RENDER_POOL")) {
        const uint32_t *frame =
            (const void *)((uintptr_t)arguments + UINT32_C(0x68));
        fprintf(stderr,
                "compat32: renderer pool lock address=0x%08" PRIx32
                " status=%d head=0x%08" PRIx32 " count=%" PRIu32
                " create=%" PRIu32 "x%" PRIu32 " format=%" PRIu32
                " levels=%" PRIu32 "\n",
                arguments[0], status,
                *(uint32_t *)(uintptr_t)pool->free_head,
                *(uint32_t *)(uintptr_t)pool->free_count,
                frame[3], frame[4], frame[2], frame[6]);
    }
    if (status == 0 &&
        (arguments[0] == pool->mutex || return_address == pool->return_address)) {
        replenish_legacy_render_pool_if_empty();
    }
    return (uint32_t)status;
}

static uint64_t fast_pthread_mutex_trylock(const uint32_t *arguments,
                                           uint32_t return_address)
{
    (void)return_address;
    pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], true);
    return mutex ? (uint32_t)pthread_mutex_trylock(mutex) : (uint32_t)ENOMEM;
}

static uint64_t fast_pthread_mutex_unlock(const uint32_t *arguments,
                                          uint32_t return_address)
{
    (void)return_address;
    pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], false);
    return mutex ? (uint32_t)pthread_mutex_unlock(mutex) : (uint32_t)EINVAL;
}

enum guest_printf_kind {
    kGuestPrintf, kGuestSprintf, kGuestSnprintf, kGuestVsprintf, kGuestVsnprintf,
    kGuestSprintfChk, kGuestVsnprintfChk,
};

/* printf-family imports; also the direct handlers for the va_list forms. */
static uint32_t guest_printf_family(enum guest_printf_kind kind, const uint32_t *arguments,
                                    uint32_t return_address)
{
    char *destination = NULL;
    size_t destination_size = 65536;
    const char *format;
    const uint32_t *values;
    bool temporary = kind == kGuestPrintf;
    if (kind == kGuestSprintf) {
        destination = (void *)(uintptr_t)arguments[0];
        format = (const void *)(uintptr_t)arguments[1];
        values = arguments + 2;
    } else if (kind == kGuestSnprintf) {
        destination = (void *)(uintptr_t)arguments[0];
        destination_size = arguments[1];
        format = (const void *)(uintptr_t)arguments[2];
        values = arguments + 3;
    } else if (kind == kGuestVsprintf) {
        destination = (void *)(uintptr_t)arguments[0];
        format = (const void *)(uintptr_t)arguments[1];
        values = (const void *)(uintptr_t)arguments[2];
    } else if (kind == kGuestVsnprintf) {
        destination = (void *)(uintptr_t)arguments[0];
        destination_size = arguments[1];
        format = (const void *)(uintptr_t)arguments[2];
        values = (const void *)(uintptr_t)arguments[3];
    } else if (kind == kGuestSprintfChk) {
        /* (dst, flag, dstlen, format, ...) */
        destination = (void *)(uintptr_t)arguments[0];
        destination_size = arguments[2];
        format = (const void *)(uintptr_t)arguments[3];
        values = arguments + 4;
    } else if (kind == kGuestVsnprintfChk) {
        /* (dst, maxlen, flag, dstlen, format, va_list) */
        destination = (void *)(uintptr_t)arguments[0];
        destination_size = arguments[1] < arguments[3] ? arguments[1] : arguments[3];
        format = (const void *)(uintptr_t)arguments[4];
        values = (const void *)(uintptr_t)arguments[5];
    } else {
        format = (const void *)(uintptr_t)arguments[0];
        values = arguments + 1;
    }

    char *scratch = temporary || kind == kGuestSprintf ||
                    kind == kGuestVsprintf ? malloc(65536) : destination;
    if ((!scratch && destination_size != 0) || !format || !values) {
        if (scratch && scratch != destination) free(scratch);
        return (uint32_t)-1;
    }
    int formatted = guest_vformat(scratch,
                                  scratch == destination ? destination_size : 65536,
                                  format, values);
    static int trace_resolution = -1;
    if (trace_resolution < 0) trace_resolution = getenv("LP32_TRACE_RESOLUTION") != NULL;
    if (trace_resolution && scratch && formatted >= 0 &&
        (strstr(scratch, " x ") || strstr(format, " x "))) {
        fprintf(stderr,
                "compat32: resolution format caller=0x%08" PRIx32
                " format=\"%s\" output=\"%s\"\n",
                return_address, format, scratch);
    }
    if (!temporary && scratch != destination && destination) strcpy(destination, scratch);
    if (temporary && formatted >= 0) fputs(scratch, stdout);
    if (scratch != destination) free(scratch);
    return (uint32_t)formatted;
}

static uint64_t fast_vsnprintf(const uint32_t *a, uint32_t caller)
{
    return guest_printf_family(kGuestVsnprintf, a, caller);
}
static uint64_t fast_snprintf(const uint32_t *a, uint32_t caller)
{
    return guest_printf_family(kGuestSnprintf, a, caller);
}
static uint64_t fast_sprintf(const uint32_t *a, uint32_t caller)
{
    return guest_printf_family(kGuestSprintf, a, caller);
}

static uint64_t fast_operator_new(const uint32_t *a, uint32_t caller)
{
    return guest_allocate_at(a[0], false, caller);
}
static uint64_t fast_operator_delete(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    guest_deallocate(a[0]);
    return 0;
}
static uint64_t fast_udivdi3(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    uint64_t dividend = (uint64_t)a[0] | ((uint64_t)a[1] << 32);
    uint64_t divisor = (uint64_t)a[2] | ((uint64_t)a[3] << 32);
    return divisor ? dividend / divisor : 0;
}
static uint64_t fast_umoddi3(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    uint64_t dividend = (uint64_t)a[0] | ((uint64_t)a[1] << 32);
    uint64_t divisor = (uint64_t)a[2] | ((uint64_t)a[3] << 32);
    return divisor ? dividend % divisor : 0;
}
static uint64_t fast_mach_absolute_time(const uint32_t *a, uint32_t caller)
{
    (void)a; (void)caller;
    return mach_absolute_time();
}

/* Math, allocator and string helpers games call thousands of times per
   frame; the same operations as their chained cases. */
#define FAST_FLOAT1(fn, op) static uint64_t fast_##fn(const uint32_t *a, uint32_t caller) \
    { (void)caller; return return_guest_float(op(guest_float(a[0]))); }
#define FAST_FLOAT2(fn, op) static uint64_t fast_##fn(const uint32_t *a, uint32_t caller) \
    { (void)caller; return return_guest_float(op(guest_float(a[0]), guest_float(a[1]))); }
#define FAST_DOUBLE1(fn, op) static uint64_t fast_##fn(const uint32_t *a, uint32_t caller) \
    { (void)caller; return return_guest_double(op(guest_double(a))); }
#define FAST_DOUBLE2(fn, op) static uint64_t fast_##fn(const uint32_t *a, uint32_t caller) \
    { (void)caller; return return_guest_double(op(guest_double(a), guest_double(a + 2))); }
FAST_FLOAT1(floorf, floorf)
FAST_FLOAT1(ceilf, ceilf)
FAST_FLOAT1(acosf, acosf)
FAST_FLOAT1(asinf, asinf)
FAST_FLOAT1(sinf, sinf)
FAST_FLOAT1(cosf, cosf)
FAST_FLOAT1(atanf, atanf)
FAST_FLOAT1(roundf, roundf)
FAST_FLOAT2(powf, powf)
FAST_FLOAT2(atan2f, atan2f)
FAST_FLOAT2(fmodf, fmodf)
FAST_DOUBLE1(floor, floor)
FAST_DOUBLE1(ceil, ceil)
FAST_DOUBLE1(sin, sin)
FAST_DOUBLE1(cos, cos)
FAST_DOUBLE2(pow, pow)
FAST_DOUBLE2(atan2, atan2)
FAST_DOUBLE2(fmod, fmod)
#undef FAST_FLOAT1
#undef FAST_FLOAT2
#undef FAST_DOUBLE1
#undef FAST_DOUBLE2
static uint64_t fast_malloc(const uint32_t *a, uint32_t caller)
{
    return guest_allocate_at(a[0], false, caller);
}
static uint64_t fast_calloc(const uint32_t *a, uint32_t caller)
{
    uint64_t size = (uint64_t)a[0] * a[1];
    return size <= SIZE_MAX ? guest_allocate_at((size_t)size, true, caller) : 0;
}
static uint64_t fast_bzero(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    memset((void *)(uintptr_t)a[0], 0, a[1]);
    return 0;
}
static uint64_t fast_strncmp(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    return (uint32_t)strncmp((const char *)(uintptr_t)a[0], (const char *)(uintptr_t)a[1], a[2]);
}
static uint64_t fast_divdi3(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    int64_t dividend = (int64_t)((uint64_t)a[0] | ((uint64_t)a[1] << 32));
    int64_t divisor = (int64_t)((uint64_t)a[2] | ((uint64_t)a[3] << 32));
    if (!divisor) return 0;
    if (dividend == INT64_MIN && divisor == -1) return (uint64_t)INT64_MIN;
    return (uint64_t)(dividend / divisor);
}
static uint64_t fast_moddi3(const uint32_t *a, uint32_t caller)
{
    (void)caller;
    int64_t dividend = (int64_t)((uint64_t)a[0] | ((uint64_t)a[1] << 32));
    int64_t divisor = (int64_t)((uint64_t)a[2] | ((uint64_t)a[3] << 32));
    if (!divisor || (dividend == INT64_MIN && divisor == -1)) return 0;
    return (uint64_t)(dividend % divisor);
}

static uint64_t fast_pthread_once(const uint32_t *arguments, uint32_t caller)
{
    (void)caller;
    return (uint32_t)guest_pthread_once(arguments[0], arguments[1]);
}

static lp32_fast_import_fn runtime_fast_import(const char *name)
{
    static const struct {
        const char *name;
        lp32_fast_import_fn handler;
    } table[] = {
        {"_OSAtomicAdd32", fast_OSAtomicAdd32},
        {"_OSAtomicAdd32Barrier", fast_OSAtomicAdd32},
        {"_OSAtomicCompareAndSwap32", fast_OSAtomicCompareAndSwap32},
        {"_OSAtomicCompareAndSwapInt", fast_OSAtomicCompareAndSwap32},
        {"_OSAtomicCompareAndSwap32Barrier", fast_OSAtomicCompareAndSwap32},
        {"_OSAtomicCompareAndSwapPtrBarrier", fast_OSAtomicCompareAndSwap32},
        {"_memcpy", fast_memmove},
        {"_memmove", fast_memmove},
        {"___memcpy_chk", fast_memmove},
        {"_memset", fast_memset},
        {"___memset_chk", fast_memset},
        {"_strlen", fast_strlen},
        {"_strcmp", fast_strcmp},
        {"_memcmp", fast_memcmp},
        {"_strcpy", fast_strcpy},
        {"_strncpy", fast_strncpy},
        {"___tolower", fast_tolower},
        {"_pthread_self", fast_pthread_self},
        {"_pthread_once", fast_pthread_once},
        {"_inflate", zlib_bridge32_inflate},
        {"_deflate", zlib_bridge32_deflate},
        {"_logf", fast_logf},
        {"_expf", fast_expf},
        {"_pthread_mutex_lock", fast_pthread_mutex_lock},
        {"_pthread_mutex_trylock", fast_pthread_mutex_trylock},
        {"_pthread_mutex_unlock", fast_pthread_mutex_unlock},
        {"_pthread_getspecific", fast_pthread_getspecific},
        {"___dynamic_cast", fast_dynamic_cast},
        {"_vsnprintf", fast_vsnprintf},
        {"_snprintf", fast_snprintf},
        {"_sprintf", fast_sprintf},
        {"__Znwm", fast_operator_new},
        {"__Znam", fast_operator_new},
        {"__ZdlPv", fast_operator_delete},
        {"__ZdaPv", fast_operator_delete},
        {"___udivdi3", fast_udivdi3},
        {"___umoddi3", fast_umoddi3},
        {"___divdi3", fast_divdi3},
        {"___moddi3", fast_moddi3},
        {"_malloc", fast_malloc},
        {"_calloc", fast_calloc},
        {"_free", fast_operator_delete},
        {"_bzero", fast_bzero},
        {"___bzero", fast_bzero},
        {"_strncmp", fast_strncmp},
        {"_floorf", fast_floorf},
        {"_ceilf", fast_ceilf},
        {"_acosf", fast_acosf},
        {"_asinf", fast_asinf},
        {"_sinf", fast_sinf},
        {"_cosf", fast_cosf},
        {"_atanf", fast_atanf},
        {"_roundf", fast_roundf},
        {"_powf", fast_powf},
        {"_atan2f", fast_atan2f},
        {"_fmodf", fast_fmodf},
        {"_floor", fast_floor},
        {"_ceil", fast_ceil},
        {"_sin", fast_sin},
        {"_cos", fast_cos},
        {"_pow", fast_pow},
        {"_atan2", fast_atan2},
        {"_fmod", fast_fmod},
    };
    /* Timing traces log from the chained handler. */
    if (!timing_trace && strcmp(name, "_mach_absolute_time") == 0) return fast_mach_absolute_time;
    lp32_fast_import_fn tlv = tlv_bridge32_fast_import(name);
    if (tlv) return tlv;
    for (size_t index = 0; index < sizeof(table) / sizeof(table[0]); ++index) {
        if (strcmp(name, table[index].name) == 0) return table[index].handler;
    }
    return NULL;
}

/* The guest's 32-bit address space above the 2 GB __LEGACY reservation.
   Left unreserved, host allocators (libmalloc, CoreAudio's caulk heap) put
   memory there, where MW2's VirtualAlloc emulation (MAP_FIXED commits and
   decommits, munmap releases) can replace it: a caulk heap page there was
   found zeroed and the next AudioQueueNewOutput trapped. At startup every
   free hole in the range is reserved; guest mmap/munmap then only touch
   those pages, and fixed mappings over anything else are refused. */
enum { kGuestArenaStart = 0x80000000u, kGuestArenaMaxRanges = 1024 };
struct guest_range { uint64_t start, end; };
static struct guest_range guest_arena[kGuestArenaMaxRanges];   /* reserved spans */
static unsigned guest_arena_count;
static struct guest_range guest_arena_free[kGuestArenaMaxRanges];
static unsigned guest_arena_free_count;
static pthread_mutex_t guest_arena_lock = PTHREAD_MUTEX_INITIALIZER;
static bool guest_arena_ready;

static bool range_within(const struct guest_range *ranges, unsigned count,
                         uint64_t start, uint64_t end)
{
    for (unsigned i = 0; i < count; ++i) {
        if (start >= ranges[i].end || end <= ranges[i].start) continue;
        if (start < ranges[i].start) return false;
        if (end <= ranges[i].end) return true;
        start = ranges[i].end;
    }
    return start >= end;
}

static void arena_free_remove(uint64_t start, uint64_t end)
{
    struct guest_range kept[kGuestArenaMaxRanges];
    unsigned count = 0;
    for (unsigned i = 0; i < guest_arena_free_count; ++i) {
        struct guest_range r = guest_arena_free[i];
        if (end <= r.start || start >= r.end) { kept[count++] = r; continue; }
        if (r.start < start && count < kGuestArenaMaxRanges) kept[count++] = (struct guest_range){r.start, start};
        if (end < r.end && count < kGuestArenaMaxRanges) kept[count++] = (struct guest_range){end, r.end};
    }
    memcpy(guest_arena_free, kept, count * sizeof(kept[0]));
    guest_arena_free_count = count;
}

static void arena_free_add(uint64_t start, uint64_t end)
{
    arena_free_remove(start, end);
    unsigned i = 0;
    while (i < guest_arena_free_count && guest_arena_free[i].start < start) ++i;
    if (guest_arena_free_count == kGuestArenaMaxRanges) return;
    memmove(guest_arena_free + i + 1, guest_arena_free + i,
            (guest_arena_free_count - i) * sizeof(guest_arena_free[0]));
    guest_arena_free[i] = (struct guest_range){start, end};
    ++guest_arena_free_count;
    /* Coalesce neighbours. */
    unsigned out = 0;
    for (unsigned j = 0; j < guest_arena_free_count; ++j) {
        if (out && guest_arena_free[out - 1].end == guest_arena_free[j].start)
            guest_arena_free[out - 1].end = guest_arena_free[j].end;
        else
            guest_arena_free[out++] = guest_arena_free[j];
    }
    guest_arena_free_count = out;
}

void compat_runtime32_reserve_guest_arena(void)
{
    pthread_mutex_lock(&guest_arena_lock);
    if (guest_arena_ready) { pthread_mutex_unlock(&guest_arena_lock); return; }
    uint64_t cursor = kGuestArenaStart, reserved = 0;
    while (cursor < UINT64_C(0x100000000) && guest_arena_count < kGuestArenaMaxRanges) {
        mach_vm_address_t region = cursor;
        mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t status = mach_vm_region(mach_task_self(), &region, &length, VM_REGION_BASIC_INFO_64,
                                              (vm_region_info_t)&info, &count, &object);
        uint64_t hole_end = status ? UINT64_C(0x100000000) :
                            (region < UINT64_C(0x100000000) ? region : UINT64_C(0x100000000));
        if (hole_end > cursor) {
            mach_vm_address_t address = cursor;
            if (mach_vm_allocate(mach_task_self(), &address, hole_end - cursor, VM_FLAGS_FIXED) == KERN_SUCCESS) {
                mach_vm_protect(mach_task_self(), address, hole_end - cursor, false, VM_PROT_NONE);
                guest_arena[guest_arena_count++] = (struct guest_range){cursor, hole_end};
                arena_free_add(cursor, hole_end);
                reserved += hole_end - cursor;
            }
        }
        if (status) break;
        cursor = region + length;
    }
    guest_arena_ready = true;
    uint64_t stacks = kNativeGuestStackBase, stacks_end = stacks + kGuestStackSize;
    if (range_within(guest_arena_free, guest_arena_free_count, stacks, stacks_end) &&
        mmap((void *)(uintptr_t)stacks, kGuestStackSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)(uintptr_t)stacks) {
        arena_free_remove(stacks, stacks_end);
        guest_stack_base = kNativeGuestStackBase;
    }
    pthread_mutex_unlock(&guest_arena_lock);
    if (getenv("LP32_TRACE_GUEST_VM"))
        fprintf(stderr, "compat32: guest arena reserved %u ranges, %llu MB above 0x%08x; stacks at 0x%08x\n",
                guest_arena_count, (unsigned long long)(reserved >> 20), kGuestArenaStart, guest_stack_base);
}

static bool trace_guest_vm(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_TRACE_GUEST_VM") != NULL;
    return enabled;
}

static uint32_t guest_arena_mmap(uint32_t hint, uint64_t size, int protection, int flags,
                                 int fd, off_t offset, uint32_t return_address)
{
    uint64_t page = (uint64_t)getpagesize();
    uint64_t length = (size + page - 1) & ~(page - 1);
    if (!length || length > UINT64_C(0x100000000)) { errno = EINVAL; return UINT32_MAX; }
    void *mapping = MAP_FAILED;
    pthread_mutex_lock(&guest_arena_lock);
    uint64_t start = hint & ~(page - 1), end = start + length;
    bool in_arena = guest_arena_ready && range_within(guest_arena, guest_arena_count, start, end);
    if ((flags & MAP_FIXED) && length > UINT64_C(0x100000000) - start) {
        /* Validate before MAP_FIXED can replace native allocations above the
           guest address space. Checking the return value is too late. */
        errno = EINVAL;
    } else if (flags & MAP_FIXED) {
        if (!guest_arena_ready || start < kGuestArenaStart || in_arena) {
            mapping = mmap((void *)(uintptr_t)hint, size, protection, flags, fd, offset);
            if (mapping != MAP_FAILED && in_arena) arena_free_remove(start, end);
        } else {
            fprintf(stderr, "compat32: refused guest MAP_FIXED over host memory 0x%08llx-0x%08llx "
                    "from 0x%08" PRIx32 "\n", (unsigned long long)start, (unsigned long long)end,
                    return_address);
            errno = ENOMEM;
        }
    } else if (guest_arena_ready) {
        uint64_t chosen = 0;
        if (hint && end <= UINT64_C(0x100000000) && range_within(guest_arena_free, guest_arena_free_count, start, end))
            chosen = start;
        for (unsigned i = 0; !chosen && i < guest_arena_free_count; ++i)
            if (guest_arena_free[i].end - guest_arena_free[i].start >= length) chosen = guest_arena_free[i].start;
        if (chosen) {
            mapping = mmap((void *)(uintptr_t)chosen, size, protection, flags | MAP_FIXED, fd, offset);
            if (mapping != MAP_FAILED) arena_free_remove(chosen, chosen + length);
        } else {
            errno = ENOMEM;
        }
    } else {
        /* Before the arena exists (self-tests): let the kernel place it
           in the upper half of the guest's space. */
        for (uint64_t try = hint ? hint : kGuestArenaStart;
             try < UINT64_C(0x100000000) && size <= UINT64_C(0x100000000) - try; try += 0x08000000) {
            mapping = mmap((void *)(uintptr_t)try, size, protection, flags, fd, offset);
            if (mapping == MAP_FAILED) break;
            if ((uintptr_t)mapping <= UINT32_MAX && size <= UINT64_C(0x100000000) - (uintptr_t)mapping) break;
            munmap(mapping, size); mapping = MAP_FAILED; errno = ENOMEM;
        }
    }
    pthread_mutex_unlock(&guest_arena_lock);
    if (mapping != MAP_FAILED && ((uintptr_t)mapping > UINT32_MAX ||
        size > UINT64_C(0x100000000) - (uintptr_t)mapping)) {
        munmap(mapping, size); mapping = MAP_FAILED; errno = ENOMEM;
    }
    if (trace_guest_vm())
        fprintf(stderr, "compat32: guest mmap hint=0x%08" PRIx32 " size=0x%llx prot=%d flags=0x%x -> 0x%08lx from 0x%08" PRIx32 "\n",
                hint, (unsigned long long)size, protection, flags,
                mapping == MAP_FAILED ? 0xffffffffUL : (unsigned long)(uintptr_t)mapping, return_address);
    return mapping == MAP_FAILED ? UINT32_MAX : (uint32_t)(uintptr_t)mapping;
}

static int guest_arena_munmap(uint32_t address, uint64_t size, uint32_t return_address)
{
    uint64_t page = (uint64_t)getpagesize();
    uint64_t start = address, end = (start + size + page - 1) & ~(page - 1);
    if (start & (page - 1) || !size || size > UINT64_C(0x100000000) - start) {
        errno = EINVAL;
        return -1;
    }
    int status = 0;
    pthread_mutex_lock(&guest_arena_lock);
    if (!guest_arena_ready || start < kGuestArenaStart) {
        status = munmap((void *)(uintptr_t)address, size);
    } else if (range_within(guest_arena, guest_arena_count, start, end)) {
        /* Keep the pages reserved for the guest instead of returning them. */
        void *reserved = mmap((void *)(uintptr_t)start, end - start, PROT_NONE,
                              MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (reserved == MAP_FAILED) status = -1;
        else arena_free_add(start, end);
    } else {
        fprintf(stderr, "compat32: refused guest munmap of host memory 0x%08llx-0x%08llx from 0x%08" PRIx32 "\n",
                (unsigned long long)start, (unsigned long long)end, return_address);
        errno = EINVAL;
        status = -1;
    }
    pthread_mutex_unlock(&guest_arena_lock);
    if (trace_guest_vm())
        fprintf(stderr, "compat32: guest munmap 0x%08" PRIx32 " size=0x%llx -> %d from 0x%08" PRIx32 "\n",
                address, (unsigned long long)size, status, return_address);
    return status;
}

/* Free arena pages are reservations, not guest memory; hide them from the
   guest's address-space walk as the holes they stand in for. */
static bool guest_arena_free_end(uint64_t address, uint64_t *end)
{
    bool found = false;
    pthread_mutex_lock(&guest_arena_lock);
    for (unsigned i = 0; i < guest_arena_free_count; ++i)
        if (address >= guest_arena_free[i].start && address < guest_arena_free[i].end) {
            *end = guest_arena_free[i].end; found = true; break;
        }
    pthread_mutex_unlock(&guest_arena_lock);
    return found;
}

/* getifaddrs with the i386 struct ifaddrs (32-bit pointers).  The whole list
   (records, names, socket addresses and AF_LINK if_data, whose layout is the
   same in both ABIs) lives in one guest block that freeifaddrs releases. */
static uint32_t guest_getifaddrs(uint32_t output)
{
    struct ifaddrs *list = NULL;
    if (!output) return (uint32_t)-1;
    if (getifaddrs(&list)) return (uint32_t)-1;
    size_t size = 0;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        size += 28 + ((strlen(i->ifa_name) + 4) & ~(size_t)3);
        struct sockaddr *addresses[] = {i->ifa_addr, i->ifa_netmask, i->ifa_dstaddr};
        for (unsigned j = 0; j < 3; ++j)
            if (addresses[j]) size += (addresses[j]->sa_len + 3u) & ~3u;
        if (i->ifa_data && i->ifa_addr && i->ifa_addr->sa_family == AF_LINK) size += sizeof(struct if_data);
    }
    uint32_t block = guest_allocate(size ? size : 4, true);
    if (!block) { freeifaddrs(list); errno = ENOMEM; return (uint32_t)-1; }
    uint32_t cursor = block, previous = 0, first = 0;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        uint32_t record = cursor;
        uint32_t *fields = (void *)(uintptr_t)record;
        cursor += 28;
        size_t name_length = strlen(i->ifa_name);
        memcpy((void *)(uintptr_t)cursor, i->ifa_name, name_length + 1);
        fields[1] = cursor;
        cursor += (uint32_t)((name_length + 4) & ~(size_t)3);
        fields[2] = i->ifa_flags;
        struct sockaddr *addresses[] = {i->ifa_addr, i->ifa_netmask, i->ifa_dstaddr};
        for (unsigned j = 0; j < 3; ++j) {
            if (!addresses[j]) continue;
            memcpy((void *)(uintptr_t)cursor, addresses[j], addresses[j]->sa_len);
            fields[3 + j] = cursor;
            cursor += (addresses[j]->sa_len + 3u) & ~3u;
        }
        if (i->ifa_data && i->ifa_addr && i->ifa_addr->sa_family == AF_LINK) {
            memcpy((void *)(uintptr_t)cursor, i->ifa_data, sizeof(struct if_data));
            fields[6] = cursor;
            cursor += sizeof(struct if_data);
        }
        if (previous) ((uint32_t *)(uintptr_t)previous)[0] = record;
        else first = record;
        previous = record;
    }
    freeifaddrs(list);
    *(uint32_t *)(uintptr_t)output = first ? first : 0;
    if (!first) guest_deallocate(block);
    return 0;
}

static uint64_t dispatch_named_import_body(uint32_t import_id, const char *name,
                                           const uint32_t *arguments,
                                           uint32_t return_address);
static uint64_t dispatch_named_import(uint32_t import_id, const char *name,
                                      const uint32_t *arguments,
                                      uint32_t return_address)
{
    uint32_t cell = guest_errno_address ? guest_errno_address : guest_thread_errno_address;
    if (cell) errno = *(int32_t *)(uintptr_t)cell;
    size_t previous_length = dispatch_name_length;
    bool previous_match = dispatch_name_matched;
    uint64_t result = dispatch_named_import_body(import_id, name, arguments, return_address);
    int error = errno;
    dispatch_name_length = previous_length;
    dispatch_name_matched = previous_match;
    cell = guest_errno_address ? guest_errno_address : guest_thread_errno_address;
    if (cell) *(int32_t *)(uintptr_t)cell = error;
    return result;
}

static uint32_t mw2_command_overflow_target;
void compat_runtime32_set_command_overflow_target(uint32_t com_error) { mw2_command_overflow_target = com_error; }

static unsigned trace_print_argument;
void compat_runtime32_set_trace_print_argument(unsigned argument) { trace_print_argument = argument; }

static uint64_t dispatch_named_import_body(uint32_t import_id, const char *name,
                                      const uint32_t *arguments,
                                      uint32_t return_address)
{
    /* $UNIX2003 selects the modern POSIX behavior already provided by our
       host-backed functions. It does not change the i386 data layout (unlike
       $INODE64, which must retain its distinct stat/dirent conversion). */
    size_t name_length=strlen(name);
    static const char unix_suffix[]="$UNIX2003";
    if(name_length>sizeof(unix_suffix)-1 && name_length<256 &&
       !strcmp(name+name_length-(sizeof(unix_suffix)-1),unix_suffix)) {
        char base[256];memcpy(base,name,name_length-(sizeof(unix_suffix)-1));
        base[name_length-(sizeof(unix_suffix)-1)]=0;
        return dispatch_named_import(import_id,base,arguments,return_address);
    }
    dispatch_name_length = strlen(name);
    dispatch_name_matched = false;
    if (!strcmp(name, kAsphalt6CommitCallback))
        return asphalt6_controller32_commit(arguments);
    if (!strcmp(name, kAsphalt6SteeringCallback))
        return compat_runtime32_return_float(asphalt6_controller32_steer(arguments));
    if (!strcmp(name, "_lp32_trace_print")) {
        uint32_t text = arguments[trace_print_argument];
        if (text >= 0x1000 && text < 0x7f000000) {
            fprintf(stderr, "GAMEPRINT %.*s", 1000, (const char *)(uintptr_t)text);
            size_t length = strnlen((const char *)(uintptr_t)text, 1000);
            if (!length || ((const char *)(uintptr_t)text)[length - 1] != '\n') fputc('\n', stderr);
        }
        return 0;
    }
    if (name[1] == '_' || !strncmp(name, "_objc_exception_", 16)) {
        uint64_t exception_result;
        if (cxx_exception_bridge32_dispatch(name, arguments, return_address, &exception_result))
            return exception_result;
    }
    if (!strcmp(name, "_lp32_mw2_command_overflow")) {
        /* Replaces SV_AddServerCommand's two Com_Error calls: print the
           commands still waiting for the client, then continue into
           Com_Error with the original arguments, as if called directly. */
        struct guest_resume_context frame;
        if (!compat_runtime32_import_frame(&frame) || !mw2_command_overflow_target) {
            compat_runtime32_trap_import(name, arguments, return_address);
            return 0;
        }
        const uint32_t *caller = (const void *)(uintptr_t)frame.ebp;
        uint32_t client = frame.ebp >= 0x1000 ? caller[2] : 0;
        const char *adding = frame.ebp >= 0x1000 && caller[3] >= 0x1000 ? (const char *)(uintptr_t)caller[3] : "";
        if (client >= 0x1000) {
            const uint32_t *fields = (const void *)(uintptr_t)client;
            uint32_t used = fields[1], sequence = fields[2], acknowledge = fields[3];
            fprintf(stderr, "compat32: server command buffer full at 0x%08" PRIx32 ": %u bytes used, %d commands pending "
                    "(sequence %u, acknowledged %u); adding %zu bytes: %.300s\n", return_address, used,
                    (int)(sequence - acknowledge), sequence, acknowledge, strlen(adding), adding);
            const char *text = (const char *)(uintptr_t)(client + 0x10);
            const uint32_t *offsets = (const void *)(uintptr_t)(client + 0x1010);
            for (uint32_t index = acknowledge + 1; (int32_t)(sequence - index) >= 0 && index - acknowledge <= 128; ++index) {
                uint32_t offset = offsets[index & 0x7f];
                if (offset >= 0x1000) continue;
                fprintf(stderr, "compat32:   pending %u (%zu bytes): %.200s\n", index,
                        strnlen(text + offset, 0x1000 - offset), text + offset);
            }
        }
        fflush(stderr);
        frame.esp -= 4; /* the call's return address is still in place */
        frame.eip = mw2_command_overflow_target;
        compat_runtime32_resume_guest(&frame);
        return 0;
    }
    if (!strcmp(name, "_puts")) {
        /* Aspyr prints the Steam proof-of-purchase key before storing it.
           Keep the license callback intact without recording its secret. */
        if (lp32_title_is_cod_carbon(lp32_profile()->title) &&
            !lp32_profile()->license_log_return_address)
            return 0;
        if (return_address && return_address == lp32_profile()->license_log_return_address)
            return (uint32_t)puts("compat32: Steam license key received");
        return (uint32_t)puts((const char *)(uintptr_t)arguments[0]);
    }
    const uint8_t *stage = import_stage_slot(import_id);
    if (stage && *stage >= kImportStageAudio) {
        return dispatch_bridge_stages(import_id, name, arguments,
                                      return_address, *stage);
    }
    uint64_t blocks_result;
    if (strncmp(name, "__Block_", 8) == 0 && blocks_bridge32_dispatch(name, arguments, &blocks_result)) return blocks_result;
    uint64_t libcpp_result;
    if ((!strncmp(name,"__ZNSt3__1",9) || !strncmp(name,"_lp32_future_",13)) &&
        libcpp_future_bridge32_dispatch(name,arguments,&libcpp_result))return libcpp_result;
    if (name[1]=='l' && tlv_bridge32_dispatch(name,arguments,&libcpp_result)) return libcpp_result;
    if ((!strncmp(name, "__ZN", 4) || !strncmp(name, "_lp32_libcpp_", 13)) &&
        libcpp_stream_bridge32_dispatch(name, arguments, &libcpp_result)) return libcpp_result;
    if (strncmp(name, "__ZN", 4) == 0 && libcpp_bridge32_dispatch(name, arguments, &libcpp_result)) return libcpp_result;
    uint64_t qt_result;
    if (quicktime_bridge32_dispatch(name, arguments, &qt_result)) return qt_result;
    uint64_t crypto_result;
    if (crypto_bridge32_dispatch(name, arguments, &crypto_result)) return crypto_result;
    if (!strncmp(name,"_iconv",6) && iconv_bridge32_dispatch(name, arguments, &crypto_result)) return crypto_result;
    uint64_t carbon_result;
    if (carbon_bridge32_dispatch(name, arguments, &carbon_result)) return carbon_result;
    uint64_t stl_result;
    if (stl_bridge32_dispatch(name, arguments, &stl_result)) return stl_result;
    uint64_t resource_result;
    if (resource_bridge32_dispatch(name, arguments, &resource_result)) return resource_result;
    uint64_t steam_result;
    if (steam_bridge32_dispatch(name, arguments, &steam_result)) return steam_result;
    if (import_is(name, "_lp32_context_signal_mask")) {
        sigset_t *mask = (void *)(uintptr_t)(arguments[0] + 32);
        return (uint32_t)(arguments[1] ? pthread_sigmask(SIG_SETMASK, mask, NULL) :
                                        pthread_sigmask(SIG_SETMASK, NULL, mask));
    }
    if (import_is(name, "_wctob")) return (uint32_t)wctob((wint_t)arguments[0]);
    if (import_is(name, "_btowc")) return (uint32_t)btowc((int)arguments[0]);
    if (import_is(name, "_X2Fix")) {
        double value; memcpy(&value, arguments, sizeof(value)); value *= 65536.0;
        return (uint32_t)(value >= INT32_MAX ? INT32_MAX : value <= INT32_MIN ? INT32_MIN : (int32_t)llrint(value));
    }
    if (import_is(name, "_Fix2X")) return return_guest_double((double)(int32_t)arguments[0] / 65536.0);
    if (import_is(name, "_system")) {
        return (uint32_t)system((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "__keymgr_get_and_lock_processwide_ptr_2")) {
        uint32_t key = arguments[0];
        uint32_t *output = (void *)(uintptr_t)arguments[1];
        uint32_t value = key < 64 ? keymgr_slots[key] : 0;
        if (output) *output = value;
        /* A nonzero status cleanly disables the obsolete libgcc facility. */
        return value ? 0 : 1;
    }
    if (import_is(name, "__NSGetExecutablePath"))
        return (uint32_t)_NSGetExecutablePath((void *)(uintptr_t)arguments[0], (void *)(uintptr_t)arguments[1]);
    uint64_t socket_result;
    if (socket_bridge32_dispatch(name, arguments, &socket_result)) return socket_result;
    uint64_t zlib_result;
    if (zlib_bridge32_dispatch(name, arguments, &zlib_result)) return zlib_result;
    if (sdl_bridge32_dispatch(name, arguments, &zlib_result)) return zlib_result;
    uint64_t dylib_result;
    if (guest_dyld32_dispatch(name, arguments, &dylib_result)) return dylib_result;
    if (dlfcn_bridge32_dispatch(name,arguments,&dylib_result)) return dylib_result;
    /* zlib uLong is 32 bits in the guest, including checksum results. */
    if (import_is(name, "_adler32"))
        return (uint32_t)adler32(arguments[0], (void *)(uintptr_t)arguments[1], arguments[2]);
    if (import_is(name, "_crc32"))
        return (uint32_t)crc32(arguments[0], (void *)(uintptr_t)arguments[1], arguments[2]);
    if (import_is(name, kControllerGlyphCallbackName)) {
        controller_bridge32_button_glyph(arguments[0],
                                         (char *)(uintptr_t)arguments[1]);
        return arguments[1];
    }
    if (import_is(name, kFeralMainEntryCallbackName)) {
        fprintf(stderr, "compat32: Feral launcher (Content.loader) bypassed; "
                        "entering the game directly\n");
        return 1;
    }

    /*
     * The game only reaches exit/_exit after its own quit confirmation and
     * save handling.  Returning through the compatibility thunk would unmap
     * the guest while CoreAudio callback threads still reference it, which
     * leaves AppKit visibly frozen.  Process exit is the correct boundary for
     * those quarantined legacy audio objects and does not run incompatible
     * host/guest teardown callbacks.
     */
    if (import_is(name, "_exit") || import_is(name, "__exit")) {
        fprintf(stderr, "compat32: guest requested process exit status=%d\n",
                (int)arguments[0]);
        compat_runtime32_heap_report("guest-exit");
        cg_report("guest-exit");
        fflush(NULL);
        _Exit((int)arguments[0]);
    }

    if (import_is(name, "_cgCreateContext")) {
        typedef void *(*function_type)(void);
        function_type function = (function_type)cg_symbol("cgCreateContext");
        return function ? guest_handle_for_cg_object(function()) : 0;
    }
    if (import_is(name, "_cgCreateProgram")) {
        typedef void *(*function_type)(void *, int, const char *, int,
                                       const char *, const char **);
        function_type function = (function_type)cg_symbol("cgCreateProgram");
        if (!function) return 0;
        const uint32_t *guest_arguments =
            (const void *)(uintptr_t)arguments[5];
        const char *host_arguments[65];
        const char **host_argument_pointer = NULL;
        if (guest_arguments) {
            size_t count = 0;
            while (count < 64 && guest_arguments[count]) {
                host_arguments[count] =
                    (const char *)(uintptr_t)guest_arguments[count];
                ++count;
            }
            host_arguments[count] = NULL;
            host_argument_pointer = host_arguments;
        }
        void *program = function(
            cg_object_for_guest(arguments[0]), (int)arguments[1],
            (const char *)(uintptr_t)arguments[2], (int)arguments[3],
            (const char *)(uintptr_t)arguments[4], host_argument_pointer);
        uint32_t handle = guest_handle_for_cg_object(program);
        __atomic_fetch_add(handle ? &cg_programs_created : &cg_null_programs,
                           1, __ATOMIC_RELAXED);
        if (getenv("LP32_TRACE_CG")) {
            const char *source = (const char *)(uintptr_t)arguments[2];
            const char *entry = (const char *)(uintptr_t)arguments[4];
            fprintf(stderr, "compat32: cgCreateProgram type=%u profile=%u entry=%s "
                    "source=%zu bytes args=", arguments[1], arguments[3],
                    entry ? entry : "(null)", source ? strlen(source) : 0);
            for (size_t index = 0; host_argument_pointer &&
                 host_argument_pointer[index]; ++index) {
                fprintf(stderr, "%s%s", index ? " " : "", host_argument_pointer[index]);
            }
            fprintf(stderr, " -> %#x\n", handle);
            if (!handle) {
                typedef const char *(*listing_type)(void *);
                listing_type listing = (listing_type)cg_symbol("cgGetLastListing");
                const char *text = listing ? listing(cg_object_for_guest(arguments[0])) : NULL;
                fprintf(stderr, "compat32: cg listing: %s\n", text ? text : "(none)");
            }
        }
        return handle;
    }
    if (import_is(name, "_cgDestroyParameter") ||
        import_is(name, "_cgDestroyProgram")) {
        typedef void (*function_type)(void *);
        const char *symbol = import_is(name, "_cgDestroyParameter") ?
                             "cgDestroyParameter" : "cgDestroyProgram";
        function_type function = (function_type)cg_symbol(symbol);
        if (function) function(cg_object_for_guest(arguments[0]));
        if (import_is(name, "_cgDestroyProgram")) {
            release_cg_program_strings(arguments[0]);
            __atomic_fetch_add(&cg_programs_destroyed, 1, __ATOMIC_RELAXED);
        }
        return 0;
    }
    if (import_is(name, "_cgIsParameterUsed")) {
        typedef int (*function_type)(void *, void *);
        function_type function = (function_type)cg_symbol("cgIsParameterUsed");
        return function ? (uint32_t)function(cg_object_for_guest(arguments[0]),
                                             cg_object_for_guest(arguments[1])) : 0;
    }
    if (import_is(name, "_cgGetArrayParameter")) {
        typedef void *(*function_type)(void *, int);
        function_type function = (function_type)cg_symbol("cgGetArrayParameter");
        return function ? guest_handle_for_cg_object(function(
                              cg_object_for_guest(arguments[0]),
                              (int)arguments[1])) : 0;
    }
    if (import_is(name, "_cgGetFirstParameter")) {
        typedef void *(*function_type)(void *, int);
        function_type function = (function_type)cg_symbol("cgGetFirstParameter");
        uint32_t handle = function ? guest_handle_for_cg_object(function(
                                         cg_object_for_guest(arguments[0]),
                                         (int)arguments[1])) : 0;
        if (!handle) {
            __atomic_fetch_add(&cg_null_first_parameters, 1, __ATOMIC_RELAXED);
        }
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: cgGetFirstParameter program=%#x ns=%u -> %#x\n",
                    arguments[0], arguments[1], handle);
        }
        return handle;
    }
    if (import_is(name, "_cgGetNamedParameter")) {
        typedef void *(*function_type)(void *, const char *);
        function_type function = (function_type)cg_symbol("cgGetNamedParameter");
        uint32_t handle = function ? guest_handle_for_cg_object(function(
                              cg_object_for_guest(arguments[0]),
                              (const char *)(uintptr_t)arguments[1])) : 0;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: cgGetNamedParameter program=%#x %s -> %#x\n",
                    arguments[0], (const char *)(uintptr_t)arguments[1], handle);
        }
        return handle;
    }
    if (import_is(name, "_cgGetNextParameter") ||
        import_is(name, "_cgGetFirstStructParameter")) {
        typedef void *(*function_type)(void *);
        function_type function = (function_type)cg_symbol(name + 1);
        uint32_t handle = function ? guest_handle_for_cg_object(function(
                              cg_object_for_guest(arguments[0]))) : 0;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: %s %#x -> %#x\n", name + 1, arguments[0],
                    handle);
        }
        return handle;
    }
    if (import_is(name, "_cgGetLastListing")) {
        typedef const char *(*function_type)(void *);
        function_type function = (function_type)cg_symbol("cgGetLastListing");
        return function ? guest_copy_nullable_cstring(function(
                              cg_object_for_guest(arguments[0]))) : 0;
    }
    if (import_is(name, "_cgGetParameterName") ||
        import_is(name, "_cgGetParameterSemantic")) {
        typedef const char *(*function_type)(void *);
        function_type function = (function_type)cg_symbol(name + 1);
        const char *parameter_name = function ?
            function(cg_object_for_guest(arguments[0])) : NULL;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: %s %#x -> %s\n", name + 1, arguments[0],
                    parameter_name ? parameter_name : "(null)");
        }
        return guest_copy_nullable_cstring(parameter_name);
    }
    if (import_is(name, "_cgGetProgramString")) {
        typedef const char *(*function_type)(void *, int);
        function_type function = (function_type)cg_symbol("cgGetProgramString");
        return function ? guest_cg_string_owned(arguments[0], function(
                              cg_object_for_guest(arguments[0]),
                              (int)arguments[1])) : 0;
    }
    if (import_is(name, "_cgGetString") || import_is(name, "_cgGetErrorString")) {
        typedef const char *(*function_type)(int);
        function_type function = (function_type)cg_symbol(name + 1);
        return function ? guest_copy_nullable_cstring(function(
                              (int)arguments[0])) : 0;
    }
    if (import_is(name, "_cgGetParameterClass") ||
        import_is(name, "_cgGetParameterResourceIndex") ||
        import_is(name, "_cgGetParameterType") ||
        import_is(name, "_cgGetParameterDirection") ||
        import_is(name, "_cgIsParameter") ||
        import_is(name, "_cgIsParameterReferenced")) {
        typedef int (*function_type)(void *);
        const char *symbol = name + 1;
        function_type function = (function_type)cg_symbol(symbol);
        uint32_t value = function ?
            (uint32_t)function(cg_object_for_guest(arguments[0])) : 0;
        if (getenv("LP32_TRACE_CG")) {
            fprintf(stderr, "compat32: %s %#x -> %u\n", symbol, arguments[0], value);
        }
        return value;
    }
    if (import_is(name, "__keymgr_get_and_lock_processwide_ptr")) {
        uint32_t key = arguments[0];
        return key < 64 ? keymgr_slots[key] : 0;
    }
    if (import_is(name, "__keymgr_set_and_unlock_processwide_ptr")) {
        uint32_t key = arguments[0];
        if (key < 64) keymgr_slots[key] = arguments[1];
        return 0;
    }
    if (import_is(name, "___keymgr_dwarf2_register_sections")) return 0;
    if (import_is(name, "_atof"))
        return return_guest_double(strtod((const char *)(uintptr_t)arguments[0], NULL));
    if (import_is(name, "_access")) return (uint32_t)access((const char *)(uintptr_t)arguments[0], (int)arguments[1]);
    if (import_is(name, "_unlink")) return (uint32_t)unlink((const char *)(uintptr_t)arguments[0]);
    if (import_is(name, "_pthread_yield_np")) { sched_yield(); return 0; }
    if (import_is(name, "_getcwd")) {
        char path[PATH_MAX];
        if (!getcwd(path, sizeof(path))) return 0;
        if (!arguments[0]) return compat_runtime32_copy_cstring(path);
        if (strlen(path) + 1 > arguments[1]) { errno = ERANGE; return 0; }
        memcpy((void *)(uintptr_t)arguments[0], path, strlen(path) + 1);
        return arguments[0];
    }
    if (import_is(name, "_lround") || import_is(name, "_lroundf")) {
        double value = import_is(name, "_lroundf") ? guest_float(arguments[0]) : guest_double(arguments);
        double rounded = round(value);
        if (!isfinite(rounded) || rounded < INT32_MIN || rounded > INT32_MAX) {
            errno = EDOM;
            return (uint32_t)INT32_MIN;
        }
        return (uint32_t)(int32_t)rounded;
    }

    if (import_is(name, "_getrlimit") || import_is(name, "_getrlimit$UNIX2003") ||
        import_is(name, "_setrlimit") || import_is(name, "_setrlimit$UNIX2003")) {
        struct rlimit limit;
        bool get = !strncmp(name, "_getrlimit", 10);
        if (!get) memcpy(&limit, (const void *)(uintptr_t)arguments[1], sizeof(limit));
        int status = get ? getrlimit((int)arguments[0], &limit) : setrlimit((int)arguments[0], &limit);
        if (get && !status) memcpy((void *)(uintptr_t)arguments[1], &limit, sizeof(limit));
        return (uint32_t)status;
    }

    if (import_is(name, "_getopt") || import_is(name, "_getopt$UNIX2003")) {
        if (arguments[0] > 4096) return (uint32_t)-1;
        int argc = (int)arguments[0];
        const uint32_t *guest_argv = (const void *)(uintptr_t)arguments[1];
        char **argv = calloc((size_t)argc + 1, sizeof(*argv));
        if (!argv) return (uint32_t)-1;
        for (int i = 0; i < argc; ++i) argv[i] = (void *)(uintptr_t)guest_argv[i];
        int option = getopt(argc, argv, (const char *)(uintptr_t)arguments[2]);
        free(argv);
        return (uint32_t)option;
    }

    if (import_is(name, "___fixunsdfdi") || import_is(name, "___fixunssfdi")) {
        double value = import_is(name, "___fixunsdfdi") ?
            guest_double(arguments) : guest_float(arguments[0]);
        return !(value > 0) ? 0 : value >= 0x1p64 ? UINT64_MAX : (uint64_t)value;
    }
    if (import_is(name, "_memset_pattern16")) {
        memset_pattern16((void *)(uintptr_t)arguments[0],
                         (const void *)(uintptr_t)arguments[1], arguments[2]);
        return 0;
    }
    if (import_is(name, "_malloc")) {
        return guest_allocate_at(arguments[0], false, return_address);
    }
    if (import_is(name, "__Znwm") || import_is(name, "__Znam")) {
        return guest_allocate_at(arguments[0], false, return_address);
    }
    if (import_is(name, "_valloc")) {
        /* Miles copies plugin code into valloc memory, marks it RWX, and later
           frees it, so it must be a normal heap block. _mprotect widens the
           unaligned range to whole pages. */
        return guest_allocate_at(arguments[0], false, return_address);
    }
    if (import_is(name, "_calloc")) {
        uint64_t size = (uint64_t)arguments[0] * arguments[1];
        return size <= SIZE_MAX ?
            guest_allocate_at((size_t)size, true, return_address) : 0;
    }
    if (import_is(name, "_realloc")) {
        return guest_reallocate(arguments[0], arguments[1], return_address);
    }
    if (import_is(name, "_malloc_size")) {
        /* Usable size of a live guest block (its capacity); 0 for any
           pointer the guest heap did not hand out, as on macOS. */
        pthread_mutex_lock(&guest_heap_lock);
        uint32_t *metadata = NULL;
        bool already_free = false;
        uint32_t size = arguments[0] && guest_heap_validate_header_locked(arguments[0], &metadata, &already_free) ?
            metadata[2] : 0;
        pthread_mutex_unlock(&guest_heap_lock);
        return size;
    }
    if (import_is(name, "_free") || import_is(name, "__ZdlPv") ||
        import_is(name, "__ZdaPv")) {
        guest_deallocate(arguments[0]);
        return 0;
    }
    if (import_is(name, "__ZNSt8ios_base4InitC1Ev") ||
        import_is(name, "__ZNSt8ios_base4InitD1Ev") ||
        import_is(name, "__ZNSaIcEC1Ev") ||
        import_is(name, "__ZNSaIcED1Ev") ||
        import_is(name, "__ZNSaIcEC2Ev") ||
        import_is(name, "__ZNSaIcED2Ev")) {
        return 0;
    }
    if (import_is(name,"___dynamic_cast")) return fast_dynamic_cast(arguments, return_address);
    if (import_is(name, "__ZNSsC2Ev")) {
        guest_string_construct(arguments[0], "", 0, return_address);
        return arguments[0];
    }
    if (import_is(name, "__ZNSsC1EPKcRKSaIcE")) {
        const char *source = (const char *)(uintptr_t)arguments[1];
        guest_string_construct(arguments[0], source,
                               source ? strlen(source) : 0, return_address);
        return arguments[0];
    }
    if (import_is(name, "__ZNSsC1EPKcmRKSaIcE")) {
        return guest_string_construct(arguments[0],(const char *)(uintptr_t)arguments[1],arguments[2],return_address);
    }
    if (import_is(name, "__ZNSsC1ERKSsmm")) {
        size_t length=guest_string_length(arguments[1]),pos=arguments[2],n=arguments[3];
        if(pos>length)return 0;if(n>length-pos)n=length-pos;
        return guest_string_construct(arguments[0],guest_string_data(arguments[1])+pos,n,return_address);
    }
    if (import_is(name, "__ZNSs4_Rep10_M_destroyERKSaIcE")) {
        if(arguments[0]>=guest_heap_base)guest_deallocate(arguments[0]);return 0;
    }
    if (import_is(name, "__ZN9__gnu_cxx18__exchange_and_addEPVii"))
        return __atomic_fetch_add((uint32_t *)(uintptr_t)arguments[0],arguments[1],__ATOMIC_SEQ_CST);
    if (import_is(name, "__ZNSs6assignEPKcm"))
        return guest_string_assign(arguments[0],(const char *)(uintptr_t)arguments[1],arguments[2],return_address);
    if (import_is(name, "__ZNSs6appendEPKcm") || import_is(name, "__ZNSs6appendERKSs") ||
        import_is(name, "__ZNSs6insertEmPKcm") || import_is(name, "__ZNSs7replaceEmmPKcm") ||
        import_is(name, "__ZNSs9_M_mutateEmmm")) {
        const char *old=guest_string_data(arguments[0]);size_t length=guest_string_length(arguments[0]);
        size_t pos=length,removed=0,added=arguments[2];const char *bytes=(const void *)(uintptr_t)arguments[1];
        bool mutate=import_is(name,"__ZNSs9_M_mutateEmmm");
        if(import_is(name,"__ZNSs6appendERKSs")){bytes=guest_string_data(arguments[1]);added=guest_string_length(arguments[1]);}
        if(import_is(name,"__ZNSs6insertEmPKcm")){pos=arguments[1];bytes=(const void *)(uintptr_t)arguments[2];added=arguments[3];}
        if(import_is(name,"__ZNSs7replaceEmmPKcm")){pos=arguments[1];removed=arguments[2];bytes=(const void *)(uintptr_t)arguments[3];added=arguments[4];}
        if(mutate){pos=arguments[1];removed=arguments[2];added=arguments[3];bytes=NULL;}
        if(import_is(name,"__ZNSs7replaceEmmPKcm") && pos<=length && removed>length-pos)removed=length-pos;
        if(pos>length||removed>length-pos||added>UINT32_MAX-13-length+removed)return 0;
        size_t new_length=length-removed+added;
        char *buffer=malloc(new_length+1);if(!buffer)return 0;
        memcpy(buffer,old,pos);if(bytes)memcpy(buffer+pos,bytes,added);else memset(buffer+pos,0,added);
        memcpy(buffer+pos+added,old+pos+removed,length-pos-removed);
        uint32_t result=guest_string_assign(arguments[0],buffer,new_length,return_address);free(buffer);return result;
    }
    if (import_is(name, "__ZNSs7reserveEm") || import_is(name, "__ZNSs12_M_leak_hardEv")) {
        uint32_t old=*(uint32_t *)(uintptr_t)arguments[0];size_t length=guest_string_length(arguments[0]);
        bool leak=import_is(name,"__ZNSs12_M_leak_hardEv");
        size_t capacity=leak?length:arguments[1];if(capacity<length)capacity=length;
        if(capacity>UINT32_MAX-13)return 0;
        uint32_t allocation=guest_allocate(12+capacity+1,false);if(!allocation)return 0;
        uint32_t *rep=(void *)(uintptr_t)allocation;rep[0]=(uint32_t)length;rep[1]=(uint32_t)capacity;rep[2]=leak?UINT32_MAX:0;
        memcpy((void *)(uintptr_t)(allocation+12),guest_string_data(arguments[0]),length+1);
        *(uint32_t *)(uintptr_t)arguments[0]=allocation+12;if(old>=12)guest_string_dispose_rep(old-12);return 0;
    }
    if (import_is(name, "__ZNKSs4findEcm") || import_is(name, "__ZNKSs5rfindEcm") ||
        import_is(name, "__ZNKSs4findEPKcmm") || import_is(name, "__ZNKSs13find_first_ofEPKcmm")) {
        const char *text=guest_string_data(arguments[0]);size_t length=guest_string_length(arguments[0]),pos=arguments[2];
        if(import_is(name,"__ZNKSs5rfindEcm")){
            if(!length)return UINT32_MAX;if(pos>=length)pos=length-1;
            do{if(text[pos]==(char)arguments[1])return (uint32_t)pos;}while(pos--);return UINT32_MAX;
        }
        if(pos>length)return UINT32_MAX;
        if(import_is(name,"__ZNKSs4findEcm")){const char *found=memchr(text+pos,(char)arguments[1],length-pos);return found?(uint32_t)(found-text):UINT32_MAX;}
        const char *needle=(const void *)(uintptr_t)arguments[1];size_t n=arguments[3];
        if(import_is(name,"__ZNKSs13find_first_ofEPKcmm")){for(size_t i=pos;i<length;i++)if(memchr(needle,text[i],n))return (uint32_t)i;return UINT32_MAX;}
        if(n>length-pos)return UINT32_MAX;
        for(size_t i=pos;i<=length-n;i++)if(!memcmp(text+i,needle,n))return (uint32_t)i;
        return UINT32_MAX;
    }
    if (import_is(name, "__ZNSsC1ERKSs")) {
        const char *source = guest_string_data(arguments[1]);
        guest_string_construct(arguments[0], source,
                               guest_string_length(arguments[1]),
                               return_address);
        return arguments[0];
    }
    if (import_is(name, "__ZNSsD2Ev") || import_is(name, "__ZNSsD1Ev")) {
        guest_string_dispose_object(arguments[0]);
        return 0;
    }
    if (import_is(name, "__ZNSs4_Rep10_M_disposeERKSaIcE")) {
        guest_string_dispose_rep(arguments[0]);
        return 0;
    }
    if (import_is(name, "__ZNSs6assignEPKc")) {
        const char *source = (const char *)(uintptr_t)arguments[1];
        return guest_string_assign(arguments[0], source,
                                   source ? strlen(source) : 0,
                                   return_address);
    }
    if (import_is(name, "__ZNSs6assignERKSs")) {
        const char *source = guest_string_data(arguments[1]);
        return guest_string_assign(arguments[0], source,
                                   guest_string_length(arguments[1]),
                                   return_address);
    }
    if (import_is(name, "__ZNSs9push_backEc")) {
        const char *source = guest_string_data(arguments[0]);
        size_t length = guest_string_length(arguments[0]);
        uint32_t old_data = arguments[0] ?
            *(const uint32_t *)(uintptr_t)arguments[0] : 0;
        uint32_t data = guest_string_make(source, length + 1,
                                          return_address);
        if (!data) return 0;
        ((char *)(uintptr_t)data)[length] = (char)arguments[1];
        ((char *)(uintptr_t)data)[length + 1] = '\0';
        *(uint32_t *)(uintptr_t)arguments[0] = data;
        if (old_data >= 12 && old_data != data) {
            guest_string_dispose_rep(old_data - 12);
        }
        return arguments[0];
    }
    if (import_is(name, "__ZNSsixEm")) {
        uint32_t data = arguments[0] ? *(uint32_t *)(uintptr_t)arguments[0] : 0;
        return data ? data + arguments[1] : 0;
    }
    if (import_is(name, "__ZNKSs7compareEPKc")) {
        const char *right = (const char *)(uintptr_t)arguments[1];
        return (uint32_t)strcmp(guest_string_data(arguments[0]), right ? right : "");
    }

    if (import_is(name, "_memcpy") || import_is(name, "_memmove") ||
        import_is(name, "___memcpy_chk")) {
        return fast_memmove(arguments, return_address);
    }
    uint64_t curl_result;
    if (strncmp(name, "_curl_", 6) == 0 && curl_bridge32_dispatch(name, arguments, &curl_result)) return curl_result;
    if (import_is(name, "_OSSpinLockLock") || import_is(name, "_OSSpinLockUnlock")) {
        void (*function)(volatile int32_t *) = dlsym(RTLD_DEFAULT, name + 1);
        if (function) function((void *)(uintptr_t)arguments[0]);
        return 0;
    }
    if (import_is(name, "_OSSpinLockTry")) {
        bool (*function)(volatile int32_t *) = dlsym(RTLD_DEFAULT, name + 1);
        return function && function((void *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_getsegbyname")) {
        const char *segment_name = (const void *)(uintptr_t)arguments[0];
        const struct mach_header *header = current_image->header;
        const uint8_t *cursor = (const void *)(header + 1);
        for (uint32_t i = 0; i < header->ncmds; ++i) {
            const struct load_command *command = (const void *)cursor;
            if (command->cmd == LC_SEGMENT) {
                const struct segment_command *segment = (const void *)cursor;
                if (strncmp(segment->segname, segment_name, sizeof(segment->segname)) == 0)
                    return (uint32_t)(uintptr_t)segment;
            }
            cursor += command->cmdsize;
        }
        return 0;
    }
    if (import_is(name, "_bzero") || import_is(name, "___bzero")) {
        memset((void *)(uintptr_t)arguments[0], 0, arguments[1]);
        return 0;
    }
    if (import_is(name, "_memset") || import_is(name, "___memset_chk")) {
        return fast_memset(arguments, return_address);
    }
    if (import_is(name, "_strpbrk")) return (uint32_t)(uintptr_t)strpbrk(
        (const char *)(uintptr_t)arguments[0],(const char *)(uintptr_t)arguments[1]);
    if (import_is(name, "_strspn")) return (uint32_t)strspn(
        (const char *)(uintptr_t)arguments[0],(const char *)(uintptr_t)arguments[1]);
    if (import_is(name, "_strcspn")) return (uint32_t)strcspn(
        (const char *)(uintptr_t)arguments[0],(const char *)(uintptr_t)arguments[1]);
    if (import_is(name, "_memcmp")) {
        return fast_memcmp(arguments, return_address);
    }
    if (import_is(name, "_memchr")) {
        return (uint32_t)(uintptr_t)memchr((const void *)(uintptr_t)arguments[0],
                                           (int)arguments[1], arguments[2]);
    }
    if (import_is(name, "_strlen")) {
        return fast_strlen(arguments, return_address);
    }
    if (import_is(name, "_strcmp")) {
        return fast_strcmp(arguments, return_address);
    }
    if (import_is(name, "_strncmp")) {
        return (uint32_t)strncmp((const char *)(uintptr_t)arguments[0],
                                 (const char *)(uintptr_t)arguments[1], arguments[2]);
    }
    if (import_is(name, "_strcpy")) {
        return fast_strcpy(arguments, return_address);
    }
    if (import_is(name, "___strcpy_chk")) {
        return fast_strcpy(arguments, return_address);
    }
    if (import_is(name, "_strncpy")) {
        return fast_strncpy(arguments, return_address);
    }
    if (import_is(name, "_strcat")) {
        strcat((char *)(uintptr_t)arguments[0],
               (const char *)(uintptr_t)arguments[1]);
        return arguments[0];
    }
    if (import_is(name, "_strcasecmp")) return (uint32_t)strcasecmp((void *)(uintptr_t)arguments[0],(void *)(uintptr_t)arguments[1]);
    if (import_is(name, "_strncasecmp")) return (uint32_t)strncasecmp((void *)(uintptr_t)arguments[0],(void *)(uintptr_t)arguments[1],arguments[2]);
    if (import_is(name, "_strcoll")) return (uint32_t)strcoll((void *)(uintptr_t)arguments[0],(void *)(uintptr_t)arguments[1]);
    if (import_is(name, "_strncat")) {strncat((void *)(uintptr_t)arguments[0],(void *)(uintptr_t)arguments[1],arguments[2]);return arguments[0];}
    if (import_is(name, "_strlcpy") || import_is(name, "___strlcpy_chk")) return (uint32_t)strlcpy((void *)(uintptr_t)arguments[0],(void *)(uintptr_t)arguments[1],arguments[2]);
    if (import_is(name, "_strlcat")) return (uint32_t)strlcat((void *)(uintptr_t)arguments[0],(void *)(uintptr_t)arguments[1],arguments[2]);
    if (import_is(name, "_strnlen")) return (uint32_t)strnlen((void *)(uintptr_t)arguments[0],arguments[1]);
    if (import_is(name, "_strtol") || import_is(name, "_strtoll") ||
        import_is(name, "_strtoul") || import_is(name, "_strtoull")) {
        const char *start = (const char *)(uintptr_t)arguments[0];
        char *end = NULL;
        uint64_t value;
        if (import_is(name, "_strtol") || import_is(name, "_strtoll")) {
            int64_t signed_value = strtoll(start, &end, (int)arguments[2]);
            if (import_is(name, "_strtol")) {
                if (signed_value > INT32_MAX) {signed_value = INT32_MAX; errno = ERANGE;}
                if (signed_value < INT32_MIN) {signed_value = INT32_MIN; errno = ERANGE;}
                value = (uint32_t)signed_value;
            } else value = (uint64_t)signed_value;
        } else {
            value = strtoull(start, &end, (int)arguments[2]);
            if (import_is(name, "_strtoul")) {
                const char *digits = start; while (isspace((unsigned char)*digits)) ++digits;
                bool negative = *digits == '-';
                uint64_t magnitude = negative ? 0 - value : value;
                if (magnitude > UINT32_MAX) {value = UINT32_MAX; errno = ERANGE;}
                else value = (uint32_t)value;
            }
        }
        if (arguments[1]) *(uint32_t *)(uintptr_t)arguments[1] = (uint32_t)(uintptr_t)end;
        return value;
    }
    if (import_is(name, "_strtod") || import_is(name, "_strtof")) {
        char *end = NULL;
        bool single = import_is(name, "_strtof");
        double value = single ? strtof((void *)(uintptr_t)arguments[0], &end) :
            strtod((void *)(uintptr_t)arguments[0], &end);
        if (arguments[1]) *(uint32_t *)(uintptr_t)arguments[1] = (uint32_t)(uintptr_t)end;
        return single ? return_guest_float((float)value) : return_guest_double(value);
    }
    if (import_is(name, "_strchr")) {
        return (uint32_t)(uintptr_t)strchr((const char *)(uintptr_t)arguments[0],
                                           (int)arguments[1]);
    }
    if (import_is(name, "_strrchr")) {
        return (uint32_t)(uintptr_t)strrchr((const char *)(uintptr_t)arguments[0],
                                            (int)arguments[1]);
    }
    if (import_is(name, "_strstr")) {
        return (uint32_t)(uintptr_t)strstr((const char *)(uintptr_t)arguments[0],
                                           (const char *)(uintptr_t)arguments[1]);
    }
    if (import_is(name, "_strtok_r")) {
        uint32_t *guest_state = (void *)(uintptr_t)arguments[2];
        char *host_state = (char *)(uintptr_t)*guest_state;
        char *token = strtok_r((char *)(uintptr_t)arguments[0],
                              (const char *)(uintptr_t)arguments[1], &host_state);
        *guest_state = (uint32_t)(uintptr_t)host_state;
        return (uint32_t)(uintptr_t)token;
    }
    if (import_is(name, "_strtok")) {
        char *string = (char *)(uintptr_t)arguments[0];
        const char *delimiters = (const char *)(uintptr_t)arguments[1];
        if (!delimiters) return 0;
        return (uint32_t)(uintptr_t)strtok_r(string, delimiters,
                                            &guest_strtok_state);
    }
    if (import_is(name, "_strdup")) {
        const char *source = (const char *)(uintptr_t)arguments[0];
        if (!source) return 0;
        size_t size = strlen(source) + 1;
        uint32_t copy = guest_allocate_at(size, false, return_address);
        if (copy) memcpy((void *)(uintptr_t)copy, source, size);
        return copy;
    }
    if (import_is(name, "_copyfile")) {
        if(arguments[2]) {errno=ENOTSUP;return (uint32_t)-1;}
        return (uint32_t)copyfile((const char *)(uintptr_t)arguments[0],
            (const char *)(uintptr_t)arguments[1],NULL,arguments[3]);
    }
    if (import_is(name, "_realpath") || import_is(name, "_realpath$DARWIN_EXTSN")) {
        char *path=realpath((const char *)(uintptr_t)arguments[0],NULL);
        if(!path)return 0;
        uint32_t result=arguments[1];
        if(result)strcpy((char *)(uintptr_t)result,path);
        else result=compat_runtime32_copy_cstring(path);
        free(path);return result;
    }
    if (import_is(name, "_stat$INODE64") || import_is(name, "_stat64") ||
        import_is(name, "_lstat$INODE64") || import_is(name, "_lstat64") ||
        import_is(name, "_fstat$INODE64") || import_is(name, "_fstat64")) {
        struct stat host;
        int status = name[1]=='f' ? fstat((int)arguments[0],&host) :
            name[1]=='l' ? lstat((const char *)(uintptr_t)arguments[0],&host) :
            stat((const char *)(uintptr_t)arguments[0],&host);
        if(status==0) {
            struct guest_stat_inode64 guest={.device=host.st_dev,.mode=host.st_mode,
                .links=host.st_nlink,.inode=host.st_ino,.uid=host.st_uid,.gid=host.st_gid,
                .rdevice=host.st_rdev,.size=host.st_size,.blocks=host.st_blocks,
                .block_size=host.st_blksize,.flags=host.st_flags,.generation=host.st_gen,
                .access={(int32_t)host.st_atimespec.tv_sec,(int32_t)host.st_atimespec.tv_nsec},
                .modify={(int32_t)host.st_mtimespec.tv_sec,(int32_t)host.st_mtimespec.tv_nsec},
                .change={(int32_t)host.st_ctimespec.tv_sec,(int32_t)host.st_ctimespec.tv_nsec},
                .birth={(int32_t)host.st_birthtimespec.tv_sec,(int32_t)host.st_birthtimespec.tv_nsec}};
            memcpy((void *)(uintptr_t)arguments[1],&guest,sizeof(guest));
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_stat")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        struct guest_stat32 *guest = (void *)(uintptr_t)arguments[1];
        struct stat host;
        int status = stat(path, &host);
        if (status == 0 && guest) {
            memset(guest, 0, sizeof(*guest));
            guest->st_dev = host.st_dev;
            guest->st_ino = (uint32_t)host.st_ino;
            guest->st_mode = host.st_mode;
            guest->st_nlink = host.st_nlink;
            guest->st_uid = host.st_uid;
            guest->st_gid = host.st_gid;
            guest->st_rdev = host.st_rdev;
            guest->st_atime_sec = (int32_t)host.st_atimespec.tv_sec;
            guest->st_atime_nsec = (int32_t)host.st_atimespec.tv_nsec;
            guest->st_mtime_sec = (int32_t)host.st_mtimespec.tv_sec;
            guest->st_mtime_nsec = (int32_t)host.st_mtimespec.tv_nsec;
            guest->st_ctime_sec = (int32_t)host.st_ctimespec.tv_sec;
            guest->st_ctime_nsec = (int32_t)host.st_ctimespec.tv_nsec;
            guest->st_size = host.st_size;
            guest->st_blocks = host.st_blocks;
            guest->st_blksize = host.st_blksize;
            guest->st_flags = host.st_flags;
            guest->st_gen = host.st_gen;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_gettimeofday")) {
        struct timeval host;
        int status = gettimeofday(&host, NULL);
        if (status == 0 && arguments[0]) {
            struct guest_timeval32 *guest =
                (void *)(uintptr_t)arguments[0];
            guest->tv_sec = (int32_t)host.tv_sec;
            guest->tv_usec = (int32_t)host.tv_usec;
        }
        /* struct timezone is obsolete; preserve the legacy ABI with zeros. */
        if (status == 0 && arguments[1]) {
            uint32_t *guest_timezone = (void *)(uintptr_t)arguments[1];
            guest_timezone[0] = 0;
            guest_timezone[1] = 0;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_localtime_r") || import_is(name, "_gmtime_r") ||
        import_is(name, "_localtime") || import_is(name, "_gmtime")) {
        const int32_t *guest_time = (const void *)(uintptr_t)arguments[0];
        static _Thread_local uint32_t time_buffer;
        bool reentrant = strstr(name, "_r") != NULL;
        if (!reentrant && !time_buffer) time_buffer = guest_allocate(sizeof(struct guest_tm32), true);
        struct guest_tm32 *guest_result = (void *)(uintptr_t)(reentrant ? arguments[1] : time_buffer);
        if (!guest_time || !guest_result) return 0;
        time_t host_time = (time_t)*guest_time;
        struct tm host_result;
        if (!(strstr(name, "gmtime") ? gmtime_r(&host_time, &host_result) : localtime_r(&host_time, &host_result))) return 0;
        return copy_host_tm_to_guest(&host_result, guest_result);
    }
    if (import_is(name, "_timegm") || import_is(name, "_mktime") || import_is(name, "_strftime")) {
        bool formatting = import_is(name, "_strftime");
        struct guest_tm32 *guest = (void *)(uintptr_t)arguments[formatting ? 3 : 0];
        if (!guest) return formatting ? 0 : UINT32_MAX;
        struct tm host = {
            .tm_sec = guest->tm_sec, .tm_min = guest->tm_min,
            .tm_hour = guest->tm_hour, .tm_mday = guest->tm_mday,
            .tm_mon = guest->tm_mon, .tm_year = guest->tm_year,
            .tm_isdst = guest->tm_isdst, .tm_wday = guest->tm_wday,
            .tm_yday = guest->tm_yday, .tm_gmtoff = guest->tm_gmtoff,
            .tm_zone = (void *)(uintptr_t)guest->tm_zone,
        };
        if (formatting) return (uint32_t)strftime((void *)(uintptr_t)arguments[0], arguments[1], (void *)(uintptr_t)arguments[2], &host);
        time_t value = import_is(name, "_timegm") ? timegm(&host) : mktime(&host);
        copy_host_tm_to_guest(&host, guest);
        if (value < INT32_MIN || value > INT32_MAX) {errno = EOVERFLOW; return UINT32_MAX;}
        return (uint32_t)value;
    }
    if (import_is(name, "_setlocale")) {
        const char *value = setlocale((int)arguments[0], (void *)(uintptr_t)arguments[1]);
        static _Thread_local uint32_t locale_string;
        if (!value) return 0;
        if (arguments[1]) update_guest_lowercase_table();
        if (locale_string) guest_deallocate(locale_string);
        locale_string = compat_runtime32_copy_cstring(value);
        return locale_string;
    }
    if (import_is(name, "_sysctl")) {
        uint32_t *guest_size = (void *)(uintptr_t)arguments[3];
        size_t size = guest_size ? *guest_size : 0;
        int status = sysctl((void *)(uintptr_t)arguments[0], arguments[1],
            (void *)(uintptr_t)arguments[2], guest_size ? &size : NULL,
            (void *)(uintptr_t)arguments[4], arguments[5]);
        if (guest_size) *guest_size = (uint32_t)size;
        return (uint32_t)status;
    }
    if (import_is(name, "_sysctlbyname")) {
        const char *sysctl_name = (const char *)(uintptr_t)arguments[0];
        void *old_value = (void *)(uintptr_t)arguments[1];
        uint32_t *guest_old_size = (void *)(uintptr_t)arguments[2];
        size_t old_size = guest_old_size ? *guest_old_size : 0;
        void *new_value = (void *)(uintptr_t)arguments[3];
        size_t new_size = arguments[4];
        int status = sysctlbyname(sysctl_name, old_value,
                                  guest_old_size ? &old_size : NULL,
                                  new_value, new_size);
        if (guest_old_size) *guest_old_size = (uint32_t)old_size;
        return (uint32_t)status;
    }
    if (import_is(name, "_qsort")) {
        if (arguments[1] < 2 || arguments[2] == 0) return 0;
        if (getenv("LP32_TRACE_QSORT")) {
            fprintf(stderr, "compat32: qsort base=0x%08x count=%u width=%u "
                            "comparator=0x%08x first=0x%08x\n",
                    arguments[0], arguments[1], arguments[2], arguments[3],
                    *(const uint32_t *)(uintptr_t)arguments[0]);
        }
        guest_qsort((void *)(uintptr_t)arguments[0], arguments[1], arguments[2],
                    arguments[3]);
        return 0;
    }
    if (import_is(name, "_asprintf") || import_is(name, "_vasprintf")) {
        uint32_t *output = (void *)(uintptr_t)arguments[0];
        const char *format = (void *)(uintptr_t)arguments[1];
        const uint32_t *values = import_is(name, "_vasprintf") ?
            (void *)(uintptr_t)arguments[2] : arguments + 2;
        if (!output || !format) { errno = EINVAL; return UINT32_MAX; }
        *output = 0;
        int size = guest_vformat(NULL, 0, format, values);
        if (size < 0) return UINT32_MAX;
        uint32_t buffer = guest_allocate((size_t)size + 1, false);
        if (!buffer) { errno = ENOMEM; return UINT32_MAX; }
        int result = guest_vformat((void *)(uintptr_t)buffer, (size_t)size + 1, format, values);
        if (result < 0) { compat_runtime32_deallocate(buffer); return UINT32_MAX; }
        *output = buffer;
        return (uint32_t)result;
    }
    if (import_is(name, "_sprintf")) return guest_printf_family(kGuestSprintf, arguments, return_address);
    if (import_is(name, "_snprintf")) return guest_printf_family(kGuestSnprintf, arguments, return_address);
    if (import_is(name, "_vsprintf")) return guest_printf_family(kGuestVsprintf, arguments, return_address);
    if (import_is(name, "_vsnprintf")) return guest_printf_family(kGuestVsnprintf, arguments, return_address);
    if (import_is(name, "___sprintf_chk")) return guest_printf_family(kGuestSprintfChk, arguments, return_address);
    if (import_is(name, "___vsnprintf_chk")) return guest_printf_family(kGuestVsnprintfChk, arguments, return_address);
    if (import_is(name, "_printf")) return guest_printf_family(kGuestPrintf, arguments, return_address);
    if (import_is(name, "_sscanf") || import_is(name, "_vsscanf")) {
        const char *input = (const void *)(uintptr_t)arguments[0];
        const char *format = (const void *)(uintptr_t)arguments[1];
        const uint32_t *values = import_is(name, "_vsscanf") ?
            (const void *)(uintptr_t)arguments[2] : arguments + 2;
        return input && format && values ? (uint32_t)guest_vscan(input, format, values) : 0;
    }
    if (import_is(name, "_fopen")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        const char *mode = (const char *)(uintptr_t)arguments[1];
        FILE *file = fopen(path, mode);
        if (!file && getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: fopen failed: %s (%s)\n",
                    path ? path : "(null)", strerror(errno));
        } else if (file && getenv("LP32_TRACE_FILES") &&
                   getenv("LP32_TRACE_FILES")[0] == '2') {
            fprintf(stderr, "compat32: fopen %s (%s) caller=%08x\n", path ? path : "(null)",
                    mode ? mode : "", return_address);
        }
        if (!file && errno == ENOENT && path && mode && mode[0] == 'r' &&
            path_is_game_config(path)) {
            game_config_missing = 1;
        }
        return guest_handle_for_file(file);
    }
    if (import_is(name, "_fclose")) {
        return (uint32_t)guest_close_file(arguments[0]);
    }
    if (import_is(name, "_feof") || import_is(name, "_clearerr") || import_is(name, "_rewind")) {
        FILE *file=lock_host_file_for_guest(arguments[0]);if(!file)return 0;
        uint32_t result=0;
        if(import_is(name,"_feof"))result=feof(file);
        else if(import_is(name,"_clearerr"))clearerr(file);
        else rewind(file);
        unlock_host_file();return result;
    }
    if (import_is(name, "_fflush")) {
        if(!arguments[0])return (uint32_t)fflush(NULL);
        FILE *file=lock_host_file_for_guest(arguments[0]);if(!file)return EOF;
        int result=fflush(file);unlock_host_file();return (uint32_t)result;
    }
    if (import_is(name, "_fgets")) {
        FILE *file=lock_host_file_for_guest(arguments[2]);if(!file)return 0;
        char *result=fgets((char *)(uintptr_t)arguments[0],(int)arguments[1],file);
        unlock_host_file();return (uint32_t)(uintptr_t)result;
    }
    if (import_is(name, "_fputc") || import_is(name, "_fputs") || import_is(name, "_ungetc")) {
        FILE *file=lock_host_file_for_guest(arguments[1]);if(!file)return EOF;
        int result=import_is(name,"_fputs")?fputs((const char *)(uintptr_t)arguments[0],file):
            import_is(name,"_ungetc")?ungetc((int)arguments[0],file):fputc((int)arguments[0],file);
        unlock_host_file();return (uint32_t)result;
    }
    if (import_is(name, "_ftello") || import_is(name, "_fseeko")) {
        FILE *file=lock_host_file_for_guest(arguments[0]);if(!file)return UINT64_MAX;
        int64_t result;
        if(import_is(name,"_ftello"))result=ftello(file);
        else {int64_t offset;memcpy(&offset,arguments+1,8);result=fseeko(file,offset,(int)arguments[3]);}
        unlock_host_file();return (uint64_t)result;
    }
    if (import_is(name, "_tmpfile")) return guest_handle_for_file(tmpfile());
    if (import_is(name, "_fsync")) return (uint32_t)fsync((int)arguments[0]);
    if (import_is(name, "_fread")) {
        FILE *file = lock_host_file_for_guest(arguments[3]);
        if (!file) return 0;
        uint32_t result = (uint32_t)fread((void *)(uintptr_t)arguments[0],
                                          arguments[1], arguments[2], file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fwrite") || import_is(name, "_fwrite$UNIX2003")) {
        FILE *file = lock_host_file_for_guest(arguments[3]);
        if (!file) return 0;
        uint32_t result = (uint32_t)fwrite(
            (const void *)(uintptr_t)arguments[0], arguments[1], arguments[2], file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fseek")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)-1;
        uint32_t result = (uint32_t)fseek(file, (int32_t)arguments[1],
                                          (int)arguments[2]);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_ftell")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)-1;
        long offset = ftell(file);
        unlock_host_file();
        return (uint32_t)offset;
    }
    if (import_is(name, "_ferror")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return 1;
        uint32_t result = (uint32_t)ferror(file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fileno")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)-1;
        uint32_t result = (uint32_t)fileno(file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_clearerr")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return 0;
        clearerr(file);
        unlock_host_file();
        return 0;
    }
    if (import_is(name, "_getc")) {
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) return (uint32_t)EOF;
        uint32_t result = (uint32_t)getc(file);
        unlock_host_file();
        return result;
    }
    if (import_is(name, "_fprintf")) {
        const char *format = (const void *)(uintptr_t)arguments[1];
        char *scratch = malloc(65536);
        if (!format || !scratch) {
            free(scratch);
            return (uint32_t)-1;
        }
        int formatted = guest_vformat(scratch, 65536, format, arguments + 2);
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file) {
            free(scratch);
            return (uint32_t)-1;
        }
        if (formatted >= 0) fputs(scratch, file);
        unlock_host_file();
        free(scratch);
        return (uint32_t)formatted;
    }
    if (import_is(name, "_opendir$INODE64") || import_is(name, "_opendir") ||
        import_is(name, "_opendir$UNIX2003")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        DIR *directory = path ? opendir(path) : NULL;
        return guest_handle_for_directory(directory);
    }
    if (import_is(name, "_readdir") || import_is(name, "_readdir$INODE64")) {
        pthread_mutex_lock(&guest_directory_lock);
        struct guest_directory_entry *entry =
            guest_directory_for_handle(arguments[0]);
        struct dirent *host = entry ? readdir(entry->directory) : NULL;
        uint32_t result = 0;
        if (host && entry->guest_dirent && import_is(name, "_readdir$INODE64")) {
            struct guest_dirent_inode64 *guest = (void *)(uintptr_t)entry->guest_dirent;
            memset(guest,0,sizeof(*guest));
            guest->inode=host->d_ino;guest->seek_offset=host->d_seekoff;
            guest->name_length=(uint16_t)strnlen(host->d_name,sizeof(guest->name)-1);
            guest->record_length=(uint16_t)((21+guest->name_length+1+3)&~3u);
            guest->type=host->d_type;memcpy(guest->name,host->d_name,guest->name_length);
            result=entry->guest_dirent;
        } else if (host && entry->guest_dirent) {
            struct guest_dirent32 *guest =
                (void *)(uintptr_t)entry->guest_dirent;
            size_t name_length = strnlen(host->d_name, sizeof(guest->d_name) - 1);
            memset(guest, 0, sizeof(*guest));
            guest->d_ino = (uint32_t)host->d_ino;
            guest->d_reclen = (uint16_t)((offsetof(struct guest_dirent32, d_name) +
                                          name_length + 1 + 3) & ~3u);
            guest->d_type = host->d_type;
            guest->d_namlen = (uint8_t)name_length;
            memcpy(guest->d_name, host->d_name, name_length);
            guest->d_name[name_length] = '\0';
            result = entry->guest_dirent;
        }
        pthread_mutex_unlock(&guest_directory_lock);
        return result;
    }
    if (import_is(name, "_closedir") ||
        import_is(name, "_closedir$UNIX2003")) {
        pthread_mutex_lock(&guest_directory_lock);
        struct guest_directory_entry *entry =
            guest_directory_for_handle(arguments[0]);
        int status = -1;
        if (entry) {
            status = closedir(entry->directory);
            entry->directory = NULL;
            entry->handle = 0;
        } else {
            errno = EBADF;
        }
        pthread_mutex_unlock(&guest_directory_lock);
        return (uint32_t)status;
    }
    if (import_is(name, "_chmod")) {
        int result = chmod((const char *)(uintptr_t)arguments[0], (mode_t)arguments[1]);
        return (uint32_t)result;
    }
    if (import_is(name, "_open") || import_is(name, "_open$UNIX2003"))
        return (uint32_t)open((const char *)(uintptr_t)arguments[0],
            (int)arguments[1], (mode_t)arguments[2]);
    if (import_is(name, "_close") || import_is(name, "_close$UNIX2003"))
        return (uint32_t)close((int)arguments[0]);
    if (!strncmp(name, "_reg", 4)) {
        uint64_t result; if (regex_bridge32_dispatch(name, arguments, &result)) return result;
    }
    if (import_is(name, "_pread") || import_is(name, "_pwrite")) {
        off_t offset = (off_t)((uint64_t)arguments[3] | ((uint64_t)arguments[4] << 32));
        void *buffer = (void *)(uintptr_t)arguments[1];
        return (uint32_t)(import_is(name, "_pread") ?
            pread((int)arguments[0], buffer, arguments[2], offset) :
            pwrite((int)arguments[0], buffer, arguments[2], offset));
    }
    if (import_is(name, "_lseek")) {
        /* i386 off_t is 64-bit: (fd, offset_lo, offset_hi, whence) -> edx:eax. */
        off_t offset = (off_t)((uint64_t)arguments[1] | ((uint64_t)arguments[2] << 32));
        return (uint64_t)lseek((int)arguments[0], offset, (int)arguments[3]);
    }
    if (import_is(name, "_read") || import_is(name, "_read$UNIX2003"))
        return (uint32_t)read((int)arguments[0], (void *)(uintptr_t)arguments[1], arguments[2]);
    if (import_is(name, "_write") || import_is(name, "_write$UNIX2003"))
        return (uint32_t)write((int)arguments[0], (const void *)(uintptr_t)arguments[1], arguments[2]);
    if (import_is(name, "_mkdir")) {
        return (uint32_t)mkdir((const char *)(uintptr_t)arguments[0],
                               (mode_t)arguments[1]);
    }
    if (import_is(name, "_remove")) {
        return (uint32_t)remove((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_rename")) {
        return (uint32_t)rename((const char *)(uintptr_t)arguments[0],
                                (const char *)(uintptr_t)arguments[1]);
    }
    if (import_is(name, "_rmdir")) {
        return (uint32_t)rmdir((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "_atoi") || import_is(name, "_atol")) {
        return (uint32_t)atoi((const char *)(uintptr_t)arguments[0]);
    }
    if (import_is(name, "___maskrune")) {
        /* i386 ctype inlines ASCII classification but calls this function for
           high-bit bytes. COD4's server-name comparator takes that path for
           UTF-8 names. Preserve the signed rune and widen the guest's mask. */
        return (uint32_t)__maskrune((int32_t)arguments[0], (unsigned long)arguments[1]);
    }
    if (import_is(name, "___toupper")) {
        return arguments[0] <= UCHAR_MAX ?
            (uint32_t)toupper((unsigned char)arguments[0]) : arguments[0];
    }
    if (import_is(name, "___tolower")) {
        return fast_tolower(arguments, return_address);
    }
    if (import_is(name, "_rand")) {
        /* Darwin exposes RAND_MAX == INT_MAX.  A 15-bit ANSI/Windows-style
           result makes the game's rejection samplers impossible to satisfy. */
        return (uint32_t)rand();
    }
    if (import_is(name, "_getenv")) {
        return guest_copy_nullable_cstring(getenv((const char *)(uintptr_t)arguments[0]));
    }
    if (import_is(name, "_time")) {
        time_t now=time(NULL);
        if(now>INT32_MAX || now<INT32_MIN){errno=EOVERFLOW;return UINT32_MAX;}
        if(arguments[0])*(int32_t *)(uintptr_t)arguments[0]=(int32_t)now;
        return (uint32_t)(int32_t)now;
    }
    if (import_is(name, "_getuid")) return getuid();
    if (import_is(name, "_getpid")) return (uint32_t)getpid();
    if (import_is(name, "_getppid")) return (uint32_t)getppid();
    if (import_is(name, "_getpagesize")) return (uint32_t)getpagesize();
    if (import_is(name, "_getprogname")) {
        static _Thread_local uint32_t program_name;
        if (!program_name) program_name = compat_runtime32_copy_cstring(getprogname());
        return program_name;
    }
    if (import_is(name, "_getgid")) return getgid();
    if (import_is(name, "_strsignal") || import_is(name, "_strerror")) {
        static _Thread_local uint32_t text;
        if (text) compat_runtime32_deallocate(text);
        text = compat_runtime32_copy_cstring(import_is(name, "_strsignal") ?
            strsignal((int)arguments[0]) : strerror((int)arguments[0]));
        return text;
    }
    /* swprintf(buffer, count, format, ...): only MW2's CRT invalid-parameter
       handler calls it, with L"%hs". Format narrow, then widen. Windows'
       %hs (narrow string in a wide format) is %s here. */
    if (import_is(name, "_swprintf")) {
        const wchar_t *wide_format = (const void *)(uintptr_t)arguments[2];
        wchar_t *destination = (void *)(uintptr_t)arguments[0];
        size_t count = arguments[1];
        if (!wide_format || !destination || !count) return (uint32_t)-1;
        char format[1024];
        size_t length = 0;
        for (const wchar_t *p = wide_format; *p && length + 1 < sizeof(format); ++p) {
            if (p[0] == L'%' && p[1] == L'h' && p[2] == L's') { format[length++] = '%'; ++p; continue; }
            format[length++] = *p < 0x80 ? (char)*p : '?';
        }
        format[length] = 0;
        char *narrow = malloc(65536);
        if (!narrow) return (uint32_t)-1;
        int formatted = guest_vformat(narrow, 65536, format, arguments + 3);
        size_t written = formatted < 0 ? (size_t)-1 : mbstowcs(destination, narrow, count - 1);
        free(narrow);
        if (written == (size_t)-1) { destination[0] = 0; return (uint32_t)-1; }
        destination[written] = 0;
        return (uint32_t)written;
    }
    if (import_is(name, "___stack_chk_fail")) {
        fprintf(stderr, "compat32: guest stack buffer overflow detected at 0x%08" PRIx32 "\n", return_address);
        compat_runtime32_trap_import(name, arguments, return_address);
        return 0;
    }
    /* Error-path libc calls in MW2's UPnP and SDL code. The dispatcher has
       already copied the guest errno into the host errno. */
    if (import_is(name, "_perror")) { perror((const char *)(uintptr_t)arguments[0]); return 0; }
    if (import_is(name, "_herror")) { herror((const char *)(uintptr_t)arguments[0]); return 0; }
    /* Only SDL's assertion prompt locks stdin; guest FILE objects are not
       host FILEs, and nothing else reads that stream concurrently. */
    if (import_is(name, "_flockfile") || import_is(name, "_funlockfile")) return 0;
    if (import_is(name, "_asctime")) {
        const struct guest_tm32 *guest = (const void *)(uintptr_t)arguments[0];
        static _Thread_local uint32_t text;
        if (!guest) return 0;
        struct tm host = {
            .tm_sec = guest->tm_sec, .tm_min = guest->tm_min,
            .tm_hour = guest->tm_hour, .tm_mday = guest->tm_mday,
            .tm_mon = guest->tm_mon, .tm_year = guest->tm_year,
            .tm_wday = guest->tm_wday, .tm_yday = guest->tm_yday,
            .tm_isdst = guest->tm_isdst,
        };
        char buffer[64];
        if (!asctime_r(&host, buffer)) return 0;
        if (!text) text = guest_allocate(sizeof(buffer), true);
        if (text) memcpy((void *)(uintptr_t)text, buffer, strlen(buffer) + 1);
        return text;
    }
    /* Multiprocessing Services Duration: positive milliseconds, negative
       microseconds. MW2's semaphore WaitForSingleObject uses it for timed
       waits; Nanoseconds comes back in edx:eax. */
    if (import_is(name, "_DurationToNanoseconds")) {
        int32_t duration = (int32_t)arguments[0];
        if (duration == INT32_MAX) return UINT64_MAX;
        if (duration < 0) return (uint64_t)-(int64_t)duration * 1000;
        return (uint64_t)duration * 1000000;
    }
    /* Darwin i386 returns the two 32-bit div_t fields in EDX:EAX. */
    if (import_is(name, "_div")) {
        div_t value=div((int32_t)arguments[0],(int32_t)arguments[1]);
        return (uint32_t)value.quot|((uint64_t)(uint32_t)value.rem<<32);
    }
    /* SDL's power query treats a missing power-source blob as unknown. */
    if (import_is(name, "_IOPSCopyPowerSourcesInfo") ||
        import_is(name, "_IOPSCopyPowerSourcesList") ||
        import_is(name, "_IOPSGetPowerSourceDescription")) return 0;
    if (import_is(name, "_SetSystemUIMode"))
        return (uint32_t)SetSystemUIMode(arguments[0], arguments[1]);
    if (import_is(name, "_sigaltstack")) {
        stack_t next, previous;
        const uint32_t *g = (void *)(uintptr_t)arguments[0];
        if (g) { next.ss_sp = (void *)(uintptr_t)g[0]; next.ss_size = g[1]; next.ss_flags = (int)g[2]; }
        if (arguments[1]) {
            if (sigaltstack(NULL, &previous)) return UINT32_MAX;
            if ((uintptr_t)previous.ss_sp > UINT32_MAX || previous.ss_size > UINT32_MAX) {
                errno = EOVERFLOW; return UINT32_MAX;
            }
        }
        int status = sigaltstack(g ? &next : NULL, arguments[1] ? &previous : NULL);
        if (!status && arguments[1]) {
            uint32_t *out = (void *)(uintptr_t)arguments[1];
            out[0] = (uint32_t)(uintptr_t)previous.ss_sp;
            out[1] = (uint32_t)previous.ss_size; out[2] = (uint32_t)previous.ss_flags;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_sigaction")) {
        /* As with signal(), asynchronous guest handlers require a dedicated
         * context bridge. Report ENOTSUP, preserving the host crash handler. */
        const uint32_t *g = (void *)(uintptr_t)arguments[1];
        struct sigaction next = {0}, previous;
        if (g && g[0] > 1) { errno = ENOTSUP; return UINT32_MAX; }
        if (sigaction((int)arguments[0], NULL, &previous)) return UINT32_MAX;
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            errno = ENOTSUP; return UINT32_MAX;
        }
        if (g) { next.sa_handler = g[0] ? SIG_IGN : SIG_DFL; next.sa_mask = g[1]; next.sa_flags = (int)g[2]; }
        int status = sigaction((int)arguments[0], g ? &next : NULL, arguments[2] ? &previous : NULL);
        if (!status && arguments[2]) {
            uint32_t *out = (void *)(uintptr_t)arguments[2];
            out[0] = previous.sa_handler == SIG_IGN; out[1] = previous.sa_mask;
            out[2] = (uint32_t)previous.sa_flags;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_sigprocmask"))
        return (uint32_t)sigprocmask((int)arguments[0], (void *)(uintptr_t)arguments[1], (void *)(uintptr_t)arguments[2]);
    if (import_is(name, "_pthread_sigmask"))
        return (uint32_t)pthread_sigmask((int)arguments[0], (void *)(uintptr_t)arguments[1], (void *)(uintptr_t)arguments[2]);
    if (import_is(name, "_pthread_setcanceltype"))
        return (uint32_t)pthread_setcanceltype((int)arguments[0], (void *)(uintptr_t)arguments[1]);
    /* _pthread_cancel cancels the host thread, so test the host request. */
    if (import_is(name, "_pthread_testcancel")) {
        pthread_testcancel();
        return 0;
    }
    if (import_is(name, "_signal")) {
        /* A 32-bit signal handler cannot be installed as a host function.
           Until asynchronous guest contexts/unwinding are supported, report
           failure without changing the host handler. CPU feature probes can
           then take their normal portable fallback. */
        int number=(int)arguments[0];
        if(number<=0 || number>=NSIG){errno=EINVAL;return UINT32_MAX;}
        if(arguments[1]>1){errno=ENOTSUP;return UINT32_MAX;}
        struct sigaction previous;
        if(sigaction(number,NULL,&previous))return UINT32_MAX;
        if(previous.sa_handler!=SIG_DFL && previous.sa_handler!=SIG_IGN){errno=ENOTSUP;return UINT32_MAX;}
        return (uint32_t)(uintptr_t)signal(number,arguments[1]?SIG_IGN:SIG_DFL);
    }
    if (import_is(name, "_ptrace") || !strcmp(name, "ptrace")) {
        return (uint32_t)ptrace((int)arguments[0], (pid_t)arguments[1],
            (caddr_t)(uintptr_t)arguments[2], (int)arguments[3]);
    }
    if (import_is(name, "_srand")) {
        srand(arguments[0]);
        return 0;
    }
    if (import_is(name, "___udivdi3") || import_is(name, "___umoddi3")) {
        uint64_t dividend = (uint64_t)arguments[0] |
            ((uint64_t)arguments[1] << 32);
        uint64_t divisor = (uint64_t)arguments[2] |
            ((uint64_t)arguments[3] << 32);
        if (!divisor) return 0;
        return import_is(name, "___udivdi3") ?
            dividend / divisor : dividend % divisor;
    }
    if (import_is(name, "___moddi3") || import_is(name, "___divdi3")) {
        int64_t dividend = (int64_t)((uint64_t)arguments[0] |
            ((uint64_t)arguments[1] << 32));
        int64_t divisor = (int64_t)((uint64_t)arguments[2] |
            ((uint64_t)arguments[3] << 32));
        if (!divisor) return 0;
        if (dividend == INT64_MIN && divisor == -1)
            return import_is(name, "___divdi3") ? (uint64_t)INT64_MIN : 0;
        return import_is(name, "___divdi3") ?
            (uint64_t)(dividend / divisor) : (uint64_t)(dividend % divisor);
    }
    if (import_is(name, "___error")) {
        if (guest_errno_address) return guest_errno_address;
        /* Newer images import __error without a global errno symbol. Each
           guest thread still needs a writable, 32-bit-addressable int. */
        if (!guest_thread_errno_address) {
            int error = errno;
            guest_thread_errno_address = guest_allocate(sizeof(int32_t), true);
            errno = error;
        }
        return guest_thread_errno_address;
    }
    if (import_is(name, "_mach_host_self")) return mach_host_self();
    if (import_is(name, "_mach_thread_self")) return mach_thread_self();
    if (import_is(name, "_mach_port_deallocate"))
        return (uint32_t)mach_port_deallocate(arguments[0], arguments[1]);
    if (import_is(name, "_mach_absolute_time")) {
        uint64_t value = mach_absolute_time();
        static uint32_t traced_clocks;
        if (timing_trace && traced_clocks++ < 32) {
            fprintf(stderr,
                    "compat32: mach_absolute_time=%" PRIu64
                    " from 0x%08" PRIx32 "\n",
                    value, return_address);
        }
        return value;
    }
    /* Mach semaphores are 32-bit port names on both sides. */
    if (import_is(name, "_semaphore_create"))
        return (uint32_t)semaphore_create((task_t)arguments[0], (semaphore_t *)(uintptr_t)arguments[1],
                                          (int)arguments[2], (int)arguments[3]);
    if (import_is(name, "_semaphore_destroy"))
        return (uint32_t)semaphore_destroy((task_t)arguments[0], (semaphore_t)arguments[1]);
    if (import_is(name, "_semaphore_signal")) return (uint32_t)semaphore_signal((semaphore_t)arguments[0]);
    if (import_is(name, "_semaphore_wait")) return (uint32_t)semaphore_wait((semaphore_t)arguments[0]);
    if (import_is(name, "_mach_timebase_info")) {
        mach_timebase_info_data_t host_info;
        kern_return_t result = mach_timebase_info(&host_info);
        static uint32_t traced_timebases;
        if (timing_trace && traced_timebases++ < 16) {
            fprintf(stderr, "compat32: mach_timebase_info=%u/%u\n",
                    host_info.numer, host_info.denom);
        }
        if (result == KERN_SUCCESS && arguments[0]) {
            uint32_t *guest_info = (void *)(uintptr_t)arguments[0];
            guest_info[0] = host_info.numer;
            guest_info[1] = host_info.denom;
        }
        return (uint32_t)result;
    }
    if (import_is(name, "_task_policy_set") || import_is(name, "_setpriority")) {
        return 0;
    }
    if (import_is(name, "_UpdateSystemActivity")) return 0;
    if (import_is(name, "_sched_yield")) return (uint32_t)sched_yield();
    if (import_is(name, "_sched_get_priority_max"))
        return (uint32_t)sched_get_priority_max((int)arguments[0]);
    if (import_is(name, "_sched_get_priority_min"))
        return (uint32_t)sched_get_priority_min((int)arguments[0]);
    if (import_is(name, "_usleep") ||
        import_is(name, "_usleep$UNIX2003")) {
        static uint32_t traced_sleeps;
        if (timing_trace && traced_sleeps++ < 12) {
            fprintf(stderr, "compat32: usleep(%" PRIu32 ") from 0x%08" PRIx32 "\n",
                    arguments[0], return_address);
        }
        return (uint32_t)usleep(arguments[0]);
    }
    if (import_is(name, "_OSMemoryBarrier")) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return 0;
    }
    if (import_is(name, "_OSAtomicAdd32") ||
        import_is(name, "_OSAtomicAdd32Barrier")) {
        return fast_OSAtomicAdd32(arguments, return_address);
    }
    if (import_is(name, "_OSAtomicAdd64")) {
        int64_t value; memcpy(&value, arguments, 8);
        return __atomic_add_fetch((int64_t *)(uintptr_t)arguments[2], value, __ATOMIC_SEQ_CST);
    }
    if (import_is(name, "_OSAtomicCompareAndSwap32") ||
        import_is(name, "_OSAtomicCompareAndSwapInt") ||
        import_is(name, "_OSAtomicCompareAndSwap32Barrier") ||
        import_is(name, "_OSAtomicCompareAndSwapPtrBarrier")) {
        return fast_OSAtomicCompareAndSwap32(arguments, return_address);
    }
    if (import_is(name, "_OSAtomicCompareAndSwap64Barrier") || import_is(name,"_OSAtomicCompareAndSwap64")) {
        uint64_t expected=(uint64_t)arguments[0]|(uint64_t)arguments[1]<<32;
        uint64_t desired=(uint64_t)arguments[2]|(uint64_t)arguments[3]<<32;
        return __atomic_compare_exchange_n((uint64_t *)(uintptr_t)arguments[4],&expected,desired,
            false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    }
    if (import_is(name, "_pthread_once")) {
        return guest_pthread_once(arguments[0], arguments[1]);
    }
    if (import_is(name, "_chdir")) {
        const char *path = (const char *)(uintptr_t)arguments[0];
        return path ? (uint32_t)chdir(path) : (uint32_t)-1;
    }
    if (import_is(name, "_fcntl") ||
        import_is(name, "_fcntl$UNIX2003")) {
        /* F_GETFD/F_SETFD/F_GETFL/F_SETFL carry an int; F_GETPATH and the
           lock commands carry a pointer, which is a 32-bit guest address
           the host can use directly. */
        int command = (int)arguments[1];
        if (command == F_GETFD || command == F_SETFD || command == F_GETFL ||
            command == F_SETFL || command == F_DUPFD || command == F_NOCACHE ||
            command == F_RDAHEAD || command == F_FULLFSYNC) {
            return (uint32_t)fcntl((int)arguments[0], command, (int)arguments[2]);
        }
        return (uint32_t)fcntl((int)arguments[0], command,
                               (void *)(uintptr_t)arguments[2]);
    }
    if (import_is(name, "_fscanf")) {
        const char *format = (const void *)(uintptr_t)arguments[1];
        FILE *file = lock_host_file_for_guest(arguments[0]);
        if (!file || !format) {
            if (file) unlock_host_file();
            return (uint32_t)EOF;
        }
        uint32_t result = (uint32_t)guest_fscan(file, format, arguments + 2);
        unlock_host_file();
        return result;
    }

    if (import_is(name, "_acosf")) return return_guest_float(acosf(guest_float(arguments[0])));
    if (import_is(name, "_asinf")) return return_guest_float(asinf(guest_float(arguments[0])));
    if (import_is(name, "_acos")) return return_guest_double(acos(guest_double(arguments)));
    if (import_is(name, "_asin")) return return_guest_double(asin(guest_double(arguments)));
    if (import_is(name, "_cosh")) return return_guest_double(cosh(guest_double(arguments)));
    if (import_is(name, "_sinh")) return return_guest_double(sinh(guest_double(arguments)));
    if (import_is(name, "_tan")) return return_guest_double(tan(guest_double(arguments)));
    if (import_is(name, "_tanh")) return return_guest_double(tanh(guest_double(arguments)));
    if (import_is(name, "_floorf")) return return_guest_float(floorf(guest_float(arguments[0])));
    if (import_is(name, "_rintf")) return return_guest_float(rintf(guest_float(arguments[0])));
    if (import_is(name, "_fmin")) return return_guest_double(fmin(guest_double(arguments), guest_double(arguments + 2)));
    if (import_is(name, "_fmodf")) return return_guest_float(fmodf(guest_float(arguments[0]), guest_float(arguments[1])));
    if (import_is(name, "_fmod")) return return_guest_double(fmod(guest_double(arguments), guest_double(arguments + 2)));
    if (import_is(name, "_atan2")) return return_guest_double(atan2(guest_double(arguments), guest_double(arguments + 2)));
    if (import_is(name, "_fmaxf")) return return_guest_float(fmaxf(guest_float(arguments[0]), guest_float(arguments[1])));
    if (import_is(name, "_fminf")) return return_guest_float(fminf(guest_float(arguments[0]), guest_float(arguments[1])));
    if (import_is(name, "_powf")) return return_guest_float(powf(guest_float(arguments[0]), guest_float(arguments[1])));
    if (import_is(name, "_atan2f")) return return_guest_float(atan2f(guest_float(arguments[0]), guest_float(arguments[1])));
    if (import_is(name, "_frexp")) return return_guest_double(frexp(guest_double(arguments), (void *)(uintptr_t)arguments[2]));
    if (import_is(name, "_modf")) return return_guest_double(modf(guest_double(arguments), (void *)(uintptr_t)arguments[2]));
    if (import_is(name, "___isfinited")) return isfinite(guest_double(arguments)) != 0;
    if (import_is(name, "___isinfd")) return isinf(guest_double(arguments)) != 0;
    if (import_is(name, "___isinff")) return isinf(guest_float(arguments[0])) != 0;
    if (import_is(name, "___isnand")) return isnan(guest_double(arguments)) != 0;
    if (import_is(name, "___isnanf")) return isnan(guest_float(arguments[0])) != 0;
    if (import_is(name, "___signbitf")) return signbit(guest_float(arguments[0])) != 0;
    if (import_is(name, "_roundf")) return return_guest_float(roundf(guest_float(arguments[0])));
    if (import_is(name, "_round")) return return_guest_double(round(guest_double(arguments)));
    if (import_is(name, "_atanf")) return return_guest_float(atanf(guest_float(arguments[0])));
    if (import_is(name, "_tanf")) return return_guest_float(tanf(guest_float(arguments[0])));
    if (import_is(name, "_log10")) return return_guest_double(log10(guest_double(arguments)));
    if (import_is(name, "_fmax")) return return_guest_double(fmax(guest_double(arguments), guest_double(arguments + 2)));
    if (import_is(name, "_logf")) return fast_logf(arguments, return_address);
    if (import_is(name, "_log10f")) return return_guest_float(log10f(guest_float(arguments[0])));
    if (import_is(name, "_expf")) return fast_expf(arguments, return_address);
    if (import_is(name, "_sinf")) return return_guest_float(sinf(guest_float(arguments[0])));
    if (import_is(name, "_cosf")) return return_guest_float(cosf(guest_float(arguments[0])));
    if (import_is(name, "_ceilf")) return return_guest_float(ceilf(guest_float(arguments[0])));

    if (import_is(name, "_log")) return return_guest_double(log(guest_double(arguments)));
    if (import_is(name, "_exp2")) return return_guest_double(exp2(guest_double(arguments)));
    if (import_is(name, "_log2")) return return_guest_double(log2(guest_double(arguments)));
    if (import_is(name, "_exp")) return return_guest_double(exp(guest_double(arguments)));
    if (import_is(name, "_sin")) return return_guest_double(sin(guest_double(arguments)));
    if (import_is(name, "_cos")) return return_guest_double(cos(guest_double(arguments)));
    if (import_is(name, "_atan")) return return_guest_double(atan(guest_double(arguments)));
    if (import_is(name, "_ceil")) return return_guest_double(ceil(guest_double(arguments)));
    if (import_is(name, "_floor")) return return_guest_double(floor(guest_double(arguments)));
    if (import_is(name, "_rint")) return return_guest_double(rint(guest_double(arguments)));
    if (import_is(name, "_nearbyint")) return return_guest_double(nearbyint(guest_double(arguments)));
    if (import_is(name, "_nearbyintf")) return return_guest_float(nearbyintf(guest_float(arguments[0])));
    if (import_is(name, "_ldexpf")) {
        return return_guest_float(ldexpf(guest_float(arguments[0]), (int)arguments[1]));
    }
    if (import_is(name, "_ldexp")) {
        return return_guest_double(ldexp(guest_double(arguments), (int)arguments[2]));
    }
    if (import_is(name, "_pow")) {
        return return_guest_double(pow(guest_double(arguments),
                                       guest_double(arguments + 2)));
    }
    if (import_is(name, "_difftime")) {
        return return_guest_double((double)(int32_t)arguments[0] -
                                   (double)(int32_t)arguments[1]);
    }
    if (import_is(name, "_CFAbsoluteTimeGetCurrent")) {
        return return_guest_double(CFAbsoluteTimeGetCurrent());
    }

    if (import_is(name, "_pthread_attr_init") || import_is(name, "_pthread_attr_destroy") ||
        import_is(name, "_pthread_attr_setstacksize") || import_is(name, "_pthread_attr_setdetachstate")) {
        uint32_t *guest = (void *)(uintptr_t)arguments[0];
        if (!guest) return EINVAL;
        pthread_attr_t *attr = NULL;
        if (import_is(name, "_pthread_attr_init")) {
            attr = malloc(sizeof(*attr));
            if (!attr) return ENOMEM;
            int status = pthread_attr_init(attr);
            if (status) { free(attr); return (uint32_t)status; }
            memset(guest, 0, 40); guest[0] = 0x4c503341;
            memcpy(guest + 1, &attr, sizeof(attr)); return 0;
        }
        if (guest[0] != 0x4c503341) return EINVAL;
        memcpy(&attr, guest + 1, sizeof(attr));
        if (import_is(name, "_pthread_attr_destroy")) {
            int status = pthread_attr_destroy(attr);
            free(attr); memset(guest, 0, 40); return (uint32_t)status;
        }
        if (import_is(name, "_pthread_attr_setdetachstate"))
            return (uint32_t)pthread_attr_setdetachstate(attr, (int)arguments[1]);
        return (uint32_t)pthread_attr_setstacksize(attr, arguments[1]);
    }
    if (import_is(name, "_pthread_mutexattr_init") ||
        import_is(name, "_pthread_mutexattr_destroy") ||
        import_is(name, "_pthread_mutexattr_destroy$UNIX2003") ||
        import_is(name, "_pthread_mutexattr_settype")) {
        return 0;
    }
    if (import_is(name,"__ZNSt3__15mutex4lockEv")) return fast_pthread_mutex_lock(arguments,return_address);
    if (import_is(name,"__ZNSt3__15mutex6unlockEv")) return fast_pthread_mutex_unlock(arguments,return_address);
    if (import_is(name,"__ZNSt3__15mutexD1Ev")) return guest_mutex_destroy(arguments[0]);
    if (import_is(name,"__ZNSt3__118condition_variableD1Ev")) return guest_cond_destroy(arguments[0]);
    if (import_is(name,"__ZNSt3__118condition_variable10notify_allEv") ||
        import_is(name,"__ZNSt3__118condition_variable10notify_oneEv")) {
        pthread_cond_t *condition=host_cond_for_guest(arguments[0],true);
        if (!condition) abort();
        if (import_is(name,"__ZNSt3__118condition_variable10notify_allEv"))
            pthread_cond_broadcast(condition);
        else pthread_cond_signal(condition);
        return 0;
    }
    if (import_is(name,"__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILx1ELx1000000000EEEEEEE")) {
        /* i386 unique_lock contains a four-byte mutex pointer followed by
           the owns-lock bool. The absolute deadline is int64 nanoseconds. */
        const uint32_t *lock=(const void *)(uintptr_t)arguments[1];
        if (!lock || !((const uint8_t *)lock)[4]) abort();
        pthread_cond_t *condition=host_cond_for_guest(arguments[0],true);
        pthread_mutex_t *mutex=host_mutex_for_guest(lock[0],true);
        int64_t ns=(int64_t)((uint64_t)arguments[2]|(uint64_t)arguments[3]<<32);
        if (ns<0)ns=0;
        struct timespec deadline={ns/1000000000,ns%1000000000};
        if (!condition || !mutex) abort();
        int status=pthread_cond_timedwait(condition,mutex,&deadline);
        if (status && status!=ETIMEDOUT) abort();
        return 0;
    }
    /* Mach semaphores are 32-bit port names on both sides. */
    if (import_is(name, "_semaphore_create"))
        return (uint32_t)semaphore_create((task_t)arguments[0], (semaphore_t *)(uintptr_t)arguments[1],
                                          (int)arguments[2], (int)arguments[3]);
    if (import_is(name, "_semaphore_destroy"))
        return (uint32_t)semaphore_destroy((task_t)arguments[0], (semaphore_t)arguments[1]);
    if (import_is(name, "_semaphore_signal")) return (uint32_t)semaphore_signal((semaphore_t)arguments[0]);
    if (import_is(name, "_semaphore_wait")) return (uint32_t)semaphore_wait((semaphore_t)arguments[0]);
    if (import_is(name, "_mmap")) {
        off_t offset=(off_t)((uint64_t)arguments[5]|(uint64_t)arguments[6]<<32);
        return guest_arena_mmap(arguments[0],arguments[1],(int)arguments[2],(int)arguments[3],
                                (int)arguments[4],offset,return_address);
    }
    if (import_is(name, "_munmap"))
        return (uint32_t)guest_arena_munmap(arguments[0],arguments[1],return_address);
    if (import_is(name, "_mprotect")) {
        uintptr_t start=arguments[0],end=start+arguments[1];
        int protection=(int)arguments[2];
        if ((protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE)) {
            uintptr_t page=(uintptr_t)getpagesize();
            start&=~(page-1);
            end=(end+page-1)&~(page-1);
        }
        return (uint32_t)mprotect((void *)start,end-start,protection);
    }
    if (import_is(name, "_host_statistics")) {
        /* Mach statistics are arrays of fixed-width integer_t values. */
        return (uint32_t)host_statistics(arguments[0],(int)arguments[1],
            (host_info_t)(uintptr_t)arguments[2],(mach_msg_type_number_t *)(uintptr_t)arguments[3]);
    }
    if (import_is(name, "_mach_vm_region")) {
        mach_vm_address_t *address=(void *)(uintptr_t)arguments[1],start=*address;
        mach_vm_size_t *size=(void *)(uintptr_t)arguments[2],length=0;
        bool guest_task=arguments[0]==mach_task_self();
        if (guest_task && start>=UINT64_C(0x100000000)) return KERN_INVALID_ADDRESS;
        mach_port_t object=MACH_PORT_NULL;
        mach_msg_type_number_t info_count=*(mach_msg_type_number_t *)(uintptr_t)arguments[5];
        kern_return_t status;
        for (;;) {
            *(mach_msg_type_number_t *)(uintptr_t)arguments[5]=info_count;
            status=mach_vm_region(arguments[0],&start,&length,(int)arguments[3],
                (vm_region_info_t)(uintptr_t)arguments[4],(mach_msg_type_number_t *)(uintptr_t)arguments[5],&object);
            uint64_t free_end;
            if (status || !guest_task || !guest_arena_free_end(start,&free_end)) break;
            if(object)mach_port_deallocate(mach_task_self(),object);
            object=MACH_PORT_NULL;start=free_end;length=0;
        }
        /* A 32-bit process cannot enumerate the loader and Rosetta mappings
           above 4 GiB. Aspyr's address-space counter otherwise wraps forever. */
        if (!status && guest_task && start>=UINT64_C(0x100000000)) {
            if(object)mach_port_deallocate(mach_task_self(),object);
            return KERN_INVALID_ADDRESS;
        }
        if (!status) {
            if (guest_task && length>UINT64_C(0x100000000)-start) length=UINT64_C(0x100000000)-start;
            *address=start;*size=length;
            *(mach_port_t *)(uintptr_t)arguments[6]=object;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_host_page_size")) {
        /* Under Rosetta the host reports the 16 KB arm64 kernel page, but
           this process (and the guest's mappings) use vm_page_size. */
        vm_size_t size=0;
        kern_return_t status=host_page_size(arguments[0],&size);
        if (!status && arguments[1]) *(uint32_t *)(uintptr_t)arguments[1]=(uint32_t)vm_page_size;
        return (uint32_t)status;
    }
    if (import_is(name, "_nanosleep")) {
        if (!arguments[0]) { errno=EFAULT; return UINT32_MAX; }
        const int32_t *guest=(const void *)(uintptr_t)arguments[0];
        struct timespec request={guest[0],guest[1]},remaining={0};
        int status=nanosleep(&request,arguments[1]?&remaining:NULL);
        if (status && errno==EINTR && arguments[1]) {
            int32_t *output=(void *)(uintptr_t)arguments[1];
            output[0]=(int32_t)remaining.tv_sec;output[1]=(int32_t)remaining.tv_nsec;
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_pthread_setname_np")) return (uint32_t)pthread_setname_np((const char *)(uintptr_t)arguments[0]);
    if (import_is(name, "_pthread_self")) return fast_pthread_self(arguments, return_address);
    if (import_is(name, "_pthread_equal")) return arguments[0]==arguments[1];
    if (import_is(name, "_pthread_main_np")) return pthread_main_np();
    if (import_is(name, "_pthread_mach_thread_np")) {
        pthread_t thread=host_thread(arguments[0]);
        return thread ? pthread_mach_thread_np(thread) : MACH_PORT_NULL;
    }
    if (import_is(name, "_pthread_getschedparam")) {
        pthread_t thread = host_thread(arguments[0]);
        if (!thread) return ESRCH;
        return (uint32_t)pthread_getschedparam(thread,
            (int *)(uintptr_t)arguments[1], (struct sched_param *)(uintptr_t)arguments[2]);
    }
    if (import_is(name, "_pthread_setschedparam")) {
        pthread_t thread = host_thread(arguments[0]);
        return thread ? (uint32_t)pthread_setschedparam(thread, (int)arguments[1],
            (const struct sched_param *)(uintptr_t)arguments[2]) : ESRCH;
    }
    if (import_is(name, "_pthread_getname_np")) {
        pthread_t thread = host_thread(arguments[0]);
        return thread ? (uint32_t)pthread_getname_np(thread,
            (char *)(uintptr_t)arguments[1], arguments[2]) : ESRCH;
    }
    if (import_is(name, "_thread_resume")) return thread_resume(arguments[0]);
    if (import_is(name, "_pthread_join") || import_is(name, "_pthread_detach") ||
        import_is(name, "_pthread_cancel")) {
        pthread_t thread=host_thread(arguments[0]);if(!thread)return ESRCH;
        if (import_is(name, "_pthread_detach")) {
            int status = pthread_detach(thread);
            if (!status) thread_finished(arguments[0], true);
            return (uint32_t)status;
        }
        if(import_is(name,"_pthread_cancel"))return (uint32_t)pthread_cancel(thread);
        void *value=NULL;int status=pthread_join(thread,&value);
        if (!status) {
            if (arguments[1]) *(uint32_t *)(uintptr_t)arguments[1] = (uint32_t)(uintptr_t)value;
            forget_thread(arguments[0]);
        }
        return (uint32_t)status;
    }
    if (import_is(name, "_pthread_exit") && guest_thread_started) {
        guest_thread_exiting = true;
        guest_thread_exit_value = arguments[0];
        lp32_leave_guest = 1;
        return 0;
    }
    if (import_is(name, "_pthread_create") || import_is(name, "_pthread_create_suspended_np")) {
        pthread_attr_t native_attr, *attr = NULL;
        int detach = PTHREAD_CREATE_JOINABLE;
        if (arguments[1]) {
            uint32_t *guest_attr = (void *)(uintptr_t)arguments[1];
            if (guest_attr[0] != 0x4c503341) return EINVAL;
            pthread_attr_t *stored;
            memcpy(&stored, guest_attr + 1, sizeof(stored));
            native_attr = *stored; attr = &native_attr;
            size_t size = 0; pthread_attr_getstacksize(attr, &size);
            /* The guest has its own low stack. Native bridge frames need the
             * host default minimum even when the guest requests a small one. */
            if (size < 512 * 1024) pthread_attr_setstacksize(attr, 512 * 1024);
            pthread_attr_getdetachstate(attr, &detach);
        }
        struct guest_thread_context *context = malloc(sizeof(*context));
        if (!context) return (uint32_t)ENOMEM;
        context->function = arguments[2];
        if (lp32_profile()->thread_argument_is_direct) {
            context->argument = arguments[3];
        } else {
            /* The engine's sole pthread_create site (Pirates 0x2fxxxx, Clone
               Wars 0x314bc4) hands the thread entry the address of a
               stack-local task-record pointer.  Native pthread scheduling
               consumed it immediately; a translated host thread can start
               after that slot is reused.  Snapshot the word into stable
               guest memory. */
            uint32_t stable_argument = guest_allocate(sizeof(uint32_t), false);
            if (!stable_argument) {
                free(context);
                return (uint32_t)ENOMEM;
            }
            *(uint32_t *)(uintptr_t)stable_argument =
                *(const uint32_t *)(uintptr_t)arguments[3];
            context->argument = stable_argument;
            if (getenv("LP32_TRACE_THREADS")) {
                uint32_t record = *(uint32_t *)(uintptr_t)stable_argument;
                uint64_t thread_id = 0;
                pthread_threadid_np(NULL, &thread_id);
                fprintf(stderr,
                        "compat32: pthread_create from tid=%llu ra=0x%08x "
                        "entry=0x%08x arg=0x%08x record=0x%08x fn=0x%08x "
                        "name=%.31s\n",
                        (unsigned long long)thread_id, return_address,
                        arguments[2], arguments[3], record,
                        record ? *(uint32_t *)(uintptr_t)record : 0,
                        record ? (const char *)(uintptr_t)(record + 8) : "");
            }
        }
        struct thread_token *record=malloc(sizeof(*record));
        if (!record) { if (!lp32_profile()->thread_argument_is_direct) guest_deallocate(context->argument); free(context); return ENOMEM; }
        uint32_t token=allocate_thread_token();
        if(!token){if(!lp32_profile()->thread_argument_is_direct)guest_deallocate(context->argument);free(context);free(record);return ENOMEM;}
        context->token=token;
        remember_thread(record, token, NULL);
        uint32_t stable_argument = lp32_profile()->thread_argument_is_direct ? 0 : context->argument;
        pthread_t thread;
        int (*create)(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *) = pthread_create;
        if(import_is(name,"_pthread_create_suspended_np"))create=dlsym(RTLD_DEFAULT,"pthread_create_suspended_np");
        int status = create ? create(&thread, attr, run_guest_thread, context) : ENOTSUP;
        if (status != 0) {
            guest_deallocate(stable_argument); free(context); forget_thread(token);
            return (uint32_t)status;
        }
        pthread_mutex_lock(&thread_tokens_lock);
        record->host = thread;
        pthread_mutex_unlock(&thread_tokens_lock);
        if (detach == PTHREAD_CREATE_DETACHED) thread_finished(token, true);
        uint32_t *guest_thread = (void *)(uintptr_t)arguments[0];
        if (guest_thread) {
            *guest_thread = token;
        }
        return 0;
    }
    if (import_is(name, "_pthread_mutex_init")) {
        return guest_mutex_initialize(arguments[0]);
    }
    if (import_is(name, "_pthread_mutex_destroy")) {
        return guest_mutex_destroy(arguments[0]);
    }
    if (import_is(name, "_pthread_mutex_lock")) {
        return fast_pthread_mutex_lock(arguments, return_address);
    }
    if (import_is(name, "_pthread_mutex_trylock")) {
        return fast_pthread_mutex_trylock(arguments, return_address);
    }
    if (import_is(name, "_pthread_mutex_unlock")) {
        return fast_pthread_mutex_unlock(arguments, return_address);
    }
    if (import_is(name, "_pthread_cond_init") ||
        import_is(name, "_pthread_cond_init$UNIX2003")) {
        return guest_cond_initialize(arguments[0]);
    }
    if (import_is(name, "_pthread_cond_destroy")) {
        return guest_cond_destroy(arguments[0]);
    }
    if (import_is(name, "_pthread_cond_signal")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        return condition ? (uint32_t)pthread_cond_signal(condition) : (uint32_t)ENOMEM;
    }
    if (import_is(name, "_pthread_cond_broadcast")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        return condition ? (uint32_t)pthread_cond_broadcast(condition) : (uint32_t)ENOMEM;
    }
    if (import_is(name, "_pthread_cond_timedwait")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        pthread_mutex_t *mutex = host_mutex_for_guest(arguments[1], false);
        const int32_t *guest = (const void *)(uintptr_t)arguments[2];
        if (!condition || !mutex || !guest) return (uint32_t)EINVAL;
        struct timespec deadline = {guest[0], guest[1]};
        return (uint32_t)pthread_cond_timedwait(condition, mutex, &deadline);
    }
    if (import_is(name, "_pthread_cond_wait") ||
        import_is(name, "_pthread_cond_wait$UNIX2003")) {
        pthread_cond_t *condition = host_cond_for_guest(arguments[0], true);
        pthread_mutex_t *mutex = host_mutex_for_guest(arguments[1], false);
        return condition && mutex ? (uint32_t)pthread_cond_wait(condition, mutex) :
                                    (uint32_t)EINVAL;
    }
    if (import_is(name, "_pthread_key_create")) {
        uint32_t *key = (void *)(uintptr_t)arguments[0];
        uint32_t next = __atomic_fetch_add(&guest_pthread_next_key, 1,
                                           __ATOMIC_RELAXED);
        if (next >= 128) return (uint32_t)EAGAIN;
        *key = next;
        return 0;
    }
    if (import_is(name, "_pthread_setspecific")) {
        if (arguments[0] >= 128) return (uint32_t)EINVAL;
        guest_pthread_values[arguments[0]] = arguments[1];
        return 0;
    }
    if (import_is(name, "_pthread_getspecific")) {
        return fast_pthread_getspecific(arguments, return_address);
    }

    if (import_is(name, "_MPCreateSemaphore")) {
        uint32_t handle = guest_semaphore_create((int32_t)arguments[0],
                                                 (int32_t)arguments[1]);
        if (!handle) return (uint32_t)-1;
        uint32_t *output = (void *)(uintptr_t)arguments[2];
        if (output) *output = handle;
        return 0;
    }
    if (import_is(name, "_MPDeleteSemaphore")) {
        struct guest_semaphore *semaphore =
            guest_semaphore_acquire(arguments[0]);
        if (!semaphore) return (uint32_t)-1;
        semaphore->initialized = false;
        ++semaphore->generation;
        pthread_cond_broadcast(&semaphore->condition);
        pthread_mutex_unlock(&semaphore->mutex);
        __atomic_fetch_sub(&guest_semaphore_live_count, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (import_is(name, "_MPSignalSemaphore")) {
        struct guest_semaphore *semaphore =
            guest_semaphore_acquire(arguments[0]);
        if (!semaphore) return (uint32_t)-1;
        if (semaphore->count < semaphore->maximum) ++semaphore->count;
        pthread_cond_signal(&semaphore->condition);
        pthread_mutex_unlock(&semaphore->mutex);
        return 0;
    }
    if (import_is(name, "_MPWaitOnSemaphore")) {
        struct guest_semaphore *semaphore =
            guest_semaphore_acquire(arguments[0]);
        if (!semaphore) return (uint32_t)-1;
        uint32_t generation = semaphore->generation;
        /* Carbon Duration: 0 immediate, INT32_MAX forever, positive values
           milliseconds, negative values microseconds. Miles' mixer thread
           ticks on a finite wait, so a timeout must return kMPTimeoutErr. */
        int32_t duration = (int32_t)arguments[1];
        if (duration == 0 && semaphore->count <= 0) {
            pthread_mutex_unlock(&semaphore->mutex);
            return (uint32_t)-1;
        }
        uint64_t wait_ns = duration < 0 ? (uint64_t)-(int64_t)duration * 1000 :
                                          (uint64_t)duration * 1000000;
        uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + wait_ns;
        while (semaphore->initialized &&
               semaphore->generation == generation && semaphore->count <= 0) {
            if (duration == INT32_MAX) {
                pthread_cond_wait(&semaphore->condition, &semaphore->mutex);
                continue;
            }
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (now >= end) {
                pthread_mutex_unlock(&semaphore->mutex);
                return (uint32_t)-29296; /* kMPTimeoutErr */
            }
            struct timespec remaining = {(time_t)((end - now) / 1000000000),
                                         (long)((end - now) % 1000000000)};
            pthread_cond_timedwait_relative_np(&semaphore->condition,
                                               &semaphore->mutex, &remaining);
        }
        if (!semaphore->initialized || semaphore->generation != generation) {
            pthread_mutex_unlock(&semaphore->mutex);
            return (uint32_t)-1;
        }
        --semaphore->count;
        pthread_mutex_unlock(&semaphore->mutex);
        return 0;
    }

    if (import_is(name, "_OTAtomicSetBit") || import_is(name, "_OTAtomicClearBit") ||
        import_is(name, "_OTAtomicTestBit")) {
        uint8_t *byte=(void *)(uintptr_t)arguments[0];
        uint8_t mask=(uint8_t)(1u<<(arguments[1]&7));
        uint8_t old;
        if (import_is(name, "_OTAtomicSetBit")) old=__atomic_fetch_or(byte,mask,__ATOMIC_SEQ_CST);
        else if (import_is(name, "_OTAtomicClearBit")) old=__atomic_fetch_and(byte,(uint8_t)~mask,__ATOMIC_SEQ_CST);
        else old=__atomic_load_n(byte,__ATOMIC_SEQ_CST);
        return (old&mask)!=0;
    }
    if (import_is(name, "_MPCreateCriticalRegion")) {
        if (!arguments[0]) return (uint32_t)-50;
        uint32_t handle = guest_allocate(4, true);
        if (!handle || !host_mutex_for_guest(handle, true)) {
            if (handle) guest_deallocate(handle);
            return (uint32_t)-108;
        }
        *(uint32_t *)(uintptr_t)arguments[0] = handle; return 0;
    }
    if (import_is(name, "_MPDeleteCriticalRegion")) {
        uint32_t status = guest_mutex_destroy(arguments[0]);
        if (!status) guest_deallocate(arguments[0]);
        return status ? (uint32_t)-50 : 0;
    }
    if (import_is(name, "_MPEnterCriticalRegion") || import_is(name, "_MPExitCriticalRegion")) {
        pthread_mutex_t *mutex = host_mutex_for_guest(arguments[0], false);
        if (!mutex) return (uint32_t)-50;
        if (import_is(name, "_MPExitCriticalRegion")) return pthread_mutex_unlock(mutex) ? (uint32_t)-50 : 0;
        int32_t duration = (int32_t)arguments[1];
        if (duration == INT32_MAX) return pthread_mutex_lock(mutex) ? (uint32_t)-50 : 0;
        uint64_t wait_ns = duration < 0 ? (uint64_t)-(int64_t)duration * 1000 : (uint64_t)duration * 1000000;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t end = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec + wait_ns;
        for (;;) {
            int status = pthread_mutex_trylock(mutex);
            if (!status) return 0;
            if (status != EBUSY) return (uint32_t)-50;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t current = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
            if (current >= end) return (uint32_t)-29296; /* kMPTimeoutErr */
            struct timespec pause = {0, (long)(end - current < 1000000 ? end - current : 1000000)};
            nanosleep(&pause, NULL);
        }
    }
    if (import_is(name, "_OTAtomicAdd32"))
        return __atomic_add_fetch((uint32_t *)(uintptr_t)arguments[1],arguments[0],__ATOMIC_SEQ_CST);

    if (import_is(name, "_OTCompareAndSwap8")) {
        uint8_t expected=(uint8_t)arguments[0];
        return __atomic_compare_exchange_n((uint8_t *)(uintptr_t)arguments[2], &expected,
            (uint8_t)arguments[1], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    if (import_is(name, "_wcslen")) return wcslen((const wchar_t *)(uintptr_t)arguments[0]);
    if (import_is(name, "_wcscpy")) return (uint32_t)(uintptr_t)wcscpy(
        (wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1]);
    if (import_is(name, "_wcscat")) return (uint32_t)(uintptr_t)wcscat(
        (wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1]);
    if (import_is(name, "_wcschr")) return (uint32_t)(uintptr_t)wcschr(
        (const wchar_t *)(uintptr_t)arguments[0],(wchar_t)arguments[1]);
    if (import_is(name, "_wcsrchr")) return (uint32_t)(uintptr_t)wcsrchr(
        (const wchar_t *)(uintptr_t)arguments[0],(wchar_t)arguments[1]);
    if (import_is(name, "_wcsstr")) return (uint32_t)(uintptr_t)wcsstr(
        (const wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1]);
    if (import_is(name, "_wmemcmp")) return (uint32_t)wmemcmp(
        (const wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1],arguments[2]);
    /* wchar_t is 32 bits for both ABIs, so the memory forms pass through. */
    if (import_is(name, "_wmemcpy")) return (uint32_t)(uintptr_t)wmemcpy(
        (wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1],arguments[2]);
    if (import_is(name, "_wmemmove")) return (uint32_t)(uintptr_t)wmemmove(
        (wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1],arguments[2]);
    if (import_is(name, "_wmemset")) return (uint32_t)(uintptr_t)wmemset(
        (wchar_t *)(uintptr_t)arguments[0],(wchar_t)arguments[1],arguments[2]);
    if (import_is(name, "_wmemchr")) return (uint32_t)(uintptr_t)wmemchr(
        (const wchar_t *)(uintptr_t)arguments[0],(wchar_t)arguments[1],arguments[2]);
    /* clock_t and long are 32 bits in the guest; CLOCKS_PER_SEC matches. */
    if (import_is(name, "_clock")) return (uint32_t)clock();
    if (import_is(name, "_lrint") || import_is(name, "_lround")) {
        double value = guest_double(arguments);
        return (uint32_t)(int32_t)(import_is(name, "_lrint") ? lrint(value) : lround(value));
    }
    if (import_is(name, "_lrintf")) { float value; memcpy(&value, arguments, 4); return (uint32_t)(int32_t)lrintf(value); }
    if (import_is(name, "_putchar")) return (uint32_t)putchar((int)arguments[0]);
    if (import_is(name, "_vprintf")) {
        char *text = malloc(65536);
        const char *format = (const void *)(uintptr_t)arguments[0];
        const uint32_t *values = (const void *)(uintptr_t)arguments[1];
        int formatted = text && format && values ? guest_vformat(text, 65536, format, values) : -1;
        if (formatted >= 0) fputs(text, stdout);
        free(text);
        return (uint32_t)formatted;
    }
    /* struct utsname is five 256-byte strings in both ABIs. */
    if (import_is(name, "_uname")) return (uint32_t)uname((struct utsname *)(uintptr_t)arguments[0]);
    if (import_is(name, "_sysconf")) {
        long value = sysconf((int)arguments[0]);
        return (uint32_t)(value > INT32_MAX ? INT32_MAX : (int32_t)value);
    }
    if (import_is(name, "_pthread_key_delete")) {
        /* Guest keys are never reused (see _pthread_key_create). */
        return arguments[0] < 128 ? 0 : (uint32_t)EINVAL;
    }
    if (import_is(name, "_getifaddrs")) return guest_getifaddrs(arguments[0]);
    if (import_is(name, "_freeifaddrs")) { guest_deallocate(arguments[0]); return 0; }
    if (import_is(name, "_CC_MD5") || import_is(name, "_SHA1") || import_is(name, "_CC_SHA1")) {
        const void *data = (const void *)(uintptr_t)arguments[0];
        unsigned char *digest = (void *)(uintptr_t)arguments[2];
        static unsigned char shared_digest[CC_SHA1_DIGEST_LENGTH];
        unsigned char *out = digest ? digest : shared_digest; /* OpenSSL's NULL form */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (import_is(name, "_CC_MD5")) CC_MD5(data, arguments[1], out);
        else CC_SHA1(data, arguments[1], out);
#pragma clang diagnostic pop
        return digest ? arguments[2] : 0;
    }
    /* GCC's default unexpected handler terminates, like a pure virtual call. */
    if (import_is(name, "___cxa_pure_virtual") || import_is(name, "___cxa_call_unexpected") ||
        import_is(name, "_abort") || import_is(name, "___assert_rtn")) {
        if (import_is(name, "___assert_rtn"))
            fprintf(stderr, "compat32: guest assertion failed: %s, function %s, file %s, line %d\n",
                    (const char *)(uintptr_t)arguments[3], (const char *)(uintptr_t)arguments[0],
                    (const char *)(uintptr_t)arguments[1], (int)arguments[2]);
        else
            fprintf(stderr, "compat32: guest %s from 0x%08" PRIx32 "\n", name + 1, return_address);
        fflush(NULL);
        abort();
    }
    if (import_is(name, "_wcscmp")) return (uint32_t)wcscmp(
        (const wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1]);
    if (import_is(name, "_wcsncmp")) return (uint32_t)wcsncmp(
        (const wchar_t *)(uintptr_t)arguments[0],(const wchar_t *)(uintptr_t)arguments[1],arguments[2]);

    if (import_is(name, "_srandom")) { srandom(arguments[0]); return 0; }
    if (import_is(name, "_random")) return (uint32_t)random();

    if (import_is(name, "_TickCount")) {
        return (uint32_t)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000) * 60 / 1000);
    }

    if (import_is(name, "_Gestalt")) {
        int32_t *response = (void *)(uintptr_t)arguments[1];
        int32_t value;
        switch (arguments[0]) {
            case UINT32_C(0x7174696d): value = 0x07000000; break; /* qtim: AVFoundation movie bridge */
            case UINT32_C(0x73797376): value = 0x1075; break; /* sysv: packed OS version */
            case UINT32_C(0x73797331): value = 10; break; /* sys1: major */
            case UINT32_C(0x73797332): value = 7; break;  /* sys2: minor */
            case UINT32_C(0x73797333): value = 5; break;  /* sys3: bugfix */
            case UINT32_C(0x72616d6d): /* ramm: physical RAM in MiB */
            case UINT32_C(0x72616d20): /* ram : physical RAM in bytes */
            case UINT32_C(0x6c72616d): { /* lram: logical RAM in bytes */
                if (lp32_profile()->title != LP32_TITLE_MW3) return (uint32_t)-5551;
                uint64_t memory_bytes = 0;
                size_t memory_size = sizeof(memory_bytes);
                if (sysctlbyname("hw.memsize", &memory_bytes, &memory_size, NULL, 0) != 0 ||
                    memory_size != sizeof(memory_bytes) || !memory_bytes)
                    return (uint32_t)-5551;
                uint64_t scaled = arguments[0] == UINT32_C(0x72616d6d) ?
                    memory_bytes >> 20 : memory_bytes;
                value = (int32_t)(scaled > INT_MAX ? INT_MAX : scaled);
                break;
            }
            default: return (uint32_t)-5551;              /* gestaltUndefSelectorErr */
        }
        if (response) *response = value;
        return 0;
    }

    /* Matching dictionaries and registry properties follow the Create rule;
       IOServiceGetMatchingService(s) consumes the guest's reference. */
    if (import_is(name, "_IOBSDNameMatching")) {
        const char *bsd_name = (const char *)(uintptr_t)arguments[2];
        if (getenv("LP32_TRACE_FILES")) fprintf(stderr, "compat32: IOBSDNameMatching %s\n", bsd_name);
        CFMutableDictionaryRef dict = IOBSDNameMatching(arguments[0], arguments[1], bsd_name);
        uint32_t token = objc_bridge32_guest_owned_object(dict); if (dict) CFRelease(dict); return token;
    }
    if (import_is(name, "_IOServiceNameMatching") || import_is(name, "_IOServiceMatching")) {
        const char *match = (const char *)(uintptr_t)arguments[0];
        CFMutableDictionaryRef dict = import_is(name, "_IOServiceNameMatching") ?
            IOServiceNameMatching(match) : IOServiceMatching(match);
        uint32_t token = objc_bridge32_guest_owned_object(dict);
        if (dict) CFRelease(dict);
        return token;
    }
    if (import_is(name, "_IOServiceGetMatchingServices") || import_is(name, "_IOServiceGetMatchingService")) {
        CFDictionaryRef dict = objc_bridge32_host_object(arguments[1]);
        if (!dict) return import_is(name, "_IOServiceGetMatchingService") ? 0 : (uint32_t)kIOReturnBadArgument;
        CFRetain(dict); /* Matching consumes its argument. */
        uint32_t status = import_is(name, "_IOServiceGetMatchingService") ?
            IOServiceGetMatchingService(arguments[0], dict) :
            IOServiceGetMatchingServices(arguments[0], dict, (io_iterator_t *)(uintptr_t)arguments[2]);
        uint64_t ignored;
        uint32_t token = arguments[1];
        objc_bridge32_dispatch("_CFRelease", &token, &ignored);
        return status;
    }
    if (import_is(name, "_IOIteratorNext")) return IOIteratorNext(arguments[0]);
    if (import_is(name, "_IORegistryEntryFromPath"))
        return IORegistryEntryFromPath(arguments[0], (const char *)(uintptr_t)arguments[1]);
    if (import_is(name, "_IOIteratorIsValid")) return IOIteratorIsValid(arguments[0]);
    if (import_is(name, "_IOObjectRelease")) return IOObjectRelease(arguments[0]);
    if (import_is(name, "_IOObjectRetain")) return IOObjectRetain(arguments[0]);
    if (import_is(name, "_IOObjectConformsTo")) {
        const char *class_name = (const char *)(uintptr_t)arguments[1];
        bool conforms = IOObjectConformsTo(arguments[0], class_name);
        if (getenv("LP32_TRACE_FILES")) fprintf(stderr, "compat32: IOObjectConformsTo %#x %s -> %d\n", arguments[0], class_name, conforms);
        return conforms;
    }
    if (import_is(name, "_IORegistryEntryGetName")) return IORegistryEntryGetName(arguments[0], (char *)(uintptr_t)arguments[1]);
    if (import_is(name, "_IORegistryEntryGetParentEntry") && arguments[0]) return IORegistryEntryGetParentEntry(arguments[0], (const char *)(uintptr_t)arguments[1], (io_registry_entry_t *)(uintptr_t)arguments[2]);
    if (import_is(name, "_IORegistryEntryCreateIterator")) return IORegistryEntryCreateIterator(arguments[0], (const char *)(uintptr_t)arguments[1], arguments[2], (io_iterator_t *)(uintptr_t)arguments[3]);
    if (import_is(name, "_IORegistryEntryCreateCFProperty")) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(arguments[0], objc_bridge32_host_object(arguments[1]), NULL, arguments[3]);
        uint32_t token = objc_bridge32_guest_owned_object((void *)value); if (value) CFRelease(value); return token;
    }
    if (import_is(name, "_IORegistryEntryCreateCFProperties") && arguments[0]) {
        CFMutableDictionaryRef dict = NULL;
        kern_return_t r = IORegistryEntryCreateCFProperties(arguments[0], &dict, NULL, arguments[3]);
        if (arguments[1]) *(uint32_t *)(uintptr_t)arguments[1] = objc_bridge32_guest_owned_object(dict);
        if (dict) CFRelease(dict); return r;
    }
    if (import_is(name, "_IONotificationPortCreate") ||
        import_is(name, "_IONotificationPortGetRunLoopSource")) return 0;
    /* Creation above reports no legacy notification port. SDL also destroys
       that NULL result while unwinding its optional joystick setup. */
    if (import_is(name, "_IONotificationPortDestroy") && !arguments[0]) return 0;
    if (import_is(name, "_IODestroyPlugInInterface")) return 0;
    if (import_is(name, "_IOMasterPort")) {
        uint32_t *port = (void *)(uintptr_t)arguments[1];
        if (port) *port = 0;
        return 0;
    }
    if (import_is(name, "_IORegistryEntryCreateCFProperties")) {
        uint32_t *properties = (void *)(uintptr_t)arguments[1];
        if (properties) *properties = 0;
        return UINT32_C(0xe00002c7);
    }
    if (import_is(name, "_IORegistryEntryGetParentEntry")) {
        uint32_t *parent = (void *)(uintptr_t)arguments[2];
        if (parent) *parent = 0;
        return UINT32_C(0xe00002c7);
    }
    if (import_is(name, "_IOCreatePlugInInterfaceForService")) {
        uint32_t *plugin = (void *)(uintptr_t)arguments[3];
        uint32_t *score = (void *)(uintptr_t)arguments[4];
        if (plugin) *plugin = 0;
        if (score) *score = 0;
        return UINT32_C(0xe00002c7);
    }
    if (import_is(name, "_IOServiceAddInterestNotification") ||
        import_is(name, "_IOServiceAddMatchingNotification")) return UINT32_C(0xe00002c7);

    /* Guest process exit uses _Exit (above), so host atexit must never
       receive a 32-bit destructor. Static objects live until process exit. */
    if (import_is(name, "___cxa_atexit")) return 0;

    /* The guest exception unwinder is not implemented: throw/rethrow imports
       still trap before unwinding begins. Normal stream sentry destructors
       nevertheless query this flag. Do not read the host's unrelated C++
       exception state. Add a guest thread-local counter when implementing EH. */
    if (import_is(name, "__ZSt18uncaught_exceptionv")) return 0;

    if (import_is(name, "___cxa_guard_acquire")) {
        uint8_t *guard = (void *)(uintptr_t)arguments[0];
        if (*guard) return 0;
        guard[1] = 1;
        return 1;
    }
    if (import_is(name, "___cxa_guard_release")) {
        uint8_t *guard = (void *)(uintptr_t)arguments[0];
        guard[0] = 1;
        guard[1] = 0;
        return 0;
    }
    if (import_is(name, "___cxa_guard_abort")) {
        uint8_t *guard = (void *)(uintptr_t)arguments[0];
        guard[1] = 0;
        return 0;
    }

    /* Titles without the i386 _setjmp (guest_context_symbol) reach this one.
       Record the caller's frame in lp32_context_fast_setjmp's layout so
       objc_exception_throw can return to it. */
    if (import_is(name, "__setjmp")) {
        struct guest_resume_context frame;
        uint32_t *buffer = (void *)(uintptr_t)arguments[0];
        if (buffer && compat_runtime32_import_frame(&frame)) {
            buffer[0] = frame.ebx; buffer[1] = frame.ebp; buffer[2] = frame.esi;
            buffer[3] = frame.edi; buffer[4] = frame.esp; buffer[5] = frame.eip;
        }
        return 0;
    }

    return dispatch_bridge_stages(import_id, name, arguments, return_address,
                                  kImportStageUnknown);
}

/* Audio and Objective-C/OpenGL stages.  known_stage is the memoized stage for
   this import id, or kImportStageUnknown on the first call (or for a name the
   runtime chain matched, which is never memoized). */
static uint64_t dispatch_bridge_stages(uint32_t import_id, const char *name,
                                       const uint32_t *arguments,
                                       uint32_t return_address,
                                       uint8_t known_stage)
{
    uint64_t stage_start = compat_runtime32_frame_profile_enabled ?
        profile_now() : 0;
    uint64_t result = 0;
    if (known_stage != kImportStageObjC &&
        audio_bridge32_dispatch(name, arguments, &result)) {
        if (stage_start) {
            uint64_t elapsed = profile_now() - stage_start;
            frame_profile.audio_ns += elapsed;
            if (elapsed >= 1000000) {
                fprintf(stderr, "compat32: slow audio import %s %.2fms\n",
                        name, (double)elapsed / 1e6);
            }
        }
        if (known_stage == kImportStageUnknown && !dispatch_name_matched) {
            uint8_t *slot = import_stage_slot(import_id);
            if (slot) *slot = kImportStageAudio;
        }
        return result;
    }

    if (time_manager_bridge32_dispatch(name, arguments, &result)) return result;
    if (audio_queue_bridge32_dispatch(name, arguments, &result)) return result;
    if (audio_converter_bridge32_dispatch(name, arguments, &result)) return result;
    if (sound_manager_bridge32_dispatch(name, arguments, &result)) return result;
    if (strncmp(name, "_al", 3) == 0 && openal_bridge32_dispatch(name, arguments, &result)) return result;

    if (objc_bridge32_dispatch(name, arguments, &result)) {
        if (strcmp(name, "_CGDisplayModeGetRefreshRate") == 0) {
            double refresh;
            memcpy(&refresh, &result, sizeof(refresh));
            result = return_guest_double(refresh);
        }
        if (stage_start) frame_profile.objc_ns += profile_now() - stage_start;
        if (known_stage == kImportStageUnknown && !dispatch_name_matched) {
            uint8_t *slot = import_stage_slot(import_id);
            if (slot) *slot = kImportStageObjC;
        }
        return result;
    }

    fprintf(stderr,
            "compat32: trapped import[%" PRIu32 "] %s from 0x%08" PRIx32
            " args=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
            import_id, name, return_address, arguments[0], arguments[1],
            arguments[2], arguments[3]);
    if (getenv("LP32_FAIL_ON_UNSUPPORTED_IMPORT")) _Exit(78);
    last_call_trapped = 1;
    lp32_leave_guest = 1;
    return 0;
}

/* A direct (fast) handler could not complete its call: stop the guest as the
   named-import chain does for an unsupported call. */
void compat_runtime32_trap_import(const char *name, const uint32_t *arguments,
                                  uint32_t return_address)
{
    fprintf(stderr,
            "compat32: trapped import %s from 0x%08" PRIx32
            " args=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
            name, return_address, arguments[0], arguments[1], arguments[2], arguments[3]);
    if (getenv("LP32_FAIL_ON_UNSUPPORTED_IMPORT")) _Exit(78);
    last_call_trapped = 1;
    lp32_leave_guest = 1;
}

static void release_guest_stack_slot(void *opaque)
{
    if (!opaque) return;
    uint32_t slot = (uint32_t)((uintptr_t)opaque - 1);
    pthread_mutex_lock(&guest_stack_slot_lock);
    guest_stack_slots_in_use &= ~(UINT64_C(1) << slot);
    pthread_mutex_unlock(&guest_stack_slot_lock);
}

static void create_guest_stack_slot_key(void)
{
    (void)pthread_key_create(&guest_stack_slot_key, release_guest_stack_slot);
}

static bool acquire_guest_stack_slot(void)
{
    enum { kGuestStackSlotCount = kGuestStackSize / kGuestStackPerThread };
    pthread_once(&guest_stack_slot_key_once, create_guest_stack_slot_key);

    pthread_mutex_lock(&guest_stack_slot_lock);
    uint32_t slot = 0;
    while (slot < kGuestStackSlotCount &&
           (guest_stack_slots_in_use & (UINT64_C(1) << slot))) {
        ++slot;
    }
    if (slot == kGuestStackSlotCount) {
        pthread_mutex_unlock(&guest_stack_slot_lock);
        return false;
    }
    guest_stack_slots_in_use |= UINT64_C(1) << slot;
    pthread_mutex_unlock(&guest_stack_slot_lock);

    if (pthread_setspecific(guest_stack_slot_key,
                            (void *)(uintptr_t)(slot + 1)) != 0) {
        pthread_mutex_lock(&guest_stack_slot_lock);
        guest_stack_slots_in_use &= ~(UINT64_C(1) << slot);
        pthread_mutex_unlock(&guest_stack_slot_lock);
        return false;
    }
    guest_thread_slot = slot;
    return true;
}

static uint32_t call_guest(uint32_t function, const uint32_t *arguments,
                           size_t argument_count, float *float_result)
{
    const uintptr_t stack_slice_size = 0x10000;
    if (guest_thread_slot == UINT32_MAX && !acquire_guest_stack_slot()) {
        last_call_trapped = 1;
        return 0;
    }
    if (guest_thread_slot >= kGuestStackSize / kGuestStackPerThread ||
        (guest_call_depth + 1) * stack_slice_size > kGuestStackPerThread) {
        last_call_trapped = 1;
        return 0;
    }
    uintptr_t stack_top = guest_stack_base + kGuestStackSize -
                          guest_thread_slot * kGuestStackPerThread -
                          guest_call_depth * stack_slice_size;
    /* Entry goes through the i386 landing trampoline, whose `ret` pops the
       function address; the function then sees esp = sp + 4, which must be
       12 mod 16 as after a call. */
    uintptr_t maximum_sp = stack_top -
                           (argument_count + 2) * sizeof(uint32_t);
    uintptr_t sp = (maximum_sp & ~(uintptr_t)0x0f) | 0x08;
    if (sp > maximum_sp) sp -= 0x10;

    uint32_t *guest_stack = (void *)sp;
    guest_stack[0] = function;
    guest_stack[1] = kBridgeCodeBase + kExitThunkOffset;
    for (size_t index = 0; index < argument_count; ++index) {
        guest_stack[index + 2] = arguments[index];
    }

    last_call_trapped = 0;
    lp32_leave_guest = 0;
    ++guest_call_depth;
    lp32_guest_execution_enter();
    uint64_t pair = run_compat32(lp32_landing32, (uint32_t)sp, lp32_cs32);
    if (float_result && !last_call_trapped)
        __asm__ volatile("fstps %0" : "=m"(*float_result) : : "st");
    lp32_guest_execution_leave();
    --guest_call_depth;
    last_call_edx = (uint32_t)(pair >> 32);
    return (uint32_t)pair;
}

uint32_t compat_runtime32_call(uint32_t function, const uint32_t *arguments,
                               size_t argument_count)
{
    return call_guest(function, arguments, argument_count, NULL);
}

float compat_runtime32_call_float(uint32_t function, const uint32_t *arguments,
                                  size_t argument_count)
{
    float result = 0;
    call_guest(function, arguments, argument_count, &result);
    return result;
}

int compat_runtime32_last_call_trapped(void)
{
    return last_call_trapped;
}

uint32_t compat_runtime32_last_call_edx(void)
{
    return last_call_edx;
}

struct guest_heap_test_slot {
    uint32_t pointer;
    uint32_t size;
    uint8_t pattern;
};

struct guest_heap_test_worker {
    unsigned index;
    int failed;
};

static bool guest_heap_test_pattern(const struct guest_heap_test_slot *slot)
{
    if (!slot->pointer) return true;
    size_t count = slot->size < 16 ? slot->size : 16;
    const uint8_t *bytes = (const void *)(uintptr_t)slot->pointer;
    for (size_t index = 0; index < count; ++index) {
        if (bytes[index] != slot->pattern) return false;
    }
    return true;
}

static void *guest_heap_test_worker_main(void *opaque)
{
    struct guest_heap_test_worker *worker = opaque;
    struct guest_heap_test_slot slots[32] = {{0}};
    uint32_t random = UINT32_C(0x9e3779b9) ^ (worker->index * 0x10203u);
    uint32_t site = UINT32_C(0xff100000) + worker->index;
    for (unsigned iteration = 0; iteration < 25000; ++iteration) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        unsigned slot_index = random % 32;
        struct guest_heap_test_slot *slot = &slots[slot_index];
        if (slot->pointer) {
            if (!guest_heap_test_pattern(slot)) {
                worker->failed = 1;
                break;
            }
            guest_deallocate(slot->pointer);
            memset(slot, 0, sizeof(*slot));
        }
        uint32_t size = ((random >> 8) % 4096) + 1;
        uint32_t pointer = guest_allocate_at(size, false, site);
        if (!pointer) {
            worker->failed = 1;
            break;
        }
        uint8_t pattern = (uint8_t)(random | 1);
        memset((void *)(uintptr_t)pointer, pattern, size < 16 ? size : 16);
        slot->pointer = pointer;
        slot->size = size;
        slot->pattern = pattern;
        if ((iteration % 7) == 0) {
            uint32_t grown_size = size + 1 + ((random >> 20) % 4096);
            uint32_t grown = guest_reallocate(pointer, grown_size, site);
            if (!grown) {
                worker->failed = 1;
                break;
            }
            size_t preserved = size < 16 ? size : 16;
            const uint8_t *grown_bytes = (const void *)(uintptr_t)grown;
            for (size_t index = 0; index < preserved; ++index) {
                if (grown_bytes[index] != pattern) {
                    worker->failed = 1;
                    break;
                }
            }
            if (worker->failed) break;
            memset((void *)(uintptr_t)grown, pattern,
                   grown_size < 16 ? grown_size : 16);
            slot->pointer = grown;
            slot->size = grown_size;
        }
    }
    for (unsigned index = 0; index < 32; ++index) {
        if (slots[index].pointer) {
            if (!guest_heap_test_pattern(&slots[index])) worker->failed = 1;
            guest_deallocate(slots[index].pointer);
        }
    }
    return NULL;
}

static bool guest_heap_validate_free_lists(void)
{
    bool valid = true;
    uint64_t capacity_sum = 0;
    pthread_mutex_lock(&guest_heap_lock);
    uint64_t maximum_nodes = guest_heap_statistics.valid_frees +
                             guest_heap_statistics.split_blocks + 1;
    for (unsigned bin = 0; bin < kGuestHeapBinCount; ++bin) {
        uint32_t slow = guest_heap_free_bins[bin];
        uint32_t fast = slow;
        uint64_t count = 0;
        while (slow) {
            if (++count > maximum_nodes || slow < guest_heap_base ||
                (slow & 15) != 0 ||
                (uintptr_t)slow + kGuestHeapHeaderSize > guest_heap_cursor) {
                valid = false;
                break;
            }
            const uint32_t *metadata = (const void *)(uintptr_t)slow;
            if (metadata[1] != kGuestHeapFreeMagic || metadata[2] < 32 ||
                (metadata[2] & 15) != 0 ||
                guest_heap_bin_for_capacity(metadata[2]) != bin) {
                valid = false;
                break;
            }
            const uint32_t *footer = (const void *)(uintptr_t)(slow +
                kGuestHeapHeaderSize + metadata[2] - 8);
            if (footer[0] != kGuestHeapFreeMagic ||
                footer[1] != metadata[2]) {
                valid = false;
                break;
            }
            capacity_sum += metadata[2];
            slow = metadata[3];

            for (unsigned step = 0; step < 2 && fast; ++step) {
                if (fast < guest_heap_base || (fast & 15) != 0 ||
                    (uintptr_t)fast + kGuestHeapHeaderSize >
                        guest_heap_cursor) {
                    valid = false;
                    fast = 0;
                    break;
                }
                const uint32_t *fast_metadata =
                    (const void *)(uintptr_t)fast;
                if (fast_metadata[1] != kGuestHeapFreeMagic) {
                    valid = false;
                    fast = 0;
                    break;
                }
                fast = fast_metadata[3];
            }
            if (fast && slow == fast) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
    }
    if (capacity_sum != guest_heap_statistics.reusable_capacity) valid = false;
    pthread_mutex_unlock(&guest_heap_lock);
    return valid;
}

int compat_runtime32_run_heap_self_test(void)
{
    if (!guest_heap_reuse) {
        fputs("Guest heap self-test: FAIL (reuse is disabled)\n", stderr);
        return -1;
    }
    pthread_mutex_lock(&guest_heap_lock);
    uint64_t initial_live_blocks = guest_heap_statistics.live_blocks;
    uint64_t initial_live_requested = guest_heap_statistics.live_requested;
    uint64_t initial_invalid = guest_heap_statistics.invalid_frees;
    uint64_t initial_double = guest_heap_statistics.double_frees;
    uint64_t initial_reused = guest_heap_statistics.reused_allocations;
    pthread_mutex_unlock(&guest_heap_lock);

    enum { reuse_block_count = 4096 };
    uint32_t blocks[reuse_block_count];
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        blocks[index] = guest_allocate_at(256, false, UINT32_C(0xff200001));
        if (!blocks[index] || blocks[index] < current_image->max_address) goto failure;
        memset((void *)(uintptr_t)blocks[index], (int)(index & 0xff), 256);
    }
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        if (!guest_deallocate(blocks[index])) goto failure;
    }
    uintptr_t first_pass_high_water;
    pthread_mutex_lock(&guest_heap_lock);
    first_pass_high_water = guest_heap_statistics.high_water;
    pthread_mutex_unlock(&guest_heap_lock);
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        blocks[index] = guest_allocate_at(256, false, UINT32_C(0xff200002));
        if (!blocks[index]) goto failure;
    }
    pthread_mutex_lock(&guest_heap_lock);
    bool reuse_grew_cursor = guest_heap_statistics.high_water !=
                             first_pass_high_water;
    pthread_mutex_unlock(&guest_heap_lock);
    if (reuse_grew_cursor) goto failure;
    for (unsigned index = 0; index < reuse_block_count; ++index) {
        guest_deallocate(blocks[index]);
    }

    uint32_t data = guest_allocate_at(4096, false, UINT32_C(0xff200003));
    if (!data) goto failure;
    const char *zero_names[] = {"_bzero", "___bzero"};
    for (unsigned i = 0; i < 2; ++i) {
        memset((void *)(uintptr_t)data, 0xa5, 16);
        uint32_t zero_args[] = {data + 1, 14, 0, 0};
        dispatch_named_import(0, zero_names[i], zero_args, 0);
        const uint8_t *bytes = (const void *)(uintptr_t)data;
        if (bytes[0] != 0xa5 || bytes[15] != 0xa5) goto failure;
        for (unsigned j = 1; j < 15; ++j) if (bytes[j]) goto failure;
    }
    memcpy((void *)(uintptr_t)data, "__TEXT", 7);
    uint32_t segment_args[] = {data, 0, 0, 0};
    uint32_t text_segment = dispatch_named_import(0, "_getsegbyname", segment_args, 0);
    if (text_segment < current_image->min_address || text_segment >= current_image->max_address ||
        strcmp(((const struct segment_command *)(uintptr_t)text_segment)->segname, "__TEXT"))
        goto failure;
    memcpy((void *)(uintptr_t)data, "__NO_SUCH_SEG", 14);
    if (dispatch_named_import(0, "_getsegbyname", segment_args, 0)) goto failure;
    for (unsigned index = 0; index < 4096; ++index) {
        ((uint8_t *)(uintptr_t)data)[index] = (uint8_t)(index * 17);
    }
    uint32_t grown = guest_reallocate(data, 16384, UINT32_C(0xff200004));
    if (!grown) goto failure;
    for (unsigned index = 0; index < 4096; ++index) {
        if (((const uint8_t *)(uintptr_t)grown)[index] !=
            (uint8_t)(index * 17)) goto failure;
    }
    uint32_t shrunk = guest_reallocate(grown, 128, UINT32_C(0xff200005));
    if (shrunk != grown) goto failure;
    guest_deallocate(shrunk);

    uint32_t dirty = guest_allocate_at(8192, false, UINT32_C(0xff200006));
    if (!dirty) goto failure;
    memset((void *)(uintptr_t)dirty, 0xa5, 8192);
    guest_deallocate(dirty);
    uint32_t zeroed = guest_allocate_at(8192, true, UINT32_C(0xff200007));
    if (!zeroed) goto failure;
    for (unsigned index = 0; index < 8192; ++index) {
        if (((const uint8_t *)(uintptr_t)zeroed)[index] != 0) goto failure;
    }
    guest_deallocate(zeroed);

    uint32_t string_object =
        guest_allocate_at(sizeof(uint32_t), true, UINT32_C(0xff200008));
    if (!string_object ||
        !guest_string_construct(string_object, "seed", 4,
                                UINT32_C(0xff200009))) goto failure;
    for (unsigned iteration = 0; iteration < 50000; ++iteration) {
        char text[64];
        int length = snprintf(text, sizeof(text), "heap-string-%u", iteration);
        if (length < 0 || (size_t)length >= sizeof(text) ||
            !guest_string_assign(string_object, text, (size_t)length,
                                 UINT32_C(0xff20000a)) ||
            strcmp(guest_string_data(string_object), text) != 0) {
            goto failure;
        }
    }
    /* Replacement must tolerate its source aliasing the old COW buffer,
       and clamp npos to the remaining suffix. */
    if (!guest_string_assign(string_object, "abcdef", 6, 0)) goto failure;
    uint32_t replace_args[] = {string_object, 1, 3,
        *(uint32_t *)(uintptr_t)string_object + 2, 3};
    if (dispatch_named_import(0, "__ZNSs7replaceEmmPKcm", replace_args, 0) != string_object ||
        strcmp(guest_string_data(string_object), "acdeef")) goto failure;
    replace_args[1]=0;replace_args[2]=UINT32_MAX;
    replace_args[3]=*(uint32_t *)(uintptr_t)string_object+1;replace_args[4]=1;
    if (dispatch_named_import(0, "__ZNSs7replaceEmmPKcm", replace_args, 0) != string_object ||
        strcmp(guest_string_data(string_object), "c")) goto failure;
    guest_string_dispose_object(string_object);
    guest_deallocate(string_object);

    enum { worker_count = 4 };
    pthread_t threads[worker_count];
    struct guest_heap_test_worker workers[worker_count] = {{0}};
    unsigned started = 0;
    for (unsigned index = 0; index < worker_count; ++index) {
        workers[index].index = index;
        if (pthread_create(&threads[index], NULL,
                           guest_heap_test_worker_main, &workers[index]) != 0) {
            break;
        }
        ++started;
    }
    if (started != worker_count) {
        for (unsigned index = 0; index < started; ++index) {
            pthread_join(threads[index], NULL);
        }
        goto failure;
    }
    bool worker_failed = false;
    for (unsigned index = 0; index < worker_count; ++index) {
        pthread_join(threads[index], NULL);
        if (workers[index].failed) worker_failed = true;
    }
    if (worker_failed) goto failure;

    pthread_mutex_lock(&guest_heap_lock);
    bool counters_valid =
        guest_heap_statistics.live_blocks == initial_live_blocks &&
        guest_heap_statistics.live_requested == initial_live_requested &&
        guest_heap_statistics.invalid_frees == initial_invalid &&
        guest_heap_statistics.double_frees == initial_double &&
        guest_heap_statistics.reused_allocations > initial_reused;
    pthread_mutex_unlock(&guest_heap_lock);
    if (!counters_valid || !guest_heap_validate_free_lists()) goto failure;

    compat_runtime32_heap_report("self-test");
    puts("Guest heap self-test: PASS "
         "(reuse, split, realloc, calloc, 50000 strings, 4-thread churn)");
    return 0;

failure:
    compat_runtime32_heap_report("self-test-failure");
    fputs("Guest heap self-test: FAIL\n", stderr);
    return -1;
}

struct guest_file_test_worker {
    unsigned iterations;
    bool failed;
};

static void *guest_file_test_worker_main(void *opaque)
{
    struct guest_file_test_worker *worker = opaque;
    for (unsigned iteration = 0; iteration < worker->iterations; ++iteration) {
        FILE *file = fopen("/dev/null", "rb");
        uint32_t handle = guest_handle_for_file(file);
        if (!handle) {
            worker->failed = true;
            break;
        }
        FILE *locked = lock_host_file_for_guest(handle);
        if (!locked) {
            worker->failed = true;
            break;
        }
        (void)getc(locked);
        unlock_host_file();
        if (guest_close_file(handle) != 0) {
            worker->failed = true;
            break;
        }
    }
    return NULL;
}

int compat_runtime32_run_file_self_test(void)
{
    enum {
        sequential_iterations = 20000,
        worker_count = 4,
        worker_iterations = 5000,
    };
    uint32_t handles[kGuestFileCapacity] = {0};
    unsigned opened = 0;

    pthread_mutex_lock(&guest_file_lock);
    bool initially_empty = guest_file_active_count == 0;
    uint64_t initial_exhaustions = guest_file_exhaustion_count;
    pthread_mutex_unlock(&guest_file_lock);
    if (!initially_empty) goto failure;

    FILE *stale_file = fopen("/dev/null", "rb");
    uint32_t stale_handle = guest_handle_for_file(stale_file);
    if (!stale_handle || guest_close_file(stale_handle) != 0) goto failure;
    if (lock_host_file_for_guest(stale_handle)) {
        unlock_host_file();
        goto failure;
    }

    for (unsigned iteration = 0; iteration < sequential_iterations; ++iteration) {
        FILE *file = fopen("/dev/null", "rb");
        uint32_t handle = guest_handle_for_file(file);
        if (!handle || guest_close_file(handle) != 0) goto failure;
    }

    pthread_t threads[worker_count];
    struct guest_file_test_worker workers[worker_count] = {{0}};
    unsigned started = 0;
    for (unsigned index = 0; index < worker_count; ++index) {
        workers[index].iterations = worker_iterations;
        if (pthread_create(&threads[index], NULL,
                           guest_file_test_worker_main, &workers[index]) != 0) {
            break;
        }
        ++started;
    }
    if (started != worker_count) {
        for (unsigned index = 0; index < started; ++index) {
            pthread_join(threads[index], NULL);
        }
        goto failure;
    }
    bool worker_failed = false;
    for (unsigned index = 0; index < worker_count; ++index) {
        pthread_join(threads[index], NULL);
        if (workers[index].failed) worker_failed = true;
    }
    if (worker_failed) goto failure;

    /* Exercise true simultaneous capacity and prove the overflow FILE is
       closed by guest_handle_for_file rather than becoming unreachable. */
    for (; opened < kGuestFileCapacity; ++opened) {
        handles[opened] = guest_handle_for_file(fopen("/dev/null", "rb"));
        if (!handles[opened]) goto failure;
    }
    FILE *overflow = fopen("/dev/null", "rb");
    if (!overflow) goto failure;
    int overflow_descriptor = fileno(overflow);
    if (guest_handle_for_file(overflow) != 0) goto failure;
    errno = 0;
    if (fcntl(overflow_descriptor, F_GETFD) != -1 || errno != EBADF) {
        goto failure;
    }
    while (opened) {
        --opened;
        if (guest_close_file(handles[opened]) != 0) goto failure;
        handles[opened] = 0;
    }

    pthread_mutex_lock(&guest_file_lock);
    bool counters_valid = guest_file_active_count == 0 &&
                          guest_file_peak_count == kGuestFileCapacity &&
                          guest_file_exhaustion_count == initial_exhaustions + 1;
    uint32_t success_peak = guest_file_peak_count;
    pthread_mutex_unlock(&guest_file_lock);
    if (!counters_valid) goto failure;

    /* The scanf formats the engine's readers depend on, pcconfig.txt's
       version probe first (a scanset the guest scanner used to reject). */
    {
        /* Destinations must be guest (32-bit) addresses. */
        uint32_t cells = guest_allocate(64, true);
        if (!cells) goto failure;
        uint32_t *version = (uint32_t *)(uintptr_t)cells;
        char *token = (char *)(uintptr_t)(cells + 16);
        uint32_t destinations[2] = { cells, 0 };
        bool scan_ok =
            guest_vscan("FileVersion                 76\nScreenWidth 1920\n",
                        "FileVersion%*[ \n\t]%d", destinations) == 1 &&
            *version == 76;
        destinations[0] = cells + 16;
        destinations[1] = cells;
        scan_ok = scan_ok &&
            guest_vscan("key-name=42", "%[a-z-]=%u", destinations) == 2 &&
            strcmp(token, "key-name") == 0 && *version == 42;
        scan_ok = scan_ok &&
            guest_vscan("abc]def", "%[^]]", destinations) == 1 &&
            strcmp(token, "abc") == 0;
        scan_ok = scan_ok && guest_vscan("   ", "%[a-z]", destinations) == 0;
        destinations[0] = cells; destinations[1] = cells + 4;
        scan_ok = scan_ok && guest_vscan("00ff1234", "%2lx%2lx", destinations) == 2 &&
            version[0] == 0 && version[1] == 255;
        scan_ok = scan_ok && guest_vscan("aa03ff", "%*2x%2x", destinations) == 1 && version[0] == 3;
        size_t consumed = 0;
        scan_ok = scan_ok && guest_vscan_consumed("1.25more", "%3f%n", destinations, &consumed) == 1 &&
            fabsf(*(float *)version - 1.2f) < 0.0001f && version[1] == 3 && consumed == 3;
        guest_deallocate(cells);
        if (!scan_ok) goto failure;
    }

    /* A NULL, zero-capacity snprintf is a size query, used before allocating
       formatted keys. Truncation must report the full size and preserve guards. */
    {
        uint32_t memory = guest_allocate(128, true);
        if (!memory) goto failure;
        char *bytes = (void *)(uintptr_t)memory;
        strcpy(bytes, "%s.%d");
        strcpy(bytes + 16, "setting");
        uint32_t *values = (void *)(bytes + 48);
        values[0] = memory + 16; values[1] = 7;
        uint32_t args[] = {0, 0, memory, memory + 48};
        bool valid = dispatch_named_import(0, "_vsnprintf", args, 0) == 9;
        args[0] = memory + 64; args[1] = 5;
        memset(bytes + 64, 0x5a, 16);
        valid = valid && dispatch_named_import(0, "_vsnprintf", args, 0) == 9 &&
            strcmp(bytes + 64, "sett") == 0 && bytes[69] == 0x5a;
        guest_deallocate(memory);
        if (!valid) goto failure;
    }

    /* Direct handlers return what the chained implementations return. */
    {
        float x = 0.37f, y = 2.5f;
        double dx = -1.75, dy = 3.25;
        uint32_t fx, fy, dxw[2], dyw[2];
        memcpy(&fx, &x, 4); memcpy(&fy, &y, 4); memcpy(dxw, &dx, 8); memcpy(dyw, &dy, 8);
        uint32_t float_args[] = {fx, fy}, double_args[] = {dxw[0], dxw[1], dyw[0], dyw[1]};
        uint32_t wide[] = {0x89abcdefu, 0xfedcba98u, 7, 0};
        uint32_t memory = guest_allocate(128, true);
        if (!memory) goto failure;
        char *bytes = (void *)(uintptr_t)memory;
        strcpy(bytes, "%s-%u"); strcpy(bytes + 16, "abc"); strcpy(bytes + 24, "abd");
        uint32_t *values = (void *)(bytes + 48);
        values[0] = memory + 16; values[1] = 42;
        uint32_t format_args[] = {memory + 64, 32, memory, memory + 48};
        uint32_t compare_args[] = {memory + 16, memory + 24, 3};
        static const struct { const char *name; int kind; } cases[] = {
            {"_floorf", 1}, {"_ceilf", 1}, {"_acosf", 1}, {"_asinf", 1}, {"_sinf", 1},
            {"_cosf", 1}, {"_atanf", 1}, {"_roundf", 1}, {"_powf", 1}, {"_atan2f", 1},
            {"_fmodf", 1}, {"_floor", 2}, {"_ceil", 2}, {"_sin", 2}, {"_cos", 2},
            {"_pow", 2}, {"_atan2", 2}, {"_fmod", 2}, {"___divdi3", 3}, {"___moddi3", 3},
            {"___udivdi3", 3}, {"___umoddi3", 3}, {"_vsnprintf", 4}, {"_strncmp", 5},
        };
        bool valid = true;
        for (unsigned i = 0; valid && i < sizeof(cases) / sizeof(cases[0]); ++i) {
            lp32_fast_import_fn handler = runtime_fast_import(cases[i].name);
            const uint32_t *args = cases[i].kind == 1 ? float_args : cases[i].kind == 2 ?
                double_args : cases[i].kind == 3 ? wide : cases[i].kind == 4 ?
                format_args : compare_args;
            uint64_t expected = dispatch_named_import(0, cases[i].name, args, 0);
            union { float f; double d; } expected_fp = {0};
            memcpy(&expected_fp, (const void *)&lp32_fp_result, sizeof(expected_fp));
            int expected_kind = lp32_return_fp_kind;
            lp32_return_fp_kind = 0;
            uint64_t actual = handler ? handler(args, 0) : ~expected;
            valid = actual == expected && lp32_return_fp_kind == expected_kind &&
                    (!expected_kind || !memcmp(&expected_fp, (const void *)&lp32_fp_result,
                                                expected_kind == 1 ? 4 : 8));
            lp32_return_fp_kind = 0;
            if (!valid) fprintf(stderr, "runtime-selftest: direct %s differs\n", cases[i].name);
        }
        valid = valid && strcmp(bytes + 64, "abc-42") == 0;
        guest_deallocate(memory);
        if (!valid) goto failure;
    }

    /* Info_RemoveKey shifts the rest of an info string left in place with
       strcpy; objective positions follow the removed key. */
    {
        static const char info[] =
            "\\state\\1\\str\\ICBM_DESTROY_THE_POWER_TRANSMISSION\\org0\\-200 -16993 140\\icon\\7";
        uint32_t memory = guest_allocate(256, true);
        if (!memory) goto failure;
        char *bytes = (void *)(uintptr_t)memory;
        bool valid = true;
        for (uint32_t shift = 1; valid && shift < 48; ++shift) {
            memcpy(bytes, info, sizeof(info));
            uint32_t args[] = {memory, memory + shift};
            valid = (uint32_t)dispatch_named_import(0, "_strcpy", args, 0) == memory &&
                    strcmp(bytes, info + shift) == 0;
        }
        guest_deallocate(memory);
        if (!valid) goto failure;
    }

    /* Save timestamps use the 44-byte i386 tm layout, including normalized
       calendar fields. A sentinel catches accidental host-structure writes. */
    {
        uint32_t cell = guest_allocate(sizeof(struct guest_tm32) + 4, true);
        if (!cell) goto failure;
        struct guest_tm32 *date = (void *)(uintptr_t)cell;
        uint32_t *guard = (void *)(uintptr_t)(cell + sizeof(*date));
        *guard = 0x1234abcd;
        date->tm_year = 100;
        date->tm_mon = 2;
        date->tm_mday = 0; /* Normalize March 0 to February 29, 2000. */
        uint32_t args[] = {cell};
        uint64_t stamp = dispatch_named_import(0, "_timegm", args, 0);
        bool valid = stamp == 951782400 && date->tm_mon == 1 &&
                     date->tm_mday == 29 && date->tm_wday == 2 &&
                     date->tm_yday == 59 && date->tm_gmtoff == 0 &&
                     *guard == 0x1234abcd;
        guest_deallocate(cell);
        if (!valid) goto failure;
    }

    /* Sparse files catch high-word truncation without writing gigabytes.
       A guard verifies that the native stat structure never escapes to i386. */
    {
        FILE *file = tmpfile();
        uint32_t cell = guest_allocate(256, true);
        if (!file || !cell) {if(file)fclose(file); goto failure;}
        uint32_t *words = (void *)(uintptr_t)cell;
        words[0] = 0x12345678;
        uint32_t a[] = {(uint32_t)fileno(file), cell, 4, 17, 1};
        bool valid = dispatch_named_import(0, "_pwrite$UNIX2003", a, 0) == 4;
        words[0] = 0;
        valid = valid && dispatch_named_import(0, "_pread", a, 0) == 4 && words[0] == 0x12345678;
        struct guest_stat_inode64 *st = (void *)(uintptr_t)(cell + 16);
        uint32_t *guard = (void *)(st + 1); *guard = 0xabcdef01;
        uint32_t stat_args[] = {(uint32_t)fileno(file), cell + 16};
        valid = valid && dispatch_named_import(0, "_fstat$INODE64", stat_args, 0) == 0 &&
            st->size == INT64_C(4294967317) && *guard == 0xabcdef01;
        fclose(file);
        guest_deallocate(cell);
        if (!valid) goto failure;
    }

    {
        uint32_t a[4] = {0};
        uint32_t cell = (uint32_t)compat_runtime32_dispatch_import("___error", a);
        if (!cell) goto failure;
        *(int32_t *)(uintptr_t)cell = 0;
        a[0] = UINT32_MAX;
        if ((int32_t)compat_runtime32_dispatch_import("_close", a) != -1 ||
            *(int32_t *)(uintptr_t)cell != EBADF) goto failure;
        *(int32_t *)(uintptr_t)cell = EDOM;
        a[0] = (uint32_t)compat_runtime32_dispatch_import("___error", a);
        if (a[0] != cell || *(int32_t *)(uintptr_t)cell != EDOM) goto failure;
    }

    printf("Guest FILE bridge self-test: PASS "
           "(%u sequential, %u-thread churn, %u simultaneous, peak=%" PRIu32 ")\n",
           sequential_iterations, worker_count, kGuestFileCapacity, success_peak);
    return 0;

failure:
    while (opened) {
        --opened;
        if (handles[opened]) (void)guest_close_file(handles[opened]);
    }
    pthread_mutex_lock(&guest_file_lock);
    uint32_t active = guest_file_active_count;
    uint32_t failure_peak = guest_file_peak_count;
    uint64_t exhausted = guest_file_exhaustion_count;
    pthread_mutex_unlock(&guest_file_lock);
    fprintf(stderr,
            "Guest FILE bridge self-test: FAIL "
            "(active=%" PRIu32 " peak=%" PRIu32 " exhaustions=%" PRIu64 ")\n",
            active, failure_peak, exhausted);
    return -1;
}

uint32_t compat_runtime32_copy_cstring(const char *string)
{
    if (!string) return 0;
    size_t size = strlen(string) + 1;
    uint32_t destination = guest_allocate(size, false);
    if (destination) memcpy((void *)(uintptr_t)destination, string, size);
    return destination;
}

uint32_t compat_runtime32_allocate(size_t size, int clear)
{
    return guest_allocate(size, clear != 0);
}

uint32_t compat_runtime32_reallocate(uint32_t pointer, size_t size)
{
    return guest_reallocate(pointer, size, kGuestHeapHostSite);
}

void compat_runtime32_deallocate(uint32_t pointer)
{
    guest_deallocate(pointer);
}

uint64_t compat_runtime32_dispatch_import(const char *name,
                                          const uint32_t *arguments)
{
    uint64_t result = dispatch_named_import(UINT32_MAX, name, arguments,
                                           kGuestHeapHostSite);
    /* Host-side callers never return through the import gateway, which is
       what consumes a pending float result. */
    lp32_return_fp_kind = 0;
    return result;
}

int compat_runtime32_run_ctype_self_test(void)
{
    const uint8_t *locale = (const void *)(uintptr_t)
        (kBridgeDataBase + kGuestRuneLocaleOffset);
    /* Match the inlined i386 isdigit load used by Marvel's #var parser. */
    for (unsigned character = 0; character < 256; ++character) {
        bool digit = (locale[0x35 + character * 4] & 4) != 0;
        if (digit != (character >= '0' && character <= '9')) goto failure;
        int32_t lower, upper;
        memcpy(&lower, locale + 0x434 + character * 4, sizeof(lower));
        memcpy(&upper, locale + 0x834 + character * 4, sizeof(upper));
        if (lower != _DefaultRuneLocale.__maplower[character] ||
            upper != _DefaultRuneLocale.__mapupper[character]) goto failure;
    }
    for (uint32_t index = 0; index < current_image->import_count; ++index) {
        if (strcmp(current_image->imports[index].name, "__DefaultRuneLocale")) continue;
        uint32_t bound = *(uint32_t *)(uintptr_t)current_image->imports[index].address;
        if (bound != kBridgeDataBase + kGuestRuneLocaleOffset) goto failure;
    }
    const int32_t runes[] = {'A', '9', ' ', 0x7f, 0xc3, 0xa9, 0xff, -1, 0x391, 0x4e2d};
    const uint32_t masks[] = {_CTYPE_A, _CTYPE_D, _CTYPE_S, _CTYPE_A | _CTYPE_D, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(runes) / sizeof(runes[0]); ++i) {
        for (unsigned j = 0; j < sizeof(masks) / sizeof(masks[0]); ++j) {
            uint32_t args[] = {(uint32_t)runes[i], masks[j]};
            if ((uint32_t)compat_runtime32_dispatch_import("___maskrune", args) !=
                (uint32_t)__maskrune(runes[i], masks[j])) goto failure;
        }
    }
    const struct lp32_code_signature *compare = &lp32_profile()->server_name_compare;
    if (compare->address) {
        if (memcmp((void *)(uintptr_t)compare->address, compare->expected, compare->length)) goto failure;
        uint32_t first = compat_runtime32_copy_cstring("^1\xc3\xa9" "Alpha");
        uint32_t second = compat_runtime32_copy_cstring("^2Zulu");
        if (!first || !second) goto failure;
        bool valid = true;
        /* Execute the unmodified LAN_CompareHostname from the MP executable.
           Exercise the UTF-8 sequence from the report, then every high-bit
           byte in both operands, repeatedly as during server-list refresh. */
        for (unsigned iteration = 0; valid && iteration <= 4096; ++iteration) {
            if (iteration) {
                char *text = (void *)(uintptr_t)first;
                text[2] = (char)(0x80 + (iteration - 1) % 128);
                text[3] = 'A'; text[4] = 0;
            }
            uint32_t args[] = {first, second};
            valid = (int32_t)compat_runtime32_call(compare->address, args, 2) < 0 &&
                    !compat_runtime32_last_call_trapped();
            args[0] = second; args[1] = first;
            valid &= (int32_t)compat_runtime32_call(compare->address, args, 2) > 0 &&
                     !compat_runtime32_last_call_trapped();
            args[0] = first;
            valid &= compat_runtime32_call(compare->address, args, 2) == 0 &&
                     !compat_runtime32_last_call_trapped();
        }
        compat_runtime32_deallocate(first);
        compat_runtime32_deallocate(second);
        if (!valid) goto failure;
        puts("COD4 MP hostname self-test: PASS (12,291 original comparator calls, UTF-8 and every high-bit byte)");
    }
    uint32_t text1 = compat_runtime32_copy_cstring("#var float4 tint : C16");
    uint32_t text2 = compat_runtime32_copy_cstring("alpha,beta");
    uint32_t spaces = compat_runtime32_copy_cstring(" ");
    uint32_t comma = compat_runtime32_copy_cstring(",");
    uint32_t states = compat_runtime32_allocate(12, 1);
    if (!text1 || !text2 || !spaces || !comma || !states) goto failure;
    uint32_t *cells = (void *)(uintptr_t)states;
    cells[2] = 0x51a7cafe;
    uint32_t args[3] = {text1, spaces, states};
    uint32_t token = (uint32_t)compat_runtime32_dispatch_import("_strtok_r", args);
    bool valid = token && !strcmp((char *)(uintptr_t)token, "#var");
    args[0] = text2; args[1] = comma; args[2] = states + 4;
    token = (uint32_t)compat_runtime32_dispatch_import("_strtok_r", args);
    valid &= token && !strcmp((char *)(uintptr_t)token, "alpha");
    args[0] = 0; args[1] = spaces; args[2] = states;
    token = (uint32_t)compat_runtime32_dispatch_import("_strtok_r", args);
    valid &= token && !strcmp((char *)(uintptr_t)token, "float4");
    args[0] = 0; args[1] = comma; args[2] = states + 4;
    token = (uint32_t)compat_runtime32_dispatch_import("_strtok_r", args);
    valid &= token && !strcmp((char *)(uintptr_t)token, "beta");
    token = (uint32_t)compat_runtime32_dispatch_import("_strtok_r", args);
    valid &= token == 0 && cells[2] == 0x51a7cafe;
    compat_runtime32_deallocate(text1);
    compat_runtime32_deallocate(text2);
    compat_runtime32_deallocate(spaces);
    compat_runtime32_deallocate(comma);
    compat_runtime32_deallocate(states);
    if (!valid) goto failure;
    puts("ctype self-test: PASS (i386 inline classification, maskrune, case maps, interleaved strtok_r)");
    return 0;
failure:
    fputs("ctype self-test: FAIL\n", stderr);
    return -1;
}

/* Compiles more programs than the retired 4,096-entry handle table held,
 * exercising parameter reflection and string lifetimes while programs are
 * destroyed in waves, as they are during a campaign. */
int compat_runtime32_run_cg_self_test(void)
{
    enum {
        program_total = 6000,
        live_window = 512,
        CG_SOURCE = 4112,
        CG_PROGRAM = 4109,
        CG_COMPILED_PROGRAM = 4106,
        CG_PROFILE_ARBVP1 = 6150,
    };
    static const char source_text[] =
        "struct Output { float4 position : POSITION; float4 color : COLOR; };\n"
        "Output main(float4 position : POSITION, float4 color : COLOR,\n"
        "            uniform float4x4 modelViewProjection,\n"
        "            uniform float4 tintColor : C16, uniform float4 fogParameters)\n"
        "{\n"
        "    Output result;\n"
        "    result.position = mul(modelViewProjection, position);\n"
        "    result.color = color * tintColor + fogParameters;\n"
        "    return result;\n"
        "}\n";

    if (!ensure_cg_library()) {
        fputs("Cg bridge self-test: SKIP (bundled Cg unavailable)\n", stderr);
        return 0;
    }

    uint32_t source = compat_runtime32_copy_cstring(source_text);
    uint32_t entry = compat_runtime32_copy_cstring("main");
    uint32_t *programs = calloc(live_window, sizeof(*programs));
    uint32_t *first_names = calloc(live_window, sizeof(*first_names));
    if (!source || !entry || !programs || !first_names) {
        fputs("Cg bridge self-test: FAIL (allocation)\n", stderr);
        free(programs);
        free(first_names);
        return -1;
    }

    uint32_t arguments[8] = {0};
    uint32_t context = (uint32_t)compat_runtime32_dispatch_import(
        "_cgCreateContext", arguments);
    if (!context) {
        fputs("Cg bridge self-test: FAIL (cgCreateContext returned 0)\n", stderr);
        free(programs);
        free(first_names);
        return -1;
    }

    uint32_t created = 0, destroyed = 0, parameters_seen = 0;
    uint32_t minimum_parameters = UINT32_MAX;
    uint32_t largest_handle = 0;
    uint32_t strings_after_first_wave = 0;
    uint32_t string_churn = 0;
    const char *failure = NULL;

    for (uint32_t index = 0; index < program_total && !failure; ++index) {
        uint32_t slot = index % live_window;
        if (programs[slot]) {
            arguments[0] = programs[slot];
            compat_runtime32_dispatch_import("_cgDestroyProgram", arguments);
            programs[slot] = 0;
            ++destroyed;
        }
        arguments[0] = context;
        arguments[1] = CG_SOURCE;
        arguments[2] = source;
        arguments[3] = CG_PROFILE_ARBVP1;
        arguments[4] = entry;
        arguments[5] = 0;
        uint32_t program = (uint32_t)compat_runtime32_dispatch_import(
            "_cgCreateProgram", arguments);
        if (!program) {
            failure = "cgCreateProgram returned 0";
            break;
        }
        programs[slot] = program;
        ++created;
        if (program > largest_handle) largest_handle = program;

        arguments[0] = program;
        arguments[1] = CG_COMPILED_PROGRAM;
        uint32_t listing = (uint32_t)compat_runtime32_dispatch_import(
            "_cgGetProgramString", arguments);
        if (!listing || strncmp((const char *)(uintptr_t)listing, "!!ARBvp1.0",
                                10) != 0) {
            failure = "cgGetProgramString did not return an ARBvp1 listing";
            break;
        }

        arguments[0] = program;
        arguments[1] = CG_PROGRAM;
        uint32_t parameter = (uint32_t)compat_runtime32_dispatch_import(
            "_cgGetFirstParameter", arguments);
        if (!parameter) {
            failure = "cgGetFirstParameter returned 0";
            break;
        }
        uint32_t count = 0;
        bool saw_tint = false;
        while (parameter) {
            arguments[0] = parameter;
            uint32_t name = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetParameterName", arguments);
            if (!name) {
                failure = "cgGetParameterName returned 0";
                break;
            }
            uint32_t again = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetParameterName", arguments);
            if (again != name) {
                failure = "cgGetParameterName guest pointer is unstable";
                break;
            }
            if (strcmp((const char *)(uintptr_t)name, "tintColor") == 0) {
                saw_tint = true;
                if (index == 0) {
                    uint32_t semantic = (uint32_t)compat_runtime32_dispatch_import(
                        "_cgGetParameterSemantic", arguments);
                    uint32_t direction = (uint32_t)compat_runtime32_dispatch_import(
                        "_cgGetParameterDirection", arguments);
                    uint32_t is_parameter = (uint32_t)compat_runtime32_dispatch_import(
                        "_cgIsParameter", arguments);
                    arguments[1] = program;
                    uint32_t used = (uint32_t)compat_runtime32_dispatch_import(
                        "_cgIsParameterUsed", arguments);
                    if (!semantic || strcmp((char *)(uintptr_t)semantic, "C16") ||
                        direction != 4097 || !is_parameter || !used) {
                        failure = "shader semantic/direction/usage reflection failed";
                        break;
                    }
                }
            }
            if (count == 0) first_names[slot] = name;
            ++count;
            parameter = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetNextParameter", arguments);
        }
        if (failure) break;
        if (!saw_tint) {
            failure = "parameter enumeration lost tintColor";
            break;
        }
        parameters_seen += count;
        if (count < minimum_parameters) minimum_parameters = count;

        if (index + 1 == live_window) {
            strings_after_first_wave = compat_runtime32_cg_string_count();
        }
    }

    if (!failure) {
        /* Names of the first program in the window must still read back
           correctly after thousands of recycles through the same slots. */
        for (uint32_t slot = 0; slot < live_window; ++slot) {
            if (!programs[slot]) continue;
            arguments[0] = programs[slot];
            arguments[1] = CG_PROGRAM;
            uint32_t parameter = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetFirstParameter", arguments);
            arguments[0] = parameter;
            uint32_t name = (uint32_t)compat_runtime32_dispatch_import(
                "_cgGetParameterName", arguments);
            if (name != first_names[slot]) ++string_churn;
        }
    }

    uint32_t strings_at_end = compat_runtime32_cg_string_count();
    uint32_t pointer_handles = compat_runtime32_cg_object_count();
    for (uint32_t slot = 0; slot < live_window; ++slot) {
        if (!programs[slot]) continue;
        arguments[0] = programs[slot];
        compat_runtime32_dispatch_import("_cgDestroyProgram", arguments);
        ++destroyed;
    }
    free(programs);
    free(first_names);

    fprintf(stderr,
            "Cg bridge self-test: created=%" PRIu32 " destroyed=%" PRIu32
            " parameters=%" PRIu32 " min-per-program=%" PRIu32
            " largest-handle=0x%" PRIx32 " pointer-handles=%" PRIu32
            " strings(after-first-wave=%" PRIu32 " end=%" PRIu32
            ") name-churn=%" PRIu32 "\n",
            created, destroyed, parameters_seen, minimum_parameters,
            largest_handle, pointer_handles, strings_after_first_wave,
            strings_at_end, string_churn);
    if (failure) {
        fprintf(stderr, "Cg bridge self-test: FAIL (%s after %" PRIu32
                " programs)\n", failure, created);
        return -1;
    }
    if (created != program_total || minimum_parameters < 5) {
        fputs("Cg bridge self-test: FAIL (incomplete run)\n", stderr);
        return -1;
    }
    /* Interned names must not allocate fresh guest strings per program, and
       compiled listings must be released with their program, so the table
       should be no larger than it was once the live window first filled. */
    if (strings_at_end > strings_after_first_wave + 64) {
        fputs("Cg bridge self-test: FAIL (guest string cache grows unbounded)\n",
              stderr);
        return -1;
    }
    fputs("Cg bridge self-test: PASS\n", stderr);
    return 0;
}

/*
 * Synchronization self-test.  Creates and destroys more guest mutexes,
 * condition variables, and MP semaphores than the retired fixed tables held,
 * verifies host objects are recycled rather than leaked, and checks that
 * stale semaphore handles are rejected once their slot is reused.
 */
struct sync_self_test_waiter {
    uint32_t semaphore;
    uint32_t condition;
    uint32_t mutex;
    uint32_t result;
    uint32_t ready;
};

static void *sync_self_test_semaphore_waiter(void *opaque)
{
    struct sync_self_test_waiter *waiter = opaque;
    uint32_t arguments[4] = {waiter->semaphore, UINT32_C(0x7fffffff), 0, 0};
    __atomic_store_n(&waiter->ready, 1, __ATOMIC_RELEASE);
    waiter->result = (uint32_t)compat_runtime32_dispatch_import(
        "_MPWaitOnSemaphore", arguments);
    return NULL;
}

static void *sync_self_test_condition_waiter(void *opaque)
{
    struct sync_self_test_waiter *waiter = opaque;
    uint32_t arguments[4] = {waiter->mutex, 0, 0, 0};
    compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments);
    __atomic_store_n(&waiter->ready, 1, __ATOMIC_RELEASE);
    arguments[0] = waiter->condition;
    arguments[1] = waiter->mutex;
    waiter->result = (uint32_t)compat_runtime32_dispatch_import(
        "_pthread_cond_wait", arguments);
    arguments[0] = waiter->mutex;
    compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments);
    return NULL;
}

static uint32_t sync_self_test_free_list_length(struct host_sync_free_node *node)
{
    uint32_t length = 0;
    while (node) {
        ++length;
        node = node->next;
    }
    return length;
}

struct pure_atomic_test_job {
    uint32_t cells, iterations;
    bool publish;
    int failure;
};

static void *pure_atomic_test_host(void *opaque)
{
    struct pure_atomic_test_job *job = opaque;
    uint32_t *cells = (void *)(uintptr_t)job->cells;
    if (!job->publish) {
        for (uint32_t i = 0; i < job->iterations; ++i)
            __atomic_add_fetch(cells, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + UINT64_C(10000000000);
    for (uint32_t i = 1; i <= job->iterations; ++i) {
        while (__atomic_load_n(cells, __ATOMIC_SEQ_CST) != 0) {
            if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) > deadline) {
                job->failure = 1;
                return NULL;
            }
            sched_yield();
        }
        cells[1] = i;
        __atomic_store_n(cells, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

int compat_runtime32_run_pure_atomic_self_test(void)
{
    uint32_t abi = macho_image32_find_symbol(current_image, "_atomic_abi");
    uint32_t loop = macho_image32_find_symbol(current_image, "_atomic_add_loop");
    uint32_t pointer = macho_image32_find_symbol(current_image, "_atomic_pointer");
    uint32_t consume = macho_image32_find_symbol(current_image, "_atomic_consume");
    uint32_t cells = guest_allocate(128, true);
    if (!abi || !loop || !pointer || !consume || !cells) return -1;
    uint32_t *values = (void *)(uintptr_t)cells;
    uint32_t args[] = {cells, 50000};
    bool valid = compat_runtime32_call(abi, args, 1) == 0 && !last_call_trapped;
    const char *const aliases[] = {
        "_OSAtomicAdd32", "_OSAtomicAdd32Barrier",
        "_OSAtomicCompareAndSwap32", "_OSAtomicCompareAndSwapInt",
        "_OSAtomicCompareAndSwap32Barrier", "_OSAtomicCompareAndSwapPtrBarrier",
    };
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        uint32_t callback = compat_runtime32_guest_callback(aliases[i]);
        strcpy((char *)(uintptr_t)(cells + 64), aliases[i] + 1);
        uint32_t lookup[] = {UINT32_C(0xfffffffe), cells + 64};
        valid &= compat_runtime32_dispatch_import("_dlsym", lookup) == callback;
        valid &= callback != 0 && compat_runtime32_resolve_symbol(aliases[i], 0) == callback;
        uint32_t call_args[] = {0, cells, 0};
        if (i >= 2) { call_args[0] = 7; call_args[1] = 7; call_args[2] = cells; }
        valid &= compat_runtime32_call(callback, call_args, i < 2 ? 2 : 3) == (i < 2 ? 7 : 1) &&
                 !last_call_trapped;
    }
    uint32_t bound = compat_runtime32_call(pointer, NULL, 0);
    valid &= bound != 0;
    uint32_t pointer_args[] = {0, cells};
    valid &= compat_runtime32_call(bound, pointer_args, 2) == 7 && !last_call_trapped;
    if (pure_import_symbol("_OSAtomicAdd32"))
        valid &= bound == compat_runtime32_guest_callback("_OSAtomicAdd32");
    values[0] = 0;
    struct pure_atomic_test_job job = {cells, args[1], false, 0};
    pthread_t host;
    if (pthread_create(&host, NULL, pure_atomic_test_host, &job)) return -1;
    compat_runtime32_call(loop, args, 2);
    valid &= !last_call_trapped;
    pthread_join(host, NULL);
    valid &= values[0] == 2 * args[1];
    memset(values, 0, 32);
    args[1] = 2000;
    job = (struct pure_atomic_test_job){cells, args[1], true, 0};
    if (pthread_create(&host, NULL, pure_atomic_test_host, &job)) return -1;
    valid &= compat_runtime32_call(consume, args, 2) == 0 && !last_call_trapped;
    pthread_join(host, NULL);
    valid &= !job.failure && values[0] == 0 && values[1] == args[1] && values[2] == 0;
    values[0] = 0;
    args[1] = 1000000;
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    compat_runtime32_call(loop, args, 2);
    uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    valid &= !last_call_trapped && values[0] == args[1];
    fprintf(stderr, "pure-atomic-selftest: %s, route=%s, actual i386 Add32 %.1f ns/call\n",
            valid ? "PASS" : "FAIL", pure_import_symbol("_OSAtomicAdd32") ? "inline" : "gateway",
            (double)elapsed / args[1]);
    guest_deallocate(cells);
    return valid ? 0 : -1;
}

static int guest_once_abi_self_test(void)
{
    uint32_t cells = guest_allocate(32, true);
    uint32_t allocation = guest_allocate(8192, true);
    if (!cells || !allocation) return -1;
    uint32_t address = (allocation + 4095) & ~UINT32_C(4095);
    uint8_t *code = (void *)(uintptr_t)address;
    if (mprotect(code, 4096, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    struct guest_once_control *control = (void *)(uintptr_t)cells;
    uint32_t *count = (void *)(uintptr_t)(cells + 8);
    *control = (struct guest_once_control){ .signature = 0x30b1bcba };
    /* i386 callback: increment the guest counter and return. */
    uint8_t initializer[] = {0xff, 0x05, 0, 0, 0, 0, 0xc3};
    uint32_t counter = cells + 8;
    memcpy(initializer + 2, &counter, 4);
    memcpy(code + 64, initializer, sizeof(initializer));
    /* Align the stack, push initializer/control, and call the import thunk. */
    uint8_t caller[] = {0x83, 0xec, 0x04, 0x68, 0, 0, 0, 0,
                       0x68, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0,
                       0x83, 0xc4, 0x0c, 0xc3};
    uint32_t initialize = address + 64;
    uint32_t thunk = compat_runtime32_guest_callback("_pthread_once");
    int32_t relative = (int32_t)(thunk - (address + 18));
    memcpy(caller + 4, &initialize, 4);
    memcpy(caller + 9, &cells, 4);
    memcpy(caller + 14, &relative, 4);
    memcpy(code, caller, sizeof(caller));
    bool valid = true;
    for (unsigned generation = 0; generation < 2; ++generation) {
        if (generation) *control = (struct guest_once_control){ .signature = 0x30b1bcba };
        for (unsigned call = 0; call < 3; ++call) {
            uint32_t result = compat_runtime32_call(address, NULL, 0);
            valid &= !last_call_trapped && result == 0 && *count == generation + 1 &&
                     control->signature == 0x30b1bcba && control->state == UINT32_MAX;
        }
    }
    /* Completed controls still reject a missing initializer in either route. */
    uint32_t invalid[] = {cells, 0};
    valid &= fast_pthread_once(invalid, 0) == EINVAL &&
             compat_runtime32_dispatch_import("_pthread_once", invalid) == EINVAL;
    guest_deallocate(allocation);
    guest_deallocate(cells);
    fprintf(stderr, "guest-once-abi-selftest: %s\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : -1;
}

int compat_runtime32_run_sync_self_test(void)
{
    if (getenv("LP32_ONCE_SELFTEST")) return guest_once_abi_self_test();
    if (getenv("LP32_IMPORT_BENCH")) {  /* TEMPORARY: import round-trip cost */
        uint32_t block = compat_runtime32_allocate(8192, 1);
        uint32_t code = (block + 4095) & ~4095u;
        uint32_t thunk = compat_runtime32_guest_callback(getenv("LP32_IMPORT_BENCH"));
        uint8_t bytes[] = {0x53, 0xbb, 0,0,0,0, 0x6a,0x00, 0xe8,0,0,0,0, 0x83,0xc4,0x04, 0x4b, 0x75,0xf3, 0x5b, 0xc3};
        uint32_t count = 1000000; memcpy(bytes + 2, &count, 4);
        int32_t delta = (int32_t)(thunk - (code + 13)); memcpy(bytes + 9, &delta, 4);
        memcpy((void *)(uintptr_t)code, bytes, sizeof(bytes));
        mprotect((void *)(uintptr_t)code, 4096, PROT_READ | PROT_EXEC);
        compat_runtime32_call(code, NULL, 0);
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        compat_runtime32_call(code, NULL, 0);
        uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
        struct rusage usage; getrusage(RUSAGE_SELF, &usage);
        fprintf(stderr, "import bench %s: %.0f ns/call (process sys=%.2fs user=%.2fs)\n",
                getenv("LP32_IMPORT_BENCH"), elapsed / 1e6,
                usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6,
                usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6);
        return 0;
    }
    enum {
        object_total = 8192,
        semaphore_total = kGuestSemaphoreCapacity - 1,
        mutex_base = UINT32_C(0x40000000),
        condition_base = UINT32_C(0x50000000),
    };
    uint32_t arguments[4] = {0};
    const char *failure = NULL;

    /* Execute real i386 callbacks to test pthread identity across the ABI,
     * including a suspended thread resumed through its Mach thread port. */
    uint32_t thread_cells = guest_allocate(32, true);
    uint8_t *thread_code = mmap((void *)0x76000000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (thread_code == MAP_FAILED || (uintptr_t)thread_code > UINT32_MAX || !thread_cells) return -1;
    uint32_t self_thunk = compat_runtime32_guest_callback("_pthread_self");
    uint8_t code[] = {0x83,0xec,0x0c,0xe8,0,0,0,0,0x83,0xc4,0x0c,0xc3};
    int32_t relative = (int32_t)(self_thunk - ((uintptr_t)thread_code + 8));
    memcpy(code + 4, &relative, 4); memcpy(thread_code, code, sizeof(code));
    for (unsigned i = 0; i < 100; ++i) {
        uint32_t create[] = {thread_cells, 0, (uint32_t)(uintptr_t)thread_code, thread_cells + 16};
        if (compat_runtime32_dispatch_import(i == 99 ? "_pthread_create_suspended_np" : "_pthread_create", create)) return -1;
        uint32_t token = *(uint32_t *)(uintptr_t)thread_cells;
        if (token < 0x1000 || *(uint32_t *)(uintptr_t)token != 0x54485244 ||
            *(uint32_t *)(uintptr_t)(token + 4) != 0) return -1;
        if (i == 99) {
            uint32_t port = (uint32_t)compat_runtime32_dispatch_import("_pthread_mach_thread_np", &token);
            if (!port || compat_runtime32_dispatch_import("_thread_resume", &port)) return -1;
        }
        uint32_t join[] = {token, thread_cells + 4};
        if (compat_runtime32_dispatch_import("_pthread_join", join) ||
            *(uint32_t *)(uintptr_t)(thread_cells + 4) != token || host_thread(token)) return -1;
    }
    /* A guest thread ending through pthread_exit, as MW2's _endthreadex does,
       delivers its value to pthread_join and releases its token. */
    uint32_t exit_thunk = compat_runtime32_guest_callback("_pthread_exit");
    uint8_t exit_code[] = {0x83,0xec,0x08,0x68,0x34,0x12,0,0,0xe8,0,0,0,0,0x0f,0x0b};
    relative = (int32_t)(exit_thunk - ((uintptr_t)thread_code + 13));
    memcpy(exit_code + 9, &relative, 4); memcpy(thread_code, exit_code, sizeof(exit_code));
    for (unsigned i = 0; i < 20; ++i) {
        uint32_t create[] = {thread_cells, 0, (uint32_t)(uintptr_t)thread_code, thread_cells + 16};
        if (compat_runtime32_dispatch_import("_pthread_create", create)) return -1;
        uint32_t token = *(uint32_t *)(uintptr_t)thread_cells;
        uint32_t join[] = {token, thread_cells + 4};
        if (compat_runtime32_dispatch_import("_pthread_join", join) ||
            *(uint32_t *)(uintptr_t)(thread_cells + 4) != 0x1234 || host_thread(token)) return -1;
    }
    /* div_t is an eight-byte register return, including a signed remainder. */
    uint32_t div_thunk=compat_runtime32_guest_callback("_div");
    uint8_t div_code[]={0x8b,0x44,0x24,0x04,0x8b,0x54,0x24,0x08,
        0x83,0xec,0x0c,0x89,0x04,0x24,0x89,0x54,0x24,0x04,
        0xe8,0,0,0,0,0x83,0xc4,0x0c,0xc3};
    relative=(int32_t)(div_thunk-((uintptr_t)thread_code+23));
    memcpy(div_code+19,&relative,4);memcpy(thread_code,div_code,sizeof(div_code));
    const int32_t divisions[][4]={{17,5,3,2},{-17,5,-3,-2},{17,-5,-3,2}};
    for(unsigned i=0;i<3;++i) {
        uint32_t a[]={(uint32_t)divisions[i][0],(uint32_t)divisions[i][1]};
        if((int32_t)compat_runtime32_call((uint32_t)(uintptr_t)thread_code,a,2)!=divisions[i][2] ||
           (int32_t)compat_runtime32_last_call_edx()!=divisions[i][3])return -1;
    }
    munmap(thread_code, 4096); guest_deallocate(thread_cells);
    const uint32_t durations[] = {0, 250, (uint32_t)-750, INT32_MAX};
    const uint64_t nanoseconds[] = {0, 250000000, 750000, UINT64_MAX};
    for (unsigned i = 0; i < 4; ++i)
        if (compat_runtime32_dispatch_import("_DurationToNanoseconds", &durations[i]) != nanoseconds[i]) return -1;

    /* Both low- and high-address requests must reject a range crossing 4GB
       before touching mappings. These would include this native executable. */
    const uint32_t crossing_starts[] = {0x70000000, 0xfffff000};
    const uint32_t crossing_sizes[] = {0xb0000000, 0x2000};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t map[] = {crossing_starts[i], crossing_sizes[i], PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON | MAP_FIXED, UINT32_MAX, 0, 0};
        if ((uint32_t)compat_runtime32_dispatch_import("_mmap", map) != UINT32_MAX) return -1;
        uint32_t unmap[] = {crossing_starts[i], crossing_sizes[i]};
        if ((int32_t)compat_runtime32_dispatch_import("_munmap", unmap) != -1) return -1;
    }

    /* The guest arena: host allocations stay above 4 GB, guest mmap comes
       from the arena, munmap keeps the pages reserved, and a freed range is
       handed out again. */
    if (guest_arena_ready) {
        void *host = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        bool host_low = host != MAP_FAILED && (uintptr_t)host < UINT64_C(0x100000000);
        if (host != MAP_FAILED) munmap(host, 1 << 20);
        if (host_low) return -1;
        uint32_t map[] = {0, 0x200000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, UINT32_MAX, 0, 0};
        uint32_t first = (uint32_t)compat_runtime32_dispatch_import("_mmap", map);
        if (first == UINT32_MAX || first < kGuestArenaStart) return -1;
        memset((void *)(uintptr_t)first, 0x5a, 0x200000);
        uint32_t unmap[] = {first, 0x200000};
        if (compat_runtime32_dispatch_import("_munmap", unmap)) return -1;
        mach_vm_address_t probe = first; mach_vm_size_t probe_size = 0;
        vm_region_basic_info_data_64_t probe_info; mach_msg_type_number_t probe_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t probe_object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &probe, &probe_size, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&probe_info, &probe_count, &probe_object) ||
            probe > first || probe_info.protection != VM_PROT_NONE) return -1;
        map[0] = first;
        uint32_t again = (uint32_t)compat_runtime32_dispatch_import("_mmap", map);
        if (again != first || *(volatile uint8_t *)(uintptr_t)again != 0) return -1;
        compat_runtime32_dispatch_import("_munmap", unmap);
    }

    /* The command-overflow dump reads SV_AddServerCommand's frame, then
       tail-calls its Com_Error target with the original arguments. */
    {
        uint8_t *code = mmap((void *)0x76200000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        uint32_t client = guest_allocate(0x1210, true);
        if (code == MAP_FAILED || !client) return -1;
        uint32_t base = (uint32_t)(uintptr_t)code;
        uint32_t thunk = compat_runtime32_guest_callback("_lp32_mw2_command_overflow");
        uint8_t frame_code[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08, 0xc7, 0x04, 0x24, 1, 0, 0, 0,
            0xc7, 0x44, 0x24, 0x04, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0x89, 0xec, 0x5d, 0xc3};
        uint32_t format = base + 0x80;
        int32_t relative = (int32_t)(thunk - (base + 26));
        memcpy(frame_code + 17, &format, 4);
        memcpy(frame_code + 22, &relative, 4);
        memcpy(code, frame_code, sizeof(frame_code));
        const uint8_t target[] = {0x8b, 0x44, 0x24, 0x04, 0x83, 0xc0, 0x40, 0xc3}; /* return arg0 + 0x40 */
        memcpy(code + 0x40, target, sizeof(target));
        uint32_t *fields = (void *)(uintptr_t)client;
        fields[1] = 10; fields[2] = 2; fields[3] = 0;
        memcpy((char *)(uintptr_t)client + 0x10, "cmd1\0cmd2", 10);
        fields[(0x1010 / 4) + 2] = 5;
        uint32_t previous_target = mw2_command_overflow_target;
        mw2_command_overflow_target = base + 0x40;
        uint32_t call_arguments[] = {client, base + 0x90};
        memcpy(code + 0x90, "next", 5);
        uint32_t result = compat_runtime32_call(base, call_arguments, 2);
        mw2_command_overflow_target = previous_target;
        munmap(code, 4096);
        guest_deallocate(client);
        if (compat_runtime32_last_call_trapped() || result != 0x41) return -1;
    }

    /* Guest stacks sit where 32-bit macOS put thread stacks, so a stale
       stack pointer read as a double (MW2's Scr_Objective_Position formats
       int zeros with %.2f) is tiny, not a 300-digit number. */
    if (guest_arena_ready) {
        uint8_t *code = mmap((void *)0x76300000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (code == MAP_FAILED) return -1;
        const uint8_t read_esp[] = {0x89, 0xe0, 0xc3}; /* mov eax, esp; ret */
        memcpy(code, read_esp, sizeof(read_esp));
        uint32_t esp = compat_runtime32_call((uint32_t)(uintptr_t)code, NULL, 0);
        munmap(code, 4096);
        if (esp < kNativeGuestStackBase || esp >= kNativeGuestStackBase + kGuestStackSize) return -1;
        uint32_t stale[] = {0, 0, 0, 1, esp}; /* three int zeros, then stale words */
        char text[128];
        if (guest_vformat(text, sizeof(text), "%.2f %.2f %.2f", stale) < 0 || strlen(text) > 20) return -1;
    }

    /* Windows-style swprintf(L"%hs"), CoreAudio device queries across the
       ABI, and Bink's Memory Manager calls. */
    uint32_t scratch = guest_allocate(4096, true);
    if (!scratch) return -1;
    wchar_t *wide = (void *)(uintptr_t)scratch;
    wcscpy(wide + 256, L"%hs-%d");
    strcpy((char *)(uintptr_t)(scratch + 2048), "abc");
    uint32_t swprintf_arguments[] = {scratch, 64, scratch + 1024, scratch + 2048, 7};
    if (compat_runtime32_dispatch_import("_swprintf", swprintf_arguments) != 5 || wcscmp(wide, L"abc-7")) return -1;
    uint32_t *audio = (void *)(uintptr_t)(scratch + 3072);
    audio[0] = kAudioHardwarePropertyDefaultOutputDevice;
    audio[1] = kAudioObjectPropertyScopeGlobal;
    audio[2] = kAudioObjectPropertyElementMain;
    audio[3] = 4;
    uint32_t device_query[] = {kAudioObjectSystemObject, scratch + 3072, 0, 0, scratch + 3084, scratch + 3088};
    if (compat_runtime32_dispatch_import("_AudioObjectGetPropertyData", device_query) == 0 && audio[4]) {
        uint32_t device = audio[4];
        audio[0] = kAudioObjectPropertyName;
        audio[3] = 4;
        uint32_t name_query[] = {device, scratch + 3072, 0, 0, scratch + 3084, scratch + 3088};
        if (compat_runtime32_dispatch_import("_AudioObjectGetPropertyData", name_query) || audio[3] != 4 ||
            audio[4] < 0x1000) return -1;
        audio[0] = kAudioDevicePropertyStreamConfiguration;
        audio[1] = kAudioObjectPropertyScopeOutput;
        uint32_t size_query[] = {device, scratch + 3072, 0, 0, scratch + 3084};
        if (compat_runtime32_dispatch_import("_AudioObjectGetPropertyDataSize", size_query) ||
            audio[3] < 16 || (audio[3] - 4) % 12) return -1;
    }
    int16_t *error = (void *)(uintptr_t)(scratch + 3500);
    uint32_t temp_arguments[] = {100, scratch + 3500};
    uint32_t handle = (uint32_t)compat_runtime32_dispatch_import("_TempNewHandle", temp_arguments);
    if (!handle || *error) return -1;
    uint32_t dispose_arguments[] = {handle, scratch + 3500};
    compat_runtime32_dispatch_import("_TempDisposeHandle", dispose_arguments);
    uint32_t library_arguments[] = {0, 0, 0, scratch + 3504, scratch + 3508, 0};
    if ((int32_t)compat_runtime32_dispatch_import("_GetSharedLibrary", library_arguments) != -2804 || *error) return -1;
    guest_deallocate(scratch);

    pthread_mutex_lock(&guest_sync_table_lock);
    uint32_t initial_mutexes = guest_mutex_count;
    uint32_t initial_conditions = guest_cond_count;
    pthread_mutex_unlock(&guest_sync_table_lock);

    /* Mutexes: init, lock/unlock, destroy, then re-create the same number and
       make sure the second wave came from the free list. */
    for (uint32_t pass = 0; pass < 2 && !failure; ++pass) {
        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = mutex_base + index * 64;
            arguments[1] = 0;
            if (compat_runtime32_dispatch_import("_pthread_mutex_init", arguments)) {
                failure = "pthread_mutex_init failed";
                break;
            }
            if (compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments) ||
                compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments)) {
                failure = "pthread_mutex lock/unlock failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        uint32_t live = guest_mutex_count;
        uint32_t free_nodes = sync_self_test_free_list_length(free_host_mutexes);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_mutexes + object_total) {
            failure = "mutex registry count mismatch";
            break;
        }
        if (pass == 1 && free_nodes != 0) {
            failure = "second mutex wave did not drain the free list";
            break;
        }
        /* A locked mutex must survive destroy (EBUSY) instead of being
           recycled underneath its owner. */
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments);
        if (compat_runtime32_dispatch_import("_pthread_mutex_destroy", arguments) !=
            (uint32_t)EBUSY) {
            failure = "destroying a locked mutex did not report EBUSY";
            break;
        }
        compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments);
        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = mutex_base + index * 64;
            if (compat_runtime32_dispatch_import("_pthread_mutex_destroy", arguments)) {
                failure = "pthread_mutex_destroy failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        live = guest_mutex_count;
        free_nodes = sync_self_test_free_list_length(free_host_mutexes);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_mutexes || free_nodes != object_total) {
            failure = "mutex destroy did not release every entry";
            break;
        }
    }

    /* Condition variables: same shape, plus a real wait/signal round trip. */
    for (uint32_t pass = 0; pass < 2 && !failure; ++pass) {
        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = condition_base + index * 64;
            arguments[1] = 0;
            if (compat_runtime32_dispatch_import("_pthread_cond_init", arguments)) {
                failure = "pthread_cond_init failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        uint32_t live = guest_cond_count;
        uint32_t free_nodes =
            sync_self_test_free_list_length(free_host_conditions);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_conditions + object_total ||
            (pass == 1 && free_nodes != 0)) {
            failure = "condition registry count mismatch";
            break;
        }

        struct sync_self_test_waiter waiter = {
            .condition = condition_base, .mutex = mutex_base,
        };
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_init", arguments);
        pthread_t thread;
        if (pthread_create(&thread, NULL, sync_self_test_condition_waiter,
                           &waiter) != 0) {
            failure = "pthread_create failed";
            break;
        }
        while (!__atomic_load_n(&waiter.ready, __ATOMIC_ACQUIRE)) sched_yield();
        /* Take the mutex so the waiter is guaranteed to be inside cond_wait. */
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_lock", arguments);
        arguments[0] = condition_base;
        if (pass == 0) {
            compat_runtime32_dispatch_import("_pthread_cond_signal", arguments);
        } else {
            /* Destroying a waited-on condition must release the waiter rather
               than strand it on a recycled host object. */
            compat_runtime32_dispatch_import("_pthread_cond_destroy", arguments);
        }
        arguments[0] = mutex_base;
        compat_runtime32_dispatch_import("_pthread_mutex_unlock", arguments);
        pthread_join(thread, NULL);
        if (waiter.result != 0) {
            failure = "pthread_cond_wait round trip failed";
            break;
        }
        compat_runtime32_dispatch_import("_pthread_mutex_destroy", arguments);

        for (uint32_t index = 0; index < object_total; ++index) {
            arguments[0] = condition_base + index * 64;
            if (compat_runtime32_dispatch_import("_pthread_cond_destroy", arguments)) {
                failure = "pthread_cond_destroy failed";
                break;
            }
        }
        if (failure) break;
        pthread_mutex_lock(&guest_sync_table_lock);
        live = guest_cond_count;
        free_nodes = sync_self_test_free_list_length(free_host_conditions);
        pthread_mutex_unlock(&guest_sync_table_lock);
        if (live != initial_conditions || free_nodes != object_total) {
            failure = "condition destroy did not release every entry";
            break;
        }
    }

    /* Semaphores: fill the table, delete everything, refill, and confirm the
       first wave's handles are rejected even though their slots were reused. */
    uint32_t *first_wave = calloc(semaphore_total, sizeof(*first_wave));
    uint32_t *second_wave = calloc(semaphore_total, sizeof(*second_wave));
    uint32_t output = compat_runtime32_allocate(sizeof(uint32_t), 1);
    uint32_t stale_rejections = 0;
    uint32_t initial_semaphores =
        __atomic_load_n(&guest_semaphore_live_count, __ATOMIC_RELAXED);
    if (!first_wave || !second_wave || !output) failure = "allocation";
    if (!failure && initial_semaphores != 0) {
        failure = "semaphores already live before test";
    }
    for (uint32_t wave = 0; wave < 2 && !failure; ++wave) {
        uint32_t *handles = wave ? second_wave : first_wave;
        for (uint32_t index = 0; index < semaphore_total; ++index) {
            arguments[0] = 1;
            arguments[1] = 0;
            arguments[2] = output;
            if (compat_runtime32_dispatch_import("_MPCreateSemaphore", arguments) ||
                !*(uint32_t *)(uintptr_t)output) {
                failure = "MPCreateSemaphore failed";
                break;
            }
            handles[index] = *(uint32_t *)(uintptr_t)output;
        }
        if (failure) break;
        arguments[0] = 1;
        arguments[1] = 0;
        arguments[2] = output;
        if (compat_runtime32_dispatch_import("_MPCreateSemaphore", arguments) !=
            (uint32_t)-1) {
            failure = "table overflow was not reported";
            break;
        }
        if (wave == 1) {
            for (uint32_t index = 0; index < semaphore_total; ++index) {
                arguments[0] = first_wave[index];
                if (compat_runtime32_dispatch_import("_MPSignalSemaphore",
                                                     arguments) == (uint32_t)-1) {
                    ++stale_rejections;
                }
            }
            if (stale_rejections != semaphore_total) {
                failure = "stale semaphore handle was accepted";
                break;
            }
        }
        /* Signal/wait round trip and a blocked waiter released by delete. */
        arguments[0] = handles[0];
        if (compat_runtime32_dispatch_import("_MPSignalSemaphore", arguments) ||
            compat_runtime32_dispatch_import("_MPSignalSemaphore", arguments)) {
            failure = "MPSignalSemaphore failed";
            break;
        }
        arguments[1] = 0;
        if (compat_runtime32_dispatch_import("_MPWaitOnSemaphore", arguments) ||
            compat_runtime32_dispatch_import("_MPWaitOnSemaphore", arguments) !=
            (uint32_t)-1) {
            failure = "semaphore count/maximum not honoured";
            break;
        }
        struct sync_self_test_waiter waiter = {.semaphore = handles[1]};
        pthread_t thread;
        if (pthread_create(&thread, NULL, sync_self_test_semaphore_waiter,
                           &waiter) != 0) {
            failure = "pthread_create failed";
            break;
        }
        while (!__atomic_load_n(&waiter.ready, __ATOMIC_ACQUIRE)) sched_yield();
        usleep(20000);
        if (wave == 0) {
            arguments[0] = handles[1];
            compat_runtime32_dispatch_import("_MPSignalSemaphore", arguments);
            pthread_join(thread, NULL);
            if (waiter.result != 0) {
                failure = "blocked semaphore waiter was not released by signal";
                break;
            }
        } else {
            arguments[0] = handles[1];
            compat_runtime32_dispatch_import("_MPDeleteSemaphore", arguments);
            pthread_join(thread, NULL);
            if (waiter.result != (uint32_t)-1) {
                failure = "blocked semaphore waiter was not released by delete";
                break;
            }
            handles[1] = 0;
        }
        for (uint32_t index = 0; index < semaphore_total; ++index) {
            if (!handles[index]) continue;
            arguments[0] = handles[index];
            if (compat_runtime32_dispatch_import("_MPDeleteSemaphore", arguments)) {
                failure = "MPDeleteSemaphore failed";
                break;
            }
        }
        if (failure) break;
        if (__atomic_load_n(&guest_semaphore_live_count, __ATOMIC_RELAXED) != 0) {
            failure = "semaphore live count did not return to zero";
            break;
        }
    }
    free(first_wave);
    free(second_wave);
    if (output) compat_runtime32_deallocate(output);

    if (failure) {
        fprintf(stderr, "Sync bridge self-test: FAIL (%s)\n", failure);
        return -1;
    }
    fprintf(stderr,
            "Sync bridge self-test: PASS (100 i386 pthread identities/resume/join; mutexes=%d conditions=%d semaphores=%d"
            " stale-rejections=%" PRIu32 " released=%" PRIu64 ")\n",
            object_total, object_total, semaphore_total, stale_rejections,
            guest_sync_released_count);
    return 0;
}
