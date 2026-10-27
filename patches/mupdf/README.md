# MuPDF on Read Pico: external CMaps (validated recipe)

## Why

MuPDF 1.26.3 with built-in CMaps linked **2,578,063 B** into the Xtensa probe
(`--gc-sections`, no built-in CJK fonts).  The CMap tables alone were roughly
1 MB.  Building with `-DNO_CJK` and loading CMaps from SD drops the probe to
**1,697,503 B** (`.text` 1,643,907 + `.data` 28,288 + `.bss` 25,308), a saving
of **880,560 B**.  Static RAM also falls from 97,836 B to 53,596 B.

The MuPDF source tree is **not vendored**.  Fetch
`mupdf-1.26.3-source.tar.gz` from <https://mupdf.com/releases> and apply the
two changes below.

## 1. `source/pdf/pdf-cmap-load.c`

Replace the `NO_CJK` branch with a firmware callback.  The callback is provided
by `src/CmapStore.cpp` (`crosspoint_cmap_load`), which reads
`/.crosspoint/cmaps/<name>` into PSRAM.

```c
#ifdef NO_CJK

extern const unsigned char *crosspoint_cmap_load(const char *name, size_t *size);

pdf_cmap *
pdf_load_builtin_cmap(fz_context *ctx, const char *name)
{
    if (!strcmp(name, "Identity-H")) return pdf_new_identity_cmap(ctx, 0, 2);
    if (!strcmp(name, "Identity-V")) return pdf_new_identity_cmap(ctx, 1, 2);
    if (!strcmp(name, "TrueType-UCS2")) return &cmap_TrueType_UCS2;
    {
        size_t len = 0;
        const unsigned char *data = crosspoint_cmap_load(name, &len);
        if (data && len) {
            fz_stream *s = fz_open_memory(ctx, data, len);
            pdf_cmap *cmap = NULL;
            fz_try(ctx) cmap = pdf_load_cmap(ctx, s);
            fz_always(ctx) fz_drop_stream(ctx, s);
            fz_catch(ctx) fz_rethrow(ctx);
            return cmap;
        }
    }
    return NULL;
}

#else
/* unchanged built-in table ... */
#endif
```

## 2. CMaps on SD

Copy MuPDF's `resources/cmaps/` (71 files, 2.1 MB) to
`/.crosspoint/cmaps/`.  `scripts/stage_pdf_assets.sh <SD-root> <cmaps-dir>`
does this together with `build/font_pack.dat`.

The firmware validates the name (single path component, `[A-Za-z0-9._-]`,
≤63 chars) and caps a CMap at 512 KB.  One CMap is cached in PSRAM; MuPDF
parses it immediately, so replacing the cache on the next miss is safe.

## 3. Build flags (Xtensa)

```sh
CC=$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc
AR=.../xtensa-esp32s3-elf-ar
RANLIB=.../xtensa-esp32s3-elf-ranlib

make -C mupdf-1.26.3-source -j8 OUT=build/xtensa_nocjk libs \
  CC=$CC AR=$AR RANLIB=$RANLIB \
  html=no xps=no mujs=no brotli=no extract=no barcode=no \
  XCFLAGS="-Os -mlongcalls -mtext-section-literals \
    -ffunction-sections -fdata-sections -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -DNO_CJK -DFZ_ENABLE_HYPHEN=0 \
    -DTOFU -DTOFU_CJK -DTOFU_CJK_EXT -DTOFU_BASE14 \
    -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types \
    -include $PWD/patches/mupdf/timegm_compat.h"
```

`-mlongcalls -mtext-section-literals` are mandatory: without them the Xtensa
linker reports `call8: call target out of range` once MuPDF's `.text` exceeds
the call8 window.

`-DNO_CJK` also excludes the built-in CMap headers; `-DTOFU*` excludes the
built-in CJK/base-14 fonts.  Additionally filter the base-14 CFF blobs in
`Makefile` (the `-DTOFU` path filters Noto/SIL, but not `urw/`):

```make
FONT_BIN := $(filter-out resources/fonts/urw/%.cff, $(FONT_BIN))
```

## 4. `timegm_compat.h`

newlib has no `timegm()`.  The header must include `<sys/types.h>` **before**
`<time.h>` and `#undef quad`, or the newlib `quad` macro rewrites MuPDF's
`fz_stext_char.quad` member into `quad_t` and `output-csv.c` fails to compile:

```c
#ifndef FZ_TIMEGM_COMPAT_H
#define FZ_TIMEGM_COMPAT_H
#include <sys/types.h>
#ifdef quad
#undef quad
#endif
#include <time.h>
static inline time_t crosspoint_timegm(struct tm *tm) {
    int y = tm->tm_year + 1900;
    unsigned m = (unsigned)tm->tm_mon + 1;
    unsigned d = (unsigned)tm->tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}
#define timegm crosspoint_timegm
#endif
```

## 4b. Static RAM: move read-only tables to `.rodata`

The probe's `.data` started at 28,288 B.  Three source-level `const` changes
moved 15,480 B of lookup tables into `.rodata`, leaving `.data` at 12,808 B:

