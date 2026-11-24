# PDF engine in the `pdfmod` flash partition (XIP)

## Why this shape

The SD code-paging prototype failed its gate: ESP32-S3's MMU can map PSRAM
pages as executable, but an unmapped I-code page does not reach a recoverable
Xtensa exception handler and `mmu_hal_unmap_region` does not invalidate the
cached ITLB entry.  MuPDF therefore runs XIP from a dedicated flash partition
instead of from SD.

`[env:readpico_pdf]` keeps the whole MuPDF engine out of the app slot.  The app
only contains the PDF reader UI and a small loader (`src/pdf/PdfModuleRuntime`),
so its size and flash margin stay close to the non-PDF firmware.

## Partition table

`partitions_readpico_xip.csv` (used only by `readpico_pdf`):

```
nvs       0x9000    0x5000
otadata   0xE000    0x2000
app0      0x10000   0x580000  (5.5 MiB, unchanged)
app1      0x590000  0x560000  (5.375 MiB, OTA staging)
pdfmod    0xAF0000  0x1E0000  (1.875 MiB, custom data subtype 0x40)
spiffs    0xCD0000  0x320000  (same base and size as the normal table)
coredump  0xFF0000  0x10000
```

The normal `partitions_readpico.csv` keeps 6.375 MiB app slots.  app0 is
unchanged so the running slot has the same room as before; the 0.125 MiB taken
for the module comes from app1, which still fits the 5.50 MB firmware with
~130 KB spare.  `spiffs` is at the same address in **these two files**, so moving
between the layouts *in this tree* does not move the pinyin/hyphen/i18n/web/emoji
blobs or the core CJK font.

★★ That sentence is about the repository, and it does NOT generalize to a device.
The blobs are read at absolute offsets inside `spiffs`, so what matters is the table
**currently on the flash**, which is whatever the last flasher wrote -- often another
branch's layout.  Measured on the one real device on 2026-11-19:

| | `spiffs` base | size |
|---|---|---|
| device as found (dev/public layout) | `0xE70000` | 1.5 MiB |
| this tree's XIP table | `0xCD0000` | 3.2 MiB |

So `--first-install` from this tree onto that device **moves and resizes** the data
partition.  Doing it without `--full` is AGENTS.md section 29.1's failure verbatim:
the app boots, esptool says `Hash of data verified.`, and pinyin / hyphenation /
the 33-language pack / web assets / emoji are all silently dead.

`scripts/flash_pdf_xip.py` therefore reads the device's live table before any table
write and refuses when `spiffs` would move (`--skip-layout-check` to override).  It
is not theoretical: the first time it ran, against real hardware, it produced
exactly that refusal -- which is also how the numbers above were obtained.  The read
uses `--after hard-reset`, because this board has no BOOT/RESET button and being left
in the ROM bootloader would need a physical power cycle.

## Module layout

`build/pdfmod.bin`:

```
0x00000  PdfModuleHeader (64 KiB sector; 80 header bytes used)
0x10000  .text (+ .literal)          linked at kPdfModuleCodeVaddr (0x42D20000)
+code    .rodata                     linked at 0x3DF00000
+rodata  .data                       linked at 0x3D800000
+data    .bss (zeroed by the loader) linked at 0x3D820000
```

- Code is mapped read/execute from flash with `esp_mmu_map`, which performs the
  MMU write, cache-bus enable and I-cache invalidation in one call.  The
  allocator must hand out exactly `kPdfModuleCodeVaddr` (`src/pdf/PdfModuleApi.h`);
  the loader fails closed otherwise.  This address is **app-size dependent**: the
  allocator returns the first free 64 KiB-aligned window above the app's static
  `ext_ram`, so when the app image crosses a 64 KiB boundary the window moves (it
  shifted `0x42D30000 -> 0x42D20000` when the selection/merge/engine work changed
  the app).  `scripts/build_pdf_module.py` now parses `kPdfModuleCodeVaddr` from
  the header and links `.text` there, so the app loader and the module image can
  no longer disagree silently — after any app rebuild, rebase the header constant
  to the value the running app reports (`CMD:PDFMODTEST` prints `code mapped at
  0x...`) and rebuild the module.
- .rodata is mapped raw into DROM at `0x3DF00000` (string/table reads).
- .data/.bss live in PSRAM.  They are mapped **raw** at `0x3D800000`, well past
  the app's static `ext_ram` and the PSRAM heap.  `esp_mmu_map` used to choose
  that address, but its first-fit result moves when the app image changes, and
  a module linked for a fixed address cannot follow it.
- The app copies .data from flash through a 512-byte scratch buffer, zeroes
  .bss, flushes the cache, then calls `pdfmod_entry`.
