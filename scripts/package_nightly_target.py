#!/usr/bin/env python3
"""Package one model-specific firmware target and emit compatibility manifests."""

import argparse
import configparser
import csv
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from nightly_targets import (
    FLAVOR_TOKENS,
    CHANNELS,
    TARGETS,
    asset_name,
    environment_for,
    manifest_name,
    matrix,
    version_for,
)
FULL_INSTALL_SEGMENTS = (
    ('bootloader', 'bootloader.bin', 0x0000),
    ('partitions', 'partitions.bin', 0x8000),
    ('boot_app0', 'boot_app0.bin', 0xE000),
    ('firmware', 'firmware.bin', 0x10000),
)
STABLE_C3_SEGMENTS = (
    ('bootloader', 'bootloader.bin', 0x0000),
    ('partitions', 'partitions.bin', 0x8000),
    ('firmware', 'firmware.bin', 0x10000),
)
OTA_SEGMENT = (('firmware', 'firmware.bin', 0x10000),)
# Offsets/sizes are read from the target's own partition table (see
# nightly_targets.TARGETS['partitionsCsv']), not pinned here: readpico grew to
# 7.19 MiB slots for MuPDF while the shared partitions.csv kept 6.25 MiB, and a
# second copy of the map is exactly the drift this check exists to catch.
PARTITION_LAYOUT_ROWS = ('otadata', 'app0', 'app1')
FLASH_BYTES = 16 * 1024 * 1024


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def git_value(root, *args):
    return subprocess.check_output(['git', *args], cwd=root, text=True).strip()


def verify_partition_csv(root, csv_name='partitions.csv'):
    """Return {'otadata'|'app0'|'app1': (offset, size)} from the table this target flashes.

    Structural checks only, because the numbers legitimately differ per target:
    the two OTA slots must be adjacent, the map must end inside the flash, and a
    slot must be non-empty. That is what makes a wrong or truncated table fail
    here instead of on a user's device.
    """
    found = {}
    with (root / csv_name).open(newline='') as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith('#')):
            if len(row) < 5:
                continue
            name = row[0].strip()
            if name in PARTITION_LAYOUT_ROWS:
                found[name] = (int(row[3].strip(), 0), int(row[4].strip(), 0))
    missing = [name for name in PARTITION_LAYOUT_ROWS if name not in found]
    if missing:
        raise SystemExit(f'{csv_name} is missing partition row(s): {missing!r}')
    app0, app1 = found['app0'], found['app1']
    if app0[1] == 0 or app1[1] == 0:
        raise SystemExit(f'{csv_name} has an empty OTA slot: {found!r}')
    if app0[0] + app0[1] != app1[0]:
        raise SystemExit(f'{csv_name} has a gap or overlap between the OTA slots: {found!r}')
    if app1[0] + app1[1] > FLASH_BYTES:
        raise SystemExit(f'{csv_name} runs past the {FLASH_BYTES} byte flash: {found!r}')
    return found


def find_boot_app0():
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR') or Path.home() / '.platformio').expanduser()
    path = core / 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
    if not path.is_file():
        raise SystemExit(f'boot_app0.bin not found at {path}; build the environment first')
    return path


def copy_asset(source, destination):
    if not source.is_file():
        raise SystemExit(f'missing build asset: {source}')
    shutil.copyfile(source, destination)


def verify_firmware(path, chip_id, board_tag):
    with path.open('rb') as stream:
        header = stream.read(24)
        if len(header) != 24 or header[0] != 0xE9:
            raise SystemExit(f'{path.name} is not an ESP image')
        actual_chip_id = int.from_bytes(header[12:14], 'little')
        if actual_chip_id != chip_id:
            raise SystemExit(
                f'{path.name} chip ID is 0x{actual_chip_id:04x}, expected 0x{chip_id:04x}'
            )

        marker = f'CROSSPOINT-BOARD-V1:{board_tag};'.encode()
        overlap = b''
        while chunk := stream.read(64 * 1024):
            data = overlap + chunk
            if marker in data:
                return
            overlap = data[-(len(marker) - 1):]
    raise SystemExit(f'{path.name} does not contain board tag {board_tag!r}')


def firmware_uses_data_packs(path):
    """True when this image contains the on-flash pack readers.

    Accepts a Path or a string; package_target passes a Path, but a small test
    or a release script may pass a string.

    The JILP/CWP1 magic constants are compiled only when the corresponding
    CROSSPOINT_*_PACK macro is set. A full-install package that never writes
    the spiffs blobs would boot, then silently fall back to English and the web
    recovery page. Fail the package instead of shipping that.
    """
    path = Path(path)
    data = path.read_bytes()
    return b'JILP' in data or b'CWP1' in data


