#include "guest_dyld.h"
#include "physics_trace.h"
#include "crash_trace.h"
#include "steam_bridge.h"
#include "macho_file.h"
#include "compat_runtime.h"
#include "objc_bridge.h"
#include "game_profile.h"
#include "quicktime_support.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* This region is inside __LEGACY but outside the existing heap, stacks and
   transition bridge. Modules are pinned: guest callbacks can outlive dlclose. */
enum { kModuleBase = 0x70000000, kModuleEnd = 0x7c000000,
       kModuleLimit = 128, kSegmentLimit = 32, kDependencyLimit = 128 };
#define HANDLE_BASE UINT32_C(0xffe00000)
#define HOST_HANDLE_BASE UINT32_C(0xffd00000)
static struct { char path[PATH_MAX]; void *handle; } host_modules[64];
static unsigned host_module_count;

struct module32 {
    char path[PATH_MAX];
    uint8_t *file;
    size_t file_size;
    const struct mach_header *header;
    const struct segment_command *segments[kSegmentLimit];
    unsigned segment_count;
    const struct symtab_command *symtab;
    const struct dysymtab_command *dysymtab;
    const struct dyld_info_command *dyld;
    uint32_t *exports; /* Open-addressed nlist indices plus one. */
    uint32_t export_capacity;
    const char *dependency_names[kDependencyLimit];
    int dependencies[kDependencyLimit]; /* -1 = host framework/runtime */
    unsigned dependency_count;
    uint32_t preferred, base, end, slide;
    unsigned state; /* 1 mapped, 2 bound, 3 initializing, 4 initialized, 5 failed */
};

static struct module32 modules[kModuleLimit];
static unsigned module_count;
static uint32_t module_cursor = kModuleBase;
static char game_root[PATH_MAX];
static const char *binary_directory = "bin";
static pthread_mutex_t loader_lock;
static pthread_once_t loader_once = PTHREAD_ONCE_INIT;
static _Thread_local char loader_error[1024];
static _Thread_local bool error_pending;

static void create_lock(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&loader_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static int fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(loader_error, sizeof(loader_error), format, args);
    va_end(args);
    error_pending = true;
    if (getenv("LP32_TRACE_DYLD_ERRORS")) fprintf(stderr, "guest-dyld: %s\n", loader_error);
    return -1;
}

static bool range(size_t offset, size_t size, size_t total)
{
    return offset <= total && size <= total - offset;
}

static const void *file_range(const struct module32 *m, uint32_t offset,
                               size_t size)
{
    return range(offset, size, m->file_size) ? m->file + offset : NULL;
}

static void *vm_range(const struct module32 *m, uint64_t address, size_t size)
{
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        uint64_t base = (uint32_t)(s->vmaddr + m->slide);
        if (address >= base && address - base <= s->vmsize &&
            size <= s->vmsize - (address - base)) return (void *)(uintptr_t)address;
    }
    return NULL;
}

static const char *symbol_name(const struct module32 *m, const struct nlist *n)
{
    if (!m->symtab || n->n_un.n_strx >= m->symtab->strsize) return NULL;
    const char *p = file_range(m, m->symtab->stroff, m->symtab->strsize);
    if (!p) return NULL;
    p += n->n_un.n_strx;
    return memchr(p, 0, m->symtab->strsize - n->n_un.n_strx) ? p : NULL;
}

static const struct nlist *symbol_table(const struct module32 *m)
{
    return m->symtab ? file_range(m, m->symtab->symoff,
                           (size_t)m->symtab->nsyms * sizeof(struct nlist)) : NULL;
}

static uint32_t name_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    while (*name) hash = (hash ^ (unsigned char)*name++) * 16777619u;
    return hash;
}

static bool is_export(const struct nlist *symbol)
{
    unsigned type = symbol->n_type & N_TYPE;
    return !(symbol->n_type & N_STAB) && (symbol->n_type & N_EXT) &&
        (type == N_SECT || type == N_ABS || type == N_INDR);
}

static uint32_t exported_recursive(const struct module32 *m, const char *name, unsigned depth)
{
    if (depth > kModuleLimit) return 0;
    const struct nlist *symbols = symbol_table(m);
    if (!symbols || !m->export_capacity) return 0;
    uint32_t slot = name_hash(name) & (m->export_capacity - 1);
    for (; m->exports[slot]; slot = (slot + 1) & (m->export_capacity - 1)) {
        const struct nlist *n = symbols + m->exports[slot] - 1;
        unsigned type = n->n_type & N_TYPE;
        const char *candidate = symbol_name(m, n);
        if (candidate && !strcmp(candidate, name)) {
            if (type == N_INDR) {
                struct nlist alias = *n;
                alias.n_un.n_strx = n->n_value;
                const char *target = symbol_name(m, &alias);
                if (!target) return 0;
                /* N_INDR reexports have a matching undefined nlist entry
                   whose two-level ordinal identifies the actual provider. */
                for (uint32_t j = 0; j < m->symtab->nsyms; ++j) {
                    const struct nlist *ref = symbols + j;
                    if ((ref->n_type & N_TYPE) != N_UNDF || (ref->n_type & N_STAB)) continue;
                    const char *ref_name = symbol_name(m, ref);
                    unsigned ordinal = GET_LIBRARY_ORDINAL(ref->n_desc);
                    if (ref_name && !strcmp(ref_name, target) && ordinal && ordinal <= m->dependency_count) {
                        int dep = m->dependencies[ordinal - 1];
                        return dep < 0 ? compat_runtime32_resolve_symbol(target, false) :
                            exported_recursive(&modules[dep], target, depth + 1);
                    }
                }
                return strcmp(target, name) ? exported_recursive(m, target, depth + 1) : 0;
            }
            return n->n_value + (type == N_SECT ? m->slide : 0);
        }
    }
    return 0;
}

static uint32_t exported(const struct module32 *m, const char *name)
{
    return exported_recursive(m, name, 0);
}

/* Loader-owned lookups only. Private/local definitions must never become
   visible to guest dlsym or cross-module import binding. */
