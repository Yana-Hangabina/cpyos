#!/usr/bin/env python3
"""Build build/web_pack.dat: the six gzip'd web pages Read Pico serves.

Why a pack instead of the embedded `*.generated.h` arrays: the four pages plus
JSZip and the plugin runner are 157,876 bytes of gzip data, and app0 has only
a few hundred KB left. Read Pico reads them from the data/spiffs partition
(offset owned by src/network/WebPackFormat.h); every other target (C3, the
simulator, other S3 boards) keeps embedding them, because their flash scripts
do not write that partition.

Two outputs, on purpose:

  build/web_pack.dat                     what exp/flash_device.sh writes
  src/network/WebPackSchema.generated.h   compile-time schema signature so a
                                          stale pack (an OTA left an older
                                          one) is rejected whole instead of
                                          serving mismatched pages

Both are regenerated from the same `web_assets` compression that build_html.py
uses, so the pack bytes and the embedded arrays cannot drift.

The pack is reproducible: gzip mtime is 0 and entries are emitted in asset-id
order, so two runs on the same tree are byte-identical.

Usage:
  python3 scripts/build_web_pack.py
  python3 scripts/build_web_pack.py --out /tmp/web_pack.dat --schema-out /tmp/schema.h
"""

import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(globals().get("__file__", os.path.join("scripts", "build_web_pack.py"))))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import web_assets  # noqa: E402  (path setup above)


def write_if_changed(path: str, data: bytes, quiet: bool) -> None:
    """Write only when the bytes differ.

    PlatformIO runs this pre-script on every build. Rewriting an identical
    schema header would bump its mtime and make SCons recompile WebAssets.cpp
    (and everything including it) after a no-op change, so compare first.
    """
    if os.path.exists(path):
        with open(path, "rb") as fh:
            if fh.read() == data:
                if not quiet:
                    print(f"[web_pack] {path} is unchanged")
                return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "wb") as fh:
        fh.write(data)
    os.replace(temporary, path)


def generate(repo: str, out: str = None, schema_out: str = None, quiet: bool = False) -> int:
    out = out or os.path.join(repo, web_assets.PACK_DEFAULT_PATH)
    schema_out = schema_out or os.path.join(repo, web_assets.SCHEMA_HEADER_PATH)

    payloads = []
    for asset in web_assets.ASSETS:
        blob, original = web_assets.asset_payload(asset, repo)
        payloads.append((asset, blob))
        if not quiet:
            print(f"[web_pack] {asset.asset_id} {asset.source}: {original} -> {len(blob)} bytes")

    pack = web_assets.build_pack(payloads)
    write_if_changed(out, pack, quiet)

    signature = web_assets.signature_for(payloads)
    schema = web_assets.render_schema_header(signature, len(payloads), len(pack)).encode("utf-8")
    write_if_changed(schema_out, schema, quiet)

    if not quiet:
        print(f"[web_pack] {out}: {len(pack)} bytes, {len(payloads)} assets, "
              f"schema 0x{signature:016X}")
        print(f"[web_pack] {schema_out}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=None, help="crossmux repository root (default: next to this script)")
    parser.add_argument("--out", default=None, help=f"pack path (default: <repo>/{web_assets.PACK_DEFAULT_PATH})")
    parser.add_argument("--schema-out", default=None,
                        help=f"schema header path (default: <repo>/{web_assets.SCHEMA_HEADER_PATH})")
    parser.add_argument("--quiet", action="store_true", help="only print errors")
    args = parser.parse_args()
    return generate(args.repo or os.path.dirname(_HERE), args.out, args.schema_out, args.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
else:
    # SCons runs this file as a pre-script. Two traps live here:
    #   1. sys.argv still holds SCons's own command line, so calling main() (which
    #      parses argv) fails with "unrecognized arguments: -Q ...". Pass values
    #      explicitly instead.
    #   2. Raising SystemExit would make SCons treat a success as a reason to stop
    #      running the remaining pre-scripts. Exceptions still fail the build.
    if "Import" in globals():
        Import("env")
        generate(env.subst("$PROJECT_DIR") or os.getcwd(), quiet=True)
