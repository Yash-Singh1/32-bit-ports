#!/usr/bin/env python3
"""Exercise the shipping importer using generated textures, never game code."""
import hashlib
import io
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


def pack(files):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            entry = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    result = bytearray(out.getvalue())
    result[:4] = b'QM\x06\x08'
    return bytes(result)


def unpack(blob):
    with zipfile.ZipFile(io.BytesIO(blob)) as archive:
        files = {}
        for entry in archive.infolist():
            pos = entry.header_offset
            assert blob[pos:pos + 4] == (b'QM\x06\x08' if pos == 0 else b'PK\x03\x04')
            n, extra = struct.unpack_from('<HH', blob, pos + 26)
            pos += 30 + n + extra
            data = blob[pos:pos + entry.compress_size]
            if entry.compress_type == 8:
                data = zlib.decompress(data, -15)
            assert len(data) == entry.file_size and zlib.crc32(data) == entry.CRC
            files[entry.filename] = data
        return files


def dds(width=8, height=8, levels=4):
    header = bytearray(128)
    header[:4] = b'DDS '
    for pos, value in {4: 124, 8: 0xa1007, 12: height, 16: width, 28: levels, 76: 32, 80: 4, 108: 0x401008}.items():
        struct.pack_into('<I', header, pos, value)
    header[84:88] = b'DXT3'
    payload = bytearray()
    for _ in range(levels):
        payload += b'\xff' * (((width + 3) // 4) * ((height + 3) // 4) * 16)
        width, height = max(1, width // 2), max(1, height // 2)
    return bytes(header + payload)


def pvr(levels=5, transparent=False):
    # Constant opaque red or transparent black PVRTC1 4bpp words.
    payload = bytearray()
    width = 16
    for _ in range(levels):
        word = struct.pack('<II', 0, 0 if transparent else 0xfc00fc00)
        payload += word * (max(width, 8) ** 2 // 16)
        width = max(1, width // 2)
    return struct.pack('<13I', 52, 16, 16, levels - 1, 25, len(payload), 4,
                       0, 0, 0, 1 if transparent else 0, 0x21525650, 1) + payload


def run_tests(helper):
    prefix = 'Data/#temp/TrackTga/'
    mac = {prefix + 'Road.PVRTC2.tga': dds(), prefix + 'Road.PVRTC4.tga': dds(),
           prefix + 'Road.PVRTC2.NOMIPMAP.tga': dds(levels=1),
           prefix + 'Alpha.PVRTC4.tga': dds(), prefix + 'Untouched.PVRTC4.tga': dds(),
           prefix + 'Same.tga': b'unchanged bytes'}
    ipad = {prefix + 'Road_x2.PVRTC4.tga': pvr(),
            prefix + 'Road_x2.PVRTC4.NOMIPMAP.tga': pvr(levels=1),
            prefix + 'Alpha_x2.PVRTC4.tga': pvr(transparent=True),
            prefix + 'Untouched_IPAD2.PVRTC4.tga': pvr(),
            prefix + 'Same.tga': b'unchanged bytes'}
    with tempfile.TemporaryDirectory(prefix='asphalt6-ipad-test-') as tmp:
        root = Path(tmp)
        ipa, source, output, report = [root / n for n in ['game.ipa', 'mac.dat', 'new.dat', 'report.json']]
        source.write_bytes(pack(mac))
        def write_ipa(files=ipad, bundle='com.gameloft.Asphalt6ipad'):
            with zipfile.ZipFile(ipa, 'w', zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('Payload/Game.app/Info.plist', plistlib.dumps({
                    'CFBundleIdentifier': bundle, 'CFBundleVersion': '1.5.1'}))
                archive.writestr('Payload/Game.app/data/file000019.dat', pack(files))
                archive.writestr('Payload/Game.app/Game', b'ARM game code must never be copied')
        write_ipa()
        args = [str(helper), '--import', str(ipa), str(source), str(output), str(report)]
        subprocess.run(args, check=True, capture_output=True)
        result = unpack(output.read_bytes())
        assert result.keys() == mac.keys()
        assert result[prefix + 'Untouched.PVRTC4.tga'] == mac[prefix + 'Untouched.PVRTC4.tga']
        assert result[prefix + 'Same.tga'] == mac[prefix + 'Same.tga']
        red = result[prefix + 'Road.PVRTC2.tga']
        assert red == result[prefix + 'Road.PVRTC4.tga']
        assert struct.unpack_from('<II', red, 12) == (16, 16)
        assert struct.unpack_from('<I', red, 28)[0] == 5
        assert len(red) == 128 + (16 + 4 + 1 + 1 + 1) * 16
        for at in range(128, len(red), 16):
            assert red[at:at + 8] == b'\xff' * 8
            # Both DXT color endpoints are exact red in RGB565.
            assert struct.unpack_from('<HH', red, at + 8) == (0xf800, 0xf800)
        assert struct.unpack_from('<I', result[prefix + 'Road.PVRTC2.NOMIPMAP.tga'], 28)[0] == 1
        alpha = result[prefix + 'Alpha.PVRTC4.tga']
        for at in range(128, len(alpha), 16):
            assert alpha[at:at + 8] == b'\0' * 8
        stats = json.loads(report.read_text())
        assert stats['upgradedTextures'] == 4 and stats['byteIdenticalTextures'] == 1
        for item in stats['macTextures']:
            assert item['sha256'] == hashlib.sha256(mac[item['name']]).hexdigest()
        # Output paths cannot overwrite existing files, even on an invalid IPA.
        old_output = output.read_bytes()
        assert subprocess.run(args, capture_output=True).returncode != 0
        assert output.read_bytes() == old_output
        output.unlink(); report.unlink()
        write_ipa(bundle='com.example.other-game')
        assert subprocess.run(args, capture_output=True).returncode != 0 and not output.exists()
        broken = dict(ipad)
        broken[prefix + 'Road_x2.PVRTC4.tga'] = pvr()[:80]
        write_ipa(broken)
        assert subprocess.run(args, capture_output=True).returncode != 0 and not report.exists() and not output.exists()
        output.unlink(missing_ok=True)
        # Comparison reads and hashes assets without creating a replacement pack.
        write_ipa()
        subprocess.run([str(helper), '--compare', str(ipa), str(source), str(report)], check=True, capture_output=True)
        assert not output.exists() and json.loads(report.read_text())['upgradedTextures'] == 4
        assert source.read_bytes() == pack(mac)
    print('PASS: IPA identity, PVRTC/DXT3 pixels, alpha, mipmaps, archive preservation, hashes and rejection paths')


if __name__ == '__main__':
    run_tests(Path(sys.argv[1]).resolve())
