#!/usr/bin/env python3
"""Run AppKit standard alerts through the guest Carbon dispatcher.

The self-test sends native button commands; it never fakes a modal result.
No game files or old Apple runtime libraries are needed.
"""
import os
from pathlib import Path
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def main():
    loader = Path(os.environ.get('LP32_TEST_LOADER', NATIVE / 'build/game_loader'))
    with tempfile.TemporaryDirectory(prefix='lp32-standard-alert-') as temporary:
        root = Path(temporary)
        (root / 'libSystem.tbd').write_text('''--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ dyld_stub_binder ]
...
''')
        helper = root / 'helper.S'
        helper.write_text('.text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n')
        source = root / 'fixture.c'; source.write_text('int main(void) { return 0; }\n')
        runtime = root / 'compat-runtime'; runtime.mkdir()
        flags = ['xcrun', 'clang', '-target', 'i386-apple-macos10.8', '-nostdlib',
                 '-fno-stack-protector', '-L' + str(root), '-lSystem', str(helper)]
        subprocess.run([*flags, '-dynamiclib', str(source),
            '-Wl,-install_name,@loader_path/libstdc++.6.dylib',
            '-o', str(runtime / 'libstdc++.6.dylib')], check=True)
        image = root / 'COD4.image'
        subprocess.run([*flags, '-Wl,-e,_main,-no_pie', str(source), '-o', str(image)], check=True)
        result = subprocess.run(['arch', '-x86_64', str(loader), str(image)],
            env=dict(os.environ, LP32_GAME='cod4', LP32_CARBON_ALERT_SELFTEST='1',
                LP32_NO_DIAGNOSTIC_LOG='1', LP32_HITCH_LOG='0', LP32_BACKGROUND_TEST='1',
                LP32_MUTE_AUDIO='1'), capture_output=True, text=True, timeout=20)
        output = result.stdout + result.stderr
        assert result.returncode == 0 and 'Carbon standard alert PASS' in output, output
        print('Carbon standard alert: PASS (AppKit OK/Cancel/Other, 16-bit result, token retirement, dynamic import)')


if __name__ == '__main__':
    main()
