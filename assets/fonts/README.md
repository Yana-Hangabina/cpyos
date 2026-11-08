# External CJK font pack

One scalable family for every CJK surface (system UI, book titles/authors,
JJWXC text and reader body). It replaces the built-in 12/14/16pt bitmap slices,
so a string never changes family or point size just because it contains a rarer
character.

## Files

| File | Use |
|---|---|
| `CPYOSHanSansSC-Regular-8105.ttf` | source TTF, 9,044 cmap entries |
| `cpyos_han_sans.def` | raw-deflate carrier linked into Read Pico app0; inflated to PSRAM at boot |
| `chars_8105.txt` | coverage contract (the 2013 通用规范汉字表) |
| `chars_extra.txt` | locale letters added beyond the Han set (Azerbaijani/Turkish) |
| `OFL.txt` | SIL Open Font License 1.1 notice |

`build/system_font.dat` (the C3 spiffs pack) is generated from the TTF; it is
not committed because it is a deterministic wrapper.

## Coverage and licensing

* Font: [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk), SIL OFL 1.1.
* Charset: [通用规范汉字表](https://github.com/jaywcjlove/table-of-general-standard-chinese-characters)
  (MIT), unioned with GB2312, ASCII and common CJK/Latin punctuation.
* The family is renamed to `CPYOS Han Sans SC`; no Reserved Font Name is used.
* Characters outside this set (including most Ext-B/C/D ideographs) still need
  a user-installed SD font.
* U+00B7 MIDDLE DOT is included even though it is not one of the 8,105 Han
  characters: bylines and metadata use it as a separator. Without the glyph,
  the renderer selects the 8pt emergency face for the whole string, making the
  line visibly smaller. The glyph is a period outline raised to the middle-dot
  position (the source subset omitted the original codepoint).
* U+00AB/U+00BB GUILLEMET and U+2212 MINUS SIGN are cmap aliases to the
  existing `<`/`>`/`-` glyphs. The Chinese/English navigation strings already
  use `« 返回`, `下一页 »` and `− / +`; without these aliases those whole
  labels fall back to 8pt as well.
* U+2039/U+203A SINGLE GUILLEMETS are the settings picker's row chevrons
  (`SettingsActivity` appends `›` to every non-toggle value). They are cmap
  aliases to `<`/`>`, like the double guillemets; without them every picker
  row ended in a tofu square that read as an empty checkbox.

### Locale letters (`chars_extra.txt`)

The Han subset has no Latin-1 Supplement, Latin Extended-A or Latin Extended-B letters, so the
Azerbaijani UI (`az`) and the pre-existing Turkish UI (`tr`) were missing
`Ə/ə ÇÖÜçöğİıŞş`. One absent codepoint sends the whole line to the 8pt
emergency face, so these are real letterforms, not a cosmetic gap.

They are **not** cmap aliases: `scripts/add_pack_codepoints.py` copies the
outlines from [Noto Sans Regular](https://github.com/notofonts/noto-fonts)
(SIL OFL 1.1, not the vendor UI brand face; download
`hinted/ttf/NotoSans/NotoSans-Regular.ttf` from `notofonts/noto-fonts@main`,
SHA-256 `b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5`)
and appends them to the pack. Both faces share unitsPerEm 1000, so the outlines
land on the pack's baseline unchanged; the script refuses to proceed if the
em size differs. 13 glyphs cost +1,972 B raw and +1,260 B in the carrier.
`chars_8105.txt` stays at exactly 8,105 entries - `test_system_font_pack.py`
asserts that count and the extra coverage separately.

## Regeneration

```bash
# 1. subset the full OTF (fontTools) to chars_8105.txt + GB2312 + punctuation
# 2. convert CFF to TrueType: otf2ttf <subset.otf> -o CPYOSHanSansSC-Regular-8105.ttf
#    then delete the unused `post`, `VORG`, `vhea`, `vmtx` and `BASE` tables
#    (FreeType only needs horizontal metrics; this saves ~120 KB raw)
# 3. merge the locale letters from the OFL Latin source (see above for the URL)
python3 scripts/add_pack_codepoints.py --source /path/NotoSans-Regular.ttf
# 4. rebuild both carriers and re-key the pack-derived font ids
python3 scripts/build_system_font_pack.py \
  --embedded-out assets/fonts/cpyos_han_sans.def
python3 scripts/build_font_ids.py
```

Step 4 is not optional: `CJK_READER_SIZE_FONT_ID` embeds
`crc32(pack TTF) & 0xFFFF`, so skipping `build_font_ids.py` keeps the old ids
and cached pagination is never rebuilt for the new metrics.
`scripts/add_pack_codepoints.py --check` verifies coverage without a source font.

The script asserts nothing about coverage; `scripts/tests/test_system_font_pack.py`
checks that the committed TTF covers `chars_8105.txt` (exactly 8,105 entries)
and `chars_extra.txt`, that the `.def` carrier inflates back to the same bytes,
and that `src/fontIds.h` still carries the pack's CRC.
