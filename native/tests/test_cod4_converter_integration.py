#!/usr/bin/env python3
"""Exercise the standalone converter with the user's Steam Mac files, read-only."""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile

NATIVE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NATIVE / 'tools'))
import bundle_cod4
import prepare_guest_runtime as runtime


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def inventory(root):
    result = {}
    for path in root.rglob('*'):
        stat = path.lstat()
        result[str(path.relative_to(root))] = (stat.st_mode, stat.st_size, stat.st_mtime_ns)
    return result


def cancel_at(command, prefix, destination, env):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        for line in process.stderr:
            if line.startswith(prefix):
                process.send_signal(signal.SIGTERM)
                break
        stdout, stderr = process.communicate(timeout=45)
        assert process.returncode == 1 and 'cancelled' in stderr, (stdout, stderr, process.returncode)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert not list(destination.glob('.cod4-converting-*')), 'Unfinished output was left behind'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--game', choices=['cod2', 'cod4'], default='cod4')
    parser.add_argument('--installer', type=Path, help='Also exercise Apple archive extraction')
    parser.add_argument('--runtime', type=Path, default=NATIVE / 'build/guest-runtime')
    args = parser.parse_args()
    source = args.source.resolve() if args.source else bundle_cod4.steam_source(game=args.game)
    before = inventory(source)
    short = args.game.upper()
    title = bundle_cod4.GAMES[args.game]['title']
    converter_app = NATIVE / f'build/{short}-Converter.app'
    swift = (NATIVE / 'cod4-converter/Conversion.swift').read_text()
    assert runtime.URL in swift and runtime.INSTALLER_SHA256 in swift
    for name, (member, digest) in runtime.LIBRARIES.items():
        assert all(x in swift for x in (name, member, digest))
    with tempfile.TemporaryDirectory(prefix='cod4-converter-test-', dir=NATIVE / 'build') as directory:
        root = Path(directory)
        moved = root / "Converter ' standalone $.app"
        run('/usr/bin/ditto', converter_app, moved)
        run('/usr/bin/codesign', '--verify', '--deep', '--strict', moved)
        executable = moved / f'Contents/MacOS/{short}Converter'
        cache = root / "cache ' $"
        destination = root / "output ' $"
        cache.mkdir()
        destination.mkdir()
        if args.installer:
            run('/bin/cp', '-c', args.installer.resolve(), cache / 'InstallMacOSX-Lion.dmg')
        else:
            shutil.copytree(args.runtime, cache / 'runtime')
        # No checkout, Python or developer-tool commands are available to the app.
        env = dict(os.environ, PATH='/usr/bin:/bin:/usr/sbin:/sbin')
        base = [str(executable), '--convert', str(source), str(destination), '--cache', str(cache)]
        outputs = []
        for mode in ('sp', 'mp'):
            print(f'Converting real Steam {mode} with the relocated app…', flush=True)
            result = run(*base, '--mode', mode, stdout=subprocess.PIPE, text=True, env=env, timeout=300)
            output = Path(result.stdout.strip())
            outputs.append(output)
            suffix = '' if mode == 'sp' else 'MP'
            assert output == destination / f'{short}{suffix}-Compat.app'
            contents = output / 'Contents'
            run('/usr/bin/codesign', '--verify', '--strict', output)
            info = plistlib.loads((contents / 'Info.plist').read_bytes())
            assert info['CFBundleExecutable'] == f'{short}{suffix}Compat'
            original_info = bundle_cod4.validate_source(source, mode, game=args.game)[2]
            assert info['CFBundleIdentifier'] == original_info['CFBundleIdentifier'] + '.compat'
            assert info['LP32GeneratedGame'] == args.game and not info['LP32ContinueWhenInactive']
            assert info.get('LP32GLMetal') is (args.game != 'cod2')
            assert info['LSEnvironment']['LP32_GL_BACKEND'] == ('apple' if args.game == 'cod2' else 'metal')
            assert info.get('NSAppTransportSecurity', {}).get('NSAllowsArbitraryLoads', False) == (mode == 'mp')
            assert runtime.sha256(contents / f'SharedSupport/{short}{suffix}.image') == runtime.sha256(bundle_cod4.validate_source(source, mode, game=args.game)[1])
            for name, (_, digest) in runtime.LIBRARIES.items():
                assert runtime.sha256(contents / 'SharedSupport/compat-runtime' / name) == digest
            # Both game-data trees are independent copies with the original bytes.
            for relative in (('main/iw_00.iwd', 'main/iw_01.iwd') if args.game == 'cod2' else ('main/iw_00.iwd', 'zone/english/cargoship.ff')):
                original = source / f'Contents/{title} Data' / relative
                copied = contents / f'{title} Data' / relative
                assert runtime.sha256(original) == runtime.sha256(copied)
                assert original.stat().st_ino != copied.stat().st_ino
            checkenv = dict(env, LP32_INITIALIZERS_SELFTEST='1', LP32_BACKGROUND_TEST='1',
                            LP32_MUTE_AUDIO='1', LP32_NO_DIAGNOSTIC_LOG='1',
                            LP32_TEST_HOME_DIR=str(root / f'home-{mode}'),
                            LP32_APPLICATION_SUPPORT_DIR=str(root / f'home-{mode}/Library/Application Support'))
            checked = run('/usr/bin/arch', '-x86_64', contents / f'MacOS/{short}{suffix}Compat',
                          env=checkenv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=45)
            assert 'initializers completed:' in checked.stdout, checked.stdout[-4000:]
            print(f'PASS: {mode} signature, original data, private runtime and guest initializers', flush=True)
        sentinel = outputs[0] / f'Contents/{title} Data/converter-test-checkpoint'
        sentinel.write_text('Preserve this newer checkpoint.\n')
        previous = inventory(outputs[0])
        cached = inventory(cache / 'runtime')
        repeated = run(*base, stdout=subprocess.PIPE, text=True, env=env, timeout=120)
        assert Path(repeated.stdout.strip()).name == f'{short}-Compat 2.app'
        assert inventory(outputs[0]) == previous and inventory(cache / 'runtime') == cached
        cancel_at(base, f'Copying {title}', destination, env)
        assert len(list(destination.iterdir())) == 3
        assert inventory(outputs[0]) == previous
        assert not list(cache.glob('extract-*'))
        mounts = plistlib.loads(run('/usr/bin/hdiutil', 'info', '-plist', stdout=subprocess.PIPE).stdout)
        assert str(root) not in str(mounts), 'An extraction volume was left mounted'
    assert inventory(source) == before, 'Original installation changed'
    print('PASS: SP/MP, relocated app, runtime preparation/cache, collision, cancellation, cleanup and original preservation')


if __name__ == '__main__':
    main()
