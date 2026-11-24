#include "../../common/game_common.h"

static const char* const kPuzzle =
    "530070000"
    "600195000"
    "098000060"
    "800060003"
    "400803001"
    "700020006"
    "060000280"
    "000419005"
    "000080079";

static uint8_t s_fixed[81];
static uint8_t s_user[81];
static int s_cursor;
static int s_solved;

static void reset_game(void) {
  for (int i = 0; i < 81; ++i) {
    const char c = kPuzzle[i];
    s_fixed[i] = (c >= '1' && c <= '9') ? (uint8_t)(c - '0') : 0;
    s_user[i] = 0;
  }
  s_cursor = 0;
  s_solved = 0;
}

static int value_at(int i) { return s_fixed[i] ? s_fixed[i] : s_user[i]; }

static int valid_place(int index, int value) {
  if (value == 0) return 1;
  const int r = index / 9;
  const int c = index % 9;
  for (int i = 0; i < 9; ++i) {
    if (i != index && value_at(i) == value) return 0;
  }
  for (int rr = 0; rr < 9; ++rr) {
    const int i = rr * 9 + c;
    if (i != index && value_at(i) == value) return 0;
  }
  for (int br = (r / 3) * 3; br < (r / 3) * 3 + 3; ++br) {
    for (int bc = (c / 3) * 3; bc < (c / 3) * 3 + 3; ++bc) {
      const int i = br * 9 + bc;
      if (i != index && value_at(i) == value) return 0;
    }
  }
  return 1;
}

static void check_solved(void) {
  for (int i = 0; i < 81; ++i) {
    if (value_at(i) == 0 || !valid_place(i, value_at(i))) {
      s_solved = 0;
      return;
    }
  }
  s_solved = 1;
}

static void draw(const CpyosPluginApi* api) {
  api->clear(0);
  g_text_center(api, 14, CPYOS_FONT_TITLE, cpyos_plugin_text(api, CPYOS_TEXT_TITLE_SUDOKU), 1);
  const int top = 68;
  const int avail = api->height - top - 56;
  int cell = g_min((api->width - 60) / 9, avail / 9);
  if (cell > 62) cell = 62;
  const int board = cell * 9;
  const int x0 = (api->width - board) / 2;
  const int y0 = top + (avail - board) / 2;
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      const int i = r * 9 + c;
      const int x = x0 + c * cell;
      const int y = y0 + r * cell;
      g_cell_frame(api, x, y, cell, 1);
      if (s_fixed[i]) {
        api->fill_rect(x + 1, y + 1, cell - 2, cell - 2, 1);
      }
      const int value = value_at(i);
      if (value != 0) {
        char text[2] = {(char)('0' + value), '\0'};
        g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, text, s_fixed[i] ? 0 : 1);
      }
      if (i == s_cursor) {
        api->fill_rect(x + 1, y + 1, cell - 2, 3, 1);
        api->fill_rect(x + 1, y + cell - 4, cell - 2, 3, 1);
        api->fill_rect(x + 1, y + 1, 3, cell - 2, 1);
        api->fill_rect(x + cell - 4, y + 1, 3, cell - 2, 1);
      }
    }
  }
  for (int i = 1; i < 9; ++i) {
    const int thickness = (i % 3 == 0) ? 3 : 1;
    api->fill_rect(x0 + i * cell, y0, thickness, board, 1);
    api->fill_rect(x0, y0 + i * cell, board, thickness, 1);
  }
  g_cell_frame(api, x0, y0, board, 1);
  char status[80];
  int n = 0;
  if (s_solved) {
    n = g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_SOLVED);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_RESTART);
  } else {
    n = g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_HINT_MOVE);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_PLACE);
  }
  g_text_center(api, api->height - 46, CPYOS_FONT_SMALL, status, 1);
  api->present();
}

int app_main(int argc, char** argv) {
  if (argc < 1 || argv == 0 || argv[0] == 0) return 1;
  const CpyosPluginApi* api = (const CpyosPluginApi*)argv[0];
  if (api->api_version != CPYOS_PLUGIN_API_VERSION) return 2;
  reset_game();
  draw(api);
  while (!api->exit_requested()) {
    CpyosPluginEvent e;
    while (api->poll_event(&e)) {
      const int b = g_next_button(&e);
      if (b == CPYOS_BUTTON_CONFIRM) {
        if (s_solved) {
          reset_game();
        } else if (!s_fixed[s_cursor]) {
          const int current = s_user[s_cursor];
          int next = (current + 1) % 10;
          while (next != 0 && !valid_place(s_cursor, next)) next = (next + 1) % 10;
          s_user[s_cursor] = (uint8_t)next;
          check_solved();
        }
        draw(api);
      } else if (b != 0) {
        int r = s_cursor / 9;
        int c = s_cursor % 9;
        if (b == CPYOS_BUTTON_UP) --r;
        if (b == CPYOS_BUTTON_DOWN) ++r;
        if (b == CPYOS_BUTTON_LEFT) --c;
        if (b == CPYOS_BUTTON_RIGHT) ++c;
        s_cursor = g_clamp(r, 0, 8) * 9 + g_clamp(c, 0, 8);
        draw(api);
      }
    }
    api->delay_ms(25);
  }
  return 0;
}
