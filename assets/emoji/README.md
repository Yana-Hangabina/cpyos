# Bundled emoji fallback

`NotoEmoji-subset.ttf` is a monochrome [Noto Emoji](https://fonts.google.com/noto/specimen/Noto+Emoji)
face (SIL Open Font License 1.1, see `OFL.txt`). It is **not** a full emoji font:
the firmware only has a 128 KiB slot for it in the data partition, while the
full 1,489-codepoint face is ~886 KiB.

## Where it is loaded

`build/emoji.dat` (32-byte `EMJ1` header + this TTF) is written into the data
partition at `0x160000` by `exp/flash_device.sh` / the release installer.
`src/EmojiFont.cpp` reads it once at boot, validates magic/version/size/CRC32,
and registers a face at the reader point size (`SETTINGS.fontPointSize`, default
18 pt) on builds with PSRAM + the language pack (`readpico`). A missing pack or
a failed load only disables emoji; it never aborts.

`GfxRenderer::drawText()` / `getTextWidth()` split each string at emoji runs and
use this face for those runs, so UI strings (Wi-Fi SSIDs, book titles, comments)
and reader content share it. Unsupported codepoints fall back to the normal
system font and then to the hollow missing-glyph box.

## Rebuilding

```bash
# One-time: download Noto Emoji 3.002 variable TTF (OFL; not committed)
curl -L -o /tmp/NotoEmoji-var.ttf \
  'https://raw.githubusercontent.com/google/fonts/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf'

python3 scripts/build_emoji_subset.py --source /tmp/NotoEmoji-var.ttf
python3 scripts/build_emoji_subset.py --check
python3 scripts/generate_emoji_pack.py          # -> build/emoji.dat
```

The script instantiates `wght=400`, keeps every codepoint already in the
committed subset and adds `ADDITIONS` (network/device symbols, common animals,
popular supplemental faces, a few objects). It fails if the result exceeds the
128 KiB slot, so grow `ADDITIONS` only by dropping other entries.

**Changing the face requires flashing the data partition** (`exp/flash_device.sh`
without `--skip-data`), not just the app slots.
