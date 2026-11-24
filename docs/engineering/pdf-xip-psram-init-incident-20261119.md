# XIP PDF PSRAM init failure: incident, what was ruled out, what was fixed, and the recovery (2026-11-19)

This note records the 2026-11-19 incident on the Read Pico (RDP-G01-W, MAC
`14:c1:9f:d1:58:78`) where the XIP PDF engine crashed and every subsequent boot
reported `PSRAM free=0`, ending in a framebuffer/render-task assert loop.  It
also records the recovery procedure that was actually executed on the device and
the changes that landed afterwards.  Read section 3 first: the crash
itself is **not yet root-caused** (two explanations were tested and refuted), so
this note is as much a record of what has been eliminated as of what was fixed.

> **Correction to the earlier note.** The 2026-11-19 section
> "`PSRAM free=0` is hardware/power, not the image" in `pdf-xip.md` is **wrong**.
> The PSRAM chip was not damaged: it initializes at 40 MHz forced type, 80 MHz
> forced/AUTO, and 120 MHz (official firmware) after the recovery.  The
> `PSRAM free=0` was a **PSRAM initialization-state** failure caused by the XIP
> engine crash, not a failed 3V3 rail and not a dead chip.  Do not cite the
> hardware/power conclusion again.

## 1. Symptom and timeline

1. `0.7.0-pdf-xip` (app `ELF SHA256 02f2a087…`, pdfmod appTag
   `02f2a087c5d9f524…`) loaded the XIP engine:
   ```
   [INF] [PDFM] code mapped at 0x42d30000
   [INF] [PDFM] rodata mapped raw at 0x3df00000 (589824 bytes)
   [INF] [PDFM] data alias=0x3d800000 heap=0x3cbb0000 paddr=0x00680000 target=2
   ```
2. MuPDF's first read crashed:
   ```
   Guru Meditation Error: Core 0 panic'ed (LoadProhibited).
   EXCVADDR: 0x0000cdcd
   ELF file SHA256: 02f2a087c
   ```
3. Every reboot after that printed:
   ```
   [ERR] [HYPH] no PSRAM headroom ...
   [ERR] [I18N] no PSRAM for the pack index
   [INF] [DISP] PSRAM free=0 maxBlock=0 ...
   E (…) EPDiy: Highlevel framebuffer allocation failed
   [ERR] [GFX] !! No framebuffer
   assert failed: void GfxRenderer::begin() / ActivityManager::begin()
   ```
4. The console later dropped USB (`[port lost]`); the device looked dead.

## 2. What was ruled out (measured, not inferred)

| Hypothesis | Evidence | Verdict |
|---|---|---|
| Dead PSRAM / bad 3V3 | `psramtest` app: `esp_psram_init()` → `initialized=1 size=8388608`, `spiram_free=8386308` at 40 MHz and at 80 MHz forced and 80 MHz AUTO; the original 80 MHz CPYOS app boots with `PSRAM free≈1.6 MB`; the official firmware boots its UI at 120 MHz | **ruled out** (by the AUTO/80 MHz variants -- see caution below on the 40 MHz one) |

★ **Caution on the 40 MHz `psramtest` variant.**  It was labelled
`defaults_p40t64` and set `CONFIG_SPIRAM_TYPE_ESPPSRAM64=y`, which on ESP32-S3
selects **QUAD**, not the in-package octal PSRAM this board actually uses.  So
that data point says "the chip answers even when addressed as quad at 40 MHz";
it does **not** by itself prove octal-at-40 MHz works.  The octal evidence is
the `TYPE_AUTO` + `MODE_OCT` runs (80 MHz forced, 80 MHz AUTO) and the
production and rescue images, which both keep `MODE_OCT=y` + `TYPE_AUTO=y`.
The same trap is why the rescue env was rewritten -- see section 5.7.
| Bad bootloader / bad partition table | Device bootloader equals the factory backup (`15f88ae1…`, first 18,720 B); XIP partition table parses correctly (`nvs/otadata/app0/app1/pdfmod/spiffs/coredump`) | **ruled out** |
| Stale app/pdfmod pairing | Crash app ELF SHA `02f2a087…` equals the installed pdfmod appTag `02f2a087…` | **ruled out** |
| 80 MHz frequency itself | 80 MHz AUTO/forced test apps pass; the original 80 MHz app boots | **ruled out** |
| Official 120 MHz not usable | Official `read_pico_firmware` build (`PSRAM_SPEED=120`) boots to its UI | **ruled out** |

The `paddr=0x00680000` in the `PDFM` log is the **PSRAM physical address** of the
module `.data` heap (`target=2`), not flash/app1.  The loader maps it to
`kPdfModuleDataVaddr` and copies `.data` from `g_part->address + h.dataOffset`
(the `pdfmod` partition).  That part is correct.

