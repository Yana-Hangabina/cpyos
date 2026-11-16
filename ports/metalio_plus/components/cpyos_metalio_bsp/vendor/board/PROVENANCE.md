# Vendored Metalio E-INK4 Plus BSP

- Source: <https://github.com/CloudZao/Metalio-E-INK4-Plus>
- Commit: `2b2c36ae58f32a9cb1a721d1000883a2693651a4` (v2.0.74, 2026-11-04)
- License: MIT; the full text is in the repository root
  `LICENSES/Metalio-E-INK4-Plus-MIT.txt`.

## Copied

- `config.h`
- `bus/` — PCA9555, TPS65185 and the I2C1 helper.
- `display/` — the S31 EPDiy output path, board glue, waveforms, `metalio_epd.c`
  and `metalio_touch.c`.

## Excluded

- `display/epd_img1.h` (7.5 MB test image), `font.c`, `builtin_waveforms.c`,
  `epd_board_common.c`, `board_specific.c`.
- `metalio_display_setup.*` (LVGL), `metalio_touch_upgrade.*`, `firmware/`.
- `output_lcd/lut.S` (Xtensa assembly); the RISC-V stub is used instead.

## Local changes

- `display/displays.c`: only `E0470A14_AF`, so the 273 KB ED047TC2 waveform
  table stays out of the image.
- `display/epd_board_metalio_eink4_plus.c`: a TPS power-on failure logs and
  returns instead of calling `esp_restart()`, so a board without a panel does
  not reboot-loop during bring-up. The failure path now calls
  `epd_board_poweroff()` first, so PWRUP/VCOM_CTRL are never left asserted.
  Added `epd_board_metalio_eink4_plus_aux_rail_set(bool)` as a future hook for
  a measured 4G/BT-audio/PA power policy. It is **not called at boot**: cutting
  P14 left the TCA/TPS path unpowered across a CPU reset on the first unit.
- `display/metalio_epd.c`: P14 is left HIGH as the vendor initialises it; do not
  drive it low until the board rail tree is measured.
- `display/metalio_touch.c`: reads both FT6336U points (P2 at 0x07–0x0A) and
  publishes a pair sequence for pinch gestures; the vendor driver exposed only
  one point.
- `include/epd_font24.h`: minimal logical-size stub needed by `metalio_touch.c`.
- `cpyos_sd.c`: 4-bit SD mount and `MetalioSd_ReleaseHold()`.
- `CMakeLists.txt`: source/include selection for the CPYOS ESP-IDF target.

The EPDiy-derived files are LGPL-3.0-or-later; see the repository's
`THIRD_PARTY_NOTICES.md` and `LICENSES/`. Do not copy kiikoread
Personal-Study files into this directory.
