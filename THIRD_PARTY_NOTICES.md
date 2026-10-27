# Third-party notices

脆皮鸭 CPYOS is a fork of CrossMux. The project code is MIT-licensed; the
components below keep their own licenses. Full license texts are in
[`LICENSES/`](./LICENSES/) where the upstream project does not already ship one.

## Upstream firmware and interface work

| Project | License | Used for |
|---|---|---|
| [CrossMux](https://github.com/0x1abin/crossmux) | MIT | Upstream firmware, HAL/board layer, build and release tooling |
| [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) | MIT | Reader, EPUB engine and interface foundation |
| [Inx](https://github.com/obijuankenobiii/inx) | See upstream | Interface and theme work |
| [cpr-vcodex](https://github.com/franssjz/cpr-vcodex) | See upstream | Reader/UI experiments |
| [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) | See upstream | Early EPUB reader inspiration |

The original MIT notice is preserved in [`LICENSE`](./LICENSE).

## kiikoread

[kiikoread](https://github.com/wegooo-cell/read-pico-reader) (Apache-2.0)
is the source of the following ported work. The port is
carried as `patches/freeink-sdk/96de1be-local.patch` (and the reader-side code in
this repository) and is licensed under Apache-2.0 where it is derived from kiikoread:

- the water-ripple page-turn engine (`e0470_page_turn`), including its
  multi-band phase steering and the per-tick waveform table;
- `e0470_textturn_soft_settle()` (TEXTTURN white-to-mid-gray early settle);
- the gamma cover LUT used before 4-bit glyph quantization;
- the offline pinyin search table and continuous-pinyin IME rules
  (`scripts/generate_pinyin_db.py`, `lib/Pinyin/`);
- the shake page-turn impulse thresholds and re-arm/cooldown policy
  (`lib/hal/ShakePageTurnGate.h`);
- display/refresh investigation notes carried into the patch comments.

Local differences are documented in `patches/freeink-sdk/README.md`. The
Apache-2.0 text is in [`LICENSES/Apache-2.0.txt`](./LICENSES/Apache-2.0.txt).

## PDF / MuPDF

The experimental PDF reader uses **MuPDF 1.26.3**, which is available under the
**GNU Affero General Public License v3.0** or a commercial license. The prebuilt
static libraries in `patches/mupdf/prebuilt/` are AGPL-3.0. A build that links
them is a combined work distributed under AGPL-3.0; the corresponding source is
this repository plus the pinned MuPDF source and the patches in
`patches/mupdf/`.

The default `readpico` release environment does **not** enable MuPDF. See
[`LICENSES/AGPL-3.0.txt`](./LICENSES/AGPL-3.0.txt).

## Fonts and text

| Component | License | Notes |
|---|---|---|
| FreeType | FTL or GPLv2 | Vector font rasterizer; CFF/OTF support is enabled for Read Pico |
| Noto Sans CJK | SIL OFL 1.1 | Embedded UI fallback and built-in reader fonts |
| Source Han Sans SC | SIL OFL 1.1 | Test/subset fonts used by the font tests |
| pypinyin 0.55.0 | MIT | Source data for the offline pinyin table |
| Hyphenation dictionaries | per language | Generated trie data; upstream notices remain with the source |
| Lucide icons | ISC | UI and toolbar icons |

OFL text: [`LICENSES/OFL-1.1.txt`](./LICENSES/OFL-1.1.txt).

## Web and tooling

| Component | License | Notes |
|---|---|---|
| esp-web-tools | Apache-2.0 | Browser flashing component, loaded from a pinned CDN build |
| PlatformIO / pioarduino | Apache-2.0 | Build system |
| Espressif Arduino core / ESP-IDF | Apache-2.0 / LGPL where noted | Target SDK |
| wolfSSL | GPLv2 / commercial | TLS transport |
| miniz | MIT | ZIP/EPUB archive handling |

## Services and trademarks

JJWXC (晋江文学城) and WeRead (微信读书) are trademarks of their respective
owners. CPYOS is an unofficial client and is not affiliated with, endorsed by,
or supported by either service or by MindReset. Their private endpoints may
change or stop working at any time.

## Reporting

If a notice is missing or a file is misattributed, please open an issue or a
private security advisory; we will correct it.