static uint32_t defined_symbol(const struct module32 *m, const char *name)
{
    const struct nlist *symbols = symbol_table(m);
    if (!symbols) return 0;
    for (uint32_t i = 0; i < m->symtab->nsyms; ++i) {
        const struct nlist *n = symbols + i;
        if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT) continue;
        const char *candidate = symbol_name(m, n);
        /* Slides wrap modulo 2^32 when a high preferred address is relocated
           down into the module arena, just like exported symbol resolution. */
        uint32_t address = n->n_value + m->slide;
        if (candidate && !strcmp(candidate, name) &&
            vm_range(m, address, 1)) return (uint32_t)address;
    }
    return 0;
}

static void *accessible_range(const struct module32 *m, uint32_t address,
                              size_t size, vm_prot_t protection)
{
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        uint64_t base = (uint32_t)(s->vmaddr + m->slide);
        if ((s->initprot & protection) == protection && address >= base &&
            address - base <= s->vmsize && size <= s->vmsize - (address - base))
            return (void *)(uintptr_t)address;
    }
    return NULL;
}

static bool module_string_equals(const struct module32 *m, uint32_t address,
                                 const char *expected)
{
    size_t size = strlen(expected) + 1;
    const char *text = accessible_range(m, address, size, VM_PROT_READ);
    return text && !memcmp(text, expected, size);
}

static uint32_t global_symbol(const char *name)
{
    for (unsigned i = 0; i < module_count; ++i) {
        uint32_t result = exported(&modules[i], name);
        if (result) return result;
    }
    return 0;
}

static bool system_path(const char *path)
{
    return !strncmp(path, "/usr/lib/", 9) || !strncmp(path, "/System/Library/", 16);
}

static int resolve_path(const char *name, const char *parent, char out[PATH_MAX])
{
    char candidate[PATH_MAX];
    int n;
    if (!strncmp(name, "@loader_path/", 13)) {
        const char *slash = strrchr(parent, '/');
        n = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                     slash ? (int)(slash - parent) : 0, parent, name + 13);
    } else if (!strncmp(name, "@executable_path/", 17)) {
        n = snprintf(candidate, sizeof(candidate), "%s/%s", game_root, name + 17);
    } else if (!strncmp(name, "@rpath/", 7)) {
        n = snprintf(candidate, sizeof(candidate), "%s/%s/%s", game_root, binary_directory, name + 7);
    } else if (name[0] == '/') {
        n = snprintf(candidate, sizeof(candidate), "%s", name);
    } else {
        n = strchr(name, '/') ? snprintf(candidate, sizeof(candidate), "%s/%s", game_root, name) :
            snprintf(candidate, sizeof(candidate), "%s/%s/%s", game_root, binary_directory, name);
    }
    if (n < 0 || (size_t)n >= sizeof(candidate)) return fail("library path is too long");
    if (!realpath(candidate, out)) {
        /* Match portal2.sh's guest-only DYLD_LIBRARY_PATH: newer Mac depots
           moved libraries to bin/osx32. Keep the original install layout so
           Source's own game-content searches still work. */
        const char *basename = strrchr(name, '/');
        basename = basename ? basename + 1 : name;
        n = snprintf(candidate, sizeof(candidate), "%s/%s/%s", game_root, binary_directory, basename);
        if (n < 0 || (size_t)n >= sizeof(candidate)) return fail("library path is too long");
        if (!realpath(candidate, out)) return fail("%s: %s", candidate, strerror(errno));
    }
    return 0;
}

int guest_dyld32_initialize(const char *image_path)
{
    char path[PATH_MAX];
    if (!realpath(image_path, path)) return fail("%s: %s", image_path, strerror(errno));
    char *slash = strrchr(path, '/');
    if (!slash) return fail("image path has no directory");
    *slash = 0;
    char *support = strrchr(path, '/');
    if (support && !strcmp(support, "/SharedSupport")) {
        const char *directory = lp32_profile()->source_data_directory;
        if (!directory || !directory[0]) return fail("missing Source data directory in game profile");
        size_t length = strlen(path);
        int n = snprintf(path + length, sizeof(path) - length, "/%s", directory);
        if (n < 0 || (size_t)n >= sizeof(path) - length) return fail("game path is too long");
    }
    if (!realpath(path, game_root)) return fail("game directory: %s", strerror(errno));
    char launcher[PATH_MAX];
    int n = snprintf(launcher, sizeof(launcher), "%s/bin/osx32/launcher.dylib", game_root);
    if (n < 0 || (size_t)n >= sizeof(launcher)) return fail("game path is too long");
    binary_directory = access(launcher, R_OK) == 0 ? "bin/osx32" : "bin";
    pthread_once(&loader_once, create_lock);
    return 0;
}

const char *guest_dyld32_game_root(void) { return game_root; }

