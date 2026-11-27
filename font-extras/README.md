# Font Extras publishing

What ships in the firmware is **not** the 8,105-character face. Measured on
2026-11-26 with `scripts/font_cmap.py`:

| Layer | Where | cmap | What it can draw |
|---|---|---:|---|
| UI face | app0 (`cpyos_han_sans.def`) | 1,520 | system strings only |
| UI scripts | app0 (`cpyos_ui_ext.def`) | 438 | Latin-1 Supplement, IPA, Greek, Cyrillic, punctuation |
| Reading core | data partition (`cjk_core_<tier>.dat`) | **3,639 on Read Pico's XIP layout, 9,044 on a layout wide enough for the 8,105 carrier** | common Han reading text |
| Extra packs | SD, downloaded | varies | everything above |

So rare name characters (璐 / 曦 / 玥, ~5,400 codepoints) are **absent** on Read Pico
until the `cjk-core` pack is installed, and the firmware never overwrites a face it did
not release itself (see `src/SystemCoreFontCheck.h`). Anything outside all of the above --
CJK Ext-B and beyond, Korean/Thai/Arabic/Hebrew, the full emoji set -- is optional and
lives outside app0 and outside the OTA data packs. Settings -> Font Extras fetches the
manifest below, downloads one pack at a time to the SD card, verifies SHA-256, and
registers it at runtime.

## Layout

```
site/fonts/manifest-v1.txt        # generated, deployed with Pages
site/fonts/<font-file>.ttf        # served by Pages (or a GitHub release asset)
packs.json                        # source of truth for the manifest
```

The default URL compiled into `readpico` is
`https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt`. A serial
override (`CMD:FEXTURL <url>`) exists for local testing.

## Published set

The public tree hosts **four** packages under `site/fonts/` (verified against the live
`manifest-v1.txt` on 2026-11-26, not against this document's memory); pack URLs use the
`cpyos` branch on raw.githubusercontent.com so the device's gh-proxy mirrors apply:

| Pack | File | Bytes | Coverage |
|---|---|---:|---|
| `emoji-full` | `NotoEmoji-Full.ttf` | 844,596 | full monochrome Noto Emoji |
| `cjk-core` | `CPYOSCJKCore.ttf` | 2,553,580 | the 8,105 reading core (superset of the partition carrier) |
| `cjk-full` | `CPYOSCJK.ttf` | 10,418,152 | CJK Unified + Ext-A + kana |
| `world-scripts` | `CPYOSWorldScripts.ttf` | 6,757,844 | Hangul/Thai/Arabic/Hebrew/Devanagari/Vietnamese |

### Why `cjk-core` shares a directory with the partition core

`cjk-core` installs to `/fonts/CPYOSCJKCore/CPYOSCJKCore.ttf`, which is the same path the
firmware releases its partition carrier to. That is deliberate, and it is the upgrade path:
the carrier is tiered by partition size (Read Pico's XIP layout only fits the 3,500 one), so
downloading `cjk-core` replaces a narrower face with its **superset** in place, and the
renderer keeps exactly one supplement family instead of choosing between two.

The two directions are protected differently, on purpose:

* the firmware **never overwrites** a face it did not release -- a size or content mismatch
  against the carrier is kept and logged (`Verdict::ForeignSize` / `ForeignContent`);
* FontExtras **does** replace it, because it verified the download's size, SHA-256 and sfnt
  magic into a `.part` file first and installs with `replaceFile`.

A plan to rename the device side to `CPYOSCore3500/8105` was considered and rejected for
this reason (and because the Pages manifest would have to move in the same change); see
`docs/plans/2026-11-26-readpico-unified-pdf-channel-and-fonts.md` §15.14.

`font-extras/packs.json` is the source spec for that manifest. Generate the
assets with `scripts/build_font_extras_assets.py` (fontTools), then rebuild the
manifest with the command below and publish `site/fonts/` only. No firmware
tag or release is needed for a font update.

## Build the manifest

1. Put the font files next to `packs.json` (or point `--assets-dir` at the
   directory that will be published). Only `.ttf`, `.otf` and `.ttc` are
   accepted; each file must be between 512 B and 32 MiB.
2. Copy `packs.example.json` to `packs.json` and edit the entries. `kind` is
   `fallback` (registered as an additive renderer fallback) or `emoji`
   (replaces EmojiFont's partition subset at runtime). A fallback pack needs a
   unique `family` name; an emoji pack uses `-`.
3. Generate:

   ```bash
   python3 scripts/build_font_extras_manifest.py \
     --spec font-extras/packs.json \
     --assets-dir site/fonts \
     --out site/fonts/manifest-v1.txt
   python3 scripts/build_font_extras_manifest.py \
     --spec font-extras/packs.json \
     --assets-dir site/fonts \
     --out site/fonts/manifest-v1.txt --check
   ```

`packs.json` is not consumed by the firmware. The generated manifest records
each asset's exact size and SHA-256, so a changed font without a regenerated
manifest is rejected by the client.

## Hosting

- **Pages (recommended for a small set)**: put the files under `site/fonts/`
  and let the existing Pages workflow deploy them. The manifest URL above then
  works without any query parsing.
- **GitHub Release assets**: upload the same files to a dedicated release tag
  (for example `fonts-1`) and point each `url` at
  `https://github.com/<owner>/<repo>/releases/download/fonts-1/<file>`. The
  client prepends the same gh-proxy mirrors used by OTA, so mainland-China
  access does not depend on GitHub directly.

Do not put these files in `assets/` or in the data-partition pack list: that
would grow app0/OTA again, which is exactly what this feature avoids.

## Licensing and safety

Use only redistributable fonts (Noto/Noto CJK are OFL). Do not publish the
JJWXC watermark fonts or any font extracted from a book. The client verifies
SHA-256 and the sfnt magic before installing; a failed download never replaces
an existing pack. A corrupt manifest is rejected as a whole.

## Device verification

- `CMD:FEXTSTATUS` prints the installed packs and the manifest URL.
- `CMD:FEXTCHECK` fetches/parses the manifest (Wi-Fi must already be up).
- `CMD:FEXTINSTALL <id>` downloads one pack and activates it.
- `CMD:FEXTDEMO` opens the Font Extras screen.
- Settings -> Font Extras shows installed/update state and removes packs with a
  second confirmation.
