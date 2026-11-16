# 4G (cellular) transport on the Metalio E-INK4 Plus

Status: **ported from the official firmware, compiled into the image, not
validated on a 4G unit.** The test unit reports no modem (`0 byte(s)` at
2 Mbaud), so the WiFi path is the only one exercised on hardware.

## Where the code comes from

The official firmware (`https://github.com/CloudZao/Metalio-E-INK4-Plus`,
reference checkout `2b2c36ae`):

| Upstream | Copied to | Licence |
|---|---|---|
| `boards/common/board/uart_eth_modem.{h,cc}` | `components/cpyos_cellular/` | Apache-2.0 (Terrence) |
| `components/78__uart-uhci/` | `components/uart_uhci/` | Apache-2.0 |
| `components/espressif__iot_eth/` | `components/iot_eth/` | Apache-2.0 (Espressif) |
| `boards/common/board/nt26_board.cc` | reference for the bring-up order | — |

`iot_eth`'s CMake used `cmake_utilities`' `cu_pkg_define_version`; the vendored
copy pins the same version macros locally, so the source needs no managed
component. `PlusCellular.{h,cc}` is the CPYOS-side wrapper (the official
`Nt26Board` is tied to their `Board`/`Application` classes).

## Hardware and protocol

- UART2 at **2,000,000 baud**, TX GPIO58 / RX GPIO59, MRDY GPIO60, SRDY GPIO3.
- Framed UART-Ethernet protocol (MRDY/SRDY handshake, 50 µs ACK), `AT`
  commands ride the same frames.
- The modem installs its own `esp_netif` (`ETH_DEF`, DHCP) through `iot_eth`,
  so plain lwIP sockets and the existing TLS/HTTP code work unchanged.
- P14 is the shared 4G/BT/PA rail. The audio boot reset cuts it, so the modem
  start waits for the BH1098 init to finish first (the official board recovers
  the modem on the P14 rising edge for the same reason).

## Detection

`probeCellularOnce()` (shell) now uses the real rate: rail reset → 1.2 s settle
→ listen 2.5 s at 2 Mbaud. The old 115200 raw-AT probe could never see a
present module. Bench: `cellular` (stored state), `cellprobe` (live probe).
A module that stays silent until the framed `AT` would still read 0 bytes; the
framed driver in `cpyos_cellular` is the definitive test when hardware exists.

## Switching (official semantics)

- `CrossPointSettings::networkType`: `0` = WiFi (default), `1` = 4G.
- Settings → System → **Network** row, visible only when the modem was
  detected (`cellularPresent`). Changing it saves and **restarts**, exactly
  like `DualNetworkBoard::SwitchToNetworkType` (save + reboot).
- `WiFi` compat maps the Arduino API onto the modem when 4G is selected:
  `mode(STA)` starts it (blocking until an IP), `status()`, `getMode()`,
  `disconnect()`, `localIP()`, `SSID()` ("4G"), `RSSI()`.
- The shell starts the modem once at boot (after the audio rail reset) when 4G
  is selected; the JJK app skips the Wi-Fi sheet in 4G mode and waits for
  `NetworkStartup::setMode()` instead.

## Memory and UI budget

- The modem installs an `esp_netif` and a UART-Ethernet task inside `iot_eth`;
  nothing in `PlusCellular` adds a full-time stack of its own. The only extra
  task is `cell-recover` (8 KiB **internal**, priority 3) and it exists only
  for the few seconds after a P14 rail cycle, not while the link is idle.
- UART2 runs at 2 Mbaud through UHCI; its DMA buffers are internal and small
  (the modem frames are 1.5 KB), and the driver is the vendor's own code.
- 4G and Wi-Fi are mutually exclusive by `networkType` (switch saves + reboots,
  like the vendor), so they never compete for the same internal pool. What does
  compete is TLS: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` puts those buffers in
  PSRAM, and the AI-voice worker's 16 KiB stack is also in PSRAM with a
  16 KiB internal-largest guard. The same guard applies when the voice page is
  opened over 4G; the log line is `worker up: stack in PSRAM, internal
  largest=<bytes>`.
- UI: My shows `4G <ip>`; Settings → Controls adds a `4G` row (only when a
  module was detected) with IP, CSQ/dBm and registration read through a safe
  try-lock (a busy modem never blocks the UI). Signal **bars** and the SIM
  internal/external switch are still absent.

## Known gaps

- No SIM internal/external switch or signal-bar widget; the official network
  tab has those. The 4G status row does show IP + CSQ/dBm.
- No P14-rising-edge recovery on every rail change, only around the BH1098
  power reset: `plus_audio` stops the modem before the reset (weak
  `plus_audio_prepare_rail_cycle()` hook) and `PlusCellular` re-initialises it
  in the background once P14 is high again
  (`plus_audio_rail_cycle_done()` → `cell-recover` task, vendor
  `PreparePowerOff` / `RecoverAfterPowerOnAsync` order). Audio and 4G now run
  together; an in-flight download is interrupted by the reset, and a failed
  recovery is logged without a reboot or Wi-Fi fallback (same as the vendor).
- Over-the-air update / firmware download over 4G is untested.
