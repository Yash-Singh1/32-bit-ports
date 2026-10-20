#ifndef LP32_COMPAT_RUNTIME_H
#define LP32_COMPAT_RUNTIME_H

#include "macho_loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t compat_runtime32_return_double(double value);
int compat_runtime32_initialize(struct macho_image32 *image);
/* The loaded guest image (valid after compat_runtime32_initialize). */
const struct macho_image32 *compat_runtime32_image(void);
uint32_t compat_runtime32_call(uint32_t function, const uint32_t *arguments,
                               size_t argument_count);
int compat_runtime32_last_call_trapped(void);
/* edx of the last compat_runtime32_call on this thread (pair results). */
uint32_t compat_runtime32_last_call_edx(void);
uint32_t compat_runtime32_copy_cstring(const char *string);
uint32_t compat_runtime32_allocate(size_t size, int clear);
uint32_t compat_runtime32_reallocate(uint32_t pointer, size_t size);
void compat_runtime32_deallocate(uint32_t pointer);
void compat_runtime32_heap_report(const char *reason);
void compat_runtime32_heap_frame(uint64_t swap_count);
int compat_runtime32_run_heap_self_test(void);
int compat_runtime32_run_file_self_test(void);
int compat_runtime32_run_cg_self_test(void);
int compat_runtime32_run_ctype_self_test(void);
int compat_runtime32_run_sync_self_test(void);
int compat_runtime32_run_pure_atomic_self_test(void);
int compat_runtime32_run_import_return_self_test(void);
uint32_t compat_runtime32_cg_object_count(void);
uint32_t compat_runtime32_cg_string_count(void);
/* Reserve the free parts of 0x80000000-0xffffffff for guest mmap; call
   first thing in main, before host allocators can claim them. */
void compat_runtime32_reserve_guest_arena(void);
uint64_t compat_runtime32_dispatch_import(const char *name,
                                          const uint32_t *arguments);

/* i386 register state for resuming guest code somewhere other than an
   import's return address (exception landing pads, longjmp targets). */
struct guest_resume_context {
    uint32_t ebx, ebp, esi, edi, esp, eip, eax, edx;
};
/* The guest frame that called the import now being dispatched: callee-saved
   registers, esp just past the return address, eip = return address. False
   for imports served by a direct (fast) handler. */
bool compat_runtime32_import_frame(struct guest_resume_context *frame);
/* When the current import returns, continue the guest at this context. Call
   it last: a nested guest call would consume it. */
void compat_runtime32_resume_guest(const struct guest_resume_context *context);

/* A guest-callable (cdecl) entry point that dispatches to the host under the
   given import name, for function pointers the game expects to call back
   (the same mechanism dlsym results use).  0 if the thunk table is full. */
uint32_t compat_runtime32_guest_callback(const char *name);
/* See LP32_TRACE_CONSOLE_PRINT in game_loader.c. */
void compat_runtime32_set_trace_print_argument(unsigned argument);
/* Com_Error, which _lp32_mw2_command_overflow continues into. */
void compat_runtime32_set_command_overflow_target(uint32_t com_error);

/* Guest callback standing in for Feral's Content.loader MainEntry (returns 1
   so the title proceeds to its own main). */
#define kFeralMainEntryCallbackName "_lp32_feral_main_entry"

/* Per-thread bridge cost counters, accumulated only while
   LP32_FRAME_STATS is set.  The render thread reads and clears them once
   per swap. */
struct compat_runtime32_frame_profile {
    uint64_t calls;
    uint64_t dispatch_ns;
    uint64_t audio_ns;
    uint64_t objc_ns;
    uint64_t lock_wait_ns;
    uint64_t lock_waits;
};
extern int compat_runtime32_frame_profile_enabled;
void compat_runtime32_take_frame_profile(
    struct compat_runtime32_frame_profile *out);
void compat_runtime32_report_import_profile(unsigned top);

/* Mode guards: how many times a thread arrived at a 64-bit pad still in
   i386 mode (to64) or at the i386 landing trampoline still in x86_64 mode
   (to32) and was transparently redirected.  See build_transition_bridge. */
void compat_runtime32_mode_guard_counts(uint32_t *recovered_to64,
                                        uint32_t *recovered_to32);
void compat_runtime32_check_mode_guards(uint64_t swap_count);

/* Non-zero once the game tried to read its pcconfig.txt and found none
   (a first launch, so no launcher-chosen resolution exists yet). */
int compat_runtime32_game_config_missing(void);
int compat_runtime32_pointer_import_matches(uint32_t address, const char *name);
int compat_runtime32_guest_symbol_matches(uint32_t address, const char *name);
void compat_runtime32_set_diagnostic_sink(void (*sink)(const char *line));

/*
 * Direct handlers for the hottest imports.  The bridges match import names
 * with long if-chains; at 25-30 thousand imports per frame the chain walk
 * alone is a tenth of the render thread.  A bridge may offer a handler for a
 * name, and the runtime memoizes it per import id after the first (chained)
 * dispatch, so later calls skip the chains entirely.  Handlers receive the
 * i386 argument words and return the eax:edx value; float results go through
 * the same lp32_fp_result path as the chained handlers.
 */
typedef uint64_t (*lp32_fast_import_fn)(const uint32_t *arguments,
                                        uint32_t return_address);
void compat_runtime32_trap_import(const char *name, const uint32_t *arguments,
                                  uint32_t return_address);

uint32_t compat_runtime32_resolve_symbol(const char *name, int data_hint);

#endif
