#include "macho_loader.h"
#include "macho_file.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_prot.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <mach/i386/thread_status.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct source_file {
    const uint8_t *bytes;
    size_t size;
    void *mapping;
    size_t mapping_size;
};

/*
 * The __LEGACY zerofill in address_space_reserve.S pins the whole low 2 GiB.
 * The image replaces its reserved pages directly; everything
 * else in the reservation is carved with MAP_FIXED by the runtime (guest heap
 * above the main image, stacks and bridge pages at 0x7c000000+). Leave at
 * least 1.5 GiB for heap allocations below the guest dylib region.
 */
enum {
    kLegacyReservationStart = 0x00001000,
    kLegacyReservationEnd = 0x10000000,
    kLegacyPageSize = 0x1000,
};

static int fail_message(const char *message)
{
    fprintf(stderr, "game_loader: %s\n", message);
    return -1;
}

static int fail_errno(const char *what)
{
    fprintf(stderr, "game_loader: %s: %s\n", what, strerror(errno));
    return -1;
}

static int open_source(const char *path, struct source_file *source)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail_errno("open recovered Mach-O");

    struct stat status;
    if (fstat(fd, &status) != 0) {
        close(fd);
        return fail_errno("fstat recovered Mach-O");
    }
    if (status.st_size <= 0 || (uint64_t)status.st_size > SIZE_MAX) {
        close(fd);
        return fail_message("invalid recovered Mach-O size");
    }

    void *mapping = mmap(NULL, (size_t)status.st_size, PROT_READ,
                         MAP_PRIVATE, fd, 0);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    if (mapping == MAP_FAILED) return fail_errno("mmap recovered Mach-O");

    source->mapping = mapping;
    source->mapping_size = (size_t)status.st_size;
    if (macho_file32_slice(mapping, source->mapping_size,
                           &source->bytes, &source->size) != 0) {
        munmap(mapping, source->mapping_size);
        return fail_message("file has no valid i386 Mach-O slice");
    }
    return 0;
}

static bool range_inside(size_t offset, size_t length, size_t container_size)
{
    return offset <= container_size && length <= container_size - offset;
}

static int verify_legacy_reservation(uint32_t start, uint32_t end)
{
    const struct mach_header_64 *host = (const void *)_dyld_get_image_header(0);
    const struct segment_command_64 *reservation = NULL;
    if (!host || host->magic != MH_MAGIC_64 || _dyld_get_image_vmaddr_slide(0) != 0)
        return fail_message("expected a fixed-address x86_64 compatibility host");
    const struct load_command *command = (const void *)(host + 1);
    for (uint32_t index = 0; index < host->ncmds; ++index) {
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const void *)command;
            if (!strncmp(segment->segname, "__LEGACY", 16)) reservation = segment;
        }
        command = (const void *)((const char *)command + command->cmdsize);
    }
    if (!reservation || reservation->vmaddr != kLegacyReservationStart ||
        reservation->vmsize < (uint64_t)end - kLegacyReservationStart ||
        start < kLegacyReservationStart || end > kLegacyReservationEnd || end <= start) {
        return fail_message("host legacy-address reservation does not cover image");
    }
    mach_vm_address_t address = start;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t result = mach_vm_region(mach_task_self(), &address, &size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object_name);
    if (object_name != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object_name);
    if (result != KERN_SUCCESS || address > start || size < end - address) {
        return fail_message("host legacy-address reservation is missing or fragmented");
    }
    /* Keep the reservation mapped until each MAP_FIXED replacement. Releasing
       it first lets native Intel's malloc/stdio claim a future guest segment;
       mapping that segment then overwrites live host allocations. */
    return 0;
}

static int mmap_protection(vm_prot_t protection)
{
    int result = 0;
    if (protection & VM_PROT_READ) result |= PROT_READ;
    if (protection & VM_PROT_WRITE) result |= PROT_WRITE;
    if (protection & VM_PROT_EXECUTE) result |= PROT_EXEC;
    return result;
}

static const char *fixed_name(const char name[16], char output[17])
{
    memcpy(output, name, 16);
    output[16] = '\0';
    return output;
}