## 3. Root cause: NOT established -- two hypotheses tested and refuted

The crash itself (`LoadProhibited`, `EXCVADDR 0xcdcd` inside `pdf_load_xref`) is
the origin of the whole incident, and it is **still not root-caused**.  Two
explanations were proposed during this investigation and both were killed by
measurement, so they are recorded here as *refuted* rather than quietly dropped.

**Refuted hypothesis 1 -- "the module's `.ext_ram.bss` is never zeroed".**
The story was: MuPDF/FreeType statics land in `.ext_ram.bss`, the module link
script does not collect that pattern into `.bss`, so they sit past `h.bssSize`
and the loader's `memset(dataAlias + kPdfModuleDataBssGap, 0, h.bssSize)` misses
them.  It is wrong:

- `scripts/build_pdf_module.py` **already** had `*(.ext_ram.bss
  .ext_ram.bss.*)` in the `.bss` output section.
- Deleting that pattern and re-linking produced a **byte-identical** image
  (`code=1132016 rodata=580224 data=28296 bss=25348 total=1863304` with and
  without it).  Byte-identical output means no input section of that name
  reaches this link at all -- there was nothing outside the memset to miss.
- `h.bssSize` = 25,348 covers the highest bss object (`nm -S`:
  `_cmsContextPoolHead` at `0x3d826298`, ending `0x3d82629c` < `0x3d826304`).
  That span check is now a build assertion (section 4, item 1), so the invariant
  is machine-enforced instead of asserted by prose.

**Refuted hypothesis 2 -- "`0xcdcd` is IDF heap-poison fill".**
`0xcd` looked like the signature of dereferencing freshly allocated, never
written memory.  Disassembling the *linked* app says otherwise:
`poison_allocated_region` (`.pio/build/readpico_pdf/firmware.elf` @ `0x4038b344`)
loads **`0xbaad5678`** for the payload and **`0xabba1234`** for the header word.
Uninitialized heap in this build reads `0xbaad...`, not `0xcdcd...`, so the
fault address did not come from an un-written `heap_caps_malloc` block.

What survives is the shape of the failure, and section 3.1 recovers it from the
log: the load completed healthily (mapping self-checks agreed), and *afterwards*
an app-side function dereferenced the value `0xcdcd` as an address while sitting
in a ROM copy loop.  So the open question is now narrow and concrete -- which
app-side copy/format path turned a `0xcdcd` field into a pointer -- rather than
"did the loader initialize the module".  It did.

Until that is answered, **do not write this incident up as "the .bss was not
zeroed"**.  It was not.

### 3.1 The crash's own console evidence (recovered from `device_log/live.log`)

The panic is still in the appended session log, registers and all.  Quoting the
shape, not the private file path that triggered it (the reproducer is
`CMD:PDFBIG 19`, which opens the dev-only large Latin-text test PDF at page 19):

```
[41515] [INF] [PDFM] code mapped at 0x42d30000
[41516] [INF] [PDFM] rodata mapped raw at 0x3df00000 (589824 bytes)
[41516] [INF] [PDFM] data alias=0x3d800000 heap=0x3cbb0000 paddr=0x00680000 aliasPaddr=0x00680000 target=2
[41529] [INF] [PDFM] calling entry=0x42d335b0 ops=0x3fcb21c8
[41530] [INF] [PDFM] engine loaded: code=1134176 data=28296 bss=25340
[41531] [ERR] [PDF] persistent PDF worker ready
Guru Meditation Error: Core  0 panic'ed (LoadProhibited).
PC 0x42309ee2  A2 0x0000cdcd  A7 0x0000cdcd  EXCVADDR 0x0000cdcd
A12 0xbcd4d500  LBEG 0x400570e8 LEND 0x400570f3 LCOUNT 0xffffffff
Backtrace: 0x42309edf 0x42306fc1 0x4224d769 0x421d98bb 0x4038657d
ELF file SHA256: 5dc4b1652
```

Four things this settles, and one it exposes:

- **The load sequence succeeded.**  Every `PDFM` line including `engine loaded`
  printed, and the mapping self-checks agreed (`paddr == aliasPaddr`,
  `target=2` = PSRAM).  So this is not a map/refuse failure and not a loader
  ordering bug -- the fault is *after* a healthy load, in the first document
  open on the worker.
- **The PC is in the app image, not the module.**  Module code lives at
  `0x42d30000+`, so `0x42309ee2` is app text, and the two frames below it are
  too.  `LBEG/LEND` sit inside the ROM (`0x400570e8..0x400570f3`) with
  `LCOUNT=-1`, i.e. a ROM memory/string routine's loop registers were still live
  when the fault was taken.  What that pairing means exactly -- app caller, ROM
  callee, fault reported in app text -- cannot be settled from the register
  block alone.  It needs the frames resolved, which needs the ELF.
