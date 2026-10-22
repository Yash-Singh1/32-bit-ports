#!/usr/bin/env python3
"""Compare real i386 core dispatch with and without import memoization.

The benchmark validates results/canaries in both modes. It reports medians;
timing is diagnostic and is deliberately not a flaky test assertion.
"""
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]
LOADER = Path(os.getenv("LP32_TEST_LOADER", str(NATIVE / "build/game_loader"))).resolve()
GAME = os.getenv("LP32_TEST_GAME", "l4d2")
if GAME not in ("l4d2", "portal2"):
    raise ValueError("LP32_TEST_GAME must be l4d2 or portal2")
EXECUTABLE = "hl2_osx" if GAME == "l4d2" else "portal2_osx"


def run(*arguments, **kwargs):
    return subprocess.run(arguments, check=True, **kwargs)


with tempfile.TemporaryDirectory(prefix="lp32-dispatch-bench-") as temporary:
    root = Path(temporary).resolve()
    (root / "bin").mkdir()
    symbols = "_fopen _fopen$UNIX2003 _fclose _fread _fseek _ftell _feof _ferror _strlen _strcmp _strncmp _strcasecmp _strrchr _memcmp ___toupper ___tolower _strtof _strtof$UNIX2003 _printf _UpTime _AbsoluteToNanoseconds ___stack_chk_guard ___stack_chk_fail dyld_stub_binder".split()
    (root / "libSystem.tbd").write_text("""--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ """ + ", ".join(symbols) + """ ]
...
""")
    (root / "helper.S").write_text(".text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n")
    (root / "start.S").write_text(".text\n.globl _start\n_start:\n ret\n")
    flags = ["xcrun", "clang", "-target", "i386-apple-macos10.6", "-O2",
             "-Wall", "-Wextra", "-Werror", "-fno-builtin", "-nostdlib",
             "-L" + str(root), "-lSystem", str(root / "helper.S")]
    if os.getenv("LP32_I386_LINKER"):
        flags.append("--ld-path=" + os.environ["LP32_I386_LINKER"])
    run(*flags, "-dynamiclib", "-Wl,-install_name,@loader_path/launcher.dylib",
        str(NATIVE / "tests/fixtures/dispatch_benchmark.c"),
        str(NATIVE / "tests/fixtures/dispatch_strings.c"), "-o", str(root / "bin/launcher.dylib"))
    image = root / EXECUTABLE
    run(*flags, "-Wl,-e,_start,-no_pie", str(root / "start.S"), "-o", str(image))
    (root / "archive.bin").write_bytes(bytes(range(256)) * 256)
    (root / "bin/broken.dylib").write_bytes(b"invalid fixture image")
    environment = dict(os.environ, LP32_GAME=GAME, LP32_DYLD_SELFTEST="1",
                       LP32_DYLD_FIXTURE_SELFTEST="1", LP32_LOG_DIR=str(root / "logs"))
    # Profiling/logging would measure a different path or add output overhead.
    for name in ("LP32_FRAME_STATS", "LP32_SLOW_IMPORT_MS", "LP32_TRACE_FILES", "LP32_NO_FAST_IMPORTS"):
        environment.pop(name, None)
    samples = {"memoized": {}, "chained": {}}
    expected_checksums = None
    for round_index in range(3):
        for mode in (("chained", "memoized") if round_index % 2 == 0 else ("memoized", "chained")):
            current = dict(environment)
            if mode == "chained":
                current["LP32_NO_FAST_IMPORTS"] = "1"
            result = run("arch", "-x86_64", str(LOADER), str(image), env=current,
                         capture_output=True, text=True, timeout=60)
            checksums = re.findall(r"LP32_BENCH result=(\d+)", result.stdout)
            assert len(checksums) == 2, (result.stdout, result.stderr)
            if expected_checksums is None:
                expected_checksums = checksums
            assert checksums == expected_checksums, (mode, checksums, expected_checksums)
            measurements = re.findall(r"LP32_BENCH case=([^ ]+) ns=(\d+) ops=(\d+) checksum=(\d+)", result.stdout)
            assert len(measurements) == 10, (result.stdout, result.stderr)
            for case, nanoseconds, operations, checksum in measurements:
                samples[mode].setdefault(case, []).append(int(nanoseconds) / int(operations))
    summary = {case: {"chained_ns_per_import": statistics.median(samples["chained"][case]),
                      "memoized_ns_per_import": statistics.median(samples["memoized"][case]),
                      "speedup": statistics.median(samples["chained"][case]) / statistics.median(samples["memoized"][case])}
               for case in samples["memoized"]}
    print(json.dumps({"game": GAME, "samples_per_mode": 6, "result_checksums": expected_checksums,
                      "cases": summary}, indent=2))
    print("Core dispatch benchmark: PASS (real i386, identical results/guards in both modes)")
