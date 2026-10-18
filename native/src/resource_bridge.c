#include "resource_bridge.h"
#include "compat_runtime.h"
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>

struct handle32 {
  uint32_t cell, size;
  struct handle32 *next;
};
static struct handle32 *handles;
static int16_t memory_error, resource_error;
struct resource_entry {
  uint32_t type, offset, size, handle;
  int16_t id;
  const unsigned char *name;
};
struct resource_file {
  unsigned char *bytes;
  size_t length, count;
  struct resource_entry *entries;
};
static struct resource_file files[64];
static int16_t current_file;
static bool initialized;
static uint16_t be16(const unsigned char *p) {
  return (uint16_t)p[0] << 8 | p[1];
}
static uint32_t be32(const unsigned char *p) {
  return (uint32_t)be16(p) << 16 | be16(p + 2);
}
static bool inside(size_t start, size_t length, size_t end) {
  return start <= end && length <= end - start;
}
static struct handle32 *find_handle(uint32_t cell) {
  for (struct handle32 *p = handles; p; p = p->next)
    if (p->cell == cell)
      return p;
  memory_error = -109;
  return NULL;
}
static uint32_t new_handle(uint32_t size, bool clear) {
  struct handle32 *h = calloc(1, sizeof(*h));
  if (!h) {
    memory_error = -108;
    return 0;
  }
  h->cell = compat_runtime32_allocate(4, 1);
  uint32_t data = compat_runtime32_allocate(size ? size : 1, clear);
  if (!h->cell || !data) {
    compat_runtime32_deallocate(h->cell);
    compat_runtime32_deallocate(data);
    free(h);
    memory_error = -108;
    return 0;
  }
  *(uint32_t *)(uintptr_t)h->cell = data;
  h->size = size;
  h->next = handles;
  handles = h;
  memory_error = 0;
  return h->cell;
}
static void dispose_handle(uint32_t cell) {
  for (struct handle32 **p = &handles; *p; p = &(*p)->next)
    if ((*p)->cell == cell) {
      struct handle32 *h = *p;
      *p = h->next;
      compat_runtime32_deallocate(*(uint32_t *)(uintptr_t)cell);
      compat_runtime32_deallocate(cell);
      free(h);
      memory_error = 0;
      return;
    }
  memory_error = -109;
}
int16_t resource_bridge32_open(const char *path) {
  resource_error = -193;
  unsigned char *bytes = NULL;
  long length = 0;
  /* A file may have both data and resource forks. Prefer the actual fork,
   * then the sidecar emitted during signing, then a flattened data fork. */
  ssize_t fork_size = getxattr(path, "com.apple.ResourceFork", NULL, 0, 0, 0);
  if (fork_size > 0 && fork_size <= 64 * 1024 * 1024) {
    length = fork_size;
    bytes = malloc((size_t)length);
    if (bytes && getxattr(path, "com.apple.ResourceFork", bytes, (size_t)length,
                          0, 0) != length) {
      free(bytes);
      bytes = NULL;
    }
  }
  if (!bytes) {
    char sidecar[PATH_MAX];
    int n = snprintf(sidecar, sizeof(sidecar), "%s.lp32-rsrc", path);
    FILE *f =
        n > 0 && (size_t)n < sizeof(sidecar) ? fopen(sidecar, "rb") : NULL;
    if (!f)
      f = fopen(path, "rb");
    if (!f) {
      resource_error = -43;
      return -1;
    }
    if (fseek(f, 0, SEEK_END)) {
      fclose(f);
      return -1;
    }
    length = ftell(f);
    rewind(f);
    if (length > 0 && length <= 64 * 1024 * 1024) {
      bytes = malloc((size_t)length);
      if (bytes && fread(bytes, 1, (size_t)length, f) != (size_t)length) {
        free(bytes);
        bytes = NULL;
      }
    }
    fclose(f);
  }
  if (!bytes || length < 16) {
    free(bytes);
    return -1;
  }
  size_t data = be32(bytes), map = be32(bytes + 4), data_size = be32(bytes + 8),
         map_size = be32(bytes + 12);
  if (!inside(data, data_size, length) || !inside(map, map_size, length) ||
      map_size < 28)
    goto invalid;
  size_t types = map + be16(bytes + map + 24),
         names = map + be16(bytes + map + 26);
  if (!inside(types, 2, map + map_size) || names > map + map_size)
    goto invalid;
  unsigned type_count = (unsigned)(uint16_t)(be16(bytes + types) + 1);
  if (!inside(types + 2, type_count * 8, map + map_size))
    goto invalid;
  size_t count = 0;
  for (unsigned i = 0; i < type_count; i++)
    count += (unsigned)be16(bytes + types + 2 + i * 8 + 4) + 1;
  if (count > 65536)
    goto invalid;
  struct resource_entry *entries = calloc(count ? count : 1, sizeof(*entries));
  if (!entries)
    goto invalid;
  size_t entry = 0;
  for (unsigned i = 0; i < type_count; i++) {
    unsigned char *t = bytes + types + 2 + i * 8;
    size_t refs = types + be16(t + 6);
    unsigned n = (unsigned)be16(t + 4) + 1;
    if (!inside(refs, n * 12, map + map_size))
      goto invalid_entries;
    for (unsigned j = 0; j < n; j++) {
      unsigned char *r = bytes + refs + j * 12;
      size_t offset = data + (be32(r + 4) & 0xffffff);
      if (!inside(offset, 4, data + data_size))
        goto invalid_entries;
      uint32_t size = be32(bytes + offset);
      offset += 4;
      if (!inside(offset, size, data + data_size))
        goto invalid_entries;
      const unsigned char *name = NULL;
      uint16_t name_offset = be16(r + 2);
      if (name_offset != 0xffff) {
        size_t pos = names + name_offset;
        if (!inside(pos, 1, map + map_size) ||
            !inside(pos + 1, bytes[pos], map + map_size))
          goto invalid_entries;
        name = bytes + pos;
      }
      entries[entry++] = (struct resource_entry){
          be32(t), (uint32_t)offset, size, 0, (int16_t)be16(r), name};
    }
  }
  for (int16_t i = 1; i < 64; i++)
    if (!files[i].bytes) {
      files[i] = (struct resource_file){bytes, (size_t)length, count, entries};
      current_file = i;
      resource_error = 0;
      return i;
    }
invalid_entries:
  free(entries);
invalid:
  free(bytes);
  return -1;
}
static void initialize_resources(void) {
  if (initialized)
    return;
  initialized = true;
  char path[PATH_MAX];
  uint32_t length = sizeof(path);
  if (_NSGetExecutablePath(path, &length))
    return;
  char *slash = strrchr(path, '/');
  if (!slash)
    return;
  *slash = 0;
  size_t used = strlen(path);
  if (used + 14 >= sizeof(path))
    return;
  strcat(path, "/../Resources");
  DIR *dir = opendir(path);
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    size_t n = strlen(entry->d_name);
    if (n < 5 || strcmp(entry->d_name + n - 5, ".rsrc"))
      continue;
    char file[PATH_MAX];
    int written = snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
    if (written > 0 && (size_t)written < sizeof(file) &&
        resource_bridge32_open(file) > 0)
      break;
  }
  closedir(dir);
}
int resource_bridge32_dispatch(const char *name, const uint32_t *a,
                               uint64_t *out) {
#define IS(s) (!strcmp(name, s))
  if (name[0] != '_' || !strchr("NDGHRPMCUTFLB", name[1]))
    return 0;
  /* Bink's temporary-memory calls report status through an OSErr pointer. */
  if (IS("_TempNewHandle")) {
    *out = new_handle(a[0], false);
    if (a[1]) *(int16_t *)(uintptr_t)a[1] = *out ? 0 : -108;
    return 1;
  }
  if (IS("_TempHLock") || IS("_TempHUnlock") || IS("_TempDisposeHandle")) {
    bool known = find_handle(a[0]) != NULL;
    if (IS("_TempDisposeHandle")) dispose_handle(a[0]);
    if (a[1]) *(int16_t *)(uintptr_t)a[1] = known ? 0 : -109;
    *out = 0;
    return 1;
  }
  if (IS("_MaxBlock")) {
    *out = 64u << 20;
    return 1;
  }
  /* No Code Fragment Manager: GetSharedLibrary finds no library
     (cfragNoLibraryErr), FindSymbol no symbol (cfragNoSymbolErr). */
  if (IS("_GetSharedLibrary")) {
    if (a[3]) *(uint32_t *)(uintptr_t)a[3] = 0;
    if (a[4]) *(uint32_t *)(uintptr_t)a[4] = 0;
    *out = (uint32_t)(int32_t)-2804;
    return 1;
  }
  if (IS("_FindSymbol")) {
    if (a[2]) *(uint32_t *)(uintptr_t)a[2] = 0;
    *out = (uint32_t)(int32_t)-2828;
    return 1;
  }
  /* QuickDraw offscreen worlds no longer exist. NewGWorld fails like an
     allocation failure (memFullErr), so BinkBufferOpen returns no buffer;
     the pixel calls below can only follow a successful NewGWorld. */
  if (IS("_NewGWorld")) {
    if (a[0]) *(uint32_t *)(uintptr_t)a[0] = 0;
    *out = (uint32_t)(int32_t)-108;
    return 1;
  }
  if (IS("_LockPortBits") || IS("_UnlockPortBits") || IS("_GetPortBitMapForCopyBits") ||
      IS("_GetPixRowBytes") || IS("_CopyBits") || IS("_BackColor") || IS("_LocalToGlobal")) {
    *out = 0;
    return 1;
  }
  if (IS("_CopyPascalStringToC")) {
    const unsigned char *source = (void *)(uintptr_t)a[0];
    char *dest = (void *)(uintptr_t)a[1];
    unsigned length = source[0];
    memmove(dest, source + 1, length);
    dest[length] = 0;
    *out = 0;
    return 1;
  }
  if (IS("_CopyCStringToPascal")) {
    const char *source = (void *)(uintptr_t)a[0];
    unsigned char *dest = (void *)(uintptr_t)a[1];
    size_t length = strnlen(source, 255);
    memmove(dest + 1, source, length);
    dest[0] = (unsigned char)length;
    *out = 0;
    return 1;
  }
  if (IS("_NewHandle") || IS("_NewHandleClear")) {
    *out = new_handle(a[0], IS("_NewHandleClear"));
    return 1;
  }
  if (IS("_DisposeHandle") || IS("_DisposeCTable")) {
    dispose_handle(a[0]);
    *out = 0;
    return 1;
  }
  if (IS("_GetCTable")) {
    /* Application color tables take precedence over the system palette.
     * Resource bytes are big endian; QuickDraw exposes host-order fields. */
    uint32_t args[] = {0x636c7574, a[0]};
    uint64_t resource = 0;
    resource_bridge32_dispatch("_GetResource", args, &resource);
    struct handle32 *source = find_handle((uint32_t)resource);
    if (source && source->size >= 8) {
      const unsigned char *bytes =
          (void *)(uintptr_t)*(uint32_t *)(uintptr_t)source->cell;
      uint32_t count = (uint32_t)be16(bytes + 6) + 1;
      if (count <= 256 && source->size >= 8 + count * 8) {
        uint32_t handle = new_handle(8 + count * 8, false);
        *out = handle;
        if (handle) {
          unsigned char *dest =
              (void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle;
          uint32_t seed = be32(bytes);
          memcpy(dest, &seed, 4);
          for (uint32_t i = 4; i < 8 + count * 8; i += 2) {
            uint16_t value = be16(bytes + i);
            memcpy(dest + i, &value, 2);
          }
        }
        return 1;
      }
    }
    static void *carbon;
    if (!carbon)
      carbon = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon",
                      RTLD_NOW | RTLD_LOCAL);
    void **(*get)(int16_t) = carbon ? dlsym(carbon, "GetCTable") : NULL;
    intptr_t (*size_of)(void **) =
        carbon ? dlsym(carbon, "GetHandleSize") : NULL;
    void (*dispose)(void **) = carbon ? dlsym(carbon, "DisposeHandle") : NULL;
    if (!get || !size_of || !dispose)
      return 0;
    void **table = get((int16_t)a[0]);
    *out = 0;
    if (table) {
      intptr_t size = size_of(table);
      if (size >= 8 && size <= 8 + 256 * 8) {
        uint32_t handle = new_handle((uint32_t)size, false);
        if (handle)
          memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle, *table,
                 (size_t)size);
        *out = handle;
      }
      dispose(table);
    }
    return 1;
  }
  if (IS("_GetHandleSize")) {
    struct handle32 *h = find_handle(a[0]);
    *out = h ? h->size : 0;
    return 1;
  }
  if (IS("_HLock") || IS("_HUnlock")) {
    memory_error = find_handle(a[0]) ? 0 : -109;
    *out = 0;
    return 1;
  }
  if (IS("_PtrAndHand")) {
    /* (ptr, handle, size): append size bytes to the handle; returns OSErr. */
    struct handle32 *h = find_handle(a[1]);
    memory_error = -109;
    if (h && a[2] <= UINT32_MAX - h->size) {
      uint32_t old_data = *(uint32_t *)(uintptr_t)h->cell, old_size = h->size;
      /* The source may lie inside the block that is about to move. */
      bool inside = a[0] >= old_data && a[0] - old_data < old_size;
      uint32_t data = compat_runtime32_reallocate(old_data, old_size + a[2] ? old_size + a[2] : 1);
      memory_error = data ? 0 : -108;
      if (data) {
        memmove((void *)(uintptr_t)(data + old_size),
                (const void *)(uintptr_t)(inside ? data + (a[0] - old_data) : a[0]), a[2]);
        *(uint32_t *)(uintptr_t)h->cell = data;
        h->size = old_size + a[2];
      }
    }
    *out = (uint32_t)(int32_t)memory_error;
    return 1;
  }
  if (IS("_ReallocateHandle")) {
    struct handle32 *h = find_handle(a[0]);
    memory_error = -109;
    if (h) {
      uint32_t data = compat_runtime32_reallocate(
          *(uint32_t *)(uintptr_t)h->cell, a[1] ? a[1] : 1);
      memory_error = data ? 0 : -108;
      if (data) {
        *(uint32_t *)(uintptr_t)h->cell = data;
        h->size = a[1];
      }
    }
    *out = 0;
    return 1;
  }
  if (IS("_PtrToHand")) {
    uint32_t h = new_handle(a[2], false);
    if (h) {
      memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)h,
             (const void *)(uintptr_t)a[0], a[2]);
    }
    *(uint32_t *)(uintptr_t)a[1] = h;
    *out = (uint32_t)(int32_t)memory_error;
    return 1;
  }
  if (IS("_NewPtr")) {
    *out = compat_runtime32_allocate(a[0], 0);
    memory_error = *out ? 0 : -108;
    return 1;
  }
  if (IS("_DisposePtr")) {
    compat_runtime32_deallocate(a[0]);
    *out = 0;
    return 1;
  }
  if (IS("_MemError")) {
    *out = (uint32_t)(int32_t)memory_error;
    return 1;
  }
  if (IS("_CurResFile")) {
    initialize_resources();
    *out = (uint32_t)(int32_t)current_file;
    return 1;
  }
  if (IS("_UseResFile")) {
    int16_t f = (int16_t)a[0];
    resource_error = 0;
    if (f > 0 && f < 64 && files[f].bytes)
      current_file = f;
    else
      resource_error = -193;
    *out = 0;
    return 1;
  }
  if (IS("_ResError")) {
    *out = (uint32_t)(int32_t)resource_error;
    return 1;
  }
  if (IS("_GetIndString")) {
    unsigned char *destination = (void *)(uintptr_t)a[0];
    destination[0] = 0;
    uint32_t request[] = {0x53545223, a[1]};
    uint64_t handle = 0;
    resource_bridge32_dispatch("_GetResource", request, &handle);
    struct handle32 *h = handle ? find_handle((uint32_t)handle) : NULL;
    if (h && h->size >= 2) {
      const unsigned char *bytes =
          (const void *)(uintptr_t)*(uint32_t *)(uintptr_t)h->cell;
      size_t offset = 2;
      int index = (int16_t)a[2];
      if (index > 0 && index <= be16(bytes))
        for (int i = 1; i <= index; i++) {
          if (offset >= h->size ||
              !inside(offset + 1, bytes[offset], h->size)) {
            resource_error = -199;
            break;
          }
          if (i == index) {
            memcpy(destination, bytes + offset, (size_t)bytes[offset] + 1);
            resource_error = 0;
            break;
          }
          offset += (size_t)bytes[offset] + 1;
        }
    }
    *out = 0;
    return 1;
  }
  if (IS("_GetResource") || IS("_Get1Resource") || IS("_GetIndResource") ||
      IS("_GetNamedResource")) {
    initialize_resources();
    *out = 0;
    resource_error = -192;
    int index = (int16_t)a[1];
    for (int f = current_file; f > 0; --f) {
      for (size_t i = 0; i < files[f].count; i++) {
        struct resource_entry *e = &files[f].entries[i];
        if (e->type != a[0])
          continue;
        bool match =
            IS("_GetIndResource") ? --index == 0 : e->id == (int16_t)a[1];
        if (IS("_GetNamedResource")) {
          const unsigned char *n = (const void *)(uintptr_t)a[1];
          match = e->name && n && e->name[0] == n[0] &&
                  !memcmp(e->name + 1, n + 1, n[0]);
        }
        if (!match)
          continue;
        if (!e->handle) {
          e->handle = new_handle(e->size, false);
          if (e->handle)
            memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)e->handle,
                   files[f].bytes + e->offset, e->size);
        }
        *out = e->handle;
        resource_error = e->handle ? 0 : -108;
        return 1;
      }
      if (IS("_Get1Resource"))
        break;
    }
    return 1;
  }
  if (IS("_ReleaseResource")) {
    for (int f = 1; f < 64; f++)
      for (size_t i = 0; i < files[f].count; i++)
        if (files[f].entries[i].handle == a[0] && a[0]) {
          dispose_handle(a[0]);
          files[f].entries[i].handle = 0;
          resource_error = 0;
          *out = 0;
          return 1;
        }
    resource_error = -192;
    *out = 0;
    return 1;
  }
  if (IS("_CloseResFile")) {
    int f = (int16_t)a[0];
    resource_error = -193;
    if (f > 0 && f < 64 && files[f].bytes) {
      for (size_t i = 0; i < files[f].count; i++)
        if (files[f].entries[i].handle)
          dispose_handle(files[f].entries[i].handle);
      free(files[f].bytes);
      free(files[f].entries);
      memset(&files[f], 0, sizeof(files[f]));
      if (current_file == f)
        current_file = 0;
      resource_error = 0;
    }
    *out = 0;
    return 1;
  }
  return 0;
#undef IS
}
