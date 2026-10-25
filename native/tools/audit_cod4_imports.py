#!/usr/bin/env python3
"""Inventory i386 Mach-O call stubs without running the game or its imports.

Literal evidence is a search aid, never proof of an implemented ABI. Review
the matching handler, its return paths and native export dependencies. This
does not enumerate Objective-C selectors, dlsym calls or imported data.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


class MachO:
    def __init__(self, path):
        self.data = path.read_bytes()
        magic, = self.unpack('I', 0)
        if magic not in (0xfeedface, 0xfeedfacf):
            raise ValueError(f'{path}: expected a thin, little-endian Mach-O')
        self.wide = magic == 0xfeedfacf
        self.sections = []
        self.libraries = []
        self.symbols = []
        symtab = indirect = None
        cursor = 32 if self.wide else 28
        commands, = self.unpack('I', 16)
        for _ in range(commands):
            command, size = self.unpack('II', cursor)
            if size < 8 or cursor + size > len(self.data):
                raise ValueError('invalid load command')
            if command in (1, 0x19):
                wide = command == 0x19
                count, = self.unpack('I', cursor + (64 if wide else 48))
                for i in range(count):
                    offset = cursor + (72 if wide else 56) + i * (80 if wide else 68)
                    name = self.data[offset:offset + 16].split(b'\0')[0].decode()
                    address, length, file_offset = self.unpack('QQI' if wide else 'III', offset + 32)
                    flags, first, stride = self.unpack('III', offset + (64 if wide else 56))
                    self.sections.append((name, address, length, file_offset, flags, first, stride))
            elif command in (0xc, 0x80000018, 0x8000001f):
                start, = self.unpack('I', cursor + 8)
                self.libraries.append(self.data[cursor + start:cursor + size].split(b'\0')[0].decode())
            elif command == 2:
                symtab = self.unpack('IIII', cursor + 8)
            elif command == 0xb:
                indirect = self.unpack('II', cursor + 56)
            cursor += size
        if symtab:
            offset, count, strings, string_size = symtab
            table = self.data[strings:strings + string_size]
            for i in range(count):
                name, kind, section, desc, value = self.unpack('IBBHQ' if self.wide else 'IBBHI',
                    offset + i * (16 if self.wide else 12))
                end = table.find(b'\0', name)
                text = table[name:end].decode(errors='replace')
                self.symbols.append((text, kind, desc, value))
        self.indirect = self.unpack('I' * indirect[1], indirect[0]) if indirect else ()

    def unpack(self, fmt, offset):
        return struct.unpack_from('<' + fmt, self.data, offset)

    def exports(self):
        return {name for name, kind, _, _ in self.symbols
                if not kind & 0xe0 and kind & 1 and kind & 0xe == 0xe}

    def literals(self):
        return {value.decode(errors='replace')
                for name, _, length, offset, *_ in self.sections if name == '__cstring'
                for value in self.data[offset:offset + length].split(b'\0')}

    def imports(self):
        if self.wide:
            raise ValueError('COD4 import inventory requires a 32-bit image')
        imports = {}
        for _, address, length, _, flags, first, stride in self.sections:
            if flags & 0xff != 8:  # S_SYMBOL_STUBS
                continue
            if not stride or length % stride:
                raise ValueError('invalid stub section')
            for i in range(length // stride):
                index = self.indirect[first + i]
                if index & 0xc0000000:
                    continue
                name, kind, desc, _ = self.symbols[index]
                if kind & 0xe:  # a locally defined symbol is not an import
                    continue
                ordinal = desc >> 8
                library = self.libraries[ordinal - 1] if 0 < ordinal <= len(self.libraries) else f'ordinal {ordinal}'
                item = imports.setdefault(name, {'library': library, 'stubs': [], 'callers': []})
                item['stubs'].append(address + i * stride)
        return imports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--loader', required=True, type=Path)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'src')
    parser.add_argument('--guest-library', action='append', default=[], type=Path)
    parser.add_argument('--disassembly', type=Path, help='llvm-objdump -d output for this exact image')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    image = MachO(args.image)
    imports = image.imports()
    literals = MachO(args.loader).literals()
    exports = {str(path.name): MachO(path).exports() for path in args.guest_library}
    sources = {}
    for path in args.source.rglob('*'):
        if path.suffix not in ('.c', '.m', '.cpp', '.h'):
            continue
        for literal in set(re.findall(r'"([^"\\\n]*)"', path.read_text())):
            sources.setdefault(literal, set()).add(str(path.relative_to(args.source)))
    for name, item in imports.items():
        # compat_runtime.c explicitly normalizes the UNIX2003 suffix.
        normalized = name.removesuffix('$UNIX2003')
        variants = {normalized, normalized.removeprefix('_')}
        item['normalized_name'] = normalized
        item['guest_export_evidence'] = sorted(path for path, names in exports.items() if name in names)
        item['loader_literal_evidence'] = sorted(variants & literals)
        item['source_literal_evidence'] = sorted(set().union(*(sources.get(variant, set()) for variant in variants)))
        item['classification'] = ('guest-export-evidence' if item['guest_export_evidence'] else
            'handler-literal-evidence' if item['loader_literal_evidence'] or item['source_literal_evidence'] else
            'needs-handler-review')
    if args.disassembly:
        targets = {stub: name for name, item in imports.items() for stub in item['stubs']}
        caller = '<unlabelled>'
        for line in args.disassembly.read_text().splitlines():
            label = re.match(r'^([0-9a-f]+) <(.+)>:', line)
            if label:
                caller = label[2]
            call = re.search(r'\b(?:calll|jmpl)\s+0x([0-9a-f]+)', line)
            if call and (name := targets.get(int(call[1], 16))):
                imports[name]['callers'].append(caller)
    for item in imports.values():
        item['callers'] = sorted(set(item['callers']))
        item['stubs'] = [f'0x{address:08x}' for address in item['stubs']]
    report = {'image_sha256': hashlib.sha256(image.data).hexdigest(),
        'loader_sha256': hashlib.sha256(args.loader.read_bytes()).hexdigest(),
        'limits': __doc__, 'counts': {kind: sum(item['classification'] == kind for item in imports.values())
            for kind in ('guest-export-evidence', 'handler-literal-evidence', 'needs-handler-review')},
        'imports': dict(sorted(imports.items()))}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['counts']))


if __name__ == '__main__':
    main()