def package_target(root, target_id, channel, output):
    target = TARGETS[target_id]
    if channel not in target['supportedChannels']:
        raise SystemExit(f'{target_id} does not support the {channel} channel')
    environment = environment_for(target_id, channel, 'global')
    build = root / '.pio/build' / environment
    output.mkdir(parents=True, exist_ok=True)
    partitions = verify_partition_csv(root, target.get('partitionsCsv', 'partitions.csv'))

    segments = (
        FULL_INSTALL_SEGMENTS
        if target['fullInstall']
        else STABLE_C3_SEGMENTS
        if channel == 'stable'
        else OTA_SEGMENT
    )
    assets = []
    for role, source_name, offset in segments:
        source = find_boot_app0() if role == 'boot_app0' else build / source_name
        destination = output / asset_name(target_id, source_name)
        copy_asset(source, destination)
        assets.append({
            'role': role,
            'name': destination.name,
            'offset': offset,
            'size': destination.stat().st_size,
            'sha256': sha256(destination),
        })

    firmware = next(output / asset['name'] for asset in assets if asset['role'] == 'firmware')
    if firmware.stat().st_size > partitions['app0'][1]:
        raise SystemExit(f'{firmware.name} exceeds the OTA app slot')
    verify_firmware(firmware, target['chipId'], target['boardTag'])

    # A full-install package must carry the data blobs when its firmware reads
    # them. readpico_nightly unflags both pack macros and stays fully embedded;
    # a future pack-using release target without spiffs assets fails here rather
    # than shipping an app-only image that silently degrades.
    if firmware_uses_data_packs(firmware):
        data_roles = {'pinyin', 'hyphen', 'i18n', 'web'}
        has_data_assets = any(asset['role'] in data_roles for asset in assets)
        if not has_data_assets:
            if target['fullInstall']:
                raise SystemExit(
                    f'{target_id}: firmware reads data/spiffs packs but this '
                    'full-install package has no data assets. Add pinyin/hyphen/'
                    'i18n/web at their manifest offsets, or build without '
                    'CROSSPOINT_I18N_PACK / CROSSPOINT_WEB_ASSETS_FROM_PARTITION.'
                )
            print(
                f'warning: {target_id}: OTA artifact uses data/spiffs packs; the '
                "device's data partition must already match or the app falls back "
                'to English / the web recovery page.'
            )

    config = configparser.ConfigParser()
    config.read(root / 'platformio.ini')
    short_sha = git_value(root, 'rev-parse', '--short=7', 'HEAD')
    manifest = {
        'schemaVersion': 1,
        'channel': channel,
        'targetId': target_id,
        'models': target['models'],
        'deviceSlug': target['deviceSlug'],
        'boardTag': target['boardTag'],
        'supportedChannels': target['supportedChannels'],
        'environment': environment,
        'chip': target['chip'],
        'version': version_for(
            config['crosspoint']['version'], target_id, channel, 'global', short_sha
        ),
        'crossmuxSha': git_value(root, 'rev-parse', 'HEAD'),
        'sdkSha': git_value(root / 'freeink-sdk', 'rev-parse', 'HEAD'),
        'assets': assets,
    }
    if target['fullInstall']:
        manifest['partitionProfile'] = 'crossmux-sticky-v1'
        manifest['flash'] = {'size': 0x1000000, 'mode': 'dio', 'frequency': '80m'}

    manifest_paths = []
    for flavor in FLAVOR_TOKENS:
        manifest_path = output / manifest_name(target_id, flavor)
        manifest_path.write_text(json.dumps({**manifest, 'flavor': flavor}, indent=2) + '\n')
        manifest_paths.append(manifest_path)

    checksum_paths = [output / asset['name'] for asset in assets] + manifest_paths
    checksum_name = f"{target['deviceSlug']}-SHA256SUMS"
    (output / checksum_name).write_text(
        ''.join(f'{sha256(path)}  {path.name}\n' for path in checksum_paths)
    )
    print(f"Packaged {target_id} {manifest['version']} in {output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('target', nargs='?', choices=TARGETS)
    parser.add_argument('--channel', choices=CHANNELS, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--matrix', action='store_true')
    args = parser.parse_args()
    if args.matrix:
        print(json.dumps(matrix(args.channel), separators=(',', ':')))
        return
    if not args.target:
        parser.error('target is required unless --matrix is used')

    root = Path(__file__).resolve().parent.parent
    output = (args.output or root / 'dist' / args.channel / args.target).resolve()
    package_target(root, args.target, args.channel, output)


if __name__ == '__main__':
    main()