static int inspect_commands(const struct source_file *source,
                            struct macho_image32 *image)
{
    const struct mach_header *header = (const void *)source->bytes;
    if (source->size < sizeof(*header) || header->magic != MH_MAGIC) {
        return fail_message("recovered image is not a native-endian 32-bit Mach-O");
    }
    if (header->cputype != CPU_TYPE_I386 || header->filetype != MH_EXECUTE) {
        return fail_message("recovered image is not an i386 executable");
    }
    if (!range_inside(sizeof(*header), header->sizeofcmds, source->size)) {
        return fail_message("Mach-O load-command region is truncated");
    }

    uint32_t min_address = UINT32_MAX;
    uint32_t max_address = 0;
    uint32_t segment_count = 0;
    uint32_t initializer_count = 0;
    uint32_t initializer_address = 0;
    uint32_t entry_eip = 0;
    const uint8_t *cursor = source->bytes + sizeof(*header);
    const uint8_t *commands_end = cursor + header->sizeofcmds;

    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if ((size_t)(commands_end - cursor) < sizeof(struct load_command)) {
            return fail_message("Mach-O load command header is truncated");
        }
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) ||
            (size_t)(commands_end - cursor) < command->cmdsize) {
            return fail_message("Mach-O load command has invalid size");
        }

        if (command->cmd == LC_SEGMENT) {
            if (command->cmdsize < sizeof(struct segment_command)) {
                return fail_message("LC_SEGMENT is truncated");
            }
            const struct segment_command *segment = (const void *)cursor;
            size_t sections_size = (size_t)segment->nsects * sizeof(struct section);
            if (sections_size > command->cmdsize - sizeof(*segment)) {
                return fail_message("LC_SEGMENT section array is truncated");
            }
            if (segment->vmsize && strncmp(segment->segname, SEG_PAGEZERO, 16) != 0) {
                if (segment->vmaddr > UINT32_MAX - segment->vmsize) {
                    return fail_message("Mach-O segment address overflows 32 bits");
                }
                if (!range_inside(segment->fileoff, segment->filesize, source->size)) {
                    return fail_message("Mach-O segment file range is truncated");
                }
                if (segment->filesize > segment->vmsize) {
                    return fail_message("Mach-O segment filesize exceeds vmsize");
                }
                if (segment->vmaddr < min_address) min_address = segment->vmaddr;
                if (segment->vmaddr + segment->vmsize > max_address) {
                    max_address = segment->vmaddr + segment->vmsize;
                }
                ++segment_count;
            }

            const struct section *section = (const void *)(segment + 1);
            for (uint32_t section_index = 0; section_index < segment->nsects;
                 ++section_index) {
                if ((section[section_index].flags & SECTION_TYPE) ==
                    S_MOD_INIT_FUNC_POINTERS) {
                    initializer_count += section[section_index].size / sizeof(uint32_t);
                    initializer_address = section[section_index].addr;
                }
                if (strncmp(section[section_index].sectname, "__cstring", 16) == 0 &&
                    strncmp(segment->segname, SEG_TEXT, 16) == 0) {
                    image->cstring_start = section[section_index].addr;
                    image->cstring_end = section[section_index].addr +
                                         section[section_index].size;
                }
                if (strncmp(section[section_index].sectname, "__cfstring", 16) == 0) {
                    image->cfstring_start = section[section_index].addr;
                    image->cfstring_end = section[section_index].addr +
                                          section[section_index].size;
                }
            }
        } else if (command->cmd == LC_UNIXTHREAD) {
            const uint32_t *words = (const void *)(cursor + sizeof(*command));
            size_t word_count = (command->cmdsize - sizeof(*command)) / sizeof(uint32_t);
            if (word_count >= 2 && words[0] == x86_THREAD_STATE32 &&
                words[1] == x86_THREAD_STATE32_COUNT &&
                word_count >= 2 + x86_THREAD_STATE32_COUNT) {
                const x86_thread_state32_t *state = (const void *)(words + 2);
                entry_eip = state->__eip;
            }
        }
        cursor += command->cmdsize;
    }

    if (cursor != commands_end || min_address == UINT32_MAX || !entry_eip) {
        return fail_message("Mach-O has incomplete segment or entry-point metadata");
    }

    image->entry_eip = entry_eip;
    image->min_address = min_address;
    image->max_address = max_address;
    image->initializer_count = initializer_count;
    image->initializer_address = initializer_address;
    image->segment_count = segment_count;
    return 0;
}