static int read_module(struct module32 *m)
{
    FILE *f = fopen(m->path, "rb");
    if (!f) return fail("%s: %s", m->path, strerror(errno));
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size <= 0 || st.st_size > 512 * 1024 * 1024) {
        fclose(f);
        return fail("invalid library size: %s", m->path);
    }
    size_t size = (size_t)st.st_size;
    uint8_t *file = malloc(size);
    if (!file) { fclose(f); return fail("allocating library file"); }
    bool read_ok = fread(file, 1, size, f) == size;
    fclose(f);
    const uint8_t *slice;
    size_t slice_size;
    if (!read_ok || macho_file32_slice(file, size, &slice, &slice_size)) {
        free(file);
        return fail("no valid i386 library slice: %s", m->path);
    }
    memmove(file, slice, slice_size);
    m->file = file;
    m->file_size = slice_size;
    m->header = (const void *)file;
    const struct mach_header *h = m->header;
    if ((h->filetype != MH_DYLIB && h->filetype != MH_BUNDLE) ||
        (h->flags & (MH_SPLIT_SEGS | MH_PREBOUND)) ||
        !range(sizeof(*h), h->sizeofcmds, m->file_size)) {
        return fail("unsupported or truncated i386 dylib: %s", m->path);
    }
    size_t cursor = sizeof(*h), end = cursor + h->sizeofcmds;
    m->preferred = UINT32_MAX;
    uint32_t maximum = 0;
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        if (!range(cursor, sizeof(struct load_command), end)) return fail("truncated load command");
        const struct load_command *lc = (const void *)(file + cursor);
        if (lc->cmdsize < sizeof(*lc) || !range(cursor, lc->cmdsize, end)) return fail("invalid load command size");
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *s = (const void *)lc;
            if (lc->cmdsize < sizeof(*s) ||
                s->nsects > (lc->cmdsize - sizeof(*s)) / sizeof(struct section) ||
                s->vmaddr > UINT32_MAX - s->vmsize || s->filesize > s->vmsize ||
                !file_range(m, s->fileoff, s->filesize) || (s->vmaddr & 4095) ||
                m->segment_count == kSegmentLimit) return fail("invalid library segment");
            if (!strncmp(s->segname, SEG_PAGEZERO, 16)) return fail("unexpected dylib pagezero");
            for (unsigned j = 0; j < m->segment_count; ++j) {
                const struct segment_command *prev = m->segments[j];
                if (s->vmsize && prev->vmsize && s->vmaddr < prev->vmaddr + prev->vmsize &&
                    prev->vmaddr < s->vmaddr + s->vmsize) return fail("overlapping library segments");
            }
            const struct section *sections = (const void *)(s + 1);
            for (uint32_t j = 0; j < s->nsects; ++j) {
                if (sections[j].addr < s->vmaddr ||
                    !range(sections[j].addr - s->vmaddr, sections[j].size, s->vmsize)) {
                    return fail("library section outside segment");
                }
            }
            m->segments[m->segment_count++] = s;
            if (s->vmsize) {
                if (s->vmaddr < m->preferred) m->preferred = s->vmaddr;
                if (s->vmaddr + s->vmsize > maximum) maximum = s->vmaddr + s->vmsize;
            }
        } else if (lc->cmd == LC_SYMTAB) {
            if (lc->cmdsize < sizeof(struct symtab_command)) return fail("truncated symtab");
            m->symtab = (const void *)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            if (lc->cmdsize < sizeof(struct dysymtab_command)) return fail("truncated dysymtab");
            m->dysymtab = (const void *)lc;
        } else if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) {
            if (lc->cmdsize < sizeof(struct dyld_info_command)) return fail("truncated dyld info");
            m->dyld = (const void *)lc;
        } else if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB ||
                   lc->cmd == LC_REEXPORT_DYLIB || lc->cmd == LC_LOAD_UPWARD_DYLIB) {
            const struct dylib_command *d = (const void *)lc;
            if (lc->cmdsize < sizeof(*d) || d->dylib.name.offset < sizeof(*d) ||
                d->dylib.name.offset >= lc->cmdsize || m->dependency_count == kDependencyLimit) {
                return fail("invalid dependency command");
            }
            const char *name = (const char *)lc + d->dylib.name.offset;
            if (!memchr(name, 0, lc->cmdsize - d->dylib.name.offset)) return fail("unterminated dependency");
            m->dependency_names[m->dependency_count++] = name;
        }
        cursor += lc->cmdsize;
    }
    if (cursor != end || m->preferred == UINT32_MAX || !symbol_table(m) ||
        !file_range(m, m->symtab->stroff, m->symtab->strsize)) return fail("incomplete library metadata");
    const struct nlist *symbols = symbol_table(m);
    uint32_t export_count = 0;
    for (uint32_t i = 0; i < m->symtab->nsyms; ++i) {
        if (!symbol_name(m, symbols + i)) return fail("invalid library symbol name");
        if (is_export(symbols + i)) ++export_count;
    }
    m->export_capacity = 2;
    while (m->export_capacity < (uint64_t)export_count * 2) m->export_capacity *= 2;
    m->exports = calloc(m->export_capacity, sizeof(*m->exports));
    if (!m->exports) return fail("allocating export index");
    for (uint32_t i = 0; i < m->symtab->nsyms; ++i) {
        if (!is_export(symbols + i)) continue;
        const char *name = symbol_name(m, symbols + i);
        uint32_t slot = name_hash(name) & (m->export_capacity - 1);
        while (m->exports[slot]) {
            if (!strcmp(name, symbol_name(m, symbols + m->exports[slot] - 1))) break;
            slot = (slot + 1) & (m->export_capacity - 1);
        }
        if (!m->exports[slot]) m->exports[slot] = i + 1;
    }
    uint64_t size64 = ((uint64_t)maximum - m->preferred + 4095) & ~UINT64_C(4095);
    /* objc_bridge's ephemeral NSEvent handles occupy this one-page range. */
    if (module_cursor <= UINT32_C(0x71000000) &&
        module_cursor + size64 > UINT32_C(0x71000000)) module_cursor = UINT32_C(0x71001000);
    if (!size64 || size64 > kModuleEnd - module_cursor) return fail("guest library address space exhausted");
    m->base = module_cursor;
    m->end = m->base + (uint32_t)size64;
    m->slide = m->base - m->preferred;
    if (mmap((void *)(uintptr_t)m->base, (size_t)size64, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) return fail("mapping library: %s", strerror(errno));
    module_cursor = m->end;
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        memcpy((void *)(uintptr_t)(s->vmaddr + m->slide), file + s->fileoff, s->filesize);
    }
    m->state = 1;
    fprintf(stderr, "guest-dyld: mapped %s at 0x%08x (slide=0x%08x)\n", m->path, m->base, m->slide);
    return 0;
}

static int map_tree(const char *path)
{
    for (unsigned i = 0; i < module_count; ++i) if (!strcmp(path, modules[i].path)) return (int)i;
    if (module_count == kModuleLimit) return fail("too many guest libraries");
    unsigned index = module_count++;
    struct module32 *m = &modules[index];
    snprintf(m->path, sizeof(m->path), "%s", path);
    if (read_module(m)) return -1;
    for (unsigned i = 0; i < m->dependency_count; ++i) {
        const char *name = m->dependency_names[i];
        char dep[PATH_MAX];
        if (system_path(name)) {
            m->dependencies[i] = -1;
            /* The old C++ ABI includes guest vtables and inline object layouts.
               A private i386 runtime can supply those; libSystem/frameworks
               must still use the host bridge. Never load a host dylib here. */
            if (strcmp(name, "/usr/lib/libstdc++.6.dylib") &&
                strcmp(name, "/usr/lib/libc++abi.dylib")) continue;
            const char *basename = strrchr(name, '/') + 1;
            const char *directory = getenv("LP32_GUEST_RUNTIME_DIR");
            char candidate[PATH_MAX];
            int n = directory ? snprintf(candidate, sizeof(candidate), "%s/%s", directory, basename) :
                snprintf(candidate, sizeof(candidate), "%s/compat-runtime/%s", game_root, basename);
            if (n < 0 || (size_t)n >= sizeof(candidate)) return fail("guest runtime path is too long");
            if (!directory && access(candidate, F_OK)) continue;
            if (!realpath(candidate, dep)) return fail("%s: %s", candidate, strerror(errno));
        } else if (resolve_path(name, m->path, dep)) return -1;
        int result = map_tree(dep);
        if (result < 0) return -1;
        m->dependencies[i] = result;
    }
    return (int)index;
}

