# Vendored Crosspoint Simulator headers

- Source: <https://github.com/0x1abin/crosspoint-simulator>
- Local library checkout: `repos/crossmux/.pio/libdeps/simulator_readpico/simulator`
  (synced from upstream `20e73803`; local commit `58b6694`).
- License: MIT; the full text is in the repository root
  `LICENSES/Crosspoint-Simulator-MIT.txt`.

## Copied

The host-compatible HAL/Arduino headers used as the starting point for the
experimental Plus UI compatibility layer: `Arduino.h`, `WString.h`, `Print.h`,
`HardwareSerial.h`, `Stream.h`, `StreamString.h`, `BoardConfig.h`,
`EInkDisplay.h`, `HalDisplay.h`, `HalGPIO.h`, `HalStorage.h`, `HalClock.h`,
`HalPowerManager.h`, `HalSystem.h`, `HalMemory.h`, `MemoryManager.h`,
`InputManager.h`, `InputModality.h` and their small support headers.

## Not copied

The simulator's host `.cpp` implementations (SDL windows/input, host
filesystem, host network) are not used. The Plus target provides its own
`HalDisplay.cpp` and stubs.

## Local changes

- `Arduino.h`: replaced `std::thread`/`std::chrono` timing with
  `esp_timer`/FreeRTOS; `ESP` uses IDF heap/PSRAM APIs.
- `BoardConfig.h`: log transport switched to `esp_rom_printf`.
- `HalDisplay.h`: replaced with the current CPYOS `lib/hal/HalDisplay.h`
  interface.
- `EInkDisplay.h`: pulls in `GrayscaleCapabilities.h`/`GrayArea.h` from the
  freeink SDK so the current renderer interface compiles.
- Host-only headers that would shadow IDF (`esp_*`, `WiFi*`, SDL, etc.) were
  removed.
