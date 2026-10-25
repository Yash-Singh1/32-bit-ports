#!/usr/bin/env python3
"""Build a self-contained COD2, COD4, MW2 or MW3 converter from compiled components."""
import argparse
from pathlib import Path
import plistlib
import shutil
import tempfile

from package_glmetal import install, run, DEFAULT_BUILD


# Each standalone converter has its own development release sequence.
BUILD_VERSIONS = {'cod2': '1', 'cod4': '6', 'mw2': '1', 'mw3': '1'}


def package(game, build, glmetal):
    title = {'cod2': 'Call of Duty 2', 'cod4': 'Call of Duty 4', 'mw2': 'Modern Warfare 2', 'mw3': 'Modern Warfare 3'}[game]
    short = game.upper()
    app = build / f'{short}-Converter.app'
    with tempfile.TemporaryDirectory(prefix=f'.{game}-converter-', dir=build) as tmp:
        staged = Path(tmp) / app.name
        contents = staged / 'Contents'
        for name in ['MacOS', 'Resources']:
            (contents / name).mkdir(parents=True)
        executable = contents / f'MacOS/{short}Converter'
        run('lipo', '-create', build / 'cod4-converter-arm64', build / 'cod4-converter-x86_64', '-output', executable)
        loader = contents / 'Resources/game_loader'
        shutil.copy2(build / 'game_loader', loader)
        run('codesign', '--force', '--sign', '-', loader)
        if game != 'cod2':
            install(glmetal, contents / 'Frameworks/GLMetal', contents / 'Resources/GLMetal-build-info.json')
        info = plistlib.loads((Path(__file__).resolve().parents[1] / 'cod4-converter/Info.plist').read_bytes())
        info.update(CFBundleExecutable=f'{short}Converter', CFBundleIdentifier=f'org.32bitgoofy.{short}Converter',
                    CFBundleName=f'{short} Converter', CFBundleDisplayName=f'{short} Converter',
                    LP32ConverterGame=game, CFBundleVersion=BUILD_VERSIONS[game],
                    NSHumanReadableCopyright=f'32bitgoofy. Requires your own Mac copy of {title}.')
        info['CFBundleDocumentTypes'][0]['CFBundleTypeName'] = f'{title} app or game folder'
        (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
        run('codesign', '--force', '--sign', '-', staged)
        run('codesign', '--verify', '--deep', '--strict', staged)
        previous = Path(tmp) / 'previous.app'
        if app.exists():
            app.rename(previous)
        try:
            staged.rename(app)
        except BaseException:
            if previous.exists():
                previous.rename(app)
            raise
    print(app)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=BUILD_VERSIONS, required=True)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--glmetal', type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()
    package(args.game, args.build, args.glmetal)