- **`0xcdcd` is a value used as an address.**  `A2 = A7 = 0x0000cdcd` and
  `EXCVADDR = 0x0000cdcd`: a 16-bit-ish quantity was zero-extended and
  dereferenced.  It is not a heap pointer (poison here is `0xbaad5678`,
  section 4.1), so the honest reading is "a field that should hold a pointer or
  an offset held 0xcdcd", which is a format/bounds problem, not an
  initialization problem.
- **`PSRAM free=0` starts 106 lines later** (`[702] [INF] [DISP] PSRAM
  free=0 ...` on the next boot), with no other event in between.  That is the
  Link B observation in its purest form: one `LoadProhibited` and the PSRAM heap
  is gone across reboots.
- ★ **What it exposes: the ELF is gone.**  `ELF file SHA256: 5dc4b1652` names the
  exact image that crashed, and no ELF on this machine hashes to it (the
  archived XIP pair hashes to `3ec695c3c`).  So the backtrace cannot be
  resolved, and every address-level claim above stops at "app text".
  **Rule going forward: archive `firmware.elf` with every flashed XIP image**,
  next to the `.bin`, and record its SHA-256 in the release line -- otherwise the
  next panic is just as unresolvable as this one.

### 3.2 What is proven and what is only observed

The chain has two links, and they are not equally solid.  Stating this matters
because the next person will otherwise re-litigate the PSRAM side.

* **Link A -- why the engine crashed: unknown** (see above).  Closing it needs
  a reproduction with evidence attached.  The coredump partition is the right
  instrument, but it keeps only the **most recent panic**: during this incident
  the reboot loop overwrote the original crash, and the dump read back after
  recovery was a boot-loop artifact (no usable magic at offset 0, an embedded
  ELF at offset 24, seven thread notes whose register sets are all zero except
  one `0x40384bc0`/`0x403818d2` pair).  Do not treat it as the XIP crash.
  To reproduce properly: flash the fixed pair, crash it **once**, and pull the
  dump before anything else panics.
* **Link B -- why `PSRAM free=0` survived *every* soft reboot until a PMU
  power-off: observed, mechanism not register-proven.**  Measured: the chip is
  healthy (initializes at 40 MHz forced, 80 MHz forced, 80 MHz AUTO and 120 MHz
  official after recovery), the bootloader is byte-identical to factory, and the
  *same* app binary that logged `PSRAM free=0` boot after boot came up with a
  working 8 MB heap as soon as the board was fully de-powered -- nothing in
  software re-added the heap while power stayed on.  The most plausible reading
  is that the in-package Octal PSRAM was left in a state an `ESP_IMAGE`/RTC soft
  reset does not clear (its mode/PLL config is not re-negotiated on that path),
  so re-initialization cannot succeed in-process and only POR fixes it.  **Do
  not present that as established.**  The actionable consequence is the same
  either way: the loader must *detect* "no PSRAM heap", say so, and require a
  power cycle instead of running on into the framebuffer assert loop.

The software changes do **not** claim to make Link B recoverable and do **not**
fix Link A.  They provide (a) an enforced memory-layout invariant, (b) a
read-back that discriminates the remaining Link A candidates, and (c) a loud,
bounded stop instead of a reboot loop.

## 4. Fix (landed 2026-11-19, built and verified offline)

1. **Assert the memory-layout invariant at build time.**  `verify_bss_coverage()`
   in `scripts/build_pdf_module.py` now fails the module build if any `b`/`d`
   object falls outside the span the loader zeroes/copies, or if a separate
   `.ext_ram*` output section appears at all:
   ```
   'b' object outside its window [0x3d820000, 0x3d820c80): 0x3d826298+0x4 _cmsContextPoolHead
   ```
   The link script's `*(.ext_ram.bss .ext_ram.bss.*)` collection was **already
   present** and is a no-op today (see section 3), but it is kept because the
   assertion depends on that shape of `.bss` and the check documents why.  The
   assertion is what makes "the loader zeroes the right range" a build fact
   instead of a belief.

