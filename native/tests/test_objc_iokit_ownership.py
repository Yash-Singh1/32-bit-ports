#!/usr/bin/env python3
"""Exercise IOKit Create results across Objective-C autorelease boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def main():
    loader = Path(os.environ.get('LP32_TEST_LOADER', NATIVE / 'build/game_loader'))
    with tempfile.TemporaryDirectory(prefix='lp32-iokit-ownership-') as temporary:
        root = Path(temporary)
        (root / 'libSystem.tbd').write_text('''--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _objc_msgSend, ___CFConstantStringClassReference, dyld_stub_binder,
      _IORegistryEntryFromPath, _IORegistryEntryCreateCFProperty,
      _IORegistryEntryCreateCFProperties, _IORegistryEntrySearchCFProperty,
      _IOObjectRelease, _CFGetTypeID, '.objc_class_name_NSAutoreleasePool' ]
...
''')
        helper = root / 'helper.S'
        helper.write_text('.text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n')
        runtime = root / 'compat-runtime'
        runtime.mkdir()
        source = root / 'runtime.c'
        source.write_text('void *__CFConstantStringClassReference;\n')
        flags = ['xcrun', 'clang', '-target', 'i386-apple-macos10.8', '-nostdlib',
                 '-fno-stack-protector', '-L' + str(root), '-lSystem', str(helper)]
        subprocess.run([*flags, '-dynamiclib', str(source),
            '-Wl,-install_name,@loader_path/libstdc++.6.dylib',
            '-o', str(runtime / 'libstdc++.6.dylib')], check=True)
        for api in ('properties', 'property', 'search'):
            for explicit in (False, True):
                defines = [] if api == 'properties' else ['-DTEST_' + api.upper()]
                if explicit:
                    defines.append('-DEXPLICIT_POOL')
                image = root / 'COD4.image'
                subprocess.run([*flags, *defines, '-Wl,-e,_main,-no_pie',
                    str(NATIVE / 'tests/fixtures/objc_iokit_ownership.m'), '-o', str(image)], check=True)
                result = subprocess.run(['arch', '-x86_64', str(loader), str(image)],
                    env=dict(os.environ, LP32_GAME='cod4', LP32_NO_DIAGNOSTIC_LOG='1',
                        LP32_HITCH_LOG='0', LP32_BACKGROUND_TEST='1', LP32_MUTE_AUDIO='1'),
                    capture_output=True, text=True, timeout=20)
                output = result.stdout + result.stderr
                assert result.returncode == 0 and 'game main returned/escaped: 0x00000000 trapped=0' in output, output
                print(f'IOKit Objective-C ownership: PASS ({api}, pool={"explicit" if explicit else "fallback"})')


if __name__ == '__main__':
    main()
