#!/usr/bin/env python3
"""Verify the 0.9.7 PEM/EVP ABI using a real RSA encryption round trip."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

NATIVE = Path(__file__).resolve().parents[1]


def main():
    loader = Path(os.environ.get('LP32_TEST_LOADER', NATIVE / 'build/game_loader'))
    with tempfile.TemporaryDirectory(prefix='lp32-openssl-') as temporary:
        root = Path(temporary)
        def run(*args):
            result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            return result
        run('/usr/bin/openssl', 'genrsa', '-out', root / 'key.pem', '1024')
        run('/usr/bin/openssl', 'rsa', '-in', root / 'key.pem', '-pubout', '-out', root / 'public.pem')
        (root / 'openssl_test_key.h').write_text('static const char public_key[] = ' +
            json.dumps((root / 'public.pem').read_text()) + ';\n')
        (root / 'libSystem.tbd').write_text('''--- !tapi-tbd-v3
archs: [ i386 ]
platform: macosx
install-name: /usr/lib/libSystem.B.dylib
exports:
  - archs: [ i386 ]
    symbols: [ _OPENSSL_add_all_algorithms_noconf, _OpenSSL_add_all_digests,
      _ERR_load_crypto_strings, _ERR_load_X509_strings, _EVP_cleanup,
      _BIO_s_mem, _BIO_new, _BIO_new_mem_buf, _BIO_free,
      _PEM_read_bio_RSA_PUBKEY, _RSA_free, _EVP_PKEY_new, _EVP_PKEY_assign,
      _EVP_PKEY_encrypt, _EVP_PKEY_free, _getenv, _fopen, _fwrite, _fclose, _memset, _memcpy,
      dyld_stub_binder ]
...
''')
        helper = root / 'helper.S'
        helper.write_text('.text\n.globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n ud2\n')
        runtime = root / 'compat-runtime'; runtime.mkdir()
        source = root / 'runtime.c'; source.write_text('int runtime_fixture;\n')
        flags = ['xcrun', 'clang', '-target', 'i386-apple-macos10.8', '-nostdlib',
                 '-fno-stack-protector', '-fno-builtin', '-I' + str(root),
                 '-L' + str(root), '-lSystem', str(helper)]
        run(*flags, '-dynamiclib', source, '-Wl,-install_name,@loader_path/libstdc++.6.dylib',
            '-o', runtime / 'libstdc++.6.dylib')
        image = root / 'COD4.image'
        run(*flags, '-Wl,-e,_main,-no_pie', NATIVE / 'tests/fixtures/openssl_legacy.c', '-o', image)
        output = root / 'ciphertext'
        result = subprocess.run(['arch', '-x86_64', str(loader), str(image)],
            env=dict(os.environ, LP32_GAME='cod4', LP32_CRYPTO_OUTPUT=str(output),
                LP32_NO_DIAGNOSTIC_LOG='1', LP32_HITCH_LOG='0', LP32_BACKGROUND_TEST='1',
                LP32_MUTE_AUDIO='1'), capture_output=True, text=True, timeout=30)
        log = result.stdout + result.stderr
        assert result.returncode == 0 and 'game main returned/escaped: 0x00000000 trapped=0' in log, log
        run('/usr/bin/openssl', 'rsautl', '-decrypt', '-inkey', root / 'key.pem',
            '-in', output, '-out', root / 'plaintext')
        assert (root / 'plaintext').read_bytes() == b'COD4 legacy EVP encryption boundary'
        print('Legacy OpenSSL: PASS (64 PEM/EVP ownership cycles, real RSA round trip, oversized input and invalid PEM rejected)')


if __name__ == '__main__':
    main()