static int collect_imports(struct macho_image32 *image)
{
    const struct mach_header *header = image->header;
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    const struct segment_command *linkedit = NULL;
    const uint8_t *cursor = (const uint8_t *)(header + 1);

    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SYMTAB) symtab = (const void *)cursor;
        if (command->cmd == LC_DYSYMTAB) dysymtab = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            if (strncmp(segment->segname, SEG_LINKEDIT, 16) == 0) linkedit = segment;
        }
        cursor += command->cmdsize;
    }
    if (!symtab || !dysymtab || !linkedit || linkedit->vmaddr < linkedit->fileoff) {
        return fail_message("Mach-O import metadata is incomplete");
    }

    uintptr_t linkedit_base = linkedit->vmaddr - linkedit->fileoff;
    const struct nlist *symbols = (const void *)(linkedit_base + symtab->symoff);
    const char *strings = (const void *)(linkedit_base + symtab->stroff);
    const uint32_t *indirect = (const void *)(linkedit_base + dysymtab->indirectsymoff);
    cursor = (const uint8_t *)(header + 1);

    for (uint32_t command_index = 0; command_index < header->ncmds; ++command_index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            const struct section *sections = (const void *)(segment + 1);
            for (uint32_t section_index = 0; section_index < segment->nsects;
                 ++section_index) {
                uint32_t type = sections[section_index].flags & SECTION_TYPE;
                uint32_t stride = 0;
                enum macho_import32_kind kind;
                if (type == S_NON_LAZY_SYMBOL_POINTERS ||
                    type == S_LAZY_SYMBOL_POINTERS) {
                    stride = sizeof(uint32_t);
                    kind = type == S_LAZY_SYMBOL_POINTERS ?
                        MACHO_IMPORT32_FUNCTION_POINTER : MACHO_IMPORT32_POINTER;
                } else if (type == S_SYMBOL_STUBS && sections[section_index].reserved2) {
                    stride = sections[section_index].reserved2;
                    kind = MACHO_IMPORT32_STUB;
                } else {
                    continue;
                }

                uint32_t count = sections[section_index].size / stride;
                if (sections[section_index].reserved1 > dysymtab->nindirectsyms ||
                    count > dysymtab->nindirectsyms - sections[section_index].reserved1) {
                    return fail_message("Mach-O indirect-symbol section is invalid");
                }
                for (uint32_t item = 0; item < count; ++item) {
                    uint32_t symbol_index = indirect[sections[section_index].reserved1 + item];
                    if (symbol_index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
                    if (symbol_index >= symtab->nsyms ||
                        symbols[symbol_index].n_un.n_strx >= symtab->strsize) {
                        return fail_message("Mach-O indirect symbol index is invalid");
                    }
                    const char *name = strings + symbols[symbol_index].n_un.n_strx;
                    if (!memchr(name, '\0', symtab->strsize - symbols[symbol_index].n_un.n_strx)) {
                        return fail_message("Mach-O import name is unterminated");
                    }
                    struct macho_import32 import = {
                        .name = name,
                        .address = sections[section_index].addr + item * stride,
                        .kind = kind,
                    };
                    unsigned symbol_type = symbols[symbol_index].n_type & N_TYPE;
                    if (symbol_type == N_SECT || symbol_type == N_ABS) {
                        /* ld's indirect table can reference definitions in
                           this executable. TFU leaves their stubs as HLTs
                           and their pointer slots zero for dyld to fill. */
                        if (macho_image32_bind_import(&import, symbols[symbol_index].n_value)) return -1;
                        continue;
                    }
                    if (image->import_count >= MACHO_IMAGE32_MAX_IMPORTS) {
                        return fail_message("Mach-O import table exceeds loader capacity");
                    }
                    image->imports[image->import_count++] = import;
                }
            }
        }
        cursor += command->cmdsize;
    }
    return 0;
}