static uint32_t resolve_symbol(struct module32 *m, const char *name, int ordinal,
                               bool weak, bool data)
{
    uint32_t result = 0;
    if (ordinal == 0) result = exported(m, name);
    else if (ordinal > 0 && (unsigned)ordinal <= m->dependency_count) {
        int dep = m->dependencies[ordinal - 1];
        if (dep >= 0) {
            result = exported(&modules[dep], name);
            if (!result && !weak) fail("%s does not export %s (needed by %s)",
                                      modules[dep].path, name, m->path);
            return result;
        }
    } else if (ordinal > 0 && ordinal < 0xfe) {
        fail("invalid library ordinal for %s", name);
        return 0;
    }
    if (!result && (ordinal <= 0 || ordinal >= 0xfe)) result = global_symbol(name);
    if (!result && weak && (name[0] != '_' || !dlsym(RTLD_DEFAULT, name + 1))) return 0;
    if (!result) result = compat_runtime32_resolve_symbol(name, data);
    if (!result) fail("cannot bridge %s (needed by %s)", name, m->path);
    return result;
}

static uint32_t resolve_nlist(struct module32 *m, uint32_t index, bool data)
{
    const struct nlist *table = symbol_table(m);
    if (!table || index >= m->symtab->nsyms) { fail("invalid symbol index"); return 0; }
    const struct nlist *n = table + index;
    const char *name = symbol_name(m, n);
    if (!name) { fail("invalid symbol name"); return 0; }
    if ((n->n_type & N_TYPE) == N_SECT) {
        if (n->n_desc & N_WEAK_DEF) {
            uint32_t shared = global_symbol(name);
            if (shared) return shared;
        }
        return n->n_value + m->slide;
    }
    if ((n->n_type & N_TYPE) == N_ABS) return n->n_value;
    return resolve_symbol(m, name, GET_LIBRARY_ORDINAL(n->n_desc),
                          (n->n_desc & N_WEAK_REF) != 0, data);
}

static int classic_rebase(struct module32 *m)
{
    const struct dysymtab_command *d = m->dysymtab;
    if (!d) return fail("missing classic relocation table");
    const uint32_t *r = file_range(m, d->locreloff, (size_t)d->nlocrel * 8);
    if (!r) return fail("truncated local relocations");
    for (uint32_t i = 0; i < d->nlocrel; ++i, r += 2) {
        uint32_t offset, length, type, pcrel;
        if (r[0] & R_SCATTERED) {
            offset = r[0] & 0x00ffffff;
            length = (r[0] >> 28) & 3;
            type = (r[0] >> 24) & 15;
            pcrel = (r[0] >> 30) & 1;
        } else {
            if ((r[1] & 0x00ffffff) == R_ABS) continue;
            offset = r[0]; length = (r[1] >> 25) & 3;
            type = r[1] >> 28; pcrel = (r[1] >> 24) & 1;
            if (r[1] & (1u << 27)) return fail("external relocation in local table");
        }
        uint32_t *slot = vm_range(m, (uint64_t)m->base + offset, 4);
        if (!slot || length != 2 || type != GENERIC_RELOC_VANILLA || pcrel) return fail("unsupported local relocation in %s", m->path);
        *slot += m->slide;
    }
    return 0;
}

static int classic_bind(struct module32 *m)
{
    const struct dysymtab_command *d = m->dysymtab;
    const uint32_t *r = file_range(m, d->extreloff, (size_t)d->nextrel * 8);
    if (!r) return fail("truncated external relocations");
    for (uint32_t i = 0; i < d->nextrel; ++i, r += 2) {
        uint32_t *slot = vm_range(m, (uint64_t)m->base + r[0], 4);
        if (!slot || r[0] & R_SCATTERED || ((r[1] >> 25) & 3) != 2 ||
            r[1] >> 28 != GENERIC_RELOC_VANILLA || !(r[1] & (1u << 27))) return fail("unsupported external relocation");
        uint32_t target = resolve_nlist(m, r[1] & 0x00ffffff, true);
        if (error_pending) return -1;
        *slot += target - ((r[1] & (1u << 24)) ? m->slide : 0);
    }
    const uint32_t *indirect = file_range(m, d->indirectsymoff, (size_t)d->nindirectsyms * 4);
    if (!indirect) return fail("truncated indirect symbols");
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        const struct section *sections = (const void *)(s + 1);
        for (uint32_t j = 0; j < s->nsects; ++j) {
            const struct section *sec = sections + j;
            unsigned type = sec->flags & SECTION_TYPE;
            bool stub = type == S_SYMBOL_STUBS;
            if (!stub && type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) continue;
            uint32_t stride = stub ? sec->reserved2 : 4;
            if (!stride || (stub && stride < 5) || sec->size % stride ||
                !range(sec->reserved1, sec->size / stride, d->nindirectsyms)) return fail("invalid indirect section");
            for (uint32_t k = 0; k < sec->size / stride; ++k) {
                uint32_t sym = indirect[sec->reserved1 + k];
                uint8_t *slot = vm_range(m, (uint64_t)(uint32_t)(sec->addr + m->slide) + k * stride, stride);
                if (!slot) return fail("invalid indirect pointer");
                if (sym & INDIRECT_SYMBOL_LOCAL) {
                    if (!(sym & INDIRECT_SYMBOL_ABS) && !stub) *(uint32_t *)slot += m->slide;
                    continue;
                }
                if (sym & INDIRECT_SYMBOL_ABS) continue;
                uint32_t target = resolve_nlist(m, sym, type == S_NON_LAZY_SYMBOL_POINTERS);
                if (error_pending) return -1;
                if (stub) {
                    slot[0] = 0xe9;
                    uint32_t displacement = target - ((uint32_t)(uintptr_t)slot + 5);
                    memcpy(slot + 1, &displacement, 4);
                } else memcpy(slot, &target, 4);
            }
        }
    }
    return 0;
}

