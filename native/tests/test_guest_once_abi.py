#!/usr/bin/env python3
"""Run once through real i386 import thunks without starting game code."""
import os
from pathlib import Path
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="lp32-once-fixture-") as directory:
    root = Path(directory)
    source = root / "start.S"
    source.write_text(".text\n.globl _start\n_start:\n ret\n")
    image = root / "COD4.image"
    subprocess.run(["xcrun", "clang", "-target", "i386-apple-macos10.6",
                    "-nostdlib", "-Wl,-e,_start,-no_pie", str(source),
                    "-o", str(image)], check=True)
    for chained in (False, True):
        env = dict(os.environ, LP32_GAME="cod4", LP32_ONCE_SELFTEST="1",
                   LP32_NO_DIAGNOSTIC_LOG="1")
        env.pop("LP32_NO_FAST_IMPORTS", None)
        if chained:
            env["LP32_NO_FAST_IMPORTS"] = "1"
        subprocess.run(["arch", "-x86_64", str(native / "build/game_loader"),
                        str(image)], env=env, check=True, timeout=30)
        print(f"guest once ABI {'chained' if chained else 'fast'} imports PASS")
