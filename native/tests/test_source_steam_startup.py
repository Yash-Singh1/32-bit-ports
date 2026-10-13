#!/usr/bin/env python3
"""Check Source helper identity and failed authentication without live Steam."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


native = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--loader", type=Path, default=native / "build/game_loader")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="lp32-source-steam-") as directory:
    root = Path(directory)
    (root / "bin").mkdir()
    source = root / "steam_api.c"
    source.write_text(r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool identity(void) {
    const char *expected = getenv("FIXTURE_APP_ID");
    const char *app = getenv("SteamAppId"), *game = getenv("SteamGameId");
    if (!expected || !app || !game || strcmp(app, expected) || strcmp(game, expected)) abort();
    return true;
}
bool SteamAPI_IsSteamRunning(void) {
    identity();
    return getenv("FIXTURE_STOPPED") == NULL;
}
bool SteamAPI_InitSafe(void) {
    identity();
    fprintf(stderr, "fixture: initialize app %s\n", getenv("SteamAppId"));
    return getenv("FIXTURE_AUTH_FAILURE") == NULL;
}
void SteamAPI_Shutdown(void) {
    identity();
    fprintf(stderr, "fixture: shutdown app %s\n", getenv("SteamAppId"));
}
""")
    subprocess.run(["xcrun", "clang", "-arch", "x86_64", "-dynamiclib", "-O2",
                    str(source), "-o", str(root / "bin/libsteam_api.dylib")], check=True)

    def run(command, app_id="550", **options):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("LP32_", "FIXTURE_"))}
        environment.update(LP32_HEADLESS_ERRORS="1", LP32_GAME="clonewars",
                           SteamAppId="wrong-app", SteamGameId="wrong-game",
                           FIXTURE_APP_ID=app_id)
        environment.update(options)
        result = subprocess.run(["arch", "-x86_64", str(args.loader.resolve()), *command],
                                env=environment, capture_output=True, text=True, timeout=15)
        return result, result.stdout + result.stderr

    for profile, app_id, title in (("L4D2", "550", "Left 4 Dead 2"),
                                   ("Portal2", "620", "Portal 2")):
        result, output = run(["--prepare-steam", str(root), profile], app_id)
        assert result.returncode == 0, output
        assert f"fixture: initialize app {app_id}" in output, output
        assert f"fixture: shutdown app {app_id}" in output, output
        assert "Steam is ready" in output, output

        result, output = run(["--prepare-steam", str(root), profile], app_id,
                             FIXTURE_AUTH_FAILURE="1")
        assert result.returncode != 0, output
        assert f"fixture: initialize app {app_id}" in output, output
        assert "fixture: shutdown" not in output and "Steam is ready" not in output, output
        assert f"account that owns {title}" in output, output

        result, output = run(["--prepare-steam", str(root), profile], app_id,
                             FIXTURE_STOPPED="1")
        assert result.returncode != 0 and "fixture: initialize" not in output, output
        assert f"opening Steam before launching {title}" in output, output

        result, output = run(["--steam-required", profile], app_id)
        assert result.returncode != 0 and f"{title} couldn’t connect to Steam" in output, output

    # Older callers have an explicit Portal 2 compatibility default.
    result, output = run(["--prepare-steam", str(root)], "620")
    assert result.returncode == 0 and "fixture: shutdown app 620" in output, output
    result, output = run(["--steam-required"], "620")
    assert result.returncode != 0 and "Portal 2 couldn’t connect to Steam" in output, output

    for profile in ("unknown-game", "pirates"):
        result, output = run(["--prepare-steam", str(root), profile])
        assert result.returncode != 0 and "invalid Steam startup profile" in output, output
        assert "fixture: initialize" not in output, output

print("Source Steam startup: PASS (profile identity, denied initialization, stopped client, legacy helpers)")
