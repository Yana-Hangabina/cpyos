#!/usr/bin/env python3
"""The Plus web installer manifest must follow the release layout, not literals."""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_cpyos_site as site  # noqa: E402


def entry(name: str, kind: int, subtype: int, offset: int, size: int) -> bytes:
    row = bytearray(32)
    struct.pack_into('<H', row, 0, 0x50AA)
    row[2], row[3] = kind, subtype
    struct.pack_into('<II', row, 4, offset, size)
    row[12:12 + len(name)] = name.encode()
    struct.pack_into('<I', row, 28, 1)
    return bytes(row)


class PlusManifestTest(unittest.TestCase):
    def test_parts_follow_partition_table_and_data_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            release = Path(tmp) / 'release'
            out = Path(tmp) / 'out'
            release.mkdir()
            out.mkdir()
            table = b''.join([
                entry('nvs', 1, 2, 0x9000, 0x5000),
                entry('otadata', 1, 0, 0xE000, 0x2000),
                entry('app0', 0, 0x10, 0x10000, 0x660000),
                entry('app1', 0, 0x11, 0x670000, 0x660000),
                entry('spiffs', 1, 0x82, 0xCD0000, 0x320000),
            ])
            (release / 'cpyos-plus-9.9-e4p-partition-table.bin').write_bytes(table)
            (release / 'cpyos-plus-9.9-e4p.bin').write_bytes(b'firmware')
            rows = [
                ('pinyin', 0xCD0000), ('web', 0xD26000), ('hyphen', 0xD60000),
                ('i18n', 0xDB6000), ('emoji', 0xE0D000), ('core', 0xE2C000),
            ]
            manifest = ['partition\tspiffs\t0x00cd0000\t0x00320000']
            for name, address in rows:
                path = release / f'{name}.dat'
                path.write_bytes(name.encode())
                manifest.append(f'data\t{name}\t0\t0x{address:08x}\t{len(name)}')
                manifest[-1] += f'\t0\tbuild/{path.name}'
            (release / 'data_manifest_plus.txt').write_text('\n'.join(manifest) + '\n', encoding='utf-8')
            site.write_plus_flash_manifest(release, out, '9.9')
            data = json.loads((out / 'flash-plus.json').read_text(encoding='utf-8'))
            offsets = [(part['offset'], part['path']) for part in data['parts']]
            self.assertEqual(offsets[0], (0x8000, 'cpyos-plus-9.9-e4p-partition-table.bin'))
            self.assertEqual(offsets[1], (0x10000, 'cpyos-plus-9.9-e4p.bin'))
            self.assertEqual(offsets[2], (0x670000, 'cpyos-plus-9.9-e4p.bin'))
            self.assertEqual(offsets[-1], (0xE2C000, 'core.dat'))
            self.assertEqual(len(data['parts']), 9)


if __name__ == '__main__':
    unittest.main()
