"""Exercise nested guest/host x87 calls using only synthetic i386 code."""
import os
from pathlib import Path
import subprocess
import tempfile

native = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="lp32-asphalt-float-") as directory:
    image = Path(directory) / "Asphalt6.image"
    subprocess.run([
        "xcrun", "clang", "-target", "i386-apple-macos10.6", "-nostdlib",
        "-Wl,-e,_start,-no_pie", str(native / "tests/fixtures/asphalt6_float.S"),
        "-o", str(image),
    ], check=True)
    for flags in ({}, {"LP32_NO_FAST_IMPORTS": "1"}, {"LP32_GUEST_LOCK": "1"}):
        environment = dict(os.environ, LP32_GAME="asphalt6", LP32_ASPHALT6_FLOAT_SELFTEST="1",
                           LP32_NO_DIAGNOSTIC_LOG="1")
        environment.update(flags)
        result = subprocess.run(["arch", "-x86_64", str(native / "build/game_loader"), str(image)],
                                env=environment, capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        print(result.stderr, end="")
        assert result.returncode == 0
        assert "Asphalt nested x87 controller fixture: PASS" in result.stderr
