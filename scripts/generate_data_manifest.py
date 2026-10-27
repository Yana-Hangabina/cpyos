#!/usr/bin/env python3
"""Emit build/data_manifest.txt: the single generated description of what lives in
the data/spiffs partition and at which flash address.

The partition map used to be written down four times -- a comment and a constant in
HyphenationStore.h, a guard in generate_hyphenation_db.py, the offsets in
exp/flash_device.sh, and a table in AGENTS.md -- and four copies of a fact drift.
This script makes the map a build artifact instead, so the flashing tool reads it
rather than remembering it.

Nothing here owns a number it could get wrong:

  - the partition's flash base and size are parsed from partitions.csv, so the
    manifest cannot disagree with the partition table;
  - the hyphenation offset is read out of HyphenationStore.h, so the C++ loader
    stays the single source of truth and this script cannot invent a second one;
  - the language-pack offset is read out of lib/I18n/I18nPack.h for the same
    reason: CROSSPOINT_I18N_PACK builds read it from there;
  - the web asset pack offset is read out of src/network/WebPackFormat.h: Read
    Pico's web server reads its pages from that pack, so a readpico image with
    no pack on the partition serves recovery pages instead of the file browser.
    That makes the pack required, not optional: the manifest fails without it
    rather than quietly describing a partition that cannot work;
  - Pinyin owns partition offset 0 by definition (it was there first and its loader
    reads from 0 unconditionally).

What it does own is the checks, and they are the reason to run it rather than flash
by hand: no two blobs overlap, nothing runs past the partition, and the reserved gap
at 0x80000 is left alone. That gap holds twelve bytes which are not 0xFF in the
factory image, which nothing in this tree reads, and whose purpose nobody knows.
Overwriting them is not reversible, so the manifest refuses to describe a layout
that would.

Usage:
  python3 scripts/generate_data_manifest.py            # -> build/data_manifest.txt
  python3 scripts/generate_data_manifest.py --check    # verify an existing manifest
"""

import argparse
import hashlib
import os
import re
import sys

# Read Pico is the only target whose flash script writes the data partition, and it
# has its own table (7.19 MiB slots, 1.5 MiB spiffs). Reading the shared
# partitions.csv here would describe a partition this target does not have.
PARTITIONS_CSV = "partitions_readpico.csv"
STORE_HEADER = "lib/Epub/Epub/hyphenation/HyphenationStore.h"
PINYIN_HEADER = "lib/Pinyin/PinyinDb.h"
I18N_PACK_HEADER = "lib/I18n/I18nPack.h"
WEB_PACK_HEADER = "src/network/WebPackFormat.h"

# Pinyin's loader reads from partition offset 0 unconditionally (PinyinDb.cpp:96),
# so that is not a choice this script gets to make.
PINYIN_PARTITION_OFFSET = 0

# Twelve bytes at this partition offset are not 0xFF in the factory image and nothing
# reads them. See AGENTS.md §5. The gap is wide enough that nobody has to think about
# where the reserved bytes end.
RESERVED_PARTITION_OFFSET = 0x80000
RESERVED_BYTES = 12


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    raise SystemExit(1)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_spiffs_partition(root):
    """Return (flash_base, size) for the data/spiffs partition, from partitions.csv."""
    path = os.path.join(root, PARTITIONS_CSV)
    if not os.path.exists(path):
        fail("no %s" % path)
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            fields = [f.strip() for f in line.split(",")]
            if len(fields) >= 5 and fields[1] == "data" and fields[2] == "spiffs":
                try:
                    return int(fields[3], 0), int(fields[4], 0)
                except ValueError:
                    fail("cannot parse the spiffs row of %s: %r" % (PARTITIONS_CSV, line))
    fail("no data/spiffs row in %s" % PARTITIONS_CSV)


def read_cpp_constant(path, name):
    """Read an `inline constexpr ... name = 0xNNN;` out of a header.

    Deliberately a text read rather than a compiled probe: the equivalence test
    compiles the header and compares the result against the manifest, so a mis-parse
    here is caught by a test rather than trusted.
    """
    pattern = re.compile(r"\b%s\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)u?\s*;" % re.escape(name))
    with open(path, "r", encoding="utf-8") as fh:
        match = pattern.search(fh.read())
    if not match:
        fail("cannot find %s in %s" % (name, path))
    return int(match.group(1), 0)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect(root, base, partition_size, hyphen_offset, i18n_offset, web_offset):
    """Build the row list from whatever data blobs actually exist.

    pinyin/hyphen/i18n degrade to a note when absent. The web pack does not:
    Read Pico holds no embedded pages, so an image flashed with an unflashed
    partition has a file-transfer page that cannot work. Refusing to describe
    that layout is the point of having a manifest.
    """
    candidates = [
        ("pinyin", PINYIN_PARTITION_OFFSET, os.path.join(root, "build", "pinyin.dat"), False),
        ("hyphen", hyphen_offset, os.path.join(root, "build", "hyphen.dat"), False),
        ("i18n", i18n_offset, os.path.join(root, "build", "i18n_pack.dat"), False),
        ("web", web_offset, os.path.join(root, "build", "web_pack.dat"), True),
    ]
    rows = []
    for name, offset, path, required in candidates:
        if not os.path.exists(path):
            if required:
                fail("%s is absent (%s). Read Pico's web server reads its pages from that pack; "
                     "run scripts/build_web_pack.py (a normal build writes it) or pass --skip-data "
                     "to flash the firmware alone." % (name, path))
            print("note: %s is absent (%s); it will not be flashed" % (name, path))
            continue
        size = os.path.getsize(path)
        rows.append({
            "name": name,
            "offset": offset,
            "address": base + offset,
            "size": size,
            "sha256": sha256(path),
            "path": os.path.relpath(path, root),
        })
    if not rows:
        fail("no data blobs found in build/; generate them first")

    rows.sort(key=lambda r: r["offset"])
    for row in rows:
        end = row["offset"] + row["size"]
        if end > partition_size:
            fail("%s ends at partition offset 0x%x, past the partition's 0x%x"
                 % (row["name"], end, partition_size))
        reserved_start = RESERVED_PARTITION_OFFSET
        reserved_end = reserved_start + RESERVED_BYTES
        if row["offset"] < reserved_end and end > reserved_start:
            fail("%s (0x%x..0x%x) would overwrite the %d reserved factory bytes at "
                 "partition offset 0x%x. Nothing in this tree reads them and their "
                 "purpose is unknown, so move the blob instead."
                 % (row["name"], row["offset"], end, RESERVED_BYTES, reserved_start))
    for earlier, later in zip(rows, rows[1:]):
        if earlier["offset"] + earlier["size"] > later["offset"]:
            fail("%s (0x%x + %d) overlaps %s (0x%x)"
                 % (earlier["name"], earlier["offset"], earlier["size"], later["name"], later["offset"]))
    return rows


