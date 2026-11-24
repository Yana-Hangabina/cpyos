// Small shared helpers for CPYOS game plugins (plain C, no libc).
#pragma once

#include "cpyos_plugin_api.h"

static int g_min(int a, int b) { return a < b ? a : b; }
static int g_max(int a, int b) { return a > b ? a : b; }
static int g_abs(int a) { return a < 0 ? -a : a; }
static int g_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int g_itoa(int value, char* out) {
  char tmp[12];
  int n = 0;
  if (value < 0) {
    out[0] = '-';
    return 1 + g_itoa(-value, out + 1);
  }
  if (value == 0) {
    out[0] = '0';
    out[1] = '\0';
    return 1;
  }
  while (value > 0 && n < 11) {
    tmp[n++] = (char)('0' + value % 10);
    value /= 10;
  }
  int k = 0;
  while (n > 0) out[k++] = tmp[--n];
  out[k] = '\0';
  return k;
}

static void g_text_center(const CpyosPluginApi* api, int y, int font, const char* text, int black) {
  const int w = api->text_width(font, text);
  api->draw_text((api->width - w) / 2, y, font, text, black);
}

static void g_text_center_rect(const CpyosPluginApi* api, int x, int y, int w, int h, int font, const char* text,
                               int black) {
  const int tw = api->text_width(font, text);
  const int fontH = font == CPYOS_FONT_LARGE ? 28 : (font == CPYOS_FONT_SMALL ? 14 : 20);
  api->draw_text(x + (w - tw) / 2, y + (h - fontH) / 2, font, text, black);
}

static void g_text_right(const CpyosPluginApi* api, int right, int y, int font, const char* text, int black) {
  const int w = api->text_width(font, text);
  api->draw_text(right - w, y, font, text, black);
}

// Small bounded string builder for localized status lines. No libc is linked
// into plugins, so these mirror g_itoa rather than calling strcat/snprintf.
static int g_append(char* out, int n, int cap, const char* text) {
  while (*text != '\0' && n < cap - 1) out[n++] = *text++;
  out[n] = '\0';
  return n;
}

static int g_append_int(char* out, int n, int cap, int value) {
  char digits[12];
  g_itoa(value, digits);
  return g_append(out, n, cap, digits);
}

static int g_append_text(char* out, int n, int cap, const CpyosPluginApi* api, int32_t id) {
  return g_append(out, n, cap, cpyos_plugin_text(api, id));
}

static int g_next_button(const CpyosPluginEvent* e) {
  return e->type == CPYOS_EVENT_BUTTON_DOWN ? (int)e->button : 0;
}

static void g_cell_frame(const CpyosPluginApi* api, int x, int y, int cell, int black) {
  api->fill_rect(x, y, cell, 2, black);
  api->fill_rect(x, y + cell - 2, cell, 2, black);
  api->fill_rect(x, y, 2, cell, black);
  api->fill_rect(x + cell - 2, y, 2, cell, black);
}
