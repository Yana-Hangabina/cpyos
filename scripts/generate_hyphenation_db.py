#!/usr/bin/env python3
"""Pack the eleven hyphenation tries into /hyphen.dat, the container the firmware
loads them from instead of compiling them into the image.

Input is the generated headers the firmware used to embed
(lib/Epub/Epub/hyphenation/generated/hyph-<lang>.trie.h), so the packed bytes are
identical to the .rodata they replace by construction -- and --verify asserts that
byte for byte rather than trusting the parser.

Output layout is HyphenationDataFormat.h: a 64-byte header, an 11 x 20-byte index,
then the blobs, each 4-byte aligned. CRC-32 is the reflected IEEE polynomial zlib
uses, over the header (with the crc field read as zero) plus the index for the
container, and per blob for the payload. The loader hashes only the blob it needs,
so an English book never pays for hashing the German trie.

Nothing here needs the network: update_hyphenation.sh is what regenerates the
headers from upstream hypher, and it is not pinned or checksummed, so re-deriving
them is a separate and less trustworthy step than repacking what is in the tree.

Usage:
  python3 scripts/generate_hyphenation_db.py                 # -> build/hyphen.dat
  python3 scripts/generate_hyphenation_db.py --verify        # re-check the file on disk
"""

import argparse
import os
import re
import struct
import sys
import zlib

MAGIC = b"HYPN"
VERSION = 1
HEADER_SIZE = 64
ENTRY_SIZE = 20
MAX_ENTRIES = 16

# Same order as entries() in LanguageRegistry.cpp. The loader looks languages up by
# tag, so the order only affects layout, but keeping the two lists in the same order
# makes a diff between them readable.
LANGUAGES = ["en", "fr", "de", "ru", "es", "it", "pl", "sv", "uk", "fi", "pt"]

# Where this container lands in the data/spiffs partition is NOT decided here: the
# loader's HYPN_PARTITION_OFFSET (HyphenationStore.h) is the single source of truth,
# and scripts/generate_data_manifest.py reads it out of that header, checks the layout
# for overlaps and for the reserved factory bytes at 0x80000, and emits the addresses
# that exp/flash_device.sh writes. This script only packs bytes.

ARRAY_RE = re.compile(
    r"constexpr\s+uint8_t\s+(\w+)_trie_data\[\]\s*=\s*\{(.*?)\}\s*;", re.S)
PATTERNS_RE = re.compile(
    r"constexpr\s+SerializedHyphenationPatterns\s+(\w+)_patterns\s*=\s*\{\s*(0x[0-9A-Fa-f]+|\d+)u?\s*,", re.S)


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    raise SystemExit(1)


def parse_trie_header(path, lang):
    """Return (rootOffset, blob bytes) for one generated header."""
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    array = ARRAY_RE.search(text)
    if not array or array.group(1) != lang:
        fail("%s: no %s_trie_data array" % (path, lang))
    # 0xHH tokens, one per byte. Splitting on commas and parsing each is faster and
    # stricter than findall over a 1.3 MB file: a token that is not a byte literal
    # means the generator changed shape and this packer must not guess.
    blob = bytearray()
    for token in array.group(2).split(","):
        token = token.strip()
        if not token:
            continue
        if not re.fullmatch(r"0[xX][0-9A-Fa-f]{1,2}", token):
            fail("%s: unexpected array token %r" % (path, token[:32]))
        blob.append(int(token, 16))
    if not blob:
        fail("%s: empty trie array" % path)

    patterns = PATTERNS_RE.search(text)
    if not patterns or patterns.group(1) != lang:
        fail("%s: no %s_patterns descriptor" % (path, lang))
    root = int(patterns.group(2), 0)
    if root >= len(blob):
        fail("%s: rootOffset %d is outside the %d byte blob" % (path, root, len(blob)))
    return root, bytes(blob)


def align4(value):
    return (value + 3) & ~3


def build(entries):
    """entries: list of (lang, rootOffset, blob). Returns the container bytes."""
    count = len(entries)
    if count == 0 or count > MAX_ENTRIES:
        fail("implausible language count %d" % count)

    blobs_off = align4(HEADER_SIZE + count * ENTRY_SIZE)
    index = bytearray()
    blobs = bytearray()
    offset = blobs_off
    for lang, root, blob in entries:
        crc = zlib.crc32(blob) & 0xFFFFFFFF
        index += struct.pack("<2s2sIIII", lang.encode("ascii"), b"\0\0", root, offset, len(blob), crc)
        blobs += blob
        pad = align4(len(blob)) - len(blob)
        blobs += b"\0" * pad
        offset += len(blob) + pad

    total = blobs_off + len(blobs)
    header = struct.pack("<4sB3sIII I 10I",
                         MAGIC, VERSION, b"\0\0\0", total, 0, count, blobs_off,
                         *([0] * 10))
    if len(header) != HEADER_SIZE:
        fail("internal: header packed to %d bytes, expected %d" % (len(header), HEADER_SIZE))

    image = bytearray(header) + index + blobs
    # Hashed over the header (with the crc field read as zero) plus the index, and
    # NOT over the blobs: each blob carries its own CRC, so a loader that wants one
    # language hashes that language instead of the whole 352 KB container. Both
    # sides must zero the field before hashing.
    covered = HEADER_SIZE + count * ENTRY_SIZE
    crc = zlib.crc32(bytes(image[:12]) + b"\0\0\0\0" + bytes(image[16:covered])) & 0xFFFFFFFF
    struct.pack_into("<I", image, 12, crc)
    return bytes(image), crc


