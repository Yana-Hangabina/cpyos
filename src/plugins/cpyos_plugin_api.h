// CPYOS native plugin ABI v1.
//
// This header is the whole contract between the firmware and an out-of-tree
// ELF plugin. It is plain C (no libc, no STL, no C++ objects) because plugins
// are relocatable modules, not shared libraries: on Xtensa a plugin that
// imports libc through the PLT is fragile, and on RISC-V the loader resolves
// only relative relocations cheaply. Passing one table in argv[0] keeps both
// targets on the same source and lets the host grow the ABI by appending
// fields (plugins must check struct_size before reading past it).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPYOS_PLUGIN_API_VERSION 1u

typedef enum CpyosPluginFont {
  CPYOS_FONT_SMALL = 0,   // 10 pt UI face
  CPYOS_FONT_MEDIUM = 1,  // 12 pt UI face
  CPYOS_FONT_LARGE = 2,   // 18 pt UI face
  CPYOS_FONT_TITLE = 3,   // 18 pt display face
} CpyosPluginFont;

typedef enum CpyosPluginButton {
  CPYOS_BUTTON_CONFIRM = 1,
  CPYOS_BUTTON_LEFT = 2,
  CPYOS_BUTTON_RIGHT = 3,
  CPYOS_BUTTON_UP = 4,
  CPYOS_BUTTON_DOWN = 5,
} CpyosPluginButton;

typedef enum CpyosPluginEventType {
  CPYOS_EVENT_NONE = 0,
  CPYOS_EVENT_BUTTON_DOWN = 1,
  CPYOS_EVENT_BUTTON_UP = 2,
  CPYOS_EVENT_TAP = 3,
  CPYOS_EVENT_SWIPE = 4,
} CpyosPluginEventType;

typedef enum CpyosPluginSwipe {
  CPYOS_SWIPE_LEFT = 1,
  CPYOS_SWIPE_RIGHT = 2,
  CPYOS_SWIPE_UP = 3,
  CPYOS_SWIPE_DOWN = 4,
} CpyosPluginSwipe;

// Localized strings exposed through CpyosPluginApi::text(). IDs are stable and
// append-only: a plugin compiled against a newer header must still guard the
// function pointer with struct_size, and the host returns "" for unknown ids.
typedef enum CpyosPluginText {
  CPYOS_TEXT_TITLE_2048 = 1,
  CPYOS_TEXT_TITLE_SOKOBAN = 2,
  CPYOS_TEXT_TITLE_GOMOKU = 3,
  CPYOS_TEXT_TITLE_MINESWEEPER = 4,
  CPYOS_TEXT_TITLE_SUDOKU = 5,
  CPYOS_TEXT_TITLE_CHINESE_CHESS = 6,
  CPYOS_TEXT_SCORE = 7,
  CPYOS_TEXT_GAME_OVER = 8,
  CPYOS_TEXT_RESTART_HINT = 9,
  CPYOS_TEXT_NEW_GAME = 10,
  CPYOS_TEXT_RESTART = 11,
  CPYOS_TEXT_MOVES = 12,
  CPYOS_TEXT_LEVEL = 13,
  CPYOS_TEXT_BLACK = 14,
  CPYOS_TEXT_WHITE = 15,
  CPYOS_TEXT_RED = 16,
  CPYOS_TEXT_BLACK_WINS = 17,
  CPYOS_TEXT_WHITE_WINS = 18,
  CPYOS_TEXT_RED_WINS = 19,
  CPYOS_TEXT_CLEARED = 20,
  CPYOS_TEXT_BOOM = 21,
  CPYOS_TEXT_SOLVED = 22,
  CPYOS_TEXT_DIG = 23,
  CPYOS_TEXT_PLACE = 24,
  CPYOS_TEXT_MOVE = 25,
  CPYOS_TEXT_SELECT = 26,
  CPYOS_TEXT_WIN_HINT = 27,
  CPYOS_TEXT_HINT_MOVE = 28,
  CPYOS_TEXT_WIN = 29,
} CpyosPluginText;

typedef struct CpyosPluginEvent {
  uint16_t type;    // CpyosPluginEventType
  uint16_t button;  // CpyosPluginButton for button events, CpyosPluginSwipe for swipes
  int16_t x;        // tap coordinates
  int16_t y;
  int16_t dx;       // swipe delta in screen pixels
  int16_t dy;
} CpyosPluginEvent;

// Host services. Every pointer is valid for the whole plugin run. Drawing and
// present() are safe to call from the plugin's own task; they take the render
// lock internally. present() pushes the framebuffer to the panel.
typedef struct CpyosPluginApi {
  uint32_t struct_size;   // sizeof(CpyosPluginApi) as compiled into the host
  uint32_t api_version;   // CPYOS_PLUGIN_API_VERSION

  int32_t width;          // logical screen size in pixels
  int32_t height;

  // Diagnostics. The host prefixes the installed plugin id.
  void (*log)(const char* message);

  // Cooperative shutdown: Back sets exit_requested; a plugin may also ask the
  // host to close by calling request_exit().
  void (*request_exit)(void);
  int32_t (*exit_requested)(void);

  uint32_t (*now_ms)(void);
  void (*delay_ms)(uint32_t ms);

  // Non-blocking input queue. Returns 1 and fills `out` when an event is
  // pending, 0 otherwise.
  int32_t (*poll_event)(CpyosPluginEvent* out);

  // Drawing. `black` is 1 for ink, 0 for paper.
  void (*clear)(int32_t black);
  void (*fill_rect)(int32_t x, int32_t y, int32_t w, int32_t h, int32_t black);
  void (*draw_text)(int32_t x, int32_t y, int32_t font, const char* text, int32_t black);
  int32_t (*text_width)(int32_t font, const char* text);
  void (*present)(void);

  // Optional PSRAM allocator; returns NULL on failure. Release with free().
  void* (*alloc)(uint32_t bytes);
  void (*release)(void* pointer);

  // Scoped persistent storage in /.crosspoint/plugin_data/<id>/. Names are
  // simple file names ([A-Za-z0-9._-], <=48 chars); data_read returns bytes or
  // -1, data_write returns 0 or -1. Appended to ABI v1; older plugins ignore
  // these fields via struct_size.
  int32_t (*data_read)(const char* name, uint8_t* buffer, int32_t capacity);
  int32_t (*data_write)(const char* name, const uint8_t* data, int32_t length);

  // Localized UI string for a CpyosPluginText id. The returned pointer is
  // borrowed for the plugin run; do not free or write to it. Appended to ABI
  // v1, so older plugins ignore it via struct_size.
  const char* (*text)(int32_t id);
} CpyosPluginApi;

// Safe accessor for the appended text() service. Returns "" when the host
// predates the field, so a newer plugin degrades to blank labels instead of
// reading past an older struct.
static inline const char* cpyos_plugin_text(const CpyosPluginApi* api, int32_t id) {
  if (api == 0) return "";
  if (api->struct_size < (uint32_t)(offsetof(CpyosPluginApi, text) + sizeof(api->text))) return "";
  if (api->text == 0) return "";
  return api->text(id);
}

// The loader calls this symbol. argv[0] is a `const CpyosPluginApi*`; argv[1]
// is the installed plugin id (never write to it). Return 0 on a clean exit.
typedef int (*CpyosPluginEntry)(int argc, char** argv);

// Plugin entry signature. The host looks this symbol up by name; plugins are
// linked with -e app_main (see elf_loader.cmake).
int app_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif
