#!/usr/bin/env python3
"""Rebuild assets/emoji/NotoEmoji-subset.ttf from the upstream Noto Emoji font.

The firmware ships a small monochrome emoji face (EMJ1 blob in the data
partition).  It is a *fallback*, not a full emoji font: the data-partition slot
is 128 KiB and a full Noto Emoji face is ~886 KiB.  This script keeps the
committed subset and adds the codepoints that actually show up in user-supplied
text (Wi-Fi SSIDs, book titles, comment bodies), checking the slot budget.

Source: Noto Emoji (OFL 1.1), version 3.002, variable wght axis.
    https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf
The upstream file is not committed (1.9 MiB).  Fetch it once and pass
--source (or set CPYOS_EMOJI_SOURCE).  Same input -> same subset.

Usage:
    python3 scripts/build_emoji_subset.py --source /path/NotoEmoji-var.ttf
    python3 scripts/build_emoji_subset.py --check
"""

import argparse
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

# The data partition gives the emoji blob a fixed 128 KiB slot
# (EmojiPackFormat.h / scripts/generate_emoji_pack.py).  The TTF must leave
# room for the 32-byte EMJ1 header.
MAX_TTF_BYTES = 0x20000 - 32  # EMJ1 header shares the 128 KiB slot

# Added on top of the committed subset.  Chosen for user-supplied names and
# content: home/network/device symbols, common animals, popular 1F900 faces,
# and a few objects.  Keep this list small; every glyph costs ~500 bytes.
ADDITIONS = [
    # home, network, devices, locks, power
    0x1F3E0,  # 🏠 house
    0x1F4F6,  # 📶 antenna bars
    0x1F310,  # 🌐 globe with meridians
    0x1F4E1,  # 📡 satellite antenna
    0x1F4BB,  # 💻 laptop
    0x1F4F1,  # 📱 mobile phone
    0x1F5A5,  # 🖥 desktop computer
    0x1F512,  # 🔒 lock
    0x1F511,  # 🔑 key
    0x26A1,   # ⚡ high voltage
    0x1F6DC,  # 🛜 wireless
    0x1F50C,  # 🔌 electric plug
    0x1F50B,  # 🔋 battery
    # animals
    0x1F431,  # 🐱 cat face
    0x1F436,  # 🐶 dog face
    # objects
    0x1F4A1,  # 💡 light bulb
    0x1F4DA,  # 📚 books
    0x1F3B5,  # 🎵 musical note
    0x1F3AE,  # 🎮 video game
    0x1F4F7,  # 📷 camera
    0x1F4B0,  # 💰 money bag
    0x1F3C6,  # 🏆 trophy
    # transport
    0x1F697,  # 🚗 car
    0x1F6B2,  # 🚲 bicycle
    0x1F684,  # 🚄 high-speed train
    # popular supplemental faces (Unicode 8-11)
    0x1F914,  # 🤔 thinking
    0x1F970,  # 🥰 smiling face with hearts
    0x1F929,  # 🤩 star-struck
    0x1F917,  # 🤗 hugging
    0x1F97A,  # 🥺 pleading
    # sky
    0x1F319,  # 🌙 crescent moon
    0x1F31F,  # 🌟 glowing star
]

# Glyph names are not used by the firmware; dropping them keeps the blob
# under the slot budget.  Keep layout/vertical tables: FreeType ignores them
# and they are small, and the emitter stays on the well-trodden path.
DROPPED_TABLES = ("GSUB", "STAT")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def committed_subset(repo):
    return os.path.join(repo, "assets", "emoji", "NotoEmoji-subset.ttf")


def cmap_of(ttf):
    result = {}
    for table in ttf["cmap"].tables:
        result.update(table.cmap)
    return set(result)


def build(source, out):
    if not os.path.exists(source):
        sys.exit(
            "error: missing source %s (Noto Emoji 3.002 variable TTF). See the "
            "docstring for the download URL." % source
        )
    base = cmap_of(TTFont(committed_subset(repo_root()), lazy=True))
    wanted = base | set(ADDITIONS)

    font = TTFont(source)
    font = instantiateVariableFont(font, {"wght": 400}, inplace=False, optimize=True)
    have = cmap_of(font)
    missing = sorted(cp for cp in wanted if cp not in have)
    if missing:
        sys.exit(
            "error: source lacks %d requested codepoints: %s"
            % (len(missing), ", ".join("U+%04X" % cp for cp in missing))
        )

    options = subset.Options()
    options.layout_features = []  # ZWJ ligatures are not rendered by GfxRenderer
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.notdef_outline = True
    options.recalc_bounds = True
    options.recalc_timestamp = False
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=wanted)
    subsetter.subset(font)
    for tag in DROPPED_TABLES:
        if tag in font:
            del font[tag]

    tmp = out + ".tmp"
    font.save(tmp)
    size = os.path.getsize(tmp)
    if size > MAX_TTF_BYTES:
        os.unlink(tmp)
        sys.exit(
            "error: subset is %d bytes; the data-partition slot allows %d "
            "(remove some ADDITIONS)" % (size, MAX_TTF_BYTES)
        )
    os.replace(tmp, out)
    print("%s: %d bytes, %d codepoints" % (out, size, len(wanted)))


def check():
    path = committed_subset(repo_root())
    ttf = TTFont(path, lazy=True)
    have = cmap_of(ttf)
    missing = sorted(cp for cp in ADDITIONS if cp not in have)
    size = os.path.getsize(path)
    if missing:
        sys.exit(
            "error: committed subset lacks %d additions: %s"
            % (len(missing), ", ".join("U+%04X" % cp for cp in missing))
        )
    if size > MAX_TTF_BYTES:
        sys.exit("error: committed subset is %d bytes (limit %d)" % (size, MAX_TTF_BYTES))
    print("%s: current (%d bytes, %d codepoints)" % (path, size, len(have)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default=os.environ.get("CPYOS_EMOJI_SOURCE"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        check()
        return
    if not args.source:
        sys.exit("error: pass --source /path/NotoEmoji-var.ttf or set CPYOS_EMOJI_SOURCE")
    out = committed_subset(repo_root())
    build(args.source, out)
    check()


if __name__ == "__main__":
    main()
