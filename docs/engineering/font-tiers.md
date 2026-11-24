# Font tiers: UI subset in app0, reading core in the data partition

Status: implemented on the public tree (2026-11-14). Applies to Read Pico and
the Metalio E-INK4 Plus port, which share `src/SystemFontPack.cpp`,
`src/SystemCoreFont.cpp`, `src/UiScriptFont.cpp` and the font carriers.

## Tiers

| Tier | Asset | Where | Size | Used by |
|---|---|---|---:|---|
| 0 | `CPYOSHanSansSC-UI.ttf` / `cpyos_han_sans.def` | app0, inflated to PSRAM | 296,572 B TTF / 193,766 B carrier | every system UI role, 8–18pt |
| 0b | `CPYOSWorldScripts-UI.ttf` / `cpyos_ui_ext.def` | app0, inflated to PSRAM | 49,152 B TTF / 25,333 B carrier | UI supplements for Vietnamese, Czech/Polish/Turkish, Hebrew, Arabic, … |
| 1 | `CPYOSHanSansSC-Regular-8105.ttf` / `cjk_core.dat` | data/spiffs at `+0x180000`; released to `/fonts/CPYOSCJKCore/` | 2,555,704 B TTF / 1,640,306 B carrier | reader body text and book titles |
| 2 | `CPYOSCJK.ttf` (font-extra) | SD | 10.4 MB | optional Ext-A/rare ideographs |
| 3 | `CPYOSWorldScripts.ttf` (font-extra) | SD | 6.76 MB | optional full non-CJK book text |

The CJK UI subset covers the 35 translation catalogs, hard-coded UI string
literals in `src/`, `lib/` **and the port's own activities**
(`ports/metalio_plus/main`, `components/cpyos_app`, `components/cpyos_ui`), the
lunar-calendar list and every non-Han glyph the source family already carried
(kana, Cyrillic, Greek, punctuation, arrows, maths). That is 1,510 UI
codepoints (1,511 glyph slots in the CJK face); it is sized to the UI, not to
book text. `chars_ui.txt` is the coverage contract and
`scripts/tests/test_system_font_pack.py` fails when a new UI string introduces
a codepoint no face can draw.

The port scan was added on 2026-11-16: the power-off confirmation
(“确定要关机吗？”) is hard-coded in `ports/metalio_plus/components/cpyos_app/
power_off_activity.cpp`, outside the old `src/` + `lib/` corpus, so 吗 existed
only in the reading core and boxed whenever the core was unavailable (no card,
or a card that failed to mount). The same scan picked up 板/钮 (OTA page) and
类/诊 (transfer page). **Any new hard-coded Chinese UI literal in a port must
go through this scan** — or better, be moved into the translation catalogs.

## Why the UI face lives in PSRAM

The previous implementation inflated the carrier, wrote
`/.crosspoint/system_font_<crc>.dat` to SD, and streamed every glyph fault from
the card. A slow card therefore slowed the whole system UI. The embedded
carrier is now inflated once into PSRAM (`Backend::Memory`) and never touches
SD: first paint, list scrolling and the status bar cannot wait on the card.
The 190 KB allocation replaces a 2.5 MB SD file and removes the first-boot
multi-MB write.

## 8pt analysis (can the built-in bitmap still be reached?)

The 3,500-glyph `notosans_cjk_8` bitmap (`341,314 B` in the ELF) was the
Compact-density `SMALL` face and the last-resort CJK fallback. After this
change:

* `SystemFontPack::configure()` accepts 8pt and `typography::apply()` always
  requests it on embedded builds. `CJK_UI_8_FONT_ID` is therefore a **vector**
  face rendered from the same UI subset as the other roles.
* The UI subset covers every UI string (enforced by the generator + test), so a
  UI label cannot fall out of it.
* The remaining path to the bitmap would be "the vector pack failed to
  initialize". The carrier is in app0, CRC-checked after inflate, and needs
  ~190 KB of a ~6 MB PSRAM pool. If that fails, the firmware has no CJK UI at
  all and the bitmap would only soften the failure. This is exactly what the
  2026-11-14 CRC bug produced: the pack was rejected, the UI fell back to the
  built-in Latin faces, and every Chinese glyph became a box.
* A wrong CRC is loud in the log (`SFP font pack CRC mismatch`) but silent on
  the glass except for tofu. There is no 8pt CJK fallback any more, so the
  failure must be caught by the carrier test and the boot log, not by a
  degraded-but-readable UI.

Conclusion: on embedded builds the bitmap is unreachable in normal operation
and is not linked. `lib/EpdFont/builtinFonts/all.h` and both `main.cpp`-style
registrations keep it for **spiffs builds (C3/X3/X4)**, where the pack can be
absent after an app-only OTA and a readable 8pt UI beats tofu. If a future
embedded change reintroduces a path that can run without the vector pack, the
bitmap must come back with it; a tiny UI-only 8pt insurance bitmap (~125 KB)
is the documented middle ground.

## Reading core: partition change and release

The data partition grew from 1.5 MiB to 3 MiB by shrinking both OTA slots from
`0x730000` to `0x660000`:

| Partition | Old | New |
|---|---|---|
| app0 / app1 | `0x730000` each | `0x660000` each (6.375 MiB) |
| spiffs | `0xe70000`, `0x180000` | `0xcd0000`, `0x320000` |
| core carrier | — | spiffs `+0x180000`, 1,640,306 B |

The UI font shrank app0 by ~1.4 MB and the 8pt bitmap by 341 KB, so the image
still has comfortable headroom in the smaller slot. `partitions.csv`,
`partitions_plus_ota.csv` and the plus single-slot table are updated together;
`scripts/generate_data_manifest.py` and `ports/metalio_plus/scripts/flash_metalio_plus.py`
read the core offset from `src/SystemCoreFont.h`, not from a second copy.

`SystemCoreFont::releaseToSd()` runs at the start of `SdCardFontSystem::begin()`:

1. validate the CPFZ header at `+0x180000`;
2. skip when `/fonts/CPYOSCJKCore/CPYOSCJKCore.ttf` already matches the size and
   the `.core-crc` sidecar;
3. otherwise mmap the partition, inflate to PSRAM, verify CRC-32, and write the
   TTF through `.tmp` + `replaceFile`.

`SdCardFontSystem::loadSupplementFonts()` treats the core as a supplement with
**reader budgets** (4 MB resident cap, 1 MB stream prefix, 1 MB glyph cache /
4096 glyphs) and, when no explicit SD reader family is selected, binds it as the
reader face (`setPreferredFont`). The UI keeps using the embedded subset.

If the carrier is absent (app-only OTA that did not update the data partition)
or there is no SD card, the release fails, the reader stays on the UI subset,
and the existing missing-glyph detection offers the downloadable
`cjk-core` pack. The `cjk-full` and `world-scripts` packs remain available for
rare ideographs and non-CJK book text.

**The partition change requires one full install.** Do not publish an OTA
upgrade manifest that writes data blobs at the new spiffs offsets to a device
still on the old layout: the new spiffs base (`0xCD0000`) overlaps the old app1
slot. This release is a web-installer/full-install upgrade; after that, OTA
app-only updates are safe because the app slots keep their offsets.

### CRC convention (fixed 2026-11-14)

`SystemFontPack.cpp`'s `crc32Update()` is the **incremental** form: it does not
complement at the start or the end. The spiffs path has always used

```cpp
uint32_t crc = crc32Update(UINT32_MAX, data, len) ^ UINT32_MAX;
```

The first embedded memory-path implementation called `crc32Update(0, ...)`,
which produces a different value. Every embedded UI pack was therefore rejected
at boot, `system_font_pack::begin()` returned false, `typography::apply()` never
ran, and the shell kept its built-in Latin UI faces: ASCII (`【0m 0d】`) rendered
and every Chinese glyph was a tofu box. That was the 2026-11-14 e4plus
regression.

