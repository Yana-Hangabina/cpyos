"""Exercise scripts/verify_release_artifacts.py against fixture images.

Every case here is a failure this project has actually shipped once: an image for
the wrong chip, a board tag from another target, an image built against the
prebuilt Arduino core (where custom_sdkconfig silently did not apply), a firmware
larger than its app slot, and a data manifest that no longer describes the blobs.
"""

import hashlib
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

import nightly_targets
import verify_release_artifacts as v
from nightly_targets import FLAVOR_TOKENS, TARGETS


def make_image(chip_id=0x0009, board='readpico', project='crossmux', version='1.6.5-readpico',
               size=4096):
    """A minimal ESP application image: header, board tag, esp_app_desc, padding."""
    image = bytearray(size)
    image[0] = 0xE9
    image[12:14] = chip_id.to_bytes(2, 'little')
    desc = struct.pack('<II8s32s32s16s16s', 0xABCD5432, 0, b'\0' * 8,
                       version.encode().ljust(32, b'\0'), project.encode().ljust(32, b'\0'),
                       b'Oct 25 2026'.ljust(16, b'\0'), b'12:00:00'.ljust(16, b'\0'))
    image[0x20:0x20 + len(desc)] = desc
    # The board tag lives outside the descriptor (as in a real image, where it is a
    # string constant elsewhere in .rodata).
    marker = f'CROSSPOINT-BOARD-V1:{board};'.encode()
    image[0x100:0x100 + len(marker)] = marker
    return bytes(image)


class ImageCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        shutil.copy(ROOT / 'partitions_readpico.csv', self.root / 'partitions_readpico.csv')

    def tearDown(self):
        self.temp.cleanup()

    def check(self, blob, release=False, target=None):
        path = self.root / 'firmware.bin'
        path.write_bytes(blob)
        return v.check_image(self.root, path, target or TARGETS['readpico'], release)

    def test_good_image_passes_and_reports_the_slot(self):
        _blob, desc, notes = self.check(make_image())
        self.assertEqual(desc['project_name'], 'crossmux')
        self.assertTrue(any('fits app0' in note for note in notes))

    def test_renamed_checkout_project_passes(self):
        # PlatformIO writes the checkout directory name into esp_app_desc; a
        # CPYOS clone is normally named cpyos, not crossmux.
        _blob, desc, _notes = self.check(make_image(project='cpyos'))
        self.assertEqual(desc['project_name'], 'cpyos')

    def test_wrong_chip_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.check(make_image(chip_id=0x0005))

    def test_wrong_board_tag_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.check(make_image(board='sticky'))

    def test_prebuilt_arduino_core_is_rejected(self):
        # The exact signature of the incident in AGENTS.md section 6.
        with self.assertRaises(SystemExit):
            self.check(make_image(project='arduino-lib-builder'))

    def test_missing_app_description_is_rejected(self):
        blob = bytearray(make_image())
        blob[0x20:0x24] = b'\0\0\0\0'
        with self.assertRaises(SystemExit):
            self.check(bytes(blob))

    def test_dirty_build_only_fails_a_release_check(self):
        dirty = make_image(version='870d7c2b-dirty')
        self.check(dirty)  # a development image is allowed
        with self.assertRaises(SystemExit):
            self.check(dirty, release=True)

    def test_image_larger_than_the_slot_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.check(make_image(size=0x730000 + 1))


class DataManifestTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / 'build').mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def write_manifest(self, blob_bytes, sha_override=None):
        blob = self.root / 'build/pinyin.dat'
        blob.write_bytes(blob_bytes)
        digest = sha_override or hashlib.sha256(blob_bytes).hexdigest()
        (self.root / 'build/data_manifest.txt').write_text(
            'partition\tspiffs\t0x00e70000\t0x00180000\n'
            'reserved\t0x00080000\t12\tfactory bytes\n'
            f'data\tpinyin\t0x00000000\t0x00e70000\t{len(blob_bytes)}\t{digest}\tbuild/pinyin.dat\n')

    def test_matching_blob_passes(self):
        self.write_manifest(b'PYDB' + b'\0' * 60)
        notes = v.check_data_manifest(self.root)
        self.assertTrue(any('sha256 ok' in note for note in notes))

    def test_stale_sha_is_rejected(self):
        # The manifest describes an older blob: the device would boot and silently
        # lose pinyin search.
        self.write_manifest(b'PYDB' + b'\0' * 60, sha_override='0' * 64)
        with self.assertRaises(SystemExit):
            v.check_data_manifest(self.root)

    def test_missing_blob_is_rejected(self):
        self.write_manifest(b'PYDB' + b'\0' * 60)
        (self.root / 'build/pinyin.dat').unlink()
        with self.assertRaises(SystemExit):
            v.check_data_manifest(self.root)


class DistCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dist = Path(self.temp.name)
        self.target = TARGETS['readpico']
        self.image = make_image(size=4096, version='1.6.5-readpico-rc+1234567')
        (self.dist / 'readpico-firmware.bin').write_bytes(self.image)
        self.assets = [{
            'role': 'firmware',
            'name': 'readpico-firmware.bin',
            'offset': 0x10000,
            'size': len(self.image),
            'sha256': hashlib.sha256(self.image).hexdigest(),
        }]
        self.write_package(version='1.6.5-readpico-rc+1234567')

    def tearDown(self):
        self.temp.cleanup()

    def write_package(self, version, second_flavor=None):
        manifest = {'channel': 'nightly', 'version': version, 'assets': self.assets}
        for flavor in FLAVOR_TOKENS:
            payload = dict(manifest)
            if second_flavor is not None and flavor == list(FLAVOR_TOKENS)[1]:
                payload['assets'] = second_flavor
            (self.dist / nightly_targets.manifest_name('readpico', flavor)).write_text(json.dumps(payload))
        lines = [f"{asset['sha256']}  {asset['name']}\n" for asset in self.assets]
        (self.dist / 'readpico-SHA256SUMS').write_text(''.join(lines))

    def check(self, image=None):
        return v.check_dist(self.dist, 'readpico', self.target,
                            hashlib.sha256(image or self.image).hexdigest(), image or self.image)

    def test_consistent_package_passes(self):
        notes = self.check()
        self.assertTrue(any('flavor manifests' in note for note in notes))

    def test_checksum_mismatch_is_rejected(self):
        (self.dist / 'readpico-firmware.bin').write_bytes(make_image(size=4096, version='x'))
        with self.assertRaises(SystemExit):
            self.check()

    def test_flavor_manifests_describing_different_binaries_are_rejected(self):
        other = dict(self.assets[0], sha256='0' * 64)
        self.write_package(version='1.6.5-readpico-rc+1234567', second_flavor=[other])
        with self.assertRaises(SystemExit):
            self.check()

    def test_image_not_carrying_the_manifest_version_is_rejected(self):
        # CROSSPOINT_RC_HASH unset at build time: the manifest says +1234567 but the
        # image does not contain it.
        plain = make_image(size=4096, version='1.6.5-readpico-rc+')
        self.image = plain
        (self.dist / 'readpico-firmware.bin').write_bytes(plain)
        self.assets[0].update(size=len(plain), sha256=hashlib.sha256(plain).hexdigest())
        self.write_package(version='1.6.5-readpico-rc+1234567')
        with self.assertRaises(SystemExit):
            self.check(plain)


if __name__ == '__main__':
    unittest.main()
