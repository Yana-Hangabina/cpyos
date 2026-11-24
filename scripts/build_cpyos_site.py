#!/usr/bin/env python3
"""Assemble the GitHub Pages site from site/ plus one CPYOS release bundle.

The flasher must not depend on GitHub release redirects or CORS, so the release
binaries are copied into the Pages artifact and every manifest references a
sibling file. The function is deliberately dumb: it refuses to publish a
manifest whose parts are missing, or an OTA manifest whose firmware URL does not
match the copied file.

Usage:
  python3 scripts/build_cpyos_release.py --version 0.1.0
  python3 scripts/build_cpyos_site.py --release-dir dist/cpyos/0.1.0 --output _site
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

REQUIRED_MANIFESTS = ('manifest-install.json', 'manifest-upgrade.json', 'manifest-app.json')
# A layout-transition release publishes only the full-install manifest; OTA
# manifests are deliberately absent so an old device cannot download an image
# written for a different partition table.
OTA_MANIFESTS = ('manifest-upgrade.json', 'manifest-app.json')


def fail(message: str) -> None:
    print(f'error: {message}', file=sys.stderr)
    raise SystemExit(1)


def load(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as error:
        fail(f'cannot read {path}: {error}')


def check_manifest(release_dir: Path, name: str) -> None:
    manifest = load(release_dir / name)
    builds = manifest.get('builds')
    if not isinstance(builds, list) or len(builds) != 1:
        fail(f'{name} must contain exactly one build')
    parts = builds[0].get('parts')
    if not isinstance(parts, list) or not parts:
        fail(f'{name} has no parts')
    for part in parts:
        path = part.get('path')
        if not isinstance(path, str) or '/' in path:
            fail(f'{name} has a non-local part path: {path!r}')
        if not (release_dir / path).is_file():
            fail(f'{name} references {path}, which is not in {release_dir}')


def firmware_is_xip_build(firmware: Path) -> bool:
    """True when this app image carries the XIP engine loader.

    Basis: `PdfModuleRuntime.cpp` is entirely inside `#if defined(CROSSPOINT_PDF_XIP)`
    and holds a log line no other translation refers to, so it cannot reach a normal
    image; confirmed present in a real `readpico_pdf` build.

    ★ Do NOT key this on '/pdfmod.bin'.  That string looks like a better marker and is
    a trap: it is the text of STR_PDF_ENGINE_INSTALL ("From /pdfmod.bin?"), so it also
    ships inside the embedded EN/ZH translation blob -- in *every* firmware, XIP or
    not.  Keying on it would classify ordinary releases as PDF builds and then refuse
    them for having no pdf-engine directory.  `test_translation_text_is_not_an_xip_marker`
    pins that.
    """
    try:
        blob = firmware.read_bytes()
    except OSError:
        return False
    return b'refusing the XIP engine' in blob


def check_pdf_engine(output: Path, firmware: Path | None = None) -> None:
    """Verify the rolling engine index that ships beside the site.

    The device reads `manifest-v2.txt` (PDFENG2) and picks the newest entry for
    its own compatibility class.  A fresh manifest beside a stale blob -- or one
    of the two simply forgotten -- would make 检查更新 fail on every device.

    `firmware` is the app image published in the same release.  An engine must
    have been linked from that ELF (build id); runtime compatibility is still the
    engineAbi/hostApi/layoutKey class, but publishing an engine from another
    build is a packaging mistake worth catching here.
    """
    engine = output / 'pdf-engine'
    if not engine.is_dir():
        if firmware is not None and firmware_is_xip_build(firmware):
            fail(f'{firmware} is an XIP PDF build (it carries the engine loader) but '
                 f'{engine} does not exist: stage the paired engine and its index -- '
                 f'scripts/build_pdf_module.py, scripts/build_pdf_engine_manifest.py, then '
                 f'copy both into site/pdf-engine/ (recipe in '
                 f'docs/engineering/pdf-xip.md "Publishing an engine build")')
        return
    manifest = engine / 'manifest-v2.txt'
    if not manifest.is_file():
        fail(f'{engine} exists without manifest-v2.txt')
    lines = [line.strip() for line in manifest.read_text(encoding='utf-8').splitlines() if line.strip()]
    if not lines or lines[0] != 'PDFENG2':
        fail(f'{manifest} is not a PDFENG2 index')
    if firmware is None or not firmware.is_file():
        fail(f'pdf-engine/ is staged but the release firmware {firmware} is missing; '
             f'cannot verify the engine/app build pairing')
    image = firmware.read_bytes()
    if len(image) < 0xd0:
        fail(f'{firmware} is too short to carry an ESP app ELF SHA at 0xb0')
    build_id = image[0xB0:0xD0].hex()

    api = (Path(__file__).resolve().parent.parent / 'src/pdf/PdfModuleApi.h').read_text()
    host = (Path(__file__).resolve().parent.parent / 'src/pdf/PdfModuleHost.h').read_text()

    def const(name: str, text: str) -> int:
        match = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", text)
        if not match:
            fail(f'{name} not found in the module headers')
        return int(match.group(1), 0)

    want_abi = const('kPdfModuleAbiVersion', api)
    want_major = const('kPdfModuleHostApiMajor', host)
    want_minor = const('kPdfModuleHostApiMinor', host)
    keys = re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;", api)
    if len(keys) < 2:
        fail('kPdfModuleLayoutKey literals not found')
    want_layout = int(keys[1], 16)  # readpico flash mapping

    selected = None
    for line in lines[1:]:
        fields = line.split('\t')
        if len(fields) != 9:
            fail(f'{manifest}: expected 9 TAB-separated fields, got {line!r}')
        try:
            abi = int(fields[3]); major = int(fields[4]); minor = int(fields[5])
            layout = int(fields[6], 16); rev = int(fields[7])
        except ValueError:
            fail(f'{manifest}: non-numeric compatibility field in {line!r}')
        if abi == want_abi and major == want_major and minor <= want_minor and layout == want_layout:
            selected = (fields, rev)
            break
    if selected is None:
        fail(f'{manifest} has no entry for this firmware (abi {want_abi}, '
             f'hostApi {want_major}.{want_minor}, layout 0x{want_layout:08X})')

    fields, rev = selected
    url, size_text, want_sha = fields[0], fields[1], fields[2]
    name = url.rsplit('/', 1)[-1]
    blob = engine / name
    if not blob.is_file():
        fail(f'{manifest} references {name}, which is not in {engine}')
    if size_text != str(blob.stat().st_size):
        fail(f'{manifest} says {name} is {size_text} B, the file is {blob.stat().st_size} B')
    blob_bytes = blob.read_bytes()
    actual = hashlib.sha256(blob_bytes).hexdigest()
    if actual != want_sha:
        fail(f'{manifest} sha256 for {name} is {want_sha}, the file hashes to {actual}')
    if len(blob_bytes) < 100:
        fail(f'{name} is too short to be a PDF engine image')
    blob_build = blob_bytes[68:100].hex()
    blob_rev = int.from_bytes(blob_bytes[64:68], 'little')
    if blob_rev != rev:
        fail(f'{name} carries rev {blob_rev} but the index says {rev}')
    if blob_build != fields[8]:
        fail(f'{name} build id does not match the index entry')
    if blob_build != build_id:
        fail(f'{name} was linked from another ELF (build id {blob_build[:12]}…, '
             f'{firmware.name} carries {build_id[:12]}…); rebuild and restage it')
    print(f'pdf-engine: {name} ({len(blob_bytes)} B, rev {rev}) matches '
          f'{firmware.name} and the {manifest.name} index')


def build(release_dir: Path, source: Path, output: Path, pages_base: str) -> dict:
    release_dir = release_dir.resolve()
    source = source.resolve()
    output = output.resolve()
    if not (source / 'index.html').is_file():
        fail(f'no {source / "index.html"}')
    latest = load(release_dir / 'latest.json')
    version = latest.get('version')
    if not isinstance(version, str) or not version:
        fail('latest.json has no version')
    ota_path = release_dir / 'ota' / 'readpico.json'
    firmware_name = latest['firmware'].get('filename', 'readpico-firmware.bin')
    for name in REQUIRED_MANIFESTS:
        if name in OTA_MANIFESTS and not ota_path.exists():
            continue
        check_manifest(release_dir, name)
    if ota_path.exists():
        ota = load(ota_path)
        firmware_url = ota.get('assets', [{}])[0].get('browser_download_url', '')
        # 0.1.1 and older advertise the Pages copy; 0.1.2+ deliberately point the
        # device at the GitHub release asset (mirror-prefixed), because Pages is slow
        # from mainland China. Accept either, but it must name THIS version's file.
        pages_suffix = f'/downloads/{version}/{firmware_name}'
        github_suffix = f'/releases/download/cpyos-{version}/{firmware_name}'
        if firmware_url and not (firmware_url.endswith(pages_suffix) or firmware_url.endswith(github_suffix)):
            fail(f'OTA firmware URL {firmware_url!r} ends with neither {pages_suffix!r} nor {github_suffix!r}')
        if latest['firmware'].get('sha256') != ota.get('assets', [{}])[0].get('sha256'):
            fail('latest.json and the OTA manifest disagree on the firmware sha256')

    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(source, output)
    (output / '.nojekyll').write_text('', encoding='utf-8')
    if ota_path.exists():
        check_pdf_engine(output, release_dir / firmware_name)
    else:
        print('  full-install release: engine is inside manifest-install.json; '
              'site engine updates use the rolling pdf-engine release')

    downloads = output / 'downloads' / version
    downloads.mkdir(parents=True, exist_ok=True)
    copied = 0
    for path in sorted(release_dir.iterdir()):
        if not path.is_file():
            continue
        if path.name in {'latest.json'} or path.suffix == '.zip':
            continue
        shutil.copy2(path, downloads / path.name)
        copied += 1
    if ota_path.exists():
        (output / 'ota').mkdir(exist_ok=True)
        shutil.copy2(ota_path, output / 'ota' / 'readpico.json')
        packs = release_dir / 'readpico-packs.txt'
        if packs.is_file():
            shutil.copy2(packs, output / 'ota' / 'readpico-packs.txt')
    shutil.copy2(release_dir / 'latest.json', output / 'latest.json')

    print(f'Pages artifact -> {output}')
    print(f'  release {version}: {copied} files under downloads/{version}')
    print('  OTA manifest: /ota/readpico.json' if ota_path.exists() else '  full-install release: no OTA manifest')
    return latest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Assemble the CPYOS GitHub Pages artifact.')
    parser.add_argument('--release-dir', required=True)
    parser.add_argument('--source', default=str(Path(__file__).resolve().parent.parent / 'site'))
    parser.add_argument('--output', default=str(Path(__file__).resolve().parent.parent / '_site'))
    parser.add_argument('--pages-base', default='https://yana-hangabina.github.io/cpyos')
    args = parser.parse_args(argv)
    build(Path(args.release_dir), Path(args.source), Path(args.output), args.pages_base)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
