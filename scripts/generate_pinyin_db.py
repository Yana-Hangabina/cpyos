#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：由固定官方拼音轮子生成 SD 卡资源 pinyin.dat；设备侧只读该文件，固件不含拼音数据。
# English: Build the SD-card resource pinyin.dat from the pinned official Pinyin wheel; the firmware only reads it.
#
# 移植自 read-pico-reader(kiikoread, Apache-2.0)的 tools/generate_search_table.py 与
# tools/generate_ime_dictionary.py，算法与数据顺序逐条保留（字表、音组、音节、反查索引、
# 词组优先级），仅把 C 头文件输出改为单一二进制数据库。
# Ported from read-pico-reader (kiikoread, Apache-2.0): the table, grouping, syllable and
# reverse-index rules are kept; only the output changes from C headers to one binary database.
#
# 数据源：pypinyin==0.55.0（MIT，LICENSE 见 scripts/pinyin/LICENSE.pypinyin）。
# Source data: pypinyin 0.55.0 (MIT; see scripts/pinyin/LICENSE.pypinyin).
import argparse
import hashlib
import json
import struct
import unicodedata
import zlib
import zipfile
from pathlib import Path

VERSION = "0.55.0"
WHEEL_SHA256 = "d53b1e8ad2cdb815fb2cb604ed3123372f5a28c6f447571244aca36fc62a286f"
ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"PYDB"
FORMAT_VERSION = 1
HEADER_SIZE = 96

# 常用字优先，随后 GB2312 一级、二级，其余基本汉字仍可完整翻页。
# Common glyphs first, then GB2312 level one/two; every other basic Han character remains reachable.
COMMON = ("的一是不了人我在有他这中大来上国个到说们为子和你地出道也时年得就那要下以生会自着去之过家学对可她"
          "里后小心多天而能好都然没日于起还发成只如事把无明看本面知现所同手时方女新前想最太见被高用开么将行长"
          "身三间加由其从两情进已又些点样意力第话走实定才爱亲当问比很世书水名作每海边信安静远山慢读清晨章客花月"
          "风雨空云春夏秋冬你我他她它孩文字篇页故事")


def normalize(value):
    # ü 保留为 v，其余组合声调去除。/ Preserve ü as v and remove other combining tone marks.
    out = ""
    for c in unicodedata.normalize("NFD", value.lower()):
        if c == "\u0308" and out.endswith("u"):
            out = out[:-1] + "v"
        elif not unicodedata.combining(c):
            out += c
    if not out.isascii() or not out.isalpha():
        raise ValueError(f"Unsupported dictionary pronunciation: {value!r} -> {out!r}")
    return out


def load_phrases(path):
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        roman, text = line.split("\t")
        entries.append((roman, text))
    return entries


class Sections:
    """按 4 字节对齐追加节，并记录偏移。Sections appended 4-byte aligned with recorded offsets."""

    def __init__(self):
        self.blob = bytearray()
        self.offsets = {}

    def _align(self):
        while len(self.blob) % 4:
            self.blob.append(0)

    def add(self, name, data: bytes):
        self._align()
        self.offsets[name] = len(self.blob)
        self.blob += data

    def add_u16(self, name, values):
        self.add(name, struct.pack(f"<{len(values)}H", *values))

    def add_u32(self, name, values):
        self.add(name, struct.pack(f"<{len(values)}I", *values))


