#pragma once

#include <BleKeyboardHost.h>
#include <BoardConfig.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;

namespace bleinput {

enum class StartContext : uint8_t { Reader, Explicit };
// `Stopping` is a teardown still in flight (BleHid.isStopping()): the host refuses
// begin() meanwhile, but the caller should retry without treating it as a refusal.
enum class StartResult : uint8_t { Started, AlreadyRunning, Stopping, LowMemory, Unavailable, Failed };

#if FREEINK_DEVICE_READPICO
// Read Pico holds a large block of CONTIGUOUS INTERNAL DMA for its panel (epdiy's
// LUTs, line queues and bounce buffers, ~45 KB; see platformio.ini's A3 note) on
// top of CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768, and its BLE host allocates
// from PSRAM anyway (CROSSPOINT_BLE_HOST_PSRAM, via [s3_ble_psram]). The generic
// reader gate below (80 KB free / 32 KB largest internal) was not sized for a board
// that deliberately spends internal RAM that way, so it rejects a start this board
// can afford and BLE never comes up inside the reader.
//
// These floors still leave room for NimBLE's task stacks (which must be internal)
// and its remaining internal bookkeeping.
inline constexpr size_t kReaderMinFreeInternal = 56 * 1024;
inline constexpr size_t kReaderMinLargestInternal = 20 * 1024;
#else
inline constexpr size_t kReaderMinFreeInternal = 80 * 1024;
inline constexpr size_t kReaderMinLargestInternal = 32 * 1024;
#endif
inline constexpr size_t kExplicitMinFreeInternal = 70 * 1024;
inline constexpr size_t kExplicitMinLargestInternal = 24 * 1024;
inline constexpr size_t kMinFreePsram = 256 * 1024;
inline constexpr size_t kMinLargestPsram = 32 * 1024;

// Caller holds RenderLock across lifecycle checks, memory recovery and startup.
StartResult ensureStarted(GfxRenderer& renderer, StartContext context);
void stop();
void logDiagnostics(const char* phase);
bool encodeKey(const freeink::KeyEvent& event, uint8_t& kind, uint8_t& value);
void describeKey(uint8_t kind, uint8_t value, char* out, size_t outLen);

// Whether a reader may keep the BLE radio up: false once this boot's first start
// showed that no remote is bonded (see BleRemotePolicy.h). Pages that own BLE
// explicitly — Bluetooth settings, key mapping — are not gated by this.
bool readerRadioAllowed();

// Reader idle-off (upstream fecee610). A radio nobody connects to keeps the CPU at
// full speed, so a bonded remote that is switched off or left at home must not hold
// it for the whole session. armIdleClock() starts a fresh window; tickIdle() stops
// the radio once 30 s pass with nothing linked and no scan/connect in flight and
// returns true when it did so; idleStopped() stays true for the rest of the visit so
// the automatic start does not undo that decision.
void armIdleClock();
bool tickIdle();
bool idleStopped();

#if FREEINK_CAP_BLE_HID_HOST
inline bool isRunning() { return BleHid.isRunning(); }
inline bool isConnected() { return BleHid.isConnected(); }
#else
inline bool isRunning() { return false; }
inline bool isConnected() { return false; }
#endif

}  // namespace bleinput
