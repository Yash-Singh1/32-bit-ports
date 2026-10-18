#!/usr/bin/env python3
"""GLMetal artifact packaging and game conversion without creating a GL context."""
import hashlib
import json
import plistlib
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bundle_cod4 as cod4
import bundle_mw2 as mw2
import package_glmetal as metal
import prepare_guest_runtime as runtime_tools

NAMES = ('libGLMetal.dylib', 'glmetal-compiler')
REAL_BUILD = Path(__file__).resolve().parents[1] / 'glmetal/build'


def image(cpu=7):
    return bytes.fromhex('cefaedfe' if cpu == 7 else 'cffaedfe') + cpu.to_bytes(4, 'little') + bytes(24)


def copy_tree(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True)
    else:
        shutil.copy2(source, destination)


def snapshot(root):
    return {str(path.relative_to(root)): path.read_bytes() for path in root.rglob('*') if path.is_file()}


class GLMetalPackagingTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='glmetal-package-test-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        for name in NAMES:
            path = self.build / name
            path.write_bytes(('fixture ' + name).encode())
            path.chmod(0o755)

    def test_configure_merges_environment_and_removes_source_path(self):
        info = {'LSEnvironment': {'UNCHANGED': 'value', 'LP32_GL_BACKEND': 'legacy',
                                 'LP32_GLMETAL_PATH': '/developer/build/libGLMetal.dylib'}}
        metal.configure(info)
        self.assertTrue(info['LP32GLMetal'])
        self.assertEqual(info['LSEnvironment'], {'UNCHANGED': 'value', 'LP32_GL_BACKEND': 'metal'})
        metal.configure(info)
        self.assertEqual(info['LSEnvironment']['UNCHANGED'], 'value')
        empty = {}
        metal.configure(empty)
        self.assertEqual(empty['LSEnvironment'], {'LP32_GL_BACKEND': 'metal'})

    def test_install_records_both_source_and_signed_hashes(self):
        original = snapshot(self.build)
        target = self.root / 'app/Contents/Frameworks/GLMetal'
        def sign(*args):
            if args[0] == 'codesign' and '--sign' in args:
                path = Path(args[-1])
                path.write_bytes(path.read_bytes() + b' signed fixture')
        with patch.object(metal, 'validate'), patch.object(metal, 'run', side_effect=sign):
            metal.install(self.build, target)
        self.assertEqual(snapshot(self.build), original)
        manifest = json.loads((target / 'build-info.json').read_text())
        for name in NAMES:
            self.assertEqual((target / name).read_bytes(), original[name] + b' signed fixture')
            self.assertEqual(manifest['files'][name]['source_sha256'], hashlib.sha256(original[name]).hexdigest())
            self.assertEqual(manifest['files'][name]['bundled_sha256'], hashlib.sha256((target / name).read_bytes()).hexdigest())
            self.assertTrue((target / name).stat().st_mode & 0o111)

    def test_missing_and_nonexecutable_artifacts_are_rejected(self):
        for name in NAMES:
            with self.subTest(name=name):
                path = self.build / name
                contents = path.read_bytes()
                path.unlink()
                with patch.object(metal, 'run'), self.assertRaises(ValueError):
                    metal.validate(self.build)
                path.write_bytes(contents)
                path.chmod(0o644)
                with patch.object(metal, 'run'), self.assertRaises(ValueError):
                    metal.validate(self.build)
                path.chmod(0o755)

    @unittest.skipUnless(all((REAL_BUILD / name).is_file() for name in NAMES), 'real GLMetal build unavailable')
    def test_real_build_can_be_validated_and_packaged_without_gpu(self):
        before = {name: hashlib.sha256((REAL_BUILD / name).read_bytes()).hexdigest() for name in NAMES}
        metal.validate(REAL_BUILD)
        destination = self.root / 'real-package'
        metal.install(REAL_BUILD, destination)
        manifest = json.loads((destination / 'build-info.json').read_text())
        for name in NAMES:
            self.assertEqual(manifest['files'][name]['source_sha256'], before[name])
            self.assertEqual(hashlib.sha256((REAL_BUILD / name).read_bytes()).hexdigest(), before[name])
            self.assertTrue((destination / name).is_file())


class GameGLMetalConversionTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='game-metal-test-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.loader = self.root / 'loader'
        self.loader.write_bytes(b'fixture loader')
        self.glmetal = self.root / 'glmetal'
        self.glmetal.mkdir()
        for name in NAMES:
            (self.glmetal / name).write_bytes(('fixture ' + name).encode())
            (self.glmetal / name).chmod(0o755)
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        self.runtime_entries = {}
        for name in runtime_tools.LIBRARIES:
            path = self.runtime / name
            path.write_bytes(b'fixture runtime')
            self.runtime_entries[name] = ('fixture', hashlib.sha256(path.read_bytes()).hexdigest())

    def fixture(self, game, mode):
        install = self.root / (game + '-' + mode)
        if game in cod4.GAMES:
            title = cod4.GAMES[game]['title']
            source = install / f'{title}.app'
            selected = source if mode == 'sp' else source / f'Contents/{title} Multiplayer.app'
            name = cod4.executables(game)[mode]
            data = source / f'Contents/{title} Data'
            libraries = ()
        else:
            name = mw2.GAMES[game][2][mode][0]
            source = selected = install / (name + '.app')
            data = install / 'GameData'
            libraries = mw2.libraries(game, mode)
        macos = selected / 'Contents/MacOS'
        macos.mkdir(parents=True)
        (selected / 'Contents/Resources').mkdir()
        (data / 'main').mkdir(parents=True)
        (data / 'main/iw_00.iwd').write_bytes(b'game assets')
        (data / 'players').mkdir()
        (data / 'players/save').write_bytes(b'player save')
        (macos / (name if game in cod4.GAMES else name + 'sub')).write_bytes(image())
        for library in libraries:
            (macos / library).write_bytes(image())
        (macos / 'libsteam_api.dylib').write_bytes(image(0x01000007))
        info = {'CFBundleExecutable': name, 'LSEnvironment': {'KEEP_ME': 'yes',
                'LP32_GLMETAL_PATH': '/old/build/libGLMetal.dylib', 'LP32_GL_BACKEND': 'old'}}
        (selected / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        return source, install

    def convert(self, game, mode, source, output):
        if game in cod4.GAMES:
            cod4.build(source, mode, self.loader, output, False, self.runtime, glmetal=self.glmetal, game=game)
        else:
            mw2.build(source, mode, self.loader, output, game=game, glmetal=self.glmetal)

    def test_all_games_and_modes_package_metal_and_preserve_sources(self):
        for game in ('cod4', 'mw2', 'mw3'):
            for mode in ('sp', 'mp'):
                with self.subTest(game=game, mode=mode):
                    source, install = self.fixture(game, mode)
                    before = snapshot(install)
                    output = self.root / (game + '-' + mode + '-Compat.app')
                    with patch.object(metal, 'validate'), patch.object(metal, 'run'), \
                         patch.object(cod4, 'ditto', side_effect=copy_tree), patch.object(cod4, 'run'), \
                         patch.object(mw2, 'ditto', side_effect=copy_tree), patch.object(mw2, 'run'), \
                         patch.object(runtime_tools, 'LIBRARIES', self.runtime_entries):
                        self.convert(game, mode, source, output)
                    self.assertEqual(snapshot(install), before)
                    info = plistlib.loads((output / 'Contents/Info.plist').read_bytes())
                    self.assertEqual(info['LP32GeneratedGame'], game)
                    self.assertTrue(info['LP32GLMetal'])
                    self.assertEqual(info['LSEnvironment'], {'KEEP_ME': 'yes', 'LP32_GL_BACKEND': 'metal'})
                    package = output / 'Contents/Frameworks/GLMetal'
                    for name in NAMES:
                        self.assertEqual((package / name).read_bytes(), (self.glmetal / name).read_bytes())
                    self.assertFalse((package / 'build-info.json').exists())
                    self.assertTrue((output / 'Contents/Resources/GLMetal-build-info.json').is_file())
                    data = output / ('Contents/Call of Duty 4 Data' if game == 'cod4' else 'Contents/GameData')
                    self.assertEqual((data / 'players/save').read_bytes(), b'player save')

    def test_cod2_keeps_apple_opengl_and_preserves_source(self):
        # COD2 renders into GL_AUX0/GL_AUX1, which GLMetal does not provide.
        for mode in ('sp', 'mp'):
            with self.subTest(mode=mode):
                source, install = self.fixture('cod2', mode)
                before = snapshot(install)
                output = self.root / f'cod2-{mode}-Compat.app'
                with patch.object(metal, 'validate', side_effect=AssertionError('GLMetal used')), \
                     patch.object(metal, 'install', side_effect=AssertionError('GLMetal used')), \
                     patch.object(cod4, 'ditto', side_effect=copy_tree), patch.object(cod4, 'run'), \
                     patch.object(runtime_tools, 'LIBRARIES', self.runtime_entries):
                    self.convert('cod2', mode, source, output)
                self.assertEqual(snapshot(install), before)
                stem = 'COD2MP' if mode == 'mp' else 'COD2'
                info = plistlib.loads((output / 'Contents/Info.plist').read_bytes())
                self.assertEqual(info['LP32GeneratedGame'], 'cod2')
                self.assertEqual(info['CFBundleExecutable'], f'{stem}Compat')
                self.assertNotIn('LP32GLMetal', info)
                self.assertEqual(info['LSEnvironment'].get('LP32_GL_BACKEND'), 'old')
                self.assertFalse((output / 'Contents/Frameworks/GLMetal').exists())
                self.assertTrue((output / f'Contents/SharedSupport/{stem}.image').is_file())
                data = output / 'Contents/Call of Duty 2 Data'
                self.assertEqual((data / 'players/save').read_bytes(), b'player save')

    def test_failed_signing_preserves_existing_cod4_bundle(self):
        source, install = self.fixture('cod4', 'sp')
        before = snapshot(install)
        output = self.root / 'existing-COD4.app'
        (output / 'Contents').mkdir(parents=True)
        (output / 'Contents/Info.plist').write_bytes(plistlib.dumps({'LP32GeneratedGame': 'cod4'}))
        (output / 'sentinel-save').write_bytes(b'keep existing output')
        old_output = snapshot(output)
        with patch.object(metal, 'validate'), \
             patch.object(metal, 'run', side_effect=RuntimeError('signing failed')), \
             patch.object(cod4, 'ditto', side_effect=copy_tree), patch.object(cod4, 'run'), \
             patch.object(runtime_tools, 'LIBRARIES', self.runtime_entries):
            with self.assertRaisesRegex(RuntimeError, 'signing failed'):
                self.convert('cod4', 'sp', source, output)
        self.assertEqual(snapshot(output), old_output)
        self.assertEqual(snapshot(install), before)
        self.assertFalse(list(self.root.glob('.cod4-stage-*')))

    def test_missing_glmetal_never_publishes_or_modifies_source(self):
        for game in ('cod4', 'mw2', 'mw3'):
            for mode in ('sp', 'mp'):
                with self.subTest(game=game, mode=mode):
                    source, install = self.fixture(game, mode)
                    before = snapshot(install)
                    output = self.root / (game + '-' + mode + '-Compat.app')
                    with patch.object(runtime_tools, 'LIBRARIES', self.runtime_entries), \
                         patch.object(metal, 'validate', side_effect=ValueError('Missing GLMetal artifact')):
                        with self.assertRaisesRegex(ValueError, 'Missing GLMetal'):
                            self.convert(game, mode, source, output)
                    self.assertFalse(output.exists())
                    self.assertEqual(snapshot(install), before)
                    self.assertFalse(list(self.root.glob('.' + game + '-stage-*')))


if __name__ == '__main__':
    unittest.main()
