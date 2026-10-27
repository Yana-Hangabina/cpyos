# OTF (CFF) support host test

Regression for TF-card vector fonts with PostScript outlines.

## What it pins

The vendored FreeType amalgamation (`freeink-sdk/libs/font/FreeInkFont`) was
built truetype-only: `ftmodule.h` registered sfnt + truetype and the CFF driver
was neither vendored nor compiled, so a user's `.otf` on the SD card failed
`FT_New_Face` with `0x02` (unknown file format) even though
`SdCardFontRegistry` advertises the `.otf` extension. `FREEINK_FONT_ENABLE_CFF=1`
(vendored cff + psaux modules, readpico env) fixes that.

`run_otf_support_test.sh` compiles the same amalgamation the firmware compiles,
twice:

| build | fixture | expected |
|---|---|---|
| `FREEINK_FONT_ENABLE_CFF=1` | `cff_fixture.otf` (CFF outlines) | loads, glyphs rasterize with non-zero coverage |
| `FREEINK_FONT_ENABLE_CFF=1` | DejaVu Sans (TrueType outlines) | still loads — no tt regression |
| `FREEINK_FONT_ENABLE_CFF=1` | padded CFF (generated) via split stream | loads and renders when CFF tables fall past a 4 KB / 64 KB RAM prefix, tail reads actually happen |
| `FREEINK_FONT_ENABLE_CFF=0` | `cff_fixture.otf` | **rejected** (0x02) — the test goes red if rejection ever stops being real |
| `FREEINK_FONT_ENABLE_CFF=0` | DejaVu Sans | still loads |

No cmake/gtest; mirrors `src/*/run_*.sh`. Run from anywhere:

```sh
bash test/otf_support/run_otf_support_test.sh
```

The split-stream case mirrors the firmware's real read pattern
(`SdCardFontSystem::prefixRead`: RAM prefix cache + SD tail). CFF puts its
outline and encoding tables at the end of the file, so this is the one shape
where a buffer-oriented host test would miss a defect — the device never hands
FreeType a whole file. `gen_tail_fixture.py` rebuilds the padded fixture at
test time (needs fontTools; the case skips when unavailable).

## Device check

Drop a CFF-flavored `.otf` (e.g. a Source Han / Noto CJK `OTF`, not the
`TTF`/`.ttc` variants) into `/fonts/` or `/.fonts/` on the SD card, select it
in the reader font settings, and turn a few pages. Logs to look for with
serial console on: the face family name in the `SDFS` load line, and no
`FT_New_Face`-style rejection (the pre-patch failure signature was the font
being listed and every glyph missing).

Getting the file onto the device without pulling the card: the web UI's
**Files** page (Settings → File Transfer) accepts any file — open it, navigate
into `/fonts`, and upload there (`/upload?path=/fonts` under the hood, no
extension filter). The dedicated **Fonts** page uploads only the compressed
`.cpfont` format by design, so a raw `.ttf`/`.otf` is rejected there with
"Invalid .cpfont file" — that is not a symptom of this change.

## Fixture provenance

`cff_fixture.otf` (13,884 B, sfnt tag `OTTO`) is a 39-codepoint subset — ASCII
printable plus 永和九年你好世界 — of a locally installed macOS system font
(Hiragino Maru Gothic ProN W4), converted to CFF outlines with fontTools.
It exists solely as test data for the FreeType CFF parser and is not
redistributed as a usable font.
