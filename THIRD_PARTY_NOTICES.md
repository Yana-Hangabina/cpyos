# Third-party notices

脆皮鸭 CPYOS is a fork of CrossMux. Original CPYOS code and the combined
distribution are licensed under the [CPYOS Project License 1.0](./LICENSE):
source-available, personal and non-commercial use only. Third-party components
keep their own licenses. Full license texts are in [`LICENSES/`](./LICENSES/).

## Upstream firmware and interface work

| Project | License | Used for |
|---|---|---|
| [CrossMux](https://github.com/0x1abin/crossmux) | MIT | Upstream firmware, HAL/board layer, build and release tooling |
| [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) | MIT | Reader, EPUB engine and interface foundation |
| [Inx](https://github.com/obijuankenobiii/inx) | See upstream | Interface and theme work |
| [cpr-vcodex](https://github.com/franssjz/cpr-vcodex) | See upstream | Reader/UI experiments |
| [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) | See upstream | Early EPUB reader inspiration |
| [epdiy](https://github.com/vroland/epdiy) (vendor fork) | LGPL-3.0-or-later | Parallel e-paper display backend in `freeink-sdk/libs/display/EpdiyLcd/src/epdiy`; see its `COPYING.LESSER` |
| [Metalio E-INK4 Plus firmware](https://github.com/CloudZao/Metalio-E-INK4-Plus) | MIT | Reference BSP for the experimental Plus port: pin map, TPS65185/TCA9555, FT6336U, 4-bit SD, the S31 parallel display path, and the BH1098G audio path (UART mode-1 handshake, I2S slave 16 kHz 32-bit stereo, amplifier control) ported into `components/cpyos_metalio_bsp/plus_audio.c`. Only files explicitly imported under `ports/metalio_plus/` are covered. |
| [CrossPoint Simulator](https://github.com/0x1abin/crosspoint-simulator) | MIT | Host Arduino/HAL headers reused as the starting point for the experimental Plus UI compatibility layer (`ports/metalio_plus/components/cpyos_ui/compat`) |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT | Header-only JSON library vendored into `ports/metalio_plus/components/ArduinoJson` for the experimental Plus port (offline/reproducible build); upstream license text is kept there |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | Apache-2.0 | JPEG decoder vendored into `ports/metalio_plus/components/imgcodecs` (book images/cover) |
| [PNGdec](https://github.com/bitbank2/PNGdec) | Apache-2.0 | PNG decoder vendored into `ports/metalio_plus/components/imgcodecs` (book images/cover) |
| [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) | Apache-2.0 | GIF decoder vendored into `ports/metalio_plus/components/imgcodecs` (first-frame image pages) |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | MIT | MQTT client vendored into `ports/metalio_plus/components/pubsubclient` for the experimental Plus port |
| [Expat](https://github.com/libexpat/libexpat) | MIT | XML parser (`lib/expat`, linked by `ports/metalio_plus/components/expat_lib`) used for container/OPF/nav/XHTML parsing in the Plus book engine |
| [uzlib](https://github.com/pfalcon/uzlib) | zlib | DEFLATE decoder (`lib/uzlib`, linked by `ports/metalio_plus/components/uzlib_lib`) used by `InflateReader` in the Plus book engine |
| [ricmoo/QRCode](https://github.com/ricmoo/QRCode) | MIT | QR encoder vendored into `ports/metalio_plus/components/qrcode` for the Plus share/transfer screens |
| [esp_tinyusb](https://github.com/espressif/esp-idf/tree/master/components) | Apache-2.0 | Managed component fetched at build time (`ports/metalio_plus/main/idf_component.yml`) and linked into the Plus images for USB mass storage. Not vendored in this repository. |
| [TinyUSB](https://github.com/hathach/tinyusb) | MIT | USB device stack bundled by esp_tinyusb; linked into the Plus images for the USB MSC LUN. Not vendored in this repository. |
| [ESP-IDF NimBLE](https://github.com/espressif/esp-nimble) | Apache-2.0 (ESP-IDF component) | BLE host/controller used by the Plus port's HID central (`ports/metalio_plus/components/cpyos_ui/ble_keyboard_host.cpp`). Not vendored; built from ESP-IDF. |
| [ESP-IDF `esp_http_server`](https://docs.espressif.com/projects/esp-idf/) | Apache-2.0 (ESP-IDF component) | HTTP server used by the Plus port's SD file-transfer page (`ports/metalio_plus/main/web_transfer.cpp`). Not vendored. |
| [mbedTLS / PSA Crypto](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 (ESP-IDF component) | Not vendored. The Plus port replaces the reference build's wolfSSL AES-GCM/SHA-256 calls with a small PSA-backed shim (`ports/metalio_plus/components/cpyos_ui/wolfcrypt_shim.cpp`); no wolfSSL code is linked into the Plus image. |
| [78/uart-uhci](https://components.espressif.com/components/78/uart-uhci) | Apache-2.0 | UHCI DMA transport for the 4G modem, vendored at `ports/metalio_plus/components/uart_uhci` (copied from the official Metalio firmware's dependency set). |
| [espressif/iot_eth](https://components.espressif.com/components/espressif/iot_eth) | Apache-2.0 | Ethernet-over-UART glue (`ETH_DEF` netif/DHCP) for the 4G modem, vendored at `ports/metalio_plus/components/iot_eth` with the version macros pinned locally. |

The original MIT notice is preserved in [`LICENSES/MIT.txt`](./LICENSES/MIT.txt)
and continues to apply to the MIT-derived portions of the code.

## CPYOS project license and the JJWXC component

The original CPYOS code and the combined distribution are licensed under the
[CPYOS Project License 1.0](./LICENSE): source-available, personal and
non-commercial use only. The JJWXC component is closed-source; its source is not
published, and Official Builds may include it in binary form. See
[`NOTICE-JJWXC.md`](./NOTICE-JJWXC.md) for the project's JJWXC support statement.

## kiikoread

[kiikoread](https://github.com/wegooo-cell/read-pico-reader) is the source of the
following ported work. Its licensing is split: code up to v0.3.3-rc93 (2026-10-15)
and files explicitly marked `SPDX-License-Identifier: Apache-2.0` remain Apache-2.0;
newer files may be under the non-OSI `LicenseRef-Kiikoread-Personal-Study-1.0`.
**No Personal-Study-licensed file is incorporated into CPYOS**, including the
Metalio E-INK4 Plus board support added in upstream rc98 and later. Any future
port of that board must use Apache-2.0/MIT sources or an independent implementation.

The port is carried as `patches/freeink-sdk/d9a1304c-local.patch` (and the reader-side
code in this repository) and is licensed under Apache-2.0 where it is derived from
kiikoread:

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
The upstream Personal Study 1.0 text is intentionally not included because no such
code is used in this repository.

CPYOS Project License 1.0 was drafted independently. Its personal-use / non-commercial framing was informed by reading the publicly documented Personal Study License 1.0 used by kiikoread, but no text is copied, and CPYOS does not adopt that license: the binding terms are only those in [`LICENSE`](./LICENSE).
中文：CPYOS 项目许可为独立起草；其「个人使用、禁止商用」的框架参考了 kiikoread 公开的 Personal Study 1.0 思路，但未复制其文本，也不采用该许可——约束条款以 `LICENSE` 为准。

## Copyleft components

Where a build links a copyleft component, that component's license prevails over
the CPYOS Project License for that component and for the combined work it
requires. This includes:

- **epdiy** (LGPL-3.0-or-later): the source and local patches are published in
  this repository and `patches/freeink-sdk/`. You may modify and relink it, and
  may replace it in your own builds; the project license does not limit those
  rights.
- **wolfSSL** (GPLv2 or commercial): used as the TLS transport by the reference
  Read Pico build. For builds that link it under GPLv2, the corresponding source
  is this repository and the GPLv2 terms apply to that component and the combined
  work; the non-commercial restriction in the project license does not reduce
  rights granted by the GPLv2. The Plus port uses ESP-IDF mbedTLS/PSA instead and
  links no wolfSSL code.

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
| Noto Sans | SIL OFL 1.1 | UI-script supplement (Vietnamese, Czech/Polish/Turkish, Hebrew, Arabic) |
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
