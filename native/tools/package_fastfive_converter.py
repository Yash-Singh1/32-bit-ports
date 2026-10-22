#!/usr/bin/env python3
"""Build a self-contained Fast Five converter app from compiled components."""
import argparse
from pathlib import Path
import shutil
import tempfile

from package_glmetal import install, run, DEFAULT_BUILD


def package(build, glmetal):
    app = build / 'FastFive-Converter.app'
    with tempfile.TemporaryDirectory(prefix='.fastfive-converter-', dir=build) as tmp:
        staged = Path(tmp) / app.name
        contents = staged / 'Contents'
        for name in ['MacOS', 'Resources']:
            (contents / name).mkdir(parents=True)
        run('lipo', '-create', build / 'fastfive-converter-arm64', build / 'fastfive-converter-x86_64',
            '-output', contents / 'MacOS/FastFiveConverter')
        loader = contents / 'Resources/game_loader'
        shutil.copy2(build / 'game_loader', loader)
        run('codesign', '--force', '--sign', '-', loader)
        install(glmetal, contents / 'Frameworks/GLMetal', contents / 'Resources/GLMetal-build-info.json')
        shutil.copy2(Path(__file__).resolve().parents[1] / 'fastfive-converter/Info.plist', contents / 'Info.plist')
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
