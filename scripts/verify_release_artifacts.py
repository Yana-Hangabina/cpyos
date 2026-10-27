#!/usr/bin/env python3
"""Prove that the image on each release path is the one this build produced.

Four paths ship a readpico image and none of them ever checked that they agree:

  * TF card         exp/flash_device.sh      .pio/build/readpico/firmware.bin
  * device OTA      src/network/OtaUpdater   <slug>-firmware.bin (server side)
  * release/nightly scripts/package_nightly_target.py -> dist/<channel>/<slug>/
  * full installs   FULL_INSTALL_SEGMENTS    bootloader + partitions + boot_app0 + firmware

The failures this exists to catch have all happened here at least once:

  * flashing the wrong target's image (the ESP32-C3 and S3 builds look alike and
    have the same name);
  * an image built against Espressif's prebuilt Arduino core, where every
    custom_sdkconfig setting silently did not apply -- only
    esp_app_desc.project_name tells those apart (AGENTS.md section 6);
  * an app image larger than the slot its partition table gives it, which the
    nightly packager only notices at upload time;
  * a data manifest (build/data_manifest.txt) that describes blobs which are no
    longer the ones on disk, so the device boots and silently loses pinyin, the
    language pack or the web pages.

What it deliberately does NOT claim: the TF image and the nightly image are the
same file. They are not, and cannot be -- readpico keeps the language and web
packs on the data partition while readpico_nightly embeds them, and they differ
in log level and version string. The script therefore checks each artifact
against its own contract and prints which path each one takes, instead of
comparing hashes that never matched.

Usage:
  python3 scripts/verify_release_artifacts.py                        # TF image + data blobs
  python3 scripts/verify_release_artifacts.py --release              # also reject a -dirty image
  python3 scripts/verify_release_artifacts.py --dist dist/nightly/readpico
  python3 scripts/verify_release_artifacts.py --firmware other.bin --no-data
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import nightly_targets  # noqa: E402
import package_nightly_target  # noqa: E402

APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
# magic, secure_version, reserved[8], version[32], project_name[32], time[16], date[16]
APP_DESC = struct.Struct('<II8s32s32s16s16s')
ESP_IMAGE_MAGIC = 0xE9
BOARD_TAG_PREFIX = b'CROSSPOINT-BOARD-V1:'
# PlatformIO writes the project directory name into esp_app_desc.project_name.
# A checkout of this fork is usually named cpyos while an upstream checkout is
# crossmux. The guard exists to catch Espressif's prebuilt core, whose name is
# arduino-lib-builder, so both of our own names are valid.
PROJECT_NAMES = ('crossmux', 'cpyos')


def fail(message):
    print('error: ' + message, file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def c_string(raw):
    return raw.split(b'\0', 1)[0].decode('utf-8', 'replace')


def parse_app_desc(blob):
    """esp_app_desc_t from a firmware image, or None when the magic is absent."""
    if len(blob) < APP_DESC_OFFSET + APP_DESC.size:
        return None
    magic, secure_version, _reserved, version, project, build_time, build_date = APP_DESC.unpack_from(
        blob, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        return None
    return {
        'secure_version': secure_version,
        'version': c_string(version),
        'project_name': c_string(project),
        'time': c_string(build_time),
        'date': c_string(build_date),
    }


def check_image(root, path, target, release):
    """Validate one firmware image against the target's own hardware and slot."""
    if not path.is_file():
        fail(f'no firmware at {path}; build it first (pio run -e {target["environments"].get("nightly", "readpico")})')
    blob = path.read_bytes()
    notes = []

    if len(blob) < APP_DESC_OFFSET + APP_DESC.size or blob[0] != ESP_IMAGE_MAGIC:
        fail(f'{path.name} does not start with an ESP application image (magic 0xE9)')

    chip_id = int.from_bytes(blob[12:14], 'little')
    if chip_id != target['chipId']:
        fail(f'{path.name} is built for chip id 0x{chip_id:04x}, but {target["deviceSlug"]} is '
             f'0x{target["chipId"]:04x}')

    board_marker = BOARD_TAG_PREFIX + target['boardTag'].encode() + b';'
    if board_marker not in blob:
        fail(f'{path.name} does not carry the board tag {board_marker!r}; it belongs to another board')

    desc = parse_app_desc(blob)
    if desc is None:
        fail(f'{path.name} has no esp_app_desc (magic 0x{APP_DESC_MAGIC:08x} missing); '
             'that is not a firmware this project built')
    if desc['project_name'] not in PROJECT_NAMES:
        fail(f'{path.name} was built against the wrong Arduino core: esp_app_desc.project_name is '
             f'{desc["project_name"]!r}, not one of {PROJECT_NAMES}. custom_sdkconfig did not apply '
             '(AGENTS.md section 6) -- rebuild after clearing the framework cache')
    notes.append(f'app_desc project=crossmux version={desc["version"]} built {desc["date"]} {desc["time"]}')

    if release and desc['version'].endswith('-dirty'):
        fail(f'{path.name} is a dirty build ({desc["version"]}); commit before releasing')

    partitions = package_nightly_target.verify_partition_csv(root, target.get('partitionsCsv', 'partitions.csv'))
    slot = partitions['app0'][1]
    if len(blob) > slot:
        fail(f'{path.name} is {len(blob)} bytes but the app0 slot is {slot} bytes '
             f'({target.get("partitionsCsv", "partitions.csv")})')
    notes.append(f'fits app0: {len(blob)} / {slot} bytes ({slot - len(blob)} spare)')

    if target.get('partitionsCsv') == 'partitions_readpico.csv':
        notes.append('TF path: app0 + pinyin/hyphen/language pack/web pack on the data partition')
    return blob, desc, notes