int macho_image32_bind_import(const struct macho_import32 *import, uint32_t target)
{
    uintptr_t address = import->address;
    size_t size = import->kind == MACHO_IMPORT32_STUB ? 5 : 4;
    uintptr_t page = address & ~(uintptr_t)0xfff;
    size_t span = ((address + size + 0xfff) & ~(uintptr_t)0xfff) - page;
    if (mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return fail_errno("mprotect image binding");
    }
    if (import->kind == MACHO_IMPORT32_STUB) {
        uint32_t displacement = target - (import->address + 5);
        *(uint8_t *)address = 0xe9;
        memcpy((void *)(address + 1), &displacement, 4);
    } else {
        memcpy((void *)address, &target, 4);
    }
    return 0;
}

int macho_image32_bind_external_relocations(const struct macho_image32 *image,
    uint32_t (*resolve)(const char *, void *), void *context)
{
    const struct symtab_command *symtab = NULL;
    const struct dysymtab_command *dysymtab = NULL;
    const struct segment_command *linkedit = NULL;
    const uint8_t *cursor = (const void *)(image->header + 1);
    uint32_t base = 0;
    bool have_base = false;
    for (uint32_t i = 0; i < image->header->ncmds; ++i) {
        const struct load_command *cmd = (const void *)cursor;
        if (cmd->cmd == LC_SYMTAB) symtab = (const void *)cmd;
        if (cmd->cmd == LC_DYSYMTAB) dysymtab = (const void *)cmd;
        if (cmd->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cmd;
            if (!have_base && (!(image->header->flags & MH_SPLIT_SEGS) || (segment->initprot & VM_PROT_WRITE))) {
                base = segment->vmaddr; have_base = true;
            }
            if (!strncmp(segment->segname, SEG_LINKEDIT, 16)) linkedit = segment;
        }
        cursor += cmd->cmdsize;
    }
    if (!symtab || !dysymtab || !linkedit) return fail_message("missing relocation metadata");
    if (image->header->flags & MH_PREBOUND) return fail_message("prebound external relocations are not supported");
#define LINK_RANGE(off, size) ((off) >= linkedit->fileoff && range_inside((off) - linkedit->fileoff, (size), linkedit->filesize))
    if (!LINK_RANGE(symtab->symoff, (size_t)symtab->nsyms * sizeof(struct nlist)) ||
        !LINK_RANGE(symtab->stroff, symtab->strsize) ||
        (dysymtab->nextrel && !LINK_RANGE(dysymtab->extreloff, (size_t)dysymtab->nextrel * 8)))
        return fail_message("external relocation metadata outside LINKEDIT");
#undef LINK_RANGE
    const uint8_t *linkbase = (const void *)(uintptr_t)(linkedit->vmaddr - linkedit->fileoff);
    const struct nlist *symbols = (const void *)(linkbase + symtab->symoff);
    const char *strings = (const void *)(linkbase + symtab->stroff);
    const uint32_t *relocations = (const void *)(linkbase + dysymtab->extreloff);
    for (uint32_t i = 0; i < dysymtab->nextrel; ++i) {
        uint32_t offset = relocations[2 * i], flags = relocations[2 * i + 1];
        uint32_t symbol_index = flags & 0x00ffffff;
        uint64_t address = (uint64_t)base + offset;
        if ((offset & R_SCATTERED) || ((flags >> 25) & 3) != 2 ||
            (flags >> 28) != GENERIC_RELOC_VANILLA || !(flags & (1u << 27)) ||
            symbol_index >= symtab->nsyms || address > UINT32_MAX)
            return fail_message("unsupported main-image external relocation");
        bool mapped = false;
        cursor = (const void *)(image->header + 1);
        for (uint32_t j = 0; j < image->header->ncmds; ++j) {
            const struct load_command *cmd = (const void *)cursor;
            if (cmd->cmd == LC_SEGMENT) {
                const struct segment_command *s = (const void *)cmd;
                if (strncmp(s->segname, SEG_PAGEZERO, 16) && address >= s->vmaddr &&
                    address - s->vmaddr + 4 <= s->vmsize) mapped = true;
            }
            cursor += cmd->cmdsize;
        }
        if (!mapped) return fail_message("main-image relocation destination is unmapped");
        const struct nlist *symbol = symbols + symbol_index;
        uint32_t target;
        unsigned type = symbol->n_type & N_TYPE;
        if (type == N_SECT || type == N_ABS) target = symbol->n_value;
        else {
            if (symbol->n_un.n_strx >= symtab->strsize ||
                !memchr(strings + symbol->n_un.n_strx, 0, symtab->strsize - symbol->n_un.n_strx))
                return fail_message("invalid external relocation symbol name");
            const char *name = strings + symbol->n_un.n_strx;
            target = resolve(name, context);
            if (!target) { fprintf(stderr, "game_loader: cannot relocate %s\n", name); return -1; }
        }
        uint32_t value;
        memcpy(&value, (const void *)(uintptr_t)address, 4);
        struct macho_import32 slot = {NULL, (uint32_t)address, MACHO_IMPORT32_POINTER};
        /* Fixed-address executable: no slide. Linker's addends already
           include the source address for PC-relative relocations. */
        if (macho_image32_bind_import(&slot, value + target)) return -1;
    }
    fprintf(stderr, "compat32: bound %u main-image external relocations\n", dysymtab->nextrel);
    return 0;
}

