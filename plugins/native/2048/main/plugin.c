// Pilot native plugin: 2048 for CPYOS.
//
// Built against cpyos_plugin_api.h only; no libc, no imports, one argv table.
// Build: see plugins/native/2048/README.md.

#include "cpyos_plugin_api.h"

static uint8_t s_board[16];
static uint32_t s_rng = 0x20482048u;
static int s_score = 0;
static int s_over = 0;

static void zero_board(void) {
  for (int i = 0; i < 16; ++i) s_board[i] = 0;
  s_score = 0;
  s_over = 0;
}

static uint32_t next_random(void) {
  s_rng = s_rng * 1664525u + 1013904223u;
  return s_rng;
}

static void add_tile(void) {
  int empties = 0;
  for (int i = 0; i < 16; ++i) {
    if (s_board[i] == 0) ++empties;
  }
  if (empties == 0) return;
  int pick = (int)(next_random() % (uint32_t)empties);
  for (int i = 0; i < 16; ++i) {
    if (s_board[i] != 0) continue;
    if (pick-- == 0) {
      const uint32_t r = next_random();
      uint8_t value = 2;
      while ((r & 3u) == 0u) {
        value = (uint8_t)(value << 1);
      }
      s_board[i] = value;
      return;
    }
  }
}

static void number_text(unsigned value, char* out) {
  char tmp[12];
  int n = 0;
  if (value == 0) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  while (value > 0 && n < 11) {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  }
  int k = 0;
  while (n > 0) out[k++] = tmp[--n];
  out[k] = '\0';
}

static int has_moves(void) {
  for (int i = 0; i < 16; ++i) {
    if (s_board[i] == 0) return 1;
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      const int i = r * 4 + c;
      if (c < 3 && s_board[i] == s_board[i + 1]) return 1;
      if (r < 3 && s_board[i] == s_board[i + 4]) return 1;
    }
  }
  return 0;
}

static int move_line(const int a, const int b, const int c, const int d) {
  const int idx[4] = {a, b, c, d};
  unsigned vals[4] = {0, 0, 0, 0};
  int n = 0;
  for (int i = 0; i < 4; ++i) {
    if (s_board[idx[i]] != 0) vals[n++] = s_board[idx[i]];
  }
  unsigned merged[4] = {0, 0, 0, 0};
  int m = 0;
  for (int i = 0; i < n; ++i) {
    if (i + 1 < n && vals[i] == vals[i + 1]) {
      merged[m++] = vals[i] * 2;
      s_score += (int)(vals[i] * 2);
      ++i;
    } else {
      merged[m++] = vals[i];
    }
  }
  int changed = 0;
  for (int i = 0; i < 4; ++i) {
    const unsigned next = i < m ? merged[i] : 0;
    if (s_board[idx[i]] != (uint8_t)(next)) changed = 1;
    s_board[idx[i]] = (uint8_t)(next);
  }
  return changed;
}

static int move_board(int dx, int dy) {
  int changed = 0;
  if (dx < 0) {
    for (int r = 0; r < 4; ++r) changed |= move_line(r * 4, r * 4 + 1, r * 4 + 2, r * 4 + 3);
  } else if (dx > 0) {
    for (int r = 0; r < 4; ++r) changed |= move_line(r * 4 + 3, r * 4 + 2, r * 4 + 1, r * 4);
  } else if (dy < 0) {
    for (int c = 0; c < 4; ++c) changed |= move_line(c, c + 4, c + 8, c + 12);
  } else if (dy > 0) {
    for (int c = 0; c < 4; ++c) changed |= move_line(c + 12, c + 8, c + 4, c);
  }
  if (changed) {
    add_tile();
    if (!has_moves()) s_over = 1;
  }
  return changed;
}

static void outline(const CpyosPluginApi* api, int x, int y, int w, int h, int thickness) {
  api->fill_rect(x, y, w, thickness, 1);
  api->fill_rect(x, y + h - thickness, w, thickness, 1);
  api->fill_rect(x, y, thickness, h, 1);
  api->fill_rect(x + w - thickness, y, thickness, h, 1);
}