def check_data_manifest(root):
    """The blobs described by build/data_manifest.txt must be the blobs on disk."""
    manifest = root / 'build' / 'data_manifest.txt'
    if not manifest.is_file():
        fail(f'no data manifest at {manifest}; run python3 scripts/generate_data_manifest.py')
    notes = []
    for line in manifest.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if fields[0] != 'data':
            continue
        _kind, name, offset, _address, size, expected_sha, relative = fields
        path = root / relative
        if not path.is_file():
            fail(f'the manifest lists {relative}, which is not on disk')
        actual_size = path.stat().st_size
        if actual_size != int(size):
            fail(f'{relative} is {actual_size} bytes but the manifest says {size}; regenerate it')
        actual_sha = sha256_file(path)
        if actual_sha != expected_sha:
            fail(f'{relative} does not match the manifest sha256 (offset {offset}); regenerate it')
        notes.append(f'data {name} {relative} {actual_size} B sha256 ok')

    # Freshness means "regenerating would produce this file", which needs the C++
    # headers the generator reads. Fixture trees in the tests have neither, so the
    # blob check above is what they exercise.
    generator = root / 'scripts' / 'generate_data_manifest.py'
    if generator.is_file():
        check = subprocess.run([sys.executable, str(generator), '--check', '--repo', str(root)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            detail = (check.stderr or check.stdout).strip().splitlines()
            fail('build/data_manifest.txt is stale (' + (detail[-1] if detail else 'no output') +
                 '); run python3 scripts/generate_data_manifest.py')
        notes.append('data_manifest.txt is current')
    return notes


def check_dist(dist, target_id, target, image_sha, image_blob):
    """Validate a packaged release directory (two flavor manifests + SHA256SUMS)."""
    if not dist.is_dir():
        fail(f'no package directory at {dist}')
    slug = target['deviceSlug']
    notes = []

    manifests = {}
    for flavor in nightly_targets.FLAVOR_TOKENS:
        path = dist / nightly_targets.manifest_name(target_id, flavor)
        if not path.is_file():
            fail(f'{path.name} is missing from {dist}')
        manifests[flavor] = json.loads(path.read_text())

    asset_sets = {flavor: {a['name']: a['sha256'] for a in manifest['assets']}
                  for flavor, manifest in manifests.items()}
    first_flavor = next(iter(nightly_targets.FLAVOR_TOKENS))
    for flavor, assets in asset_sets.items():
        if assets != asset_sets[first_flavor]:
            fail(f'{flavor} and {first_flavor} manifests describe different binaries; '
                 'one installer would flash an image the other never published')
    notes.append(f'{len(asset_sets[first_flavor])} assets identical across '
                 f'{len(nightly_targets.FLAVOR_TOKENS)} flavor manifests')

    sums = dist / f'{slug}-SHA256SUMS'
    if not sums.is_file():
        fail(f'{sums.name} is missing from {dist}')
    listed = {}
    for line in sums.read_text().splitlines():
        if not line.strip():
            continue
        digest, name = line.split(None, 1)
        listed[name.strip()] = digest
    for name, digest in listed.items():
        path = dist / name
        if not path.is_file():
            fail(f'{sums.name} lists {name}, which is not in {dist}')
        if sha256_file(path) != digest:
            fail(f'{name} does not match its {sums.name} entry')
    missing = set(asset_sets[first_flavor]) - set(listed)
    if missing:
        fail(f'{sums.name} does not cover {sorted(missing)}')
    notes.append(f'{len(listed)} files match {sums.name}')

    firmware_name = next(name for name in asset_sets[first_flavor] if name.endswith('-firmware.bin'))
    packaged = (dist / firmware_name).read_bytes()
    packaged_desc = parse_app_desc(packaged)
    if packaged_desc is None or packaged_desc['project_name'] not in PROJECT_NAMES:
        fail(f'{firmware_name} in {dist} is not a CrossMux/CPYOS image')

    version = manifests[first_flavor].get('version', '')
    if '+' in version and version.encode() not in packaged:
        fail(f'{firmware_name} does not embed the packaged version {version!r}; the manifest and the '
             'image disagree about which build this is (CROSSPOINT_RC_HASH missing at build time?)')

    if image_blob is not None:
        # Only a real mismatch is worth failing on: a local readpico build and a
        # packaged readpico_nightly image are different builds by design.
        same_version = version and version.encode() in image_blob
        if same_version and sha256_file(dist / firmware_name) != image_sha:
            fail(f'{firmware_name} claims version {version!r} like the local build but its bytes differ; '
                 'this is the artifact mix-up the check exists for')
        if same_version:
            notes.append(f'packaged image matches the local build ({version})')
        else:
            notes.append('packaged image is a different build than the local one (nightly vs local env)')
    return notes


def main(argv=None):
    parser = argparse.ArgumentParser(description='Verify that release artifacts describe one build.')
    parser.add_argument('--target', default='readpico', help='target id from scripts/nightly_targets.py')
    parser.add_argument('--firmware', help='firmware image (default: the target env build)')
    parser.add_argument('--dist', help='packaged output directory to verify as well')
    parser.add_argument('--release', action='store_true', help='reject a -dirty image')
    parser.add_argument('--no-data', action='store_true', help='skip the data manifest/blob check')
    args = parser.parse_args(argv)

    if args.target not in nightly_targets.TARGETS:
        fail(f'unknown target {args.target!r}; known: {sorted(nightly_targets.TARGETS)}')
    target = nightly_targets.TARGETS[args.target]
    root = Path(__file__).resolve().parent.parent

    firmware = Path(args.firmware) if args.firmware else (
        root / '.pio' / 'build' / 'readpico' / 'firmware.bin')
    blob, _desc, notes = check_image(root, firmware, target, args.release)
    image_sha = hashlib.sha256(blob).hexdigest()

    if not args.no_data:
        notes += check_data_manifest(root)
    if args.dist:
        notes += check_dist(Path(args.dist), args.target, target, image_sha, blob)

    print(f'Verified {args.target} artifacts ({len(notes)} checks):')
    for note in notes:
        print(f'  {note}')
    print(f'  firmware sha256 {image_sha}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
