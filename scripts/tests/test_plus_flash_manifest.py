#!/usr/bin/env python3
"""The Plus web installer manifest must follow the release layout, not literals."""
import contextlib
import hashlib
import io
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
                digest = hashlib.sha256(name.encode()).hexdigest()
                manifest.append(f'data\t{name}\t0\t0x{address:08x}\t{len(name)}\t{digest}'
                                f'\tbuild/{path.name}')
            (release / 'data_manifest_plus.txt').write_text('\n'.join(manifest) + '\n', encoding='utf-8')
            site.write_plus_flash_manifest(release, out, '9.9')
            data = json.loads((out / 'flash-plus.json').read_text(encoding='utf-8'))
            offsets = [(part['offset'], part['path']) for part in data['parts']]
            self.assertEqual(offsets[0], (0x8000, 'cpyos-plus-9.9-e4p-partition-table.bin'))
            self.assertEqual(offsets[1], (0x10000, 'cpyos-plus-9.9-e4p.bin'))
            self.assertEqual(offsets[2], (0x670000, 'cpyos-plus-9.9-e4p.bin'))
            self.assertEqual(offsets[-1], (0xE2C000, 'core.dat'))
            self.assertEqual(len(data['parts']), 9)


class PlusCoreNamingTest(unittest.TestCase):
    """The Plus installer must find the core whether it is published by alias or by carrier.

    generate_data_manifest.py now names the tiered carrier (build/cjk_core_8105.dat) in the
    manifest's file column, while the release workflow copies the stable alias
    (cjk_core.dat) into the release directory -- the name 0.8.0 shipped and the name a
    device-facing installer should keep using. Deriving the asset name from only one of the
    two makes the Pages build fail (or, worse, ship a manifest whose core is absent).
    """

    def make_release(self, release: Path, core_column: str, core_files: list[str]) -> None:
        release.mkdir()
        table = b''.join([
            entry('nvs', 1, 2, 0x9000, 0x5000),
            entry('otadata', 1, 0, 0xE000, 0x2000),
            entry('app0', 0, 0x10, 0x10000, 0x660000),
            entry('app1', 0, 0x11, 0x670000, 0x660000),
            entry('spiffs', 1, 0x82, 0xCD0000, 0x320000),
        ])
        (release / 'cpyos-plus-9.9-e4p-partition-table.bin').write_bytes(table)
        (release / 'cpyos-plus-9.9-e4p.bin').write_bytes(b'firmware')
        manifest = ['partition\tspiffs\t0x00cd0000\t0x00320000']
        rows = [('pinyin', 0xCD0000), ('web', 0xD26000), ('hyphen', 0xD60000),
                ('i18n', 0xDB6000), ('emoji', 0xE0D000)]
        for name, address in rows:
            path = release / f'{name}.dat'
            path.write_bytes(name.encode())
            digest = hashlib.sha256(name.encode()).hexdigest()
            manifest.append(f'data\t{name}\t0\t0x{address:08x}\t{len(name)}\t{digest}'
                            f'\tbuild/{path.name}')
        payload = b'core-carrier'
        for name in core_files:
            (release / name).write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        manifest.append(f'data\tcore\t0\t0x{0xE2C000:08x}\t{len(payload)}\t{digest}\t{core_column}')
        (release / 'data_manifest_plus.txt').write_text('\n'.join(manifest) + '\n', encoding='utf-8')

    def paths_of(self, out: Path) -> dict:
        data = json.loads((out / 'flash-plus.json').read_text(encoding='utf-8'))
        return {part['offset']: part['path'] for part in data['parts']}

    def test_alias_asset_satisfies_a_tiered_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            release = Path(tmp) / 'release'
            out = Path(tmp) / 'out'
            out.mkdir()
            self.make_release(release, 'build/cjk_core_8105.dat', ['cjk_core.dat'])
            site.write_plus_flash_manifest(release, out, '9.9')
            self.assertEqual(self.paths_of(out)[0xE2C000], 'cjk_core.dat')

    def test_carrier_asset_also_works(self):
        with tempfile.TemporaryDirectory() as tmp:
            release = Path(tmp) / 'release'
            out = Path(tmp) / 'out'
            out.mkdir()
            self.make_release(release, 'build/cjk_core_8105.dat', ['cjk_core_8105.dat'])
            site.write_plus_flash_manifest(release, out, '9.9')
            self.assertEqual(self.paths_of(out)[0xE2C000], 'cjk_core_8105.dat')

    def test_an_alias_holding_another_tier_is_refused(self):
        """build/cjk_core.dat is whichever layout was generated last -- it must not be trusted.

        The Plus workflow copies that alias into the release. If a Read Pico build ran in the
        same job first, the alias holds the 3,500-glyph carrier while the Plus manifest
        describes 8,105, and a device flashed from the Pages installer would silently get the
        narrow font. Two shapes are refused: the same length with different bytes (only the
        SHA-256 can tell those apart) and a different length.
        """
        for label, payload in (('same size, other bytes', b'xore-carrier'),
                               ('other size', b'narrow-3500')):
            with self.subTest(label), tempfile.TemporaryDirectory() as tmp:
                release = Path(tmp) / 'release'
                out = Path(tmp) / 'out'
                out.mkdir()
                self.make_release(release, 'build/cjk_core_8105.dat', ['cjk_core.dat'])
                (release / 'cjk_core.dat').write_bytes(payload)
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                    site.write_plus_flash_manifest(release, out, '9.9')
                self.assertIn('not what data_manifest_plus.txt describes', stderr.getvalue())

    def test_a_release_with_neither_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            release = Path(tmp) / 'release'
            out = Path(tmp) / 'out'
            out.mkdir()
            self.make_release(release, 'build/cjk_core_8105.dat', [])
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                site.write_plus_flash_manifest(release, out, '9.9')
            message = stderr.getvalue()
            self.assertIn('Plus data pack core is missing', message)
            self.assertIn('cjk_core.dat', message)
            self.assertIn('cjk_core_8105.dat', message)


if __name__ == '__main__':
    unittest.main()
