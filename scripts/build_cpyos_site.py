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
import json
import shutil
import sys
from pathlib import Path

REQUIRED_MANIFESTS = ('manifest-install.json', 'manifest-upgrade.json', 'manifest-app.json')


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
    for name in REQUIRED_MANIFESTS:
        check_manifest(release_dir, name)

    ota = load(release_dir / 'ota' / 'readpico.json')
    firmware_url = ota.get('assets', [{}])[0].get('browser_download_url', '')
    firmware_name = latest['firmware'].get('filename', 'readpico-firmware.bin')
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
    (output / 'ota').mkdir(exist_ok=True)
    shutil.copy2(release_dir / 'ota' / 'readpico.json', output / 'ota' / 'readpico.json')
    shutil.copy2(release_dir / 'latest.json', output / 'latest.json')

    print(f'Pages artifact -> {output}')
    print(f'  release {version}: {copied} files under downloads/{version}')
    print(f'  OTA manifest: /ota/readpico.json')
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
