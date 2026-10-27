"""Single source of truth for the gzip'd web assets.

The same bytes are consumed two ways:

  * scripts/build_html.py turns every page into a `*.generated.h` C array. That
    is what C3, the simulator and every other target embed in the firmware.
  * scripts/build_web_pack.py packs six of them into build/web_pack.dat, which
    Read Pico reads from the data/spiffs partition (src/network/WebAssets.cpp).

If each generator compressed independently, the two paths could drift: a page
tested on C3 would not be the page the device actually serves. So both call
asset_payload() in this module -- one compression per build, two consumers.

The pack layout mirrors src/network/WebPackFormat.h. Keep the two in step; the
host test (src/webpack/run_web_pack_test.sh) compiles that header against this
module's output and fails when they disagree.

Deliberately stdlib-only and side-effect free (no printing, no file writes
except through the explicit functions below) so it can be imported by tests.
"""

import gzip
import hashlib
import os
import re
import struct
import zlib
from dataclasses import dataclass
from typing import Dict, Iterable, List, Tuple

# --- pack format constants (mirror WebPackFormat.h) ------------------------
PACK_MAGIC = b"CWP1"
PACK_FORMAT_VERSION = 1
PACK_HEADER_SIZE = 32
PACK_ENTRY_SIZE = 20
PACK_MAX_ENTRIES = 16
PACK_MAX_TOTAL = 1 << 20
PACK_PARTITION_OFFSET = 0x56000
PACK_DEFAULT_PATH = "build/web_pack.dat"
SCHEMA_HEADER_PATH = "src/network/WebPackSchema.generated.h"

HEADER_STRUCT = struct.Struct("<4sBBHIHIQ6x")
ENTRY_STRUCT = struct.Struct("<IIIII")
assert HEADER_STRUCT.size == PACK_HEADER_SIZE
assert ENTRY_STRUCT.size == PACK_ENTRY_SIZE


@dataclass(frozen=True)
class Asset:
    """One page/script that lives in the pack."""

    asset_id: int
    symbol: str  # generated header array name (build_html.py)
    source: str  # repo-relative path
    kind: str  # "html" or "js"


# Order is the index order in the pack and the order the schema signature is
# computed in. Ids are fixed by WebPackFormat.h::AssetId.
ASSETS: Tuple[Asset, ...] = (
    Asset(1, "FilesPageHtml", "src/network/html/en/FilesPage.html", "html"),
    Asset(2, "FilesPageHtml", "src/network/html/zh-CN/FilesPage.html", "html"),
    Asset(3, "SettingsPageHtml", "src/network/html/en/SettingsPage.html", "html"),
    Asset(4, "SettingsPageHtml", "src/network/html/zh-CN/SettingsPage.html", "html"),
    Asset(5, "jszip_minJs", "src/network/html/js/jszip.min.js", "js"),
    Asset(6, "RunnerPageHtml", "src/network/html/RunnerPage.html", "html"),
)


# --- source processing (shared with build_html.py) -------------------------
def resolve_includes(html: str, base_dir: str) -> str:
    """Inline <!--#include file="path" --> directives (path relative to base_dir).

    Must run BEFORE minify_html, which strips all HTML comments. Includes are
    single-level (no included file includes another), so one pass suffices."""
    pattern = re.compile(r'<!--\s*#include\s+file="([^"]+)"\s*-->')

    def repl(match):
        inc_path = os.path.join(base_dir, match.group(1))
        with open(inc_path, "r", encoding="utf-8") as f:
            return f.read()

    return pattern.sub(repl, html)


def minify_html(html: str) -> str:
    # Tags where whitespace should be preserved
    preserve_tags = ['pre', 'code', 'textarea', 'script', 'style']
    preserve_regex = '|'.join(preserve_tags)

    # Protect preserve blocks with placeholders
    preserve_blocks = []

    def preserve(match):
        preserve_blocks.append(match.group(0))
        return f"__PRESERVE_BLOCK_{len(preserve_blocks)-1}__"

    html = re.sub(rf'<({preserve_regex})[\s\S]*?</\1>', preserve, html, flags=re.IGNORECASE)

    # Remove HTML comments
    html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)

    # Collapse all whitespace between tags
    html = re.sub(r'>\s+<', '><', html)

    # Collapse multiple spaces inside tags
    html = re.sub(r'\s+', ' ', html)

    # Restore preserved blocks
    for i, block in enumerate(preserve_blocks):
        html = html.replace(f"__PRESERVE_BLOCK_{i}__", block)

    return html.strip()


def sanitize_identifier(name: str) -> str:
    """Sanitize a filename to create a valid C identifier.

    C identifiers must:
    - Start with a letter or underscore
    - Contain only letters, digits, and underscores
    """
    # Replace non-alphanumeric characters (including hyphens) with underscores
    sanitized = re.sub(r'\W', '_', name, flags=re.ASCII)
    # Prefix with underscore if starts with a digit
    if sanitized and sanitized[0].isdigit():
        sanitized = f"_{sanitized}"
    return sanitized


def header_symbol(file_path: str) -> str:
    """The C array name build_html.py gives this file (FilesPage.html -> FilesPageHtml)."""
    base = os.path.splitext(os.path.basename(file_path))[0]
    suffix = "Html" if file_path.endswith(".html") else "Js"
    return sanitize_identifier(f"{base}{suffix}")