The embedded check now uses the same `UINT32_MAX ... ^ UINT32_MAX` convention as
the spiffs path. `UiScriptFont` and `SystemCoreFont` carry self-contained helpers
that complement at both ends, so they are called with a seed of `0`. **Any new
carrier check must pick one convention and be verified on the host; a carrier
that is valid under `fontTools`/`fc-scan` can still be rejected by a wrong CRC
call.**

### Core release failure modes (fixed 2026-11-14)

Two independent faults made the released core invisible even though the carrier
was flashed correctly:

1. **mmap offset.** `SystemCoreFont::releaseToSd()` mapped the partition from
   the CPFZ **header** and passed that pointer to `tinfl_decompress_mem_to_mem`.
   The deflate stream starts at `+32`, and the mapping must cover
   `kHeaderSize + compressedSize` bytes; mapping only `compressedSize` bytes
   from the header both starts at the wrong byte and truncates the last 32
   bytes. Inflate failed and returned **silently**.
2. **No Plus prompt consumer.** The Plus `shell_demo` initialized
   `FontCacheManager` but never consumed `missingCodepoints_`, so a glyph the
   UI subset could not draw (a book-title character, a font-extra pack name)
   stayed tofu with no download offer.

Before the fix the boot log showed `[CORE] carrier present` and then
`SDREG Discovery complete: 0 families`, with no `released` line. After the fix
the release propagates as a normal SD font family and the UI supplement covers
titles and manifest names.

The mmap path now maps `kHeaderSize + compressedSize` bytes and feeds
`mapped + kHeaderSize`; an inflate failure logs
`CORE core carrier inflate failed`. The Plus shell now runs the same
stable-page prompt as Read Pico (`FontExtrasActivity(Purpose::ReaderPrompt)`,
once per boot), so an unfixable codepoint offers `cjk-core` / `cjk-full`.

### One role table for bindings and supplements (2026-11-16)

The core and font-extra UI faces used to be attached only to the ids that walk
through `typography::apply()`'s preferred bindings (`SMALL`, `UI_10`, `UI_12`,
footer/estimate). Any page that asks a role id **directly** could still miss the
core:

- `NOTOSERIF_18` (book-detail hero title, library grid titles) resolves to
  `CJK_UI_16` only while the preferred binding is intact;
- `SdCardFontSystem::loadTtfFamily()` calls `renderer.clearPreferredFonts()`
  when an SD vector family loads, so after that every `NOTOSERIF_*`,
  `NOTOSANS_18` and `CONTROL_18` resolved to its own Latin-only face;
- `NOTOSERIF_TITLE` was never bound.

Result on the e4plus: system pages were fine, while the JJWXC book list and
detail boxed every Han title. `Typography` now exposes

```cpp
struct RoleFont { int fontId; uint8_t pointSize; bool bindPreferred; };
size_t roleFontsFor(uint8_t scaleIndex, RoleFont* out, size_t capacity);
int cjkFontIdForPointSize(uint8_t pointSize);
```

`apply()` uses it for the preferred bindings and `SdCardFontSystem::
loadSupplementFonts()` uses the same table to attach a **size-matched** core or
font-extra face to every role id (plus the `CJK_UI_*` id for that size). The
reach no longer depends on the preferred map or on the current scale, and each
role gets a face of its own point size instead of the first zero-missing
candidate. `NOTOSERIF_TITLE` is `bindPreferred = false`: it keeps its serif
Latin design and receives the CJK face only for glyphs the design lacks.

`unloadSupplementFonts()` no longer calls `clearSupplementFonts()`: it removes
its own ids through `removeFont()` (which prunes every chain), and clearing the
global map also dropped `UiScriptFont`'s small-language faces until the next
scale change re-ran `apply()`.

