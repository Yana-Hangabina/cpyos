# CPYOS font extras

Optional fonts for the on-device **Settings -> Font Extras** screen. They are
served from GitHub Pages and downloaded to the SD card on demand; none of them
are linked into app0, the data partition, or the firmware release.

| File | Source | Coverage |
|---|---|---|
| `NotoEmoji-Full.ttf` | Noto Emoji, Google Fonts (OFL 1.1), wght=400 | full monochrome emoji |
| `CPYOSCJK.ttf` | Noto Sans SC, Google Fonts (OFL 1.1), wght=400 | CJK Unified, Ext-A, kana |
| `CPYOSWorldScripts.ttf` | merged Noto Sans KR/Thai/Arabic/Hebrew/Devanagari/Sans (OFL 1.1), wght=400 | Hangul, Thai, Arabic, Hebrew, Devanagari, Latin/Vietnamese |

`manifest-v1.txt` is the FEXT1 manifest consumed by the firmware. Its pack URLs
use `raw.githubusercontent.com` on the `cpyos` branch so the device's gh-proxy
mirror list applies. The manifest itself is at
`https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt`.

The assets are generated with `scripts/build_font_extras_assets.py` in the dev
tree (fontTools instantiates the variable fonts and merges the world-script
family). Only OFL/redistributable fonts belong here; never publish JJWXC
watermark fonts or fonts extracted from a book.
