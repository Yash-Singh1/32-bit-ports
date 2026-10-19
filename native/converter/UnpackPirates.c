/* Native equivalent of tools/unpack_securom.py. The bundled Unicorn engine
   runs only the shipped decompression stub and stops before its dyld call. */
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IMAGE_BASE = 0xe56000, IMAGE_SIZE = 0x49a000, ENTRY = 0x12ee474,
       STAGE2 = 0x12eea28, SYSENTER = 0x12eea6e, HANDOFF = 0x12eedd9,
       STACK_BASE = 0x70000000, STACK_SIZE = 0x200000 };
struct state { int stage2, done, failed; uint32_t cursor; };
static uint64_t align_up(uint64_t x) { return (x + 4095) & ~(uint64_t)4095; }

static int ensure_mapped(uc_engine *uc, uint32_t address, uint32_t length)
{
    uint64_t cursor = address & ~(uint64_t)4095, end = align_up((uint64_t)address + length);
    if (!length || end > 0x100000000ULL) return -1;
    uc_mem_region *regions = NULL; uint32_t count = 0;
    if (uc_mem_regions(uc, &regions, &count)) return -1;
    while (cursor < end) {
        uint64_t next = end; int mapped = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (regions[i].begin <= cursor && cursor <= regions[i].end) {
                cursor = regions[i].end + 1; mapped = 1; break;
            }
            if (regions[i].begin > cursor && regions[i].begin < next) next = regions[i].begin;
        }
        if (!mapped) {
            if (uc_mem_map(uc, cursor, (size_t)(next - cursor), UC_PROT_ALL)) { uc_free(regions); return -1; }
            cursor = next;
        }
    }
    uc_free(regions); return 0;
}

static void hook(uc_engine *uc, uint64_t address, uint32_t size, void *opaque)
{
    (void)size;
    struct state *state = opaque;
    if (address == STAGE2) { state->stage2 = 1; return; }
    if (address == HANDOFF) { state->done = 1; uc_emu_stop(uc); return; }
    if (address != SYSENTER) return;
    uint32_t syscall = 0, pointer = 0, arguments[8] = {0}, result = 0, flags = 0, next = 0;
    if (uc_reg_read(uc, UC_X86_REG_EAX, &syscall) || uc_reg_read(uc, UC_X86_REG_ECX, &pointer) ||
        uc_mem_read(uc, (uint64_t)pointer + 4, arguments, sizeof(arguments))) goto failed;
    if (syscall == 197) {
        uint32_t requested = arguments[0];
        if (!requested && !(arguments[3] & 0x10)) {
            requested = (uint32_t)align_up(state->cursor);
            uint64_t end = (uint64_t)requested + align_up(arguments[1]);
            if (end > UINT32_MAX) goto failed;
            state->cursor = (uint32_t)end;
        }
        if (ensure_mapped(uc, requested, arguments[1])) goto failed;
        result = requested;
    } else if (syscall != 73 && syscall != 74) {
        fprintf(stderr, "Unsupported packer syscall %u\n", syscall); goto failed;
    }
    if (uc_reg_read(uc, UC_X86_REG_EFLAGS, &flags) || uc_reg_read(uc, UC_X86_REG_EDX, &next)) goto failed;
    flags &= ~1u;
    if (uc_reg_write(uc, UC_X86_REG_EAX, &result) || uc_reg_write(uc, UC_X86_REG_EFLAGS, &flags) ||
        uc_reg_write(uc, UC_X86_REG_EIP, &next)) goto failed;
    return;
failed:
    state->failed = 1; uc_emu_stop(uc);
}

