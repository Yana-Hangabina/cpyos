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
import subprocess
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
# Device OTA downloads the GitHub release asset, not the Pages copy: Pages is
# ~9 KB/s from mainland China, while the GitHub mirrors below deliver far more.
GITHUB_RELEASES = 'https://github.com/Yana-Hangabina/cpyos/releases/download'
# Firmware up to 0.1.1 downloads the manifest URL verbatim, so advertise a
# working mirror. 0.1.2+ strips this prefix, probes direct/ghfast/gh-proxy and
# downloads through the fastest route.
OTA_MIRROR_PREFIX = 'https://gh-proxy.com/'

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


def flash_readme(version: str, partitions: dict, data_rows: list, full_install_only: bool = False) -> str:
    app0 = partitions['app0'][0]
    app1 = partitions['app1'][0]
    data_table = "\n".join(
        f"| {row['name']} | data partition + 0x{row['partitionOffset']:06x} (flash 0x{row['offset']:06x}) |"
        for row in data_rows)
    manual_data = " ".join(
        f"0x{row['offset']:06x} {row['name']}" for row in data_rows)
    app1_note = ("not written here; kept for manual recovery"
                 if full_install_only else "written when the updater flashes both slots")
    if full_install_only:
        upgrade_section = """## Upgrade from an existing CPYOS install

Do not use OTA for this release. The PDF partition and data layout changed; use
the full installer above. OTA resumes with the next release, whose manifest
writes **both** app slots. There is intentionally no `ota/readpico.json` in this
folder, so older firmware cannot pick up a half-migrated image.
"""
    else:
        upgrade_section = """## Device OTA

`readpico-firmware.bin` is the app-only image used by the device's OTA screen.
`ota/readpico.json` is the manifest the firmware fetches; do not rename the
asset entry `firmware.bin` inside that JSON.
"""
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
| readpico-firmware.bin | 0x{app0:06x} |
| readpico-app1.bin | 0x{app1:06x} ({app1_note}) |
| readpico-pdfmod.bin | 0x{partitions['pdfmod'][0]:06x} |
{data_table}

The install manifest asks the browser to erase the chip. That is required for
this release: 0.7.0 and older use a different partition table and a different
PDF/data layout, and must not be upgraded by OTA. SD-card books and settings
under `/.crosspoint` are not touched by a full install.

{upgrade_section}

## Manual esptool

Upgrade (or first install after writing the bootloader and partition table):

```bash
esptool.py --chip esp32s3 write_flash \\
  0x0 readpico-bootloader.bin 0x8000 readpico-partitions.bin \\
  0xE000 readpico-boot_app0.bin 0x{app0:06x} readpico-firmware.bin \\
  0x{app1:06x} readpico-app1.bin \\
  0x{partitions['pdfmod'][0]:06x} readpico-pdfmod.bin \\
  {manual_data}
```