static int map_segments(const struct source_file *source,
                        struct macho_image32 *image)
{
    const struct mach_header *header = (const void *)source->bytes;
    const uint8_t *cursor = source->bytes + sizeof(*header);

    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            if (segment->vmsize && strncmp(segment->segname, SEG_PAGEZERO, 16) != 0) {
                void *target = (void *)(uintptr_t)segment->vmaddr;
                void *mapping = mmap(target, segment->vmsize,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
                if (mapping == MAP_FAILED) return fail_errno("mmap legacy segment");
                if (mapping != target) return fail_message("mmap returned wrong legacy address");
                memcpy(mapping, source->bytes + segment->fileoff, segment->filesize);

                char segment_name[17];
                printf("segment: %-10s 0x%08x-0x%08x file=%u prot=%c%c%c\n",
                       fixed_name(segment->segname, segment_name), segment->vmaddr,
                       segment->vmaddr + segment->vmsize, segment->filesize,
                       segment->initprot & VM_PROT_READ ? 'r' : '-',
                       segment->initprot & VM_PROT_WRITE ? 'w' : '-',
                       segment->initprot & VM_PROT_EXECUTE ? 'x' : '-');
            }
        }
        cursor += command->cmdsize;
    }

    cursor = source->bytes + sizeof(*header);
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        const struct load_command *command = (const void *)cursor;
        if (command->cmd == LC_SEGMENT) {
            const struct segment_command *segment = (const void *)cursor;
            if (segment->vmsize && strncmp(segment->segname, SEG_PAGEZERO, 16) != 0 &&
                mprotect((void *)(uintptr_t)segment->vmaddr, segment->vmsize,
                         mmap_protection(segment->initprot)) != 0) {
                return fail_errno("mprotect legacy segment");
            }
        }
        cursor += command->cmdsize;
    }

    image->header = (const void *)(uintptr_t)image->min_address;
    return 0;
}

int macho_image32_load(const char *path, struct macho_image32 *image)
{
    memset(image, 0, sizeof(*image));
    struct source_file source = {0};
    if (open_source(path, &source) != 0) return -1;

    int result = inspect_commands(&source, image);
    if (result == 0) {
        result = verify_legacy_reservation(image->min_address, image->max_address);
    }
    if (result == 0) result = map_segments(&source, image);
    if (result == 0) result = collect_imports(image);

    int saved_errno = errno;
    munmap(source.mapping, source.mapping_size);
    errno = saved_errno;
    return result;
}

void macho_image32_unload(const struct macho_image32 *image)
{
    if (image->header && image->max_address > image->min_address) {
        munmap((void *)(uintptr_t)image->min_address,
               image->max_address - image->min_address);
    }
}