static void draw_board(const CpyosPluginApi* api) {
  const int margin = 24;
  const int top = 96;
  int cell = (api->width - margin * 2) / 4;
  const int maxCellH = (api->height - top - 72) / 4;
  if (cell > maxCellH) cell = maxCellH;
  const int boardW = cell * 4;
  const int x0 = (api->width - boardW) / 2;

  api->clear(0);
  api->draw_text(margin, 22, CPYOS_FONT_TITLE, cpyos_plugin_text(api, CPYOS_TEXT_TITLE_2048), 1);

  char scoreText[20];
  number_text((unsigned)s_score, scoreText);
  const char* scoreLabel = cpyos_plugin_text(api, CPYOS_TEXT_SCORE);
  const int scoreLabelW = api->text_width(CPYOS_FONT_MEDIUM, scoreLabel);
  const int scoreValueX = api->width - margin - api->text_width(CPYOS_FONT_MEDIUM, scoreText);
  if (scoreLabelW > 0) {
    api->draw_text(scoreValueX - scoreLabelW - 8, 32, CPYOS_FONT_MEDIUM, scoreLabel, 1);
  }
  api->draw_text(scoreValueX, 32, CPYOS_FONT_MEDIUM, scoreText, 1);

  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      const int x = x0 + c * cell;
      const int y = top + r * cell;
      const unsigned value = s_board[r * 4 + c];
      if (value == 0) {
        outline(api, x + 2, y + 2, cell - 4, cell - 4, 2);
        continue;
      }
      api->fill_rect(x + 2, y + 2, cell - 4, cell - 4, 1);
      char text[12];
      number_text(value, text);
      const int font = value >= 100 ? CPYOS_FONT_MEDIUM : CPYOS_FONT_LARGE;
      const int tw = api->text_width(font, text);
      const int tx = x + (cell - tw) / 2;
      const int ty = y + cell / 2 - 16;
      api->draw_text(tx, ty, font, text, 0);
    }
  }

  const int footer = api->height - 56;
  if (s_over) {
    api->draw_text(margin, footer - 24, CPYOS_FONT_MEDIUM, cpyos_plugin_text(api, CPYOS_TEXT_GAME_OVER), 1);
    api->draw_text(margin, footer - 4, CPYOS_FONT_SMALL, cpyos_plugin_text(api, CPYOS_TEXT_RESTART_HINT), 1);
  } else {
    api->draw_text(margin, footer - 24, CPYOS_FONT_SMALL, cpyos_plugin_text(api, CPYOS_TEXT_HINT_MOVE), 1);
  }
  api->present();
}

static void save_state(const CpyosPluginApi* api) {
  if (api->data_write == 0) return;
  uint8_t data[29];
  data[0] = '2';
  data[1] = '0';
  data[2] = '4';
  data[3] = '8';
  for (int i = 0; i < 16; ++i) data[4 + i] = s_board[i];
  const uint32_t score = (uint32_t)s_score;
  data[20] = (uint8_t)(score & 0xFF);
  data[21] = (uint8_t)((score >> 8) & 0xFF);
  data[22] = (uint8_t)((score >> 16) & 0xFF);
  data[23] = (uint8_t)((score >> 24) & 0xFF);
  data[24] = (uint8_t)(s_rng & 0xFF);
  data[25] = (uint8_t)((s_rng >> 8) & 0xFF);
  data[26] = (uint8_t)((s_rng >> 16) & 0xFF);
  data[27] = (uint8_t)((s_rng >> 24) & 0xFF);
  data[28] = (uint8_t)(s_over != 0);
  api->data_write("save.bin", data, (int32_t)sizeof(data));
}

static int load_state(const CpyosPluginApi* api) {
  if (api->data_read == 0) return 0;
  uint8_t data[29];
  const int32_t got = api->data_read("save.bin", data, (int32_t)sizeof(data));
  if (got != (int32_t)sizeof(data) || data[0] != '2' || data[1] != '0' || data[2] != '4' || data[3] != '8') {
    return 0;
  }
  for (int i = 0; i < 16; ++i) s_board[i] = data[4 + i];
  s_score = (int)(data[20] | ((uint32_t)data[21] << 8) | ((uint32_t)data[22] << 16) | ((uint32_t)data[23] << 24));
  s_rng = (uint32_t)data[24] | ((uint32_t)data[25] << 8) | ((uint32_t)data[26] << 16) | ((uint32_t)data[27] << 24);
  s_over = data[28] != 0;
  return 1;
}

static void new_game(const CpyosPluginApi* api) {
  zero_board();
  s_rng = (uint32_t)api->now_ms() | 1u;
  add_tile();
  add_tile();
  api->present();
}

int app_main(int argc, char** argv) {
  if (argc < 1 || argv == 0 || argv[0] == 0) return 1;
  const CpyosPluginApi* api = (const CpyosPluginApi*)argv[0];
  if (api->api_version != CPYOS_PLUGIN_API_VERSION) {
    api->log("unsupported host API version");
    return 2;
  }
  api->log("2048 starting");
  if (!load_state(api)) new_game(api);
  draw_board(api);

  while (!api->exit_requested()) {
    CpyosPluginEvent event;
    while (api->poll_event(&event)) {
      int dx = 0;
      int dy = 0;
      if (event.type == CPYOS_EVENT_BUTTON_DOWN) {
        if (s_over && event.button == CPYOS_BUTTON_CONFIRM) {
          new_game(api);
          draw_board(api);
          continue;
        }
        if (event.button == CPYOS_BUTTON_LEFT) dx = -1;
        if (event.button == CPYOS_BUTTON_RIGHT) dx = 1;
        if (event.button == CPYOS_BUTTON_UP) dy = -1;
        if (event.button == CPYOS_BUTTON_DOWN) dy = 1;
      } else if (event.type == CPYOS_EVENT_SWIPE) {
        if (event.button == CPYOS_SWIPE_LEFT) dx = -1;
        if (event.button == CPYOS_SWIPE_RIGHT) dx = 1;
        if (event.button == CPYOS_SWIPE_UP) dy = -1;
        if (event.button == CPYOS_SWIPE_DOWN) dy = 1;
      }
      if ((dx != 0 || dy != 0) && !s_over) {
        if (move_board(dx, dy)) draw_board(api);
      }
    }
    api->delay_ms(30);
  }
  save_state(api);
  api->log("2048 exiting");
  return 0;
}
