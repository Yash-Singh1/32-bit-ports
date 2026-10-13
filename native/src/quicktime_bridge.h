#ifndef LP32_QUICKTIME_BRIDGE_H
#define LP32_QUICKTIME_BRIDGE_H
#include <stdint.h>

/* The exercised Movie Toolbox subset. All outward references are guest32
   tokens; native AVFoundation objects never cross the import gateway. */
int quicktime_bridge32_dispatch(const char *name, const uint32_t *arguments,
                               uint64_t *result);
#endif
