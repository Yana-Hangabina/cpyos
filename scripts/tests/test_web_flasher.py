"""The web flasher bundle must agree with the install package it is built from.

It exists so a user can install the firmware from a browser, which only stays
true if the parts, their offsets and the version come from the same sources the
full-install package and the partition table use.
"""

import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

import build_web_flasher  # noqa: E402
import nightly_targets  # noqa: E402
import package_nightly_target  # noqa: E402


def make_image(chip_id=0x0009, board='readpico', size=2048, version='1.6.5-readpico-rc+1234567'):
    image = bytearray(size)
    image[0] = 0xE9
    image[12:14] = chip_id.to_bytes(2, 'little')
    desc = struct.pack('<II8s32s32s16s16s', 0xABCD5432, 0, b'\0' * 8,
                       version.encode().ljust(32, b'\0'), b'crossmux'.ljust(32, b'\0'),
                       b'Oct 25 2026'.ljust(16, b'\0'), b'12:00:00'.ljust(16, b'\0'))
    image[0x20:0x20 + len(desc)] = desc
    marker = f'CROSSPOINT-BOARD-V1:{board};'.encode()
    image[0x100:0x100 + len(marker)] = marker
    return bytes(image)


class WebFlasherTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.dist = self.root / 'dist'
        self.dist.mkdir()
        self.output = self.root / 'web'
        shutil.copy(ROOT / 'partitions_readpico.csv', self.root / 'partitions_readpico.csv')
        for _role, source_name, _offset in package_nightly_target.FULL_INSTALL_SEGMENTS:
            name = nightly_targets.asset_name('readpico', source_name)
            blob = make_image() if source_name == 'firmware.bin' else b'\x01' * 64
            (self.dist / name).write_bytes(blob)
        flavor = next(iter(nightly_targets.FLAVOR_TOKENS))
        (self.dist / nightly_targets.manifest_name('readpico', flavor)).write_text(
            json.dumps({'version': '1.6.5-readpico-rc+1234567'}))

    def tearDown(self):
        self.temp.cleanup()

    def build(self):
        return build_web_flasher.build('readpico', self.dist, self.output, root=self.root)

    def test_parts_offsets_and_version_come_from_the_install_package(self):
        manifest, output = self.build()
        expected = [{'path': nightly_targets.asset_name('readpico', name), 'offset': offset}
                    for _role, name, offset in package_nightly_target.FULL_INSTALL_SEGMENTS]
        self.assertEqual(manifest['builds'][0]['parts'], expected)
        self.assertEqual(manifest['builds'][0]['chipFamily'], 'ESP32-S3')
        self.assertEqual(manifest['version'], '1.6.5-readpico-rc+1234567')
        for part in expected:
            self.assertTrue((output / part['path']).is_file(), part['path'])
        page = (output / 'index.html').read_text()
        self.assertIn('1.6.5-readpico-rc+1234567', page)
        self.assertIn('manifest.json', page)

    def test_missing_asset_is_rejected(self):
        (self.dist / nightly_targets.asset_name('readpico', 'firmware.bin')).unlink()
        with self.assertRaises(SystemExit):
            self.build()

    def test_firmware_larger_than_the_slot_is_rejected(self):
        # 7.19 MiB slot + 1 byte. The bundle would install an image the bootloader
        # cannot hold, so this has to fail here rather than on the device.
        oversized = make_image(size=0x730000 + 1)
        (self.dist / nightly_targets.asset_name('readpico', 'firmware.bin')).write_bytes(oversized)
        with self.assertRaises(SystemExit):
            self.build()


if __name__ == '__main__':
    unittest.main()