- ABI 3 appends `set_paragraph_split`, which carries the reader's 智能合并断行 option
  into the extractor.  Ops are appended at the END and the loader zero-initialises the table
  (`PdfModuleRuntime`'s `PdfModuleOps g_ops{}`), so a module predating a trailing field
  leaves it null and the app's null-check skips it -- the bump is a loud refusal on top of a
  layout that is already safe.  ★ `scripts/build_pdf_module.py` used to carry
  `ABI = 2  # must match kPdfModuleAbiVersion`; when the constant moved the literal stayed,
  so the image was stamped 2 and the pair would have been refused *on device*.  The builder
  now parses MAGIC/VERSION/ABI out of this header (`_parse_header_int`), `check_pairing()`
  compares the image's `abi` before flashing, and
  `scripts/tests/test_pdf_module_abi.py` fails if a literal comes back or the comparison
  disappears from either side.
- ABI 2 adds the password-aware `open(path, password)` and
  `needs_password`/`wrong_password` ops.  An older ABI-1 module is rejected by
  the header check; rebuild the module after any app-side ABI change.

## Host symbols: only what the module does not implement

The module is linked against `firmware.elf` so libc/HAL/logging resolve to
absolute host addresses.  It does **not** use `--just-symbols` blindly: the
build script reads the app's global symbols and the two MuPDF archives, then
emits `PROVIDE(name = address)` for every app symbol that the archives do not
define.

This matters because CrossPoint also links FreeType and zlib (EPUB fonts and
zip/deflate).  With `--just-symbols`, MuPDF's `inflate`/`FT_*` calls resolved to
the app's *different* builds, whose private structures and configuration do not
match: flate streams and text extraction corrupted the ESP heap ("Bad tail")
and large PDFs died in `pdf_load_xref`.  The module now always uses its own
FreeType and zlib; libc, newlib and HAL calls still go to the host.

Cost: about 250 KB more module code (FreeType + zlib), which is why the module
partition is 1.875 MiB.  The app slot is untouched by that growth.

## Build

```bash
pio run -e readpico_pdf
python3 scripts/build_pdf_module.py --firmware-elf .pio/build/readpico_pdf/firmware.elf
python3 -m unittest scripts.tests.test_pdf_module
```

The script compiles `src/pdf/PdfDocument.cpp` and `PdfModuleEntry.cpp` with
`-DCROSSPOINT_PDF_MODULE=1 -DPdfDocument=PdfModuleDocument`, links the prebuilt
`libmupdf.a` / `libmupdf-third.a` at the fixed addresses above, and emits the
header/code/rodata/data image plus `build/pdfmod.elf` for crash symbols.

Current sizes (2026-11-18):

| item | size |
|---|---:|
| module code (`.text` + `.literal`) | 1,134,176 B |
| module rodata | 580,304 B |
| module data + bss | 28,296 + 25,340 B |
| `build/pdfmod.bin` | 1,863,304 B (fits 1,966,080 B) |
| app `firmware.bin` | 5,515,088 B (fits 5.5 MiB app0 / 5.375 MiB app1) |
| app RAM | 89,664 B / 327,680 B (27.4%) |
| app flash (linker) | 5,509,819 B / 5,767,168 B (95.5%) |
| final image SHA-256 | app `7d7204af9f7487e9707992cc0f1ad46da6601b937ee46ef5746f367c4443536c`; module `fcaf92cc98c907ae921b57f2a09be1328c0b13c23f8448a3df8e18f573a36396` |

The final pair is archived in `artifacts/pdf-xip/readpico_pdf_firmware_final_20261118.bin`
and `artifacts/pdf-xip/pdfmod_final_20261118.bin` with `SHA256SUMS.final`.  The app
reports `project_name=crossmux`, `version=0.7.0-rp` in this dev tree.

### Compatibility classes (image v2)

The engine links **no app symbols**.  `pdfmod_entry(host, ops)` receives a
versioned host-service table (`src/pdf/PdfModuleHost.h`: heap, logging, storage,
CMap and font-cache hooks), and the module carries its own runtime glue
(new/delete, malloc routed to the host heap, the libc entry points it needs).
That removed the old 20,790 absolute-symbol bindings, which made every rebuild
move addresses and orphan a perfectly good engine.

Compatibility is now a property of the contract, not of the build:

| field | meaning | changes when |
|---|---|---|
| `version` (2) | image/header layout | the container changes |
| `abi` (4) | `PdfModuleOps` table layout | an op is added (append-only) or `pdfmod_entry` changes |
| `hostApi` (major.minor) | `PdfModuleHost` table | a service is added (minor) or its signature changes (major) |
| `layoutKey` | fixed VADDRs + bss gap | the module memory map changes |
| `buildId[32]` | sha256(firmware.elf) | every build; **provenance only** |

`pdf_header::check()` compares the first four; `buildId` is never compared at
load time.  A rebuild that changes no contract keeps the installed engine valid;
only a deliberate contract change (or a missing/broken image) triggers an update.
The layout key is a literal in `PdfModuleApi.h` pinned by `static_assert` against
the constants, and the build scripts read that literal rather than re-deriving it.

`scripts/flash_pdf_xip.py` still requires the flashed app and module to come from
the same ELF (build id) -- a hand flash is one unit -- but the device does not.

### The code window is a moving target

`esp_mmu_map` picks the address, the module must be *linked* at it, and the pick
depends on how much IROM the app occupies.  It has moved several times
(`0x42D30000 → 0x42D20000`, back, and on 2026-11-24 to **0x42D50000** when the
host-table/engine-work app grew).  The loader records the chosen address and the
reader can show it (diagnostic builds); when it differs, update
`kPdfModuleCodeVaddr`, rebuild **both** images and reflash.  This is a
flash-XIP-only constraint; the Plus PSRAM engine is unaffected.  The symptom is unmistakable and safe:

```
[ERR] [PDFM] code map err=259 at 0x42d30000 (need 0x42d20000)
[INF] [PDFM] PDFMODTEST ok=0 ops=0x0
```

Fix: set `kPdfModuleCodeVaddr` in `src/pdf/PdfModuleApi.h` to the address the
loader just reported, rebuild the **app** (that constant is compiled into the
loader too, so the appTag changes), rebuild the module (it now reads the address
from the same header, so the two cannot drift), regenerate the manifest, flash
app0 + app1 + pdfmod, and confirm `code mapped at 0x…` + `engine loaded` +
`PDFMODTEST ok=1`.

## Updates only when incompatible -- and as a continuation, not an error

`pdf_module::ensureLoaded()` records a typed `Refusal`; the reader distinguishes
three outcomes:

* `Incompatible` / `NoPartition` / `BadHeader` -- fixable by installing the right
  engine.  The reader **opens `PdfEngineUpdateActivity` immediately**, with
  `returnOnSuccess=true`; the activity silently associates with the last saved
  network, downloads the newest matching manifest entry, verifies it and installs
  it, then pops and the reader retries `loadBook()` at the same position.  The
  user sees a progress card, not an error, and does not need a second tap.
* `MapFailed` / `NoPsram` -- this boot's memory state.  A download cannot help;
  the reader shows the memory card (restart advice) and does not auto-update.
* Anything else (the file itself) keeps the unreadable-file card.

If the update cannot run (no saved network, fetch failure), the reader falls back
to a card titled with `STR_PDF_ENGINE_MISMATCH` (now worded "PDF engine needs an
update" / 「PDF 引擎需要更新」) plus `STR_TAP_TO_RETRY`; tapping it opens the
update again.  No new i18n keys were added -- the EN blob is at its 15-bit ceiling.

## Engine update without re-flashing

The engine answers the user's question directly: **an engine change never needs a
web flash**.  `pdfmod` is its own data partition, so it can be replaced while the
bootloader, partition table, app slots and the spiffs data blobs stay untouched.
Two doors, one rule (the image must have been built against *this* firmware ELF):

**1. Download it on the device** -- Settings → 应用与维护 → 「PDF 引擎」 →
**检查更新** (`STR_CHECK_UPDATES`).  `PdfEngineUpdateActivity` then:

- reuses a live Wi-Fi session, or associates silently with the last saved
  network (same recipe as the browser, 15 s bound, `NetworkStartup::setMode` so
  BLE stops and render memory is reclaimed first);
- runs `pdf_engine::startUpdate()`: a 16 KB **PSRAM-stack** worker holding a
  `background_work::Guard` + a quiet 240 MHz lock + `NetworkStartup::suspendTimeSync()`
  (never start DNS with a pending SNTP callback), which fetches
  `CPYOS_PDF_ENGINE_MANIFEST_URL`, checks the manifest's `appTag` against the
  running image **before** downloading, then downloads to
  `/.crosspoint/pdf_engine/download.bin` and verifies size + SHA-256;
- the **activity's own task** (internal stack) performs the partition erase and
  write once the download lands -- `esp_partition_write` with the stack in PSRAM
  trips `esp_task_stack_is_sane_cache_disabled()` and restart-loops;
- shows one shared `status_card` wait card the whole time with a real figure
  (检查更新 → 下载中 `1.2 / 1.9 MB` → verify → install) and a Cancel pill;
  Back cancels at the same boundary.  Cancelling never deletes the worker
  mid-heap -- `onExit` asks it to stop and waits up to 30 s.
- ends on a `status_card` message (完成 / 更新失败 + the localized reason) that
  pops back to Settings on any input.

Failure reasons reuse existing generic strings (`STR_FONT_EXTRAS_TIMEOUT`,
`_NETWORK`, `_BAD_MANIFEST`, `_VERIFY_FAILED`, `_STORAGE`,
`STR_PDF_ENGINE_MISMATCH`, `STR_WIFI_CONN_FAILED`, `STR_DOWNLOAD_FAILED`) -- the
EN i18n blob is at its 15-bit/32,767 B ceiling, so no new key was affordable.
The module's developer-facing English detail goes to the log only.

**2. Install from the SD card** -- the same row's second option (「从
/pdfmod.bin 安装？」).  `pdf_module::installFromSdFile()` inspects the header
*first* and returns a typed `InstallStatus` (`NotFound` / `BadHeader` /
`Mismatch` / `IoError`) **before** erasing anything, so a wrong download cannot
remove a working engine.

Supporting pieces (all host-tested):

- `src/pdf/PdfEngineManifest.h` -- `url<TAB>size<TAB>sha256hex<TAB>appTaghex`,
  https-only, plus `isForRunningImage`.  `test/pdf_engine_manifest/` (19 checks
  + a mutation on the https-only rule).
- `pdf_module::runningAppTag(out)` -- the device's own `0xb0` ELF SHA, so a
  mismatched blob is refused without downloading 1.8 MB of it.
- `src/pdf/PdfEngineUpdate.{h,cpp}` -- `Failure` / `State` / `Progress` and the
  worker above; `progress()` publishes stage + bytes for the wait card.
- `scripts/build_pdf_engine_manifest.py` -- emits the manifest with
  `appTag = sha256(firmware.elf)`, which equals the tag `build_pdf_module.py`
  stamps into `pdfmod.bin`; builder ↔ manifest ↔ device parser share one
  invariant.
- `CMD:PDFENGUPDATE` drives the same code path from the serial console.

### Device evidence (2026-11-19 build, `readpico_pdf`)

- Settings/serial path entered and exited cleanly twice with **no usable Wi-Fi**:
  `Entering activity: PdfEngineUpdate` → `engine update requested:
  manifest=https://github.com/…/releases/download/pdf-engine/…manifest-v2.txt` → silent association →
  `saved network connect failed` → `[NET] releasing radio (mode=1)` →
  `radio released, mode=0`.  The failure card is on screen (`artifacts/pdf-xip/
  pdfeng_fail_card.png`: one 560 px framed card over a scrim-erased page), Back /
  HOME pop it, `Free 53,755 / MaxAlloc 18,420`, PSRAM 4.47 MB, 0 panics.
  The loop spike it logs (`New max loop duration: 15234 ms`) is the association
  window by design -- the card is presented before it (`requestUpdateAndWait`).
- The download/verify/install-with-bytes branch could **not** be exercised here:
  the saved AP would not associate this session, and
  the rolling index has no entry yet.  A published index and blob are needed for a
  real end-to-end run; `--check` below is what keeps that run honest.
- `--check` proved itself the moment it existed: after an app rebuild it refused
  to flash (`manifest … does not match these inputs`, exit 1) because the
  manifest still carried the previous ELF's appTag.

### Publishing an engine build (maintainer recipe)

Engine images and the rolling index are release assets, not Pages files: a
stable URL is what lets old firmware keep finding matching engines.  The site
builder only refuses an XIP firmware with no engine directory at all.
land at the exact path `CPYOS_PDF_ENGINE_MANIFEST_URL` points at.

The site builder now **verifies those two files instead of trusting the person
who staged them** (`check_pdf_engine`, 13 host tests in
`scripts/tests/test_pdf_engine_site_gate.py`): the blob must exist beside the
manifest, its size and sha256 must match, every line is checked, the appTag field
must be a full digest and must equal the appTag in the blob's own header, and --
the part no other tool can do -- it must equal the ELF SHA at `0xb0` of the
firmware image published in the same release, which is exactly the equality the
device enforces before it will run an engine.  A staged `pdf-engine/` with no
findable app image is a **failure, not a skip**: "cannot prove the pair" is the
case most likely to be published by accident.

The mirror of that rule also fails closed now: if the firmware being published **is**
an XIP build but `site/pdf-engine/` was never staged, the build stops.  Without it, the
ordinary accident was to release a PDF-capable app, forget the engine directory, and
leave every device on that firmware unable to update PDF rendering -- `检查更新` cannot
fetch a list that does not exist.  An absent directory stays legal for non-XIP
releases.  The classifier keys on one string that lives only inside the XIP-gated
loader -- ★ **not** on `/pdfmod.bin`, which is the text of `STR_PDF_ENGINE_INSTALL`
and therefore rides inside the embedded translation blob of *every* firmware; using it
would have made this gate refuse all ordinary releases.
`test_translation_text_is_not_an_xip_marker` pins that trap against the real yaml.  An absent `pdf-engine/` directory
is not an error, so non-PDF releases are unaffected.  Because both
`cpyos-release.yml` and `pages.yml` run this script, a stale blob beside a fresh
manifest can no longer be published.

★ That last check earned its keep on its first run against the real files in this
tree: the staged `pdfmod.bin` carried appTag `caf0efdb…` while the freshly built
app carried `a682921e…`.  Nothing was wrong with the manifest -- it agreed with
its own blob -- the pair was simply from an older build, and publishing it would
have shipped an update that every device on the new firmware refuses to install.
The rule this encodes is the one from section 3 of the crash incident: **an engine
is paired to one exact app image, so staging the engine is part of publishing the
app, never a separate afterthought.**

```bash
pio run -e readpico_pdf                       # fresh PLATFORMIO_BUILD_CACHE_DIR, see section 4.2
python3 scripts/build_pdf_module.py --firmware-elf .pio/build/readpico_pdf/firmware.elf \
    --layout flash --mupdf-libs patches/mupdf/prebuilt
# Publish one entry into the rolling index (keep every other compatibility class):
python3 scripts/build_pdf_engine_manifest.py \
    --firmware-elf .pio/build/readpico_pdf/firmware.elf --pdfmod build/pdfmod.bin \
    --url https://github.com/Yana-Hangabina/cpyos/releases/download/<TAG>/readpico-pdfmod.bin \
    --merge-into site/pdf-engine/manifest-v2.txt \
    --out site/pdf-engine/manifest-v2.txt
```

Upload the blob to the release and merge the entry into the rolling index;
**run the publish gate before committing/publishing** --
`verify_release_artifacts.py --pdf-firmware` re-derives the pairing from the firmware image
itself (the ELF SHA at offset 0xB0), so it catches the one mistake that is otherwise
invisible here: a rebuilt engine has the *same size* as the previous one (measured: 1,863,304
bytes both times), so a stale `site/pdf-engine/pdfmod.bin` looks perfectly normal in a diff,
in a Content-Length header and on the download page, and is only refused after the user has
waited through a ~2 MB download.  That exact state happened while this feature was in
development, which is why the check exists and why `--pdf-firmware` is mandatory whenever an
engine is staged (`scripts/tests/test_pdf_engine_publish_gate.py`, 9 cases).

```bash
# Blob: upload to the release (asset name is what the entry URL points at).
# Index: merge the entry into the rolling `pdf-engine` release asset.
gh release upload <TAG> build/pdfmod.bin --clobber
gh release download pdf-engine --pattern readpico-manifest-v2.txt --dir /tmp \
  && python3 scripts/build_pdf_engine_manifest.py ... --merge-into /tmp/readpico-manifest-v2.txt --out /tmp/readpico-manifest-v2.txt \
  && gh release upload pdf-engine /tmp/readpico-manifest-v2.txt --clobber
python3 - <<'EOF'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location("vra", "scripts/verify_release_artifacts.py")
vra = importlib.util.module_from_spec(spec); sys.argv = ["x"]; spec.loader.exec_module(vra)
print(vra.check_pdf_engine(pathlib.Path(".").resolve(),
                           ".pio/build/readpico_pdf/firmware.bin", True))
EOF
```

A CI version of this gate needs `readpico_pdf` in the release pipeline (see the note at the
end of this section); until then it runs at staging time, and refuses outright when an engine
is staged without the flag.

The published index lives on the **rolling `pdf-engine` release**, not on a
particular version's asset: firmware compiles `CPYOS_PDF_ENGINE_MANIFEST_URL`
to that URL and therefore keeps reading it years later.  Each publish uploads
the new block (immutable release asset) and updates the index with
`--merge-into`, which replaces only the same compatibility class.  That is what
lets a device flashed with older firmware still receive the newest engine ever
built for its class.  The Plus release workflow does exactly this
(`plus-manifest-v2.txt`); for Read Pico the same steps are manual today because
`readpico_pdf` is not in the release pipeline.

## Flash and update

```bash
# Routine app + engine flash (partition table untouched)
python3 scripts/flash_pdf_xip.py --port /dev/cu.usbmodem101

# First XIP install or partition-table change
python3 scripts/flash_pdf_xip.py --port /dev/cu.usbmodem101 --first-install

# First install including the spiffs data blobs
python3 scripts/flash_pdf_xip.py --port /dev/cu.usbmodem101 --full

# Engine-only update (no app slot is erased)
python3 scripts/flash_pdf_xip.py --port /dev/cu.usbmodem101 --pdfmod-only
```

- **The two layouts are not interchangeable without a partition-table write, and
  that is a user decision.** As of 2026-11-19 the main device is on the normal
  layout (`partitions_readpico.csv`, `Starting CrossPoint version 0.7.0-cpyos`,
  bootloader read back as factory `9812237f…e0c8`), because of the crash recorded
  in `pdf-xip-psram-init-incident-20261119.md`. `partitions_readpico_xip.csv`
  shares `nvs`/`otadata`/`spiffs`/`coredump` with it but moves `app1` and inserts
  `pdfmod`, so going to XIP is a `--first-install` action and going back rewrites
  `0x8000` again; either direction also re-lays the data blobs. Do not "just try"
  it on the machine someone else is testing.
- `--first-install` also writes bootloader/partition table/boot_app0; routine
  flashes must not rewrite them. **A custom bootloader is never the fix for a
  PSRAM problem** -- `nm` shows zero PSRAM symbols in `bootloader.bin`, the
  initialisation lives in the app and the Arduino core, and the bootloader is the
  one region this board cannot recover by button (no BOOT/RESET keys; the PMU
  owns BOOT).
- `--pdfmod-only` is the intended engine update path.  A new MuPDF build can
  ship without touching the app or the data partition.
- On-device: copy `pdfmod.bin` to the SD card and run
  `CMD:PDFMODINSTALL /pdfmod.bin` from the serial console with the PDF reader
  closed.  It erases and rewrites only the `pdfmod` partition.
- **No reflash needed for an engine-only change** (the common case the user
  asked about).  A new MuPDF build for the same app has an unchanged `appTag`,
  so the user copies `pdfmod.bin` to the SD root and taps
  **Settings → 应用/维护 → 「PDF 引擎」 (PDF engine)** → 确认.  The device reads
  `/pdfmod.bin`, checks the header and the running app's ELF SHA, and only then
  erases and rewrites the `pdfmod` partition — the bootloader, partition table,
  app slots and the data partition are untouched.  A blob built for a different
  firmware is refused (message "Engine: wrong firmware") before the good engine
  is erased, so a mistaken download cannot brick the reader.  This is the
  `readpico_pdf` build only; the row and its dispatch are guarded by
  `CROSSPOINT_PDF_XIP`.
- Verified on-device via `CMD:PDFMODINSTALL`: an absent file reports `status=1
  (NotFound)`, a non-module file (e.g. `/test.pdf`) reports `status=2
  (BadHeader)`; both return **before** `unload()`/erase, so a bad choice never
  disturbs the engine already running.  A blob whose `appTag` differs reports
  `status=3 (Mismatch)`, also pre-erase.  `status=5 (Busy)` means a
  `PdfDocument` still holds the engine; also pre-erase.
- **Engine replacement refuses while a document is open.**  `unload()` unmaps
  the code window, and MuPDF keeps function and object pointers inside it, so
  unmapping under a live document is a crash rather than an error.  This used to
  be a comment on the caller ("must close every PdfDocument first"), which is
  the shape this project has been burned by repeatedly.  `PdfDocument` now
  counts every handle transition (`noteDocumentOpened/Closed`, a relaxed atomic
  because documents open on both the UI task and the PDF worker) and
  `installFromSdFile()` returns `InstallStatus::Busy` before it erases when the
  count is non-zero.  `unload()` has exactly one caller -- the install path --
  so the counter covers the whole hazard, and an unbalanced close is logged
  instead of silently underflowing the count.
- Because the engine binds absolute host symbols, an **app/OTA update that moves
  those addresses leaves the old engine refused at load** (the reader falls back
  to "no engine").  The fix is the same device-side row: fetch the paired
  `pdfmod.bin` for the new firmware and install it — still no USB reflash.
- `CMD:PDFMODTEST` loads the engine and prints its entry/ABI result.

OTA notes:

- The XIP partition table is not the release table.  A PDF-capable firmware
  must be installed and updated as the XIP image (USB or SD firmware update);
  a normal release OTA would write a firmware that expects the normal table.
- **Validation happens before the erase, not only at load.** `src/pdf/PdfModuleHeaderCheck.h`
  is a pure predicate over the 80-byte header (magic, version, abi, `codeOffset`, non-empty
  code, section ordering, entry inside code, an absurd `.bss`, and the two containment rules
  `dataOffset + dataSize <= fileBytes` and `<= partitionBytes`).  `inspectSdHeader()` -- which
  both the SD installer and the download installer call -- refuses on any of those, so the
  partition is never wiped for an image that the loader would reject anyway.  Before this the
  pre-erase check looked only at magic/version/abi/`codeSize`, so an image whose header was
  intact and whose payload was short (interrupted copy, spliced download) passed inspection,
  erased the only working engine, and then failed the loader's CRC -- on a board with no
  BOOT/RESET button, which means a full web flash to recover.  Pinned by
  `test/pdf_header_check/` (34 checks, 5 mutations), which asserts both that the **real**
  `build/pdfmod.bin` passes and that the same file truncated by one byte is refused.
- The app validates the module header (`magic`, `version`, `abi`, `appTag`
  against `CROSSPOINT_VERSION`) and its CRCs.  A stale module is refused
  instead of jumping into moved host symbols.
- The module still binds to absolute host symbols, so **rebuild it after any
  app change** and ship both together (or bump `CROSSPOINT_VERSION` so the
  old module is rejected).  A future runtime host-API table would remove that
  coupling.
- This branch has **no device-side OTA gate for a layout change**.  A normal
  OTA can only write app slots and will silently keep the old partition table,
  so the PDF image must be published as a full install until the merge
  checklist in `docs/cpyos/release.md` ("破坏性布局变更…") is implemented:
  a layout id / `fullInstallRequired` in the manifest, device-side comparison,
  site upgrade-button suppression, and tests.  A migration image that rewrites
  bootloader/partition table over OTA is not safe (no rollback if power fails
  mid-write); keep "full flash required".

### A verified digest is not a verified boot (2026-11-19)

`Hash of data verified.` only proves the bytes landed.  On 2026-11-19 an app +
engine flash verified on both slots and the device then produced no usable
boot: the console stopped at `entry 0x403c8898`, and the boot immediately
before it had logged

```
[283] [ERR] [HYPH] no PSRAM headroom for a 26943 byte trie      (all 11 languages)
[678] [ERR] [I18N] no PSRAM for the 1088-byte pack index
[518] [INF] [SYS] Dumped panic info to SD card
```

A 1 KB PSRAM allocation failing means there is no PSRAM heap at all -- which is
a different image than the one under test, and the host had no way to know.
Two things were wrong in the process, not in the image:

1. `esp_app_desc.project_name` was **`crossmux`**, so the AGENTS.md section 6
   vendor-core signature did *not* apply here.  Do not reach for that
   explanation again on this evidence; the check is still worth having, but it
   cannot be the proof of a healthy build.
2. Nothing read the first boot.  The flash was declared good on esptool's
   output alone.

`scripts/flash_pdf_xip.py` now closes both gaps: it refuses to flash an image
whose `project_name` is `arduino-lib-builder` (`--allow-vendor-core` overrides
deliberately), and after writing it resets the device, reads the console for
`--boot-seconds` (default 20) and exits non-zero on a missing PSRAM heap, a
panic dump, a reset loop (`rst:0x` three or more times), or a boot that never
reaches its `[BOOT] boot Nms` line.  An unusable port is reported as
"could not attach", never as "the device is dead" -- a serial monitor holding
the port is the common cause (`pkill -f mon_device.py`).

Host regression, with the mutation step that proves the loop gate is real:

```bash
bash test/pdf_flash_gates/run_flash_gates_test.sh    # 9 checks + mutation
```

The ROM/second-stage banner shapes in that test were copied from
`device_log/live.log`, not written from memory.


### `PSRAM free=0` after an XIP crash: initialization state, not hardware (2026-11-19)

> **Corrected 2026-11-19 (device evidence).** This section's hardware/power
> conclusion is wrong.  The PSRAM chip and the 80 MHz/120 MHz timing are fine;
> what happened was an XIP engine crash followed by a PSRAM
> initialization-state failure that only a full power-off cleared.  The crash's
> own cause is **still open** -- the `.ext_ram.bss` story raised during the
> investigation was tested and refuted.  See
> [`pdf-xip-psram-init-incident-20261119.md`](pdf-xip-psram-init-incident-20261119.md)
> for the evidence chain, the refutations, the code changes, and the recovery
> procedure.

A device that flashed and verified cleanly started logging, on every reboot:

```
[DISP] PSRAM free=0 maxBlock=0 internal free=161760 maxBlock=73716 before panel init
E (702) EPDiy: Highlevel framebuffer allocation failed
```

What that actually was: **an XIP engine crash that left the in-package octal
PSRAM in a state a soft reset does not clear.**  Only a full PMU power-off
restored it.  The chip was never damaged -- it initializes at 40 MHz, 80 MHz
forced, 80 MHz AUTO and at 120 MHz on the official firmware afterwards.  The
crash's own cause is still open; the incident note carries the evidence chain and
the two hypotheses that were tested and refuted.

Two readings in that log are **red herrings**, and both nearly cost a whole
diagnosis:

- `epdiy: cache line size is set to 32 (< 64B)` appears across *healthy* boots
  too.  It is epdiy's own constant and is **not** the AGENTS.md section 6
  vendor-core signature; `esp_app_desc.project_name` is the only proof of that.
- `internal free` is not a symptom either: 161 KB looked alarming beside
  `PSRAM free=0`, but healthy boots of this env report 164 KB, and during the
  loop it also read 205 KB and 99 KB -- variance from a partly-initialized
  system, not a config change.

**The wrong conclusion I drew here, recorded so nobody repeats it:** because the
PSRAM heap *and* the SD card were both failing in the same boot, and both are
3V3 loads, I wrote that up as a supply/contact problem and told the user to change
cables and power sources.  That was inference from a plausible mechanism, not
measurement: the decisive facts (same binary boots with 8 MB after a power cycle;
a minimal IDF test app initializing the chip at four different timings; the
bootloader byte-identical to factory) were available and pointed at
initialization state, not power.  The SD timeout was a **co-symptom of the same
crashed state**, not independent evidence about the rail.  Lesson: before
blaming hardware, run the test that distinguishes the hypotheses -- here a 165 KB
`esp_psram_init()` app, which settles it in one flash.

What `classify_boot` in `scripts/flash_pdf_xip.py` does with this log now: it
reports `PSRAM free=0`, `Highlevel framebuffer allocation failed`, and the SDMMC
`send_op_cond … 0x107` timeout as **symptoms that the boot never came up**, and
nothing more -- deliberately no cause, and it ignores epdiy's cache-line message.
15 host checks in `test/pdf_flash_gates/`, mutation still required.

Do not re-flash repeatedly while PSRAM reads 0: every attempt "succeeds"
(`Hash of data verified.`) and the device still cannot boot.  Power-cycle first,
then judge.


## External resources and trimming

- CMaps: `/.crosspoint/cmaps/`, staged with
  `python3 scripts/stage_pdf_cmaps.py <mupdf>/resources/cmaps /Volumes/<SD>`.
  The `NO_CJK` prebuilt calls the app's `crosspoint_cmap_load` host symbol.
- Built-in MuPDF CJK/base-14 fonts are disabled (`NO_CJK`, `TOFU*`).  PDFs use
  their embedded fonts; reflow text is drawn with the system reader font.
- MuPDF is built PDF-only: HTML/XPS/MuJS/brotli/extract/barcode/tesseract are
  off; the module registers the PDF document handler only.
- The core CJK font lives in the spiffs data partition (`cjk_core` blob) and
  is installed to `/fonts/CPYOSCJKCore` for the font system; it is not part of
  the module or app image.
- README warns that ICC/lcms remains because the prebuilt MuPDF references it;
  removing it needs a source rebuild with `FZ_ENABLE_ICC=0`.

## Runtime memory and reader integration

- The PDF reader releases the SD font family (the resident core CJK TTF is
  ~2.5 MB) and the external font pack before MuPDF work, and reloads the SD
  font on exit.  Without that, a figure-heavy page runs out of PSRAM and the
  home screen afterwards is missing ordinary glyphs.
- The optional full emoji TTF (~0.9 MB, PSRAM) is also released for the PDF
  session and reloaded on exit before the core CJK family (that order matters:
  the 2.5 MB family would otherwise fragment the contiguous block emoji needs).
- Colour JPEG images are decoded as one gray channel (`grayjpeg=N` in the
  render-end log).  Verified on a generated 1200x900 RGB DCTDecode PDF:
  `grayjpeg=1`, page renders; the old build would have held RGB bytes.  The
  conversion is per-image: explicit `/Decode`, image masks, colour keys and
  CMYK JPEGs stay colour so the filter and tile stride always agree.
- Reflow text needs that SD family (the embedded system pack only covers UI
  strings, not body CJK).  `buildReflow()`/`renderReflow()` reload it after the
  stext pass and before measuring/drawing; the bitmap path releases it again so
  MuPDF gets the PSRAM.  Measured: keeping the family resident left 0.84 MB for
  MuPDF (page 68 bitmap failed); loading the pack's UI faces via
  `typography::apply()` cost 1.8 MB (also failed); the per-mode toggle keeps
  3.7 MB for MuPDF and renders full CJK reflow text without boxes.
- All MuPDF calls run on one persistent PSRAM-stack worker fed by a job queue:
  render, text extraction, `pageHasText`, and the reflow probe.  MuPDF's stext
  and FreeType frames overflowed the 8 KB loopTask stack and corrupted the heap
  when they ran there.
- The reflow decision is asynchronous.  The first page is shown as a bitmap;
  a background job samples up to three pages, and when it reports usable text
  the reader rebuilds the current page as reflow.  Opening a book no longer
  blocks for the 5–20 s those samples cost.
- Image decode is capped by `fz_set_image_budget` (96 KB for books >32 pages,
  2 MB otherwise) so figures are decoded at up to 1/8 scale instead of eating
  the heap.
- The MuPDF context is dropped in `PdfReaderActivity::onExit()` **before** the
  SD family, external pack, hyphenation tries and emoji TTF are reloaded.
  Without that reset the reload ran while MuPDF still owned 4–5 MB, and the
  emoji font silently fell back to the 126 KB subset on some exits.  With the
  reset the full 844 KB emoji TTF and the 2.5 MB CJK family reload reliably;
  a heavy page can still fragment the heap enough for the optional emoji to
  fall back, which is harmless and logged as `not enough PSRAM for the SD
  emoji font`.
- The reflow decision is stored as one byte in `<cache>/reflow.bin`; a changed
  PDF invalidates the cache directory.  Page bitmaps carry cache version 3, so
  the older uncropped/undersized layouts are rebuilt once after this change.
- The module caches the last `extractText()` result.  The bitmap path normally
  follows a reflow attempt for the same page, so the crop decision reuses that
  text instead of running stext twice; on page 60 this removed a duplicate
  ~2 s pass before the bitmap render.

### Reflow typography

Text pages are laid out with the same settings as the EPUB reader, not with a
PDF-specific approximation:

| setting | use |
|---|---|
| reader font / size | `SETTINGS.getReaderFontId()`; one face is resolved per paragraph with `chooseFontForText()` |
| screen margin | added to all four `getOrientedViewableTRBL()` margins |
| line spacing | `getReaderLineCompression()` |
| first-line indent | `paragraphIndentSpaces` and `firstLineIndentPx` |
| paragraph spacing | `extraParagraphSpacing` factors |
| alignment | left / center / right / justified |

The layout measures each paragraph once and then scales the per-character
advances by `fullWidth / sum(advances)`.  Measuring each growing prefix called
`getTextWidth()` thousands of times and took ~22 s per page; the scaled model
takes ~1.5 s and matches the width the renderer actually draws (the old model
was 594 px for a 682 px line, which is why the embedded-font sample overflowed
the right edge).  Wrapping is word-aware; a word that does not fit moves to the
next line, long words fall back to per-character breaks, and a 16 px safety
margin absorbs right-bearing differences.

When the reader's **Hyphenation** setting is enabled, English words use the
bundled Liang patterns; a fitting syllable prefix is placed at the line end
with a visible `-` instead of moving the whole word.  PDFs rarely expose a
document language, so English is the default pattern set; CJK still breaks by
character.  Hyphenation tries are already preloaded by `main.cpp`; the PDF
reader keeps them resident when the setting is on, because both the activity
and render task have PSRAM stacks and cannot load flash-backed tries there.

Justified lines only distribute slack when there are at least three words and
the extra gap is at most three spaces.  A two-word line on a 624 px column was
otherwise stretched into the conspicuous gaps seen in the first reflow build;
those lines now stay left-aligned.  CJK lines without spaces are left-aligned.

Reflow rejects pages with fewer than 200 bytes of extracted text and pages with
figures or side-by-side text blocks (columns/tables), so those keep the bitmap
path.  When the bitmap fallback itself runs out of memory, a page with
extractable text is retried as reflow with the figures omitted; the reader logs
`page N fell back to reflow after a failed bitmap render`.

### Non-embedded (base-14) fonts render blank -- measured

MuPDF is built here without its bundled fonts (`patches/mupdf`: no built-in
font data, NO_CJK, core fonts stripped) because every kilobyte of the engine
must fit the 1.875 MiB `pdfmod` partition. Consequence, measured twice on
device with two different files:

```
[PDFM] opened pages=1
[PDF] auto-levels 255..255                      <- the whole page is white
[PDF] reflow support: 0/0 sampled pages usable  <- and stext yields no text
```

A page whose text uses one of the 14 standard names (`/BaseFont /Helvetica`
with no `FontFile`) therefore draws **nothing** -- not tofu, not a warning: a
blank bitmap. Modern exports embed their fonts and are unaffected, but
hand-written PDFs and some old/OCR toolchains do not, and `CMD:MAKEPDF`'s
`/test.pdf` is exactly that case (it opens blank and is kept as the
reproduction, not as a working fixture).

Two implications for anyone touching this area:

1. **Test fixtures must embed a font.** A base-14 fixture proves nothing about
   layout or reflow -- it fails earlier, at font resolution, and looks like a
   broken reflow. `scripts/make_pdf_hardwrap_test.py` documents the shape it
   wants but cannot be used until it embeds a real `FontFile2`.
2. The engine has no font-substitution path to "fix" this with; the honest
   options are shipping a small core-font set (costs partition space the OTA
   budget does not have) or surfacing the blank-text condition as a message.
   The latter is deliberately **not** done here: it needs a new i18n key and the
   EN blob is at its 15-bit ceiling (see AGENTS.md section 65), and guessing at
   copy for a state the user has not reported is exactly the kind of change this
   project keeps having to revert.

## Smart line merge (PDF-specific option)
### Parity audit: every "like the EPUB reader" claim, checked against code

Comments that assert parity are not evidence -- one of them was false and shipped as an
accidental-chip bug (above).  Each claim in `PdfReaderActivity` was therefore re-checked
against `EpubReaderActivity`:

| claim | verified by | result |
|---|---|---|
| press-and-hold timing, dropped-sample tolerance, last-point firing | one `selection_hold::Tracker` in both readers (mutation 1 restores the old PDF rule and goes red) | shared |
| slop circle, handle hit tests, start/never-crosses-end clamp | one `selection_geom` (75 checks / 5 mutations) | shared |
| key widening and the anchor pair it needs | `selection_geom::indexOf/stepWord`, anchor set at 1420-1429 (EPUB) and in `updateSelectionDragTo` (PDF) | matches |
| opening-press swallow on the action bar | PDF had **nothing** | fixed: `selection_hold::barAcceptsInput` |
| Back is checked before that window in both | EPUB ~1749, PDF ~2120 | matches |
| quote join uses the same gap/glue rule and records an anchor prefix | `selection_text::joinSelection` + `anchorPrefixBytes` → `normalizePhrase(substr(0, prefix))` at EPUB 1964 and PDF 2343 | matches |
| annotation file codec rejects a damaged file whole | both call `annotations::deserialize` | shared |
| in-place remedy offered from a failed load | EPUB `handleLoadFailure` → `STR_CLOCK_SYNC_NOW` → `beginLoanTimeSync`; PDF → `openEngineUpdate` on a downloadable refusal | analogous |
| action bar drawn after the whole page | PDF draws after the page loop (the original transparent-chip bug) | fixed earlier |

### The action bar's opening press (a parity bug the comments hid)

Each reader carries a comment saying its selection handling works "like the EPUB reader".
One of those comments was false: EPUB swallows taps and chip keys for 250 ms after the bar
opens, and PDF had no such window, so the release of the long press that opened the bar hit
chip 0 -- an automatic Lookup on a fresh selection, or an automatic Delete while editing an
annotation. The rule now lives once, as `selection_hold::barAcceptsInput()` with
`kBarOpenSwallowMs`; the EPUB reader's local literal is gone, since a literal living in one
file is exactly how the other reader never got the guard. Back stays checked BEFORE the
window in both readers: cancelling must work on the first press.


**The option is measurable on both sides of "off".** `PdfDocument::extractText()` logs
`reflow engine: N blocks -> M paragraphs split=on|off` on every extraction, from inside the
engine, because the app-side `reflow merge:` line only exists when the option is on -- without
it there was no off-state to observe.  Related defect found while adding that: `CMD:PDFMERGE`
used to write SETTINGS and stop (no engine notification, no `resetReflow()`), so a serial A/B
measured a half-applied state that no user can reach, and the `--sweep` note "blocks is what
the reader shows with the option off" was simply untrue while the engine split unconditionally.
The menu row and the bench hook now share
`PdfReaderActivity::applySmartLineMerge()` (setting -> engine -> save -> reflow -> redraw),
`CMD:PDFENGINE` prints `loaded/abi/runningTag/open`, and `exp/pdf_verify_matrix.py` asserts
that off really yields `blocks == paragraphs` and that the device's ABI and ELF-digest prefix
match the values read from this tree and from the staged/rolling manifest entry.

### Prior art, read rather than assumed (2026-11-19)

The user asked for a mature, small, elegant solution rather than an invented
one.  What the established tools actually do:

- **calibre `ebook-convert --enable-heuristics`** (`manual.calibre-ebook.com`,
  "Heuristic Processing").  Verbatim:
  - `--disable-dehyphenate` — "Analyze hyphenated words throughout the document.
    **The document itself is used as a dictionary** to determine whether hyphens
    should be retained or removed."
  - `--disable-unwrap-lines` — "Unwrap lines using punctuation and other
    formatting clues."
  - `--html-unwrap-factor` — "Scale used to determine the length at which a line
    should be unwrapped… **The default is 0.4, just below the median line
    length.**"
  - `--disable-delete-blank-paragraphs` — "Remove empty paragraphs from the
    document when they exist between every other paragraph."
- **pdf2xml (Helsinki-NLP)** splits the same problem into three independently
  switchable passes — `dehyphenate`, `paragraph_merging`, `character_merging`
  (its documented example repairs letters spread apart by bad spacing:
  `c r i t è r e s` → `critères`), plus a `vocabulary` built from the document.
- Academic line/block reconstruction (**Aaltodoc thesis**, *Logical segmentation
  and labeling of PDF documents*) and **Freki** (*PDF-to-Text Reanalysis for
  Linguistic Data Mining*, ACL L18-1116) both work from **geometry**: they join
  fragments into lines, then lines into blocks, using box positions.

What this portation adopted, and why:

| rule | adopted from | note |
|---|---|---|
| sentence-final stop | calibre unwrap clues | text-only, no geometry needed |
| short line = paragraph end | `--html-unwrap-factor` | **median-relative**, not a fixed count |
| blank run = break | `--disable-delete-blank-paragraphs`, inverted | in a reflow the blank is the signal, not noise |
| indent = break | same family | U+3000 counted as two spaces |
| de-hyphenation | calibre / pdf2xml `dehyphenate` | **without** the document-dictionary gate, see below |
| `character_merging` | pdf2xml | **rejected** — see below |

Two adoptions were considered and declined, both for measured reasons:

1. **Document-as-dictionary de-hyphenation.**  It needs a pass over the whole
   book's words before the first page can be laid out, and a place to keep them.
   `extract_text` hands back one page at a time, so a faithful version means
   caching the book's vocabulary across pages.  My earlier note claimed this was
   blocked by Flash budget — **that reason was wrong**: the dictionary comes
   from the document, not from shipped data.  The real blocker is the per-page
   extraction API, and it stays open until the reflow cache keeps more than one
   page's text around.
2. **`character_merging`** (rejoining letters spread by bad spacing).  On our
   extraction path (MuPDF `FZ_STEXT`, which emits real codepoints and does not
   invent inter-letter spaces) it has no measured case, and it is the one rule
   that reliably damages legitimate text — spaced initials, `J. R. R.`,
   enumerations, and CJK text already has no spaces to exploit.  Adding it
   without a sample would be exactly the kind of unmeasured change this round has
   been fixing.

### The rules themselves

`src/pdf/PdfLineMerge.h` is pure and header-only, so every rule above is a host
test (`test/pdf_line_merge/run_pdf_line_merge_test.sh`, **54 checks + 4
mutations**, including one that disables the median-relative unwrap and must go
red).

The on-device assertion for the fixture page (`reflow merge: 8 blocks -> 4
paragraphs`) is pinned by the same test against the **same eight strings** the
generator writes into the PDF (`testFixturePageShape`), so the device check and
the host check cannot drift apart.  Verified by running the merge over those
bytes rather than reasoning about them: the split comes out 4, with paragraph 3
kept separate because the indented line ends in a full stop, and the short
heading isolated because the line before it was blank.

### The page that broke the rule: tables of contents (found by probing, 2026-11-19)

The median-relative rule has a blind side, and it is not hypothetical -- it is the
same defect the option exists to repair, pointed the other way, and the option is on
by default.  Feeding the shipped rule a real contents page:

```
TOC:    6 rows -> 1 paragraph   |第一章  废土地铁站 .................... 1 第二章  午夜便…
VERSE:  4 lines -> 1 paragraph  |床前明月光疑是地上霜举头望明月低头思故乡
```

On a page whose rows are all alike the median **equals** the row length, so nothing
looks short and every row looks like a wrapped line.  The three guards now in
`pdf_merge` are text-only, each keyed to something a wrapped sentence does not do:

| guard | signal | why prose cannot produce it |
|---|---|---|
| `hasLeaderRun` | three or more `.`/`·`, or two `…` | filler dots exist to bridge a title to its number |
| `endsWithDigit` | the last codepoint is a digit (half-width or full-width) | a wrap inside `1234` leaves a fragment, not a finished number |
| `startsEnumerated` | `第X章/节/回/卷/篇/部`, `12.` / `3、` / `4)`, or a bullet | a row announces itself; prose does not |

`measurePage()` counts them and `pageIsAnEntryList()` bails out for the whole page at
**half the rows** (three rows minimum) -- the asymmetry is the reason: merging prose
that should have been left alone invents text, while leaving a wrapped page as the
source wrote it is exactly what switching the option off means.  Per line, an
entry-like row both refuses to be joined *to* and refuses to start a new merge (the
enumerated half matters: without it a leaderless contents page merged in pairs,
measured 3 -> 2, now 3 -> 3).

Both `dropBlanks` and the other rules stay active on a table page, because collapsing
invisible filler is not a paragraphing judgement.

### Measured on real extracted text, not on my own examples

Hand-written fixtures only contain the shapes I thought of, so the guards were then
run over a page taken from a shipped PDF -- `thirdparty/lcms2/doc/LittleCMS2.16 API.pdf`
(177 pages).  Extraction used the host's PyMuPDF 1.28.0 (MuPDF 1.29.0), **not** the
1.26.3 the `pdfmod` module builds, and that is stated rather than glossed: this is a
proxy for the shape of real text (leader length, page-number tails, wraps), while the
device-side measurement is `exp/pdf_verify_matrix.py --sweep N`.  Page 2 is a genuine
contents page, page 20 body text.  `test/pdf_line_merge/make_real_fixture.py` regenerates
the embedded arrays and prints the version it used.

| real page | shape | guards removed | guards in place |
|---|---|---|---|
| contents, page 2 | 34 rows | **34 -> 2** (the whole TOC as two paragraphs) | **34 -> 34** |
| body text, page 20 | 24 lines | 24 -> 14 | 24 -> 15 |
| body text, page 20 | 10 blocks | 10 -> 5 | 10 -> 6 |

All three shapes are now checked into `test/pdf_line_merge` as `kRealTocRows` /
`kRealProseLines` plus the block array (140 checks in that suite), so the numbers above
are reproducible rather than quoted from this document.

The block row is the interesting one, and it is the reason the run was worth doing:
the single "lost" merge is the line `*None*` at first, then `2.6 void
cmsSetLogErrorHandlerTHR(...`.  The first was **my rule being wrong** -- a leading `*`
was in the bullet set, but in real text a leading asterisk is emphasis or a placeholder
far more often than a list bullet, and it cost three legitimate merges.  The bullet set
is now restricted to unambiguous glyphs (`• ‧ · ● ○ ▪ – —`) plus `- ` with a following
space, which is what took page 20's blocks from 8 paragraphs back to 6.  The second is
the rule working: a numbered heading should not be glued to the paragraph beneath it.

**What still merges (documented limit, not a surprise):** a column of verse or a
table whose rows carry no leader, no number and no marker.  Equal-length unpunctuated
lines are indistinguishable, in pure text, from a hard-wrapped CJK column -- CJK wraps
at any character, so a prose column's full lines are equally uniform.  This is exactly
the case the module-side geometry rule would settle (`fz_stext_line.bbox` is available
inside the engine, see the survey table), and the pin
`testLeaderRowStopsOnlyItsOwnMerge` asserts the merge *does* happen, so the limit is
owned rather than hidden.

### Splitting a block that MuPDF merged (module-side geometry rule)

Fixing `pdf_merge` is half the problem, and the other half turned up when a page still
showed two paragraphs glued together.  `test/pdf_line_join/make_cjk_block_probe.py`
measures why: two Chinese paragraphs, each hard-wrapped at 16 characters on a uniform
16 pt pitch, with no blank line and no indent, come back from MuPDF as **one text block of
six lines**.  `PdfDocument::extractText()` glues the lines of a block together
unconditionally, so the newline that separated those two paragraphs never reaches the app.
No text rule can recover it there -- only geometry can, and geometry lives in the module.

`src/pdf/PdfLineJoin.h` therefore decides per line pair of a block:

| Signal | Threshold | Catches |
|---|---|---|
| the line ends >= 25% of the block's column width short of the widest line (`kRaggedPercentOfColumn`) | ragged right edge | Chinese, which has no spaces to break on |
| the gap before the next line is >= 3x the mean line height (`kGapMultiple`) | vertical whitespace | justified Latin prose, where every line reaches the margin and the first test sees nothing |
| a line with no printable character | blank inside a block | PDF's own paragraph separators, which are otherwise invisible to MuPDF's grouping |

**A line with no ink is a break, not text.** PDF fills space with blank-only lines, and
the extractor used to append their characters (nothing) with a space between them.  As
*text* such a line is nothing; as *geometry* it has a small box, which the ragged test
reads as a colossal shortfall.  Both readings matter, so it is now reported as a break and
contributes no characters.

**The ragged test is gated on the block looking like a text column**, and that gate is the
whole difference between a fix and a defect.  Applied naively -- "short line means
paragraph end", no more thought than that -- the rule shredded four pages of a real
manual: `join_real_pages.py` measures **62 text blocks into 90 paragraphs (28 splits)** on
a gate-free build of the same header.  Source listings are ragged on every line, and each
of those reads as a paragraph end.  Requiring at least 60% of the block's lines to reach
its own right edge (`kMinFullLinePercent`, within `kFullTolerancePercent` of it) gives
**62 -> 69 paragraphs (7 splits)** on the same pages, and all seven are real: every one of
them is a blank line inside a block.  `make_cjk_block_probe.py` still recovers the fused
CJK pair (6 lines, 4 of them reaching the margin -> 2 paragraphs).

Both numbers come from `test/pdf_line_join/PdfLineJoinProbe.cpp`, which includes
`PdfLineJoin.h` and calls `paragraphStarts()` -- the function the module calls.  The
gate-free figure was produced by building that same probe against a mutated copy of the
header (`PDF_JOIN_PROBE=<path>` skips the rebuild).  Nothing here is a Python restatement
of the rule: the first version of the probe script carried exactly such a mirror, and it
went silently stale the day the gate was added, still printing plausible numbers about
code that no longer existed.

The same question in Chinese, where the rule exists to help: `make_cjk_corpus.py` lays
out seven shapes with MuPDF, knows how many paragraphs it wrote, and is run by
`run_pdf_line_join_test.sh` (so a regression here is a failed suite, not a stale number).
`full` is how many lines reached the block's margin -- the gate's own input:

| shape (CJK, written as hard wraps) | lines | full | rule | truth |
|---|---|---|---|---|
| two paragraphs of 3 lines | 6 | 4 | **2** | 2 |
| two paragraphs of 4 lines | 8 | 6 | **2** | 2 |
| two paragraphs separated by a blank line | 5 | 2 | **2** | 2 |
| a listing (every line a different width) | 7 | 1 | **1** | 1 |
| two paragraphs of 2 lines | 4 | 2 | 1 | *1 (known miss)* |
| two justified 3-line paragraphs, no blank | 6 | 6 | 1 | *1 (blind)* |
| two justified 4-line paragraphs, no blank | 8 | 8 | 1 | *1 (blind)* |

Read the last three rows as the honest limits rather than as todos.  A fused pair of
TWO-line paragraphs shows 50% full lines, under the 60% gate, and stays glued -- catching it
means catching the listings too (measured above: 28 bogus splits against 7 real ones).  And
when every line of every paragraph lands exactly on the margin there is **no** geometric
signal at all: the ragged test has nothing to see and there is no blank line to fall back on.
Only inter-paragraph spacing or text statistics could recover that, which is out of scope for
a per-page extractor call.

**One option, both directions.** `paragraphStarts()` takes the reader's
`智能合并断行` flag: `PdfReaderActivity` calls `PdfDocument::setParagraphSplit()` before
`load()` and again when the menu row is toggled, `load()` re-sends it after every open (a
retry after a wrong password creates a fresh engine-side document), and the engine forwards
it through the ABI-3 op into `extractText()`.  With the option off the module splits nothing
and emits exactly the one-paragraph-per-block behaviour it had before this existed -- blank
lines are still not emitted as *characters*, because gluing a space in was a defect rather
than a behaviour the reader asked to keep.  `testSwitchOffRestoresLegacyGluing` pins all of
it, and mutation 9 ("the paragraph-split switch is ignored") proves the flag is read: a
switch that cannot switch is worse than no switch, because the reader believes the menu.

Two further notes on the shape of the code:

* `paragraphStarts()` is exported rather than inlined at the call site so the host test and
  the real-page measurement run the same walk.  The caller in `PdfDocument.cpp` builds one
  `LineRecord` per MuPDF line, asks for the paragraph starts, and then emits text with those
  indices -- one buffer for the whole page, and the geometry judgement stays in one place.
* `kMinColumnLines` existed in an earlier draft ("a two-line block cannot describe a
  column") and is gone: no input can reach it, because a two-line block either has a line
  missing the margin -- then the ratio refuses -- or has only full lines, which leaves the
  ragged test nothing to find.  A guard that no input can reach is not a safety property,
  and it made the mutation harness believe it was testing something it was not.

**The known miss is a test, not a footnote.** Two *two-line* paragraphs fused into one
block show only 50% full lines, so the gate refuses and they stay glued
(`testShortFusedParagraphsAreTheKnownMiss`).  Lowering the gate to catch them costs the
listings above, so this is the price of the refusal rather than an oversight -- and it is a
cosmetic miss (two short paragraphs print as one) rather than an invented break.

The direction of the whole rule is deliberate: it can only ever ADD a break it is
confident about, and degenerate input (one line, a one-glyph-wide block, a zero-height box,
a missing height) keeps the pre-existing glue.  Same asymmetry as the entry-row guards: an
extra break looks like what the page did before this ran, while a missing one silently fuses
two thoughts.


### What the heuristic can still get wrong (measured, not hypothetical)

Both failure modes were seen on device on 2026-11-19 and are recorded, not hidden.

The option **defaults on**, and the two are kept apart by turning off the half that
can do damage: `pdf_merge::Options::deHyphenate` is `false` in the shipped
configuration, so the default path can only join lines that were already separated
-- it cannot invent a word.  That is what makes an on-by-default heuristic
defensible: the reported symptom (ragged one-line paragraphs, runs of blanks) is a
layout defect, and joining is reversible by eye and by the toggle; a fabricated word
is a correctness defect and this project does not ship those silently.  A wrapped
`com-` + `puter` therefore reads `com puter` -- odd, visible, and never wrong.

1. **De-hyphenation cannot tell a wrap from a real hyphen** -- so it is disabled
   by default.  Left on, a line ending in a hyphenated compound is glued into a
   non-word: the English fixture's
   `...identifiers, and-` + `extraordinarily...` came back as
   `andextraordinarily`.  calibre gates this on a document-derived dictionary
   (see Prior art above); the reason we cannot is the per-page `extract_text`
   API, not Flash budget -- my earlier note said "a word list is Flash this
   partition budget does not have", which is the wrong reason, because calibre
   ships no word list at all.  Pinned by
   `testCompoundHyphenGluesByDesign` so changing it stays a decision.
2. **A table-of-contents page merged into one paragraph.**  *(Closed 2026-11-19 by the
   entry-row guards above; verse and unmarked tables remain, with the reason.)*  The old
   text of this section claimed the absolute floor (`kAbsoluteShortCp = 3`) covered "a
   page whose lines are all short (a table of contents, a poem)".  It does not: real TOC
   rows are 15-25 codepoints, so the floor never fired and the probe above collapsed a
   whole contents page.  That claim was written before anything was measured against it.
3. **A Chinese paragraph break with neither indent nor a blank line was not
   detected.**  *(Closed 2026-11-19 by the median-relative unwrap rule.)*  The
   rules now stop a merge on sentence-final punctuation, a leading indent
   (ASCII spaces or U+3000), a blank line, or a **line that is short relative to
   this page's median line length** -- which is exactly calibre's
   `--html-unwrap-factor` reasoning and needs no geometry.  What remains
   undetectable: a paragraph whose last line happens to fill the column, and a
   short line that is a *heading* rather than a paragraph tail (the rule treats
   it as a tail, so the following line still starts a new paragraph -- the
   outcome is the same either way).
   The geometry signal would make the short-line test exact instead of statistical,
   and it is **available** -- `fz_stext_line` carries `wmode`, `dir` and `bbox`
   (`patches/mupdf/include/mupdf/fitz/structured-text.h:386-393`) and
   `extractText()` already walks those lines module-side, so the exact version is a
   module-side merge with **no `pdfmod_api`/ABI change**.  My earlier note claimed
   the opposite ("`extract_text` does not carry line boxes, so passing them would be
   a module ABI change"), which conflated the app-facing text buffer with what the
   module can see.  It is still not attempted, for the honest reasons: any engine
   edit re-pairs and re-flashes `pdfmod` (the `appTag` check), and the merge quality
   can only be judged **on paper**, which this environment cannot do.

Turning the option off restores the source's own breaks exactly.

### Who joins what (verified against the sources, not assumed)

Two layers, deliberately different:

1. **Inside one MuPDF block** -- `PdfDocument::extractText` (module side) walks
   `fz_stext_line`s and appends each line with: drop a trailing `-`
   (de-hyphenate), else a space only when both boundary characters are narrow
   (Latin), else nothing (CJK glue).  It used to carry **no judgement at all**, which
   was wrong in the direction the app-side rule can never repair -- see "Splitting a
   block that MuPDF merged" below.  It now asks `pdf_join::between()` per line pair, and
   starts a new paragraph when the geometry says so.
2. **Between blocks** -- the app receives them separated by `\n\n`, and that is
   where `pdf_merge` earns its keep: an OCR page that exposes every visual line
   as its own block would otherwise stay a page of one-line paragraphs. Its
   rules are conservative (sentence-terminal stop, short-heading stop,
   indentation/full-width-indent start, blank run collapses to one break).

`pdf_merge::splitHardBreaks` flattens a stray `\n` inside an entry before that
loop, so a future extractor that keeps line breaks *within* a block feeds the
same judgement instead of silently skipping it. It is a safeguard, not the
current data path -- `test/pdf_line_merge` mutates it away (mutation 3) to prove
the rules, not the splitter, are what decide a merge.


`extractText()` already joins the lines of one MuPDF text block (and
de-hyphenates inside it), so a visual break that reaches reflow means MuPDF
split those lines into **separate blocks** -- the OCR / hard-wrapped case the
user reported (`本书如何帮助 / 你成为`, plus runs of blank lines).  The reader
option **智能合并断行** (`SETTINGS.pdfSmartLineMerge`, More-panel row 6) runs
`pdf_merge::merge` (`src/pdf/PdfLineMerge.h`) across those block-paragraphs: it
rejoins consecutive non-terminal lines, de-hyphenates across the break, and
collapses whitespace-only paragraphs into a single paragraph boundary.  It is
the calibre "heuristic / post-OCR" rule with no geometry or ML -- purely the
text.  **On by default** (`SETTINGS.pdfSmartLineMerge`, and the fromJson fallback
uses the same constant so an absent key cannot silently mean "off"); a well-formed
one-block-per-paragraph extraction passes through unchanged.  De-hyphenation is a
separate, off-by-default half of `pdf_merge::Options` -- see "What the heuristic can
still get wrong".

- Verified on-device on the 112-page book (page 15): `reflow merge: 7 blocks ->
  3 paragraphs`, so MuPDF really does emit separate blocks here and the merge
  rejoins them before layout.
- `CMD:PDFMERGE <0|1>` sets/persists the option from the serial console and the
  merge emits the `N blocks -> M paragraphs` log line, so the effect is
  measurable on any PDF without a finger.
- Host suite `test/pdf_line_merge/run_pdf_line_merge_test.sh` pins every rule
  (terminal-stop, short-line-stop, de-hyphenate, blank-run collapse, invisible-fill
  blankness) with **six** mutation checks, each of which must turn the suite red.
- Blankness is codepoint-aware (`utf8IsBlankText`, `lib/Utf8/Utf8.h`): OCR'd Chinese fills
  the gaps between passages with U+3000 / NBSP / ZWSP, which are invisible on paper yet
  non-empty as bytes. Byte-level `empty()` calls them content, and each one became a blank
  line -- the "runs of blank lines" half of the report. The reflow loop skips such
  paragraphs even with the option OFF, so turning the feature off does not restore them.

### What was surveyed for the line merge, and why this rule

The report asked for a mature, small, elegant solution rather than an invention. The
options actually considered:

| Approach | Mechanism | Why this project did not take it |
|---|---|---|
| **MuPDF's own block grouping** (already in use) | `fz_stext_device` merges lines into `fz_stext_block`s by layout | This is the first pass, and it is exactly what fails on OCR'd PDFs: every hard-wrapped line arrives as its own block, so the "paragraphs" reaching reflow are one line long. `extractText()` already joins the lines *inside* a block. |
| **MuPDF stext options** (`FZ_STEXT_SEGMENT`, `FZ_STEXT_PARAGRAPH_BREAK`, `patches/mupdf/include/mupdf/fitz/structured-text.h:151-158`) | flags handed to `fz_new_stext_page_from_page` | **Considered and rejected after reading the option text**: `PARAGRAPH_BREAK` *"will break blocks of text at what appear to be paragraph boundaries… works best on a segmented page"* -- it **splits**, and `SEGMENT` segments regions; neither **joins** hard-wrapped lines, so neither can fix this symptom. (`PdfDocument.cpp:790`/`:820` pass `nullptr` params today, so no stext option is active at all.) |
| **calibre's heuristic pipeline** (`unwrap-texts`, `infer-figure-coalescings` etc.) | median-relative line length: a line shorter than a fraction (default 0.4, "just below the median line length") of the page's median line is treated as a real paragraph end unless the line ends in punctuation | **Taken**, in the smallest useful part: the median-relative threshold (`kUnwrapPercent = 40`) plus the terminal-punctuation guard. calibre runs a whole pipeline over an ebook; there is no ebook here, only per-page text. |
| **Geometry-based paragraph detection** (pdfplumber / grobid / Unstructured: first-line x-indent, vertical gap > line spacing, left-edge alignment) | per-line bounding boxes | Genuinely better on two-column and mixed-size pages, and the right next step. **Not blocked by the ABI** -- my first note here said it was, because `pdf_module::extract_text()` returns plain text only, but that describes the *app-facing* buffer: `fz_stext_line` carries `wmode`/`dir`/`bbox` inside the module (`structured-text.h:386-393`), and `extractText()` already walks those lines, so an exact geometry merge is a module-side change with no new `pdfmod_api` entry.  (It *did* later need one `abi` bump -- not for the geometry but to carry the reader's option in, ABI 3.  Do not read this row as "no bump is ever needed": a new *input* from the app needs an op, a new *decision* on data the module already holds does not.) What it does cost is an engine rebuild + re-pair (`appTag` ties module to firmware image) and, decisively, **judgement on paper**: a mis-merge is only visible as a wrong paragraph on the e-ink screen, which this environment cannot see. Kept as the documented follow-up rather than implemented blind. |
| **calibre's own protection against table damage** | calibre unwraps *ebooks*: the paragraphs already exist as markup, and `unwrap_lines` only fires where the text was hard-wrapped without markup | Not available here -- there is no markup, only per-page extracted text.  This is the one place the port is **not** a copy of calibre: the entry-row guards (`hasLeaderRun` / `endsWithDigit` / `startsEnumerated` / `pageIsAnEntryList`) exist because the median rule, transplanted alone, ate a contents page. |
| **ML layout models** (Marker, MinerU, Docling) | learned reading order / paragraph classes | Milliseconds-to-seconds per page and megabytes of model; the device has neither the time budget nor the flash, and an unexplainable mis-merge is worse for a reader than a visible one. |
| **Dictionary-based de-hyphenation** (calibre gates it on a word list) | split/join only when both halves are words | The list is the cost, and `readpico_pdf` has ~250 KB of app0 left. So `deHyphenate` ships **off**: the measured failure was `and` + `extraordinarily` → `andextraordinarily`, i.e. a fabricated non-word. A wrapped `com puter` is cosmetic; a wrong word is not. |

The rule as implemented is therefore deliberately text-only and deterministic, which is
also what makes it host-testable: median line length per page, an absolute floor
(`kAbsoluteShortCp = 3` codepoints -- a heading or a page number is never a paragraph),
two-or-more leading spaces / U+3000 as an explicit indent signal, terminal punctuation as a
stop, blank runs as one break, and an opt-in hyphen join. Tuning is two constants in
`src/pdf/PdfLineMerge.h`; every one of those behaviours has a named case in the host suite.

### Image pages and white-margin crop

For image-like pages (`< 400` extracted characters, including scans with a
caption or page number), the rendered 8-bit BMP is scanned for ink.  The crop
is accepted only when the ink occupies at least 1/2000 of the page and the
saved margin is meaningful in at least one dimension; otherwise the original
BMP is kept.  The cropped BMP is then fitted inside the reader's margins with
its aspect ratio preserved, not stretched to fill.  Small documents also use a
one-pass content-box pre-crop before rasterising; long documents use the
post-render crop so the page's own text/vector bbox cannot discard content.

## Reader toolbar, typography and selection

### Declining a lookup is an acknowledgement, not a dialog

EPUB's rule (see the comment in `EpubReaderActivity::render`): a transient
acknowledgement is a bottom capsule, never a blocking card in the middle of the
page the reader just turned to. The PDF reader used `GUI.drawPopup` for its
three declines, which is the other surface -- so all three now go through the
same `Toast` component EPUB uses (`STR_DICT_NO_DICT_SET` when the saved
dictionary name no longer resolves to a folder, `STR_NOT_AVAILABLE` when the
selection has nothing lookable or the child activity could not start), painted
in the reflow page pass before the single present.

Two log lines were added at the same time, because without them a declined
lookup and a swallowed keypress look identical on the console -- that cost a
full debugging round on 2026-11-19:

    [PDF] phrase: 4 bytes, anchor at 0
    [PDF] lookup declined: no usable dictionary behind the saved name

Sizes only. The selected text is book content and is never logged.


- The PDF toolbar now uses the same clean-page snapshot pattern as the EPUB
  reader.  Opening a panel stores the page once; switching Contents / Format /
  Bookmarks / More restores that snapshot and repaints only the chrome with one
  FAST push.  It no longer calls `renderPage()` for every key, so the double
  page scan and visible flicker are gone.
- The **Format** panel opens `TextSettingsActivity` in its Layout tab.  Changes
  invalidate the per-page BMP cache (font/margins/crop affect bitmap pages too),
  reset the reflow layout and redraw the current page.
- Long-press on a reflowed page selects a word and shows a chip action bar.
  The chip drawing/geometry and the selection geometry now both live in shared
  headers (`SelectionActionBar.h`, `SelectionGeometry.h`) used by the EPUB and
  the PDF reader alike, so the two behave identically.  `SelectionGeometry.h`
  owns the word-rect nearest hit, the (line, word) range, the start/end clamp,
  and the two draggable endpoint handles; the PDF reflow records a
  `ReflowWord{x,y,w,h,line,word,start,end}` for every atom it draws, so its
  selection rects come from the same layout that painted them (no drift).
  Dragging either handle re-ranges the selection, and the chip bar's first
  action opens the dictionary lookup (`查词`) for the joined phrase — the same
  `selection_text` join + `DictionaryWordSelectActivity` path the EPUB reader
  uses.  Annotations still use the shared `ANNO` store and quote anchoring.
  - Verified on-device (the screen need not be touched): `CMD:PDFSEL <x> <y>
    [dragX] [dragY]` resolves a word and, with the second pair, drags the end
    handle -- e.g. single word `range=[5:2..5:2] quote="可"`, then `CMD:PDFSEL
    120 260 300 360` widened to `range=[5:2..7:9] quote="可用的数据划分出30%…"`.
    This proves `selection_geom::nearest`, the `moveEndTo` clamp and the joined
    multi-word `selectionQuote()` run on real MuPDF reflow words; the handle
    glyph rendering and the lookup popup on top are the same shared code and
    still need an on-screen check.
  - `startPhraseLookup()` now mirrors EPUB's anchor: `selectionQuote()` also
    returns the byte offset of the originally-pressed word within the joined
    phrase, and `query.anchorOffset` is the normalized prefix length.  A
    single-word lookup anchors at 0 (as before); a dragged multi-word selection
    looks up the tapped word, exactly like EPUB.
- ★ The PDF action bar is drawn **after the whole page**, exactly like the EPUB
  reader's `drawSelectionMenu()`.  It was first drawn inside the per-line loop,
  so every later body line painted over the white chips and the buttons read as
  transparent; capturing the selected word's rect and drawing the bar once at
  the end fixes it.  Verified on-device (`artifacts/pdf_sel_final.png`).
- Turning past the final screen of the last page keeps the reflow layout
  (`pageTurn` no longer calls `resetReflow()` when it crosses into the
  end-of-book notice), and `onReturnFromEndOfBook()` restores the last screen
  rather than the top of the page.  Returning from the notice is now a single
  ~100 ms repaint with no `reflow page`/`renderPage` rebuild — previously it
  re-extracted and re-laid-out the whole page (seconds of lag).  A 10-page skip
  onto the notice still lands at the end via `reflowEndScreenPending_`.
- The module code window was rebased by the larger app image: `0x42D20000` ->
  `0x42D30000`.  `kPdfModuleCodeVaddr`, `build_pdf_module.py` and the docs are
  kept in sync; a high-level MMU mapping is used again (raw code mapping was
  tested and rejected).

## ★ Which core signal is trustworthy (2026-11-20)

A device shipped from this tree showed a **permanently white screen**: every frame starved
(`line buffer underrun ... frame=37/37` -> `Frame failed (1024)`, 1.9 s per 684-line frame),
starting with the boot splash, while the app source was unchanged and healthy.

The documented detector in AGENTS.md section 6 -- `esp_app_desc.project_name` -- **could not
see it**. After forcing a genuine core recompile (`Archiving
.../esp-idf/framework-arduinoespressif32/libframework-arduinoespressif32.a`), fixing the bug
and restoring the frame budget (639 ms, zero underruns), the descriptor still read
`project_name=backup/dev-pre-reconcile-202611`, `version=plugin-runtime`. That is expected
(section 23.6g): the descriptor is supplied by the **shared** `libesp_app_format.a`, so it
reports whichever tree linked last, not the core our image used.

What did move, and is the signal to use:

| Measured | Core without our `custom_sdkconfig` | Core recompiled from this tree |
|---|---|---|
| Link-time static internal RAM | 94,736 B | **89,872 B** |
| `SCANSTATS` per full frame | ~1.9 s, starves in the last phase | **639 ms, clean** |
| Boot stage `panel` | 4352 ms | ~2700 ms |

Line feeding costs ~75 us/line instead of ~16 us when `CONFIG_ESP32S3_DATA_CACHE_64KB` /
`_LINE_64B` are absent, which matches the cache-bound scaling measured in AGENTS.md
section 45.4 (86 vs 163 us/line). A frame can never finish, so the glass keeps whatever was
last driven onto it.

Consequences, both implemented:

1. `scripts/flash_pdf_xip.py` refuses when pioarduino's applied-config record
   (`framework-arduinoespressif32-libs/esp32s3/sdkconfig.crosspoint`) lacks any setting that
   `platformio.ini` declares -- demands are parsed from the repo, never hardcoded here.
   The neighbouring `sdkconfig` is Espressif's untouched reference config and says
   `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=0` even on a healthy machine: reading it produces a
   false alarm. Guarded by `scripts/tests/test_core_config_guard.py`.
2. The record alone still cannot catch the failure we hit (record present, packaged libs
   replaced underneath it, so pioarduino skipped the rebuild). The end-to-end detector is on
   the device: after boot, force a redraw and require **0 underruns and < ~900 ms per full
   frame** in `SCANSTATS`. "The boot log shows no `[ERR]`" is not evidence -- the failures
   start after the banner and the UI keeps running normally while nothing reaches the glass.

## Encrypted PDFs

- `PdfDocument::load()` checks `fz_needs_password()` after opening the stream
  and calls `fz_authenticate_password()` with a password supplied by the reader.
  The password lives only in RAM in that document object; it is cleared when
  the activity exits and is never logged or written to SD.
- A missing/wrong password records `passwordRequired_` / `passwordWrong_` and
  the activity opens the normal `KeyboardEntryActivity` password field.  A wrong
  password re-prompts with `STR_PDF_PASSWORD_WRONG`; Cancel exits to the library.
- Two memory/stack rules were required for encrypted files:
  - the reader fonts, emoji, pinyin and hyphenation are released **before**
    opening, because AES/xref setup needs several hundred KB (and encrypted
    pages previously failed with only 5 KB free when fonts were still resident);
  - the module is loaded from flash on the UI task first, then all MuPDF work
    (including `load()`) runs on the 64 KB PSRAM worker at idle priority.  The
    engine loader uses `esp_partition_read`; doing that on a PSRAM stack trips
    `esp_task_stack_is_sane_cache_disabled()`.  The idle priority keeps IDLE0
    alive during the CPU-bound AES key setup.
- AES-256 fixtures take roughly 15 s to unlock on this CPU; normal unencrypted
  PDFs are unaffected.  After unlock the pages render normally.

## Annotations, bookmarks and the PDF outline

- Native PDF outline: `fz_load_outline()` is flattened (up to 500 items,
  `page >= 0`) and opened from the toolbar Contents panel.  Entries with no
  resolvable page are skipped; the panel also falls back to page numbers when
  a PDF has no outline.  Verified with a 3-entry fixture:
  `[PDFM] opened pages=3 outline=3`.
- User bookmarks are stored by page number (`PDFB` v1). Notes/highlights use the
  shared annotation codec (`ANNO`) with `spineIndex` = PDF page and
  `startOffset`/`endOffset` = **page-local line indices** (a reflowed PDF page has
  no stable character offset; the EPUB reader uses text offsets in the same field).

### Selection notes on the PDF page: visible now, still unmarked

`annotationSpanOnLine()` filtered on `Kind::Highlight`.  A **selection note** therefore
drew no rule and matched no hit-test, so it was invisible on the page and could not be
re-opened or deleted from it -- only from the annotations list.  Both kinds resolve now,
which makes a note underlined, pressable, and editable/deletable through the existing
editing chips (which also prefill from the stored note text, so editing no longer starts
from blank and risks discarding it).

★ **Remaining gap, deliberately not closed blind**: the EPUB reader marks a note with a
small bubble (18x16, tucked into the line gap after the range's last word) and that
bubble is a tap target.  The PDF page has no equivalent yet, so a note currently looks
identical to a highlight.  Drawing one is ~25 lines reusing `reflowWords_` and the
record-the-rect-while-drawing pattern already used by the selection handles, but it is a
*visual* affordance and this repo's rule is that visual changes get checked on the panel
before they ship (the box-average cover scaling is the precedent for what happens
otherwise).  It is the next item for the PDF round, not this one.

### Re-anchoring a highlight across a merge toggle (quote_anchor)

Two D1/D2 features interact badly.  A PDF highlight stores its **quote**, not a
character offset (there is none that survives re-layout), and the quote is looked up
in the page's current lines by `findQuoteLineRange()`.  Turning 智能合并断行 on joins
hard-wrapped CJK lines **without a space** -- so a quote captured before the toggle
contains a space the page can no longer produce, the lookup fails, and the only
remaining hint is the pair of line indices stored at creation, which describe a layout
with one MORE line than now exists.  The highlight does not error out; it drifts onto
the wrong lines or disappears.

Matching therefore runs two passes, in `src/activities/reader/QuoteAnchor.h`:
whitespace-collapsed exact first (Latin word boundaries keep their precision whenever
an exact match exists), then a whitespace-insensitive retry.  When the second pass
fires the reader logs `annotation re-anchored without spaces: lines A..B`, which is
diagnostic, not an error -- it means the layout changed under an annotation that used
to match exactly.  `exp/pdf_verify_matrix.py` asserts the behaviour that matters: after
a toggle, pressing inside the highlight still reports `editing>=0`.

Both passes match on **whole words**, not substrings -- and that is an older bug than
the merge toggle.  `annotationSpanOnLine()` underlines the WHOLE line when the quote
matches, and a bare `find()` lets the quote `"and extra"` occur verbatim inside an
unrelated line `"band extraordinary results"` (as a prefix too, `"and extra"` inside
`"and extraordinary"`).  So a highlight could land on a paragraph the reader never
selected, with no error anywhere.  The guard rejects a hit whose neighbour and edge are
both ASCII alphanumerics, and deliberately does NOT apply to CJK, where a quote starts
mid-run because Chinese has no separators -- mutating the guard to cover CJK turns the
suite red.  Matching also continues past a rejected hit instead of giving up on the
page, so a real occurrence later on still wins.

### Underline precision: one matcher for drawing and hit-testing

`annotationSpanOnLine()` resolved the quote itself, with a bare `text.find(quote)` as a
fast path.  Two consequences, both now removed by delegating to
`quote_anchor::spanForLine()`:

- that fast path had **no word-boundary check either**, so the same
  `"and extra"`-inside-`"band extraordinary"` false positive underlined a word nobody
  selected -- the guard above had to be applied to the matcher that actually draws, not
  just to the one that locates the lines;
- a multi-line quote painted its **first and last line end to end**, where the EPUB
  reader underlines exactly the selected run.  `Span` now carries byte columns into the
  original line (`buildPage` records them alongside the line index), so the boundary
  lines get partial spans and only genuinely interior lines are full width.  Columns of
  `-1` (a synthesised separator) fall back to the whole line rather than clipping a word.

`spanForLine()` returns a tri-state on purpose: `kAbsent` may fall through to the stored
line offsets, `kElsewhere` must not -- underlining an offset range recorded against a
layout that has since changed is exactly the drift the quote exists to prevent.

★ This changes what a highlight *looks* like (shorter underlines at paragraph edges), so
it needs an on-device look, not just green tests.

`test/quote_anchor/` pins 48 checks with eight mutations, one per decision: remove the
relaxed pass; run it FIRST (the loosened match then beats an exact one); stop inserting
the line separator; drop the left word guard; drop the right word guard; apply the word
guard to CJK; paint the first boundary line full width; and collapse `kElsewhere` into
`kAbsent`.  All eight turn red.  Two were caught by the harness rather than by design:
the right-guard mutation was initially UNDETECTED (the suite had no prefix case), and a
runner patch that silently failed to apply would have printed "mutation detected" for a
rule it never touched -- which is why `mutate` now fails hard when the header is
unchanged.

### Selection parity with the EPUB reader (verified against the sources)

The reflow page drives the same shared pieces as the EPUB reader, not a parallel
implementation: `selection_geom::hitHandle` for grabbing a handle,
`selection_geom::moveStartTo/moveEndTo` for the no-cross clamp,
`selection_geom::nearest` for point→word, and `selection_bar::draw/hitTest` for the
action chips (two while editing an existing annotation, three for a fresh
selection).  A press outside the handles clears the selection; a lookup with no
usable dictionary is an acknowledgement (`Toast`) rather than a dialog, per
"Declining a lookup is an acknowledgement, not a dialog" above.

`CMD:PDFSEL <x> <y> <toX> <toY> <1|2>` drives `updateSelectionDragTo()` -- the
production function the finger reaches through `handleSelectionHandles()`.  The
5th argument chooses the endpoint: until this existed only the **end** handle was
reachable from the console, so the start handle's clamp had a host test but had
never been exercised against a real reflow word list.  The probe log now carries
`drag=N` so a run can prove which endpoint moved rather than assuming it.

### The gesture itself was a second implementation (found by diffing, 2026-11-19)

The geometry was shared but the **timing** was not: each reader carried its own
press-and-hold state machine, with the same four numbers written twice
(`650 / 64 / 320 / 350`) and, worse, two of the EPUB reader's branches missing.
Diffing the two bodies rather than trusting the "matching the EPUB reader" comments
turned up a real defect:

| branch | EPUB reader | PDF reader (before) |
|---|---|---|
| frame with no touch sample | ignored if shorter than `kHoldGapMs` (150 ms) | **cancelled the press** |
| panel stops reporting a stationary finger | fires the hold from the **last known point** | never fires |

The Read Pico panel does both of those things -- they are the reason the EPUB
branches exist (each was written after a long press silently failed on device).  So
the same gesture could open a selection in the EPUB reader and do nothing in the PDF
one, with no error anywhere: the finger simply never looked like a hold.

Both readers now drive one pure state machine, `selection_hold::Tracker`
(`src/activities/reader/SelectionHold.h`), which owns the timers, the slop circle,
the dropped-sample gap, the last-point firing, and the constants
(`kPressMs / kSlopPx / kHoldGapMs / kDragRepaintMs / kClosedGuardMs`).  The drag
repaint gate is the shared `repaintDue()`, so the 32-bit `millis()` wrap is handled
in one place.  The only per-reader difference left is deliberate: the EPUB reader
keeps its own `dragLastX/dragLastY` for the drag repaint dedupe, because "did the
finger move since the last repaint" is a different question from "was this a hold",
and reusing the tracker's last known point would couple them.

Host suite `test/selection_hold/run_selection_hold_test.sh` -- **31 checks, seven
mutations all detected**, including mutation 1 which re-instates the old PDF rule
(one dropped sample cancels the press) and mutation 4 which turns the slop circle
into a per-axis box.  The diagonal case at `(50, 50)` is what distinguishes those:
inside a 64 px box, outside a 64 px circle.

### Widening a selection with the keys (and the Back key the bar was eating)

`test/selection_hold` fixed the gesture that *opens* a selection. Opening was only
half of the EPUB reader's model though, and the PDF reader was missing the rest --
found by reading every key branch of both, not by trusting the comments again:

| state | EPUB reader | PDF reader (before) |
|---|---|---|
| bar up, Back pressed | ends the selection | **swallowed**: the bar could only be left by choosing a chip or tapping away |
| after a dictionary lookup | bar closes, selection stays alive | bar stayed open, so the widen state never existed |
| bar closed + selection alive | Left/Right walk words, Up/Down jump a line keeping the column | any key fell through to `clearSelection()` |

The move itself is now shared arithmetic -- `selection_geom::indexOf()` and
`selection_geom::stepWord(words, from, KeyStep, anchorX)` -- templated over the word
rectangle because the two readers keep their own (`AnnoWordRect` carries text and
paragraph, `ReflowWord` carries byte offsets); pinning the helper to one type would
have forced a second copy of the rule, which is the failure mode this header exists
to prevent. `widenSelectionTo()` mirrors `EpubReaderActivity::extendSelectionTo()`:
the anchored end stays put, the anchor moves to the far end, the column is remembered
for the next vertical press. The drag path now maintains that same anchor pair too,
so a drag followed by a key press widens from where the finger left off.

`indexOf()` returns -1 when the page no longer contains the focused word (a rebuild
renumbered the lines). Doing nothing is the only safe answer; the EPUB reader's old
inline loop left `focus = 0` in that case, so a single Right press would have yanked
the selection to the **first word of the page**.

Host coverage: `test/selection_geometry` is up to **75 checks / 5 mutations**, with
three new ones each re-creating a plausible mistake -- a missing focus defaulting to
index 0, a vertical step that forgets the column, and a Right at the last word that
wraps to the first. The `(50, 50)`-style edge cases and a wide-rectangle fixture keep
the template honest in both directions.

`exp/pdf_verify_matrix.py` asserts all of it against the real firmware (37
assertions total), and `--sweep N` additionally walks N reflowed pages printing
`N blocks -> M paragraphs (ratio)` per page -- the quantitative half of the
"unexpected line breaks" report, since `blocks` is exactly what the reader lays out
with the option off.  A page near 1.0 was already well formed; a page near 0.1 was a
wall of one-line paragraphs.

Verification without a finger: **`CMD:PDFSELACTION 100..103`** widens the live
selection left/right/up/down through the production path and logs
`sel widen: step=N focus=I next=J range=[l:w..l:w] words=K`. `next=-1` is the visible
proof of a page edge or a stale focus word; the follow-up
`selection: widen to line L word W` plus a `CMD:PAGEDUMP`-style re-read shows the
range actually grew.

Two smaller parity items came out of the same diff and were fixed here:
the PDF reader now emits the same `selection: hold fired / press ended by release /
press cancelled (travel)` debug lines (they are the only way to see this gesture from
the console), and it clears its stale drag-handle id when a new hold fires -- the
drag path only reads that id while dragging, so this is hygiene, not a fix.

### Two annotation data-loss bugs this coordinate difference caused (2026-11-19)

Reusing an offset-shaped record for line indices was not harmless. Two ways:

1. **The second highlight on a page deleted the first.** The selection paths wrote
   `(startOffset,endOffset) = (0,0)` for every highlight and every selection note,
   and `annotations::normalize()` deduplicated on `(spineIndex,startOffset,endOffset)`
   alone -- so two genuinely different selections on one page were literally the
   same record and only the last survived. Fix is both sides: the PDF selection
   paths now store the real line range (`annotationLineRange()`, which reuses
   `findQuoteLineRange()` so anchoring and drawing resolve identically), and the
   identity key includes `quote`. EPUB's quote is derived from the same snapshot as its
   offsets, so re-highlighting a range still yields an identical (range, quote) pair and
   still collapses, and its edits/deletes go through an index-based `erase` rather than
   this dedupe (checked in `EpubReaderActivity`, after this section first claimed
   otherwise) -- so the key change cannot alter EPUB behaviour; it only stopped being
   able to merge two *different* selections. A side effect worth knowing: the
   degenerate `0/0` records also skipped `normalize()`'s UTF-8 clamping, so an
   oversized PDF note was persisted unbounded -- real ranges restore the clamp.
   Pinned by `test/annotation_store/` (23 checks, 2 mutations: reverting the key and
   widening the clamp gate each turn the suite red).
2. **Writing a page note wiped the page.** `noteForPage()` matched on page alone, so
   the page-marker was "any annotation on this page"; `editPageNote()` prefilled
   from an unrelated selection note and then erased **every** annotation on the page
   before saving. The marker is now a named, exact predicate --
   `anno_anchor::isWholePageMarker()` (`0,1`) -- used by the lookup, the erase, and
   the creation site, so the three cannot drift.

Every annotation mutation now logs one observable count, `annotations persisted:
total=N onPage=M`, because both bugs were of the "an annotation quietly vanished"
family; `exp/pdf_verify_matrix.py` asserts that two highlights made from two
different selections on one page end at `onPage=2`.

★ Not covered: the EPUB reader's own annotation tests live in the CMake suites, and
this machine has no cmake, so the shared-codec change is verified by the new host
suite (which links the real `AnnotationStore.cpp`) plus reading the EPUB call sites
-- not by running EPUB on device.
- Saves now use `<file>.part` + `Storage.replaceFile()` for both
  `bookmarks.bin` and `annotations.bin`; a failed write no longer leaves a
  truncated file.
- Clearing the cache no longer deletes them.  `ensureCacheValid()` used to
  `removeDir()` the whole `pdf_<hash>` directory whenever the PDF's size or mtime
  changed, which took the reader's highlights, notes, bookmarks and position with
  it; the sweep now prunes only regenerable entries
  (`book_cache::pruneRegenerable`, see
  [cache-management.md](cache-management.md#what-a-cache-clear-must-keep)).  Keeping
  the position is safe: `loadProgress()` clamps the page to `pageCount()`, and a
  highlight whose quote no longer matches simply stops drawing rather than drifting.
- Annotations export like they do in EPUB, from one implementation:
  `annotation_export::write()` (`src/activities/reader/AnnotationExport.{h,cpp}`)
  builds the date stamp, formats the list and writes `<cache>/annotations.txt`
  under the **same** constant the cache-clear keep list matches, so the export
  product cannot become deletable.  The PDF entry is 工具栏 → 更多 → 导出批注;
  the panel's fixed action strip holds only one button and PDF keeps it for
  添加书签, so the placement differs while the behaviour does not.
- Deleting a row from the Bookmarks panel removes **only that annotation**.
  The previous code erased every note/highlight on the page when one row's
  delete button was used.  Rows now carry their index and show the note/highlight
  kind plus the captured quote.
- New highlights also store the selected text.  Rendering prefers that quote
  and maps it onto the current reflow lines, so changing font/margin/line
  spacing does not silently move an underline to unrelated text.  Old
  annotations (empty quote) keep the line-index fallback.  A quote is capped at
  120 bytes, so a highlight spanning far beyond that cap may only underline the
  matching prefix after a repagination.
- A page that stays bitmap cannot draw text highlights; the menu action remains
  page-local and is intentionally a no-op there (notes still work at page level).
- **Pressing inside an existing highlight re-selects it and the bar's first chip
  becomes Delete** (`selectionEditingIndex_`, chip count 2 = Delete/Note) -- the
  EPUB reader's rule, now in PDF too.
  Why this was not cosmetic: a selection-made PDF highlight stores **no offsets
  at all** (`startOffset = endOffset = 0`, quote only), because a PDF page has no
  stable text coordinate across re-renders.  The screen-range delete path matches
  on `startOffset`, so it could never remove such a highlight; the only way to
  delete one was to find it in the Bookmarks panel.  Re-pressing the page now
  finds the annotation **by the same quote matcher the renderer uses**
  (`annotationSpanOnLine`, refactored out of `lineHighlightSpan` so drawing and
  hit-testing cannot drift), widens the selection to the annotation's whole word
  range, and deletes **by index**.
  Verified from the serial console with no finger, because the chip bar was
  previously unreachable from it: `CMD:PDFSEL x y` → `CMD:PDFSELACTION 1`
  (highlight) → `CMD:PDFSEL x y` again must log `editing>=0` with a widened
  range → `CMD:PDFSELACTION 0` (delete) → pressing again must log `editing=-1`.
  That whole sequence is steps 2b of `exp/pdf_verify_matrix.py`.
  Remaining parity gap, deliberately not papered over: EPUB persists annotation
  ranges as text offsets and can therefore re-anchor across a full re-layout even
  when the quote itself changed (edited source, different extraction); PDF can
  only re-anchor while the quote still occurs in the text.  After a font/margin
  change the quote survives, so the underline follows the text -- but if an
  extractor change ever alters the quote's bytes, a PDF highlight goes unanchored
  while an EPUB one would not.  Fixing that needs line boxes from
  `extract_text`, i.e. a module ABI change.
- Verified on the 112-page book: a seeded highlight on page 10 survives a full
  close/reopen and still underlines the same text from the cached annotation.

## Embedded PDF fonts in reflow

Exact embedded-font reflow is **not reliable** and is deliberately not used:

- PDFs commonly embed subset CID/CFF/Type1 programs with custom glyph IDs and
  no Unicode cmap.  MuPDF's text extraction recovers Unicode through the PDF
  encoding/ToUnicode map, but the app renderer looks up glyphs by Unicode; the
  embedded program cannot be handed to it as a normal TTF face in the general
  case.  Using it would produce missing or wrong glyphs.
- Reflow therefore keeps the reader's typography, while the bitmap path retains
  the PDF's embedded fonts and original styling.  Pages with figures, tables,
  multi-column text, or no usable Unicode mapping keep the bitmap path.

A future partial improvement would be to carry paragraph/run bold-italic and
relative-size hints into the reflow layout; it would not reproduce the embedded
face exactly.  No such style-run ABI is implemented yet.

### The other embedded-font cost: a 450 KB slurp on the bitmap path (open)

Separate from reflow, an *embedded* font also limits the bitmap renderer.
Measured on page 19 of the 112-page test book (incident doc section 8.1):

    psramRealloc(463911) failed   free=203520 largest=196596
      fz_resize_buffer / fz_grow_buffer / fz_read_best
    PSRAM alloc 378319 failed     free=323552 largest=319476   (the page pixmap)

MuPDF loads an embedded font by growing one `fz_buffer` to hold the whole
program, and it does so while the destination pixmap is already allocated -- two
~400 KB contiguous PSRAM blocks at once, on a heap whose largest block is often
under 350 KB because the panel holds ~3.1 MB.

Two tempting answers are already ruled out by measurement, not opinion:

- **The emergency reserve does not fix it.** It served at most one of the two
  allocations on pages 41/28 and both pages still failed, while the megabyte it
  pins comes straight out of the same working set (see the comment at
  `psramMalloc`/`reserveInit` in `src/pdf/PdfDocument.cpp`).
- **Refusing the page early is not a fix either** -- page 19 is a normal text
  page; losing it to a font is a real capability loss.

The one route that actually removes the peak is in MuPDF, not in our file: keep
the font stream as a stream (or read it into a buffer we own and hand to
`fz_new_font_from_buffer` with `FZ_FONT_*` already sized) so it never
doubling-grows past the live pixmap.  We already ship a MuPDF patch set
(`patches/mupdf/source/fitz/`), so this is a patch + `libmupdf.a` rebuild +
`--pdfmod-only` flash, not an engine rewrite.  It is unprioritised because one
page of one book is the only observed case; the failure is loud (`tile render
failed` / alloc log) rather than silent, so a recurrence is measurable.
Note that `src/pdf/PdfDocument.cpp` compiles **into the engine**, so any change
here needs `scripts/build_pdf_module.py` and a `pdfmod` reflash, not an app flash.

## Verified matrix (device, 2026-11-18)

| case | result |
|---|---|
| `CMD:PDFMODTEST` | engine entry/ABI OK, code window `0x42d30000`, `code=1134176 data=28296 bss=25340` |
| 2-page embedded-TrueType text PDF | text extracted (373/397 chars), bitmap renders, no heap corruption |
| 2-page text-heavy PDF | reflow probe on, page 1 reflows with reader settings |
| 112-page real text/figure PDF, page 68 | reflow on, `reflow layout: font=687012015 width=624 height=1153 margin=25 line=45 indent=30 paraGap=0`, `22 lines, 1 screen` |
| same book, page 19 | previously the only unrecoverable page; now `57 lines, 3 screen(s)` via reflow |
| same book, page 41 | previously failed in `pdf_load_embedded_font`; out-of-core fonts + reflow give `59 lines, 3 screen(s)` |
| same book, page 60 | `75 bytes` of text → reflow rejected, bitmap path `renderPage ok page=60` (674-wide figure; ink-bbox crop found no meaningful margin) |
| one-page RGB DCTDecode PDF with white margins | `grayjpeg=1`; content pre-crop + ink bbox yields `624x972 -> 597x938`, black artwork fills the reader's usable area with its 1% pad |
| AES-256 encrypted PDF, password `a` | password keyboard appears; after typing and Confirm `opened: pages=2 outline=1`, page renders; wrong password re-prompts |
| encrypted PDF with an embedded TrueType body | unlock succeeds and the page is visibly rendered (screenshot archived) |
| 3-page PDF with native outline | `opened pages=3 outline=3`; toolbar Contents has three entries |
| seeded highlight on page 10 of the 112-page book | underline persists across close/reopen from `annotations.bin`; no panic |
| English reflow fixture, Hyphenation on | visible line-end hyphens (`terminology` → `termi-` / `nology`, `electroencephalography` → `elec-` etc.); 25 lines, 1 screen |
| `/test.pdf` (base-14 text) | renders blank on purpose: built-in base-14 fonts are trimmed; use an embedded font |
| open/exit a PDF then return to Home | CJK family, MiSans and the full 844 KB emoji TTF reload; no `not enough PSRAM` line in the clean case |

## Known constraints

- The data alias must be MMU-mappable; the loader fails closed otherwise.
- **Embedded fonts are out-of-core.**  Large font programs (compressed
  `/Length` ≥ 64 KiB) are decompressed in 4 KiB chunks to
  `/.crosspoint/pdf_font_cache/<hash>.fnt`; FreeType reads them through a
  custom `FT_Stream` whose callbacks use the app's `HalStorage`, so the font
  bytes do not stay in PSRAM.  Small fonts keep the old in-memory path, and
  building without SD falls back silently.  Cache files are reusable across
  pages/attempts and keyed by an FNV-1a hash of the decompressed program.
  This is what makes page 41 of the stress book render: it used to die in
  `pdf_load_embedded_font` with several MB of font buffers (allocation stacks
  at `fz_read_best`/`inflate`/`pdf_load_font`); with out-of-core fonts the page
  completes 674×873 in ~15 s with min free ~687 KB.  SD read latency is the
  price; the first bitmap/out-of-core pass on a hard page takes roughly
  10–15 s, and the first stext/reflow pass adds a few seconds more.
- A page whose decoded figures/vectors exceed available PSRAM still fails
  cleanly (reader error page), not with a crash.  Page 41 of the stress book is
  no longer in that class: its large embedded fonts are streamed from SD
  (`out-of-core`) and its body reflows with the reader font.  The allocator
  traces that led to out-of-core fonts are kept in the history of this
  document.  Attempts measured and rejected for other hard pages: image-budget
  downscaling (96 KB / 2–4 MB), full-page retries at 100–40 %, 384-row and
  64-row tiles, text-only tiles, a bounded 2 MB MuPDF store, and reclaiming the
  emoji TTF.  They die inside the page run with `malloc/realloc failed` when
  the page's own content exceeds free PSRAM.  Colour JPEGs now decode straight
  to one gray channel (≈3× less image memory, see
  `patches/mupdf/README.md`), which helps scans and figure-heavy pages but
  cannot rescue a vector-bound page.  Out-of-core/streaming interpretation or
  rendering elsewhere would be needed for those pages; neither is implemented.
- Reflow deliberately uses the reader's typography; it does not reproduce the
  PDF's embedded fonts or line styling.  Bitmap pages preserve the original
  fonts/styles exactly.  A page that mixes a large vector illustration with
  enough text to pass the reflow probe can still be reflowed with the vector
  artwork omitted; `FZ_STEXT_BLOCK_IMAGE` and side-by-side text blocks are
  detected, but arbitrary vector drawings are not.  Pages whose body has no
  Unicode mapping fall back to bitmap rendering.
- Two-up spreads are not split into single pages; a spread is fitted as one
  bitmap.  Encrypted/password-protected PDFs, malformed files, missing external
  CMaps, damaged embedded fonts, and SD-full/cache-write failures surface as
  the reader's load/render failure page; a page turn retries the page.  There
  is no password prompt or per-feature error code yet.  The out-of-core font
  cache lives in `/.crosspoint/pdf_font_cache/` and is reusable; deleting that
  directory or the per-book `/.crosspoint/pdf_*` cache only costs a rebuild.
- The module is tied to the firmware ELF (see OTA notes).
- app1 is 5.375 MiB in the XIP table; firmware must stay below that.
- Chinese CMaps must be present on SD for fonts that use external CMaps.


## ★ Two crash classes found by "PDF after a network session" (2026-11-20)

Both were reproducible only in this order: Wi-Fi used earlier in the boot, then a
PDF opened.  Repro command: `CMD:WIFICONNECT as` then `CMD:PDF /testcomplex.pdf`.

### 1. The module zeroed the APP's external-RAM bss on every open (fixed)

`PdfDocument::load()` did `memset(_ext_ram_bss_start, 0, end-start)` -- a guard
written when MuPDF's `.ext_ram.bss` was landing outside the loader's memset.
`_ext_ram_bss_start/_end` are **linker layout symbols of the app image**, and
`build_pdf_module.py: host_provides()` exported every app global to the module,
so the module resolved them to `0x3c530000..0x3c5339b0` (14,768 B): the app's own
`EXT_RAM_BSS_ATTR` region.  Every PDF open wiped live firmware state that the
Wi-Fi stack keeps there, and the victim was lwIP:
`assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))` inside
`tcpip_thread` during the first page render (`nm` proof: the module ELF carried
the identical absolute values as the app ELF).

Why it looked intermittent: with no Wi-Fi ever used, those globals are already
zero, so the memset was harmless -- which is how it survived every earlier PDF
test.

Treatment, in force order:
1. The loader already zeroes the module's own bss through the alias and reads it
   back before calling the entry (`PdfModuleRuntime` memset + verify).  The
   module-side memset was redundant and was removed.
2. `host_provides()` now withholds linker layout symbols
   (`^_[a-z0-9_]+_(start|end|begin|limit)$`) while still exporting compiler
   runtime helpers (`__divsf3`, `__stack_chk_fail`).  If the module ever needs
   such a value again, it must come from a **host function**, not an address.
3. `scripts/tests/test_module_symbol_binding.py` pins all of it (4 checks:
   withheld list, helpers still flow, real builder output clean, linked module
   ELF references none).

### 2. The PHY tracking timer, created by modem sleep, aborted on a thin heap (fixed)

```
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at phy_common.c:118
func: phy_track_pll_init, expression: esp_timer_create(&phy_track_pll_timer_args, ...)
```

Two earlier explanations of this were wrong, and both are worth naming because they
shaped the first two fixes:

* "it happens on the second `WiFi.mode()` of a session" -- no. `phy_track_pll_init()`
  is called by IDF's **modem-sleep PHY power gating**, i.e. every time the radio
  sleeps toward a DTIM beacon, at a moment chosen by the power manager. No call site
  guards it, which is why gating the start call never stopped it.
* "the `largest` free block predicts it" -- no, and this is the second time a
  contiguous-block threshold in this area was wrong (it also refused an update whose
  radio was already connected, at `largest=3,828`). Measured: association *succeeded*
  at internal `largest=7,156`, and `esp_wifi_init` once failed with `ESP_ERR_NO_MEM`
  at a *larger* `largest=7,668`. The driver wants many small internal/DMA blocks, so
  total free is the quantity that means anything.

Fix, in force order:
1. **Make the code path not exist**: keep modem sleep off while the STA is up.
   Asking once at `WiFi.mode()` is not enough -- the driver is not started there, and
   Arduino's connect path plus `WifiPowerSaveGuard`'s restore both write `MIN_MODEM`
   back. `NetworkStartup::pollIdle()` therefore re-asserts `WIFI_PS_NONE` once a
   second, logs the verified transition per radio session, and logs ERR when it
   cannot be applied while connected. `CMD:WIFIPS` reads the state back on demand
   (`exp/pdf_wifi_repro.py` fails a round unless PS is *proven* off).
2. **One owner for the memory decision**: `NetworkStartup::canAffiliate()` is applied
   inside `setMode()` *after* `prepare()` reclaimed render/font memory, on total
   internal free. Callers do not keep their own thresholds; the update screen only
   asks it afterwards to name the reason (内存不足 vs 网络不通).

Cost of (1) is idle-connected current while the radio is up -- accepted, against a
reboot in the middle of a book. The deeper reduction (why internal RAM gets to a few
KB at all) is tracked in AGENTS.md section 85.4: Wi-Fi association itself is what
takes ~39 KB and drops `largest` from 31.7 KB to 11.3 KB.

★ **Superseded 2026-11-21**: this paragraph used to end with "radio release on idle is
still disabled because every teardown variant so far panics inside `tcpip_thread`".
The deep teardown (`NetworkStartup::releaseRadioDeep()`) now runs clean on device --
twice, `mode=0 free 26,843 -> 51,907` -- and the panic it was blamed for was a symptom
of crash class 1 (the engine module zeroing the app's external-RAM bss, fixed).  What
is still true is the *scope*: the release is not automatic on idle, only on entry to
the PDF reader, and it must keep WiFi storage in RAM while running on a PSRAM stack
(AGENTS 85.13).

## ★ Reflow 的单字测量：真正的成本，以及一次把自己引进死胡同的归因（2026-11-21）

`PdfReader first frame: render=25.7 s` 里最大的一块是 `buildReflow()`。上一轮我在
AGENTS.md §85.8 写下"一次单字测量要 20–36 ms"，并据此设计了一套"用首尾两小段采样
代替整段测量"的规则。**那个归因是错的，采样规则因此也是错的**。真机数字如下。

### 测量事实（同一本书、同一页，逐段日志）

```
改前：reflow measure: 63 codepoints in 1097 ms (chars=389)
      reflow measure: 60 codepoints in 9978 ms (chars=3913)     ← 12 s 页面构建里的一段
改后：reflow measure: 38 calls in 4 ms  (chars=686 scale=1.191)
      reflow measure: 40 calls in 20 ms (chars=873 scale=1.098)
      PdfReader first frame: render=8155ms                      ← 原 25,700 ms
```

错在哪：计时器把**整段字符串的 `getTextWidth`**（不在 `measureCalls` 计数里）和
逐码位测量算在同一个区间里，于是我拿"时间 ÷ 次数"当作单字成本。真正的病根是
**旧代码只给 ASCII 做记忆表**（`int asciiWidth[128]`），非 ASCII 每个字符都重新量一遍；
CJK 段落重复率极高（873 字符只有 40 个不同码位），所以这是纯浪费。
把记忆表换成"码位 → advance"的开地址表（`reflow_metrics::AdvanceMemo`，512 槽、3 KiB 栈）
之后，**整段测量本身只要十几毫秒** —— 我为了躲它设计的采样逻辑毫无必要。

### 采样规则的代价不只是无用（一次内存回归）

采样需要"码位序号 → 字节偏移"的第三张每字符数组（`std::vector<uint32_t> byteStarts`）。
`buildReflow()` 里已经有 `rawWidths` 与 `chars` 两张，再加一张之后，翻一页就变成：

```
[MEM] Free: 10155 bytes, Min Free: 7172, MaxAlloc: 2932      ← 改前同类操作 ~27–36 KB
TASKS count=14 renderHolder=(none)  … TASK ActivityManager st=rdy … TASK loopTask st=run
```

内部 RAM 掉到 10 KB、最大可分配块 2.9 KB 并且不再恢复，渲染任务停在 `rdy`。
**这里的纪律是：`buildReflow()` 里任何"每字符一个元素"的数组都是在花射频要用的内存**
（§7 的同一个坑换个形态重现），不能因为"只多 4 字节/字符"就加。
最终形态因此是：记忆表（栈上、固定 3 KiB）+ 一次精确的整段测量，**没有任何新增容器**。

### 规则本身被测试钉住

`src/activities/reader/ReflowScale.h`（不引渲染器/存储/ESP 头）+
`test/reflow_scale/run_reflow_scale_test.sh`：**22 项检查**、
3 个变异（去掉合理区间带宽、去掉装载因子、把探测长度从 8 改成 1）都必须变红。
其中"重复文本只量一次"是**用性质表达**的（2,000 次使用 / 40 个不同码位 ⇒ 恰好 40 次测量），
所以不需要真机就能抓到回归；冲突键绝不返回别人的宽度（先比 key 再读值）也被单独钉住。

## ★ 第三类故障：不是崩，是"翻页没反应"（2026-11-21，已定位并改成可见）

`waitPdfJob()` 原来是无截止时间的轮询。真机抓到一次 `kind=1 page=0`（Render）**180 秒没回来**，
期间日志里什么都没有：UI 任务在 `vTaskDelay`（task dump 里显示 `block`），
worker 卡在 MuPDF 里，用户看到的是"按了没反应"，而且本机没有复位键。

定位到的选择错误（`src/pdf/PdfDocument.cpp`）：整页这一档是用 **PSRAM 总空闲** 判定的
（`freeNow > outBytes + 1.5MB`），而整页 pixmap 是**一次连续分配**。
实测那一轮 `free=1,422,708 / largest=573,428`，而 pixmap 需要 1.09 MB 连续 ——
于是选了一条不可能成功的档，并且那件事在日志里完全不可见。
另外两处预备复位与 strip 兜底都用 `pageCount_ > 32` 当门，
而 `testcomplex.pdf` 只有 2 页 —— 页数从来不是这里的物理量，内存才是。

改成：
- 整页档必须同时满足 `largest > outBytes + 256KB`；否则走 tiles（tiles 每片都打一行进度，
  所以"慢"和"死"在日志里从此可分）。
- 预备复位与 strip 兜底的条件改为直接看 `heap_caps_get_*`，不再看页数。
- 等待侧：**渲染 75 s、打开/抽取 180 s** 的截止时间 + 每 10 s 一行
  `pdf worker waiting: kind/page/elapsed/free/largest`，超时**返回失败并显示失败卡**
  （不重启、不中途删任务：worker 仍可能持有指向调用者容器与 MuPDF 的指针，中途删任务就是本项目
  已经付过学费的堆损坏）。同步作业搬进静态槽 `gSyncJob` + `gSyncJobInFlight`；超时后槽位要等
  worker 自己报 `done` 才回收（`awaitSyncSlot()` 用同一套等待规则等它），渲染侧的降级阶梯另有
  **90 s 墙钟预算**，所以出卡之后 worker 最多再跑十几秒。队列满/重叠提交都写日志。
  早期版本写的是"超时重启"——那正是后来被删掉的设计，看到旧日志别照着改。

规则有主机测试：`test/pdf_job_wait/`（15 项 + 4 个变异，含 `millis()` 回绕、
"只在超时时报"、"进场就报"、"0 超时变成无限等"）。
**尚未查完的部分**（诚实记录）：那一次 Render 到底停在 MuPDF 的哪一步仍未知 ——
现在它要么走 tiles 并逐片打点，要么在 75 s 后带着内存快照**返回失败、显示失败卡**，
引擎侧的阶梯也会在 90 s 自行收手——三者都会自己报名字，不再需要猜。

## ★ 阅读时分段体验：把"慢/挂"变成可读、可退出的状态（2026-11-21）

这一轮把"PSRAM 很碎时读大图页"从**静默变慢/静默挂死**改造成**三种可见状态**，
并顺手关掉了系统里两个正在偷速度的东西。

### 1. 进阅读器就彻底关射频（已验证，且推翻了旧结论）

旧结论是"任何 teardown 都会在 `tcpip_thread` 崩"，所以 `releaseRadio()` 只敢断开关联、
留着协议栈 —— 而协议栈本身就是那 ~25 KB 内部 RAM 的持有者。真机实测（本轮）：

```
deep radio release: mode=0 free 26843->51907 largest 7156->10228
```

两次连续执行都干净、之后还能重新 `WiFi.mode(STA)` 并再次连接。旧崩溃与本轮已修的
**崩溃类 1（模块把 app 的 EXT_RAM_BSS 清零）**高度相关：那段 bss 被清掉后 lwIP 引用了
无效状态，teardown 才在 `tcpip_thread` 里断言。现在 `PdfReaderActivity::onEnter()` 调
`NetworkStartup::releaseRadioDeep()`（disconnect → `WiFi.mode(WIFI_OFF)` → `esp_wifi_deinit()`），
**AP 模式不动**（文件传输开着服务时不能被拆）。`CMD:WIFIDEINIT` 保留为回归入口。

首次发生这件事时弹一次说明（`SETTINGS.pdfWifiHintSeen`，与首阅手势提示同一种一次性机制）。
★ **文案受限**：内嵌英文 blob 只剩 12 字节余量（15-bit 偏移上限 32,767），
新增一句说明会被 `gen_i18n.py` 拒绝；因此标题用现有 `STR_WIFI_IDLE_TIMEOUT`、
正文用现有 `STR_DICT_LOW_MEMORY`，不新增键。

### 2. 渲染过程中的 UI

- **等待卡**：翻页前先上 `status_card::showWait()`（鸭子波浪 + `第 N 页 / 共 M 页`），
  不再是一行居中的 "loading"。
- **长渲染**：`waitPdfJob` 每 10 秒回调一次，把卡片重画并追加已用秒数（`12 s`），
  这样"慢"和"死"在屏幕上是两回事。
- **彻底失败**：`status_card::paintMessage` 画"PDF 读取失败"卡，并明确两条出路 ——
  **确认=重试**（重开会清掉 `.failed` 标记，真的会重跑整条回退阶梯）、
  **翻页=跳过**（其余页仍可读）。任何按键/点按都能离开这张卡；不再只有 Back 一条死路。
  `CMD:PDFFAILDEMO` 是这张卡的稳定复现入口。

### 3. 顺手修掉的两个"系统级偷速度"

- **串口命令被模态吞掉**：键盘/弹窗盖在阅读器上时，`CMD:PDF*` 原来只打一行
  `ignored: foreground activity is KeyboardEntry`。现在若阅读器仍在栈里，会
  **先关掉那层模态并提示"resend the command"**，把"命令发错"和"被挡住"分开；
  验收脚本收到这一行会自动重发一次。
- **字体下载限频**：`FontDownloadActivity` 的 manifest 与字体文件下载原先没有
  `HalPowerManager::Lock`，空闲 3 秒就掉到 80 MHz（OTA 那条线实测因此只有 4 KB/s）。
  现在两处都持锁（quiet），与字体扩展/引擎更新的做法一致。

### 两种页面场景的确定性夹具（2026-11-21）

失败卡与"慢但能读"的等待卡，过去都只能靠碰运气遇到。现在有两份夹具，
生成器 `exp/make_hard_scan_pdf.py`（PyMuPDF，6 秒），产物 gitignore：

| 场景 | 文件 | 形状（主机实测） | 设备上应发生 |
|---|---|---|---|
| **必然失败** | `hard_scan.pdf`（7.3 MB，6 页） | 每页 2400×3100 灰度 JPEG，**解码需 7.44 MB**，输出 624×806 | `CMD:FRAG 1024 10`（可用 ~2.24 MB / largest ~0.9 MB）后开书 → 解码放不下而失败。**两处都可能先报**：`render N failed: <MuPDF reason>`（快速失败）或 `pdf worker slow: … exceeded 75000ms`（若噪声吞掉），二者都会走到失败卡（重试/下一页），**不重启** |
| **慢但能读** | `slow_scan.pdf`（1.2 MB，3 页） | 每页 1400×1800，**解码需 2.52 MB** | **不加 FRAG**（PSRAM 充裕）：能放下但逐片渲染要十几秒 → 等待卡每 10 s 刷新已用秒数。<br>★ 加了 `FRAG 1024 10` 它也会失败（2.52 > 2.24），想模拟"慢"就把 FRAG 调到留 ≥3 MB |

用法（设备恢复后一条命令内可复现）：

```
CMD:PDF /hard_scan.pdf        # 先确认正常堆下能读（应成功）
CMD:FRAG 1024 10              # 占住 ~3 MB，largest 掉到 ~0.9 MB
CMD:PDF /hard_scan.pdf        # 应走失败卡；Confirm=重试、翻页=跳过
CMD:UNFRAG
```

★ 为什么不用 `CMD:FRAG` + `testimg.pdf`：那张图只有 625×808，解码量小，
撕裂后的堆也放得下，实测 7 s 正常渲染完——它证明不了失败路径。

### 为什么"单字宽之和"永远不等于"整串宽"（2026-11-21，读代码定案）

之前把重排里 `widthScale = 整串宽 / 单字宽之和` 当成一个需要"校准"的系数，并试过两套估算。
读 `lib/EpdFont/EpdFont.cpp:46` 后可以确定它**不是**可校准的常数：

```cpp
const auto kernFP = missing || prevMissing ? 0 : getKerning(prevCp, cp);
lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);   // 12.4 定点，一次取整
```

- advance 与"到下一个字的 kern"在 **12.4 定点**里相加后**只取整一次**，所以
  `Σ toPixel(adv_i) + Σ toPixel(kern_i)` ≠ `toPixel(Σ(adv_i + kern_i))`；
- 测量前还有 `applyLigatures()`，整串的**字形数**可能少于码位数；
- 因此系数随脚本/字形组合变化（实测 1.09–1.20，与段落长度无关）。

**结论（写进 `ReflowScale.h`）**：整串测量是唯一忠实的模型；在 wrapper 里自己累加 kern
会每对差半个像素，行断点就会变，属于"看着省一点、实际改版式"的坑。
代价也可接受：有了码位记忆表之后，873 码位的段落整段测量是 **~20 ms**。

### 验证状态（2026-11-21，主机侧全量）

| 范围 | 结果 |
|---|---|
| `test/*/run_*.sh` 全部 38 套 | **38/38 通过**（含本轮新增 `pdf_job_slot`、`pdf_job_wait`、`pdf_fail_card`、`reflow_scale`） |
| `scripts/tests/test_download_power_locks.py` | 9 in-file / 10 upstream / 3 impl，0 失败；**删一个锁会精确点名并失败** |
| `python3 -m unittest discover -s scripts/tests` | 277 项：73 项失败/错误，**全部为本树既有问题**，与本轮改动无关。逐一核对过最可疑的三项：`test_committed_assets_match_the_generator`（`assets/fonts/CPYOSHanSansSC-UI.ttf` 与生成器不一致，该资产最后修改是 2026-11-17 的 `cee8bbdc`，早于本轮）、`test_every_ui_codepoint_has_a_face`（缺 U+64CE/U+6784/U+69FD/U+6D3B/U+95F2，其中 构/槽/活 不在任何翻译文件里，扫描源的是其他字符串集合）、`test_settings_and_glyph_coverage`（`STR_THEME_CPY` 中英不一致，AGENTS §29.6 已记录） |
| 设备侧 | **未完成**：设备在 2026-11-21 10:26 的一次被打断刷写后不响应（USB 枚举正常、ROM 与固件均无输出），需断电恢复。待补：三镜像刷写、两张卡片实拍、两份夹具各一次、`pdf_verify_matrix.py` 全量 |

### 换 PSRAM 大小的鲁棒性：渲染余量按总 PSRAM 派生（2026-11-21）

渲染阶梯原来把三处余量写成本机的数字：整页档 `+1.5 MB` 空闲 / `+256 KB` 连续块、
预备复位 `+4 MB`、strip 兜底 `< 3 MB`。在一台 2 MB PSRAM 的板子上，
"重开文档省内存"的 4 MB 预订永远成立（每页都白付 0.6 s），strip 兜底永远不触发
（正是最需要它的地方）。

现在 `src/pdf/PdfRenderBudget.h` 按 `heap_caps_get_total_size(SPIRAM)` 派生：
**≥6 MB 走实测那组常数（本机数值一字不变）**，更小的板子按 `total/8`、`total/8/4`、
`total/2`、`total/3` 缩放。测试 `test/pdf_render_budget`（26 项）同时钉住
"8 MB 数值不许漂"与"缩放必须随堆单调"，三个变异（常数漂移、缩放不看堆、连续块余量
大于空闲余量）都必须变红——第二个变异第一次是**存活**的，因为当时只断言了"不越界"而没断言"随堆缩放"。

### 设备验收手册（一条命令 + 两个夹具，2026-11-21）

前置：设备能开机、插在 Mac 上。**刷写期间不要打开任何串口工具**（§AGENTS 85.11）。

```bash
# 1. 刷写 + 基础复现 + 两张卡片截图 + 完整矩阵
bash exp/pdf_verify_round.sh
#    成功判据：verified images 3/3；pdf+wifi repro 2/2 clean；
#    artifacts/fail_card.png、artifacts/wifi_hint.png；矩阵尾部无 FAIL

# 2. 必然失败页（需要卡上有夹具；WebDAV 拒绝带点路径，必须走 /upload）
bash exp/put_on_card.sh <设备IP> exp/fixtures/hard_scan.pdf    # 需先开"文件传输"
#    然后：
#    CMD:PDF /hard_scan.pdf      → 正常堆下应能读（否则换更小的夹具）
#    CMD:FRAG 1024 10            → 占住 ~3 MB，largest 掉到 ~0.9 MB
#    CMD:PDF /hard_scan.pdf      → 期望：pdf worker 超时 → 失败卡
#                                  连续三页失败后标题变「此 PDF 无法显示」，右钮变「返回」
#    CMD:UNFRAG

# 3. 慢但可读（等待卡的秒数）
#    CMD:PDF /slow_scan.pdf      → 不加 FRAG；等待卡每 10 s 刷新已用秒数
```

期望日志（可直接 grep）：
`deep radio release: mode=0 free X->Y`（Y−X ≥ 8 KB）、`wifi-off hint armed`（**只一次**）、
`render-failure card: retry at …`、`leaving the failure card for page change`、
`pdf worker slow: kind=… exceeded 75000ms`（真实磨页才会出现）、`page N failed (3 in a row, this file cannot be shown here)`。

### 切 env 会退回 vendor core（2026-11-21 再次踩到，判据补一条）

为了验证非 PSRAM 目标，跑了 `pio run -e default`（C3）。它重新生成了项目根的
`CMakeLists.txt` / `sdkconfig.defaults` / `.dummy`，并让随后 `readpico_pdf` 的构建
**链接了 Espressif 预编译 core**：镜像 `esp_app_desc` 的 `version` 变成
`arduino-lib-builder` 那套（本次为 `ee57070`）。按 §6 配方清理脚手架 +
删包内 `s3/sdkconfig.crosspoint`（先备份到 `artifacts/`）后重编，构建日志出现
`Archiving …libframework-arduinoespressif32.a`，RAM 回到 **90,080 B（27.5%）**。

★ **判据补充**：`esp_app_desc` 的 `project_name/version` 可能来自**共享的**
`libesp_app_format.a`（由最后构建的那棵树生成，本次还带着另一棵树的
`backup/dev-pre-reconcile-2026…` 字样），所以在本机它**不能**单独用来判定 core 身份。
可靠的组合是：
1. 构建日志里有 `Archiving … libframework-arduinoespressif32.a`；
2. 静态 RAM 与基线同量级（readpico_pdf ≈ 90 KB；vendor core 会高约 15.5 KB）；
3. 无警告地链接完成。

**顺序建议**：需要验 C3 时，先跑 C3 再跑 S3 并做上面的 core 校验；反之如上所述必须按 §6 重编。

### 镜像自证（2026-11-21，构建产物里的字符串核对）

按 §11.10 的规矩（“编译通过 ≠ 代码在镜像里”），对本轮构建产物逐个核对：

| 字符串 | 应在 | 实测 |
|---|---|---|
| `render step: bmp header written` / `pixmap alloc` / `draw device created, run_page start` | `build/pdfmod.bin`（模块） | 各 1 |
| `pdf worker waiting:` / `pdf worker slow:` / `deep radio release: mode=` | app ELF | 1 / 1 / 1 |
| `render-failure card: retry at` / `leaving the failure card for page change` / `wifi-off hint armed` | app ELF | 1 / 1 / 1 |
| `This page cannot be shown.` / `PDF cannot be shown.` | app ELF（EN i18n） | 1 / 1 |
| `build/pdfmod.bin` 与 `site/pdf-engine-test/pdfmod.bin` | 同一文件 | sha256 `e1ff25358163ea2f…` 相同 |

`exp/pdf_chain.sh` 现在在**重链接模块之后**自动做这组断言：缺任何一个字符串就拒绝继续刷写。

★ 为什么"必然失败"不一定等到 75 s：选档用的是 `largest > outBytes + pixmapSlack`
（0.9 MB > 0.5 MB + 256 KB 成立），所以它会先走整页档，而 MuPDF 在 **源图解码**（7.44 MB）时才失败，
通常在几秒内抛 `malloc/realloc failed` 并返回 false。两种情况都出同一张卡，区别只是日志里哪一行先出现——
所以验收时两行都要接受，不要只 grep 超时那一行。

### 降级阶梯的最后一级不能变成"白纸"（2026-11-21）

`PdfDocument::renderPage()` 失败后会依次重试：整页 100% → 90% → 80%（缩图）→ 80%（关 AA）→ 60%（极小图）
→ 分带 → **text-only** 100/80/60/40%。阶梯本身是对的——多数页在中间某一级就能出图。
问题在最后三/四级：`text-only` 会把图像全部跳过，而**扫描件的正文就是图像**，于是这一级"成功"并写出
一张白纸。用户看到的是空白页而不是失败卡，正是"静默失败"里最糟的一种（既没内容也没提示，还得自己
猜是不是书坏了）。

规则（`src/pdf/PdfInk.h`，`test/pdf_ink` 3 个变异钉住）：**只对 `skipImages` 的那几级**做墨量检查。
墨量来自 `stretchContrast()` 已有的直方图——平坦页（只有一种色调）报 0，其余报"低于中间调的比例"；
低于 0.05% 视为空白，删除该 BMP、该级判失败、继续阶梯，最终由失败卡接管（可跳过 / 可返回）。
真·空白页不会误伤：它在前几级就正常渲染成功，根本走不到 text-only。

判据（串口）：`render N: text-only at 40% rendered blank (ink 0.000%); images were the content`
之后要么 `text-only … -> fail` 收尾进失败卡，要么更早一级已成功。

### 退出阅读器就卸载引擎模块（2026-11-21）

"整个 PDF 阅读器只在阅读时常驻"落到代码：`PdfReaderActivity::onExit()` 在文档关闭、worker 已停之后调用
`pdf_module::unload()`，把引擎镜像从地址空间摘掉（1.8 MB code/rodata 的映射 + `.data/.bss` 的 ~54 KB PSRAM）。
所有消费者（设置页的引擎信息、更新页、阅读器）都先 `ensureLoaded()`，所以下次打开会按同一条首次加载路径
重新映射——不是新代码路径。

★ **唯一例外**：`stopPdfWorker()` 5 秒内没等到同步作业结束时会**故意泄漏**任务（不能在 MuPDF 里删任务），
此时文档可能还活着，于是**跳过卸载**并打 `engine module kept mapped: worker stopped=0 documents=N`。
判据：干净退出是 `engine module unmapped; PSRAM free …`。

### 超时之后的那次翻页：等槽位，而不是立刻判该页失败（2026-11-21）

超时的同步作业**不会**归还槽位（`syncSlotFree()` 只在观察到 worker 自己的 `done` 后才回收——超时就丢弃作业等于在 MuPDF 里改写它的内存，这是已经付过两次代价的堆损坏）。
于是"75 s 超时 → 用户按跳过 → 下一页"时，槽位往往还被那个慢作业占着。旧行为是**立刻拒绝**并把新页判成失败：
用户看到的是"这张图也显示不出来"，而它其实一次都没被渲染过。

现在 `awaitSyncSlot()` 用**和渲染等待同一条规则**（`PdfJobWait`：10 s 起报、可 tick 到等待卡、按调用方预算超时）等槽位归还；
等到了就正常渲染，等不到才判失败并打 `previous job did not release the sync slot within …`。
判据：超时后翻页的日志先出现 `waiting Nms for the previous job to release the sync slot`，
随后是正常的 `renderPage: page=…`，而不是立刻的 `render refused`。

### 泄漏的 worker 不能在新会话里"复活"（2026-11-21，静态审查发现）

`stopPdfWorker()` 在 5 s 内等不到同步作业时会**故意泄漏**任务与队列（不能在 MuPDF 里删任务）。
审查时发现 `pdfRenderWorker` 原来在循环里读**全局** `gPdfWorkerQueue` 而不是任务参数：
被泄漏的任务在 MuPDF 里跑完后回到循环，读到的已经是**下一个会话**刚创建的队列，于是它会接走新会话的作业——
两个 worker 同时在同一个文档上跑 MuPDF，正是泄漏本该避免的那类损坏。
修法：队列句柄在 `xTaskCreatePinnedToCoreWithCaps` 时作为 `pvParameters` 传入，worker 只认自己的队列；
泄漏的任务此后永远阻塞在一个没人发送的队列上，按设计"惰化"。
判据（静态）：worker 体内不得出现 `gPdfWorkerQueue`。

### 阶梯本身有墙钟预算（90 s，2026-11-21）

应用侧在 **75 s** 停止等待并显示失败卡，但阶梯当时仍在 worker 里一级一级重开文档重试——每级都要
重新解析页面，于是槽位被占着，下一页只能等它。现在 `PdfDocument::renderPageToBmp()` 在入口设一个
**90 s 的墙钟预算**（`kLadderBudgetMs`，RAII 作用域保证退出即清）：`attempt()` 在开新一级前检查、
`renderTiles()` 在每带之间检查，超时就打 `ladder budget reached before …` / `ladder budget reached after N tiles`
并返回失败。

为什么是 90 而不是 75：卡出现后仍给"马上就能画完"的页留 15 s 余量（否则会出现"差一点就成功却被我们自己砍掉"）。
为什么仍然不是"可中断"：MuPDF 的单次调用无法从外部取消，要真中断得改引擎 ABI（暂不做）；
90 s 把"卡之后还可能磨几分钟"压成"最多十几秒"。

### ★ 超时后必须**继续拥有**槽位（2026-11-21，审查发现的既存缺陷）

`renderPdfPage()` 原来在 `waitPdfJob()` 之后**无条件** `gSyncJobInFlight.store(false)`（加载/抽取两条
路径用的是 `slotReturnedByWait()` 规则，只有渲染这条漏了）。后果：一次渲染超时把槽位交还，而下一次
提交会覆写 `gSyncJob`——worker 可能还在 MuPDF 里、手里就攥着这个槽位里的 `std::string path` 和
`pdf` 指针。这正是 `test/pdf_job_slot` 一直在防的堆损坏，只是渲染路径绕过了它。

修法：四处等待统一走 `releaseSlotAfterWait(completed)`（内部就是 `slotReturnedByWait`），
新增 `scripts/tests/test_pdf_slot_release.py` —— 逐条检查“每个 `waitPdfJob` 之后都有
`releaseSlotAfterWait(completed)`”“`slotReturnedByWait` 只出现在该 helper 里”“helper 不递归”。
**已在旧代码上验证为红**。
★ 这也是 `awaitSyncSlot()` 能真正起作用的前提：如果槽位被提前交还，等待逻辑根本不会被触发。

### 无界等待清点（2026-11-21，定位工作的收尾）

原始事件的"不返回"来自**等待侧**没有截止时间。为确保这类问题不再以别的面目出现，对 PDF 路径上
所有阻塞点做了一遍清点（`grep portMAX_DELAY|for(;;)|while(true)`）：

| 位置 | 性质 | 结论 |
|---|---|---|
| `pdfRenderWorker` 的 `xQueueReceive(portMAX_DELAY)` | 常驻 worker 的空闲阻塞 | 设计如此；任务被泄漏后阻塞在**自己的**旧队列上（队列句柄来自任务参数） |
| `waitPdfJob()` | 应用侧等待同步作业 | 有 `PdfJobWait` 预算（渲染 75 s / 打开与抽取 180 s），10 s 起报状态 |
| `awaitSyncSlot()` | 起新作业前等旧作业归还槽位 | 渲染路径用页预算（75 s）；**打开路径只有 30 s**——打开发生在任何卡片画出之前，泄漏的旧作业不该让阅读器空等 180 s；超时落到可见的"打不开"卡 |
| `PdfEngineUpdate::sha256File()` 的 `while(true)` | 文件读循环 | 遇 0/负值即退出，受文件大小约束；下载本身由 `HttpDownloader` 超时约束 |

结论：**PDF 路径上不再存在"没有截止时间、且会把界面卡住"的等待**；剩下的唯一"不可中断但有序"的是
MuPDF 单次调用（见阶梯 90 s 预算一节）。

### 卸引擎必须在**内部栈**上做（2026-11-21，自审发现的真隐患）

`mmu` 解映射外部内存会让 IDF 做 cache flush/invalidate。`onExit` 通常由 **渲染任务**执行，
而它的栈就在 PSRAM——本分支已经因为"在 PSRAM 栈上碰 cache"吃过一次 `Double exception`（§35.6）。
所以 `onExit` 里直接 `pdf_module::unload()` 是一个**延迟引爆**的缺陷。

现在的规则：
- `onExit` 若发现自己在 PSRAM 栈上，只调 `pdf_module::scheduleUnload()` 并打
  `engine unmap deferred: onExit ran on a PSRAM stack`；
- `main.cpp` 的 `loop()`（内部栈）每轮调 `flushScheduledUnload()`：没有打开的文档、且当前栈在内部 RAM 时才真正卸，
  并打 `engine module unmapped by the main loop`；
- `scripts/tests/test_pdf_unmap_stack.py` 钉住"每个 `unload()` 调用点八行内必须有 `esp_ptr_external_ram` 判断"，
  以及 `flushScheduledUnload()` 自己保留该判断（把判断删掉的变异会变红）。
★ 这也是 `onExit` 里其它重载（字体、断词）早就采用的同一模式——它们本来就跳过 PSRAM 栈，只有我新加的卸载漏了。

### 深度关射频的"连带功能"：存回来的不只是模式（2026-11-21）

`releaseRadioDeep()` 为了在 PSRAM 栈上安全拆除，必须先 `esp_wifi_set_storage(WIFI_STORAGE_RAM)`。
但 **storage 是驱动级全局设置**：留着 RAM 模式，则本次开机后续任何 `esp_wifi_set_config`
（包括用户在 Wi-Fi 页输入的新网络）都不会写进 NVS——下次开机发现"密码没保存"，
而界面上当时一切正常。拆除完成后立即 `esp_wifi_set_storage(WIFI_STORAGE_FLASH)` 恢复。
（重新连接本身没问题：Arduino 在 `WiFi.mode(WIFI_OFF)` 内部 deinit 时把 `lowLevelInitDone`
置回 false，下一次 `WiFi.mode()` 会重新 `esp_wifi_init`。）