struct stream { const uint8_t *p, *end; bool bad; };

static uint64_t uleb(struct stream *s)
{
    uint64_t result = 0;
    unsigned shift = 0;
    while (s->p < s->end && shift < 64) {
        unsigned byte = *s->p++;
        if (shift == 63 && (byte & 0x7e)) break;
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return result;
        shift += 7;
    }
    s->bad = true;
    return 0;
}

static int64_t sleb(struct stream *s)
{
    uint64_t result = 0;
    unsigned shift = 0, byte = 0;
    do {
        if (s->p == s->end || shift >= 64) { s->bad = true; return 0; }
        byte = *s->p++;
        result |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) result |= UINT64_MAX << shift;
    return (int64_t)result;
}

static uint32_t *stream_slot(struct module32 *m, int segment, uint64_t offset)
{
    if (segment < 0 || (unsigned)segment >= m->segment_count ||
        !range(offset, 4, m->segments[segment]->vmsize)) return NULL;
    return vm_range(m, (uint64_t)(uint32_t)(m->segments[segment]->vmaddr + m->slide) + offset, 4);
}

static int compressed_rebase(struct module32 *m)
{
    uint32_t size = m->dyld->rebase_size;
    const uint8_t *bytes = file_range(m, m->dyld->rebase_off, size);
    if (!bytes) return fail("truncated rebase stream");
    struct stream s = {bytes, bytes + size, false};
    int segment = -1;
    unsigned type = 0;
    uint64_t offset = 0;
    while (s.p < s.end && !s.bad) {
        unsigned byte = *s.p++, op = byte & REBASE_OPCODE_MASK, imm = byte & REBASE_IMMEDIATE_MASK;
        uint64_t count = 0, skip = 0;
        switch (op) {
            case REBASE_OPCODE_DONE: return 0;
            case REBASE_OPCODE_SET_TYPE_IMM: type = imm; break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: segment = (int)imm; offset = uleb(&s); break;
            case REBASE_OPCODE_ADD_ADDR_ULEB: offset += uleb(&s); break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: offset += imm * 4; break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES: count = imm; break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: count = uleb(&s); break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: count = 1; skip = uleb(&s); break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: count = uleb(&s); skip = uleb(&s); break;
            default: return fail("unsupported rebase opcode 0x%x", op);
        }
        if (s.bad || count > m->file_size || offset > UINT32_MAX) return fail("invalid rebase stream");
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t *slot = stream_slot(m, segment, offset);
            if (!slot || (type != REBASE_TYPE_POINTER && type != REBASE_TYPE_TEXT_ABSOLUTE32)) return fail("invalid rebase target/type");
            *slot += m->slide;
            offset += 4 + skip;
        }
    }
    return s.bad ? fail("truncated rebase operand") : 0;
}

static int compressed_bind(struct module32 *m, uint32_t start, uint32_t size, bool lazy, bool weak_stream)
{
    const uint8_t *bytes = file_range(m, start, size);
    if (!bytes) return fail("truncated bind stream");
    struct stream s = {bytes, bytes + size, false};
    int segment = -1, ordinal = weak_stream ? -3 : 0;
    unsigned type = BIND_TYPE_POINTER, flags = 0;
    uint64_t offset = 0;
    int64_t addend = 0;
    const char *name = NULL;
    while (s.p < s.end && !s.bad) {
        unsigned byte = *s.p++, op = byte & BIND_OPCODE_MASK, imm = byte & BIND_IMMEDIATE_MASK;
        uint64_t count = 0, skip = 0;
        switch (op) {
            case BIND_OPCODE_DONE:
                if (!lazy) return 0;
                segment = -1; ordinal = 0; addend = 0; name = NULL;
                type = BIND_TYPE_POINTER; flags = 0; break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: ordinal = (int)imm; break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: ordinal = (int)uleb(&s); break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: ordinal = imm ? (int8_t)(imm | 0xf0) : 0; break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
                flags = imm; name = (const char *)s.p;
                const uint8_t *nul = memchr(s.p, 0, (size_t)(s.end - s.p));
                if (!nul) return fail("unterminated bind symbol");
                s.p = nul + 1; break;
            }
            case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
            case BIND_OPCODE_SET_ADDEND_SLEB: addend = sleb(&s); break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: segment = (int)imm; offset = uleb(&s); break;
            case BIND_OPCODE_ADD_ADDR_ULEB: offset += uleb(&s); break;
            case BIND_OPCODE_DO_BIND: count = 1; break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: count = 1; skip = uleb(&s); break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: count = 1; skip = imm * 4; break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: count = uleb(&s); skip = uleb(&s); break;
            default: return fail("unsupported bind opcode 0x%x", op);
        }
        if (s.bad || count > m->file_size || offset > UINT32_MAX) return fail("invalid bind stream in %s at %td (op=0x%x offset=0x%llx skip=0x%llx count=%llu bad=%d)", m->path, s.p - bytes, op, (unsigned long long)offset, (unsigned long long)skip, (unsigned long long)count, s.bad);
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t *slot = stream_slot(m, segment, offset);
            if (!slot || !name || type < BIND_TYPE_POINTER || type > BIND_TYPE_TEXT_PCREL32) return fail("invalid bind target/type");
            uint32_t target = resolve_symbol(m, name, ordinal, (flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0, !lazy);
            if (error_pending) return -1;
            *slot = target + (uint32_t)addend - (type == BIND_TYPE_TEXT_PCREL32 ? (uint32_t)(uintptr_t)slot + 4 : 0);
            offset += 4 + skip;
        }
    }
    return s.bad ? fail("truncated bind operand") : 0;
}