For an existing CPYOS install, skip the bootloader/partition/boot_app0 part and
write both app slots plus the data packs. Verify every file against
`SHA256SUMS` before flashing. A complete chip backup before the first install is
strongly recommended.
'''


def build(version: str, root: Path, build_dir: Path, output: Path, notes: dict,
          pages_base: str, allow_dirty: bool = False, full_install_only: bool = False) -> dict:
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
    expected_build_version = f'{version}-rp'
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

    # The XIP PDF app needs its engine in the dedicated pdfmod partition.  It is
    # part of the full install only: later app-only OTA keeps the installed
    # engine, which is exactly what the v2 compatibility contract allows.
    pdfmod_source = root / 'build' / 'pdfmod.bin'
    pdfmod_part = None
    if pdfmod_source.is_file() and 'pdfmod' in partitions:
        pdfmod_name = nightly_targets.asset_name(TARGET_ID, 'pdfmod.bin')
        copy(pdfmod_source, output / pdfmod_name)
        pdfmod_part = {'role': 'pdf_engine', 'name': pdfmod_name,
                       'offset': partitions['pdfmod'][0],
                       'size': (output / pdfmod_name).stat().st_size,
                       'sha256': sha256_file(output / pdfmod_name)}
        if pdfmod_part['size'] > partitions['pdfmod'][1]:
            fail(f'{pdfmod_name} is {pdfmod_part["size"]} bytes but pdfmod is {partitions["pdfmod"][1]}')
    elif full_install_only:
        fail('build/pdfmod.bin is missing; build the paired PDF engine before a full install')

    # The font tiers moved app1 and spiffs. An OTA/manifest written at the new
    # addresses would land inside an old device's app1 slot (the new spiffs base
    # is the old app1 range), so the transition release must be a full install.
    layout_moved = partitions['app1'][0] != 0x740000 or partitions['spiffs'][0] != 0xE70000
    if layout_moved and not full_install_only:
        fail('this tree moved app1/spiffs for the font tiers (app1=0x%x, spiffs=0x%x); '
             'an OTA manifest would write at offsets an older device does not have and can '
             'corrupt its app1 slot. Publish with --full-install-only; later app-only OTA '
             'releases from the new layout are safe.'
             % (partitions['app1'][0], partitions['spiffs'][0]))

    for part in app_parts:
        limit = package_nightly_target.full_install_segment_limit(part['role'])
        if limit is not None and part['size'] > limit:
            fail(f'{part["name"]} is {part["size"]} bytes but its flash region is {limit}')

    firmware_asset = next(item for item in app_parts if item['role'] == 'firmware')
    if firmware_asset['size'] > partitions['app0'][1]:
        fail(f'{firmware_asset["name"]} is {firmware_asset["size"]} bytes but app0 is {partitions["app0"][1]}')
    if firmware_asset['size'] > partitions['app1'][1]:
        fail(f'{firmware_asset["name"]} is {firmware_asset["size"]} bytes but app1 is {partitions["app1"][1]}')

    # A device that has run an OTA boots from app1 (otadata seq), and the
    # browser flasher cannot read otadata to pick the active slot. Writing only
    # app0 would therefore look successful while the device kept booting the
    # old app1. Upgrade and app-only manifests write BOTH app slots; the image
    # is byte-identical, so whichever slot the bootloader picks is current.
    app1_name = nightly_targets.asset_name(TARGET_ID, 'app1.bin')
    copy(firmware_source, output / app1_name)
    app1_part = {'role': 'firmware_app1', 'name': app1_name, 'offset': partitions['app1'][0],
                 'size': (output / app1_name).stat().st_size,
                 'sha256': sha256_file(output / app1_name)}
    if app1_part['sha256'] != firmware_asset['sha256']:
        fail('the app1 copy is not byte-identical to the app0 firmware')

    data_rows = read_data_manifest(root)
    for row in data_rows:
        copy(row['source'], output / row['name'])
        print(f'  data {row["role"]:<6} 0x{row["offset"]:06x} {row["size"]:>8} B  {row["name"]}')

    # Device OTA reads this next to readpico-ota.json. It lists the same blobs
    # with partition offsets, SHA-256 and an erase limit; without it an app-only
    # OTA can only update the app slot.
    subprocess.run([sys.executable, str(root / 'scripts' / 'generate_pack_manifest.py'), '--repo', str(root)],
                   check=True)
    copy(root / 'build' / 'readpico-packs.txt', output / 'readpico-packs.txt')
    print('  data packs manifest: readpico-packs.txt')

    install_app_parts = app_parts + ([pdfmod_part] if pdfmod_part is not None else [])
    install_parts = parts(install_app_parts, data_rows)
    update_parts = parts([firmware_asset, app1_part], data_rows)
    app_only = parts([firmware_asset, app1_part], [])

    def require_both_slots(parts_list, label):
        offsets = {item['offset'] for item in parts_list}
        if partitions['app0'][0] not in offsets or partitions['app1'][0] not in offsets:
            fail(f'{label} must write both app0 and app1; got {sorted(hex(o) for o in offsets)}')

    require_both_slots(update_parts, 'manifest-upgrade.json')
    require_both_slots(app_only, 'manifest-app.json')

    write_json(output / 'manifest-install.json', build_manifest('install', version, install_parts, True))
    if full_install_only:
        print('  full-install-only: no manifest-upgrade.json, manifest-app.json or OTA manifest')
    else:
        write_json(output / 'manifest-upgrade.json', build_manifest('upgrade', version, update_parts, False))
        write_json(output / 'manifest-app.json', build_manifest('app', version, app_only, False))

    download_base = f'{pages_base.rstrip("/")}/downloads/{version}'
    github_asset = f'{GITHUB_RELEASES}/cpyos-{version}/{firmware_asset["name"]}'
    ota = {
        'tag_name': version,
        'name': f'{PRODUCT} {version}',
        'release_notes': notes['zh'],
        'assets': [{
            'name': 'firmware.bin',
            'browser_download_url': f'{OTA_MIRROR_PREFIX}{github_asset}',
            'size': firmware_asset['size'],
            'sha256': firmware_asset['sha256'],
        }],
    }
    if not full_install_only:
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
            **({} if full_install_only else {
                'manifestUpgrade': f'{download_base}/manifest-upgrade.json',
                'ota': f'{pages_base.rstrip("/")}/ota/readpico.json',
            }),
        },
        'notes': notes,
        'firmware': {
            'size': firmware_asset['size'],
            'sha256': firmware_asset['sha256'],
        },
    }
    write_json(output / 'latest.json', latest)
    (output / 'FLASH.md').write_text(flash_readme(version, partitions, data_rows, full_install_only), encoding='utf-8')

    # The release workflow uploads top-level files only. Keep the project license,
    # the JJWXC statement and every third-party license text in that flat shape.
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'NOTICE-JJWXC.md'):
        copy(root / name, output / name)
    for src in sorted((root / 'LICENSES').iterdir()):
        if src.is_file():
            copy(src, output / f'LICENSE-{src.name}')
    print('  legal: LICENSE, NOTICE-JJWXC.md, THIRD_PARTY_NOTICES.md, LICENSE-*.txt')

    # Build the archive first: the sums must describe the zip as shipped, and a
    # leftover SHA256SUMS from an earlier run must not be archived (it would be a
    # file whose own hash is listed).
    package = output / f'cpyos-readpico-{version}-full.zip'
    stale_sums = output / 'SHA256SUMS'
    if stale_sums.exists():
        stale_sums.unlink()
    with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(output.iterdir()):
            if path.is_file() and path.name not in {package.name}:
                archive.write(path, arcname=path.name)
        if not full_install_only:
            archive.write(output / 'ota' / 'readpico.json', arcname='ota/readpico.json')

    sums = []
    for path in sorted(output.iterdir()):
        if path.is_file() and path.name not in {'SHA256SUMS'}:
            sums.append(f'{sha256_file(path)}  {path.name}')
    (output / 'SHA256SUMS').write_text('\n'.join(sums) + '\n', encoding='utf-8')

    print(f'Release {PRODUCT} {version} -> {output}')
    print(f'  install parts: {len(install_parts)}, upgrade parts: {len(update_parts)}')
    if full_install_only:
        print(f'  full install only; app {firmware_asset["size"]} B')
    else:
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
    parser.add_argument('--full-install-only', action='store_true',
                        help='emit the full-install bundle but no OTA/upgrade manifest (layout transition)')
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
    build(args.version, root, build_dir, output, notes, args.pages_base, args.allow_dirty,
          args.full_install_only)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
