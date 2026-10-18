#!/usr/bin/env python3
"""Convert tiny fixtures with relocated standalone apps. Never launch a game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

NATIVE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NATIVE / 'tools'))
from prepare_guest_runtime import LIBRARIES, sha256


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=NATIVE / 'build/guest-runtime',
                        help='Existing verified COD4 runtime, copied locally; no downloads')
    parser.add_argument('--game', choices=['cod2', 'cod4', 'mw2', 'mw3'], action='append')
    args = parser.parse_args()
    for name, (_, digest) in LIBRARIES.items():
        if not (args.runtime / name).is_file() or sha256(args.runtime / name) != digest:
            parser.error(f'Provide an existing verified private runtime with --runtime: {name}')
    with tempfile.TemporaryDirectory(prefix="cod-converters ' $ ") as directory:
        root = Path(directory)
        cache = root / 'cache'
        shutil.copytree(args.runtime, cache / 'runtime')
        host = root / 'steam.dylib'
        run('xcrun', 'clang', '-x', 'c', '-', '-dynamiclib', '-arch', 'x86_64', '-o', host,
            input=b'int fixture_library = 1;\n', stdout=subprocess.DEVNULL)
        guest = bytes.fromhex('cefaedfe07000000') + bytes(24)
        env = dict(os.environ, PATH='/usr/bin:/bin:/usr/sbin:/sbin')
        for game in args.game or ['cod2', 'cod4', 'mw2', 'mw3']:
            carbon = game in ('cod2', 'cod4')
            uses_metal = game != 'cod2'
            title = 'Call of Duty 2' if game == 'cod2' else 'Call of Duty 4'
            short = game.upper()
            moved = root / f"{short} relocated ' $.app"
            shutil.copytree(NATIVE / f'build/{short}-Converter.app', moved)
            run('codesign', '--verify', '--deep', '--strict', moved)
            converter = moved / f'Contents/MacOS/{short}Converter'
            converter_metal = moved / 'Contents/Frameworks/GLMetal'
            manifest = json.loads((moved / 'Contents/Resources/GLMetal-build-info.json').read_text()) if uses_metal else {'files': {}}
            if not uses_metal:
                assert not converter_metal.exists()
            for name, hashes in manifest['files'].items():
                assert sha256(NATIVE / 'glmetal/build' / name) == hashes['source_sha256']
                assert sha256(converter_metal / name) == hashes['bundled_sha256']
            for mode in ['sp', 'mp']:
                stem = short + ('MP' if mode == 'mp' else '')
                folder = root / f'{game}-{mode}'
                app_name = (title + (' Multiplayer' if mode == 'mp' else '')) if carbon else f'COD_{short}_{mode.upper()}'
                source = folder / (app_name + '.app')
                contents = source / 'Contents'
                macos = contents / 'MacOS'
                resources = contents / 'Resources'
                macos.mkdir(parents=True)
                resources.mkdir()
                (macos / (app_name + ('' if carbon else 'sub'))).write_bytes(guest)
                shutil.copy2(host, macos / 'libsteam_api.dylib')
                if not carbon:
                    bink = 'libBink2Macx86.dylib' if (game, mode) == ('mw3', 'mp') else 'libBinkMacx86.dylib'
                    for name in [bink, 'libMilesX86.dylib']:
                        (macos / name).write_bytes(guest)
                    (macos / 'codec.asi').write_bytes(b'MZfixture')
                    (macos / 'host.asi').write_bytes(b'not a guest PE codec')
                data = contents / f'{title} Data' if carbon else folder / 'GameData'
                (data / 'main').mkdir(parents=True)
                (data / 'main/iw_00.iwd').write_bytes(b'fixture game data')
                source_info = {'CFBundleExecutable': app_name, 'CFBundleIdentifier': f'test.{game}.{mode}',
                               'LSEnvironment': {'KEEP_ME': 'yes', 'LP32_GL_BACKEND': 'metal' if game == 'cod2' else 'apple', 'LP32_GLMETAL_PATH': '/stale/driver'}}
                (contents / 'Info.plist').write_bytes(plistlib.dumps(source_info))
                before = {str(p.relative_to(folder)): sha256(p) for p in folder.rglob('*') if p.is_file()}
                destination = root / f'output-{game}-{mode}'
                destination.mkdir()
                result = run(converter, '--convert', source, destination, '--mode', mode, '--cache', cache,
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
                output = Path(result.stdout.strip())
                assert output.name == stem + '-Compat.app', result.stdout
                out = output / 'Contents'
                info = plistlib.loads((out / 'Info.plist').read_bytes())
                assert info['LP32GeneratedGame'] == game and info['LP32GLMetal'] is uses_metal
                assert info['CFBundleExecutable'] == stem + 'Compat'
                assert info['LSEnvironment'] == {'KEEP_ME': 'yes', 'LP32_GL_BACKEND': 'metal' if uses_metal else 'apple'}
                if uses_metal:
                    assert json.loads((out / 'Resources/GLMetal-build-info.json').read_text()) == manifest
                else:
                    assert not (out / 'Frameworks/GLMetal').exists()
                    assert not (out / 'Resources/GLMetal-build-info.json').exists()
                if carbon:
                    for name, (_, digest) in LIBRARIES.items():
                        assert sha256(out / 'SharedSupport/compat-runtime' / name) == digest
                    assert bool(info.get('NSAppTransportSecurity', {}).get('NSAllowsArbitraryLoads')) == (mode == 'mp')
                for name, hashes in manifest['files'].items():
                    assert sha256(out / 'Frameworks/GLMetal' / name) == hashes['bundled_sha256']
                assert (out / f'SharedSupport/{stem}.image').read_bytes() == guest
                if not carbon:
                    assert (out / 'SharedSupport' / bink).is_file()
                    assert (out / 'SharedSupport/codec.asi').read_bytes() == b'MZfixture'
                    assert not (out / 'SharedSupport/host.asi').exists()
                run('codesign', '--verify', '--strict', output)
                for name in manifest['files']:
                    run('codesign', '--verify', '--strict', '--all-architectures', out / 'Frameworks/GLMetal' / name)
                assert before == {str(p.relative_to(folder)): sha256(p) for p in folder.rglob('*') if p.is_file()}
                print(f'PASS: relocated {short} {mode} conversion, renderer package, signatures and source preservation', flush=True)


if __name__ == '__main__':
    main()