def compress_source(file_path: str, kind: str) -> Tuple[bytes, int]:
    """Return (gzip bytes, original character count) for one source file.

    HTML is include-resolved and minified first; JS is assumed pre-minified.
    mtime=0 keeps the output reproducible across builds, and compresslevel 9 is
    the same setting the firmware has always used.
    """
    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()
    if kind == "html":
        processed = minify_html(resolve_includes(content, os.path.dirname(file_path)))
    else:
        processed = content
    compressed = gzip.compress(processed.encode("utf-8"), compresslevel=9, mtime=0)
    return compressed, len(processed)


def asset_payload(asset: Asset, root: str = ".") -> Tuple[bytes, int]:
    """(gzip bytes, original size) for one pack asset, read from `root`."""
    return compress_source(os.path.join(root, asset.source), asset.kind)


def etag_for(compressed: bytes) -> str:
    """The strong ETag build_html.py bakes into each header."""
    return hashlib.sha256(compressed).hexdigest()[:16]


# --- pack serialization ----------------------------------------------------
def _align4(value: int) -> int:
    return (value + 3) & ~3


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def schema_signature(entries: Iterable[Tuple[int, int, int]]) -> int:
    """FNV-1a64 over (format version, entry count, each id/size/crc32).

    Deliberately excludes offsets: an OTA that reshuffles the pack layout but
    serves the same bytes must keep working, while a pack built from different
    page bytes must not be served by a firmware that expects the new ones.
    """
    rows = list(entries)
    payload = struct.pack("<BH", PACK_FORMAT_VERSION, len(rows))
    for asset_id, size, crc in rows:
        payload += struct.pack("<III", asset_id, size, crc)
    return fnv1a64(payload)


def signature_for(payloads: List[Tuple[Asset, bytes]]) -> int:
    """The schema signature these payloads would be packed with."""
    return schema_signature([(asset.asset_id, len(blob), zlib.crc32(blob) & 0xFFFFFFFF)
                             for asset, blob in payloads])


def build_pack(payloads: List[Tuple[Asset, bytes]]) -> bytes:
    """Serialize (asset, gzip bytes) pairs into the on-flash pack.

    Layout, little-endian:
      header  32 bytes
      index   count * 20 bytes
      data    each asset's gzip payload, 4-byte aligned
    """
    if not payloads:
        raise ValueError("refusing to build an empty web pack")
    if len(payloads) > PACK_MAX_ENTRIES:
        raise ValueError(f"{len(payloads)} assets exceeds the format's {PACK_MAX_ENTRIES}")
    ids = [asset.asset_id for asset, _ in payloads]
    if len(set(ids)) != len(ids):
        raise ValueError(f"duplicate asset id in {ids}")

    index_size = len(payloads) * PACK_ENTRY_SIZE
    cursor = _align4(PACK_HEADER_SIZE + index_size)
    body = bytearray()
    body_base = PACK_HEADER_SIZE + index_size
    index = bytearray(index_size)
    signature_rows = []

    for position, (asset, blob) in enumerate(payloads):
        offset = _align4(cursor)
        body.extend(b"\x00" * (offset - (body_base + len(body))))
        body.extend(blob)
        cursor = offset + len(blob)
        crc = zlib.crc32(blob) & 0xFFFFFFFF
        ENTRY_STRUCT.pack_into(index, position * PACK_ENTRY_SIZE, asset.asset_id, offset, len(blob), crc, 0)
        signature_rows.append((asset.asset_id, len(blob), crc))

    total_size = cursor
    if total_size > PACK_MAX_TOTAL:
        raise ValueError(f"pack is {total_size} bytes, over the {PACK_MAX_TOTAL}-byte cap")

    header = HEADER_STRUCT.pack(
        PACK_MAGIC,
        PACK_FORMAT_VERSION,
        PACK_HEADER_SIZE,
        len(payloads),
        total_size,
        PACK_ENTRY_SIZE,
        zlib.crc32(index) & 0xFFFFFFFF,
        schema_signature(signature_rows),
    )
    assert len(header) == PACK_HEADER_SIZE
    return header + bytes(index) + bytes(body)


def expected_entries(root: str = ".") -> Dict[int, bytes]:
    """The six gzip payloads, keyed by asset id, as this tree would pack them."""
    out = {}
    for asset in ASSETS:
        blob, _ = asset_payload(asset, root)
        out[asset.asset_id] = blob
    return out


def render_schema_header(signature: int, entry_count: int, total_size: int) -> str:
    """The compile-time companion header WebAssets.cpp includes."""
    return (
        "// THIS FILE IS AUTOGENERATED, DO NOT EDIT MANUALLY\n"
        "#pragma once\n\n"
        "#include <cstdint>\n\n"
        "namespace web_pack {\n\n"
        "// FNV-1a64 over (format version, entry count, each entry's id/size/crc32).\n"
        "// A pack whose signature differs was built from different page bytes -- or\n"
        "// by an older tree -- and is rejected whole rather than mixing versions.\n"
        f"inline constexpr uint64_t WEB_PACK_SCHEMA_SIGNATURE = 0x{signature:016X}ull;\n\n"
        f"inline constexpr uint32_t WEB_PACK_SCHEMA_ENTRY_COUNT = {entry_count}u;\n"
        f"inline constexpr uint32_t WEB_PACK_SCHEMA_TOTAL_SIZE = {total_size}u;\n\n"
        "}  // namespace web_pack\n"
    )
