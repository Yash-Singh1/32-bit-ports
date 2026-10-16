/* Exercise the real loader with host allocations between segment mappings. */
#include <assert.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static unsigned mapped_segments;
static void *host_allocations[3];

static void *tracked_mmap(void *address, size_t size, int protection, int flags,
                          int fd, off_t offset)
{
    if (flags & MAP_FIXED) {
        /* No future image page or gap may become available to host mmap or
           malloc while an earlier segment's printf initializes stdio. */
        for (unsigned page = 0x1000; page < 0xa000; page += 0x1000) {
            mach_vm_address_t region = page;
            mach_vm_size_t length = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            assert(mach_vm_region(mach_task_self(), &region, &length,
                VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
                &object) == KERN_SUCCESS);
            if (object) mach_port_deallocate(mach_task_self(), object);
            assert(region <= page && region + length > page);
        }
        assert(mapped_segments < 3);
        void *host = mmap((void *)0x5000, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(host != MAP_FAILED && (uintptr_t)host >= 0x80000000);
        memset(host, 0x7b, 4096);
        host_allocations[mapped_segments++] = host;
    }
    return mmap(address, size, protection, flags, fd, offset);
}

#define mmap tracked_mmap
#include "../src/macho_loader.c"
#undef mmap

int main(int argc, char **argv)
{
    (void)argv;
    uint8_t bytes[0x3000] = {0};
    struct mach_header *header = (void *)bytes;
    *header = (struct mach_header){.magic = MH_MAGIC, .cputype = CPU_TYPE_I386,
        .filetype = MH_EXECUTE, .ncmds = 6};
    struct segment_command *segments = (void *)(header + 1);
    for (unsigned i = 0; i < 3; ++i) {
        segments[i] = (struct segment_command){.cmd = LC_SEGMENT,
            .cmdsize = sizeof(*segments), .vmaddr = 0x1000 + i * 0x4000,
            .vmsize = 4096, .fileoff = i * 4096, .filesize = 4096,
            .initprot = VM_PROT_READ | (i == 0 ? VM_PROT_EXECUTE : VM_PROT_WRITE)};
        strcpy(segments[i].segname, i == 0 ? SEG_TEXT : i == 1 ? SEG_DATA : SEG_LINKEDIT);
    }
    struct entry_point_command *entry = (void *)(segments + 3);
    *entry = (struct entry_point_command){.cmd = LC_MAIN,
        .cmdsize = sizeof(*entry), .entryoff = 0x800};
    struct symtab_command *symbols = (void *)(entry + 1);
    *symbols = (struct symtab_command){.cmd = LC_SYMTAB,
        .cmdsize = sizeof(*symbols), .symoff = 0x2000, .stroff = 0x2000, .strsize = 1};
    struct dysymtab_command *dynamic = (void *)(symbols + 1);
    *dynamic = (struct dysymtab_command){.cmd = LC_DYSYMTAB,
        .cmdsize = sizeof(*dynamic), .indirectsymoff = 0x2000};
    header->sizeofcmds = (uint32_t)((uint8_t *)(dynamic + 1) - (uint8_t *)(header + 1));
    bytes[0x1000] = 0x42;

    char path[] = "/tmp/lp32-mapping-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    close(fd);
    struct macho_image32 image;
    if (argc > 1) {
        /* With no linked reservation, fail before any fixed mappings. */
        void *host = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(host != MAP_FAILED);
        memset(host, 0x7b, 4096);
        assert(macho_image32_load(path, &image) == -1);
        assert(*(uint8_t *)host == 0x7b && mapped_segments == 0);
        munmap(host, 4096);
    } else {
        *(uint8_t *)0x3000 = 0x69; /* A gap must retain its reservation. */
        assert(macho_image32_load(path, &image) == 0);
        assert(mapped_segments == 3 && image.header == (void *)0x1000);
        assert(*(uint8_t *)0x5000 == 0x42 && *(uint8_t *)0x3000 == 0x69);
        for (unsigned i = 0; i < mapped_segments; ++i) {
            assert(*(uint8_t *)host_allocations[i] == 0x7b);
            munmap(host_allocations[i], 4096);
        }
        macho_image32_unload(&image);
    }
    unlink(path);
    puts("Mach-O mapping PASS");
    return 0;
}
