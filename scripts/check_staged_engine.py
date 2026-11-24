#!/usr/bin/env python3
"""Confirm a staged site/pdf-engine* channel matches the built firmware.

The manifest is a rolling PDFENG2 index; a channel is valid for this build when
it contains an entry for the build's compatibility class, that entry describes
the staged blob, and the blob was linked from this firmware's ELF (build id).
The failure this catches is invisible on every transport: a stale engine has the
same size and downloads happily, and only the device refuses it afterwards.
"""
import hashlib
import pathlib
import re
import struct
import sys

ELF = pathlib.Path(sys.argv[1])
SITE = pathlib.Path(sys.argv[2])
channels = sys.argv[3:] or ["pdf-engine"]
ROOT = pathlib.Path(__file__).resolve().parents[1]
API = ROOT / "src/pdf/PdfModuleApi.h"
HOST = ROOT / "src/pdf/PdfModuleHost.h"


def header_constant(name: str) -> int:
    for path in (API, HOST):
        match = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", path.read_text())
        if match:
            return int(match.group(1), 0)
    raise SystemExit(f"{name} not found in the module headers")


def layout_key(sd: bool) -> int:
    keys = re.findall(r"kPdfModuleLayoutKey\s*=\s*(0x[0-9A-Fa-f]+)u;", API.read_text())
    if len(keys) < 2:
        raise SystemExit("kPdfModuleLayoutKey literals not found")
    return int(keys[0 if sd else 1], 16)


elf = ELF.read_bytes()
want_build = hashlib.sha256(elf).hexdigest()
image = pathlib.Path(str(ELF).replace("firmware.elf", "firmware.bin"))
in_image = image.read_bytes()[0xB0:0xB0 + 32].hex() if image.exists() else None
want_magic = header_constant("kPdfModuleMagic")
want_version = header_constant("kPdfModuleVersion")
want_abi = header_constant("kPdfModuleAbiVersion")
want_major = header_constant("kPdfModuleHostApiMajor")
want_minor = header_constant("kPdfModuleHostApiMinor")
bad = 0
for channel in channels:
    root = SITE / channel
    sd = "plus" in channel
    want_layout = layout_key(sd)
    try:
        lines = [ln.strip() for ln in (root / "manifest-v2.txt").read_text().splitlines() if ln.strip()]
    except OSError as exc:
        print(f"{channel}: cannot read manifest-v2.txt: {exc}")
        bad += 1
        continue
    if not lines or lines[0] != "PDFENG2":
        print(f"{channel}: manifest is not a PDFENG2 index")
        bad += 1
        continue
    selected = None
    for line in lines[1:]:
        f = line.split("\t")
        if len(f) == 9 and int(f[3]) == want_abi and int(f[4]) == want_major \
                and int(f[5]) <= want_minor and int(f[6], 16) == want_layout:
            selected = f
            break
    reasons = []
    if selected is None:
        reasons.append("no manifest entry for this compatibility class")
        blob = b""
        version = abi = 0
    else:
        url, size, sha, _, _, _, _, rev, build = selected
        name = url.rsplit("/", 1)[-1]
        path = root / name
        if not path.is_file():
            path = root / "pdfmod.bin"
        blob = path.read_bytes() if path.is_file() else b""
        magic, version, abi, host_api, layout = struct.unpack_from("<IHHII", blob.ljust(16, b"\0"), 0)
        if len(blob) != int(size):
            reasons.append(f"size {size} != blob {len(blob)}")
        if hashlib.sha256(blob).hexdigest() != sha:
            reasons.append("sha256 does not match the staged blob")
        if magic != want_magic or version != want_version:
            reasons.append("blob is not this format version")
        if abi != want_abi or (host_api >> 16) != want_major \
                or (host_api & 0xFFFF) > want_minor or layout != want_layout:
            reasons.append("blob compatibility class differs from the manifest entry")
        if int.from_bytes(blob[64:68], "little") != int(rev):
            reasons.append("blob engineRev differs from the manifest")
        if blob[68:100].hex() != build:
            reasons.append("manifest build id differs from the blob header")
        if build != want_build:
            reasons.append(f"build id != ELF sha256 ({want_build[:12]}..)")
        if in_image is not None and build != in_image:
            reasons.append(f"build id != the app image's 0xB0 field ({in_image[:12]}..)")
    status = "pairs" if not reasons else "REFUSED: " + "; ".join(reasons)
    print(f"{channel}: {len(blob)} bytes, v{version} abi={abi} -> {status}")
    bad += bool(reasons)
sys.exit(1 if bad else 0)