static int rebuild_at(uc_engine *uc, uint64_t address, const char *destination)
{
    struct mach_header header;
    if (uc_mem_read(uc, address, &header, sizeof(header)) || header.magic != MH_MAGIC ||
        header.cputype != CPU_TYPE_I386 || header.filetype != MH_EXECUTE ||
        header.ncmds > 1000 || header.sizeofcmds > 0x100000) return -1;
    uint8_t *commands = malloc(header.sizeofcmds);
    if (!commands) return -1;
    if (uc_mem_read(uc, address + sizeof(header), commands, header.sizeofcmds)) { free(commands); return -1; }
    size_t cursor = 0, output_size = 0;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        if (cursor > header.sizeofcmds || header.sizeofcmds - cursor < sizeof(struct load_command)) goto failed;
        struct load_command *command = (void *)(commands + cursor);
        if (command->cmdsize < sizeof(*command) || command->cmdsize > header.sizeofcmds - cursor) goto failed;
        if (command->cmd == LC_SEGMENT) {
            if (command->cmdsize < sizeof(struct segment_command)) goto failed;
            struct segment_command *segment = (void *)command;
            if (strncmp(segment->segname, SEG_PAGEZERO, 16)) {
                uint64_t end = (uint64_t)segment->fileoff + segment->filesize;
                if (end > 128 * 1024 * 1024) goto failed;
                if (end > output_size) output_size = (size_t)end;
            }
        }
        cursor += command->cmdsize;
    }
    if (!output_size) goto failed;
    uint8_t *output = calloc(1, output_size);
    if (!output) goto failed;
    cursor = 0;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        struct load_command *command = (void *)(commands + cursor);
        if (command->cmd == LC_SEGMENT) {
            struct segment_command *segment = (void *)command;
            if (strncmp(segment->segname, SEG_PAGEZERO, 16) && segment->filesize &&
                uc_mem_read(uc, segment->vmaddr, output + segment->fileoff, segment->filesize)) {
                free(output); goto failed;
            }
        }
        cursor += command->cmdsize;
    }
    FILE *file = fopen(destination, "wb");
    if (!file) { free(output); goto failed; }
    int result = fwrite(output, 1, output_size, file) == output_size ? 0 : -1;
    if (fclose(file)) result = -1;
    free(output); free(commands); return result;
failed:
    free(commands); return -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fputs("Usage: UnpackPirates INPUT OUTPUT\n", stderr); return 2; }
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 1;
    long size = ftell(file);
    if (size < 28 || size > 64 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    uint8_t *packed = malloc((size_t)size);
    if (!packed || fread(packed, 1, (size_t)size, file) != (size_t)size) { fclose(file); free(packed); return 1; }
    fclose(file);
    uc_engine *uc = NULL;
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc)) { free(packed); return 1; }
    uint64_t mapping = align_up((uint64_t)size);
    if (mapping < IMAGE_SIZE) mapping = IMAGE_SIZE;
    uint32_t stack = STACK_BASE + STACK_SIZE - 0x1000;
    int result = 1;
    if (uc_mem_map(uc, IMAGE_BASE, (size_t)mapping, UC_PROT_ALL) ||
        uc_mem_write(uc, IMAGE_BASE, packed, (size_t)size) ||
        uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL) ||
        uc_reg_write(uc, UC_X86_REG_ESP, &stack)) goto done;
    struct state state = {.cursor = 0x50000000};
    uc_hook hooks[3]; uint64_t markers[] = {STAGE2, SYSENTER, HANDOFF};
    for (unsigned i = 0; i < 3; ++i)
        if (uc_hook_add(uc, &hooks[i], UC_HOOK_CODE, (void *)hook, &state, markers[i], markers[i])) goto done;
    uc_err error = uc_emu_start(uc, ENTRY, 0, 120000000, 500000000);
    if (error || !state.done || state.failed) {
        fprintf(stderr, "Could not unpack this Pirates executable: %s, stage2=%d\n", uc_strerror(error), state.stage2);
        goto done;
    }
    uc_mem_region *regions = NULL; uint32_t count = 0;
    if (uc_mem_regions(uc, &regions, &count)) goto done;
    for (uint32_t i = 0; i < count && result; ++i) {
        uint64_t length = regions[i].end - regions[i].begin + 1;
        if (regions[i].begin >= IMAGE_BASE || length > 0x10000000) continue;
        uint8_t *bytes = malloc((size_t)length);
        if (!bytes) break;
        if (!uc_mem_read(uc, regions[i].begin, bytes, (size_t)length)) {
            for (size_t offset = 0; offset + 4 <= length; ++offset) {
                if (!memcmp(bytes + offset, "\xce\xfa\xed\xfe", 4) &&
                    rebuild_at(uc, regions[i].begin + offset, argv[2]) == 0) { result = 0; break; }
            }
        }
        free(bytes);
    }
    uc_free(regions);
done:
    uc_close(uc); free(packed);
    if (result) fputs("This Pirates release could not be unpacked.\n", stderr);
    return result;
}
