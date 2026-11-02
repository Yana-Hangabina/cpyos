# External CJK font pack

One scalable family for every CJK surface (system UI, book titles/authors,
JJWXC text and reader body). It replaces the built-in 12/14/16pt bitmap slices,
so a string never changes family or point size just because it contains a rarer
character.

## Files

| File | Use |
|---|---|
| `CPYOSHanSansSC-Regular-8105.ttf` | source TTF, 9,029 cmap entries |
| `cpyos_han_sans.def` | raw-deflate carrier linked into Read Pico app0; inflated to PSRAM at boot |
| `chars_8105.txt` | coverage contract (the 2013 通用规范汉字表) |
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

## Regeneration

```bash
# 1. subset the full OTF (fontTools) to chars_8105.txt + GB2312 + punctuation
# 2. convert CFF to TrueType: otf2ttf <subset.otf> -o CPYOSHanSansSC-Regular-8105.ttf
#    then delete the unused `post`, `VORG`, `vhea`, `vmtx` and `BASE` tables
#    (FreeType only needs horizontal metrics; this saves ~120 KB raw)
# 3. rebuild both carriers
python3 scripts/build_system_font_pack.py \
  --embedded-out assets/fonts/cpyos_han_sans.def
```

The script asserts nothing about coverage; `scripts/tests/test_system_font_pack.py`
checks that the committed TTF covers `chars_8105.txt` and that the `.def`
carrier inflates back to the same bytes.
