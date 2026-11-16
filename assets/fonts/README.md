# Scalable CJK and UI-script font assets

See [`docs/engineering/font-tiers.md`](../../docs/engineering/font-tiers.md) for
the design and the 8pt analysis.

| File | Use |
|---|---|
| `CPYOSHanSansSC-UI.ttf` | Tier-0 UI subset: translations + hard-coded UI strings + calendar + all non-Han glyphs the source carried |
| `cpyos_han_sans.def` | CPFZ carrier linked into Read Pico / Plus app0 and kept in PSRAM (system UI never reads SD) |
| `CPYOSWorldScripts-UI.ttf` | Tier-0b non-CJK UI vocabulary (Vietnamese, Czech/Polish/Turkish, Hebrew, Arabic, …) |
| `cpyos_ui_ext.def` | CPFZ carrier for the UI-script supplement (~25 KB) |
| `chars_ui.txt` | UI coverage contract (1,509 UI codepoints) |
| `CPYOSHanSansSC-Regular-8105.ttf` | Full 8,105-character reading family; source of the C3 spiffs pack and `build/cjk_core.dat` |
| `chars_8105.txt` | Reading coverage contract (2013 通用规范汉字表) |
| `OFL.txt` | SIL Open Font License 1.1 notice |

`build/cjk_core.dat` (the compressed reading core written to the data partition)
and `build/system_font.dat` (the C3 spiffs wrapper) are generated, not committed.

## Coverage and licensing

* CJK font: [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk), SIL OFL 1.1.
* UI-script font: Noto Sans family (Latin/Greek/Cyrillic/Arabic/Hebrew/Vietnamese),
  SIL OFL 1.1.
* Reading charset: [通用规范汉字表](https://github.com/jaywcjlove/table-of-general-standard-chinese-characters)
  (MIT), unioned with GB2312, ASCII and common CJK/Latin punctuation. The family
  is renamed to `CPYOS Han Sans SC`; no Reserved Font Name is used.
* Characters outside these sets (most Ext-B/C/D ideographs, full non-CJK book
  text) still need a downloadable font-extra or a user-installed SD font.
* U+00B7 MIDDLE DOT, U+00AB/U+00BB and U+2039/U+203A GUILLEMETS, and U+2212
  MINUS are cmap-aliased into the UI subset. Without them bylines, navigation
  labels and every settings-picker chevron fell back to a different face.
* `chars_extra.txt` carries the Azerbaijani/Turkish letters (`Ə ə Ğ ğ İ ı Ş ş
  Ç ç Ö ö Ü ü`) that the Han subset lacks. They are merged into the full
  8,105 TTF from Noto Sans Regular with
  `python3 scripts/add_pack_codepoints.py --source lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf`;
  the UI subset then picks them up automatically.

### Locale letters (`chars_extra.txt`)

The Han subset has no Latin-1 Supplement, Latin Extended-A or Latin Extended-B
letters, so the Azerbaijani UI (`az`) and Turkish UI (`tr`) need
`Ə/ə ÇÖÜçöğİıŞş`. One absent codepoint sends the whole line to the 8pt
emergency face, so these are real letterforms, not a cosmetic gap.

They are **not** cmap aliases: `scripts/add_pack_codepoints.py` copies the
outlines from [Noto Sans Regular](https://github.com/notofonts/noto-fonts)
(SIL OFL 1.1, not the vendor UI brand face; download
`hinted/ttf/NotoSans/NotoSans-Regular.ttf` from `notofonts/noto-fonts@main`,
SHA-256 `b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5`)
and appends them to the pack. Both faces share unitsPerEm 1000, so the outlines
land on the pack's baseline unchanged; the script refuses to proceed if the em
size differs. 13 glyphs cost +1,972 B raw and +1,260 B in the carrier.
`chars_8105.txt` stays at exactly 8,105 entries - `test_system_font_pack.py`
asserts that count and the extra coverage separately.

## Regeneration

```bash
python3 scripts/build_ui_font_subset.py           # all UI assets + build/cjk_core.dat
python3 scripts/build_ui_font_subset.py --check   # verify committed assets
python3 scripts/generate_data_manifest.py         # core row + partition checks
python3 -m unittest scripts.tests.test_system_font_pack
```

The UI subset is derived from the committed 8,105 TTF (single design source),
so a UI glyph and the same glyph in a book have identical outlines. The
`--check` mode and the unit test fail when a new UI string introduces a
codepoint that neither the CJK face nor the UI-script face can draw.

The C3 pack still comes from `scripts/build_system_font_pack.py --font
assets/fonts/CPYOSHanSansSC-Regular-8105.ttf`.
