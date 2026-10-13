#!/usr/bin/env python3
"""Exercise Steam's native filename output followed by Portal's bounded copy.

No game data or live Steam account is used. --expect-dev3-crash checks the
published loader's failure, including the addresses from issue #7.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--loader", type=Path, default=native / "build/game_loader")
parser.add_argument("--expect-dev3-crash", action="store_true")
parser.add_argument("--log", type=Path)
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="lp32-steam-filename-") as directory:
    root = Path(directory)
    (root / "bin").mkdir()
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _getenv, _dlopen, _dlsym, _malloc, _free, _strcmp, _strncpy, dyld_stub_binder ]
...
""")
    (root / "host.c").write_text(r'''
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
struct object { void **vtable; };
static void *client_table[42], *storage_table[59];
static struct object client = {client_table}, storage = {storage_table};
static char *filename;
static bool details(void *self, uint64_t handle, unsigned *app, char **name,
                    int *size, uint64_t *owner) {
    if (self != &storage || handle != UINT64_C(0x123456789abcdef0)) abort();
    *app = 620; *name = filename; *size = 4096; *owner = 17;
    return true;
}
static void *get(void *self, int user, int pipe, const char *version) {
    if (self != &client || user != 17 || pipe != 23 ||
        strcmp(version, "STEAMREMOTESTORAGE_INTERFACE_VERSION016")) abort();
    return &storage;
}
void *CreateInterface(const char *version, int *status) {
    if (strcmp(version, "SteamClient020")) abort();
    if (!filename) {
        /* Allocate a valid host string whose truncated address matches the
           report. Fixed Mach allocation fails rather than replacing memory. */
        mach_vm_address_t address = UINT64_C(0x1e3f1f000);
        if (mach_vm_allocate(mach_task_self(), &address, 4096, VM_FLAGS_FIXED)
            != KERN_SUCCESS) abort();
        filename = (char *)(uintptr_t)(address + 0xe67);
        strcpy(filename, "workshop/fixture-chamber.bsp");
    }
    client_table[12] = get; storage_table[26] = details; *status = 0;
    return &client;
}
''')
    (root / "guest.c").write_text(r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
extern char *bounded_copy(char *, const char *, unsigned long) __asm("_strncpy");
#define METHOD(obj, slot, type) ((type)((*(void ***)(obj))[slot]))
int LauncherMain(void) {
    void *library = dlopen(getenv("LP32_STEAM_FIXTURE"), RTLD_NOW);
    void *(*create)(const char *, int *) = dlsym(library, "CreateInterface");
    if (!library || !create) return -1;
    int status = -1;
    void *client = create("SteamClient020", &status);
    if (!client || status) return -2;
    typedef void *(*get_fn)(void *, int, int, const char *);
    void *storage = METHOD(client, 12, get_fn)(client, 17, 23,
        "STEAMREMOTESTORAGE_INTERFACE_VERSION016");
    if (!storage) return -3;
    typedef bool (*details_fn)(void *, uint64_t, unsigned *, char **, int *, uint64_t *);
    struct { uint32_t before; char *name; uint32_t after; } out = {0x11223344, 0, 0xaabbccdd};
    unsigned app = 0; int size = 0; uint64_t owner = 0;
    if (!METHOD(storage, 26, details_fn)(storage, UINT64_C(0x123456789abcdef0),
                                       &app, &out.name, &size, &owner)) return -4;
    /* The reported destination is a valid guest heap address. Claim the
       whole containing allocation before using that address in the probe. */
    void *allocation = malloc(0x11000000);
    char *destination = (char *)0x102b6120;
    if (!allocation || (uintptr_t)allocation > (uintptr_t)destination ||
        (uintptr_t)destination + 260 > (uintptr_t)allocation + 0x11000000) return -5;
    bounded_copy(destination, out.name, 260);
    destination[259] = 0; // V_strncpy's terminating write.
    int valid = !strcmp(destination, "workshop/fixture-chamber.bsp") &&
        out.before == 0x11223344 && out.after == 0xaabbccdd &&
        app == 620 && size == 4096 && owner == 17;
    free(allocation);
    return valid ? 26 : -6;
}
''')
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    def build(*command):
        subprocess.run(command, check=True, text=True)
    build("xcrun", "clang", "-arch", "x86_64", "-dynamiclib", str(root / "host.c"),
          "-o", str(root / "steamclient.dylib"))
    flags = ["xcrun", "clang", "-target", "i386-apple-macos10.6", "-nostdlib",
             "-fno-builtin", "-L" + str(root), "-lSystem", str(root / "helper.S")]
    if os.environ.get("LP32_I386_LINKER"):
        flags.append("-fuse-ld=" + os.environ["LP32_I386_LINKER"])
    build(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/launcher.dylib",
          str(root / "guest.c"), "-o", str(root / "bin/launcher.dylib"))
    build(*flags, "-Wl,-e,_start", str(root / "start.S"), "-o", str(root / "portal2_osx"))
    environment = dict(os.environ, LP32_GAME="portal2", LP32_DYLD_SELFTEST="1",
        LP32_DYLD_FIXTURE_SELFTEST="1", LP32_LOG_DIR=str(root / "logs"),
        LP32_STEAM_FIXTURE=str(root / "steamclient.dylib"))
    result = subprocess.run(["arch", "-x86_64", str(args.loader.resolve()), str(root / "portal2_osx")],
        env=environment, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    if args.log:
        args.log.write_text(output)
    if args.expect_dev3_crash:
        assert result.returncode == 139, output
        for expected in ("address=0xe3f1fe67", "rsi=00000000e3f1fe67",
                         "rdi=00000000102b6120", "rdx=0000000000000104"):
            assert expected in output, output
        print("Steam filename chain: reproduced dev.3's invalid read with issue #7's source, destination, and copy limit")
    else:
        assert result.returncode == 0 and "guest-dyld self-test: PASS" in output, output
        print("Steam filename chain: PASS (native high pointer -> guest filename -> 260-byte copy; guards preserved)")
