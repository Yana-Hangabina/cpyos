#!/usr/bin/env python3
"""Assemble a 脆皮鸭 CPYOS Read Pico release from one PlatformIO build.

The release has four consumers and they need different shapes:

  * first install  — bootloader + partition table + boot_app0 + app + data packs
                     (the factory layout differs, so a full install must also
                     write the partition table and may erase the chip);
  * upgrade        — app + data packs, no erase (for a device already on CPYOS);
  * device OTA     — the app image only, described by a GitHub-release-shaped
                     JSON that OtaUpdater already parses;
  * web flasher    — the same install/upgrade parts, referenced by relative
                     paths so GitHub Pages serves them from one origin.

Nothing here invents an offset: the app segments come from
package_nightly_target.FULL_INSTALL_SEGMENTS and the data addresses come from
build/data_manifest.txt, which is generated from the partition table and the
C++ loader constants. verify_release_artifacts is reused for the chip, board tag,
project name and app-slot checks.

Usage:
  pio run -e readpico
  python3 scripts/generate_hyphenation_db.py
  python3 scripts/generate_pinyin_db.py /path/to/pypinyin-0.55.0-py3-none-any.whl
  python3 scripts/generate_data_manifest.py
  python3 scripts/build_cpyos_release.py --version 0.1.0 --notes-json docs/releases/0.1.0.ota.json
"""

from __future__ import annotations

import argparse
import configparser
import json
import shutil
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_ota_notes  # noqa: E402
import nightly_targets  # noqa: E402
import package_nightly_target  # noqa: E402
import verify_release_artifacts  # noqa: E402

TARGET_ID = 'readpico'
ENVIRONMENT = 'readpico'
PRODUCT = '脆皮鸭 CPYOS'
PAGES_BASE = 'https://yana-hangabina.github.io/cpyos'

DATA_FILES = {
    'pinyin': 'pinyin.dat',
    'hyphen': 'hyphen.dat',
    'i18n': 'i18n_pack.dat',
    'web': 'web_pack.dat',
    'emoji': 'emoji.dat',
}


