#ifndef LP32_OBJC_DIAGNOSTICS_H
#define LP32_OBJC_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

enum objc_diag_kind {
    OBJC_DIAG_ENTER, OBJC_DIAG_RESOLVED, OBJC_DIAG_METHOD, OBJC_DIAG_LEAVE,
    OBJC_DIAG_CREATE, OBJC_DIAG_DEALLOC, OBJC_DIAG_RETIRE,
    OBJC_DIAG_POOL_PUSH, OBJC_DIAG_POOL_POP, OBJC_DIAG_AUTORELEASE,
};

/* Only copied scalar data and bounded names survive into the signal handler.
   No retained objects or pointers to temporary diagnostic strings. */
struct objc_diag_record {
    uint64_t sequence, time, native;
    uint32_t thread, kind, token, klass, caller, depth, value, arguments[6];
    char text[80], encoding[80];
};
struct objc_diag_scope {
    bool enabled, previous_valid;
    struct objc_diag_record previous;
};

void objc_diag_begin(struct objc_diag_scope *scope, const char *import,
                     const uint32_t *arguments, uint32_t depth);
void objc_diag_end(struct objc_diag_scope *scope);
void objc_diag_resolved(uint32_t token, const void *native, uint32_t klass);
void objc_diag_method(uint32_t token, uint32_t imp, const char *encoding);
void objc_diag_note(enum objc_diag_kind kind, uint32_t token, const void *native,
                    uint32_t klass, const char *text, uint32_t depth);
/* Called from the crash handler. Does not allocate, query ObjC/dyld, or
   acquire a blocking lock, including if a writer was interrupted. */
void objc_diag_crash_report(int fd, int extra_fd);

#endif
