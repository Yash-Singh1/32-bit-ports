#!/usr/bin/env python3
"""Exercise the shipped L4D2 converter with synthetic game files and a verified runtime cache."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    native = Path(__file__).resolve().parents[1]
    parser.add_argument('--app', type=Path, default=native / 'build/L4D2-Converter.app')
    parser.add_argument('--runtime', type=Path, required=True,
                        help='prepared guest-runtime directory; libraries are verified by the converter')
    args = parser.parse_args()
    app = args.app.resolve()
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    assert info['CFBundleExecutable'] == 'L4D2Converter'
    assert info['CFBundleIdentifier'] == 'org.32bitgoofy.L4D2Converter'
    assert info['CFBundleVersion'] == '1'
    assert '@tomtastic' in info['NSHumanReadableCopyright']
    executable = app / 'Contents/MacOS/L4D2Converter'
    environment = dict(os.environ, PATH='/usr/bin:/bin', DEVELOPER_DIR='/nonexistent')

    def unsigned_digest(path, scratch):
        shutil.copyfile(path, scratch)
        subprocess.run(['/usr/bin/codesign', '--remove-signature', str(scratch)], check=True)
        return digest(scratch)

    with tempfile.TemporaryDirectory(prefix='l4d2-converter-', dir=native / 'build') as folder:
        root = Path(folder).resolve()
        source = root / "Left 4 Dead 2 ' $ ` literal"
        output = root / 'output'
        output.mkdir()
        cache = root / 'cache'
        (cache / 'runtime').mkdir(parents=True)
        for name in ('libstdc++.6.dylib', 'libc++abi.dylib'):
            shutil.copyfile(args.runtime / name, cache / 'runtime' / name)
        # A packaging fixture, not executable game code.
        image = root / 'original-launcher'
        image.write_bytes(bytes.fromhex('cefaedfe070000000300000002000000') + bytes(16))
        image.chmod(0o755)
        files = {'bin/osx32/launcher.dylib': b'fixture',
                 'left4dead2/gameinfo.txt': b'fixture',
                 'left4dead2/cfg/config.cfg': b'original settings',
                 'left4dead2/resource/game.icns': b'fixture icon'}
        for relative, data in files.items():
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (source / 'hl2_osx').symlink_to(image)
        (source / 'compat-runtime').symlink_to(cache / 'runtime')
        before = {p: digest(source / p) for p in files}
        command = [str(executable), '--convert', str(source), str(output), '--cache', str(cache)]

        def run(*prefix):
            return subprocess.run([*prefix, *command], env=environment, capture_output=True,
                                  text=True, timeout=45)

        for architecture, name in [('arm64', 'L4D2-Compat.app'), ('x86_64', 'L4D2-Compat 2.app')]:
            result = run('/usr/bin/arch', '-' + architecture)
            assert result.returncode == 0, result.stdout + result.stderr
            converted = output / name
            assert Path(result.stdout.strip()) == converted
            contents = converted / 'Contents'
            game = contents / 'SharedSupport/L4D2'
            assert (contents / 'SharedSupport/L4D2.image').read_bytes() == image.read_bytes()
            assert not (contents / 'SharedSupport/L4D2.image').is_symlink()
            assert not (game / 'hl2_osx').is_symlink()
            assert not (game / 'compat-runtime').is_symlink()
            # Signing the new app changes the loader's signature identifier.
            assert unsigned_digest(contents / 'MacOS/L4D2Compat', root / 'output-loader') == unsigned_digest(app / 'Contents/Resources/game_loader', root / 'input-loader')
            assert plistlib.loads((contents / 'Info.plist').read_bytes())['CFBundleExecutable'] == 'L4D2Compat'
            for relative, data in files.items():
                assert (game / relative).read_bytes() == data
            for library in ('libstdc++.6.dylib', 'libc++abi.dylib'):
                assert digest(game / 'compat-runtime' / library) == digest(args.runtime / library)
            subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(converted)], check=True)
            if architecture == 'arm64':
                (game / 'left4dead2/cfg/config.cfg').write_text('existing converted settings')
        assert (output / 'L4D2-Compat.app/Contents/SharedSupport/L4D2/left4dead2/cfg/config.cfg').read_text() == 'existing converted settings'
        assert {p: digest(source / p) for p in files} == before
        assert image.stat().st_mode & 0o777 == 0o755
        assert len(list(output.iterdir())) == 2
        print('PASS: both architectures convert without developer tools, private runtime/image, signatures, numbered output and source preservation')

        # Copy cancellation exercises the shipped signal handler and staging cleanup.
        bulk = source / 'bulk'
        bulk.mkdir()
        for index in range(3000):
            (bulk / str(index)).write_bytes(b'fixture')
        process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            while True:
                line = process.stderr.readline()
                assert line, 'converter exited before copying'
                if line.startswith('Copying Left 4 Dead 2'):
                    process.send_signal(signal.SIGTERM)
                    break
            _, error = process.communicate(timeout=15)
            assert process.returncode != 0 and 'Conversion cancelled.' in error, error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        assert len(list(output.iterdir())) == 2, 'cancelled conversion left staging/output'
        assert {p: digest(source / p) for p in files} == before
        print('PASS: cancellation removes only its staging and leaves source/existing apps unchanged')


if __name__ == '__main__':
    main()
