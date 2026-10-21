#!/usr/bin/env python3
"""Build an Asphalt 6: Adrenaline compatibility app from Gameloft's 32-bit Mac release.

SOURCE may be Asphalt 6.app, a folder containing it, or the release's ZIP or disk
image (mounted read-only for the copy). The original files are only read.
"""
import argparse
import contextlib
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path

import package_glmetal
from bundle_cod4 import ditto, macho_slice, run

BUNDLE_ID = 'com.gameloft.asphalt6mac'
TITLE = 'Asphalt 6 Adrenaline'
STEM = 'Asphalt6'
RUNTIME = ('libstdc++.6.dylib', 'libc++abi.dylib')


@contextlib.contextmanager
def mounted_source(source):
    """Yield an app path for SOURCE, attaching a disk image read-only if needed."""
    if source.suffix.lower() == '.zip':
        with tempfile.TemporaryDirectory(prefix='asphalt6-source-') as directory:
            run('ditto', '-x', '-k', source, directory)
            yield Path(directory)
        return
    if source.suffix.lower() != '.dmg':
        yield source
        return
    mount = Path(tempfile.mkdtemp(prefix='asphalt6-source-'))
    try:
        run('hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', mount, source)
        yield mount
    finally:
        if subprocess.run(['hdiutil', 'detach', str(mount)], capture_output=True).returncode:
            subprocess.run(['hdiutil', 'detach', '-force', str(mount)], capture_output=True)
        try:
            mount.rmdir()
        except OSError:
            pass


def find_app(source):
    source = source.resolve()
    candidates = [source] if source.suffix == '.app' else []
    if source.is_dir():
        candidates += sorted(source.glob('*.app'))
    for app in candidates:
        info_path = app / 'Contents/Info.plist'
        if not info_path.is_file():
            continue
        info = plistlib.loads(info_path.read_bytes())
        if info.get('LP32GeneratedGame'):
            raise ValueError('Choose the original Asphalt 6 app, not a converted copy')
        if info.get('CFBundleIdentifier', '').lower() == BUNDLE_ID:
            return app, info
    raise ValueError(f'No Asphalt 6: Adrenaline Mac app ({BUNDLE_ID}) found in {source}')


def validate_source(app, info):
    contents = app / 'Contents'
    executable = info.get('CFBundleExecutable', '')
    if not executable or '/' in executable or executable in ('.', '..'):
        raise ValueError('The Asphalt 6 app has an invalid CFBundleExecutable')
    image = contents / 'MacOS' / executable
    for path in (image, contents / 'Resources/data/file000000.dat'):
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Incomplete Asphalt 6 installation: missing {path}')
    macho_slice(image.read_bytes(), 7)
    return image


def build(source, loader, bundle, runtime, glmetal=None):
    if not loader.is_file():
        raise ValueError(f'Missing loader: {loader}')
    for name in RUNTIME:
        if not (runtime / name).is_file():
            raise ValueError(f'Missing i386 C++ runtime {runtime / name}; run make cod4-runtime')
    if glmetal:
        package_glmetal.validate(glmetal)
    if bundle.exists():
        raise ValueError('The output already exists. Choose a new BUNDLE path to keep its saves.')
    with mounted_source(source) as root:
        app, info = find_app(root)
        image = validate_source(app, info)
        destination = bundle.resolve()
        if app.resolve() == destination or app.resolve() in destination.parents:
            raise ValueError('Build destination must be outside the original game')
        bundle.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.asphalt6-stage-', dir=bundle.parent) as tmp:
            staged = Path(tmp) / bundle.name
            contents = staged / 'Contents'
            for directory in ('MacOS', 'SharedSupport/compat-runtime'):
                (contents / directory).mkdir(parents=True)
            # The game reads its packed data and MainMenu.nib from Resources.
            ditto(app / 'Contents/Resources', contents / 'Resources')
            shutil.copy2(loader, contents / f'MacOS/{STEM}Compat')
            (contents / f'SharedSupport/{STEM}.image').write_bytes(macho_slice(image.read_bytes(), 7))
            for name in RUNTIME:
                shutil.copy2(runtime / name, contents / 'SharedSupport/compat-runtime' / name)
            info = dict(info)
            info.update(CFBundleExecutable=f'{STEM}Compat',
                        CFBundleIdentifier=f'{BUNDLE_ID}.compat',
                        CFBundleDisplayName=f'{TITLE} (Compatibility)',
                        LSMinimumSystemVersion='11.0', NSHighResolutionCapable=False,
                        LP32GeneratedGame='asphalt6', LP32ContinueWhenInactive=False)
            if glmetal:
                package_glmetal.install(glmetal, contents / 'Frameworks/GLMetal',
                                        contents / 'Resources/GLMetal-build-info.json')
                package_glmetal.configure(info)
            else:
                environment = dict(info.get('LSEnvironment', {}))
                environment['LP32_GL_BACKEND'] = 'apple'
                info['LSEnvironment'] = environment
                info['LP32GLMetal'] = False
            (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
            run('xattr', '-cr', staged)
            run('codesign', '--force', '--sign', '-', staged)
            run('codesign', '--verify', '--strict', staged)
            staged.rename(bundle)
    print(f'Asphalt 6 compatibility bundle: {bundle}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Asphalt 6.app, its folder, or the release .zip or .dmg')
    parser.add_argument('--loader', type=Path, default=Path('build/game_loader'))
    parser.add_argument('--bundle', type=Path, default=Path('build/Asphalt6-Compat.app'))
    parser.add_argument('--runtime', type=Path, default=Path('build/guest-runtime'))
    parser.add_argument('--glmetal', type=Path, default=package_glmetal.DEFAULT_BUILD,
                        help='Directory with the built GLMetal driver and compiler')
    renderer = parser.add_mutually_exclusive_group()
    renderer.add_argument('--metal', action='store_true', help='Use experimental GLMetal rendering')
    renderer.add_argument('--apple-gl', action='store_true', help="Use Apple OpenGL (the default)")
    args = parser.parse_args()
    try:
        build(args.source, args.loader.resolve(), args.bundle.absolute(), args.runtime.resolve(),
              args.glmetal.resolve() if args.metal else None)
    except (ValueError, OSError, plistlib.InvalidFileException, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
