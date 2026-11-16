# Third-party notice — kiikoread (read-pico-reader)

Parts of the Metalio E-INK4 Plus display stack are derived from **kiikoread
(read-pico-reader) release candidate 110**, an Apache-2.0 licensed e-reader
firmware for the same E0470 panel family.

* Upstream: `read-pico-reader` rc110 (local reference checkout at
  `repos/kiikoread-rc110`; `.scratch/refs/kiikoread-rc110` is a compatibility
  symlink in the development tree).
* Copyright (c) 2026 mindreset / the kiikoread contributors.
* Licensing is per file:
  * `boards/metalio47/src/main/display.c` is **Apache-2.0**
    (`SPDX-License-Identifier: Apache-2.0`). The refresh-policy structure is
    derived from it.
  * `components/epdiy` is vendored **epdiy** (**LGPL-3.0-or-later**); its
    `highlevel.c` carries kiikoread's `force_full` extension. Our
    `epd_hl_update_area_full()` is derived from that file, which is why the
    vendored epdiy in this port stays LGPL (see the repository's
    `THIRD_PARTY_NOTICES.md`).
  * Files marked `LicenseRef-Kiikoread-Personal-Study-1.0` are **not** copied;
    `scripts/tests/test_license_scope.py` enforces this.

## What was ported

| Our file | Reference | Notes |
|---|---|---|
| `components/cpyos_metalio_bsp/vendor/board/display/highlevel.c` – `epd_hl_update_area_full()` | `components/epdiy/src/highlevel.c` – `hl_update_area(..., force_full=true)` | Non-differential drive of one rectangle: every pixel in the area is conditioned, including unchanged ones. Needed for overlays/sliders drawn over a previous page. |
| `components/cpyos_ui/HalDisplay.cpp` – refresh policy | `boards/metalio47/src/main/display.c` | Soft GL16 while a gesture runs, GC16 sweep at the gesture boundary and for periodic/manual clean; full (non-differential) drive for clean paths. |
| `components/cpyos_metalio_bsp/vendor/board/display/waveforms/` | vendor CloudZao waveform tables shared by both projects | The waveform LUT data itself comes from the vendor firmware; kiikoread's scene tables are derived from the same source. |

The port does **not** copy kiikoread's application code, fonts or assets; only
the epdiy highlevel primitive and the refresh-policy structure described above
were adapted to CPYOS's HAL and to this repository's coding conventions.

Per Apache-2.0 §4, this notice is kept with the derived sources and the
derivation is recorded in the commit message of the change that introduced it.
