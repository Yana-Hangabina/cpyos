#!/usr/bin/env python3
"""The Plus web installer manifest must follow the release layout, not literals.

Contract since the data manifest became the single naming authority:
  * the partition table comes from the release asset, and both app slots are
    placed from the offsets inside it;
  * each data pack is the file the manifest's last column names (no alias
    lookup -- a rename table is how a pack silently went missing);
  * a Plus firmware published without its partition table or manifest refuses
    the site build.  That omission (0.8.1..0.8.4) left the install page serving
    a 404 for flash-plus.json while the release looked complete.
"""
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

TABLE = b''.join([
    struct.pack('<H', 0x50AA) + bytes([1, 2]) + struct.pack('<II', 0x9000, 0x5000) + b'nvs'.ljust(16, b'\0') + struct.pack('<I', 1),
    struct.pack('<H', 0x50AA) + bytes([1, 0]) + struct.pack('<II', 0xE000, 0x2000) + b'otadata'.ljust(16, b'\0') + struct.pack('<I', 1),
    struct.pack('<H', 0x50AA) + bytes([0, 0x10]) + struct.pack('<II', 0x10000, 0x660000) + b'app0'.ljust(16, b'\0') + struct.pack('<I', 1),
    struct.pack('<H', 0x50AA) + bytes([0, 0x11]) + struct.pack('<II', 0x670000, 0x660000) + b'app1'.ljust(16, b'\0') + struct.pack('<I', 1),
    struct.pack('<H', 0x50AA) + bytes([1, 0x82]) + struct.pack('<II', 0xCD0000, 0x320000) + b'spiffs'.ljust(16, b'\0') + struct.pack('<I', 1),
])

PACKS = [('pinyin', 0xCD0000), ('web', 0xD26000), ('hyphen', 0xD60000),
         ('i18n', 0xDB6000), ('emoji', 0xE0D000), ('core', 0xE2C000)]


def make_release(root: Path, *, table: bool = True, manifest: bool = True,
                 firmware: bool = True, packs: list[str] | None = None) -> Path:
    release = root / 'release'
    release.mkdir()
    if table:
        (release / 'cpyos-plus-9.9-e4p-partition-table.bin').write_bytes(TABLE)
    if firmware:
        (release / 'cpyos-plus-9.9-e4p.bin').write_bytes(b'firmware')
    wanted = {name for name, _ in PACKS} if packs is None else set(packs)
    lines = ['partition\tspiffs\t0x00cd0000\t0x00320000']
    for name, address in PACKS:
        payload = name.encode()
        if name in wanted:
            (release / f'plus-{name}.dat').write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        lines.append(f'data\t{name}\t0\t0x{address:08x}\t{len(payload)}\t{digest}\tplus-{name}.dat')
    if manifest:
        (release / 'data_manifest_plus.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return release


class PlusManifestTest(unittest.TestCase):
    def test_parts_follow_partition_table_and_data_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = make_release(root)
            out = root / 'out'
            out.mkdir()
            site.write_plus_flash_manifest(release, out, '9.9')
            data = json.loads((out / 'flash-plus.json').read_text(encoding='utf-8'))
            offsets = [(part['offset'], part['path']) for part in data['parts']]
            self.assertEqual(offsets[0], (0x8000, 'cpyos-plus-9.9-e4p-partition-table.bin'))
            self.assertEqual(offsets[1], (0x10000, 'cpyos-plus-9.9-e4p.bin'))
            self.assertEqual(offsets[2], (0x670000, 'cpyos-plus-9.9-e4p.bin'))
            self.assertEqual(offsets[-1], (0xE2C000, 'plus-core.dat'))
            self.assertEqual(len(data['parts']), 9)

    def test_data_pack_is_taken_from_the_manifest_name(self):
        """No alias lookup: a manifest naming plus-core.dat must find that file."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = make_release(root)
            (release / 'cjk_core.dat').write_bytes(b'stale-alias')  # must be ignored
            out = root / 'out'
            out.mkdir()
            site.write_plus_flash_manifest(release, out, '9.9')
            parts = json.loads((out / 'flash-plus.json').read_text(encoding='utf-8'))['parts']
            core = next(part for part in parts if part['offset'] == 0xE2C000)
            self.assertEqual(core['path'], 'plus-core.dat')

    def test_firmware_without_partition_table_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = make_release(root, table=False)
            out = root / 'out'
            out.mkdir()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                site.write_plus_flash_manifest(release, out, '9.9')
            self.assertIn('partition-table.bin', stderr.getvalue())
            self.assertFalse((out / 'flash-plus.json').exists())

    def test_firmware_without_data_manifest_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = make_release(root, manifest=False)
            out = root / 'out'
            out.mkdir()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                site.write_plus_flash_manifest(release, out, '9.9')
            self.assertIn('data_manifest_plus.txt', stderr.getvalue())

    def test_missing_data_pack_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = make_release(root, packs=['pinyin', 'web'])
            out = root / 'out'
            out.mkdir()
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                site.write_plus_flash_manifest(release, out, '9.9')
            self.assertIn('Plus data pack hyphen is missing', stderr.getvalue())

    def test_release_without_plus_firmware_omits_the_installer(self):
        """A Read Pico only release is legitimate: skip, do not fail."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release = make_release(root, table=False, manifest=False, firmware=False)
            out = root / 'out'
            out.mkdir()
            site.write_plus_flash_manifest(release, out, '9.9')
            self.assertFalse((out / 'flash-plus.json').exists())


if __name__ == '__main__':
    unittest.main()
