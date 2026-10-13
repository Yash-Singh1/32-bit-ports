#!/usr/bin/env python3
"""Verify a real L4D2 Mac bundle/rebuild without entering gameplay or Steam."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def inventory(root):
    result = {}
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            path = Path(directory) / name
            st = path.lstat()
            result[str(path.relative_to(root))] = (st.st_mode, st.st_size, st.st_mtime_ns)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--copy-only', action='store_true',
                        help='check actual copying/rebuilding without a loader, runtime download or signing')
    args = parser.parse_args()
    native = Path(__file__).resolve().parents[1]
    source = args.source.resolve()
    run('make', '-C', native, 'build/l4d2_source')
    tool = native / 'build/l4d2_source'
    layout = json.loads(run(tool, '--describe', source, stdout=subprocess.PIPE, text=True).stdout)
    root = Path(layout['roots'][0])
    before = inventory(source)
    with tempfile.TemporaryDirectory(prefix='l4d2-bundle-', dir=native / 'build') as folder:
        app = Path(folder) / "L4D2 ' test.app"
        build = ('make', '-C', native, 'GAME=l4d2', 'SOURCE_GAME=' + str(source), 'BUNDLE=' + str(app))
        if args.copy_only:
            run(tool, '--copy', source, app)
        else:
            run(*build, 'bundle')
        contents = app / 'Contents'
        game = contents / 'SharedSupport/L4D2'
        assert digest(contents / 'SharedSupport/L4D2.image') == digest(game / 'hl2_osx')
        assert not list(game.rglob('.DepotDownloader'))
        for directory, dirs, files in os.walk(root):
            dirs[:] = [name for name in dirs if name not in ('.DepotDownloader', 'compat-runtime')]
            for name in files:
                original = Path(directory) / name
                copied = game / original.relative_to(root)
                assert copied.stat().st_size == original.stat().st_size, original
                if original.suffix == '.dylib' or name in ('gameinfo.txt', 'game.icns', 'hl2_osx'):
                    assert digest(copied) == digest(original), original
        env = dict(os.environ, LP32_GAME='l4d2', LP32_DYLD_SELFTEST='1')
        env.pop('LP32_GUEST_RUNTIME_DIR', None)
        if not args.copy_only:
            info = plistlib.loads((contents / 'Info.plist').read_bytes())
            assert info['CFBundleExecutable'] == 'L4D2Compat'
            assert (game / 'compat-runtime/libstdc++.6.dylib').is_file()
            assert (game / 'compat-runtime/libc++abi.dylib').is_file()
            run('/usr/bin/codesign', '--verify', '--deep', '--strict', app)
            run('/usr/bin/arch', '-x86_64', contents / 'MacOS/L4D2Compat', env=env)

        # Packaging preservation fixtures do not claim campaign save support.
        settings = game / 'left4dead2/cfg/config.cfg'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text('bind F9 "echo l4d2_bundle_test"\n')
        (game / 'bin/obsolete.dylib').write_text('obsolete binary')
        if args.copy_only:
            run(tool, '--copy', source, app)
        else:
            run(*build, 'bundle')
        assert settings.read_text() == 'bind F9 "echo l4d2_bundle_test"\n'
        assert not (game / 'bin/obsolete.dylib').exists()
        if not args.copy_only:
            run('/usr/bin/arch', '-x86_64', contents / 'MacOS/L4D2Compat', env=env)
            data_before = inventory(game)
            run(*build, 'promote-loader')
            assert inventory(game) == data_before, 'loader promotion changed game data/settings'
        assert inventory(source) == before, 'original game changed'
        print('PASS: real L4D2 copy, library hashes, rebuild/settings, stale libraries and source preservation')
        if not args.copy_only:
            print('PASS: signature, dependency loading and loader promotion')


if __name__ == '__main__':
    main()
