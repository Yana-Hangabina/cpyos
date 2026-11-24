#include "../../common/game_common.h"

static const char* const kLevels[3][7] = {
    {"########", "#      #", "# .$@  #", "#      #", "########", "", ""},
    {"#########", "#       #", "# .$ $ .#", "#   @   #", "#       #", "#########", ""},
    {"##########", "#        #", "# .$  $@ #", "#  ####  #", "# .    . #", "#        #", "##########"},
};
static const uint8_t kLevelRows[] = {5, 6, 7};
static const uint8_t kLevelCount = 3;

static char s_map[12][20];
static int s_rows;
static int s_cols;
static int s_pr;
static int s_pc;
static int s_level;
static int s_won;

static void load_level(int index) {
  if (index < 0) index = 0;
  if (index >= (int)kLevelCount) index = (int)kLevelCount - 1;
  s_level = index;
  s_rows = kLevelRows[index];
  s_cols = 0;
  for (int r = 0; r < s_rows; ++r) {
    const char* row = kLevels[index][r];
    int c = 0;
    for (; row[c] != '\0' && c < 20; ++c) {
      char cell = row[c];
      s_map[r][c] = cell;
      if (cell == '@' || cell == '+') {
        s_pr = r;
        s_pc = c;
      }
      if (c + 1 > s_cols) s_cols = c + 1;
    }
    for (; c < 20; ++c) s_map[r][c] = ' ';
  }
  s_won = 0;
}

static int is_floor(char c) { return c == ' ' || c == '.' || c == '@' || c == '+' || c == '$' || c == '*'; }

static int box_done(char c) { return c == '*'; }

static void check_win(void) {
  for (int r = 0; r < s_rows; ++r) {
    for (int c = 0; c < s_cols; ++c) {
      if (s_map[r][c] == '$') return;
    }
  }
  s_won = 1;
}

static void move_player(int dr, int dc) {
  if (s_won) return;
  const int nr = s_pr + dr;
  const int nc = s_pc + dc;
  if (nr < 0 || nc < 0 || nr >= s_rows || nc >= s_cols) return;
  char* dest = &s_map[nr][nc];
  if (*dest == '#') return;
  if (*dest == '$' || *dest == '*') {
    const int br = nr + dr;
    const int bc = nc + dc;
    if (br < 0 || bc < 0 || br >= s_rows || bc >= s_cols) return;
    char* beyond = &s_map[br][bc];
    if (!is_floor(*beyond) || *beyond == '$' || *beyond == '*') return;
    *beyond = (*beyond == '.' || *beyond == '+') ? '*' : '$';
  }
  char* from = &s_map[s_pr][s_pc];
  *from = (*from == '+') ? '.' : ' ';
  *dest = (*dest == '.' || *dest == '*') ? '+' : '@';
  s_pr = nr;
  s_pc = nc;
  check_win();
}

static void draw(const CpyosPluginApi* api) {
  api->clear(0);
  char title[64];
  int n = g_append_text(title, 0, (int)sizeof(title), api, CPYOS_TEXT_TITLE_SOKOBAN);
  n = g_append(title, n, (int)sizeof(title), " ");
  n = g_append_int(title, n, (int)sizeof(title), s_level + 1);
  n = g_append(title, n, (int)sizeof(title), "/");
  g_append_int(title, n, (int)sizeof(title), kLevelCount);
  g_text_center(api, 16, CPYOS_FONT_TITLE, title, 1);
  const int top = 76;
  const int availW = api->width - 32;
  const int availH = api->height - top - 62;
  int cell = g_min(availW / s_cols, availH / s_rows);
  if (cell > 74) cell = 74;
  const int boardW = cell * s_cols;
  const int boardH = cell * s_rows;
  const int x0 = (api->width - boardW) / 2;
  const int y0 = top + (availH - boardH) / 2;
  for (int r = 0; r < s_rows; ++r) {
    for (int c = 0; c < s_cols; ++c) {
      const int x = x0 + c * cell;
      const int y = y0 + r * cell;
      const char v = s_map[r][c];
      if (v == '#') {
        api->fill_rect(x, y, cell, cell, 1);
      } else {
        g_cell_frame(api, x, y, cell, 1);
        if (v == '.' || v == '+' || v == '*') api->fill_rect(x + cell / 2 - 5, y + cell / 2 - 5, 10, 10, 1);
        if (v == '$' || v == '*') {
          api->fill_rect(x + cell / 5, y + cell / 5, cell - 2 * cell / 5, cell - 2 * cell / 5, 1);
        }
        if (v == '@' || v == '+') {
          g_text_center_rect(api, x, y, cell, cell, CPYOS_FONT_LARGE, "@", 1);
        }
      }
    }
  }
  if (s_won) {
    g_text_center(api, api->height - 54, CPYOS_FONT_MEDIUM, cpyos_plugin_text(api, CPYOS_TEXT_WIN), 1);
  } else {
    g_text_center(api, api->height - 54, CPYOS_FONT_SMALL, cpyos_plugin_text(api, CPYOS_TEXT_HINT_MOVE), 1);
  }
  api->present();
}

int app_main(int argc, char** argv) {
  if (argc < 1 || argv == 0 || argv[0] == 0) return 1;
  const CpyosPluginApi* api = (const CpyosPluginApi*)argv[0];
  if (api->api_version != CPYOS_PLUGIN_API_VERSION) return 2;
  load_level(0);
  draw(api);
  while (!api->exit_requested()) {
    CpyosPluginEvent e;
    while (api->poll_event(&e)) {
      const int b = g_next_button(&e);
      if (s_won && b == CPYOS_BUTTON_CONFIRM) {
        load_level((s_level + 1) % (int)kLevelCount);
        draw(api);
      } else if (b == CPYOS_BUTTON_UP) {
        move_player(-1, 0);
        draw(api);
      } else if (b == CPYOS_BUTTON_DOWN) {
        move_player(1, 0);
        draw(api);
      } else if (b == CPYOS_BUTTON_LEFT) {
        move_player(0, -1);
        draw(api);
      } else if (b == CPYOS_BUTTON_RIGHT) {
        move_player(0, 1);
        draw(api);
      }
    }
    api->delay_ms(25);
  }
  return 0;
}