def render(base, partition_size, rows):
    out = [
        "# Generated by scripts/generate_data_manifest.py -- do not edit.",
        "# What lives in the data/spiffs partition and where esptool must write it.",
        "# Offsets are partition-relative; addresses are absolute flash addresses.",
        "# Read by exp/flash_device.sh, and checked against the compiled",
        "# HYPN_PARTITION_OFFSET by src/run_hyphenation_test.sh.",
        "partition\tspiffs\t0x%08x\t0x%08x" % (base, partition_size),
        "reserved\t0x%08x\t%d\tfactory bytes of unknown purpose; never write here"
        % (RESERVED_PARTITION_OFFSET, RESERVED_BYTES),
    ]
    for row in rows:
        out.append("data\t%s\t0x%08x\t0x%08x\t%d\t%s\t%s"
                   % (row["name"], row["offset"], row["address"], row["size"], row["sha256"], row["path"]))
    return "\n".join(out) + "\n"


def check(path, expected):
    if not os.path.exists(path):
        fail("%s does not exist" % path)
    with open(path, "r", encoding="utf-8") as fh:
        actual = fh.read()
    if actual != expected:
        fail("%s is stale; re-run scripts/generate_data_manifest.py" % path)
    print("%s is current" % path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=repo_root(), help="crossmux repository root (default: %(default)s)")
    parser.add_argument("--out", default=None, help="output path (default: <repo>/build/data_manifest.txt)")
    parser.add_argument("--check", action="store_true", help="fail if the existing manifest is stale")
    args = parser.parse_args()

    base, partition_size = read_spiffs_partition(args.repo)
    hyphen_offset = read_cpp_constant(os.path.join(args.repo, STORE_HEADER), "HYPN_PARTITION_OFFSET")
    if hyphen_offset <= PINYIN_PARTITION_OFFSET:
        fail("HyphenationStore.h puts hyphenation at partition offset 0x%x, which is where Pinyin lives"
             % hyphen_offset)
    i18n_offset = read_cpp_constant(os.path.join(args.repo, I18N_PACK_HEADER), "I18N_PACK_PARTITION_OFFSET")
    if i18n_offset <= hyphen_offset:
        fail("I18nPack.h puts the language pack at partition offset 0x%x, which overlaps hyphenation (0x%x)"
             % (i18n_offset, hyphen_offset))
    web_offset = read_cpp_constant(os.path.join(args.repo, WEB_PACK_HEADER), "WEB_PACK_PARTITION_OFFSET")
    # No ordering assumption between the web pack and the language pack: the web
    # pack is a fixed ~158 KB while the language pack grows with every new i18n key,
    # so the language pack sits last and the web pack lives in the gap before the
    # reserved factory bytes. collect() is what proves they do not overlap.
    if web_offset == i18n_offset:
        fail("WebPackFormat.h and I18nPack.h both claim partition offset 0x%x" % web_offset)
    rows = collect(args.repo, base, partition_size, hyphen_offset, i18n_offset, web_offset)
    text = render(base, partition_size, rows)

    out = args.out or os.path.join(args.repo, "build", "data_manifest.txt")
    if args.check:
        check(out, text)
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(text)

    print("%s: partition spiffs at 0x%x (%d bytes), %d blob(s)" % (out, base, partition_size, len(rows)))
    for row in rows:
        print("  %-7s partition 0x%06x -> flash 0x%06x  %8d bytes  sha256:%s"
              % (row["name"], row["offset"], row["address"], row["size"], row["sha256"][:12]))
    free = partition_size - max(r["offset"] + r["size"] for r in rows)
    print("  reserved 0x%06x..0x%06x untouched; %d bytes of partition left free"
          % (RESERVED_PARTITION_OFFSET, RESERVED_PARTITION_OFFSET + RESERVED_BYTES, free))


if __name__ == "__main__":
    main()
