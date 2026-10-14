#ifndef GUEST_ONCE_H
#define GUEST_ONCE_H

#include <stdint.h>

/* Darwin i386 pthread_once_t has a 32-bit signature and 4 opaque bytes.
   Preserve the signature; use the opaque word for release/acquire completion.
   Resetting or reusing storage requires that no concurrent calls remain. */
struct guest_once_control {
    uint32_t signature;
    uint32_t state;
};

int guest_once_run(struct guest_once_control *control,
                   int (*initialize)(void *), void *context);

#endif
