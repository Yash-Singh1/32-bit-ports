#!/usr/bin/env python3
"""Exercise actual guest methods and deallocation across bridge pool boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def main():
    loader = Path(os.environ.get('LP32_TEST_LOADER', NATIVE / 'build/game_loader'))
    with tempfile.TemporaryDirectory(prefix='lp32-objc-lifetime-') as temporary:
        root = Path(temporary)
        (root / 'libSystem.tbd').write_text('''--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _objc_msgSend, _objc_msgSendSuper, _memcpy, dyld_stub_binder,
      '.objc_class_name_NSObject', '.objc_class_name_NSAutoreleasePool',
      '.objc_class_name_LP32NativeLifetimeFactory' ]
...
''')
        helper = root / 'helper.S'
        helper.write_text('.text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n')
        runtime = root / 'compat-runtime'
        runtime.mkdir()
        source = root / 'runtime.c'
        source.write_text('int fixture_runtime;\n')
        flags = ['xcrun', 'clang', '-target', 'i386-apple-macos10.8',
                 '-nostdlib', '-fno-stack-protector', '-fno-builtin', '-L' + str(root), '-lSystem', str(helper)]
        subprocess.run([*flags, '-dynamiclib', str(source),
                        '-Wl,-install_name,@loader_path/libstdc++.6.dylib',
                        '-o', str(runtime / 'libstdc++.6.dylib')], check=True)
        variants = [(['-DSUPER_AUTORELEASE'] if use_super else []) +
                    (['-DEXPLICIT_POOL'] if explicit_pool else [])
                    for use_super in (False, True) for explicit_pool in (False, True)]
        variants.append(['-DSTALE_OBJECT_DIAGNOSTIC'])
        variants.append(['-DNATIVE_CRASH_DIAGNOSTIC'])
        native_factory = root / 'native_factory.dylib'
        subprocess.run(['xcrun', 'clang', '-arch', 'x86_64', '-dynamiclib',
                        str(NATIVE / 'tests/fixtures/objc_native_factory.m'),
                        '-framework', 'Foundation', '-o', str(native_factory)], check=True)
        variants.extend([['-DNATIVE_FACTORY'], ['-DNATIVE_FACTORY', '-DEXPLICIT_POOL']])
        variants.append(['-DNATIVE_FACTORY_OWNED'])
        for defines in variants:
            image = root / 'COD4.image'
            subprocess.run([*flags, *defines, '-Wl,-e,_main,-no_pie',
                            str(NATIVE / 'tests/fixtures/objc_lifetime.m'),
                            '-o', str(image)], check=True)
            env = dict(os.environ, LP32_GAME='cod4', LP32_NO_DIAGNOSTIC_LOG='1',
                       LP32_HITCH_LOG='0', LP32_BACKGROUND_TEST='1', LP32_MUTE_AUDIO='1')
            if '-DNATIVE_CRASH_DIAGNOSTIC' in defines:
                env['LP32_OBJC_DIAGNOSTICS'] = '1'
            command = ['arch', '-x86_64', str(loader), str(image)]
            if any(option in defines for option in ('-DNATIVE_FACTORY', '-DNATIVE_FACTORY_OWNED')):
                env['DYLD_INSERT_LIBRARIES'] = str(native_factory)
                # Launch the thin x86_64 binary directly. Apple's protected
                # arch executable would strip the injected fixture library.
                command = [str(loader), str(image)]
            result = subprocess.run(command,
                                    env=env, capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
            if '-DNATIVE_CRASH_DIAGNOSTIC' in defines:
                assert result.returncode == 139 and 'signal 11' in output, output
                active = next((line for line in output.splitlines() if 'objc active ' in line), '')
                assert 'text=crash' in active and 'encoding=v' in active, output
                assert 'native=0000000000000000' not in active, output
                assert 'method=00000000' not in active, output
                lifetime = [line for line in output.splitlines() if 'objc lifetime ' in line]
                history = [line for line in output.splitlines() if 'objc history ' in line]
                assert any('create ' in line and 'LP32AutoreleaseTest' in line for line in lifetime), output
                assert len(history) == 128 and len(lifetime) <= 64, output
                assert 'native image=' in output and 'native stack[0]=' in output, output
                # The same records must reach the persistent Finder log.
                home = root / 'home'
                (home / 'Library/Logs').mkdir(parents=True)
                logged_env = dict(env, HOME=str(home))
                logged_env.pop('LP32_NO_DIAGNOSTIC_LOG', None)
                if os.environ.get('LP32_TEST_DIAGNOSTICS_DEFAULT') == '1':
                    logged_env.pop('LP32_OBJC_DIAGNOSTICS', None)
                logged = subprocess.run(['arch', '-x86_64', str(loader), str(image)],
                                        env=logged_env, capture_output=True, text=True, timeout=20)
                assert logged.returncode == 139, logged.stderr
                persistent = (home / 'Library/Logs/COD4Compat/last-run.log').read_text()
                assert 'objc active ' in persistent and 'text=crash' in persistent, persistent
                assert 'native image=' in persistent, persistent
                disabled = subprocess.run(['arch', '-x86_64', str(loader), str(image)],
                    env=dict(env, LP32_OBJC_DIAGNOSTICS='0'), capture_output=True, text=True, timeout=20)
                assert disabled.returncode == 139 and 'signal 11' in disabled.stderr, disabled.stderr
                assert 'compat32: objc ' not in disabled.stderr, disabled.stderr
            elif '-DSTALE_OBJECT_DIAGNOSTIC' in defines:
                assert result.returncode == 1 and 'selector=retain' in output, output
                assert 'guest class=LP32AutoreleaseTest' in output, output
                assert 'identity=absent' in output and 'previous mapping retired' in output, output
            else:
                assert result.returncode == 0 and 'game main returned/escaped: 0x00000000 trapped=0' in output, output
            print(f'Guest Objective-C lifetime: PASS ({defines})')


if __name__ == '__main__':
    main()
