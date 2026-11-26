#!/usr/bin/env python3
"""Minimal sfnt cmap reader with no fontTools dependency.

Three build tools need "which codepoints does this face claim to draw?" and the
toolchain cannot assume fontTools is installed (the host test suites are expected
to run in a bare CI image, and scripts/tests/test_emoji_pack.py already carries its
own parser for exactly that reason).  This is the shared implementation.

Only what a cmap query needs is parsed: the table directory, the encoding
subtables, and formats 4 / 6 / 12.  A face using anything else reports the
subtables it did understand, which for our OFL sources is all of them.
"""

from __future__ import annotations

import pathlib
import struct

__all__ = ["cmap_of", "cmap_codepoints", "cmap_subtables", "UNICODE_SUBTABLES",
           "rendered_codepoints", "CmapError"]


class CmapError(ValueError):
    """The bytes are not an sfnt with a readable Unicode cmap."""


def _tables(data: bytes) -> dict[bytes, tuple[int, int]]:
    try:
        sfnt_type = data[:4]
        num_tables = struct.unpack_from(">H", data, 4)[0]
    except (IndexError, struct.error) as exc:  # pragma: no cover - short input
        raise CmapError("not an sfnt") from exc
    if sfnt_type not in (b"\x00\x01\x00\x00", b"true", b"typ1", b"OTTO", b"ttcf"):
        raise CmapError(f"unknown sfnt version {sfnt_type!r}")
    if sfnt_type == b"ttcf":  # font collection: use the first font
        count = struct.unpack_from(">I", data, 8)[0]
        offset = struct.unpack_from(">I", data, 12)[0]
        if offset + 12 > len(data):
            raise CmapError("collection offset outside the file")
        num_tables = struct.unpack_from(">H", data, offset + 4)[0]
        base = offset + 12
    else:
        base = 12
    out: dict[bytes, tuple[int, int]] = {}
    for i in range(num_tables):
        try:
            tag, _checksum, offset, length = struct.unpack_from(">4sIII", data, base + i * 16)
        except (IndexError, struct.error) as exc:  # pragma: no cover
            raise CmapError("truncated table directory") from exc
        if offset + length > len(data):
            raise CmapError(f"table {tag!r} runs past the end of the file")
        out[tag] = (offset, length)
    return out


def _fmt4(data: bytes, off: int) -> set[int]:
    seg_x2 = struct.unpack_from(">H", data, off + 6)[0]
    seg = seg_x2 // 2
    ends = struct.unpack_from(f">{seg}H", data, off + 14)
    starts = struct.unpack_from(f">{seg}H", data, off + 16 + seg_x2)
    deltas = struct.unpack_from(f">{seg}H", data, off + 16 + seg_x2 * 2)
    range_off_at = off + 16 + seg_x2 * 3
    range_offsets = struct.unpack_from(f">{seg}H", data, range_off_at)
    cps: set[int] = set()
    for i, (start, end) in enumerate(zip(starts, ends)):
        if start == 0xFFFF:
            continue
        for cp in range(start, min(end, 0xFFFF) + 1):
            delta = deltas[i]
            if range_offsets[i] != 0:
                addr = range_off_at + i * 2 + range_offsets[i] + (cp - start) * 2
                if addr + 2 > len(data):
                    continue
                gid = struct.unpack_from(">H", data, addr)[0]
                if gid == 0:
                    continue
                gid = (gid + delta) & 0xFFFF
                if gid == 0:
                    continue
            cps.add(cp)
    return cps


def _fmt6(data: bytes, off: int) -> set[int]:
    first, count = struct.unpack_from(">HH", data, off + 6)
    cps: set[int] = set()
    for i in range(count):
        addr = off + 10 + i
        if addr >= len(data):
            break
        if data[addr]:
            cps.add(first + i)
    return cps


