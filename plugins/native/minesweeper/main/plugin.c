#include "../../common/game_common.h"

#define SIDE 9
#define CELLS (SIDE * SIDE)
#define MINES 10

static uint8_t s_mine[CELLS];
static uint8_t s_open[CELLS];
static int s_cursor;
static int s_over;
static int s_won;
static int s_placed;
static uint32_t s_rng;

static void reset_game(const CpyosPluginApi* api) {
  for (int i = 0; i < CELLS; ++i) {
    s_mine[i] = 0;
    s_open[i] = 0;
  }
  s_cursor = 4 * SIDE + 4;
  s_over = 0;
  s_won = 0;
  s_placed = 0;
  s_rng = api->now_ms() | 1u;
}

static int neighbors(int r, int c) {
  int count = 0;
  for (int dr = -1; dr <= 1; ++dr) {
    for (int dc = -1; dc <= 1; ++dc) {
      if (dr == 0 && dc == 0) continue;
      const int rr = r + dr;
      const int cc = c + dc;
      if (rr >= 0 && rr < SIDE && cc >= 0 && cc < SIDE && s_mine[rr * SIDE + cc]) ++count;
    }
  }
  return count;
}

static void place_mines(int safe_index) {
  int placed = 0;
  while (placed < MINES) {
    s_rng = s_rng * 1664525u + 1013904223u;
    const int i = (int)(s_rng % CELLS);
    if (i == safe_index || s_mine[i]) continue;
    s_mine[i] = 1;
    ++placed;
  }
  s_placed = 1;
}

static void reveal(int index) {
  int stack[CELLS];
  int top = 0;
  stack[top++] = index;
  while (top > 0) {
    const int i = stack[--top];
    if (s_open[i]) continue;
    s_open[i] = 1;
    const int r = i / SIDE;
    const int c = i % SIDE;
    if (s_mine[i]) {
      s_over = 1;
      continue;
    }
    if (neighbors(r, c) == 0) {
      for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
          if (dr == 0 && dc == 0) continue;
          const int rr = r + dr;
          const int cc = c + dc;
          if (rr >= 0 && rr < SIDE && cc >= 0 && cc < SIDE) {
            const int ni = rr * SIDE + cc;
            if (!s_open[ni] && !s_mine[ni]) stack[top++] = ni;
          }
        }
      }
    }
  }
}

static void check_win(void) {
  int opened = 0;
  for (int i = 0; i < CELLS; ++i) opened += s_open[i] != 0;
  if (!s_over && opened == CELLS - MINES) {
    s_won = 1;
    s_over = 1;
  }
}

static void dig(int index) {
  if (s_over || s_open[index]) return;
  if (!s_placed) place_mines(index);
  reveal(index);
  check_win();
}

static void draw(const CpyosPluginApi* api) {
  api->clear(0);
  g_text_center(api, 14, CPYOS_FONT_TITLE, cpyos_plugin_text(api, CPYOS_TEXT_TITLE_MINESWEEPER), 1);
  const int top = 70;
  const int availW = api->width - 40;
  const int availH = api->height - top - 58;
  int cell = g_min(availW / SIDE, availH / SIDE);
  if (cell > 54) cell = 54;
  const int boardW = cell * SIDE;
  const int x0 = (api->width - boardW) / 2;
  const int y0 = top + (availH - boardW) / 2;
  for (int r = 0; r < SIDE; ++r) {
    for (int c = 0; c < SIDE; ++c) {
      const int i = r * SIDE + c;
      const int x = x0 + c * cell;
      const int y = y0 + r * cell;
      g_cell_frame(api, x, y, cell, 1);
      if (s_over && s_mine[i]) {
        g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, "*", 1);
      } else if (!s_open[i]) {
        g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, "#", 1);
      } else {
        const int n = neighbors(r, c);
        if (n > 0) {
          char text[2] = {(char)('0' + n), '\0'};
          g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, text, 1);
        }
      }
      if (i == s_cursor) {
        api->fill_rect(x + 1, y + 1, cell - 2, 3, 1);
        api->fill_rect(x + 1, y + cell - 4, cell - 2, 3, 1);
        api->fill_rect(x + 1, y + 1, 3, cell - 2, 1);
        api->fill_rect(x + cell - 4, y + 1, 3, cell - 2, 1);
      }
    }
  }
  char status[80];
  int n = 0;
  if (s_won) {
    n = g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_CLEARED);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_RESTART);
  } else if (s_over) {
    n = g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_BOOM);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_RESTART);
  } else {
    n = g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_HINT_MOVE);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_DIG);
  }
  g_text_center(api, api->height - 48, CPYOS_FONT_SMALL, status, 1);
  api->present();
}

int app_main(int argc, char** argv) {
  if (argc < 1 || argv == 0 || argv[0] == 0) return 1;
  const CpyosPluginApi* api = (const CpyosPluginApi*)argv[0];
  if (api->api_version != CPYOS_PLUGIN_API_VERSION) return 2;
  reset_game(api);
  draw(api);
  while (!api->exit_requested()) {
    CpyosPluginEvent e;
    while (api->poll_event(&e)) {
      const int b = g_next_button(&e);
      if (b == CPYOS_BUTTON_CONFIRM) {
        if (s_over) {
          reset_game(api);
        } else {
          dig(s_cursor);
        }
        draw(api);
      } else if (b != 0) {
        int r = s_cursor / SIDE;
        int c = s_cursor % SIDE;
        if (b == CPYOS_BUTTON_UP) --r;
        if (b == CPYOS_BUTTON_DOWN) ++r;
        if (b == CPYOS_BUTTON_LEFT) --c;
        if (b == CPYOS_BUTTON_RIGHT) ++c;
        s_cursor = g_clamp(r, 0, SIDE - 1) * SIDE + g_clamp(c, 0, SIDE - 1);
        draw(api);
      }
    }
    api->delay_ms(25);
  }
  return 0;
}
