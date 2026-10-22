#!/usr/bin/env python3
"""Check Carbon mouse coordinates and guest HIPoint writes using hidden windows."""
import argparse
import os
from pathlib import Path
import subprocess

native = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--loader", type=Path, default=native / "build/game_loader")
args = parser.parse_args()
environment = dict(os.environ, LP32_HI_MOUSE_SELFTEST="1", LP32_BACKGROUND_TEST="1")
result = subprocess.run(["arch", "-x86_64", str(args.loader.resolve())], env=environment,
                        capture_output=True, text=True, timeout=30)
output = result.stdout + result.stderr
assert result.returncode == 0 and "Carbon mouse self-test: PASS" in output, output
print("Carbon mouse bridge: PASS (actual cursor, hidden native windows, float32 output guards)")