| Table | Bytes | Change |
|---|---:|---|
| `agl_dup_names` | 5,960 | `static const char *` → `static const char * const` (`source/fitz/glyphlist.h`) |
| `fz_glyph_name_from_*` (7 × 256) | 7,168 | `const char *` → `const char * const` (`source/fitz/encodings.h` + `include/mupdf/fitz/font.h`) |
| `svg_predefined_colors` | 2,352 | `struct { ... }` → `static const struct { ... }` (`source/svg/svg-color.c`) |

Static RAM is now **38,116 B** (`.data` 12,808 + `.bss` 25,308).  With the
`readpico_fontpack` firmware's 89,432 B that is **127,548 B** total, leaving
roughly 61–69 KB of internal heap at boot — above the 45 KB DMA block.

`FZ_DATA` is empty on non-Windows targets, so it does not force a section; the
top-level `const` is what moves the arrays.

## 5. Full firmware link (validated)

`readpico_pdf_mupdf` (a local, never-flashed env) links `libmupdf.a` and
`libmupdf-third.a` into the firmware with:

```ini
build_flags =
  ${env:readpico_pdf.build_flags}
  -DCROSSPOINT_MUPDF
  -I<mupdf>/include
  -Wl,--undefined=pdf_probe_entry
  -L<mupdf>/build/xtensa_nocjk
  -Wl,--start-group -lmupdf -lmupdf-third -Wl,--end-group
```

`src/PdfProbe.cpp` (guarded by `CROSSPOINT_MUPDF`) is a forced-reference
probe; the real reader is `src/pdf/PdfDocument.{h,cpp}` +
`src/activities/reader/PdfReaderActivity.{h,cpp}`.  Both call
`fz_new_context` + `fz_register_document_handlers` + open/load/render, so the
PDF interpreter is in the image.

Result (with the PDF reader, `partitions_readpico.csv`, external CJK fonts, external
CMaps):

| | bytes | % of app0 |
|---|---:|---:|
| Flash | **6,825,334 / 7,536,640** | **90.6%** |
| app0 headroom | **706,048** | ≈690 KB |
| Static RAM | **127,264 / 327,680** | 38.8% |
| `firmware.bin` | 6,830,592 | — |

The reader honours `SETTINGS.readerMenuStyle`:

* **List menu**: `EpubReaderMenuActivity` with `isPdf` (Orientation, Go to %,
  Go to page, Toggle bookmark, Screenshot, Go home, Clear cache).
* **Toolbar**: `ReaderToolbarUi` with Contents (MuPDF outline when present,
  otherwise the page list), Format (page info), Bookmarks (add/remove the
  current page, jump, row-tail delete) and More (the same actions).  Confirm
  or the touch menu gesture opens it; scrub/row/tool events jump pages or run
  actions.

`Go to page` uses the mainline numeric keyboard (`KeyboardEntryActivity`,
`InputType::Ascii`); the new `STR_GO_TO_PAGE` key makes the build-time i18n
pack 1,466 keys / 336,500 B.  PDF bookmarks are stored per file as `bookmarks.bin`
(magic `PDFB`) in the page-cache directory.  MuPDF's outline is flattened to
at most 500 entries and refreshed when the PDF opens.

The status bar and page area use the same oriented margins as XTC/EPUB, and
`SETTINGS.orientation` is honoured (the menu can rotate and persist).

The reader writes an 8-bit grey BMP and drives the panel's native 16-level
path (`beginGrayscale16` → `drawBitmapGrayscale16` → `commitGrayscale16`);
if the panel has no gray buffer it falls back to the 1-bit dithered path.
Page BMPs live under `/.crosspoint/pdf_<fnv>/page_<n>.bmp` with a 4-page LRU,
and `meta.bin` (PDF size + mtime) invalidates the whole page cache when the
source changes.  `BookCacheUtils` now recognizes the `pdf_` prefix, so
clear/rename/move operations include PDF caches.

`esp_app_desc.project_name == crossmux`, `version == 870d7c2b-dirty`.
`pdf_probe_entry`, `fz_open_document`, `crosspoint_cmap_load` are all in the
ELF.  OTA is preserved: `app1` is 7.19 MB, and `spiffs` is 1.5 MB.

Flashed on 2026-10-23 (full backup
`device/readpico_full_backup_20261023_001547.bin`, sha256
`aa924e2ceda62ed1e85e1fe05d187619b645f63999fb72f6d3c65b14180703e2`):
`0x8000 partitions.bin`, `0x10000 firmware.bin`, `0xE70000 pinyin.dat`,
`0xF00000 hyphen.dat`, `0xF70000 i18n_pack.dat`.  All five images reported
`Hash of data verified.`; `app1`, `nvs`, `otadata` and `coredump` were not
written.  The PDF env embeds the web assets, so no web pack is flashed.  The
device booted to the heartbeat with no `[ERR]`.