def _fmt12(data: bytes, off: int) -> set[int]:
    groups = struct.unpack_from(">I", data, off + 12)[0]
    cps: set[int] = set()
    for g in range(groups):
        try:
            start, end, _gid = struct.unpack_from(">III", data, off + 16 + g * 12)
        except (IndexError, struct.error):
            break
        if end < start:
            continue
        # A group of 100k+ ideographs is normal for CJK; do not materialise it
        # as a set of every codepoint when the caller only asks "is X present".
        cps.update(range(start, min(end, 0x10FFFF) + 1))
    return cps


_FORMATS = {4: _fmt4, 6: _fmt6, 12: _fmt12}

# Subtables a renderer will actually consult for a Unicode codepoint. FreeType maps
# FT_ENCODING_UNICODE onto the Microsoft/Unicode platform pairs (it prefers 3/1 and takes
# 3/10 for astral planes); Mac Roman (1/0) is a legacy 8-bit map whose slots mean different
# characters, so including it in a *coverage* claim overstates what the device can draw.
UNICODE_SUBTABLES = ((0, 3), (0, 4), (3, 1), (3, 10))


def _as_bytes(data) -> bytes:
    """Accept bytes, a path, or a path string.

    Every caller starts from a file, and ``TypeError: a bytes-like object is
    required, not 'str'`` from inside the parser is a bad way to say that --
    it reads like "this is not a font" rather than "I was handed the wrong
    argument type".
    """
    if isinstance(data, (str, pathlib.PurePath)):
        return pathlib.Path(data).read_bytes()
    return data


def cmap_subtables(data: "bytes | pathlib.Path | str") -> dict[tuple[int, int], set[int]]:
    """Every encoding subtable this face ships, keyed by (platform, encoding)."""
    data = _as_bytes(data)
    tables = _tables(data)
    if (b"cmap" not in tables) or (b"glyf" not in tables and b"CFF " not in tables):
        raise CmapError("no cmap or no glyph outline table")
    cmap_off = tables[b"cmap"][0]
    count = struct.unpack_from(">H", data, cmap_off + 2)[0]
    out: dict[tuple[int, int], set[int]] = {}
    for i in range(count):
        platform, encoding, offset = struct.unpack_from(">HHI", data, cmap_off + 4 + i * 8)
        off = cmap_off + offset
        fmt = struct.unpack_from(">H", data, off)[0]
        reader = _FORMATS.get(fmt)
        if reader is None:
            continue
        key = (platform, encoding)
        if key not in out:
            out[key] = reader(data, off)
    if not out:
        raise CmapError("no readable Unicode subtable")
    return out


def rendered_codepoints(data: "bytes | pathlib.Path | str") -> set[int]:
    """What the device can draw: the union over UNICODE_SUBTABLES only."""
    data = _as_bytes(data)
    cps: set[int] = set()
    for key in UNICODE_SUBTABLES:
        cps |= cmap_subtables(data).get(key, set())
    return cps


def cmap_codepoints(data: "bytes | pathlib.Path | str") -> set[int]:
    """Every codepoint any Unicode subtable maps to a non-zero glyph.

    Accepts a path as well as bytes: every caller starts from a file, and a
    ``TypeError: 'PosixPath' object is not subscriptable`` from inside the parser is a
    confusing way to report that.

    Note for coverage assertions: this union includes Mac Roman (1/0) because some OFL
    sources ship only legacy maps. For "can this device draw it", use
    rendered_codepoints() -- an 8-bit legacy table's slot is not the same character.
    """
    cps: set[int] = set()
    for subtable in cmap_subtables(data).values():
        cps |= subtable
    if not cps:
        raise CmapError("no readable Unicode subtable")
    return cps


def cmap_of(path: pathlib.Path | str) -> set[int]:
    p = pathlib.Path(path)
    try:
        data = p.read_bytes()
    except OSError as exc:
        raise CmapError(f"cannot read {p}: {exc}") from exc
    return cmap_codepoints(data)
