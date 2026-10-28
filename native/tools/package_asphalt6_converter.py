#!/usr/bin/env python3
"""Build a self-contained Asphalt 6: Adrenaline converter app from compiled components."""
import argparse
from pathlib import Path
import shutil
import tempfile

from package_glmetal import install, run, DEFAULT_BUILD


def package(build, glmetal):
    app = build / 'Asphalt6-Converter.app'
    with tempfile.TemporaryDirectory(prefix='.asphalt6-converter-', dir=build) as tmp:
        staged = Path(tmp) / app.name
        contents = staged / 'Contents'
        for name in ['MacOS', 'Resources']:
            (contents / name).mkdir(parents=True)
        run('lipo', '-create', build / 'asphalt6-converter-arm64', build / 'asphalt6-converter-x86_64',
            '-output', contents / 'MacOS/Asphalt6Converter')
        loader = contents / 'Resources/game_loader'
        shutil.copy2(build / 'game_loader', loader)
        run('codesign', '--force', '--sign', '-', loader)
        importer = contents / 'Resources/asphalt6-ipad-assets'
        run('lipo', '-create', build / 'asphalt6-ipad-assets-arm64', build / 'asphalt6-ipad-assets-x86_64',
            '-output', importer)
        run('codesign', '--force', '--sign', '-', importer)
        notices = contents / 'Resources/ThirdParty'
        third_party = Path(__file__).resolve().parents[1] / 'third_party'
        for library in ('powervr', 'stb'):
            (notices / library).mkdir(parents=True)
            for name in ('README.md', 'LICENSE.md' if library == 'powervr' else 'LICENSE'):
                shutil.copy2(third_party / library / name, notices / library / name)
        install(glmetal, contents / 'Frameworks/GLMetal', contents / 'Resources/GLMetal-build-info.json')
        shutil.copy2(Path(__file__).resolve().parents[1] / 'asphalt6-converter/Info.plist', contents / 'Info.plist')
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
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--glmetal', type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()
    package(args.build, args.glmetal)
