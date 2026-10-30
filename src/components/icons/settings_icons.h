#pragma once

// Per-row leading icons for the CPY settings list (design frame 06: every row
// carries a 40px glyph). Lucide (MIT) bitmaps from listIcons48.h.
//
// Every row that can appear in the four Settings tabs must resolve to a glyph:
// FreeInkUI only reserves the leading slot for rows that have one, so a nullptr
// shifts that row's label ~58 px left of its neighbours. That covers the
// SettingInfo entries, the ACTION rows appended in
// SettingsActivity::rebuildSettingsLists() (Text Settings, Wi-Fi, About, ...),
// and the dictionary row built at runtime.
// `scripts/tests/test_settings_icons.py` audits the real sources and fails when
// a visible row is missing here, so add a mapping (and a whitelist entry with a
// written reason if a row is deliberately icon-less) instead of leaving a gap.

#include <I18nKeys.h>

#include "listIcons48.h"

namespace settings_icons {

inline const freeink::Icon* forSetting(const StrId id) {
  switch (id) {
    // Displays
    case StrId::STR_SLEEP_SCREEN:
      return &icon_clock_48;
    case StrId::STR_SLEEP_COVER_MODE:
    case StrId::STR_SLEEP_COVER_FILTER:
    case StrId::STR_READING_BACKGROUND:
    case StrId::STR_IMAGES:
      return &icon_image_48;
    case StrId::STR_QUICK_RESUME_TIMEOUT:
      return &icon_zap_48;
    case StrId::STR_STANDBY_TITLE:
    case StrId::STR_CLOCK:
    case StrId::STR_CLOCK_FORMAT:
    case StrId::STR_CLOCK_UTC_OFFSET:
    case StrId::STR_CLOCK_DST:
    case StrId::STR_CLOCK_IN_HEADER:
    case StrId::STR_CLOCK_SYNCED:
    case StrId::STR_AUTO_TIME:
    case StrId::STR_TIMEZONE:
      return &icon_clock_48;
    case StrId::STR_HIDE_BATTERY:
    case StrId::STR_BATTERY:
      return &icon_battery_48;
    case StrId::STR_REFRESH_FREQ:
    case StrId::STR_PERIODIC_REFRESH:
      return &icon_refresh_cw_48;
    case StrId::STR_REFRESH_INTERVAL:
      return &icon_timer_48;
    case StrId::STR_UI_THEME:
      return &icon_palette_48;
    case StrId::STR_INX_RECENT_LAYOUT:
    case StrId::STR_INX_LIBRARY_LAYOUT:
    case StrId::STR_INX_APPS_LAYOUT:
      return &icon_layout_grid_48;
    case StrId::STR_INX_TAB_POSITION:
    case StrId::STR_READER_MENU_STYLE:
      return &icon_panel_bottom_48;
    case StrId::STR_UI_DENSITY:
      return &icon_sliders_horizontal_48;
    case StrId::STR_SUNLIGHT_FADING_FIX:
    case StrId::STR_DBL_CLICK_PWR_LIGHT:
    case StrId::STR_BRIGHTNESS:
    case StrId::STR_WARMTH:
    case StrId::STR_FRONTLIGHT:
      return &icon_sun_48;
    case StrId::STR_SHOW_BUTTON_HINTS:
      return &icon_keyboard_48;
    case StrId::STR_RESTORE_LIGHT_ON_WAKE:
      return &icon_sunrise_48;
    case StrId::STR_NIGHT_MODE:
      return &icon_moon_48;

    // Reader
    case StrId::STR_FONT_FAMILY:
    case StrId::STR_TITLE:
      return &icon_type_48;
    case StrId::STR_FONT_SIZE:
      return &icon_case_sensitive_48;
    case StrId::STR_LINE_SPACING:
      return &icon_align_justify_48;
    case StrId::STR_WORD_SPACING:
    case StrId::STR_CHARACTER_SPACING:
    case StrId::STR_EXTRA_SPACING:
    case StrId::STR_READING_GUIDE_LINE_OFFSET:
    case StrId::STR_SCREEN_MARGIN:
    case StrId::STR_FIRST_LINE_INDENT:
    case StrId::STR_PARAGRAPH_INDENTATION:
      return &icon_move_horizontal_48;
    case StrId::STR_PARA_ALIGNMENT:
      return &icon_align_left_48;
    case StrId::STR_EMBEDDED_STYLE:
    case StrId::STR_DOCUMENT_MATCHING:
    case StrId::STR_OPDS_FILENAME_FORMAT:
      return &icon_file_text_48;
    case StrId::STR_PAGE_TURN_EFFECT:
    case StrId::STR_TEXT_AA:
      return &icon_sparkles_48;
    case StrId::STR_FAKE_BOLD:
      return &icon_bold_48;
    case StrId::STR_FOCUS_READING:
      return &icon_focus_48;
    case StrId::STR_READING_GUIDE_LINE:
    case StrId::STR_READING_GUIDE_LINE_STYLE:
      return &icon_ruler_48;
    case StrId::STR_HYPHENATION:
    case StrId::STR_CHAPTER_PAGE_COUNT:
      return &icon_book_48;
    case StrId::STR_ORIENTATION:
    case StrId::STR_PAGE_TURN_DIRECTION:
    case StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION:
    case StrId::STR_TILT_PAGE_TURN:
    case StrId::STR_MOTION_PAGE_TURN:
      return &icon_rotate_cw_48;
    case StrId::STR_IMAGE_SCALING:
      return &icon_maximize_48;
    case StrId::STR_PROGRESS_BAR:
    case StrId::STR_PROGRESS_BAR_THICKNESS:
    case StrId::STR_BOOK_PROGRESS_PERCENTAGE:
      return &icon_chart_line_48;
    case StrId::STR_DAILY_GOAL:
      return &icon_target_48;
    case StrId::STR_ENABLE_ACHIEVEMENTS:
      return &icon_trophy_48;
    case StrId::STR_ACHIEVEMENT_POPUPS:
      return &icon_bell_48;
    // Reader tab: navigation rows and the runtime dictionary row. They sit in
    // the same list as the value rows above, so they need the same slot.
    case StrId::STR_FAST_PAGE_TURN:
      return &icon_zap_48;
    case StrId::STR_DICTIONARY:
    case StrId::STR_MANAGE_DICTIONARIES:
      return &icon_book_48;
    case StrId::STR_TEXT_SETTINGS:
      return &icon_type_48;
    case StrId::STR_MANAGE_FONTS:
      return &icon_case_sensitive_48;
    // Status-bar subpage row; it never takes this list's path today, but a
    // hide/bottom/top mode belongs to the same glyph if that changes.
    case StrId::STR_CUSTOMISE_STATUS_BAR:
    case StrId::STR_XTC_STATUS_BAR:
      return &icon_panel_bottom_48;
    case StrId::STR_READING_STATS:
      return &icon_chart_line_48;

    // Controls
    case StrId::STR_SIDE_BTN_LAYOUT:
    case StrId::STR_SHORT_PWR_BTN:
      return &icon_power_48;
    case StrId::STR_TOUCH_READER_CONTROLS:
    case StrId::STR_LONG_PRESS_BEHAVIOR:
      return &icon_hand_48;
    case StrId::STR_NEXT_PAGE_GESTURE:
      return &icon_chevron_right_48;
    case StrId::STR_PREV_PAGE_GESTURE:
      return &icon_chevron_left_48;
    case StrId::STR_SHOW_READER_MENU:
    case StrId::STR_LONG_PRESS_MENU:
      return &icon_menu_48;
    case StrId::STR_PWR_BTN_FOOTNOTE_BACK:
      return &icon_corner_down_left_48;
    case StrId::STR_BACK_SHORT_TO_FILE_BROWSER:
    case StrId::STR_OPDS_DOWNLOAD_FOLDER:
      return &icon_folder_48;
    // Controls tab: navigation rows. Bluetooth and the home-key remap are board
    // dependent (BLE host / capacitive home key); mapped unconditionally so the
    // list is aligned on every board that shows them.
    case StrId::STR_BLUETOOTH:
      return &icon_bluetooth_48;
    case StrId::STR_HOME_BUTTON:
      return &icon_house_48;
    case StrId::STR_REMAP_FRONT_BUTTONS:
      return &icon_sliders_horizontal_48;

    // System
    case StrId::STR_TIME_TO_SLEEP:
      return &icon_timer_48;
    case StrId::STR_SHOW_HIDDEN_FILES:
      return &icon_eye_48;
    case StrId::STR_SOUND_FEEDBACK:
      return &icon_volume_2_48;
    case StrId::STR_HAPTIC_FEEDBACK:
      return &icon_vibrate_48;
    case StrId::STR_LIBRARY_USE_METADATA:
      return &icon_library_48;
    case StrId::STR_REMOVE_READ_FROM_RECENTS:
      return &icon_eraser_48;
    case StrId::STR_MOVE_FINISHED_TO_READ:
      return &icon_check_48;
    case StrId::STR_SYNC_SERVER_URL:
    case StrId::STR_KOREADER_USERNAME:
    case StrId::STR_KOREADER_PASSWORD:
      return &icon_radio_tower_48;
    case StrId::STR_SEND_METADATA:
      return &icon_upload_48;
    case StrId::STR_SYNC_BEHAVIOR:
      return &icon_refresh_cw_48;
    // Buzzer feedback (Read Pico): the source is a sound event, the pitch is a
    // tone, and the strength is a continuous level.
    case StrId::STR_BUZZER_FEEDBACK:
      return &icon_volume_2_48;
    case StrId::STR_BUZZER_PITCH:
      return &icon_music_48;
    case StrId::STR_BUZZER_STRENGTH:
      return &icon_sliders_horizontal_48;
    // System tab: navigation rows, in the order the list shows them.
    case StrId::STR_APPS_TITLE:
      return &icon_blocks_48;
    case StrId::STR_APP_VISIBILITY:
      return &icon_layout_grid_48;
    case StrId::STR_DATE_AND_TIME:
      return &icon_clock_48;
    case StrId::STR_WIFI_NETWORKS:
      return &icon_wifi_48;
    case StrId::STR_KOREADER_SYNC:
    case StrId::STR_OPDS_SERVERS:
      return &icon_radio_tower_48;
    case StrId::STR_CLEAR_READING_CACHE:
      return &icon_eraser_48;
    case StrId::STR_RESTORE_SYSTEM_SETTINGS:
      return &icon_refresh_cw_48;
    case StrId::STR_CHECK_UPDATES:
      return &icon_download_48;
    // A firmware file dragged onto the card, not the U-disk mode.
    case StrId::STR_SD_FIRMWARE_UPDATE:
      return &icon_file_48;
    // Three full-screen refreshes; the arrow glyph reads as "restore the panel".
    case StrId::STR_SCREEN_REPAIR:
      return &icon_maximize_48;
    case StrId::STR_PLUGINS:
      return &icon_blocks_48;
    case StrId::STR_KEYBOARD_LAYOUTS:
      return &icon_keyboard_48;
    case StrId::STR_ABOUT:
      return &icon_info_48;
    case StrId::STR_LANGUAGE:
      return &icon_languages_48;

    default:
      return nullptr;
  }
}

}  // namespace settings_icons
