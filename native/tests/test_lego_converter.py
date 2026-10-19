#!/usr/bin/env python3
"""Exercise the shipped engine, publication safety, and supported layouts.

Uses private fixtures made from locally available game images. Game archives
are tiny fixtures; a real installation can also be passed with --real-source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import select
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GAMES = {
    "clonewars": ("LEGOCloneWars", "LEGOCloneWars.macbin", "com.feralinteractive.legostarwarsiii"),
    "pirates": ("LEGOPirates", "LEGOPirates.macbin", "com.t-tales.www"),
    "marvel": ("LEGOMarvel", "LEGOMarvel.macbin", "com.feralinteractive.legomarvelsuperheroes"),
    "batman3": ("LEGOBatman3", "LEGO Batman 3", "com.feralinteractive.legobatman3"),
    "movie": ("LEGOMovie", "The LEGO Movie", "com.feralinteractive.legomovie"),
    "hobbit": ("LEGOHobbit", "LEGO The Hobbit", "com.feralinteractive.legohobbit"),
    "saga-retail": ("LEGOCompleteSaga", "LEGO Star Wars Saga", "com.feralinteractive.Saga"),
    "saga-steam": ("LEGOCompleteSaga", "LEGO Star Wars Saga", "com.feralinteractive.Saga"),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(folder):
    result = {}
    for path in folder.rglob("*"):
        key = str(path.relative_to(folder))
        if path.is_symlink(): result[key] = ("link", os.readlink(path))
        elif path.is_file(): result[key] = ("file", digest(path))
    return result


def fixture(game, folder):
    stem, executable, identifier = GAMES[game]
    bundle = ROOT / "build" / (stem + ("-Steam" if game == "saga-steam" else "") + "-Compat.app")
    image = bundle / f"Contents/SharedSupport/{stem}.image"
    if not image.is_file():
        return None
    app = folder / (game + ".app")
    contents = app / "Contents"
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Resources").mkdir()
    info = {"CFBundleIdentifier": identifier, "CFBundleName": game,
            "CFBundleExecutable": executable, "CFBundlePackageType": "APPL"}
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))
    shutil.copy2(image, contents / "MacOS" / executable)
    if game not in ("saga-steam", "saga-retail"):
        cg = bundle / "Contents/Library/Frameworks/Cg.framework"
        target = contents / "Library/Frameworks/Cg.framework"
        target.parent.mkdir(parents=True)
        shutil.copytree(cg, target, symlinks=True)
    if game == "pirates":
        packed = ROOT.parent / "analysis/original-copy/LEGOPirates.packed.macbin"
        if packed.is_file(): shutil.copy2(packed, contents / "MacOS" / executable)
        for name in ["GAME.DAT"] + [f"GAME{i}.DAT" for i in range(5)]:
            (contents / "MacOS" / name).write_bytes(b"fixture archive\n")
    steam_source = bundle / "Contents/Resources/libsteam_api.dylib"
    if steam_source.is_file():
        target = contents / "Frameworks/libsteam_api.dylib"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(steam_source, target)
    if game == "saga-steam":
        if not (contents / "Frameworks").exists(): (contents / "Frameworks").mkdir()
        data = folder / "LEGOStarWarsSagaData"; data.mkdir(exist_ok=True)
        for name in ["game.dat", "savegame.zip"] + [f"episode_{i}.dat" for i in ("i", "ii", "iii", "iv", "v", "vi")]:
            (data / name).write_bytes(b"fixture archive\n")
        for name in ["audio", "movies", "feralshaders", "stuff"]: (data / name).mkdir(exist_ok=True)
    if game == "saga-retail":
        data = folder / "Data"; data.mkdir(exist_ok=True)
        for name in ["GAME.DAT"] + [f"EPISODE_{i}.DAT" for i in ("I", "II", "III", "IV", "V", "VI")]:
            (data / name).write_bytes(b"fixture archive\n")
        for name in ["Audio", "Movies"]: (data / name).mkdir(exist_ok=True)
    (contents / "Resources/fixture.txt").write_text("original resource\n")
    return app


def run(executable, *arguments, success=True, timeout=90):
    env = {k: v for k, v in os.environ.items() if not k.startswith("LP32_")}
    env.update(PATH="/usr/bin:/bin", DEVELOPER_DIR="/no-developer-tools")
    result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True, text=True,
                            timeout=timeout, env=env)
    assert (result.returncode == 0) == success, (arguments, result.returncode, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "build/LEGO-Converter.app")
    parser.add_argument("--real-source", type=Path)
    args = parser.parse_args()
    executable = args.app.resolve() / "Contents/MacOS/LEGOConverter"
    with tempfile.TemporaryDirectory(prefix="converter-regression-", dir=ROOT / "build") as temporary:
        root = Path(temporary)
        apps = {}
        for game in GAMES:
            app = fixture(game, root)
            if app is None:
                print("SKIP missing local game image:", game); continue
            apps[game] = app
            before = snapshot(app)
            inspected = json.loads(run(executable, "--inspect", app).stdout)
            assert inspected["game"] == game, inspected
            result = run(executable, "--convert", app, root / "output", "--log-dir", root / "logs")
            converted = Path(result.stdout.strip())
            info = plistlib.loads((converted / "Contents/Info.plist").read_bytes())
            assert info["LEGOConverterConversion"]["game"] == game
            assert info["CFBundleExecutable"] == GAMES[game][0] + "Compat"
            subprocess.run(["codesign", "--verify", "--deep", "--strict", str(converted)], check=True)
            assert snapshot(app) == before, "Conversion changed the original game"
            if game == "saga-steam": assert (converted.parent / "LEGOStarWarsSagaData/game.dat").is_file()
            if game == "saga-retail": assert (converted.parent / "Data/GAME.DAT").is_file()
            print("PASS conversion, metadata, signature, source preservation:", game, flush=True)
        assert apps, "No game images available for the integration checks"
        source = apps.get("clonewars", next(iter(apps.values())))
        game = next(game for game, app in apps.items() if app == source)
        # A Steam game folder resolves to its original app, including nested layouts.
        steam_folder = root / "steamapps/common/Example Game"
        steam_folder.mkdir(parents=True)
        steam_app = steam_folder / source.name
        shutil.copytree(source, steam_app, symlinks=True)
        assert json.loads(run(executable, "--inspect", steam_folder).stdout)["source"] == str(steam_app)
        converted = Path(run(executable, "--convert", steam_folder, root / "steam-output",
                             "--log-dir", root / "logs").stdout.strip())
        assert (converted / "Contents/Resources/fixture.txt").read_text() == "original resource\n"
        run(executable, "--convert", steam_folder, steam_folder / "output", success=False)
        assert not (steam_folder / "output").exists()
        nested = steam_folder / "Mac"; nested.mkdir()
        steam_app.rename(nested / source.name)
        assert json.loads(run(executable, "--inspect", steam_folder).stdout)["game"] == game
        shutil.copytree(nested / source.name, nested / "Second.app", symlinks=True)
        run(executable, "--inspect", steam_folder, success=False)
        run(executable, "--inspect", root / "steam-output", success=False)
        print("PASS Steam folder discovery, folder conversion, ambiguous and converted folder rejection", flush=True)
        # A wrong bundle identifier must not override the executable fingerprint.
        info_file = source / "Contents/Info.plist"
        info = plistlib.loads(info_file.read_bytes())
        info["CFBundleIdentifier"] = "org.example.wrong-label"
        info_file.write_bytes(plistlib.dumps(info))
        assert json.loads(run(executable, "--inspect", source).stdout)["game"] == game
        first = Path(run(executable, "--convert", source, root / "duplicates", "--game", game,
                         "--log-dir", root / "logs").stdout.strip())
        marker = first / "user-kept-this.txt"; marker.write_text("keep\n")
        second = Path(run(executable, "--convert", source, root / "duplicates", "--game", game,
                          "--log-dir", root / "logs").stdout.strip())
        assert first != second and marker.read_text() == "keep\n"
        wrong = "hobbit" if game != "hobbit" else "clonewars"
        run(executable, "--convert", source, root / "duplicates", "--game", wrong,
            "--log-dir", root / "logs", success=False)
        run(executable, "--convert", source, source / "nested", "--log-dir", root / "logs", success=False)
        assert not (source / "nested").exists()
        run(executable, "--inspect", first, success=False)
        print("PASS manual override, incorrect override rejection, duplicate preservation, unsafe destination rejection", flush=True)

        # External resource links must not escape into the published app.
        outside = root / "private.txt"; outside.write_text("private")
        link = source / "Contents/Resources/external-link"; link.symlink_to(outside)
        run(executable, "--convert", source, root / "external-link-output", "--log-dir", root / "logs", success=False)
        assert not list((root / "external-link-output").iterdir())
        link.unlink()
        # A signal during a multi-file copy uses the GUI cancellation cleanup.
        many = source / "Contents/Resources/cancel-fixture"; many.mkdir()
        for i in range(2500): (many / f"{i}.txt").write_text("test\n")
        cancel_output = root / "cancel-output"
        process = subprocess.Popen([str(executable), "--convert", str(source), str(cancel_output),
            "--log-dir", str(root / "logs")], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            while True:
                readable, _, _ = select.select([process.stderr], [], [], 20)
                assert readable, "No copy stage reached before cancellation"
                line = process.stderr.readline()
                assert line, "Conversion exited before cancellation"
                if "Copying your game:" in line:
                    process.send_signal(signal.SIGTERM); break
            stdout, stderr = process.communicate(timeout=15)
            assert process.returncode != 0, (stdout, stderr)
            assert "cancelled" in stderr.lower(), stderr
            assert cancel_output.is_dir() and not list(cancel_output.iterdir())
        finally:
            if process.poll() is None: process.kill(); process.wait()
        assert outside.read_text() == "private" and marker.read_text() == "keep\n"
        print("PASS external symlink rejection and cancellation cleanup", flush=True)
        if args.real_source:
            result = run(executable, "--convert", args.real_source, root / "real-output",
                         "--log-dir", root / "logs", timeout=240)
            print("PASS real installation:", result.stdout.strip(), flush=True)


if __name__ == "__main__": main()
