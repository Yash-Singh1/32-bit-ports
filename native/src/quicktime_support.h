#ifndef LP32_QUICKTIME_SUPPORT_H
#define LP32_QUICKTIME_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#define LP32_QUICKTIME_FRAMEWORK_HANDLE UINT32_C(0xffb00001)

uint32_t quicktime_support32_open(const char *path);
uint32_t quicktime_support32_symbol(uint32_t handle, const char *name);
int quicktime_support32_dispatch(const char *name, const uint32_t *arguments,
                                  uint64_t *result);

/* Returns an owned copy, or NULL for a closed/invalid signed16 file ref.
   NewMovieFromFile must copy/consume this path before CloseMovieFile. */
char *quicktime_support32_file_path(int16_t reference);

/* Native decoded frames are copied into owned guest32 storage. No host pointer
   escapes. WRITE returns 0 on success, -50 for invalid input or a locked world.
   A world may be disposed while locked; its buffer remains pinned until the
   matching final UnlockPixels. */
int quicktime_support32_world_info(uint32_t world, int *width, int *height,
                                    size_t *row_bytes);
int quicktime_support32_world_write(uint32_t world, const void *bgra,
                                     size_t source_stride, int width, int height);

#endif
