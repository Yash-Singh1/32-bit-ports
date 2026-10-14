#!/usr/bin/env python3
"""Exercise actual i386 atomic imports and concurrent host ordering."""
import os
from pathlib import Path
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]
loader = Path(os.environ.get("LP32_TEST_LOADER", str(native / "build/game_loader")))
with tempfile.TemporaryDirectory(prefix="lp32-pure-atomic-") as directory:
    root = Path(directory)
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _OSAtomicAdd32, _OSAtomicAdd32Barrier, _OSAtomicCompareAndSwap32,
      _OSAtomicCompareAndSwap32Barrier, _OSAtomicCompareAndSwapInt,
      _OSAtomicCompareAndSwapPtrBarrier, dyld_stub_binder ]
...
""")
    helper = root / "helper.S"
    helper.write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    image = root / "COD4.image"
    subprocess.run(["xcrun", "clang", "-target", "i386-apple-macos10.6",
                    "-nostdlib", "-Wl,-e,_start,-no_pie", "-L" + str(root),
                    "-lSystem", str(helper), str(native / "tests/fixtures/pure_atomic.S"),
                    "-o", str(image)], check=True)
    cases = ({}, {"LP32_NO_PURE_IMPORTS": "1"}, {"LP32_NO_FAST_IMPORTS": "1"},
             {"LP32_HITCH_FULL_IMPORTS": "1"}, {"LP32_FRAME_STATS": "1"},
             {"LP32_TRACE_ATOMIC": "0"}, {"LP32_GUEST_LOCK": "1"})
    for flags in cases:
        env = dict(os.environ, LP32_GAME="cod4", LP32_PURE_ATOMIC_SELFTEST="1",
                   LP32_NO_DIAGNOSTIC_LOG="1")
        for name in ("LP32_NO_PURE_IMPORTS", "LP32_NO_FAST_IMPORTS", "LP32_HITCH_FULL_IMPORTS",
                     "LP32_FRAME_STATS", "LP32_TRACE_ATOMIC", "LP32_GUEST_LOCK"):
            env.pop(name, None)
        env.update(flags)
        result = subprocess.run(["arch", "-x86_64", str(loader), str(image)],
                                env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=30)
        print(result.stdout, end="")
        assert result.returncode == 0
        assert f"route={'gateway' if flags else 'inline'}" in result.stdout
        print(f"pure atomic CPU fixture PASS {flags or 'inline'}")