2. **`readpico_pdf` could not be built from a clean object cache -- fixed, and
   it invalidates a lot of confidence.**  `src/pdf/PdfDocument.cpp` was guarded
   by `#if defined(CROSSPOINT_MUPDF)` only, while every sibling XIP file uses
   the pair (`PdfModuleEntry.cpp`:
   `#if defined(CROSSPOINT_MUPDF) && !defined(CROSSPOINT_PDF_XIP)`).  Under
   `[env:readpico_pdf]` that means the in-app engine compiles and then demands
   `libmupdf`, which this env deliberately does not link -- so a from-scratch
   build died with hundreds of `undefined reference to 'fz_*'`, and any image
   that did get produced was reusing a stale `PdfDocument.cpp.o`.  The file now
   carries the same `&& !defined(CROSSPOINT_PDF_XIP)` term as its siblings.
   Two wrong turns this caused, both reverted: the first "fix" in this session
   put an `ext_ram_bss` memset into `PdfDocument.cpp`, which was not the empty
   TU my notes claimed (it was being compiled, wrongly); and adding
   `-Ipatches/mupdf/include` to make that compilation succeed treated the
   symptom -- nothing in the XIP app build needs the MuPDF headers.
   ★ **Consequence for every "it built, therefore it landed" claim in this
   note:** a `[SUCCESS]` off a stale cache proves the cache, not the source.
   Use a fresh `PLATFORMIO_BUILD_CACHE_DIR` (AGENTS section 38.4) and confirm
   with `strings`/`nm` on the object or image before believing anything here.

3. **PSRAM init fallback before touching the MMU.**  `pdf_module::ensureLoaded()`
   now starts with: if `heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0`, retry
   `psramInit()` (the Arduino wrapper that does `esp_psram_init()` +
   `esp_psram_extram_add_to_heap_allocator()`); if PSRAM is still empty, refuse
   the engine with the new `Refusal::NoPsram` and log
   `no PSRAM heap; refusing the XIP engine (power-cycle required)` instead of
   letting the framebuffer/render task assert into a reboot loop.  The reader
   maps `Refusal::NoPsram` to `STR_MEMORY_ERROR` (no new i18n key -- the pack
   key count is load-bearing, section 29.1) because telling such a user to
   install an engine they already have is a dead end; the remedy itself
   (power cycle) is the log line.  Verified on the fresh-cache build:
   `no PSRAM heap; refusing the XIP engine` is present in `firmware.bin`.
   Note the private-IDF route was tried first and rejected: including
   `esp_private/esp_psram_extram.h` fails to build against the Arduino core
   (`conflicting declaration of 'esp_psram_extram_test()' with 'C' linkage`).
   ★ **Honest scope: the useful half is the refusal, not the retry.**  The
   broken state in this incident survived every soft reboot (§3.2 Link B), so an
   in-process `psramInit()` retry probably also fails there; we never got to
   test it, because reproducing Link B requires the unfixed module.  The retry
   costs nothing and covers the benign case (a first-boot race, a transient
   `esp_psram_init` failure).  The guarantee this code actually provides is
   "**never** proceed to the MMU/map/entry path with no PSRAM heap" -- that is
   what turns the reboot loop into one readable error line.
4. **Validate the loader window.**  `validHeader()` keeps checking that
   `dataOffset/dataSize/rodata/bss` lie inside the `pdfmod` partition, and the
   loader logs the resolved ranges; a rejected module falls back to "no engine".
5. **Read back what was written before entering the engine.**  After the
   `.data` copy and the `.bss` memset, `ensureLoaded()` now walks the zeroed
   span **through the heap pointer** (not through the alias -- re-reading the
   alias would prove nothing) and refuses with `Refusal::MapFailed` if any word
   is non-zero, logging `module .bss read-back is not zero at +<off>; alias and
   heap differ`.  This is the instrument that turns the surviving Link A
   candidate (section 3: the copy never landed) into a yes/no on the next
   occurrence -- with the caveat from section 3.1 that for *this* crash the
   copy demonstrably did land, so the read-back is a permanent invariant guard,
   not the explanation of the 2026-11-19 fault.
6. **Pairing and flash checks stay.**  The `appTag` must remain the firmware
   ELF SHA-256 (`839f3058…` for the current pair, and `build_pdf_module.py`
   reported exactly that against the freshly linked ELF);
   `flash_pdf_xip.py` must keep its host-side pairing check and its
   first-boot gate (`no PSRAM heap`, panic dump, reset loop, missing `[BOOT]`).

### 4.1 Offline verification (2026-11-19, fresh `PLATFORMIO_BUILD_CACHE_DIR`)

