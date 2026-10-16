#include "macho_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/vm_prot.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
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
};

/*
 * The __LEGACY zerofill in address_space_reserve.S pins the whole low 2 GiB.
 * Replace image pages directly with MAP_FIXED; never release the reservation
 * first. Otherwise stdio, malloc, or another host thread can allocate in the
 * hole between segment mappings and be overwritten by a later MAP_FIXED.
 * Everything else in the reservation is carved by the runtime (guest heap
 * above max(0x02000000, image end), stacks and bridge pages at 0x7c000000+), so the image must
 * end below the heap base.
 */
enum {
    kLegacyReservationStart = 0x00001000,
    kLegacyReservationEnd = 0x10000000,
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

    source->bytes = mapping;
    source->size = (size_t)status.st_size;
    return 0;
}

static bool range_inside(size_t offset, size_t length, size_t container_size)
{
    return offset <= container_size && length <= container_size - offset;
}

static int verify_legacy_reservation(uint32_t start, uint32_t end)
{
    /* Check the actual loaded host segment before replacing any pages. A
       loader linked without address_space_reserve.S must fail safely. */
    const struct mach_header_64 *host =
        (const void *)_dyld_get_image_header(0);
    if (!host || host->magic != MH_MAGIC_64 ||
        start < kLegacyReservationStart || end <= start ||
        end > kLegacyReservationEnd) {
        return fail_message("host legacy-address reservation does not cover image");
    }
    const struct load_command *command = (const void *)(host + 1);
    bool reserved = false;
    for (uint32_t index = 0; index < host->ncmds; ++index) {
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const void *)command;
            if (!strncmp(segment->segname, "__LEGACY", 16) &&
                segment->vmaddr == kLegacyReservationStart &&
                segment->vmsize >= end - kLegacyReservationStart &&
                _dyld_get_image_vmaddr_slide(0) == 0) reserved = true;
        }
        command = (const void *)((const uint8_t *)command + command->cmdsize);
    }
    if (!reserved) {
        return fail_message("host is missing its fixed legacy-address reservation");
    }

    mach_vm_address_t address = start;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t result = mach_vm_region(
        mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object_name);

    if (object_name != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), object_name);
    }
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "game_loader: mach_vm_region failed: %s\n",
                mach_error_string(result));
        return -1;
    }
    if (address > start || size < end - address ||
        !(info.protection & VM_PROT_WRITE)) {
        return fail_message("host legacy-address reservation is not intact");
    }
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
    const struct entry_point_command *main_entry = NULL;
    const struct segment_command *text_segment = NULL;
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
            if (strncmp(segment->segname, SEG_TEXT, 16) == 0) text_segment = segment;
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
        } else if (command->cmd == LC_MAIN) {
            if (command->cmdsize < sizeof(struct entry_point_command) || main_entry) {
                return fail_message("LC_MAIN is truncated or duplicated");
            }
            main_entry = (const void *)cursor;
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

    if (main_entry) {
        /* entryoff is a file offset in __TEXT, not a virtual address or a
           crt startup stub. Validate it before mapping or executing anything. */
        if (entry_eip || !text_segment ||
            !(text_segment->initprot & VM_PROT_EXECUTE) ||
            main_entry->entryoff < text_segment->fileoff ||
            main_entry->entryoff - text_segment->fileoff >= text_segment->filesize) {
            return fail_message("LC_MAIN has an invalid executable entry point");
        }
        entry_eip = text_segment->vmaddr +
                    (uint32_t)(main_entry->entryoff - text_segment->fileoff);
        image->main_address = entry_eip;
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
                    if (image->import_count >= MACHO_IMAGE32_MAX_IMPORTS) {
                        return fail_message("Mach-O import table exceeds loader capacity");
                    }
                    const char *name = strings + symbols[symbol_index].n_un.n_strx;
                    if (!memchr(name, '\0', symtab->strsize - symbols[symbol_index].n_un.n_strx)) {
                        return fail_message("Mach-O import name is unterminated");
                    }
                    struct macho_import32 *import = &image->imports[image->import_count++];
                    import->name = name;
                    import->address = sections[section_index].addr + item * stride;
                    import->kind = kind;
                    /* Modern linkers retain indirect slots for coalesced and
                       private-external definitions. Resolve these to the guest
                       implementation, rather than asking the host bridge to
                       implement the game's own C++ template instantiations. */
                    if ((symbols[symbol_index].n_type & N_TYPE) == N_SECT) {
                        uint32_t target = symbols[symbol_index].n_value;
                        if (target < image->min_address || target >= image->max_address)
                            return fail_message("Mach-O defined indirect symbol is outside image");
                        import->target = target;
                    }
                }
            }
        }
        cursor += command->cmdsize;
    }
    /* Non-lazy symbol pointers also hold addresses of functions. Taking &fopen,
     * for example, must resolve to executable bridge code, not a data cell.
     * A matching stub/lazy entry provides the symbol's callable classification
     * without guessing from its spelling (undefined nlist entries lack types). */
    for (size_t i = 0; i < image->import_count; ++i) {
        struct macho_import32 *import = &image->imports[i];
        if (import->kind != MACHO_IMPORT32_POINTER) continue;
        for (size_t j = 0; j < image->import_count; ++j) {
            if (image->imports[j].kind != MACHO_IMPORT32_POINTER &&
                strcmp(import->name, image->imports[j].name) == 0) {
                import->kind = MACHO_IMPORT32_FUNCTION_POINTER;
                break;
            }
        }
    }
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
    munmap((void *)source.bytes, source.size);
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
