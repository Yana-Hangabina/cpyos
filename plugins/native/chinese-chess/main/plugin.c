#include "../../common/game_common.h"

#define RANKS 10
#define FILES 9
#define CELLS 90

static uint8_t s_piece[CELLS];
static int s_cursor;
static int s_selected;
static int s_turn;    // 0 red, 1 black
static int s_winner;  // -1 none

static const char* const kRedNames[7] = {"帅", "仕", "相", "马", "车", "炮", "兵"};
static const char* const kBlackNames[7] = {"将", "士", "象", "马", "车", "炮", "卒"};

static void put(int r, int c, int side, int kind) {
  if (r >= 0 && r < RANKS && c >= 0 && c < FILES) s_piece[r * FILES + c] = (uint8_t)((side ? 10 : 0) + kind + 1);
}

static void reset_game(void) {
  for (int i = 0; i < CELLS; ++i) s_piece[i] = 0;
  static const int kBack[9] = {4, 3, 2, 1, 0, 1, 2, 3, 4};
  for (int c = 0; c < 9; ++c) put(0, c, 1, kBack[c]);
  put(2, 1, 1, 5);
  put(2, 7, 1, 5);
  for (int c = 0; c < 9; c += 2) put(3, c, 1, 6);
  for (int c = 0; c < 9; c += 2) put(6, c, 0, 6);
  put(7, 1, 0, 5);
  put(7, 7, 0, 5);
  for (int c = 0; c < 9; ++c) put(9, c, 0, kBack[c]);
  s_cursor = 9 * FILES + 4;
  s_selected = -1;
  s_turn = 0;
  s_winner = -1;
}

static int side_of(int p) { return p >= 10 ? 1 : 0; }
static int kind_of(int p) { return (p % 10) - 1; }
static int in_palace(int side, int r, int c) {
  if (c < 3 || c > 5) return 0;
  return side == 0 ? (r >= 7 && r <= 9) : (r >= 0 && r <= 2);
}
static int path_clear(int from, int to) {
  const int fr = from / FILES, fc = from % FILES;
  const int tr = to / FILES, tc = to % FILES;
  if (fr == tr) {
    const int step = tc > fc ? 1 : -1;
    for (int c = fc + step; c != tc; c += step) {
      if (s_piece[fr * FILES + c]) return 0;
    }
    return 1;
  }
  if (fc == tc) {
    const int step = tr > fr ? 1 : -1;
    for (int r = fr + step; r != tr; r += step) {
      if (s_piece[r * FILES + fc]) return 0;
    }
    return 1;
  }
  return 0;
}
static int count_between(int from, int to) {
  const int fr = from / FILES, fc = from % FILES;
  const int tr = to / FILES, tc = to % FILES;
  int count = 0;
  if (fr == tr) {
    const int step = tc > fc ? 1 : -1;
    for (int c = fc + step; c != tc; c += step) count += s_piece[fr * FILES + c] != 0;
  } else {
    const int step = tr > fr ? 1 : -1;
    for (int r = fr + step; r != tr; r += step) count += s_piece[r * FILES + fc] != 0;
  }
  return count;
}

static int can_move(int from, int to) {
  if (from == to) return 0;
  const int p = s_piece[from];
  if (!p) return 0;
  const int side = side_of(p);
  const int kind = kind_of(p);
  const int fr = from / FILES, fc = from % FILES;
  const int tr = to / FILES, tc = to % FILES;
  const int target = s_piece[to];
  if (target && side_of(target) == side) return 0;
  const int dr = tr - fr, dc = tc - fc;
  const int adr = g_abs(dr), adc = g_abs(dc);
  if (kind == 0) {  // king
    if (!in_palace(side, tr, tc)) return 0;
    return (adr + adc == 1);
  }
  if (kind == 1) {  // advisor
    return adr == 1 && adc == 1 && in_palace(side, tr, tc);
  }
  if (kind == 2) {  // elephant
    if (adr != 2 || adc != 2) return 0;
    if (side == 0 ? tr < 5 : tr > 4) return 0;
    return s_piece[((fr + tr) / 2) * FILES + (fc + tc) / 2] == 0;
  }
  if (kind == 3) {  // horse
    if (!((adr == 1 && adc == 2) || (adr == 2 && adc == 1))) return 0;
    if (adc == 2) {
      if (s_piece[fr * FILES + (fc + tc) / 2]) return 0;
    } else {
      if (s_piece[((fr + tr) / 2) * FILES + fc]) return 0;
    }
    return 1;
  }
  if (kind == 4) return (fr == tr || fc == tc) && path_clear(from, to);           // chariot
  if (kind == 5) {                                                                  // cannon
    if (fr != tr && fc != tc) return 0;
    const int screen = count_between(from, to);
    return target ? (screen == 1) : (screen == 0);
  }
  if (kind == 6) {  // pawn
    const int forward = side == 0 ? -1 : 1;
    if (dr == forward && dc == 0) return 1;
    const int crossed = side == 0 ? fr <= 4 : fr >= 5;
    return crossed && dr == 0 && adc == 1;
  }
  return 0;
}