static int bind_module(struct module32 *m)
{
    if (m->state >= 2) return 0;
    if (m->dyld) {
        if (compressed_rebase(m) ||
            compressed_bind(m, m->dyld->bind_off, m->dyld->bind_size, false, false) ||
            compressed_bind(m, m->dyld->weak_bind_off, m->dyld->weak_bind_size, false, true) ||
            compressed_bind(m, m->dyld->lazy_bind_off, m->dyld->lazy_bind_size, true, false)) return -1;
    } else if (classic_rebase(m) || classic_bind(m)) return -1;
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        int prot = 0;
        if (s->initprot & VM_PROT_READ) prot |= PROT_READ;
        if (s->initprot & VM_PROT_WRITE) prot |= PROT_WRITE;
        if (s->initprot & VM_PROT_EXECUTE) prot |= PROT_EXEC;
        if (s->vmsize && mprotect((void *)(uintptr_t)(s->vmaddr + m->slide), s->vmsize, prot)) {
            return fail("protecting library: %s", strerror(errno));
        }
    }
    m->state = 2;
    const unsigned char *uuid = NULL;
    const struct load_command *command = (const void *)(m->header + 1);
    for (unsigned i = 0; i < m->header->ncmds; ++i) {
        if (command->cmd == LC_UUID && command->cmdsize >= sizeof(struct uuid_command))
            uuid = ((const struct uuid_command *)command)->uuid;
        command = (const void *)((const char *)command + command->cmdsize);
    }
    lp32_trace_module(m->path, m->base, m->end, m->slide, uuid);
    lp32_physics_trace_install(m->path, m->slide, uuid);
    return 0;
}

static int initialize_module(struct module32 *m);

static int run_initializers(struct module32 *m)
{
    for (unsigned i = 0; i < m->dependency_count; ++i) {
        if (m->dependencies[i] >= 0 && initialize_module(&modules[m->dependencies[i]])) return -1;
    }
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        const struct section *sections = (const void *)(s + 1);
        for (uint32_t j = 0; j < s->nsects; ++j) {
            const struct section *sec = sections + j;
            if (!strncmp(sec->sectname, "__module_info", 16) &&
                objc_bridge32_register_legacy_module(sec->addr + m->slide, sec->size)) {
                return fail("registering legacy Objective-C classes: %s", m->path);
            }
        }
    }
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct segment_command *s = m->segments[i];
        const struct section *sections = (const void *)(s + 1);
        for (uint32_t j = 0; j < s->nsects; ++j) {
            const struct section *sec = sections + j;
            if ((sec->flags & SECTION_TYPE) != S_MOD_INIT_FUNC_POINTERS) continue;
            const uint32_t *functions = vm_range(m, (uint32_t)(sec->addr + m->slide), sec->size);
            if (!functions || sec->size % 4) return fail("invalid initializer array");
            for (uint32_t k = 0; k < sec->size / 4; ++k) {
                if (!k || getenv("LP32_TRACE_DYLD_INITIALIZERS")) {
                    fprintf(stderr, "guest-dyld: initializing %s (%u functions, first=0x%08x)\n",
                            m->path, sec->size / 4, functions[k]);
                }
                if (!vm_range(m, functions[k], 1)) return fail("initializer outside its module");
                compat_runtime32_call(functions[k], NULL, 0);
                if (compat_runtime32_last_call_trapped()) return fail("guest initializer trapped: %s[%u]", m->path, k);
            }
        }
    }
    return 0;
}

/* Source release builds construct development-only ConVars without registering
   them. Apply explicit overrides after constructors, before SpawnServer builds
   m_FullSendTables. Use the guest setter so its string/float/int stay coherent. */
static char *trim_override(char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = 0;
    return text;
}

static bool override_name(const char *name)
{
    if (!*name) return false;
    for (; *name; ++name)
        if (!isalnum((unsigned char)*name) && *name != '_') return false;
    return true;
}

static bool override_list(const char *variable, char buffer[1024])
{
    const char *value = getenv(variable);
    if (!value || !*value) return false;
    if (strlen(value) >= 1024) {
        fprintf(stderr, "compat32: %s exceeds 1023 bytes; no overrides applied\n", variable);
        return false;
    }
    strcpy(buffer, value);
    return true;
}

static int apply_convar_overrides(struct module32 *m)
{
    char buffer[1024];
    if (!override_list("LP32_SET_CONVARS", buffer)) return 0;
    uint32_t setter = defined_symbol(m, "__ZN6ConVar8SetValueEPKc");
    if (!setter || !accessible_range(m, setter, 1, VM_PROT_READ | VM_PROT_EXECUTE)) {
        fprintf(stderr, "compat32: convar setter unavailable in %s; no overrides applied\n", m->path);
        return 0;
    }
    char *state;
    for (char *entry = strtok_r(buffer, ",", &state); entry; entry = strtok_r(NULL, ",", &state)) {
        char *equals = strchr(entry, '=');
        if (!equals) {
            fprintf(stderr, "compat32: convar override requires name=value\n");
            continue;
        }
        *equals = 0;
        char *name = trim_override(entry), *value = trim_override(equals + 1);
        char symbol[256];
        if (!override_name(name) || snprintf(symbol, sizeof(symbol), "_%s", name) >= (int)sizeof(symbol)) {
            fprintf(stderr, "compat32: invalid convar override name\n");
            continue;
        }
        uint32_t address = defined_symbol(m, symbol);
        const uint32_t *object = accessible_range(m, address, 52, VM_PROT_READ | VM_PROT_WRITE);
        /* ConCommandBase::m_pszName is at +12 in the i386 Source layout. */
        if (!object || !module_string_equals(m, object[3], name)) {
            fprintf(stderr, "compat32: convar %s missing or incompatible in %s; left unchanged\n", name, m->path);
            continue;
        }
        uint32_t arguments[] = { address, compat_runtime32_copy_cstring(value) };
        if (!arguments[1]) return fail("allocating convar override value");
        compat_runtime32_call(setter, arguments, 2);
        bool trapped = compat_runtime32_last_call_trapped();
        compat_runtime32_deallocate(arguments[1]);
        if (trapped) return fail("convar setter trapped for %s in %s", name, m->path);
        fprintf(stderr, "compat32: convar %s = %s at 0x%08x float_bits=%08x int=%d\n",
                name, value, address, object[11], (int32_t)object[12]);
    }
    return 0;
}

struct server_class32 { uint32_t name, table, next, class_id, baseline; };