| Check | Result |
|---|---|
| `pio run -e readpico_pdf` from a clean cache | `[SUCCESS] Took 407.81 s`; RAM 89,548 B (27.3%), Flash 5,509,975 / 5,767,168 (95.5%), `firmware.bin` 5,515,232 B, app0 margin ≈251,936 B; `esp_app_desc project_name=plugin-runtime` (this worktree's own build-machine name in the shared `libesp_app_format.a`, AGENTS section 23.6g -- not the `arduino-lib-builder` vendor failure) |
| In-app engine really excluded | `.pio/build/readpico_pdf/src/pdf/PdfDocument.cpp.o` = 1,332 B, **0 symbols** from `nm` |
| PSRAM refusal in image | `strings firmware.bin` finds `no PSRAM heap; refusing the XIP engine` (1) |
| Read-back refusal in image | `strings firmware.bin` finds `module .bss read-back is not zero` (1) |
| `NoPsram` message | `Memory error` present (reuses `STR_MEMORY_ERROR`; pack key count unchanged) |
| Module rebuild | `code=1132016 rodata=580224 data=28296 bss=25348 entry=+0x2dac total=1863304`, fits the 1.875 MiB `pdfmod` slot |
| Layout assertion is real | mutating `bssSize` to 3,200 / 20,000 B makes `verify_bss_coverage()` fail and names `_cmsContextPoolHead` at `0x3d826298` |
| `.ext_ram.bss` collection is a no-op | deleting `*(.ext_ram.bss .ext_ram.bss.*)` re-links a **byte-identical** `pdfmod.bin` -- the basis of hypothesis 1 in section 3 |
| Poison fill in this build | `objdump -d` of `poison_allocated_region` (`0x4038b344`) loads `0xbaad5678` payload / `0xabba1234` header -- **not** `0xcd`, killing hypothesis 2 |
| `appTag` pairing | `pdfmod.bin` appTag (header offset **60**) == `firmware.bin[0xb0:0xd0]` = `839f3058352e7229…23c4cb5c` |

★ `PdfModuleHeader.appTag` is at offset **60**, not 64 -- the header is
`u32 magic; u16 version; u16 abi;` then 14 more `u32` fields
(`kPdfModuleHeaderBytes = 0x10000`, so `codeOffset` is a full 64 KiB).  An
offset guess produces a convincing "MISMATCH"; read the struct instead.

**Not yet done (needs the device):** flash the fixed app + fixed module and open
a pure-image PDF, expecting no `EXCVADDR 0xcdcd`; then deliberately fault the
engine and confirm `PSRAM free` recovers through the `psramInit()` retry instead
of boot-looping.  The device is currently still on the known-good
`0.7.0-rp` app + the old (unfixed, still-paired-by-appTag-`3ec6…`) module.

## 5. Recovery procedure actually used (2026-11-19)

### 5.1 Enter download mode (no BOOT button)

The PMU owns BOOT.  With the host running, press the power key twice quickly
(≤1.2 s apart) and then hold it ≥3 s.  The LED shows the download pattern
(0.15 s × 7, off-white-off-red-…).  `esptool --before no-reset chip-id` then
connects without a reset.  A normal `esptool --before default-reset` is not
reliable while the app is in the assert/reboot loop.

### 5.2 Back up before touching anything

The current XIP state was backed up by components because a single 16 MB
`read-flash` stops responding in the app0 region on this unit:

- 1 MB chunk 0 (`0x0–0x100000`): bootloader, partition table, NVS, otadata,
  app0 header — `/tmp/chunk0.bin`.
- Factory bootloader: `/tmp/dev_bl.bin` (32 KB; equals the factory backup).
- XIP partition table: `/tmp/dev_part.bin` (`0x8000`, 4 KB).
- NVS/otadata: `chunk0[0x9000:0xE000]`, `chunk0[0xE000:0x10000]`.
- Original app: `artifacts/pdf-xip/firmware.bin` (0.7.0-rp, app0/app1).
- Paired pdfmod: `artifacts/pdf-xip/pdfmod.bin` (appTag `3ec695c3…`, matches the
  original app `0xb0`).
- Data partition (`spiffs` at `0xCD0000`) is untouched by the official flash and
  by the restore; no data loss.

### 5.3 Prove the PSRAM chip is alive (minimal IDF test app)

`.scratch/psramtest` (or a rebuilt equivalent) is a 165 KB IDF app that calls
`esp_psram_init()` and `esp_psram_extram_add_to_heap_allocator()` and prints:

```
PSRAMTEST: tick: initialized=1 size=8388608 spiram_free=8386308 spiram_largest=8257536
```

Variants used: 40 MHz forced ESP-PSRAM64, 80 MHz forced, 80 MHz AUTO.  All pass.
This proves the chip and the 80 MHz timing; the app's own `psramInit` will work
once the PSRAM state is re-initialized.

### 5.4 Restore the XIP CPYOS app

```
esptool --before no-reset --after no-reset write-flash \
  0x0      /tmp/dev_bl.bin \
  0x8000   /tmp/dev_part.bin \
  0x9000   /tmp/nvs.bin \
  0xE000   /tmp/ota.bin \
  0x10000  artifacts/pdf-xip/firmware.bin \
  0x590000 artifacts/pdf-xip/firmware.bin \
  0xAF0000 artifacts/pdf-xip/pdfmod.bin
```

Then a full PMU power cycle (10 s off, 3–5 s on).  The original `0.7.0-rp` app
boots, shows its UI and `PSRAM free≈1.6 MB`; the paired pdfmod is
`3ec695c3…` -> `match: True`.

### 5.5 Official 120 MHz check (controlled experiment)

The official `read_pico_firmware` was built with IDF 6.1 / GCC 15.2 and flashed
(bootloader + its partition table + `Read_Pico.bin` only; storage/pdfmod/data
untouched).  It booted to the **official UI**, proving PSRAM 120 MHz works on
this unit.  Note: the repository `sdkconfig.defaults` generates
`SPIRAM_SPEED=120` but `FLASHFREQ="80m"`/DIO; the official comment says 120M
PSRAM needs flash 120M + HPM.  The product's full 120M flash timing was not
reproduced here, but the PSRAM 120 MHz itself is functional.

### 5.6 The coredump is the right instrument for Link A -- and it is perishable

`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` with `DATA_FORMAT_ELF`, partition
`coredump` at `0xFF0000` (64 KiB).  Two facts measured on this device:

- **It holds only the most recent panic.**  The reboot loop after this incident
  overwrote the XIP crash.  So on the first crash of a new build, pull the dump
  *before* letting the device panic again.
- **`exp/coredump.py` mis-reads the current format.**  It searches for the magic
  at file offset 0 and reports "no core dump magic ... the partition is empty".
  The partition is not empty: the dump begins with a short IDF header and the
  ELF core starts at **offset 24** (`\x7fELF` at 24, `e_phoff` 52, `e_phnum` 32),
  with `PT_NOTE` at file offset `0x24c`, length 4256.  Carving from 24 and
  walking the notes gives 7 `CORE` descriptors of 588 B each.  On the
  post-recovery read the register sets were all zero apart from one word per
  thread (`0x40384bc0`, `0x403818d2`) -- which is what a boot-loop dump looks
  like, and the reason it proved nothing about the XIP crash.
  ★ Do not "fix" the tool by pointing it at that dump and trusting the output.
  It needs the offset-24 handling, and it needs a dump captured right after the
  crash it is meant to explain.

The decisive Link A measurement, when it can be taken: `PC`, `EXCVADDR` and the
faulting thread's stack from that dump, against the ELF that was actually
running (the pairing check in `flash_pdf_xip.py` already ties `appTag` to that
ELF, so the same digest identifies the right `firmware.elf`).

### 5.7 The 40 MHz rescue image, as a reproducible target (verified 2026-11-20)

It used to be a hand-copied `platformio.ini` in `/tmp`, which is not a recovery
plan (a reboot deletes it, and nothing else can rebuild it).  It is now a
tracked env:

```
pio run -e readpico_pdf_rescue40          # platformio.ini [env:readpico_pdf_rescue40]
```

★ **One knob: the speed.**  Production (`sdkconfig.defaults`, lines 2099-2113)
is octal + auto-detect at 80 MHz:

```
# CONFIG_SPIRAM_MODE_QUAD is not set
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
# CONFIG_SPIRAM_TYPE_ESPPSRAM64 is not set
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_SPEED_40M=y          (both listed; CONFIG_SPIRAM_SPEED=80 wins)
```

The first draft of the rescue env, copied from the throwaway `/tmp` config, also
pinned `# CONFIG_SPIRAM_TYPE_AUTO is not set` + `CONFIG_SPIRAM_TYPE_ESPPSRAM64=y`.
That is not a speed change: on ESP32-S3 `TYPE_ESPPSRAM64` forces the **QUAD**
mode (octal is the separate `ESP32S3PSRAM64`), so that draft would have flashed
a rescue image wired differently from the one it is meant to rescue -- and note
the same mistake is baked into the `defaults_p40t64` label used during the
investigation (see the caution in section 2).  The env now overrides only the
speed, and the generated config proves it:

```
$ grep SPIRAM sdkconfig.readpico_pdf_rescue40
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
# CONFIG_SPIRAM_SPEED_80M is not set
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_SPIRAM_SPEED=40
```

Verified end to end on the device on 2026-11-20 -- flashed to both slots, then
read back:

```
[20122] [INF] [MEM] PSRAM: Free: 1744420 bytes, Total: 8373840 bytes, MaxAlloc: 1703924 bytes
```

so the rescue image boots with the full 8 MB PSRAM heap.

★ **The rescue image is deliberately not kept on disk** (user decision,
2026-11-20): a 5.5 MB bin nobody re-verifies is worse than a command that
rebuilds it in ~11 minutes from a tracked env.  What was flashed that day, for
the record:

```
firmware.bin 5,515,344 B   sha256 7098e2babe947e4623bea1dbe91a494038a7b1e62b955f2e4b9e89847e355eda
firmware.elf                 sha256 81b49ee4d0afc05ee2e239ba13408854dc883b06a6a2a2b5087f506aaa39a183
```

Rescue command (XIP layout: app0 `0x10000`, app1 `0x590000`), rebuilt when
needed -- the hashes above record exactly what was flashed and verified:

```bash
pio run -e readpico_pdf_rescue40                      # [env:readpico_pdf_rescue40]
PORT=$(ls /dev/cu.usbmodem* | head -1)
~/.platformio/penv/bin/python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  write_flash 0x10000 .pio/build/readpico_pdf_rescue40/firmware.bin \
              0x590000 .pio/build/readpico_pdf_rescue40/firmware.bin
```

Write **both** slots: otadata on this unit selects app1 after any device OTA, so
an app0-only write is the silent no-op AGENTS section 5 warns about.  Do not
touch `pdfmod`, the partition table, otadata or NVS to rescue PSRAM -- nothing
about this recovery needs them.

### 5.8 Coming back off the XIP layout (verified 2026-11-20)

`partitions_readpico.csv` and `partitions_readpico_xip.csv` share `spiffs`
(`0xCD0000`, `0x320000`), `nvs`, `otadata` and `coredump`; they differ only in
the two app slots and the XIP table's `pdfmod` (`0xAF0000`).  So leaving XIP
means: write the normal partition table, write the normal app into both slots,
and leave the data blobs where they are -- except the i18n/web packs must be
regenerated for the normal build, because the two envs do not share a key set.

```bash
D=repos/crossmux/.pio/build/readpico
python -m esptool --chip esp32s3 --port $PORT --baud 460800 write_flash \
  0x8000 $D/partitions.bin 0x10000 $D/firmware.bin 0x670000 $D/firmware.bin
bash exp/flash_device.sh        # app0 + the six data blobs, manifest-generated offsets
```

Device evidence after that, 2026-11-20:

```
[648] [INF] [SD] SDMMC mounted at 40000 kHz
[793] [INF] [I18N] pack: 34 languages, 1859 keys, 354377 bytes at partition +0x100000
[796] [INF] [I18N] language pack ready; current=简体中文
[823] [DBG] [MAIN] Starting CrossPoint version 0.7.0-cpyos
[6329] [INF] [BOOT] boot 6329ms
```

Before trusting "the bootloader is original", read it: `0x00000000..0x00007fff`
hashes to `9812237f232f5403067c0c20049e7929ecb40d53245c536f6c7a2fff7361e0c8`,
which is byte-identical to the first 32 KiB of
`device/readpico_full_backup_20261013_002207.bin`.  The custom bootloaders built
during the PSRAM investigation (120/80/40/20 MHz, 64 B cache line variants)
**were flashed and were never needed** -- `nm` on them shows zero PSRAM symbols,
because PSRAM initialization lives in the app and the Arduino core, not in the
bootloader.  They were overwritten again with the factory bytes during recovery,
which the hash above proves.  Do not go down that path: a bootloader is the one
region this board has no button-level recovery for.

★ Two operational traps hit while doing exactly this:
`esptool --no-stub chip_id` leaves the board in `DOWNLOAD(USB/UART0)` and it
will not boot until something resets it -- read the banner, do not assume a
reset happened; and the serial port cannot be shared, so a running
`exp/mon_device.py` makes every later `qcmd`/esptool call fail or hang.  Stop
the monitor before touching the port.


## 6. 80 MHz (CrossMux/CPYOS) vs 120 MHz (official)

| | CrossMux/CPYOS | Official MindReset |
|---|---|---|
| PSRAM | 80 MHz | **120 MHz** |
| Flash | 80 MHz DIO | 120 MHz + HPM (product); repo build 80m/DIO |
| Cache line | 32 B | 64 B |
| Intent | stability/compatibility with the base | display/refresh bandwidth |
| Measured | original app UI, PSRAM 80M | official UI, PSRAM 120M |

- **Performance:** 120M gives 1.5× PSRAM bandwidth.  The official comment says
  80M effective ~32 MB/s is the refresh ceiling; 120M raises it.  This affects
  framebuffer line streaming, partial refresh, large fonts/PDFs.
- **Stability:** 120M is not "safer".  The official comment warns that the
  board's Zbit flash/PSRAM are not in the IDF temperature-tuning validation set,
  `SPIRAM_TIMING_TUNING` must stay off, and ~20 °C drift can cause random PSRAM
  errors.  80M has more margin.
- **The incident was not the frequency.**  The crossmux→cpyos migration did not
  change the PSRAM frequency.  The trigger was a crash **inside** the XIP
  engine whose cause is still open (section 3); the crash then left the PSRAM
  initialization state unusable until a full power-off.

## 7. Prevention checklist

- [ ] `build_pdf_module.py` keeps `verify_bss_coverage()`: every `b`/`d` object
      inside its loader span, no separate `.ext_ram*` output section.  Re-run
      the mutation (shrink `bssSize`) whenever the check is touched -- an
      assertion that has never failed is not evidence.
- [ ] `src/pdf/PdfDocument.cpp` keeps `&& !defined(CROSSPOINT_PDF_XIP)` on its
      outer guard, like `PdfModuleEntry.cpp`.  Without it a clean `readpico_pdf`
      build cannot link, and any image that appears to build is a stale object.
- [ ] Prove a patch landed with `strings`/`nm` on the object or image, and build
      at least once with a fresh `PLATFORMIO_BUILD_CACHE_DIR` -- `[SUCCESS]` off
      a reused cache is not evidence about the current source.
- [ ] `PdfModuleRuntime` validates the data/bss window inside the `pdfmod`
      partition and logs the resolved ranges.
- [ ] PSRAM heap check before the XIP loader (`Refusal::NoPsram` + a readable
      message) instead of the framebuffer assert loop.  ★ This bounds the
      failure, it does not undo it: with Link B unresolved, an engine crash
      still costs a PMU power cycle.
- [ ] `appTag` = firmware ELF SHA-256; `flash_pdf_xip.py` pairing + first-boot
      gate remain.
- [ ] Keep the factory 1 MB chunk / component backups and the known-good app +
      pdfmod; after any crash-loop, PMU power cycle before judging PSRAM.
- [ ] Running XIP on the main device is a **user decision, not a default**.
      The blanket ban was the wrong answer (the engine must be testable), but
      Link A is still open, so an engine crash costs a power cycle and the
      session's PSRAM features.  Keep the known-good app + `pdfmod` pair and the
      section 5 restore path within reach whenever the engine is being pushed,
      and pull the coredump (before anything else panics) on the first crash of
      a new build.

## 8. Reproduction attempt on the fixed pair (2026-11-20): it did not crash

Everything below is from the correctly built pair flashed after the fixes
(`panic tag 98fd62825`, archived with its ELF in
`artifacts/pdf-xip/flashes/20261119-161828Z/`).

- `CMD:PDFBIG 19` ran the same workload that produced the original panic.  It
  produced **no panic at all**: the engine loaded, the document opened
  (`pages=112`), and page 19 failed through the checked allocation paths with
  explicit reasons and soft fallbacks.
- So the original `LoadProhibited / EXCVADDR 0xcdcd` is **not reproduced**, and
  the explanation in section 3 is *not confirmed by reproduction*.  What is
  confirmed is narrower and should be stated that way: the build defect that
  could put two layout-incompatible copies of the PDF document code into one
  image is gone, the image is now built and flashed from a clean cache with its
  ELF kept, and the same workload degrades in checked code instead of faulting.
- ★ Do not write this up as "fixed and verified".  It is "the mechanism was
  removed; the fault itself was never seen again on a correct build".

### 8.1 A live, unrelated defect that the same repro surfaced

Page 19 of this PDF cannot be rendered today, and the cause is PSRAM capacity,
not correctness.  Resolved with `scripts/pdf_crash_capture.py --replay` against
`build/pdfmod.elf`:

```
psramRealloc(463911) failed                free=203520 largest=196596
  do_scavenging_realloc / fz_realloc
  fz_resize_buffer / fz_grow_buffer / fz_read_best
  pdf_load_image_stream -> pdf_load_stream -> pdf_load_embedded_font
  pdf_load_font_descriptor -> load_cid_font -> pdf_load_type0_font
```

and separately `PSRAM alloc 378319 failed  free=323552 largest=319476` for the
full-page pixmap.  The session also hit `PSRAM Min Free: 868 bytes`.

Two concrete follow-ups, neither of which is this incident:

1. **`fz_read_best` grows a single buffer for an embedded CID font of ~450 KB**
   while the page pixmap (~380 KB) is also wanted.  The reserve
   (`gReserve`, ~4.9 MB) did not serve the realloc, because the failing pointer
   was not inside it; a font stream that large should be streamed or fetched
   into the reserve explicitly.
2. **The `tile render failed` log had a format/argument mismatch** (5
   conversions, 4 arguments), so every field was shifted and `largest=` printed
   an uninitialized vararg (`largest=0` beside `free=368628`).  Fixed in
   `src/pdf/PdfDocument.cpp`; that file is compiled into the *engine*, so a log
   change there means rebuilding **and re-flashing `pdfmod`**, not just the app.
