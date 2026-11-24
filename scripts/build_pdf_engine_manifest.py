#!/usr/bin/env python3
"""Build or merge the PDF engine update manifest (PDFENG2).

The manifest is a rolling index, not a single release pointer.  Each line
describes one published engine image and its compatibility class:

  url<TAB>size<TAB>sha256<TAB>engineAbi<TAB>hostMajor<TAB>hostMinor<TAB>layoutKey<TAB>engineRev<TAB>buildId

A device downloads only the newest line whose engineAbi/hostApi/layoutKey match
its own build.  Keeping old-class lines is what lets a device that was flashed
with older firmware still update to the newest engine ever built for it.

  python3 scripts/build_pdf_engine_manifest.py \
      --firmware-elf .pio/build/readpico_pdf/firmware.elf \
      --pdfmod build/pdfmod.bin \
      --url https://github.com/OWNER/cpyos/releases/download/TAG/readpico-pdfmod.bin \
      --out build/pdf-engine-manifest.txt

  # publish: add this build to the rolling Pages index, keeping older classes
  python3 scripts/build_pdf_engine_manifest.py ... \
      --merge-into site/pdf-engine/manifest-v2.txt \
      --out site/pdf-engine/manifest-v2.txt
"""
import argparse
import hashlib
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "src", "pdf", "PdfModuleApi.h")
HOST_HEADER = os.path.join(ROOT, "src", "pdf", "PdfModuleHost.h")
MANIFEST_HEADER = "PDFENG2"


def sha256_file(path: str) -> bytes:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.digest()


def header_int(name: str) -> int:
    for path in (HEADER, HOST_HEADER):
        with open(path, encoding="utf-8") as f:
            m = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", f.read())
        if m:
            return int(m.group(1), 0)
    raise SystemExit(f"{name} not found in the module headers")


def parse_manifest(text: str):
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines or lines[0] != MANIFEST_HEADER:
        raise SystemExit(f"existing manifest is not {MANIFEST_HEADER}")
    return lines[1:]


def compat_key(line: str):
    f = line.split("\t")
    if len(f) != 9:
        raise SystemExit("existing manifest line does not have 9 fields: " + line)
    return (int(f[3]), int(f[4]), int(f[5]), int(f[6], 16))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware-elf", required=True)
    ap.add_argument("--pdfmod", required=True)
    ap.add_argument("--url", required=True, help="where the device downloads pdfmod.bin (https)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "pdf-engine-manifest.txt"))
    ap.add_argument("--merge-into", default="",
                    help="existing rolling manifest; replaced entries keep every other compatibility class")
    ap.add_argument("--engine-rev", type=int, default=-1,
                    help="monotonic revision; defaults to the value stamped in the image header")
    ap.add_argument("--check", action="store_true",
                    help="do not write: verify --out already equals the manifest this build produces")
    args = ap.parse_args()

    for p in (args.firmware_elf, args.pdfmod):
        if not os.path.isfile(p):
            print(f"missing input: {p}", file=sys.stderr)
            return 2
    if not args.url.startswith("https://"):
        print("url must be https (the transport is wolfSSL)", file=sys.stderr)
        return 2

    size = os.path.getsize(args.pdfmod)
    if size < 512 or size > 4 * 1024 * 1024:
        print(f"pdfmod.bin size {size} out of range", file=sys.stderr)
        return 2

    with open(args.pdfmod, "rb") as f:
        hdr = f.read(100)
    if len(hdr) < 100:
        print("pdfmod header is short", file=sys.stderr)
        return 2
    magic, version, abi, host_api, layout_key = struct.unpack_from("<IHHII", hdr, 0)
    engine_rev = struct.unpack_from("<I", hdr, 64)[0]
    blob_build = hdr[68:100].hex()

    want_version = header_int("kPdfModuleVersion")
    want_abi = header_int("kPdfModuleAbiVersion")
    want_major = header_int("kPdfModuleHostApiMajor")
    want_minor = header_int("kPdfModuleHostApiMinor")
    if magic != header_int("kPdfModuleMagic") or version != want_version or abi != want_abi:
        print("pdfmod header does not carry this tree's magic/version/ABI", file=sys.stderr)
        return 1
    if (host_api >> 16) != want_major or (host_api & 0xFFFF) > want_minor:
        print("pdfmod host API is not compatible with this tree", file=sys.stderr)
        return 1

    elf_sha = sha256_file(args.firmware_elf).hex()
    if blob_build != elf_sha:
        print(f"blob buildId {blob_build} != sha256(firmware.elf) {elf_sha}; rebuild the module",
              file=sys.stderr)
        return 1
    if args.engine_rev >= 0 and args.engine_rev != engine_rev:
        print(f"blob header carries rev {engine_rev}, but --engine-rev is {args.engine_rev}",
              file=sys.stderr)
        return 1

    blob_sha = sha256_file(args.pdfmod).hex()
    entry = ("%s\t%d\t%s\t%d\t%d\t%d\t0x%08X\t%d\t%s\n" %
             (args.url, size, blob_sha, abi, host_api >> 16, host_api & 0xFFFF, layout_key,
              engine_rev, blob_build))

    kept = []
    if args.merge_into:
        if not os.path.isfile(args.merge_into):
            print(f"--merge-into file does not exist: {args.merge_into}", file=sys.stderr)
            return 2
        key = compat_key(entry)
        for line in parse_manifest(open(args.merge_into, encoding="utf-8").read()):
            if compat_key(line) == key:
                continue  # this class is replaced by the build being published
            kept.append(line)
    manifest = MANIFEST_HEADER + "\n" + entry + "".join(l + "\n" for l in kept)

    if args.check:
        try:
            current = open(args.out, encoding="utf-8").read()
        except OSError as exc:
            print(f"--check: cannot read {args.out}: {exc}", file=sys.stderr)
            return 1
        if current != manifest:
            print(f"--check: {args.out} does not match these inputs", file=sys.stderr)
            return 1
        print("check ok:", args.out)
        return 0

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(manifest)
    print("wrote", args.out)
    print(entry.rstrip())
    print(f"  kept {len(kept)} other compatibility class(es)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
