#pragma once

#include <cstdint>

namespace appVisibility {

// Persisted bit positions: never reorder or reuse IDs, including uncompiled apps.
enum class AppId : uint8_t {
  ReadingStats = 0,
  WeRead = 1,
  Sudoku = 2,
  Gomoku = 3,
  ChineseChess = 4,
  Minesweeper = 5,
  Game2048 = 6,
  UglyAvatar = 7,
  Standby = 8,
  AirPage = 9,
  Buddy = 10,
  Sokoban = 11,
  PixelSwitch = 12,
  FileTransfer = 13,
  OpdsBrowser = 14,
  Calculator = 15,
  Woodfish = 16,
  // JJWXC (晋江文学城). Appended, never reordered: these are persisted bit
  // positions in hiddenAppsMask. IDs 18..31 remain available.
  Jjwxc = 17,
  // Reading browser. Appended for the same reason: bit positions are persisted.
  WebBrowser = 18,
  // Still camera (Metalio E-INK4 Plus only; other builds have no such tile).
  // Appended, never reordered: 0..18 keep their persisted bits.
  Camera = 19,
#if defined(CROSSPOINT_PLUS_PORT)
  // Voice memo (Metalio E-INK4 Plus only): microphone capture to WAV plus
  // playback through the BH1098. Appended for the same reason; the guard keeps
  // every other target's id table byte-identical to what shipped.
  VoiceMemo = 20,
  // AI voice client (Metalio E-INK4 Plus only): push-to-talk to a xiaozhi
  // WebSocket server. Appended, never reordered: 0..20 keep their persisted
  // bits, and 22..31 remain available.
  VoiceClient = 21,
#endif
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  // Native ELF plugin picker. Only builds with the plugin runtime link it;
  // every other target keeps its previous Count and bit table.
  NativePlugins = 22,
  Count = 23,
#elif defined(CROSSPOINT_PLUS_PORT)
  Count = 22,
#else
  Count = 20,
#endif
};

constexpr uint32_t appBit(const AppId id) { return uint32_t{1} << static_cast<uint8_t>(id); }

constexpr uint32_t DEFAULT_HIDDEN_APPS_MASK =
    appBit(AppId::ChineseChess) | appBit(AppId::Minesweeper) | appBit(AppId::Game2048) | appBit(AppId::UglyAvatar) |
    appBit(AppId::Buddy) | appBit(AppId::Sokoban) | appBit(AppId::PixelSwitch) | appBit(AppId::Woodfish);

static_assert(static_cast<uint8_t>(AppId::Count) <= 32, "app IDs must fit the persisted mask");

}  // namespace appVisibility
