#include "../../common/game_common.h"

#define BOARD 15

static uint8_t s_board[BOARD * BOARD];
static int s_cursor;
static int s_turn;    // 1 black, 2 white
static int s_winner;  // 0 none
static int s_moves;

static void reset_game(void) {
  for (int i = 0; i < BOARD * BOARD; ++i) s_board[i] = 0;
  s_cursor = (BOARD / 2) * BOARD + BOARD / 2;
  s_turn = 1;
  s_winner = 0;
  s_moves = 0;
}

static int check_win(int r, int c, int who) {
  static const int dr[4] = {0, 1, 1, 1};
  static const int dc[4] = {1, 0, 1, -1};
  for (int d = 0; d < 4; ++d) {
    int count = 1;
    for (int sign = -1; sign <= 1; sign += 2) {
      int rr = r + dr[d] * sign;
      int cc = c + dc[d] * sign;
      while (rr >= 0 && rr < BOARD && cc >= 0 && cc < BOARD && s_board[rr * BOARD + cc] == who) {
        ++count;
        rr += dr[d] * sign;
        cc += dc[d] * sign;
      }
    }
    if (count >= 5) return 1;
  }
  return 0;
}

static void place_stone(void) {
  if (s_winner != 0 || s_board[s_cursor] != 0) return;
  const int r = s_cursor / BOARD;
  const int c = s_cursor % BOARD;
  s_board[s_cursor] = (uint8_t)s_turn;
  ++s_moves;
  if (check_win(r, c, s_turn)) {
    s_winner = s_turn;
  } else {
    s_turn = (s_turn == 1) ? 2 : 1;
  }
}

static void draw(const CpyosPluginApi* api) {
  api->clear(0);
  g_text_center(api, 14, CPYOS_FONT_TITLE, cpyos_plugin_text(api, CPYOS_TEXT_TITLE_GOMOKU), 1);
  const int top = 68;
  const int availW = api->width - 32;
  const int availH = api->height - top - 54;
  int cell = g_min(availW / BOARD, availH / BOARD);
  if (cell > 44) cell = 44;
  const int boardW = cell * BOARD;
  const int x0 = (api->width - boardW) / 2;
  const int y0 = top + (availH - boardW) / 2;
  for (int r = 0; r < BOARD; ++r) {
    for (int c = 0; c < BOARD; ++c) {
      const int x = x0 + c * cell;
      const int y = y0 + r * cell;
      api->fill_rect(x, y, cell, 1, 1);
      api->fill_rect(x, y, 1, cell, 1);
      const uint8_t v = s_board[r * BOARD + c];
      if (v == 1) {
        api->fill_rect(x + 4, y + 4, cell - 7, cell - 7, 1);
        g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, "B", 0);
      } else if (v == 2) {
        g_cell_frame(api, x + 4, y + 4, cell - 7, 1);
        g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, "W", 1);
      }
    }
  }
  for (int i = 1; i < BOARD; ++i) {
    api->fill_rect(x0, y0 + i * cell, boardW, 1, 1);
    api->fill_rect(x0 + i * cell, y0, 1, boardW, 1);
  }
  const int cr = s_cursor / BOARD;
  const int cc = s_cursor % BOARD;
  api->fill_rect(x0 + cc * cell + 1, y0 + cr * cell + 1, cell - 1, 2, 1);
  api->fill_rect(x0 + cc * cell + 1, y0 + cr * cell + cell - 3, cell - 1, 2, 1);
  api->fill_rect(x0 + cc * cell + 1, y0 + cr * cell + 1, 2, cell - 1, 1);
  api->fill_rect(x0 + cc * cell + cell - 3, y0 + cr * cell + 1, 2, cell - 1, 1);
  char status[80];
  int n = 0;
  if (s_winner != 0) {
    n = g_append_text(status, n, (int)sizeof(status), api,
                      s_winner == 1 ? CPYOS_TEXT_BLACK_WINS : CPYOS_TEXT_WHITE_WINS);
  } else {
    n = g_append_text(status, n, (int)sizeof(status), api, s_turn == 1 ? CPYOS_TEXT_BLACK : CPYOS_TEXT_WHITE);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_PLACE);
  }
  g_text_center(api, api->height - 48, CPYOS_FONT_SMALL, status, 1);
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
        if (s_winner != 0) {
          reset_game();
        } else {
          place_stone();
        }
        draw(api);
      } else if (b != 0) {
        int r = s_cursor / BOARD;
        int c = s_cursor % BOARD;
        if (b == CPYOS_BUTTON_UP) --r;
        if (b == CPYOS_BUTTON_DOWN) ++r;
        if (b == CPYOS_BUTTON_LEFT) --c;
        if (b == CPYOS_BUTTON_RIGHT) ++c;
        r = g_clamp(r, 0, BOARD - 1);
        c = g_clamp(c, 0, BOARD - 1);
        s_cursor = r * BOARD + c;
        draw(api);
      }
    }
    api->delay_ms(25);
  }
  return 0;
}
