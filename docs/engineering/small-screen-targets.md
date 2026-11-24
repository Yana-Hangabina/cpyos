# Small-screen targets: EEGO A4 and Metalio E-Ink 4

Both boards are separate ESP32-S3 compile-time targets that already exist in
the pinned FreeInk SDK. This document records what CpyOS adds on top of them:
the compact UI contract, the host-tested home geometry, and the build-only
acceptance boundary. **No hardware was available while this adaptation was
written; every image produced from these targets is UNTESTED ON HARDWARE.**

```sh
pio run -e eego_a4
pio run -e metalio_eink4
```

| Target | Panel | Native glass | UI orientation | Touch / input | Storage |
|---|---|---|---|---|---|
| `eego_a4` | UC8279C | 768×552 | 552×768 portrait | GSLX680 nine-point, screen Home/Back key, Up/Down/Power | SD, PCF8563 RTC, optional LM3630A frontlight |
| `metalio_eink4` | SSD1677 | 800×480 | 480×800 portrait | CST816S + TCA9555 bezel keys, Power only | 1-bit SDMMC, TCA9555 power rails |

The board facts live in `freeink-sdk/libs/hardware/BoardConfig` and in
[`eego-a4.md`](eego-a4.md) / [`metalio-eink4.md`](metalio-eink4.md). Do not copy
pin tables into CpyOS UI code.

### Flash layout

The full CpyOS image (33 languages, UI fonts, apps) is ~7.37 MB, above the
shared 6.25 MiB dual-OTA table. Both targets therefore use
`partitions_readpico.csv` (7.19 MiB per OTA slot, 1.5 MiB spiffs), the same
layout as Read Pico. Both are 16 MiB N16R8 parts. A first installation must
write the matching bootloader and partition table; an app-only write against a
device that still has the 6.25 MiB table will not fit. The Nightly target
metadata declares the same CSV so full-install packages use the right offsets.

## Relationship to kiikoread

`kiikoread` rc110 ships two ESP32-S31 boards, both 1216×684 (`pico` and
`metalio47`), so it does not contain EEGO A4 or Metalio E-Ink 4 board bring-up.
What this port reuses from that project is its **board-layer discipline**:
a compile-time board identity, capability macros, and shared UI code that never
guesses hardware at runtime. The actual panel/touch/power support comes from
the FreeInk SDK profiles already referenced by CrossMux (`metalio-hw-test`
2.0.51 for Metalio and the eego template firmware for A4). No Personal-Study or
vendored vendor application code was copied.

## Compact UI contract

Small panels do not merely scale the Read Pico layout. CpyOS renders in the
portrait UI frame, so the pressure is the **content box** left after the status,
title and tab bands — roughly 480×636 on Metalio and 552×604 on A4 versus
684×1060 on Read Pico.

Reader font sizes follow the same rule rather than a per-device setting:
`readerPointRangeForPortraitHeight()` in `src/ReaderFontSizes.h` derives the maximum from
the portrait panel height. Read Pico / Plus (1216 px) expose the full 10–40 pt
ladder; the 3.97" A4 (768 px) and the 800 px panels cap at 26 pt, and a 1100 px
panel caps at 36 pt. A future panel only needs its `BoardConfig` metrics; no
new settings row or board-specific branch is required.

The shared breakpoints live in
[`src/components/ResponsiveUi.h`](../../src/components/ResponsiveUi.h):

- compact when the content width is below 560 px, or when the height cannot fit
  the standard home cards (900 px, or 1000 px when the update row is present);
- new pages should branch on these predicates, not on `FREEINK_DEVICE_*`.

The home dashboard is the first page converted. Its geometry lives in
[`src/components/HomeDashboardLayout.h`](../../src/components/HomeDashboardLayout.h)
and is host-tested for 480×636, 552×604, 600×800, 800×1100 and a short
landscape window:

| Element | Standard (Read Pico) | Compact (A4 / Metalio) |
|---|---|---|
| Continue card | 318 px, 140×212 cover, 56 px action, 34 px wave | 34% of content height (200–232 px), cover fits the card, 44 px action, 14 px wave |
| Reading strip | 140 px: four metrics + seven-day bars | 84–110 px: four metrics only (the 148 px bar column does not fit beside them) |
| Recent covers | 370 px, three 132×184 covers, title + author + meta | remaining height, three width-derived covers, title + meta (author omitted) |
| Update row | 96 px | 56–76 px |
| Text column | fixed offsets from the 604 px design | offsets derived from the card and the actual line heights |

Every compact element is clamped so it cannot leave the content box, overlap
the next card, or push the action under the waveform. If a card is shorter than
the action plus the wave, the wave is dropped rather than drawn over the action.
The ring hit rectangles come from the same layout object as the drawing, so a
resized card cannot drift away from its touch target.

Other CpyOS pages already derive their geometry from the content box (library
grid, settings/list rows, reader toolbar sheet, popups clamp their width to the
safe area). The CPY theme keeps its legacy 64 px list rhythm on non-high-density
targets, which is the intended density for these 480/552 px panels.

## Adding another screen size

1. Add or extend the SDK `BoardConfig` profile (panel driver, native resolution,
   touch, keys, rails, SD, frontlight) and a separate PlatformIO env. Never make
   a runtime guess between two panel controllers.
2. Add the target to the Nightly matrix only after a first-flash plan exists.
3. Do not add board-specific UI offsets. Run the home layout test with the new
   content box; if a page has its own fixed geometry, give it a pure layout
   header and a host test with both compact and standard viewports.
4. Label build-only artifacts `UNTESTED ON HARDWARE` and list the hardware-only
   checks below.

## Build-only acceptance boundary

The following are **not** validated without hardware and must not be claimed by
a successful build:

- display waveforms, ghosting, partial/grayscale refresh, orientation;
- touch calibration, gestures, screen-key Home/Back behavior;
- PMU/rail sequencing, battery ADC, charging indication, frontlight curves;
- SD hot-plug/contention, RTC retention, deep-sleep wake and standby current;
- panel-specific bezel insets and rounded-corner masks.

Host checks that do run:

```sh
bash src/home_dashboard_layout/run_home_dashboard_layout_test.sh
```

It asserts the frozen Read Pico geometry is unchanged, then validates every
compact viewport for containment, ordering and text-column fit, and includes a
mutation that must fail when the action starts overlapping the wave.

## Version label and artifacts

Builds from these two envs carry `-untested+<sha>` in `CROSSPOINT_VERSION`
(for example `0.7.0-eego-a4-untested+1a2b3c4`), so About/OTA and the boot log
cannot present a build-only image as hardware-validated. Package them with:

```sh
python3 tools/package_untested_board.py --env eego_a4 --env metalio_eink4 --out artifacts/untested
```

The helper copies the image plus the first-install segments
(bootloader/partitions/boot_app0/app0), writes `manifest.json`, and refuses an
image whose `CROSSPOINT_VERSION` lacks the `untested` label. `manifest.json`
records `hardwareTested: false` and the exact offsets.

## Build record (2026-11-07, dev tree, hardware not present)

| Target | RAM static | Flash | Image | Core |
|---|---:|---:|---:|---|
| `eego_a4` | 88,572 B (27.0%) | 7,367,938 / 7,536,640 (97.8%) | 7,373,200 B | rebuilt `crossmux` core; app desc `640b9069-dirty` |
| `metalio_eink4` | 122,740 B (37.5%) | 7,097,187 / 7,536,640 (94.2%) | 7,102,384 B | prebuilt `arduino-lib-builder` core (TinyUSB/MSC graph); `CROSSPOINT_VERSION` still carries the untested label |

The Metalio image deliberately keeps the prebuilt TinyUSB core because its env
enables USB MSC; rebuilding that core from source would drop the MSC component
graph. Its higher static RAM is that core plus MSC state, not the UI port. The
EEGO image rebuilds the tuned core (loop-stack/BLE policy), hence the lower
static RAM.

Both images contain the dev tree's private JJWXC component; the public
no-source build omits it and is ~300 KB smaller.

A green build is not an acceptance test.