The core UI faces are built at exactly the role sizes of the current
typography scale (`pointsFor(SETTINGS.uiFontScale)`): 12/14/16 at the default
Large scale, 8/10/12/14 at Compact, 10/12/14/16 at Standard. Every role gets a
face of its own size, so a title cannot silently shrink to the first
zero-missing candidate. `SdCardFontSystem` remembers the scale that produced
the loaded set; changing **UI font scale** in Settings re-runs
`loadSupplementFonts()` so the new sizes exist immediately instead of boxing
until the next reader/app entry.

## Small languages

The CJK family never contained Vietnamese tone marks, Czech/Polish/Turkish
diacritics or Arabic/Hebrew. `ui_script_font` registers a 25 KB supplement whose
426 codepoints are exactly the non-CJK UI vocabulary. `resolveTextFontId` picks
it for a Czech or Vietnamese label and keeps Chinese labels on the CJK face.

Limits:

* Arabic/Persian render the correct glyphs but **not joined**: the renderer has
  no GSUB/shaping engine. Thai/Devanagari would need mark positioning and
  reordering; there are no translations for them yet.
* Hebrew relies on the existing bidi pass; letters are not shaped, which is
  correct for Hebrew.
* A mixed string such as "中文 Česky" may still choose one face for the whole
  run and draw tofu for the other script. Mixed-script UI labels are avoided;
  per-codepoint fallback is a larger layout change.
* Full non-CJK book text needs the downloadable `world-scripts` pack.

## Regeneration and tests

```bash
python3 scripts/build_ui_font_subset.py          # UI TTF/.def, ext TTF/.def, chars_ui.txt, build/cjk_core.dat
python3 scripts/build_ui_font_subset.py --check  # fail when an asset is stale
python3 scripts/generate_data_manifest.py         # adds the core row, verifies the partition layout
python3 -m unittest scripts.tests.test_system_font_pack
```

`build_ui_font_subset.py` fails if a UI codepoint has no face. The test suite
also checks that the supplementary face covers every non-CJK UI codepoint, that
all three carriers inflate to their committed TTFs, that the C3 pack still fits,
and that the committed files match a fresh generation.

## Known limitations

* Old `/.crosspoint/system_font_*.dat` files from pre-change firmware are no
  longer read; they can be deleted manually to reclaim ~2.5 MB on the card.
* The core is also present as a downloadable font-extra (`cjk-core`) for
  app-only upgrades. When both exist the registry de-duplicates by family name;
  either copy is the same 8,105-character TTF.
* `readpico_nightly` still embeds the 33-language pack; the font tiers are
  independent of that switch.
* The core release is a one-time 2.5 MB SD write after a full install. It is
  skipped on later boots when the CRC sidecar matches.

## Validation (2026-11-14)

* **Read Pico**: `pio run -e readpico` SUCCESS. Image 5,368,208 B in a 6,684,672 B
  app0 slot (**1,316,464 B free**), RAM 91,352 B. The ELF contains the real
  `system_core_font::releaseToSd` (0x252 B), `ui_script_font::begin`/`configure`
  and the 25 KB UI-script carrier; `notosans_cjk_8Bitmaps` is absent.
* **Metalio E-INK4 Plus**: `CPYOS_UI_PROBE=1 idf.py --preview reconfigure build`
  SUCCESS (image 3,525,088 B). The partition table is the new
  `app0/app1 0x660000 + spiffs 0x320000`; `releaseToSd` is 0x2ce B and the
  UI-script carrier is linked.
* **Plus full shell** (the real ActivityManager UI, without the private JJWXC
  component): SUCCESS, image **5,430,256 B**, which fits the 6,375,040 B dual-OTA
  slot. It contains `system_core_font::releaseToSd` (0x2e2 B),
  `ui_script_font::begin` (0x1ae B) and no `notosans_cjk_8Bitmaps`. The shell
  therefore no longer needs the single-slot table just to fit.