static void draw(const CpyosPluginApi* api) {
  api->clear(0);
  g_text_center(api, 12, CPYOS_FONT_TITLE, cpyos_plugin_text(api, CPYOS_TEXT_TITLE_CHINESE_CHESS), 1);
  const int top = 64;
  const int availH = api->height - top - 52;
  int cell = g_min((api->width - 50) / FILES, availH / RANKS);
  if (cell > 64) cell = 64;
  const int boardW = cell * FILES;
  const int boardH = cell * RANKS;
  const int x0 = (api->width - boardW) / 2;
  const int y0 = top + (availH - boardH) / 2;
  for (int r = 0; r < RANKS; ++r) {
    for (int c = 0; c < FILES; ++c) {
      const int x = x0 + c * cell;
      const int y = y0 + r * cell;
      api->fill_rect(x + cell / 2, y, 1, cell, 1);
      api->fill_rect(x, y + cell / 2, cell, 1, 1);
      const int index = r * FILES + c;
      const int p = s_piece[index];
      if (p) {
        const int side = side_of(p);
        const int kind = kind_of(p);
        const char* text = side ? kBlackNames[kind] : kRedNames[kind];
        const int w = api->text_width(CPYOS_FONT_MEDIUM, text);
        api->draw_text(x + (cell - w) / 2, y + cell / 2 - 16, CPYOS_FONT_MEDIUM, text, 1);
        if (side == 1) g_cell_frame(api, x + cell / 2 - 18, y + cell / 2 - 18, 36, 1);
      }
      if (index == s_cursor) {
        api->fill_rect(x + 1, y + 1, cell - 2, 3, 1);
        api->fill_rect(x + 1, y + cell - 4, cell - 2, 3, 1);
        api->fill_rect(x + 1, y + 1, 3, cell - 2, 1);
        api->fill_rect(x + cell - 4, y + 1, 3, cell - 2, 1);
      }
    }
  }
  api->fill_rect(x0, y0, boardW, 2, 1);
  api->fill_rect(x0, y0 + boardH - 2, boardW, 2, 1);
  api->fill_rect(x0, y0, 2, boardH, 1);
  api->fill_rect(x0 + boardW - 2, y0, 2, boardH, 1);
  char status[80];
  int n = 0;
  if (s_winner >= 0) {
    n = g_append_text(status, n, (int)sizeof(status), api,
                      s_winner == 0 ? CPYOS_TEXT_RED_WINS : CPYOS_TEXT_BLACK_WINS);
  } else if (s_selected >= 0) {
    n = g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_SELECT);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_MOVE);
  } else {
    n = g_append_text(status, n, (int)sizeof(status), api, s_turn == 0 ? CPYOS_TEXT_RED : CPYOS_TEXT_BLACK);
    n = g_append(status, n, (int)sizeof(status), "  \xC2\xB7  ");
    g_append_text(status, n, (int)sizeof(status), api, CPYOS_TEXT_MOVE);
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
        if (s_winner >= 0) {
          reset_game();
        } else if (s_selected < 0) {
          if (s_piece[s_cursor] && side_of(s_piece[s_cursor]) == s_turn) s_selected = s_cursor;
        } else {
          if (can_move(s_selected, s_cursor)) {
            const int moving = s_piece[s_selected];
            const int target = s_piece[s_cursor];
            s_piece[s_cursor] = s_piece[s_selected];
            s_piece[s_selected] = 0;
            if (target && kind_of(target) == 0) s_winner = side_of(moving);
            s_turn = 1 - s_turn;
          }
          s_selected = -1;
        }
        draw(api);
      } else if (b != 0) {
        int r = s_cursor / FILES;
        int c = s_cursor % FILES;
        if (b == CPYOS_BUTTON_UP) --r;
        if (b == CPYOS_BUTTON_DOWN) ++r;
        if (b == CPYOS_BUTTON_LEFT) --c;
        if (b == CPYOS_BUTTON_RIGHT) ++c;
        s_cursor = g_clamp(r, 0, RANKS - 1) * FILES + g_clamp(c, 0, FILES - 1);
        draw(api);
      }
    }
    api->delay_ms(25);
  }
  return 0;
}