def fail(message: str) -> None:
    print(f'error: {message}', file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    return package_nightly_target.sha256(path)


def read_config_version(root: Path) -> str:
    config = configparser.ConfigParser()
    config.read(root / 'platformio.ini', encoding='utf-8')
    return config.get('crosspoint', 'version')


def read_data_manifest(root: Path) -> list[dict]:
    """Rows of build/data_manifest.txt, with absolute flash addresses."""
    path = root / 'build' / 'data_manifest.txt'
    if not path.is_file():
        fail(f'no {path}; run scripts/generate_data_manifest.py after building the packs')
    rows = []
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if fields[0] != 'data':
            continue
        if len(fields) != 7:
            fail(f'malformed data manifest row: {line!r}')
        _kind, name, offset, address, size, digest, relative = fields
        blob = root / relative
        if not blob.is_file():
            fail(f'the data manifest lists {relative}, which is missing')
        if blob.stat().st_size != int(size) or sha256_file(blob) != digest:
            fail(f'{relative} no longer matches the data manifest; re-run scripts/generate_data_manifest.py')
        rows.append({
            'role': name,
            'offset': int(address, 0),
            'partitionOffset': int(offset, 0),
            'size': int(size),
            'sha256': digest,
            'source': blob,
            'name': DATA_FILES.get(name, blob.name),
        })
    if not rows:
        fail('the data manifest contains no blobs')
    missing = {'pinyin', 'hyphen', 'i18n', 'web', 'emoji'} - {row['role'] for row in rows}
    if missing:
        fail('the data manifest is missing required blobs: ' + ', '.join(sorted(missing)))
    return rows


def copy(source: Path, destination: Path) -> Path:
    if not source.is_file():
        fail(f'missing build asset: {source}')
    shutil.copyfile(source, destination)
    return destination


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def parts(app_parts: list[dict], data_rows: list[dict]) -> list[dict]:
    out = [{'path': item['name'], 'offset': item['offset']} for item in app_parts]
    out.extend({'path': row['name'], 'offset': row['offset']} for row in data_rows)
    return out


def build_manifest(name: str, version: str, parts_list: list[dict], erase: bool) -> dict:
    return {
        'name': f'{PRODUCT} {version}',
        'version': version,
        'new_install_prompt_erase': erase,
        'builds': [{
            'chipFamily': 'ESP32-S3',
            'parts': parts_list,
        }],
    }


def flash_readme(version: str) -> str:
    return f'''# {PRODUCT} {version} — Read Pico (小纸 Pico)

This folder is a complete Read Pico installation. Keep it together; the
manifests reference the files next to them by name.

## First install or recovery

Use `manifest-install.json` (or the web flasher on the project site). It writes:

| file | flash offset |
|---|---|
| readpico-bootloader.bin | 0x0 |
| readpico-partitions.bin | 0x8000 |
| readpico-boot_app0.bin | 0xE000 |
| readpico-firmware.bin | 0x10000 |
| pinyin.dat / web_pack.dat / hyphen.dat / i18n_pack.dat / emoji.dat | data partition |

The install manifest asks the browser to erase the chip. That is required when
coming from the factory partition layout (7.19 MiB slots vs the factory table at
0xE000). SD-card books and settings under `/.crosspoint` are not touched.

## Upgrade from an existing CPYOS install

Use `manifest-upgrade.json`: it writes the app and data packs only, keeps NVS
and the partition table, and does not erase.

## Device OTA

`readpico-firmware.bin` is the app-only image used by the device's OTA screen.
`ota/readpico.json` is the manifest the firmware fetches; do not rename the
asset entry `firmware.bin` inside that JSON.

## Manual esptool

```bash
esptool.py --chip esp32s3 write_flash \\
  0x0 readpico-bootloader.bin 0x8000 readpico-partitions.bin \\
  0xE000 readpico-boot_app0.bin 0x10000 readpico-firmware.bin \\
  0xE70000 pinyin.dat 0xEC6000 web_pack.dat \\
  0xF00000 hyphen.dat 0xF70000 i18n_pack.dat \\
  0xFD0000 emoji.dat
```

Verify every file against `SHA256SUMS` before flashing. A complete chip backup
before the first install is strongly recommended.
'''


def build(version: str, root: Path, build_dir: Path, output: Path, notes: dict,
          pages_base: str, allow_dirty: bool = False) -> dict:
    configured = read_config_version(root)
    if configured != version:
        fail(f'platformio.ini version is {configured}, but --version is {version}')

    target = nightly_targets.TARGETS[TARGET_ID]
    partitions = package_nightly_target.verify_partition_csv(root, target['partitionsCsv'])
    firmware_source = build_dir / 'firmware.bin'
    if not firmware_source.is_file():
        fail(f'no {firmware_source}; run pio run -e {ENVIRONMENT} first')

    blob, desc, notes_lines = verify_release_artifacts.check_image(
        root, firmware_source, target, release=not allow_dirty)
    # esp_app_desc.version is the git revision (dirty builds end in -dirty), not
    # CROSSPOINT_VERSION. The UI and OTA compare CROSSPOINT_VERSION, which is a
    # literal in the image; check that instead.
    expected_build_version = f'{version}-cpyos'
    if expected_build_version.encode() not in blob:
        fail(f'{firmware_source.name} does not contain CROSSPOINT_VERSION {expected_build_version!r}')
    if not package_nightly_target.firmware_uses_data_packs(firmware_source):
        fail('this readpico image does not read the data packs; build the readpico '
             'environment (not readpico_nightly)')
    print('\n'.join('  ' + line for line in notes_lines))

    output.mkdir(parents=True, exist_ok=True)
    app_parts = []
    for role, source_name, offset in package_nightly_target.FULL_INSTALL_SEGMENTS:
        source = package_nightly_target.find_boot_app0() if role == 'boot_app0' else build_dir / source_name
        name = nightly_targets.asset_name(TARGET_ID, source_name)
        copy(source, output / name)
        app_parts.append({'role': role, 'name': name, 'offset': offset,
                          'size': (output / name).stat().st_size,
                          'sha256': sha256_file(output / name)})

    firmware_asset = next(item for item in app_parts if item['role'] == 'firmware')
    if firmware_asset['size'] > partitions['app0'][1]:
        fail(f'{firmware_asset["name"]} is {firmware_asset["size"]} bytes but app0 is {partitions["app0"][1]}')

    data_rows = read_data_manifest(root)
    for row in data_rows:
        copy(row['source'], output / row['name'])
        print(f'  data {row["role"]:<6} 0x{row["offset"]:06x} {row["size"]:>8} B  {row["name"]}')

    install_parts = parts(app_parts, data_rows)
    update_parts = parts([item for item in app_parts if item['role'] == 'firmware'], data_rows)
    app_only = parts([item for item in app_parts if item['role'] == 'firmware'], [])

    write_json(output / 'manifest-install.json', build_manifest('install', version, install_parts, True))
    write_json(output / 'manifest-upgrade.json', build_manifest('upgrade', version, update_parts, False))
    write_json(output / 'manifest-app.json', build_manifest('app', version, app_only, False))

    download_base = f'{pages_base.rstrip("/")}/downloads/{version}'
    ota = {
        'tag_name': version,
        'name': f'{PRODUCT} {version}',
        'release_notes': notes['zh'],
        'assets': [{
            'name': 'firmware.bin',
            'browser_download_url': f'{download_base}/{firmware_asset["name"]}',
            'size': firmware_asset['size'],
            'sha256': firmware_asset['sha256'],
        }],
    }
    (output / 'ota').mkdir(exist_ok=True)
    write_json(output / 'ota' / 'readpico.json', ota)
    # GitHub release assets are flat; keep a root-level copy for `gh release
    # upload` while the nested one is what Pages serves at /ota/readpico.json.
    write_json(output / 'readpico-ota.json', ota)

    latest = {
        'product': PRODUCT,
        'version': version,
        'tag': version,
        'publishedAt': datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace('+00:00', 'Z'),
        'downloads': {
            'firmware': f'{download_base}/{firmware_asset["name"]}',
            'manifestInstall': f'{download_base}/manifest-install.json',
            'manifestUpgrade': f'{download_base}/manifest-upgrade.json',
            'ota': f'{pages_base.rstrip("/")}/ota/readpico.json',
        },
        'notes': notes,
        'firmware': {
            'size': firmware_asset['size'],
            'sha256': firmware_asset['sha256'],
        },
    }
    write_json(output / 'latest.json', latest)
    (output / 'FLASH.md').write_text(flash_readme(version), encoding='utf-8')

    sums = []
    for path in sorted(output.iterdir()):
        if path.is_file() and path.name not in {'SHA256SUMS'}:
            sums.append(f'{sha256_file(path)}  {path.name}')
    (output / 'SHA256SUMS').write_text('\n'.join(sums) + '\n', encoding='utf-8')

    package = output / f'cpyos-readpico-{version}-full.zip'
    with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(output.iterdir()):
            if path.is_file() and path.name not in {package.name}:
                archive.write(path, arcname=path.name)
        archive.write(output / 'ota' / 'readpico.json', arcname='ota/readpico.json')

    print(f'Release {PRODUCT} {version} -> {output}')
    print(f'  install parts: {len(install_parts)}, upgrade parts: {len(update_parts)}')
    print(f'  OTA tag {ota["tag_name"]}, app {firmware_asset["size"]} B')
    print(f'  package: {package.name} ({package.stat().st_size} B)')
    return latest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Build a CPYOS Read Pico release bundle.')
    parser.add_argument('--version', required=True, help='exact [crosspoint] version, e.g. 0.1.0')
    parser.add_argument('--repo', default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument('--build-dir', default=None, help='default: <repo>/.pio/build/readpico')
    parser.add_argument('--output', default=None, help='default: <repo>/dist/cpyos/<version>')
    parser.add_argument('--notes-json', help='{"en":[...],"zh":[...]} OTA notes')
    parser.add_argument('--pages-base', default=PAGES_BASE)
    parser.add_argument('--allow-dirty', action='store_true',
                        help='local dry runs only; CI releases must use a clean tree')
    args = parser.parse_args(argv)

    root = Path(args.repo).resolve()
    build_dir = Path(args.build_dir) if args.build_dir else root / '.pio' / 'build' / ENVIRONMENT
    output = Path(args.output) if args.output else root / 'dist' / 'cpyos' / args.version
    if args.notes_json:
        notes = generate_ota_notes.validate_notes(json.loads(Path(args.notes_json).read_text(encoding='utf-8')))
    else:
        notes = generate_ota_notes.validate_notes({
            'en': [
                'refreshed UI home, library and find-books screens',
                'JJWXC shelf, discover, comments and purchase',
                'Water ripple page turn and shake page turn',
                'Chinese pinyin input and SD card fonts',
            ],
            'zh': [
                '界面 v5 首页、书架与找书界面',
                '晋江书架、发现、段评与购章',
                '水波纹翻页与摇晃翻页',
                '拼音输入与 SD 卡字体',
            ],
        })
    build(args.version, root, build_dir, output, notes, args.pages_base, args.allow_dirty)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
