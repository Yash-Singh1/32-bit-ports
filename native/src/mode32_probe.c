#include <architecture/i386/table.h>
#include <errno.h>
#include <inttypes.h>
#include <i386/user_ldt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern int lp32_native_segments;
extern uint32_t run_compat32(uint32_t eip, uint32_t esp, uint16_t cs32);

enum {
    kCodeMapSize = 0x4000,
    kStackMapSize = 0x10000,
    kReturnStubOffset = 0x100,
    kFar64Offset = 0x180,
};

struct __attribute__((packed)) far_ptr32 {
    uint32_t offset;
    uint16_t selector;
};

static void fatal_errno(const char *what)
{
    fprintf(stderr, "mode32_probe: %s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static void crash_handler(int sig, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;
    fprintf(stderr, "mode32_probe: signal %d at %p while crossing execution modes\n",
            sig, info->si_addr);
    fprintf(stderr, "rip=%llx rsp=%llx cs=%llx r14=%llx trap=%u error=%u\n",
            uc->uc_mcontext->__ss.__rip, uc->uc_mcontext->__ss.__rsp,
            uc->uc_mcontext->__ss.__cs, uc->uc_mcontext->__ss.__r14,
            uc->uc_mcontext->__es.__trapno, uc->uc_mcontext->__es.__err);
    _exit(128 + sig);
}

static void install_crash_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGILL, &action, NULL) || sigaction(SIGSEGV, &action, NULL) ||
        sigaction(SIGBUS, &action, NULL) || sigaction(SIGTRAP, &action, NULL)) {
        fatal_errno("sigaction");
    }
}

static bool process_is_translated(void)
{
    int translated = 0;
    size_t size = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) != 0) {
        if (errno == ENOENT) return false;
        fatal_errno("sysctl.proc_translated");
    }
    return translated == 1;
}

static void *map_low(size_t size, int protection)
{
    static uintptr_t next_hint = UINT32_C(0x20000000);
    void *hint = (void *)next_hint;
    void *mapping = mmap(hint, size, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) fatal_errno("mmap(low hint)");
    if ((uintptr_t)mapping + size > UINT32_MAX) {
        fprintf(stderr, "mode32_probe: mmap ignored low-address hint and returned %p\n",
                mapping);
        exit(EXIT_FAILURE);
    }
    next_hint = ((uintptr_t)mapping + size + UINT32_C(0x0fffff)) &
                ~UINT32_C(0x0fffff);
    return mapping;
}

static int allocate_cs32(void)
{
    ldt_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.code.limit00 = 0xffff;
    entry.code.base00 = 0;
    entry.code.base16 = 0;
    entry.code.type = DESC_CODE_READ;
    entry.code.dpl = 3;
    entry.code.present = 1;
    entry.code.limit16 = 0x0f;
    entry.code.opsz = DESC_CODE_32B;
    entry.code.granular = DESC_GRAN_PAGE;
    entry.code.base24 = 0;

    /*
     * Rosetta rejects LDT_AUTO_ALLOC even though native XNU supports it.
     * Wine reserves entries 0..31 and allocates its first user descriptor at
     * index 32, so use the same convention.  This process owns its LDT and has
     * not installed any other entries.
     */
    const int index = 32;
    if (i386_set_ldt(index, &entry, 1) < 0) fatal_errno("i386_set_ldt(cs32)");
    return index;
}

