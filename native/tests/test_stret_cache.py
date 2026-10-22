#!/usr/bin/env python3
"""Execute both i386 struct-return cleanup conventions through real gateways."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    assert result.returncode == 0, f"exit={result.returncode}\n" + result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix="lp32-stret-") as directory:
    root = Path(directory)
    # Reuse the loader's exact compiler/linker flags and bridge dependencies,
    # replacing its entry point with an isolated native fixture driver.
    recipes = run(["make", "-s", "-n", "-B", "build/game_loader"], cwd=native)
    commands = (shlex.split(line) for line in recipes.splitlines())
    command = next(parts for parts in commands if "src/game_loader.c" in parts)
    command[command.index("src/game_loader.c")] = str(native / "tests/test_stret_cache_host.c")
    command[command.index("-o") + 1] = str(root / "host")
    command.extend(["-Isrc"])
    run(command, cwd=native)
    run(["codesign", "--force", "--sign", "-", str(root / "host")])
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _CGDisplayBounds, _CGMainDisplayID, _CFUUIDGetConstantUUIDWithBytes,
      _CFUUIDGetUUIDBytes, _CFTimeZoneCopySystem, _CFAbsoluteTimeGetGregorianDate,
      _CFGregorianDateGetAbsoluteTime, _CFRelease, _objc_msgSend_stret,
      _memcpy, _memset, ___stack_chk_guard, ___stack_chk_fail, dyld_stub_binder ]
...
""")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    flags = ["xcrun", "clang", "-target", "i386-apple-macos10.6", "-nostdlib",
             "-fno-builtin", "-L" + str(root), "-lSystem", str(root / "helper.S")]
    if os.environ.get("LP32_I386_LINKER"):
        flags.append("-fuse-ld=" + os.environ["LP32_I386_LINKER"])
    for optimization in ("-O0", "-O2"):
        image = root / "fixture"
        run(flags + [optimization, "-Wl,-e,_stret_fixture,-no_pie",
            str(native / "tests/fixtures/stret_stack.c"),
            str(native / "tests/fixtures/stret_stack.S"), "-o", str(image)])
        for profile in ("portal2", "l4d2", "pirates", "clonewars"):
            for disabled in (False, True):
                environment = dict(os.environ)
                if disabled:
                    environment["LP32_NO_STRET_CACHE"] = "1"
                else:
                    environment.pop("LP32_NO_STRET_CACHE", None)
                output = run(["arch", "-x86_64", str(root / "host"), str(image), profile],
                             env=environment, timeout=30)
                assert "i386 struct-return stack: PASS" in output, output
                print(f"Struct-return {profile} {optimization} cache={'off' if disabled else 'on'}: PASS")
