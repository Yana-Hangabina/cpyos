# 脆皮鸭 CPYOS

A reading-first firmware for the **Read Pico** (MindReset RDP-G01-W, ESP32-S3, 684×1216, 16 grey levels), built on [CrossMux](https://github.com/0x1abin/crossmux).

Project site and browser flasher: <https://yana-hangabina.github.io/cpyos/><br>
Releases, full install packages and checksums: <https://github.com/Yana-Hangabina/cpyos/releases>

## What this is

CPYOS targets the Read Pico only. It keeps what CrossMux already does and rebuilds the parts that matter for reading Chinese web novels, local books and web pages on this panel:

- a redesigned home, library, find-books, My and reader UI;
- a native JJWXC client and a reader-mode web browser;
- one scalable CJK family, pinyin input, hyphenation and emoji fallback;
- display work tuned for this panel: partial refresh, anti-aliasing and page-turn effects;
- device OTA and a browser flasher.

> **Metalio E-INK4 Plus (4.7″, ESP32-S31, 1216×684 parallel panel) is an experimental port: no hardware validation and no prebuilt image yet.**
> A separate ESP-IDF 6.1 project under `ports/metalio_plus/` links the display
> (E0470A14-AF / GC16), TPS65185/TCA9555, FT6336U touch, 4-bit SD, the BQ27220 fuel gauge and
> the PCF8563 RTC. It offers four builds: BSP only, a bring-up status page
> (`CPYOS_UI_PROBE=1`), an **end-to-end reading demo** (`CPYOS_READER_DEMO=1`: SD scan → EPUB
> parse → layout → display with touch/cover-key page turns and chapter crossing), and the
> **full CPYOS activity shell** (`CPYOS_SHELL_DEMO=1`: Home/shelf/settings/apps/reader/file
> browser/sleep, with the CPY theme, fonts, i18n, and a real `esp_wifi` + `esp_tls` stack).
> 16-level text and windowed partial refresh are implemented but **nothing has been verified on
> hardware**. The 7.19 MiB app slot currently limits the shell to EN+ZH, a shared 12pt CJK face,
> and excludes games/AirPage/PixelSwitch and the web-server/Calibre pages; **OTA release
> plumbing, USB MSC and BLE are not ported yet** (protected EPUB and font extras are wired).
> It uses a different SoC and display/power topology. The `metalio_eink4` source target in
> this tree is the 3.97″ Metalio E-Ink 4 (ESP32-S3 + SSD1677 SPI), not the Plus. Do not flash
> the Read Pico firmware to a Plus.
> See [`docs/engineering/metalio-eink4-plus.md`](docs/engineering/metalio-eink4-plus.md).

## What CPYOS adds over upstream CrossMux

### UI and interaction

- Home keeps only what is read next: last book, recent books, 7-day stats and JJWXC updates. Library, find-books and My share one bottom navigation.
- Settings are grouped into category pages. Apps live in **My → Tools**; the old separate app page is gone.
- The reader uses a single title line and moves the progress percentage onto the footer wave, gaining one text line.
- Popups, wait cards, message cards, toasts and bottom sheets share one surface family. Opening a modal fades the page underneath instead of darkening it.
- Book covers use 8-bit grayscale thumbnails, and switching between main tabs no longer flashes the whole screen.

### JJWXC

- Shelf, discovery/rankings, book/author/advanced search (Chinese queries are sent as GBK) and favourite folders.
- Windowed chapter list with jump-to-chapter, chapter purchase (price confirmation and six-digit password), placeholders for unpurchased chapters and retry.
- Free chapters and purchased VIP chapters can be cached, read offline and synced.
- Paragraph and whole-chapter comments: read, like, reply and post paragraph comments. Comment bodies are cached offline and prefetched in the background while reading.
- Author notes are cached with the chapter; reading progress syncs both ways; shelf snapshots show new chapters; the last page offers to download more.
- Packaged EPUBs and cached chapters are encrypted with a device-bound key.

### Reading browser

Enter a URL and the page is fetched and re-typeset as text at the current reading size. In-page links, paging, a page menu, bookmarks and visit history; CJK pages are detected as GBK / GB18030 / UTF-16, and both HTTPS and plain HTTP open. No JavaScript, forms or images.

### Chinese text and fonts

- One scalable CJK family covering the 8,105 standard Chinese characters plus GB2312, shared by the UI, titles, JJWXC and body text.
- Separate UI and reading sizes (reading 10–40pt, narrowed automatically by panel size; fixed .cpfont files list their own sizes plus built-in min/max endpoints); richer SD-card fonts fill in rarer characters.
- Offline pinyin search and continuous pinyin input; 11 hyphenation languages loaded on demand.
- Bundled emoji glyphs for common names and titles.
- Optional font-extension packs (full emoji, extended CJK, more scripts) download to the SD card on demand; missing glyphs prompt for the right pack, and the firmware size is unchanged.

### E-ink display and page turns

- 16-level text and UI anti-aliasing, banded partial refresh and local ghost cleanup.
- Water-ripple page turns (ported from kiikoread, adjusted for this board) and fast DU page turns.
- Asynchronous pushes, forward pre-rendering, a full-page scan watchdog, an area-refreshed clock and screen repair.
- Tilt / shake page turns.

### System

- Device OTA (GitHub first, mirrors on failure, dual app-slot upgrade manifest) and SD-card firmware update.
- Data partition packs loaded on demand: pinyin, hyphenation, language pack, web assets and emoji.
- 33 UI languages; a crash report page records the reset reason and the last boot stage.
- Optional USB mass storage, browser transfer/settings, WebDAV, OPDS and Calibre wireless (from CrossMux).

## Install

Prebuilt images target the **Read Pico only**. Do not flash them to a Metalio E-INK4 Plus or any other ESP32 board: the Plus uses an ESP32-S31 with a different partition layout, panel interface and power topology. Back up the full 16 MiB flash before the first install:

```bash
python -m esptool --chip esp32s3 read_flash 0x0 0x1000000 readpico-backup.bin
```

Then use desktop Chrome or Edge on the [project site](https://yana-hangabina.github.io/cpyos/#install):

- **First install / recovery** when coming from factory firmware or CrossMux 1.6.5 (rebuilds the partition table);
- **Upgrade from CPYOS** for an existing CPYOS install (keeps settings and the data partition).

Device OTA is under **Settings → System → Check for updates**. It writes the inactive app slot and any changed data packs, and leaves the SD card, books and settings alone.

Devices on **0.2.1 or older** should use the web flasher once; their OTA manifest is on GitHub Pages, which some networks cannot reach.

Device OTA updates both the app and the data packs; a full install is only needed when the partition table or a pack format changes incompatibly. See `FLASH.md` and `SHA256SUMS` in the release bundle for offsets and checksums.

### PDF / XIP build (this branch)

The PDF-enabled build runs MuPDF from a dedicated `pdfmod` partition and uses a
**different partition table**: app0 5.5 MiB, app1 5.375 MiB, `pdfmod` 1.875 MiB
(normal builds use two 6.375 MiB slots). When switching between a normal and a
PDF/XIP image:

- **Use "First install / recovery" (a full flash)**: the web flasher's install
  button, or `python3 scripts/flash_pdf_xip.py --first-install`. Device OTA and
  the web page's "Upgrade from CPYOS" button are not valid across layouts; app1
  and the data partition would overlap.
- **Updating the PDF engine needs no reflash**: Settings -> **PDF engine** (the list that
  also holds Apps, WiFi networks and Date and time; the row only exists in XIP builds)
  -> **Check for updates**. The device connects to the saved Wi-Fi silently, fetches the
  engine manifest, downloads it and verifies size, SHA-256 **and** whether that engine
  was built for this firmware -- all before it erases anything. The SD card, library and
  settings are untouched, and the download shows real progress with a cancel control.
- Offline fallback: copy `pdfmod.bin` to the SD card root and choose "From /pdfmod.bin?"
  in the same menu.
- Three messages are protections, not faults: no `/pdfmod.bin` on the card, "Wrong
  firmware" (the engine was linked against a different app image), and a refusal while a
  PDF is open (it would replace a running engine).
- "Check for updates" depends on the maintainer publishing the manifest and blob to the
  site (recipe in `docs/engineering/pdf-xip.md`); until then the menu only reports that
  the list could not be fetched.
- On the flashing side: `scripts/flash_pdf_xip.py --pdfmod-only` rewrites the engine and
  nothing else. `--first-install/--full` now **reads the device's live partition table
  first** and refuses if that table write would move the data partition -- otherwise the
  device boots normally while pinyin, hyphenation, the language pack, web assets and
  emoji are all silently dead.
- Any app change requires rebuilding the module against the new firmware and
  flashing both (the module links to absolute host symbols).
- Text pages reflow with the EPUB reader settings (font, size, margins, line
  spacing, paragraph spacing, first-line indent and alignment). With hyphenation
  enabled, long English words break at a Liang syllable point with a visible
  line-end hyphen. Figures, tables and multi-column pages stay bitmaps; image
  pages auto-crop white margins and fit the reader's margins with their aspect
  ratio preserved.
- Encrypted PDFs prompt on the device; the password stays in RAM only. Reader
  bookmarks, notes and highlights are stored beside the book cache, and a PDF's
  own outline appears in the reader's Contents panel.
- The toolbar's Format panel opens Text Settings. Panel switching repaints only
  the chrome instead of re-rendering the PDF on every key. Long-press on reflowed
  text selects a word and uses the same action bar as the EPUB reader for
  highlights and notes.
- PDFs that need external CMaps require staging them to `/.crosspoint/cmaps` on
  the SD card first.

## Build from source

PlatformIO Core, Python 3 and Git are required. `freeink-sdk` is a pinned submodule and the display patches live in `patches/`:

```bash
git clone --recursive https://github.com/Yana-Hangabina/cpyos.git
cd cpyos
git submodule update --init --recursive

bash patches/freeink-sdk/apply.sh
python3 scripts/patch_pioarduino_cache.py --prepare-platform

# public build without JJWXC
pio run -e readpico

python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused
python3 scripts/build_web_pack.py
python3 scripts/generate_hyphenation_db.py
python3 scripts/generate_pinyin_db.py /path/to/pypinyin-0.55.0-py2.py3-none-any.whl
python3 scripts/generate_data_manifest.py
```

A build is good when the log contains `[SUCCESS]` and the image's `esp_app_desc.project_name` is `crossmux` or `cpyos` (never `arduino-lib-builder`).

Desktop simulator:

```bash
pio run -e simulator_readpico -t run_simulator
```

## Contributing

- The default branch is `cpyos`; `main` only mirrors upstream.
- Branch from `cpyos`, build, then merge.
- Never commit accounts, cookies, captured page bodies or device screenshots.
- Bug reports: include the device model, CPYOS version, reproduction steps and a redacted serial log.

Upstream sync:

```bash
git remote add upstream https://github.com/0x1abin/crossmux.git
git fetch upstream
git switch main
git merge --ff-only upstream/main
git switch cpyos
git merge main
```

## Credits

- **CrossMux**, **CrossPoint Reader**, **Inx**, **cpr-vcodex** and **diy-esp32-epub-reader** for the upstream firmware, interface work and early EPUB experiments.
- **kiikoread (read-pico-reader)** for the water-ripple page-turn engine, TEXTTURN settle, gamma cover LUT, offline pinyin search/IME and shake page-turn thresholds.
- Noto Sans CJK / Source Han Sans, FreeType, pypinyin, Lucide and the hyphenation dictionary authors.

## License

Original CPYOS code and Official Builds are licensed under the [CPYOS Project License 1.0](./LICENSE): source-available, personal and non-commercial use only. The JJWXC component is closed-source; Official Builds may include it in binary form. Third-party components keep their own licenses (MIT / Apache-2.0 / LGPL-3.0 / OFL / AGPL and others); see [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md). MIT-licensed upstream code (CrossMux / CrossPoint and others) remains available under MIT; this license does not change third-party licenses. See [NOTICE-JJWXC.md](./NOTICE-JJWXC.md) for the JJWXC support statement.

Versions released under MIT before 2026-11-06 remain under MIT. This license applies to versions released on or after 2026-11-06.

## Disclaimer

This is unofficial firmware, not affiliated with MindReset, JJWXC or WeRead. The JJWXC and WeRead endpoints are private and may change. Device traffic is encrypted but the client does not verify the server certificate or hostname; use a trusted network. Flashing is at your own risk — always keep a full-chip backup.
