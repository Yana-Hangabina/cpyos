#!/usr/bin/env python3
"""Emit a browser-flashable bundle (esp-web-tools) for one target.

docs/07 F1: the first contact with a new device is a flashing page, not a
toolchain. This writes that page next to the four install segments, so a user
with a USB cable and Chrome can install the firmware without PlatformIO. It
touches no firmware and costs no flash: the artifact is static files.

Nothing here invents an offset. The parts come from
package_nightly_target.FULL_INSTALL_SEGMENTS, which is the same list the
full-install package uses, and the slot check reads the target's own partition
table -- the same sources `verify_release_artifacts.py` audits.

Usage:
  python3 scripts/package_nightly_target.py readpico --channel nightly --output dist/readpico
  python3 scripts/build_web_flasher.py readpico --dist dist/readpico --output dist/web/readpico

Notes for whoever hosts this:
  * Web Serial requires a secure context (https:// or http://localhost). A page
    opened from file:// cannot talk to the port.
  * esp-web-tools itself is loaded from a CDN, so the page needs network access
    on first load. Vendoring it is a deliberate non-goal here: the bundle stays
    a handful of kilobytes of HTML plus the four binaries.
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import nightly_targets  # noqa: E402
import package_nightly_target  # noqa: E402

ESP_WEB_TOOLS_VERSION = '10'
MANIFEST_NAME = 'manifest.json'
PAGE_NAME = 'index.html'

PAGE_TEMPLATE = '''<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{product} 固件刷写 / Firmware install</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ font-family: -apple-system, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
         max-width: 42rem; margin: 3rem auto; padding: 0 1.25rem; line-height: 1.6; }}
  h1 {{ font-size: 1.5rem; margin-bottom: .25rem; }}
  .version {{ color: #666; font-family: ui-monospace, Menlo, monospace; font-size: .875rem; }}
  .card {{ border: 1px solid #ccc; border-radius: .75rem; padding: 1.25rem; margin: 1.5rem 0; }}
  ol {{ padding-left: 1.25rem; }}
  code {{ font-family: ui-monospace, Menlo, monospace; }}
  .note {{ font-size: .9rem; color: #555; }}
  esp-web-install-button {{ display: block; margin-top: 1rem; }}
</style>
<script type="module" src="https://unpkg.com/esp-web-tools@{esp_web_tools}/dist/web/install-button.js?module"></script>
</head>
<body>
<h1>{product} 固件刷写</h1>
<div class="version">{version} · {chip}</div>

<div class="card">
  <ol>
    <li>用 USB-C 线把设备连到电脑（设备会以 <code>{port_hint}</code> 出现）。</li>
    <li>用 Chrome 或 Edge 打开本页，点下面的按钮，选择那个串口。</li>
    <li>等待写入完成，设备会自动重启。</li>
  </ol>
  <esp-web-install-button manifest="{manifest}">
    <button slot="activate">连接设备并刷写 / Connect and install</button>
    <span slot="unsupported">这个浏览器不支持 Web Serial，请用桌面版 Chrome/Edge。<br>
      This browser has no Web Serial support; use desktop Chrome or Edge.</span>
    <span slot="not-allowed">页面必须是 https 或 localhost（file:// 不行）。<br>
      The page must be served over https or from localhost.</span>
  </esp-web-install-button>
  <p class="note">写入的是完整安装包（bootloader + 分区表 + 应用），不会动 NVS 里的设置，也不会整片擦除。<br>
    This installs bootloader, partition table and the app; it does not erase NVS or the whole chip.</p>
</div>

<p class="note">
  SHA-256 (firmware): <code>{firmware_sha}</code><br>
  没有浏览器刷机条件时，也可以把固件放到 TF 卡根目录，用设备上的「SD 卡固件更新」。
</p>
</body>
</html>
'''

PRODUCT_NAMES = {
    'readpico': 'Read Pico',
}


def fail(message):
    print('error: ' + message, file=sys.stderr)
    raise SystemExit(1)


def sha256_file(path):
    return package_nightly_target.sha256(path)


def build(target_id, dist_dir, output_dir, root=None, manifest_name=MANIFEST_NAME, page_name=PAGE_NAME):
    if target_id not in nightly_targets.TARGETS:
        fail(f'unknown target {target_id!r}; known: {sorted(nightly_targets.TARGETS)}')
    target = nightly_targets.TARGETS[target_id]
    root = Path(root) if root is not None else Path(__file__).resolve().parent.parent
    partitions = package_nightly_target.verify_partition_csv(
        root, target.get('partitionsCsv', 'partitions.csv'))

    # The same segment list the full-install package uses: a page that offered a
    # different set would flash a device into a layout its own table disagrees with.
    parts = []
    copied = []
    for role, source_name, offset in package_nightly_target.FULL_INSTALL_SEGMENTS:
        asset = dist_dir / nightly_targets.asset_name(target_id, source_name)
        if not asset.is_file():
            fail(f'{asset} is missing; run scripts/package_nightly_target.py {target_id} first')
        parts.append({'path': asset.name, 'offset': offset})
        copied.append((asset, output_dir / asset.name))

    firmware = dist_dir / nightly_targets.asset_name(target_id, 'firmware.bin')
    firmware_size = firmware.stat().st_size
    slot = partitions['app0'][1]
    if firmware_size > slot:
        fail(f'{firmware.name} is {firmware_size} bytes but the app0 slot is {slot}')

    version = 'unknown'
    for flavor in nightly_targets.FLAVOR_TOKENS:
        manifest = dist_dir / nightly_targets.manifest_name(target_id, flavor)
        if manifest.is_file():
            version = json.loads(manifest.read_text()).get('version', version)
            break

    chip_family = target['chip']
    manifest = {
        'name': f"CrossMux {PRODUCT_NAMES.get(target_id, target_id)}",
        'version': version,
        'new_install_prompt_erase': True,
        'builds': [{'chipFamily': chip_family, 'parts': parts}],
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    for source, destination in copied:
        shutil.copyfile(source, destination)
    (output_dir / manifest_name).write_text(json.dumps(manifest, indent=2) + '\n')
    (output_dir / page_name).write_text(PAGE_TEMPLATE.format(
        product=f"CrossMux {PRODUCT_NAMES.get(target_id, target_id)}",
        version=version,
        chip=chip_family,
        esp_web_tools=ESP_WEB_TOOLS_VERSION,
        port_hint='USB JTAG/serial debug unit',
        manifest=manifest_name,
        firmware_sha=sha256_file(firmware),
    ))
    return manifest, output_dir


def main(argv=None):
    parser = argparse.ArgumentParser(description='Build an esp-web-tools flashing bundle.')
    parser.add_argument('target', help='target id from scripts/nightly_targets.py')
    parser.add_argument('--dist', required=True, help='packaged output directory (four install segments)')
    parser.add_argument('--output', required=True, help='directory to write the web bundle into')
    args = parser.parse_args(argv)

    manifest, output = build(args.target, Path(args.dist), Path(args.output))
    parts = manifest['builds'][0]['parts']
    print(f"Web flasher for {args.target} ({manifest['version']}) -> {output}")
    for part in parts:
        print(f"  {part['path']} @ 0x{part['offset']:05x}")
    print(f'  {PAGE_NAME} + {MANIFEST_NAME} (esp-web-tools {ESP_WEB_TOOLS_VERSION})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