def verify(path, entries):
    """Re-read a written container and check it against the parsed headers."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < HEADER_SIZE:
        fail("%s: shorter than a header" % path)
    magic, version, _pad, total, index_crc, count, blobs_off = struct.unpack_from("<4sB3sIIII", data, 0)
    if magic != MAGIC:
        fail("%s: bad magic %r" % (path, magic))
    if version != VERSION:
        fail("%s: unsupported version %d" % (path, version))
    if total != len(data):
        fail("%s: header says %d bytes, file is %d" % (path, total, len(data)))
    if count != len(entries):
        fail("%s: %d entries, expected %d" % (path, count, len(entries)))
    if blobs_off != align4(HEADER_SIZE + count * ENTRY_SIZE):
        fail("%s: blobsOff %d is not the aligned index end" % (path, blobs_off))

    recomputed = zlib.crc32(data[:12] + b"\0\0\0\0" + data[16:HEADER_SIZE + count * ENTRY_SIZE]) & 0xFFFFFFFF
    if recomputed != index_crc:
        fail("%s: index CRC mismatch (file %08x, recomputed %08x)" % (path, index_crc, recomputed))

    by_lang = {lang: (root, blob) for lang, root, blob in entries}
    seen = set()
    for i in range(count):
        lang, _res, root, off, size, crc = struct.unpack_from("<2s2sIIII", data, HEADER_SIZE + i * ENTRY_SIZE)
        lang = lang.decode("ascii")
        if lang in seen:
            fail("%s: duplicate entry for %s" % (path, lang))
        seen.add(lang)
        if lang not in by_lang:
            fail("%s: unexpected language %s" % (path, lang))
        if off + size > len(data):
            fail("%s: %s blob runs past the end of the file" % (path, lang))
        payload = data[off:off + size]
        if zlib.crc32(payload) & 0xFFFFFFFF != crc:
            fail("%s: %s blob CRC mismatch" % (path, lang))
        want_root, want_blob = by_lang[lang]
        if root != want_root:
            fail("%s: %s rootOffset %d, header says %d" % (path, lang, root, want_root))
        if payload != want_blob:
            # The one assertion that matters: the bytes a device loads must be the
            # bytes the firmware used to carry, or hyphenation silently changes.
            first = next((j for j in range(min(len(payload), len(want_blob)))
                          if payload[j] != want_blob[j]), min(len(payload), len(want_blob)))
            fail("%s: %s blob differs from %s_trie_data at byte %d (%d vs %d bytes)"
                 % (path, lang, lang, first, len(payload), len(want_blob)))
    if seen != set(by_lang):
        fail("%s: missing languages %s" % (path, sorted(set(by_lang) - seen)))
    return len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        help="crossmux repository root (default: %(default)s)")
    parser.add_argument("--out", default=None, help="output path (default: <repo>/build/hyphen.dat)")
    parser.add_argument("--verify", action="store_true",
                        help="check an existing file against the headers instead of writing one")
    args = parser.parse_args()

    generated = os.path.join(args.repo, "lib", "Epub", "Epub", "hyphenation", "generated")
    entries = []
    for lang in LANGUAGES:
        path = os.path.join(generated, "hyph-%s.trie.h" % lang)
        if not os.path.exists(path):
            fail("missing %s" % path)
        root, blob = parse_trie_header(path, lang)
        entries.append((lang, root, blob))

    if args.verify:
        target = args.out or os.path.join(args.repo, "build", "hyphen.dat")
        if not os.path.exists(target):
            fail("%s does not exist; run without --verify first" % target)
        size = verify(target, entries)
        print("verified %s: %d bytes, %d languages, every blob byte-identical to its header"
              % (target, size, len(entries)))
        return

    image, crc = build(entries)

    out = args.out or os.path.join(args.repo, "build", "hyphen.dat")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(image)
    verify(out, entries)

    print("hyphen.dat: %d bytes, crc32=%08x, %d languages -> %s" % (len(image), crc, len(entries), out))
    for lang, root, blob in sorted(entries, key=lambda e: -len(e[2])):
        print("  %s  %8d bytes  root=0x%X" % (lang, len(blob), root))
    print("  firmware .rodata this replaces: %d bytes" % sum(len(b) for _, _, b in entries))

    # Deliberately no committed copy of the output: unlike the Pinyin database, which
    # needs a pinned wheel download to regenerate, this container is rebuilt from the
    # generated headers already in the tree in about a tenth of a second. Committing
    # 352 KB that `--verify` can reproduce would only be a second thing to keep in
    # step. Copy build/hyphen.dat to a card, or flash it (see
    # docs/hyphenation-trie-format.md).


if __name__ == "__main__":
    main()
