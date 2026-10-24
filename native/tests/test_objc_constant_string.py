#!/usr/bin/env python3
"""Run compiler-generated CF and fragile-ABI string literals through the loader."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def sections(data):
    cursor = 28
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, cursor)
        if command == 1:
            count = struct.unpack_from('<I', data, cursor + 48)[0]
            for index in range(count):
                offset = cursor + 56 + index * 68
                name = data[offset:offset + 16].split(b'\0')[0]
                segment = data[offset + 16:offset + 32].split(b'\0')[0]
                address, length, file_offset = struct.unpack_from('<III', data, offset + 32)
                yield segment, name, address, length, file_offset, offset
        cursor += size


def check(loader, path, failure=False):
    env = dict(os.environ, LP32_GAME='cod4', LP32_NO_DIAGNOSTIC_LOG='1',
               LP32_BACKGROUND_TEST='1', LP32_MUTE_AUDIO='1', LP32_HITCH_LOG='0')
    result = subprocess.run(['arch', '-x86_64', str(loader), str(path)],
                            env=env, capture_output=True, text=True, timeout=20)
    output = result.stdout + result.stderr
    if failure:
        assert result.returncode == 1 and 'Objective-C lookup failed' in output, output
        assert 'trapped import' in output and 'receiver words' in output, output
    else:
        assert result.returncode == 0 and 'game main returned/escaped: 0x00000000 trapped=0' in output, output
    return output


def main():
    loader = Path(os.environ.get('LP32_TEST_LOADER', NATIVE / 'build/game_loader'))
    with tempfile.TemporaryDirectory(prefix='lp32-objc-literals-') as temporary:
        root = Path(temporary)
        (root / 'libSystem.tbd').write_text('''--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _objc_msgSend, __NSConstantStringClassReference,
      ___CFConstantStringClassReference, _CFStringGetLength, _strcmp, dyld_stub_binder ]
...
''')
        helper = root / 'helper.S'
        helper.write_text('.text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n')
        # The COD4 startup path loads a C++ library even for a C/ObjC-only
        # image. Supply an original fixture with the two class data symbols.
        runtime = root / 'compat-runtime'
        runtime.mkdir()
        source = root / 'runtime.c'
        source.write_text('void *_NSConstantStringClassReference;\n'
                          'void *__CFConstantStringClassReference;\n')
        subprocess.run(['xcrun', 'clang', '-target', 'i386-apple-macos10.8',
                        '-nostdlib', '-dynamiclib', '-L' + str(root), '-lSystem',
                        str(helper), str(source),
                        '-Wl,-install_name,@loader_path/libstdc++.6.dylib',
                        '-o', str(runtime / 'libstdc++.6.dylib')], check=True)
        fixture = NATIVE / 'tests/fixtures/objc_constant_string.m'
        for kind in ('cf', 'legacy', 'legacy-high', 'misaligned'):
            image = root / 'COD4.image'
            flags = [] if kind == 'cf' else ['-fno-constant-cfstrings', '-DLEGACY_LITERAL']
            if kind == 'legacy-high':
                flags += ['-Wl,-segaddr,__OBJC,0x0c160000']
            if kind == 'misaligned':
                flags += ['-DMISALIGNED_LITERAL']
            subprocess.run(['xcrun', 'clang', '-target', 'i386-apple-macos10.8',
                            '-nostdlib', '-fno-stack-protector', '-fno-builtin',
                            '-L' + str(root), '-lSystem', '-Wl,-e,_main,-no_pie',
                            *flags, str(helper), str(fixture), '-o', str(image)], check=True)
            output = check(loader, image, failure=kind == 'misaligned')
            if kind == 'misaligned':
                assert '__OBJC,__cstring_object' in output, output
            print(f'Objective-C constant string: PASS ({kind})')
            if kind != 'legacy-high':
                continue
            original = image.read_bytes()
            record = next(section for section in sections(original)
                          if section[:2] == (b'__OBJC', b'__cstring_object'))
            for label, field, value in (('invalid bytes', 4, 0xdeadbeef),
                                        ('oversized length', 8, 0xffffffff)):
                damaged = bytearray(original)
                struct.pack_into('<I', damaged, record[4] + field, value)
                image.write_bytes(damaged)
                check(loader, image, failure=True)
                print(f'Objective-C constant string: PASS ({label} rejected)')
            for label, field, value in (('partial record', 36, record[3] - 1),
                                        ('section outside segment', 32, 0xfffffff0)):
                damaged = bytearray(original)
                struct.pack_into('<I', damaged, record[5] + field, value)
                image.write_bytes(damaged)
                result = subprocess.run(['arch', '-x86_64', str(loader), str(image)],
                                        env=dict(os.environ, LP32_NO_DIAGNOSTIC_LOG='1'),
                                        capture_output=True, text=True, timeout=20)
                assert result.returncode == 1 and 'invalid legacy Objective-C' in result.stderr, result
                print(f'Objective-C constant string: PASS ({label} rejected)')


if __name__ == '__main__':
    main()