int main(void)
{
    install_crash_handlers();

    uint16_t cs64 = 0, ds_before, es_before;
    __asm__ volatile("movw %%ds, %0; movw %%es, %1" : "=r"(ds_before), "=r"(es_before));
    __asm__ volatile("movw %%cs, %0" : "=r"(cs64));

    const bool translated = process_is_translated();
    lp32_native_segments = !translated;
    printf("host: arch=x86_64 translated=%s cs64=0x%04x\n",
           translated ? "yes" : "no", cs64);
    /* Test the actual segment/mode-switch capability on either host. */
    int ldt_index = allocate_cs32();
    uint16_t cs32 = (uint16_t)((ldt_index << 3) | 7);
    printf("ldt: index=%d cs32=0x%04x\n", ldt_index, cs32);

    uint8_t *code = map_low(kCodeMapSize, PROT_READ | PROT_WRITE);
    uint8_t *stack = map_low(kStackMapSize, PROT_READ | PROT_WRITE);
    uint32_t guest_eip = (uint32_t)(uintptr_t)code;
    uint32_t guest_esp = (uint32_t)((uintptr_t)stack + kStackMapSize - 16);
    uint32_t return_stub = (uint32_t)(uintptr_t)(code + kReturnStubOffset);
    uint32_t far64_address = (uint32_t)(uintptr_t)(code + kFar64Offset);

    /* Exercise SS stack accesses, an ES string store and a DS load before
       returning through the far pointer. Native Intel validates all three. */
    const uint8_t guest_template[] = {
        0xb8, 0x78, 0x56, 0x34, 0x12,
        0x50, 0x31, 0xc0, 0x58, /* push eax; xor eax,eax; pop eax */
        0x89, 0xe7, 0xab,       /* mov esp,edi; stosd */
        0x31, 0xc0, 0x8b, 0x47, 0xfc, /* xor eax,eax; mov -4(edi),eax */
        0xff, 0x2d, 0, 0, 0, 0,
        0x0f, 0x0b,
    };
    memcpy(code, guest_template, sizeof(guest_template));
    memcpy(code + 19, &far64_address, sizeof(far64_address));

    /* Restore the complete host ABI frame established by run_compat32. */
    const uint8_t return_template[] = {
        0x4c, 0x87, 0xf4,
        /* Only native Intel needs selector restoration. */
        0x83, 0x7c, 0x24, 0x0c, 0x00, 0x74, 0x0e,
        0x66, 0x8b, 0x4c, 0x24, 0x08, 0x8e, 0xd9,
        0x66, 0x8b, 0x4c, 0x24, 0x0a, 0x8e, 0xc1,
        0x48, 0x83, 0xc4, 0x18,
        0x41, 0x5f,
        0x41, 0x5e,
        0x41, 0x5d,
        0x41, 0x5c,
        0x5b,
        0x5d,
        0xc3,
    };
    memcpy(code + kReturnStubOffset, return_template, sizeof(return_template));

    const struct far_ptr32 far64 = {
        .offset = return_stub,
        .selector = cs64,
    };
    memcpy(code + kFar64Offset, &far64, sizeof(far64));

    if (mprotect(code, kCodeMapSize, PROT_READ | PROT_EXEC) != 0) {
        fatal_errno("mprotect(PROT_EXEC)");
    }

    printf("maps: guest_code=0x%08" PRIx32 " guest_stack=0x%08" PRIx32
           " return64=0x%08" PRIx32 "\n",
           guest_eip, guest_esp, return_stub);
    fflush(stdout);

    uint32_t result = run_compat32(guest_eip, guest_esp, cs32);
    uint16_t ds_after, es_after;
    __asm__ volatile("movw %%ds, %0; movw %%es, %1" : "=r"(ds_after), "=r"(es_after));
    if (ds_before != ds_after || es_before != es_after) {
        fprintf(stderr, "mode32_probe: host DS/ES were not preserved\n");
        return EXIT_FAILURE;
    }
    printf("result: 0x%08" PRIx32 " (%s)\n", result,
           result == UINT32_C(0x12345678) ? "PASS" : "FAIL");

    /* Rosetta does not reliably implement removal of an LDT entry.  The LDT
     * is process-owned and the kernel releases it during normal teardown. */
    if (munmap(code, kCodeMapSize) || munmap(stack, kStackMapSize)) {
        fatal_errno("munmap");
    }
    return result == UINT32_C(0x12345678) ? EXIT_SUCCESS : EXIT_FAILURE;
}
