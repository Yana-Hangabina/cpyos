#!/usr/bin/env python3
"""Build the native-plugin catalog TSV from packaged ELF files.

    python3 scripts/build_plugin_catalog.py --out plugins/catalog/manifest-v1.txt \
      --base-url https://yana-hangabina.github.io/cpyos/plugins \
      --artifact 2048:2048:1.0.0:riscv32:plugins/native/2048/dist/plugin.elf \
      --artifact 2048:2048:1.0.0:xtensa:plugins/native/2048/dist-xtensa/plugin.elf

Each artifact is `id:name:version:arch:path`; the URL is
`<base-url>/<id>-<arch>.elf`. The device verifies size and SHA-256 before
installing, and uses the same TSV for its checked-in local catalog fallback.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--artifact", action="append", default=[], metavar="ID:NAME:VERSION:ARCH:PATH")
    args = parser.parse_args()
    lines = ["PEXT1"]
    for spec in args.artifact:
        parts = spec.split(":", 4)
        if len(parts) != 5:
            raise SystemExit(f"bad artifact spec: {spec}")
        pid, name, version, arch, path_text = parts
        if arch not in ("riscv32", "xtensa"):
            raise SystemExit(f"unsupported arch in {spec}")
        path = pathlib.Path(path_text)
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        url = f"{args.base_url.rstrip('/')}/{pid}-{arch}.elf"
        lines.append("\t".join([pid, name, version, "1", arch, str(len(data)), digest, url, "CPYOS native plugin"]))
    if len(lines) == 1:
        raise SystemExit("at least one --artifact is required")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {args.out} ({len(lines) - 1} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