static void drop_server_classes(struct module32 *m)
{
    char buffer[1024];
    if (!override_list("LP32_DROP_SERVER_CLASSES", buffer)) return;
    uint32_t head_address = defined_symbol(m, "_g_pServerClassHead");
    uint32_t *head = accessible_range(m, head_address, 4, VM_PROT_READ | VM_PROT_WRITE);
    if (!head) {
        fprintf(stderr, "compat32: server class head unavailable in %s; no classes dropped\n", m->path);
        return;
    }
    char *state;
    for (char *entry = strtok_r(buffer, ",", &state); entry; entry = strtok_r(NULL, ",", &state)) {
        char *name = trim_override(entry), symbol[256];
        if (!override_name(name) || snprintf(symbol, sizeof(symbol), "__ZL%zug_%s_ClassReg",
                strlen(name) + 11, name) >= (int)sizeof(symbol)) {
            fprintf(stderr, "compat32: invalid server class override name\n");
            continue;
        }
        uint32_t target = defined_symbol(m, symbol);
        struct server_class32 *victim = accessible_range(m, target, sizeof(*victim), VM_PROT_READ | VM_PROT_WRITE);
        if (!victim || !module_string_equals(m, victim->name, name)) {
            fprintf(stderr, "compat32: server class %s missing or incompatible in %s; left unchanged\n", name, m->path);
            continue;
        }
        /* Validate the entire bounded chain before writing. Walking the actual
           head handles first entries and repeated removals, and cannot modify
           an unrelated static registration that merely points at the victim. */
        uint32_t seen[4096], count = 0, *link = head, *predecessor = NULL;
        bool valid = true;
        while (*link) {
            if (count == sizeof(seen) / sizeof(seen[0])) { valid = false; break; }
            for (unsigned i = 0; i < count; ++i) if (seen[i] == *link) valid = false;
            if (!valid) break;
            seen[count++] = *link;
            struct server_class32 *node = accessible_range(m, *link, sizeof(*node), VM_PROT_READ | VM_PROT_WRITE);
            if (!node) { valid = false; break; }
            if (*link == target) predecessor = link;
            link = &node->next;
        }
        if (!valid || !predecessor) {
            fprintf(stderr, "compat32: server class %s %s; left unchanged\n", name,
                    valid ? "is not in the active list" : "has an invalid/cyclic class list");
            continue;
        }
        *predecessor = victim->next;
        fprintf(stderr, "compat32: dropped server class %s at 0x%08x before map spawn\n", name, target);
    }
}

static int apply_source_overrides(struct module32 *m)
{
    if (lp32_profile()->title != LP32_TITLE_PORTAL2) return 0;
    const char *base = strrchr(m->path, '/');
    base = base ? base + 1 : m->path;
    if (!strcmp(base, "engine.dylib")) return apply_convar_overrides(m);
    if (!strcmp(base, "server.dylib")) drop_server_classes(m);
    return 0;
}

static int initialize_module(struct module32 *m)
{
    if (m->state == 5) return fail("library initialization previously failed: %s", m->path);
    if (m->state == 3 || m->state == 4) return 0;
    m->state = 3;
    int result = run_initializers(m);
    if (!result) result = apply_source_overrides(m);
    m->state = result ? 5 : 4;
    return result;
}

static uint32_t open_locked(const char *name, bool initialize)
{
    if (!name) return HANDLE_BASE;
    char path[PATH_MAX];
    if (resolve_path(name, game_root, path)) return 0;
    unsigned old_count = module_count;
    uint32_t old_cursor = module_cursor;
    int index = map_tree(path);
    bool ok = index >= 0;
    for (unsigned i = old_count; ok && i < module_count; ++i) ok = bind_module(&modules[i]) == 0;
    if (!ok) {
        /* Nothing executed yet: a failed dependency or bind can be rolled back. */
        for (unsigned i = old_count; i < module_count; ++i) {
            if (modules[i].state) munmap((void *)(uintptr_t)modules[i].base, modules[i].end - modules[i].base);
            free(modules[i].file);
            free(modules[i].exports);
            memset(&modules[i], 0, sizeof(modules[i]));
        }
        module_count = old_count;
        module_cursor = old_cursor;
        return 0;
    }
    if (initialize && initialize_module(&modules[index])) return 0;
    return HANDLE_BASE + (uint32_t)index + 1;
}

uint32_t guest_dyld32_open(const char *name, int flags)
{
    pthread_once(&loader_once, create_lock);
    pthread_mutex_lock(&loader_lock);
    error_pending = false;
    uint32_t result = 0;
    if (lp32_profile()->title == LP32_TITLE_L4D2 &&
        (result = quicktime_support32_open(name))) {
        /* Native AVFoundation backs this scoped obsolete framework surface. */
    } else if (name && system_path(name)) {
        unsigned i;
        for (i = 0; i < host_module_count; ++i) if (!strcmp(host_modules[i].path, name)) break;
        if (i < host_module_count) result = HOST_HANDLE_BASE + i + 1;
        else if (host_module_count == 64 || strlen(name) >= PATH_MAX) fail("too many host libraries or invalid path");
        else {
            if (!(flags & (RTLD_LAZY | RTLD_NOW))) flags |= RTLD_LAZY;
            void *handle = dlopen(name, flags);
            if (!handle) fail("%s: %s", name, dlerror());
            else {
                strcpy(host_modules[i].path, name);
                host_modules[i].handle = handle;
                ++host_module_count;
                result = HOST_HANDLE_BASE + i + 1;
            }
        }
    } else {
        result = steam_bridge32_open(name);
        if (!result) result = open_locked(name, true);
    }
    pthread_mutex_unlock(&loader_lock);
    return result;
}

