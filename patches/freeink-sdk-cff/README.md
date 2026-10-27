# OTF (CFF) support for the vendored FreeType, carried as a patch

`freeink-sdk` is a gitlink pinned at upstream `96de1be` with its working tree
present but no `.git` of its own, so edits inside it are invisible to this
repository's git (same constraint as `patches/freeink-sdk/README.md`). The CFF
work therefore travels as `cff-otf.patch`, applied with `./apply.sh`
(idempotent: refuses when `ft_cff.c` already exists).

## What is in the patch

Fixes a real gap: the vendored FreeType amalgam was built **truetype-only**
(`ftmodule.h` registered sfnt + truetype; no `cff`/`psaux` sources existed in
the tree), so an OpenType face with PostScript outlines (`.otf`, sfnt tag
`OTTO`) failed `FT_New_Face` with `0x02` (unknown file format) — even though
`SdCardFontRegistry` advertises the `.otf` extension and `TtfEpdFont`'s header
documents OTF support. The font appeared on the SD card, was listed, and could
never render.

Contents (all from FreeType 2.13.3, the exact version already vendored —
verified byte-identical trees, so no cross-version glue):

1. `src/freetype/ft_cff.c` + `ft_psaux.c` — build wrappers in the same style as
   `ft_truetype.c`, each a one-line `#include` of the amalgam behind
   `FREEINK_FONT_ENABLE_CFF` (PlatformIO picks them up from `src/` automatically;
   flag off compiles empty objects).
2. `third_party/freetype/src/cff/` — the upstream CFF driver
   (`cff.c` amalgam: cmap/drivr/gload/parse/load/objs).
3. `third_party/freetype/src/psaux/` — the Type 2 charstring interpreter the
   CFF driver needs (`psaux.c` amalgam).
4. `third_party/freetype/include/freetype/config/ftmodule.h` — register
   `cff_driver_class` and `psaux_module_class` under `FREEINK_FONT_ENABLE_CFF`,
   matching how the file already gates psnames/autofit/raster.

The flag is enabled per-environment (`readpico_hardware` build_flags in
`platformio.ini`), not globally: the driver adds ~57 KB of text (xtensa
measurement) and the no-PSRAM C3 default env sits at 97.5% flash. On the C3 the
flag stays 0 — `.otf` remains rejected there, same as today, while `.ttf`
`.ttc` `.cpfont` are unaffected.

## Verification

- `bash test/otf_support/run_otf_support_test.sh` — compiles the same
  amalgamation twice; CFF=1 loads a CFF fixture (and keeps the TrueType path
  green), CFF=0 rejects the same fixture (so the guard is pinned in both
  directions), plus DejaVu TrueType in both builds.
- Device-side judgement: dropping an `.otf` into `/.fonts` (or `/fonts`) must
  now list and render it; the failure signature before this patch is
  `TTF`-tagged `FT_New_Face` rejection in the logs (font shows as rejected).
- `strings firmware.elf | grep cff` should show the cff driver name once the
  readpico image is built with the flag on (cheap "is it linked" check; the
  empty build produces no such strings).

## Reverting

`git submodule update --init freeink-sdk` (destroys the whole local tree,
including the display patch — restore both with their `apply.sh`), or delete
`src/freetype/ft_cff.c` + `ft_psaux.c`, the two source directories, and revert
the ftmodule.h hunk. Turning the flag back to 0 in platformio.ini is enough to
take CFF out of the firmware image without touching the tree.