On 2026-10-23 PDF assets are provisioned from a one-time app1 carrier:
`build/pdf_full_assets.bin` (1,485,684 B) contains the full font pack
(1,317,348 -> 703,233 B raw deflate) and all 71 CMaps (782,431 B `CMPS`).
It is flashed at app1 offset `0x1000` (flash `0x741000`).  On first boot
`PdfAssets.cpp::ensurePdfAssets()` reads it, writes the full
`/.crosspoint/font_pack.dat` (cjk12+title) and all 71 CMaps, then writes
`/.crosspoint/pdf_assets_v2`.  Older SD packs are replaced when the marker is
absent.  This is full CJK and full title; i18n remains the complete
33-language spiffs pack plus EN/ZH, so neither is reduced.

Build/flash numbers: Flash **6,827,470 / 7,536,640 (90.6%)**, app0 headroom
**709,170 B**, `firmware.bin` **6,832,736 B**, RAM **106,160 B**.  app1 remains
7.19 MB, so OTA still fits with about 704 KB margin.  Flash command:

```bash
write-flash 0x10000 .pio/build/readpico_pdf_mupdf/firmware.bin \
            0x741000 build/pdf_full_assets.bin
```

`build/pdf_full_assets.bin` is a build artifact.  Regenerate it with
`python3 tools/gen_pdf_assets.py` before building; it needs the full
`build/font_pack.dat` and all files in `build/cmaps/`.

The carrier lives in the OTA slot: a later OTA overwrites it, but by then the
SD copy is already installed.  If an OTA happens before the first boot, reflash
the carrier (or use the network-download hook).  `PdfAssetDownload.cpp` keeps
the first-boot network path compiled but disabled because no release URL exists
yet.

To make the web server survive memory pressure, MuPDF and third-party `.bss`
sections are renamed to `.ext_ram.bss` and linked into PSRAM; discovery UDP
failure is also non-fatal (`CrossPointWebServer.cpp`).  The current boot
reports internal free **72,435 B** and PSRAM **4.65 MB** after full-font load.

**Important:** without `fz_register_document_handlers()` the probe links only
**767,440 B** — the PDF interpreter is then dead-stripped and the measurement
is meaningless.  Always check `fz_open_document` is present in the ELF.

**Build gotcha:** MuPDF's `barcode.cpp` / `tessocr.cpp` use `CXX`; if it is not
set, the host `clang++` receives `-mlongcalls` and fails.  Pass
`CXX=$TC/xtensa-esp32s3-elf-g++` and `tesseract=no` (see the build script in
this directory).

## 6. Status and remaining work

The PDF reader is implemented in `src/pdf/PdfDocument.{h,cpp}` and
`src/activities/reader/PdfReaderActivity.{h,cpp}` (8-bit gray BMP, native
16-level path with 1-bit fallback, 4-page BMP cache).  The current image is
`project_name=crossmux`, Flash **7,455,202 / 7,536,640 (98.9%)**, app0
headroom **81,438 B**, `firmware.bin` **7,460,464 B**, RAM **127,264 B**.
`app1` remains 7.19 MB, so OTA still fits (76,176 B margin).

First-boot provisioning is in `src/pdf/PdfAssets.cpp`:
`pdf_font_part.bin` (628,233 B, cjk12-only raw deflate) is linked into app0;
`gb_cmaps.pack` (202,744 B) lives in spiffs at `0x55540` (flash `0xEC5540`).
`ensurePdfAssets()` runs from `ExternalFontPackStorage` and writes
`/.crosspoint/font_pack.dat` (1,114,324 B) plus 17 GB CMaps.  Existing SD files
are not overwritten.  After the first boot the PSRAM free figure dropped from
6.02 MB to 4.87 MB, consistent with the cjk12 pack being loaded.

Remaining:

* **User test** of “文件传输 → 加入网络”: discovery UDP is now optional; the
  serial log should show the warning followed by `Web server started
  successfully`.  If it still fails, the next step is to move FreeType/MuPDF
  `.bss` tables (`FT0–3`/`RT0–3`/`get1_tab_*`/`skip_buf`, ~20 KB) to PSRAM.
* **First-boot provisioning on a blank SD**: remove
  `/.crosspoint/font_pack.dat` and `/.crosspoint/cmaps/`, reboot, and expect
  `[PDF] installed font pack` / `[PDF] installed 17 GB CMaps`.  The installer
  writes from the main task; it never writes the data partition.
* **PDF UI validation**: open/scroll PDFs, 16-level gray, toolbar/Contents,
  bookmarks, page jump, screenshot.  The reader has not been exercised on
  hardware beyond the boot/PSRAM checks.
* **CMap coverage is GB-only by design.**  All 71 MuPDF CMaps need about
  1.46 MB compressed versus 1.24 MB free across app0+spiffs; full coverage
  would require dropping the i18n pack or shrinking the app slots.  Japanese/
  Korean CMaps are therefore absent unless that repartitioning is accepted.
* **Title face**: the cjk12-only pack has no `title` entry; the loader copies
  cjk12 into the title slot.  UI section titles are slightly smaller, but PDF
  text and the rest of the reader are unaffected.
* The 16-level commit promotes the next refresh to FULL (ghosting cleanup), so
  page turns are slower than the 1-bit fallback; measure on device.
* `src/PdfProbe.cpp` is now only a forced-reference link probe and can be
  removed after the reader is validated on hardware.

