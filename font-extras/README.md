# Font Extras publishing

The firmware embeds the 8,105-character CPYOS Han Sans SC base face. Anything
outside that (rare CJK extensions, Korean/Thai/Arabic/Hebrew/Vietnamese, the
full emoji set) is optional and lives outside app0 and outside the OTA data
packs. Settings -> Font Extras fetches the manifest below, downloads one pack
at a time to the SD card, verifies SHA-256, and registers it at runtime.

## Layout

```
site/fonts/manifest-v1.txt        # generated, deployed with Pages
site/fonts/<font-file>.ttf        # served by Pages (or a GitHub release asset)
packs.json                        # source of truth for the manifest
```

The default URL compiled into `readpico` is
`https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt`. A serial
override (`CMD:FEXTURL <url>`) exists for local testing.

## Published set (2026-11-05)

The public tree now hosts three packages under `site/fonts/`; the manifest is
`https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt` and pack URLs use
the `cpyos` branch on raw.githubusercontent.com so the device's gh-proxy
mirrors apply:

| Pack | File | Coverage |
|---|---|---|
| `emoji-full` | `NotoEmoji-Full.ttf` | full monochrome Noto Emoji |
| `cjk-full` | `CPYOSCJK.ttf` | CJK Unified + Ext-A + kana |
| `world-scripts` | `CPYOSWorldScripts.ttf` | Hangul/Thai/Arabic/Hebrew/Devanagari/Vietnamese |

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
