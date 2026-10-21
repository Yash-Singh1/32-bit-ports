#ifndef LP32_ASPHALT_GLSL_H
#define LP32_ASPHALT_GLSL_H
#include <stddef.h>
/* malloc'd compatible source, or NULL when unchanged/allocation fails. */
char *lp32_asphalt_rewrite_glsl(const char *source,size_t length);
#endif