* **Host tests**: `test_system_font_pack` 9/9 (carriers inflate to their TTFs,
  every UI codepoint is covered, non-CJK UI is in the supplement, the committed
  assets match a fresh generation, the core carrier round-trips);
  `test_font_extras_manifest` 6/6; the release/nightly/web-flasher suites pass;
  the Plus flash planner 22/22 with the new offsets and the core segment.
* **Device**: the Plus was fully installed (new table, both app slots, six data
  blobs including the 1,638,942-byte core). The single `write-flash` drops the
  USB-JTAG link after ~0.5 MB on this unit, so the install used
  `ports/metalio_plus/scripts/flash_chunked.py` (64 KB chunks, 0 retries).
  Read-back verified the partition table, the app header/sample and the core
  header + head/tail samples.
* **Device font bring-up (after the CRC fix)**: the full shell boots from app1
  at `0x670000` and the USB log shows

  ```text
  [INF] [SFP] ... EMBEDDED-V1 ready: 293152 bytes; faces load on demand (4-bit coverage)
  [INF] [USF] UI script supplement ready: 49152 bytes
  [INF] [CORE] carrier present: 1638910 -> 2553580 bytes
  [INF] [PERF] InxRecent first frame: render=1080ms
  ```

  Chinese renders correctly on the panel (user-confirmed). The
  `[ERR] [TTF] Auto hinting unavailable (FREEINK_FONT_ENABLE_AUTOHINT not
  compiled)` lines are expected and non-fatal: the port intentionally builds
  FreeInkFont without autohint and uses the non-hinted path. The shell also
  mirrors its log to `/.crosspoint/logs/boot.log` on SD.
* **Core release + Plus prompt (2026-11-14, flashed, awaiting screen/SD
  confirmation)**: the shell image is **5,471,728 B**. It contains
  `releaseToSd` (0x2f8 B), the `core carrier inflate failed` log, and the
  stable-page `FontExtrasActivity(ReaderPrompt)` consumer. On a boot whose
  `/fonts` root does not exist yet, the expected lines are
  `[CORE] released the 2553580-byte core to /fonts/CPYOSCJKCore/CPYOSCJKCore.ttf`,
  `[SDREG] Discovery complete: 1 families` and
  `[SDFS] font supplements: 1 families at <pt>`. The core is the **full
  2,553,580-byte TTF (8,105 characters)**; the 1.5 MB figure was its compressed
  carrier, not a reduced charset.


## Validation (2026-11-16): unified role attachment and port UI literals

* **Role attachment** (`typography::roleFontsFor()`): the core/font-extra UI
  faces are attached to every role id at its own size, not only to
  `SMALL`/`UI_10`/`UI_12`. Verified on the Plus: the JJWXC online shelf (30
  book titles + authors), the book-detail hero title (`NOTOSERIF_18`), the font
  extras pack list and the library grid titles (`亏成首富从…`) all render with
  zero tofu. `loadSupplementFonts()` is re-run when **UI font scale** changes so
  the newly sized role faces exist immediately.
* **Port UI literals**: `build_ui_font_subset.py` now also scans
  `ports/metalio_plus/{main,components/cpyos_app,components/cpyos_ui}`. The UI
  subset grew to 293,152 B / 191,521 B carrier (1,424 UI codepoints, 1,487 glyph
  slots) and covers `吗 板 钮 类 诊`, which previously existed only in the
  reading core. The power-off dialog (`电源键已按住 3 秒 / 确定要关机吗？`) now
  draws entirely from the embedded UI face; verified on the Plus with a
  screenshot. This matters when the card is missing: the core is absent and
  the UI subset is the only face.
* **Builds**: `readpico`, `readpico_jjwxc` and the Plus `build_shell_jjwxc`
  (JJWXC private component) all SUCCESS; the Plus image (6,252,848 B) was
  flashed to app0+app1 and boots with `[SFP] ... 293152 bytes`,
  `[CORE] ... 2553580 bytes`, one supplement family and the full SD emoji face.