def main():
    parser = argparse.ArgumentParser(description="Generate pinyin.dat for the reader's SD card")
    parser.add_argument("wheel", type=Path, help="pypinyin-0.55.0-py2.py3-none-any.whl")
    parser.add_argument("-o", "--out", type=Path, default=ROOT / "build/pinyin.dat",
                        help="output path (default: build/pinyin.dat)")
    args = parser.parse_args()

    digest = hashlib.sha256(args.wheel.read_bytes()).hexdigest()
    if digest != WHEEL_SHA256:
        raise ValueError("Wheel SHA-256 differs from the pinned official release")
    with zipfile.ZipFile(args.wheel) as wheel:
        metadata = wheel.read(f"pypinyin-{VERSION}.dist-info/METADATA").decode()
        assert f"Version: {VERSION}\n" in metadata
        entries = json.loads(wheel.read("pypinyin/pinyin_dict.json"))

    # ---- reading table / 读音表（generate_search_table.py 的搬运） ----
    pronunciations = {int(cp): tuple(sorted(set(normalize(v) for v in values.split(","))))
                      for cp, values in entries.items()}
    syllables = sorted({v for values in pronunciations.values() for v in values})
    syllable_ids = {v: i for i, v in enumerate(syllables)}
    groups = sorted(set(pronunciations.values()))
    group_ids = {v: i for i, v in enumerate(groups)}
    chars = sorted(pronunciations)
    group_offsets, readings = [0], []
    for group in groups:
        readings.extend(syllable_ids[v] for v in group)
        group_offsets.append(len(readings))
    syllable_offsets, syllable_data = [], b""
    for syllable in syllables:
        syllable_offsets.append(len(syllable_data))
        syllable_data += syllable.encode() + b"\0"
    syllable_offsets.append(len(syllable_data))
    assert max(len(groups), len(readings), len(syllable_data)) <= 0xFFFF

    # ---- IME reverse index / 反查索引（generate_ime_dictionary.py 的搬运） ----
    priority = {ord(ch): i for i, ch in reversed(list(enumerate(COMMON)))}
    level = {}
    for hi in range(0xb0, 0xf8):
        for lo in range(0xa1, 0xff):
            try:
                level[ord(bytes([hi, lo]).decode("gb2312"))] = 0 if hi < 0xd8 else 1
            except UnicodeDecodeError:
                pass
    reverse = [[] for _ in syllables]
    for cp, group in zip(chars, (pronunciations[cp] for cp in chars)):
        if 0x4E00 <= cp <= 0x9FFF:
            for index in readings[group_offsets[group_ids[group]]:group_offsets[group_ids[group] + 1]]:
                reverse[index].append(cp)
    ime_offsets, ime_chars = [0], []
    for bucket in reverse:
        bucket.sort(key=lambda cp: (0, priority[cp]) if cp in priority else (1 + level.get(cp, 2), cp))
        ime_chars.extend(bucket)
        ime_offsets.append(len(ime_chars))
    assert len(ime_chars) <= 0xFFFF * 8  # u32 offsets, u16 code points: bounds only for sanity

    # ---- phrases / 词库 ----
    phrases = load_phrases(ROOT / "scripts/pinyin/phrases.tsv")
    phrase_offsets, phrase_blob = [0], bytearray()
    for roman, text in phrases:
        phrase_blob += roman.encode() + b"\0" + text.encode() + b"\0"
        phrase_offsets.append(len(phrase_blob))

    sections = Sections()
    sections.add_u32("codepoints", chars)
    sections.add_u16("groups", [group_ids[pronunciations[cp]] for cp in chars])
    sections.add_u16("group_offsets", group_offsets)
    sections.add_u16("readings", readings)
    sections.add_u16("syllable_offsets", syllable_offsets)
    sections.add("syllables", syllable_data)
    sections.add_u32("ime_offsets", ime_offsets)
    sections.add_u16("ime_chars", ime_chars)
    sections.add_u32("phrase_offsets", phrase_offsets)
    sections.add("phrases", bytes(phrase_blob))

    total = HEADER_SIZE + len(sections.blob)
    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = FORMAT_VERSION
    struct.pack_into("<I", header, 8, total)  # crc32 at 12 stays zero until the first pass is hashed
    struct.pack_into("<IIIIII", header, 16,
                     len(chars), len(groups), len(readings), len(syllables), len(ime_chars), len(phrases))
    struct.pack_into("<IIIIII", header, 40,
                     HEADER_SIZE + sections.offsets["codepoints"],
                     HEADER_SIZE + sections.offsets["groups"],
                     HEADER_SIZE + sections.offsets["group_offsets"],
                     HEADER_SIZE + sections.offsets["readings"],
                     HEADER_SIZE + sections.offsets["syllable_offsets"],
                     HEADER_SIZE + sections.offsets["syllables"])
    struct.pack_into("<IIIIII", header, 64,
                     len(syllable_data),
                     HEADER_SIZE + sections.offsets["ime_offsets"],
                     HEADER_SIZE + sections.offsets["ime_chars"],
                     HEADER_SIZE + sections.offsets["phrase_offsets"],
                     HEADER_SIZE + sections.offsets["phrases"],
                     len(phrase_blob))
    image = bytes(header) + bytes(sections.blob)
    crc = zlib.crc32(image)
    struct.pack_into("<I", header, 12, crc & 0xFFFFFFFF)
    image = bytes(header) + bytes(sections.blob)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)
    print(f"pypinyin {VERSION}; wheel sha256={digest[:16]}...")
    print(f"{len(chars)} codepoints; {len(groups)} groups; {len(readings)} readings; {len(syllables)} syllables; "
          f"{len(set(ime_chars))} IME characters in {len(ime_chars)} mappings; {len(phrases)} phrases")
    print(f"pinyin.dat: {len(image)} bytes, crc32={crc & 0xFFFFFFFF:08x} -> {args.out}")


if __name__ == "__main__":
    main()
