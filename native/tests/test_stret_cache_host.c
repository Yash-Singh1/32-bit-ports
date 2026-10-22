#include "compat_runtime.h"
#include "game_profile.h"
#include "macho_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t *lp32_adjust_import_stack(uint32_t *);

int main(int argc, char **argv)
{
    if (argc != 3 || setenv("LP32_GAME", argv[2], 1)) return 1;
    struct macho_image32 image;
    if (macho_image32_load(argv[1], &image) || lp32_profile_select(&image) ||
        compat_runtime32_initialize(&image)) return 2;
    const uint32_t invalid_ids[] = {image.import_count, 0x7fffffff, 0x80002000, 0xffffffff};
    for (unsigned i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++i) {
        uint32_t stack[] = {invalid_ids[i], 0x12345678, 0xabcdef01, 0x88776655};
        uint32_t original[4]; memcpy(original, stack, sizeof(stack));
        if (lp32_adjust_import_stack(stack) != stack || memcmp(original, stack, sizeof(stack))) return 6;
    }
    uint32_t configuration = compat_runtime32_allocate(20, 1);
    if (!configuration) return 3;
    uint32_t *words = (void *)(uintptr_t)configuration;
    words[0] = lp32_profile()->callee_pops_struct_return;
    const char *names[] = {"_CGDisplayBounds", "_CFUUIDGetUUIDBytes",
        "_CFAbsoluteTimeGetGregorianDate", "_objc_msgSend_stret"};
    for (unsigned i = 0; i < 4; ++i) {
        words[i + 1] = compat_runtime32_resolve_symbol(names[i], 0);
        if (!words[i + 1]) return 4;
    }
    uint32_t result = compat_runtime32_call(image.entry_eip, &configuration, 1);
    if (result || compat_runtime32_last_call_trapped()) {
        fprintf(stderr, "i386 struct-return fixture failed: %u\n", result);
        return 5;
    }
    puts("i386 struct-return stack: PASS (static/dynamic IDs, four native results, guards, caller/callee cleanup, invalid IDs)");
    return 0;
}
