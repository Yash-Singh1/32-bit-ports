#!/usr/bin/env python3
"""Optional regression using the issue #13 image in a converted app's runtime.

No game files are included. Stops before startAsynchronous, so no App Store
request is sent. The image hash guards the diagnostic function/checkpoint
addresses; these addresses are never applied as runtime compatibility patches.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess

NATIVE = Path(__file__).resolve().parents[1]
IMAGE_HASH = '5e943a76063d7e1e26bad289a879434dcebd5df2ac1607d58dc15113c42c71d4'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--loader', type=Path, default=NATIVE / 'build/game_loader')
    args = parser.parse_args()
    assert hashlib.sha256(args.image.read_bytes()).hexdigest() == IMAGE_HASH, 'Different image; diagnostic addresses do not apply'
    env = dict(os.environ, LP32_GAME='cod4', LP32_NO_DIAGNOSTIC_LOG='1',
        LP32_HITCH_LOG='0', LP32_BACKGROUND_TEST='1', LP32_MUTE_AUDIO='1',
        LP32_OBJC_FACTORY_TEST_CLASS='bfe6a9e3d32d1f9be4311343e9fe')
    cases = [
        ('factory and IOKit, fallback pool', dict(LP32_OBJC_FACTORY_TEST_DATA_FUNCTION='0x339250')),
        ('factory and IOKit, explicit pool', dict(LP32_OBJC_FACTORY_TEST_DATA_FUNCTION='0x339250',
                                                 LP32_OBJC_FACTORY_TEST_POOL='1')),
        ('original request encryption', dict(LP32_OBJC_FACTORY_TEST_FUNCTION='0x339340',
            LP32_TRACE_GUEST_ADDRESS='0x33964c', LP32_TEST_EXIT_AT_GUEST_BREAKPOINT='1')),
    ]
    for label, settings in cases:
        result = subprocess.run(['arch', '-x86_64', str(args.loader), str(args.image)],
            env=env | settings, capture_output=True, text=True, timeout=20)
        output = result.stdout + result.stderr
        assert result.returncode == 0 and 'trapped import' not in output, output
        if label == 'original request encryption':
            assert 'test reached guest breakpoint 0x0033964c' in output, output
        else:
            assert 'Guest IOKit data self-test: PASS' in output, output
            assert 'Guest request factory self-test: PASS' in output, output
        print(f'Issue #13 actual image: PASS ({label}; no network request started)')


if __name__ == '__main__':
    main()