uint32_t guest_dyld32_symbol(uint32_t handle, const char *symbol)
{
    pthread_mutex_lock(&loader_lock);
    error_pending = false;
    uint32_t result = 0;
    char name[1024];
    if (!symbol || snprintf(name, sizeof(name), "_%s", symbol) >= (int)sizeof(name)) {
        fail("invalid dlsym name");
    } else if (handle == LP32_STEAM_CLIENT_HANDLE) {
        result = steam_bridge32_symbol(symbol);
        if (!result) fail("native Steam symbol %s is unavailable", symbol);
    } else if (handle == LP32_QUICKTIME_FRAMEWORK_HANDLE &&
               lp32_profile()->title == LP32_TITLE_L4D2) {
        result = quicktime_support32_symbol(handle, symbol);
        if (!result) fail("QuickTime compatibility symbol %s is unavailable", symbol);
    } else if (handle == HANDLE_BASE || handle == (uint32_t)(uintptr_t)RTLD_DEFAULT) {
        result = global_symbol(name);
        if (!result && dlsym(RTLD_DEFAULT, symbol)) result = compat_runtime32_resolve_symbol(name, false);
        if (!result) fail("symbol %s not found", symbol);
    } else if (handle > HOST_HANDLE_BASE && handle - HOST_HANDLE_BASE <= host_module_count) {
        if (dlsym(host_modules[handle - HOST_HANDLE_BASE - 1].handle, symbol)) {
            result = compat_runtime32_resolve_symbol(name, false);
        }
        if (!result) fail("host symbol %s not found", symbol);
    } else if (handle > HANDLE_BASE && handle - HANDLE_BASE <= module_count) {
        struct module32 *m = &modules[handle - HANDLE_BASE - 1];
        result = exported(m, name);
        for (unsigned i = 0; !result && i < m->dependency_count; ++i) {
            if (m->dependencies[i] >= 0) result = exported(&modules[m->dependencies[i]], name);
        }
        if (!result) fail("symbol %s not found in %s", symbol, m->path);
    } else fail("invalid guest dylib handle 0x%08x", handle);
    pthread_mutex_unlock(&loader_lock);
    return result;
}

int guest_dyld32_close(uint32_t handle)
{
    if (handle == LP32_STEAM_CLIENT_HANDLE) return 0;
    if (handle == LP32_QUICKTIME_FRAMEWORK_HANDLE && lp32_profile()->title == LP32_TITLE_L4D2) return 0;
    if (handle > HOST_HANDLE_BASE && handle - HOST_HANDLE_BASE <= host_module_count) return 0;
    if (handle < HANDLE_BASE || handle - HANDLE_BASE > module_count) return fail("invalid dlclose handle");
    return 0;
}

uint32_t guest_dyld32_error(void)
{
    if (!error_pending) return 0;
    error_pending = false;
    return compat_runtime32_copy_cstring(loader_error);
}

uint32_t guest_dyld32_symbol_module(uint32_t address)
{
    pthread_mutex_lock(&loader_lock);
    uint32_t handle = 0;
    for (unsigned i = 0; i < module_count; ++i) if (vm_range(&modules[i], address, 1)) { handle = HANDLE_BASE + i + 1; break; }
    pthread_mutex_unlock(&loader_lock);
    return handle;
}

uint32_t guest_dyld32_module_name(uint32_t handle)
{
    pthread_mutex_lock(&loader_lock);
    const char *name = NULL;
    if (handle > HANDLE_BASE && handle - HANDLE_BASE <= module_count) name = modules[handle - HANDLE_BASE - 1].path;
    if (handle > HOST_HANDLE_BASE && handle - HOST_HANDLE_BASE <= host_module_count) name = host_modules[handle - HOST_HANDLE_BASE - 1].path;
    uint32_t result = name ? compat_runtime32_copy_cstring(name) : 0;
    pthread_mutex_unlock(&loader_lock);
    return result;
}

int guest_dyld32_contains(uint32_t address, size_t size)
{
    for (unsigned i = 0; i < module_count; ++i) if (vm_range(&modules[i], address, size)) return 1;
    return 0;
}

int guest_dyld32_section_contains(const char *name, uint32_t address)
{
    for (unsigned i = 0; i < module_count; ++i) {
        const struct module32 *m = &modules[i];
        for (unsigned j = 0; j < m->segment_count; ++j) {
            const struct segment_command *s = m->segments[j];
            const struct section *sec = (const void *)(s + 1);
            for (unsigned k = 0; k < s->nsects; ++k) {
                uint32_t base = sec[k].addr + m->slide;
                if (!strncmp(sec[k].sectname, name, 16) && address >= base && address - base < sec[k].size) return 1;
            }
        }
    }
    return 0;
}

const char *guest_dyld32_describe(uint32_t address, uint32_t *offset)
{
    for (unsigned i = 0; i < module_count; ++i) {
        if (address >= modules[i].base && address < modules[i].end) {
            *offset = address - modules[i].slide;
            return modules[i].path;
        }
    }
    return NULL;
}

int guest_dyld32_self_test(void)
{
    /* Real Source code executes only when the normal launcher runs. This
       checks relocations, binding, paths, reuse and lookup without saves/UI. */
    pthread_mutex_lock(&loader_lock);
    error_pending = false;
    uint32_t launcher = open_locked("launcher.dylib", false);
    uint32_t main = launcher ? guest_dyld32_symbol(launcher, "LauncherMain") : 0;
    unsigned count = module_count;
    bool ok = launcher && main && guest_dyld32_contains(main, 1) &&
              open_locked("launcher.dylib", false) == launcher && module_count == count;
    if (ok) {
        ok = !guest_dyld32_symbol(launcher, "__lp32_missing_symbol__") &&
             guest_dyld32_error() && !guest_dyld32_error() &&
             !guest_dyld32_symbol(HANDLE_BASE, "__lp32_missing_symbol__") &&
             guest_dyld32_error() && !guest_dyld32_error();
    }
    if (ok && getenv("LP32_DYLD_FIXTURE_SELFTEST")) {
        for (unsigned attempt = 0; ok && attempt < 2; ++attempt) {
            ok = guest_dyld32_open("bin/launcher.dylib", RTLD_NOW) == launcher;
            if (ok) {
                uint32_t value = compat_runtime32_call(main, NULL, 0);
                ok = value == 26 && !compat_runtime32_last_call_trapped();
                if (!ok) fprintf(stderr, "guest-dyld fixture attempt %u returned %d (expected 26)\n", attempt + 1, (int32_t)value);
            }
        }
        if (ok) {
            ok = !guest_dyld32_open("bin/broken.dylib", RTLD_NOW) &&
                 module_count == count && guest_dyld32_error() && !guest_dyld32_error() &&
                 guest_dyld32_symbol(launcher, "LauncherMain") == main;
        }
    }
    fprintf(stderr, "guest-dyld self-test: %s (%u modules, LauncherMain=0x%08x)\n",
            ok ? "PASS" : "FAIL", module_count, main);
    pthread_mutex_unlock(&loader_lock);
    return ok ? 0 : -1;
}
