#include <Arduino.h>
#include <BoardConfig.h>
#include <DeviceSecret.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalOtaSlot.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SPI.h>
#include <TrustedTime.h>
#include <VectorFontSupport.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif
#include <builtinFonts/all.h>

#include <cstring>

#include "AchievementsStore.h"
#include "BleInput.h"
#include "BootTimings.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingStatsStore.h"
#include "EmojiFont.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "UiCjkFallbacks.h"
#include "Epub/hyphenation/HyphenationEmbedding.h"
#if !defined(CROSSPOINT_EMBED_HYPHENATION)
#include "Epub/hyphenation/HyphenationStore.h"
#endif
#if defined(CROSSPOINT_EXTERNAL_FONTS)
#include "ExternalFontPackStorage.h"
#endif
#include "WifiCredentialStore.h"
#include "network/OtaUpdater.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/home/FindBooksActivity.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/home/MyActivity.h"
#include "activities/library/LibraryListActivity.h"
#ifdef BOARD_HAS_PSRAM
#include "activities/apps/jjwxc/JjwxcActivity.h"
#endif
#include "activities/home/ReaderRecoveryActivity.h"
#if defined(CROSSPOINT_MUPDF)
#include "activities/reader/PdfReaderActivity.h"
#endif
#include "activities/network/WifiSelectionActivity.h"
#ifdef ENABLE_CHINESE_VERSION
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/SettingsSearchActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#endif
#include "activities/settings/LanguageSelectActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/PluginEvents.h"
#include "util/ScreenshotUtil.h"
#include "util/Timezones.h"
#include "util/UserGuide.h"

#if CROSSPOINT_VECTOR_FONTS
// Rendering (incl. FreeType TTF rasterization) runs on the Arduino loop task.
// The default 8 KB stack overflows inside FreeType's FT_Open_Face / variable-font
// parsing. This runtime override applies even with the prebuilt (dio_opi) core,
// where CONFIG_ARDUINO_LOOP_STACK_SIZE from sdkconfig is baked in and ignored.
// Vector-font boards only: without TTF the stock loop stack has always sufficed,
// and non-PSRAM boards need the 16KB back in DRAM.
SET_LOOP_TASK_STACK_SIZE(24 * 1024)
#endif

#if CROSSPOINT_CAP_SOUND_FEEDBACK
#include <SoundFeedback.h>
#endif
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
#include "BuzzerFeedback.h"
#endif

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
static unsigned long allowSleepAt = 0;
constexpr unsigned long READING_STATS_CHECKPOINT_IDLE_MS = 15UL * 1000UL;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
static_assert(static_cast<uint8_t>(BuzzerFeedback::Pitch::Deep) == CrossPointSettings::BUZZER_PITCH_DEEP);
static_assert(static_cast<uint8_t>(BuzzerFeedback::Pitch::Soft) == CrossPointSettings::BUZZER_PITCH_SOFT);
static_assert(static_cast<uint8_t>(BuzzerFeedback::Pitch::Crisp) == CrossPointSettings::BUZZER_PITCH_CRISP);
#endif
#if CROSSPOINT_CAP_SOUND_FEEDBACK
static_assert(CrossPointSettings::SOUND_FEEDBACK_OFF == static_cast<uint8_t>(SoundFeedback::Level::Off));
static_assert(CrossPointSettings::SOUND_FEEDBACK_LOW == static_cast<uint8_t>(SoundFeedback::Level::Low));
static_assert(CrossPointSettings::SOUND_FEEDBACK_MEDIUM == static_cast<uint8_t>(SoundFeedback::Level::Medium));
static_assert(CrossPointSettings::SOUND_FEEDBACK_HIGH == static_cast<uint8_t>(SoundFeedback::Level::High));
static_assert(HalGPIO::BTN_BACK == SoundFeedback::BUTTON_BACK &&
              HalGPIO::BTN_CONFIRM == SoundFeedback::BUTTON_CONFIRM &&
              HalGPIO::BTN_LEFT == SoundFeedback::BUTTON_LEFT && HalGPIO::BTN_RIGHT == SoundFeedback::BUTTON_RIGHT &&
              HalGPIO::BTN_UP == SoundFeedback::BUTTON_UP && HalGPIO::BTN_DOWN == SoundFeedback::BUTTON_DOWN &&
              HalGPIO::BTN_POWER == SoundFeedback::BUTTON_POWER);
#endif

void updateBluetoothLifecycle() {
#if FREEINK_CAP_BLE_HID_HOST
  static unsigned long nextStartAttemptAt = 0;
  const auto wanted = [] {
    return SETTINGS.bluetoothEnabled && activityManager.keepsBluetoothAlive() &&
           !activityManager.requiresExclusiveStorageLoop() && WiFi.getMode() == WIFI_MODE_NULL;
  };
  if (!wanted()) {
    bleinput::stop();
    return;
  }
  // Preparation blocks new starts; C3 and PSRAM readers keep existing links
  // through chapter construction and book indexing.
  if (bleinput::isRunning() || activityManager.deferBluetoothStart() || millis() < nextStartAttemptAt) return;
  // Non-reader pages that keep an existing link alive own their explicit start
  // attempts; only readers use the automatic reader-memory gate and retry loop.
  if (!activityManager.isReaderActivity()) return;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  // Rendering may have started a chapter build while we were acquiring the lock.
  if (!wanted() || activityManager.deferBluetoothStart() || !activityManager.isReaderActivity() ||
      bleinput::isRunning())
    return;
  const auto result = bleinput::ensureStarted(renderer, bleinput::StartContext::Reader);
  if (result == bleinput::StartResult::LowMemory || result == bleinput::StartResult::Failed) {
    nextStartAttemptAt = millis() + 2000;
  }
#endif
}
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
// All legacy built-in reader IDs share one 12pt offline fallback. Complete
// families, other sizes, and style variants come from SD .cpfont files.
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_cjk_14.h>
#include <builtinFonts/notosans_cjk_16.h>
#if !defined(CROSSPOINT_EXTERNAL_FONTS)
#include <builtinFonts/notoserif_title_23.h>
#endif
#endif

#if defined(CROSSPOINT_EXTERNAL_FONTS)
// Read Pico: the 12pt GB2312 face and the title serif are loaded from
// /.crosspoint/font_pack.dat into PSRAM by setupDisplayAndFonts().  Keeping the
// static EpdFontData here (not the pack buffer) means EpdFont's pointer stays
// valid once the loader fills it.
static EpdFontData externalCjk12Data;
static const EpdFontData& cjk12Face() { return externalCjk12Data; }
#else
static const EpdFontData& cjk12Face() { return notosans_cjk_12; }
#endif
EpdFont offlineReaderFont(&cjk12Face());
EpdFontFamily offlineReaderFontFamily(&offlineReaderFont);

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
// Embedded Song serif for page titles (~48px em): Noto Serif SC (OFL) subset
// over lib/EpdFont/scripts/cn_title_chars.txt, rebuilt by
// build-title-serif-font.sh. ReadPico only — the size does not fit C3.
#if defined(CROSSPOINT_EXTERNAL_FONTS)
static EpdFontData externalTitleData;
static const EpdFontData& titleFace() { return externalTitleData; }
#else
static const EpdFontData& titleFace() { return notoserif_title; }
#endif
EpdFont titleSerifFont(&titleFace());
EpdFontFamily titleSerifFamily(&titleSerifFont);
#endif

void useBuiltinFontFallback() {
#if defined(CROSSPOINT_EXTERNAL_FONTS) && defined(CROSSMUX_UI_PROFILE_HIGH_DPI)
  externalCjk12Data = notosans_cjk_14;
  externalTitleData = notosans_cjk_14;
#endif
}

void refreshExternalFontFaces() {
#if defined(CROSSPOINT_EXTERNAL_FONTS)
  EpdFontData extCjk12;
  EpdFontData extTitle;
  if (loadExternalFontPack(extCjk12, extTitle)) {
    externalCjk12Data = extCjk12;
#if defined(CROSSMUX_UI_PROFILE_HIGH_DPI)
    externalTitleData = extTitle;
#endif
  } else {
    useBuiltinFontFallback();
  }
#endif
}

// Large UI text keeps the upstream face; INX restores its historical fallback
// through the theme reload hook; Ubuntu UI families are shared by all themes.
extern EpdFont ui18RegularFont;
extern EpdFont ui18BoldFont;
EpdFontFamily ui18FontFamily(&ui18RegularFont, &ui18BoldFont);

extern EpdFontFamily control18FontFamily;

// International UI fonts remain primary; CJK subsets are selected only when
// the primary is missing a Han glyph.
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
EpdFont smallFont(&notosans_12_regular);
#else
EpdFont smallFont(&notosans_8_regular);
#endif
EpdFontFamily smallFontFamily(&smallFont);

extern EpdFont ui10RegularFont;
extern EpdFont ui10BoldFont;
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

extern EpdFont ui12RegularFont;
extern EpdFont ui12BoldFont;
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
// Size-matched CJK faces for the UI, with the wide 12pt face behind them.
// The 14pt/16pt subsets carry only the i18n vocabulary (803 glyphs), so UI
// copy keeps the size the theme was designed around; a string containing a
// character outside that vocabulary falls through to the GB2312 12pt face
// (6,879 glyphs) instead of drawing tofu. applyUiCjkFallbacks() wires the
// chain, and SETTINGS.uiDensity picks this chain or the all-12pt compact one.
EpdFontFamily cjk8FontFamily(&offlineReaderFont);
EpdFont cjk14Font(&notosans_cjk_14);
EpdFont cjk16Font(&notosans_cjk_16);
EpdFontFamily cjk10FontFamily(&cjk14Font);
EpdFontFamily ui14FallbackFamily(&cjk14Font);
EpdFontFamily ui16FallbackFamily(&cjk16Font);
static EpdFont rtlRegularFont(&ubuntu_12_regular);
static EpdFont rtlBoldFont(&ubuntu_12_bold);
static EpdFontFamily rtlFontFamily(&rtlRegularFont, &rtlBoldFont);
static constexpr int kRtlFontId = 0x52544C0C;
#else
EpdFont cjk8Font(&notosans_cjk_8);
EpdFont cjk10Font(&notosans_cjk_10);
EpdFontFamily cjk8FontFamily(&cjk8Font);
EpdFontFamily cjk10FontFamily(&cjk10Font);
#endif
EpdFont cjk12Font(&cjk12Face());
EpdFontFamily cjk12FontFamily(&cjk12Font);

// Chinese chess piece glyphs (subset CJK font, 14 characters at 16pt).
EpdFont chineseChessPieceFont(&chinese_chess_16);
EpdFontFamily chineseChessPieceFontFamily(&chineseChessPieceFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootFontPointSize;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
enum class SilentRebootTarget : uint32_t {
  Home,
  Reader,
  ReaderSuppressFontPrompt,
  ReaderPreloadChineseFont,
  Settings,
  JoinNetwork,
  Count,
};
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

#if FREEINK_CAP_TOUCH
#if FREEINK_DEVICE_READPICO
// Defer font I/O until after activity teardown releases its render lock.
static bool sdFontReloadPending = false;
#endif

static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;
  if (esp_sntp_enabled()) esp_sntp_stop();
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
#if FREEINK_DEVICE_READPICO
  sdFontReloadPending = true;
#endif
  return true;
}
#endif

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::Home);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Returns instead of rebooting when sleep supersedes the reboot; callers keep
// running in that case.

void silentRestartToReader(const bool suppressChineseFontPrompt) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = static_cast<uint32_t>(suppressChineseFontPrompt ? SilentRebootTarget::ReaderSuppressFontPrompt
                                                                       : SilentRebootTarget::Reader);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader%s)", suppressChineseFontPrompt ? ", suppress-font-prompt" : "");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReaderAndPreloadChineseFont(const uint8_t pointSize) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::ReaderPreloadChineseFont);
  silentRebootFontPointSize = pointSize;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader-preload-font, size=%u)", static_cast<unsigned>(pointSize));
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToSettings() {
  if (deepSleepInProgress) return;
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::Settings);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToJoinNetwork() {
  if (deepSleepInProgress) return;
#if FREEINK_CAP_TOUCH
  // A software reset would cycle touch/frontlight rails; those boards proceed
  // into Join Network without the fresh-heap reboot (return, don't stop WiFi —
  // this runs on the way *in*, unlike the exit-time silentRestart()).
  if (BoardConfig::hasTouch()) return;
#endif
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::JoinNetwork);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=join-network)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage handoff reboot
  silentRebootTarget = static_cast<uint32_t>(SilentRebootTarget::Home);
  silentRebootFontPointSize = 0;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

void toggleFrontlight() {
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }
  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  toggleFrontlight();
  return true;
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Plugin-event delivery on the way into deep sleep. sleep.enter is delivered
// now — over the live connection, or by bringing WiFi up when a plugin
// subscribes (e.g. fetching a fresh /sleep.bmp so THIS sleep shows it — the
// drain runs before goToSleep() renders the sleep screen). The connect path
// is bounded (join deadline + drain event budget), skipped on low battery,
// and sleep is never blocked on the network: a failed join or delivery just
// sleeps with the previous image and the queued events retry on the next
// drain (at-least-once). The caller's WiFi shutdown tears the radio down
// either way. Deferrable events already queued (reader.exit) ride along in
// the same drain.
static void deliverSleepPluginEvents() {
  // Activity-owned state must be queued before sleep.enter and before this
  // same-sleep drain. The hook is idempotent with ordinary activity teardown.
  activityManager.prepareForSleep();

  // Sleeping straight out of a book is the common flow, but the reader's own
  // reader.exit only fires later, inside goToSleep() — after this drain. Carry
  // the book and progress on sleep.enter itself so a sync handler bound to it
  // pushes current progress on THIS connection, not the next one.
  pluginevents::Var vars[2];
  size_t varCount = 0;
  char percent[8];
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  if (info.readerType != ScreenshotInfo::ReaderType::None && !APP_STATE.openEpubPath.empty()) {
    snprintf(percent, sizeof(percent), "%d", info.progressPercent);
    vars[varCount++] = {"book", APP_STATE.openEpubPath.c_str()};
    vars[varCount++] = {"percent", percent};
  }
  pluginevents::emit(pluginevents::Event::SleepEnter, vars, varCount);
  if (WiFi.status() == WL_CONNECTED) {
    pluginevents::drain(&renderer);
    return;
  }
  // Any connect-flagged queued event justifies the join, not only
  // sleep.enter: reader.session is queued while reading and delivered on this
  // same sleep, and a progress-sync plugin usually subscribes to it alone.
  if (!pluginevents::wantsConnectAny()) return;
  if (powerManager.getBatteryPercentage() < 20) return;
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) return;

  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long joinDeadline = millis() + 10000;
  while (WiFi.status() != WL_CONNECTED && millis() < joinDeadline) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    trustedtime::startSync();  // snap the clock floor while the network is up
    pluginevents::drain(&renderer);
  } else {
    LOG_DBG("MAIN", "Sleep-event WiFi join timed out; deferring delivery");
  }
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  bleinput::stop();
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // Sleep may end in a power-off (battery death, latch); persist the clock
  // floor now so a later cold boot resumes from it.
  trustedtime::note();

  deliverSleepPluginEvents();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (!READING_STATS.saveToFile()) {
    LOG_ERR("RST", "Failed to save reading stats before deep sleep");
  }
  if (!ACHIEVEMENTS.saveToFile()) {
    LOG_ERR("ACH", "Failed to save achievements before deep sleep");
  }

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

#if FREEINK_CAP_HAPTIC
  gpio.stopHapticFeedback();
#endif
  halTiltSensor.deepSleep();
  Frontlight.setOn(false);
#if CROSSPOINT_CAP_SOUND_FEEDBACK
  SoundFeedback::shutdown();
#endif
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
  BuzzerFeedback::shutdown();
#endif
  display.deepSleep();
#if !FREEINK_DEVICE_EEGO_A4
  Storage.prepareForDeepSleep();
#endif
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

bool setupDisplayAndFonts(bool seamless = false, bool logSdFontLoadHeap = false) {
#if defined(CROSSPOINT_EXTERNAL_FONTS)
  {
    EpdFontData extCjk12;
    EpdFontData extTitle;
    if (loadExternalFontPack(extCjk12, extTitle)) {
      externalCjk12Data = extCjk12;
#if defined(CROSSMUX_UI_PROFILE_HIGH_DPI)
      externalTitleData = extTitle;
#endif
    }
  }
#endif
#if FREEINK_DEVICE_X4PRO
  // X4 Pro batches use SSD1677 or UC81xx. Resolve the controller before
  // display.begin(); C3 X3/X4 already do this once in HalGPIO::begin().
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    freeink::applyXteinkDisplayController();
  }
#endif

  display.begin(seamless);
#if FREEINK_DEVICE_READPICO
  // Read Pico step 4 of 5 (see the boot-order note in setup()): the panel has
  // stepped through its own rails by now — BoardReadPico::epdPrepare/On/Off are
  // the board's LgfxEpdPowerHooks, so the SY7636A enable (FCA9555 P0.3), VCOM and
  // the PGOOD wait all ran inside display.begin(). Only now does the CST836U
  // probe go out on the shared 400 kHz bus, which is what HalGPIO::begin()
  // deferred. Idempotent, so any other path that initialises the panel is safe.
  gpio.beginInput();
#endif
#if FREEINK_DEVICE_MURPHY_M4
  if (!gpio.restoreTouchAfterDisplayReset()) {
    LOG_ERR("MAIN", "Failed to restore Murphy M4 touch after display reset");
  }
#endif
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  const bool fontDecompressorReady = fontDecompressor.init();
  if (!fontDecompressorReady) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, offlineReaderFontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, offlineReaderFontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, offlineReaderFontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, ui18FontFamily);
#endif  // OMIT_FONTS
  // The fixed slider title font keeps its face while INX uses its historical large-text fallback.
  renderer.insertFont(CONTROL_18_FONT_ID, control18FontFamily);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(CJK_UI_8_FONT_ID, cjk8FontFamily);
  renderer.insertFont(CJK_UI_10_FONT_ID, cjk10FontFamily);
  renderer.insertFont(CJK_UI_12_FONT_ID, cjk12FontFamily);
  renderer.setFallbackFont(SMALL_FONT_ID, CJK_UI_8_FONT_ID);
  renderer.setFallbackFont(CONTROL_18_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(UI_10_FONT_ID, CJK_UI_10_FONT_ID);
  renderer.setFallbackFont(UI_12_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.setFallbackFont(NOTOSANS_18_FONT_ID, CJK_UI_12_FONT_ID);
  renderer.insertFont(BaseTheme::STATUS_NUMERIC_FONT_ID, smallFontFamily);
#if FREEINK_DEVICE_READPICO
  renderer.insertFont(READER_STATUS_FONT_ID, smallFontFamily);
  renderer.insertFont(READER_ESTIMATE_FONT_ID, ui10FontFamily);
  renderer.setFallbackFont(READER_STATUS_FONT_ID, CJK_UI_8_FONT_ID);
  renderer.setFallbackFont(READER_ESTIMATE_FONT_ID, CJK_UI_10_FONT_ID);
#endif
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  renderer.insertFont(UiHighDpiProfile::reader12FontId, offlineReaderFontFamily);
  renderer.insertFont(CJK_UI_14_FONT_ID, ui14FallbackFamily);
  renderer.insertFont(CJK_UI_16_FONT_ID, ui16FallbackFamily);
  // The CJK size policy lives in one place: comfortable = size-matched 14/16pt
  // for UI copy with the wide 12pt face behind, compact = all 12pt. Driven by
  // SETTINGS.uiDensity, so there is no second "Chinese size" setting.
  applyUiCjkFallbacks(renderer, SETTINGS.uiDensity != 0);
  renderer.insertFont(kRtlFontId, rtlFontFamily);
  renderer.setFallbackFont(CJK_UI_12_FONT_ID, kRtlFontId);
  // Title serif: registered here so the whole-run fallback resolves too — a
  // title outside the subset's coverage (non-CJK scripts) renders in the
  // standard UI title face rather than dropping glyphs.
  renderer.insertFont(NOTOSERIF_TITLE_FONT_ID, titleSerifFamily);
  renderer.setFallbackFont(NOTOSERIF_TITLE_FONT_ID, UI_12_FONT_ID);
#endif
  renderer.insertFont(CHINESE_CHESS_FONT_ID, chineseChessPieceFontFamily);

  // Discover and load SD card fonts
  if (logSdFontLoadHeap) {
    LOG_INF("FONT", "Clean restart before font load: free=%u, maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  sdFontSystem.begin(renderer);
  // Bundled mono emoji face (data partition, OFL Noto Emoji subset). Failure is
  // non-fatal: no emoji pack simply leaves the old tofu behaviour.
  emoji_font::begin(renderer, SETTINGS.fontPointSize);

  LOG_DBG("MAIN", "Fonts setup");
  return fontDecompressorReady;
}

#ifdef ENABLE_CHINESE_VERSION
void continueChineseFontInstall(const uint8_t expectedPointSize) {
  if (APP_STATE.openEpubPath.empty() || !Storage.exists(APP_STATE.openEpubPath.c_str())) {
    LOG_ERR("FONT", "Cannot resume automatic font install: original EPUB is unavailable");
    activityManager.goHome();
    return;
  }

  const bool fontReady =
      strcmp(SETTINGS.sdFontFamilyName, SdCardFontSystem::COMPLETE_CHINESE_NOTO_SANS_FAMILY) == 0 &&
      SETTINGS.fontPointSize == expectedPointSize &&
      sdFontSystem.resolveFontId(SdCardFontSystem::COMPLETE_CHINESE_NOTO_SANS_FAMILY, expectedPointSize) != 0;
  if (!fontReady) {
    LOG_ERR("FONT", "Failed to load selected family %s at point size %u after clean restart",
            SdCardFontSystem::COMPLETE_CHINESE_NOTO_SANS_FAMILY, static_cast<unsigned>(expectedPointSize));
    SETTINGS.clearSdFontFamily();
    SETTINGS.fontPointSize = expectedPointSize;
    if (!SETTINGS.saveToFile()) LOG_ERR("FONT", "Failed to restore reader point size after font load failure");
  }

  activityManager.goToReader(APP_STATE.openEpubPath);
  activityManager.loop();
  if (!activityManager.isReaderActivity()) {
    LOG_ERR("FONT", "Cannot resume automatic font install: reader allocation failed");
    activityManager.goHome();
    return;
  }

  if (!fontReady) {
    auto error = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager,
                                                         FontDownloadActivity::Purpose::ReaderAutoInstall,
                                                         FontDownloadActivity::StartMode::ResumeFontLoadError);
    if (!error) {
      LOG_ERR("FONT", "OOM allocating resumed FontDownloadActivity (%zu bytes)", sizeof(FontDownloadActivity));
      return;
    }
    activityManager.pushActivity(std::move(error));
    activityManager.loop();
    return;
  }

  auto textSettings = makeUniqueNoThrow<TextSettingsActivity>(
      renderer, mappedInputManager, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family,
      TextSettingsActivity::InitialFontState::Changed, TextSettingsActivity::StartMode::AskThenExit);
  if (textSettings) {
    activityManager.pushActivity(std::move(textSettings));
    activityManager.loop();
    return;
  }

  LOG_ERR("FONT", "OOM allocating automatic TextSettingsActivity (%zu bytes)", sizeof(TextSettingsActivity));
  SETTINGS.sdFontFlashPreload = 0;
  if (!SETTINGS.saveToFile()) LOG_ERR("FONT", "Failed to persist disabled font preload after OOM");

  auto notice = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInputManager, "", tr(STR_FONT_PRELOAD_FAILED),
                                                        ConfirmationActivity::BodyPlacement::PopupTitle);
  if (!notice) {
    LOG_ERR("FONT", "OOM allocating font preload failure notice (%zu bytes)", sizeof(ConfirmationActivity));
    return;
  }
  activityManager.pushActivity(std::move(notice));
  activityManager.loop();
}
#endif

void setup() {
  // Stage timing starts before the board bring-up it is meant to measure.
  boottimings::begin();
  BoardConfig::holdPowerRails();

  // --- Read Pico boot order (read-pico.md 1.4) ------------------------------
  // This board's bring-up is ordered by its power tree, and the reference
  // firmware uses the same order (read_pico_init.c: board I2C + expander and the
  // touch reset first, then the EPD, then the accelerometer/touch drivers):
  //
  //   1. shared I2C, SDA39/SCL40 at 400 kHz
  //   2. FCA9555 expander + PMU handshake + CST836U reset pulse
  //   3. SY7636A EPD rails, then the panel itself
  //   4. touch backend (and the IMU, which this board does not use)
  //   5. SD card
  //
  // 1-2 is gpio.begin() below (BoardReadPico::begin()): nothing downstream can
  // be sequenced before the expander answers, because it enables the SY7636A
  // (P0.3), drives XOE (P0.1), gates the touch reset (P0.7), senses PGOOD (P0.5)
  // and carries the card detect (P0.6). 3-4 is setupDisplayAndFonts(): the
  // SY7636A sequence is the board's LgfxEpdPowerHooks inside display.begin(),
  // and gpio.beginInput() brings the CST836U up immediately after it.
  //
  // 5 (SD) deliberately stays where it is, ahead of the panel: storage is a
  // boot-time dependency in CrossMux — SETTINGS/APP_STATE/reading state are read
  // from it before any activity exists, and the SD-failure path itself paints a
  // screen — and this board's SDMMC pins (CLK38/CMD42/D0=44) share nothing with
  // the shared I2C bus (39/40) or the EPD bus (3..21, 45..48), so mounting late
  // would reorder shared setup code for every target with no hardware reason.
  // The board only needs the expander to be up first, which gpio.begin() does.
  //
  // Every step degrades instead of aborting: a dead expander leaves the panel
  // dark and the keys dead (logged), a missing card shows the SD error screen,
  // and a failed PMU handoff leaves battery/RTC/power-off reporting unknown.
#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
#if !defined(CROSSPOINT_EMBED_HYPHENATION)
  // The hyphenation loader reads the data partition.  Warm every trie here, on
  // the main task, before the PSRAM-stack render task exists; otherwise loading
  // a TXT/PDF section trips esp_task_stack_is_sane_cache_disabled().
  hyphenation::HyphenationStore::shared().preloadAll();
#endif
#if !defined(SIMULATOR)
  {
    // NVS is internal flash; a PSRAM-stack task cannot read it (IDF asserts).
    // Warm the cached secret here, on the main task, before the library /
    // thumbnail scan can reach BookKey -> deviceSecret from the render task.
    // The first protected EPUB open otherwise reboot-loops at boot.
    uint8_t warmSecret[32];
    if (!deviceSecret(warmSecret)) LOG_ERR("DSEC", "device secret preload failed");
  }
#endif
  const bool otaPendingAtBoot = HalOtaSlot::runningImageState() == HalOtaSlot::RunningImageState::PendingVerify;

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const bool targetIsValid = isSilentReboot && silentRebootTarget < static_cast<uint32_t>(SilentRebootTarget::Count);
  SilentRebootTarget snapshotTarget =
      targetIsValid ? static_cast<SilentRebootTarget>(silentRebootTarget) : SilentRebootTarget::Home;
  const bool fontPointSizeIsValid = snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont &&
                                    silentRebootFontPointSize > 0 && silentRebootFontPointSize <= UINT8_MAX;
  const uint8_t snapshotFontPointSize =
      snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont && fontPointSizeIsValid
          ? static_cast<uint8_t>(silentRebootFontPointSize)
          : 0;
  if (snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont && snapshotFontPointSize == 0) {
    snapshotTarget = SilentRebootTarget::Home;
  }
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootFontPointSize = 0;
#ifdef ENABLE_CHINESE_VERSION
  if (snapshotTarget == SilentRebootTarget::ReaderSuppressFontPrompt ||
      snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont) {
    FontDownloadActivity::suppressChineseFontPromptThisBoot();
  }
#endif
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootPayload = 0;

  // On Read Pico this pair is steps 1-2 of the boot order above: gpio.begin()
  // runs BoardReadPico::begin() (shared I2C, FCA9555 self-test + Port-0 config,
  // CST836U reset pulse, PMU handshake, accelerometer identity probe) before
  // anything else on that board is touched, and it deliberately leaves that
  // board's input backends unstarted. powerManager.begin() installs the same
  // board's PMU host-shutdown hook, so it has to follow. Every other target is
  // unchanged.
  gpio.begin();
  powerManager.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
#if !FREEINK_DEVICE_EEGO_A4
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.verifyPowerButtonWakeup();
#endif

  // IMU bring-up on every target. Read Pico's SC7A20H is an accelerometer with no
  // gyroscope, so HalTiltSensor differentiates the gravity component for the tilt
  // page-turn gesture instead of reading an angular rate; the thresholds are converted
  // from the same 270 dps. halClock.begin() restores the system clock from the board RTC
  // when the system clock is invalid; Read Pico's RTC is the PMU, whose hooks
  // BoardReadPico::begin() already installed.
  halTiltSensor.begin();
  halClock.begin();
  boottimings::mark(boottimings::Stage::Board);

  LOG_INF("MAIN", "Hardware detect: %s", BoardConfig::ACTIVE.name);

  bool recoveryFirmwareMode = false;
#if !FREEINK_DEVICE_PAPERMONO
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    const uint8_t recoveryButton =
        (BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC) ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
    recoveryFirmwareMode = gpio.isPressed(recoveryButton);
    if (recoveryFirmwareMode) {
      LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
              (BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC) ? "DOWN" : "UP");
    }
  }
#endif

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  // Read Pico step 5: 1-bit SDMMC, mounted by attempt. The FCA9555 card-detect
  // line (P0.6) is a hint for the failure log only and never a mount gate
  // (read-pico.md B11), so a card that the expander misreads still mounts, and an
  // unreadable card lands here — the error screen below is the controlled path,
  // nothing aborts.
  const bool storageReady = Storage.begin();
  boottimings::mark(boottimings::Stage::SdCard);
  if (!storageReady) {
    LOG_ERR("MAIN", "SD card initialization failed");
    // The SD write will fail, but checkPanic() also prints the report to the
    // serial console; returning here without it lost the panic from the log.
    HalSystem::checkPanic();
    const bool fontsReady = setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD,
                                         "Card missing or unreadable. Reinsert it, then retry.", true);
    activityManager.requestUpdateAndWait();
    if (otaPendingAtBoot && fontsReady) {
      if (HalOtaSlot::confirmRunningImage()) {
        LOG_INF("OTA", "Running image confirmed after SD error display");
      } else {
        LOG_ERR("OTA", "Running image confirmation failed after SD error display");
      }
    }
    return;
  }

  HalSystem::checkPanic();

  const bool settingsLoaded = SETTINGS.loadFromFile();
#if FREEINK_DEVICE_READPICO
  if (!settingsLoaded) SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
#endif
  const auto onboardingMode =
      settingsLoaded ? LanguageSelectActivity::Mode::Upgrade : LanguageSelectActivity::Mode::Initial;
  const bool requiresOnboarding = CrossPointSettings::requiresOnboarding(SETTINGS.onboardingVersion);
#ifndef SIMULATOR
  halClock.setUseChinaServers(SETTINGS.contentProfile == CrossPointSettings::ContentProfile::China);
#endif
  halClock.setAutoSyncEnabled(SETTINGS.clockAutoSync != 0);
  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  timezones::applyToClock();
  const bool recentsLoaded = RECENT_BOOKS.loadFromFile();
  if (!recoveryFirmwareMode && !HalSystem::isRebootFromPanic()) UserGuide::prepare(recentsLoaded);
  READING_STATS.loadFromFile();
  ACHIEVEMENTS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  // PSRAM targets rebuild the per-language offset tables here, before any UI
  // calls tr(). No-op on targets that keep the tables in flash.
  I18N.init();
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  pluginevents::refreshSubscriptions();
  // Restore the monotonic clock floor before anything reads time() (event
  // timestamps, loan-expiry checks).
  trustedtime::init();
  boottimings::mark(boottimings::Stage::Stores);

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference.
  const bool restoreLightOn =
      isSilentReboot ? silentRebootLightOn : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
#if FREEINK_DEVICE_EEGO_A4
      LOG_DBG("MAIN", "Verifying power button press duration");
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
        powerManager.startDeepSleep(gpio);
      }
#else
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
#endif
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4 || \
    FREEINK_DEVICE_WAVESHARE_EPAPER_397 || FREEINK_DEVICE_METALIO_EINK4 || FREEINK_DEVICE_READPICO
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Waveshare 3.97 hits both: its side key is
      // behind the AXP2101 (input.power == PIN_UNASSIGNED) and it is a native-USB
      // S3, so startDeepSleep() there is a PMIC shutdown on every cabled boot.
      // Read Pico is the same class of device and would be worse: its power key is
      // PMU-owned (input.power == PIN_UNASSIGNED), it is a native-USB S3 whose
      // console is USB Serial/JTAG, and its "off" is a PMU host shutdown whose only
      // wake sources are the PMU key / AC-in / RTC alarm — so a cabled boot with a
      // charging battery (isUsbConnected() reads the PMU charge state) would drop
      // the EN rail immediately and look like a boot loop to the user.
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;

  const bool fontsReady = setupDisplayAndFonts(resume != BootResume::Splash,
                                               snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont);
  boottimings::mark(boottimings::Stage::Panel);
  const bool postOtaBoot = otaPendingAtBoot && fontsReady && activityManager.goToPostOtaBoot(!recoveryFirmwareMode);

  if (!postOtaBoot) {
    switch (resume) {
      case BootResume::Silent:
        // Splash skipped: the routing block below picks the target activity; the
        // panel keeps showing the pre-reboot popup until that first paint lands.
        break;
      case BootResume::SplashlessWake:
        // One-shot flag: re-arm the splash for the next ordinary boot. Save
        // before any painting so a hang in the blocking paint path can't strand
        // us in a splashless-with-no-frame loop on the next boot.
        APP_STATE.showBootScreen = true;
        APP_STATE.saveToFile();
        if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
          if (gpio.deviceIsX3()) {
            // begin() clears the X3 controller RAM, so restore the saved frame as
            // the baseline for the first reader paint without refreshing the panel.
            renderer.cleanupGrayscaleWithFrameBuffer();
            allowFastInitialReaderRefresh = true;
          }
        } else {
          // Clean the retained sleep image as part of the first Home paint.
          needsWakeRefresh = true;
        }
        break;
      case BootResume::Splash:
        activityManager.goToBoot();
        break;
    }
    // "First frame requested" is the last thing setup() owes the user; the
    // render task stamps the physical paint on top of it.
    boottimings::mark(boottimings::Stage::Splash);
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivityWith<SdFirmwareUpdateActivity>(/*recoveryMode=*/true);
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (postOtaBoot) {
    if (requiresOnboarding) {
      activityManager.replaceActivityWith<LanguageSelectActivity>(onboardingMode);
    } else {
      activityManager.goHome();
    }
  } else if (requiresOnboarding) {
    activityManager.replaceActivityWith<LanguageSelectActivity>(onboardingMode);
  } else if (resume == BootResume::Silent && snapshotTarget == SilentRebootTarget::ReaderPreloadChineseFont) {
#ifdef ENABLE_CHINESE_VERSION
    continueChineseFontInstall(snapshotFontPointSize);
#else
    activityManager.goHome();
#endif
  } else if (resume == BootResume::Silent &&
             (snapshotTarget == SilentRebootTarget::Reader ||
              snapshotTarget == SilentRebootTarget::ReaderSuppressFontPrompt) &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SilentRebootTarget::JoinNetwork) {
    activityManager.goToJoinNetwork();
  } else if (resume == BootResume::Silent && snapshotTarget == SilentRebootTarget::Settings) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings();
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.readerActivityLoadCount > 0 && !APP_STATE.openEpubPath.empty()) {
    // A previous boot died while opening this book (docs/07 D8). Say so and let
    // the user choose, instead of the silent fall-through to Home below.
    activityManager.replaceActivityWith<ReaderRecoveryActivity>();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    if (needsWakeRefresh) renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    // Splashless wake leaves the retained sleep frame on the panel; without a
    // clean first paint the reader shows the previous screen's residue (A4
    // grayscale panels ghost worst). Mirror the Home branch's HALF refresh so
    // reader resume also clears the retained frame.
    if (needsWakeRefresh) renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    if (postOtaBoot) {
      // Apply the queued Home replacement before waiting for its first physical paint.
      activityManager.loop();
    }
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

#if FREEINK_CAP_TOUCH && FREEINK_DEVICE_READPICO
  if (sdFontReloadPending) {
    sdFontReloadPending = false;
    const unsigned long reloadStart = millis();
    LOG_DBG("MAIN", "Reloading SD font family after the network session");
    {
      // The independent render task shares the font maps and resident font objects.
      RenderLock lock;
      sdFontSystem.ensureLoaded(renderer);
    }
    LOG_DBG("MAIN", "SD font reload after network took %lu ms", millis() - reloadStart);
    activityManager.requestUpdate();
  }
#endif

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();
#if FREEINK_DEVICE_READPICO
  // Cancel before any handler can wait for the framebuffer lock. These are
  // snapshot queries: they do not consume the gesture from its normal owner.
#if CROSSPOINT_EMULATED
  const bool physicalPress = gpio.wasAnyPressed();
#else
  const bool physicalPress = gpio.physicalPressedMask() != 0;
#endif
  if (mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased() || physicalPress ||
      gpio.wasTouchActivity()) {
    activityManager.cancelIdleRender();
  }
#endif
#if FREEINK_CAP_HAPTIC
  gpio.updateHapticFeedback(SETTINGS.hapticFeedbackLevel);
#endif
  updateBluetoothLifecycle();

  static bool bluetoothWasConnected = false;
  const bool bluetoothConnected = bleinput::isConnected();
  if (bluetoothConnected != bluetoothWasConnected) {
    bluetoothWasConnected = bluetoothConnected;
    bleinput::logDiagnostics(bluetoothConnected ? "connected" : "disconnected");
    if (activityManager.isReaderActivity()) activityManager.requestUpdate();
  }

#if CROSSPOINT_CAP_SOUND_FEEDBACK
  SoundFeedback::update(SETTINGS.soundFeedbackLevel, gpio.physicalPressedMask());
#endif
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
  BuzzerFeedback::update();
  if (SETTINGS.buzzerFeedbackSource != CrossPointSettings::BUZZER_FEEDBACK_OFF) {
    const bool screenCue = SETTINGS.buzzerFeedbackSource == CrossPointSettings::BUZZER_FEEDBACK_SCREEN ||
                           SETTINGS.buzzerFeedbackSource == CrossPointSettings::BUZZER_FEEDBACK_BOTH;
    const bool keysCue = SETTINGS.buzzerFeedbackSource == CrossPointSettings::BUZZER_FEEDBACK_KEYS ||
                         SETTINGS.buzzerFeedbackSource == CrossPointSettings::BUZZER_FEEDBACK_BOTH;
    float nx = 0.0f;
    float ny = 0.0f;
    bool cue = screenCue && gpio.wasTouchTap(nx, ny);
    if (!cue && keysCue) {
      // Read Pico's three capacitive zones surface as these logical buttons
      // (HalGPIO.cpp); the touch panel and the zones are mutually exclusive.
      cue = gpio.wasPressed(HalGPIO::BTN_UP) || gpio.wasPressed(HalGPIO::BTN_BACK) ||
            gpio.wasPressed(HalGPIO::BTN_DOWN);
    }
    if (cue) {
      BuzzerFeedback::cue(static_cast<BuzzerFeedback::Pitch>(SETTINGS.buzzerPitch), SETTINGS.buzzerStrength);
    }
  }
#endif

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  halTiltSensor.update(SETTINGS.motionPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());
  halClock.update();

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setUiFullRefreshInterval(SETTINGS.getUiRefreshInterval());

  // The ROM console does not depend on Arduino USB CDC's connection state.
  if ((Serial || FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF) && millis() - lastMemPrint >= 10000) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes, RenderStkMin: %u bytes",
            heap.freeBytes, heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes,
            static_cast<unsigned>(activityManager.renderStackMinFreeBytes()));
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    if (bleinput::isRunning()) bleinput::logDiagnostics("running");
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      } else if (cmd == "FIND") {
        activityManager.pushActivity(std::make_unique<FindBooksActivity>(renderer, mappedInputManager));
      } else if (cmd == "MY") {
        activityManager.pushActivity(std::make_unique<MyActivity>(renderer, mappedInputManager));
      } else if (cmd == "LIBRARY") {
        activityManager.goToLibrary();
      } else if (cmd == "HOME") {
        activityManager.goHome();
      } else if (cmd == "SEARCH") {
        activityManager.pushActivity(std::make_unique<LibraryListActivity>(renderer, mappedInputManager));
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugOpenSearch();
      } else if (cmd == "SETSEARCH") {
        activityManager.pushActivity(std::make_unique<SettingsSearchActivity>(
            renderer, mappedInputManager, "\xE5\xAD\x97\xE4\xBD\x93"));
      } else if (cmd == "SEARCHSAMPLE") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugOpenSearchSample();
      } else if (cmd == "EMPTY") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugForceEmpty();
      } else if (cmd == "LIBSNAP") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugRequestSnapshot();
      } else if (cmd == "LIBSEND") {
        size_t snapSize = 0;
        const uint8_t* snap = LibraryListActivity::debugSnapshotData(snapSize);
        if (snap != nullptr && snapSize > 0) {
          logSerial.printf("LIBSNAP_START:%u\n", static_cast<unsigned>(snapSize));
          logSerial.write(snap, snapSize);
          logSerial.printf("LIBSNAP_END\n");
          LibraryListActivity::debugClearSnapshot();
        } else {
          logSerial.printf("LIBSNAP_EMPTY\n");
        }
      } else if (cmd == "HINTRESET") {
        SETTINGS.readerGestureHintSeen = 0;
        SETTINGS.saveToFile();
      } else if (cmd == "HINT") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugDismissHint();
      } else if (cmd.startsWith("TAB")) {
        if (auto* activity = activityManager.currentActivityPtr()) {
          const int space = cmd.indexOf(' ');
          activity->debugSetTab(space > 0 ? cmd.substring(space + 1).toInt() : 0);
        }
      } else if (cmd.startsWith("TEXT")) {
        activityManager.pushActivity(makeUniqueNoThrow<TextSettingsActivity>(
            renderer, mappedInputManager, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family));
      } else if (cmd.startsWith("SETTINGS")) {
        int category = 0;
        const int space = cmd.indexOf(' ');
        if (space > 0) category = cmd.substring(space + 1).toInt();
        activityManager.pushActivity(
            std::make_unique<SettingsActivity>(renderer, mappedInputManager, category, /*subpage=*/true));
      } else if (cmd.startsWith("OPENROW")) {
        if (auto* activity = activityManager.currentActivityPtr()) {
          const int space = cmd.indexOf(' ');
          activity->debugActivateContentsRow(space > 0 ? cmd.substring(space + 1).toInt() : 0);
        }
      } else if (cmd.startsWith("FILTER")) {
        if (auto* activity = activityManager.currentActivityPtr()) {
          const int space = cmd.indexOf(' ');
          activity->debugSetContentsFilter(space > 0 ? cmd.substring(space + 1).toInt() : 1);
        }
      } else if (cmd.startsWith("PANEL")) {
        if (auto* activity = activityManager.currentActivityPtr()) {
          const int space = cmd.indexOf(' ');
          activity->debugOpenPanel(space > 0 ? cmd.substring(space + 1).toInt() : 1);
        }
      } else if (cmd.startsWith("READ")) {
        String path = "/BOOKS/test.epub";
        const int space = cmd.indexOf(' ');
        if (space > 0) path = cmd.substring(space + 1);
        activityManager.pushActivity(std::make_unique<EpubReaderActivity>(
            renderer, mappedInputManager, std::string(path.c_str()), false));
      } else if (cmd == "STATS") {
        activityManager.goToReadingStatsMenu();
      } else if (cmd == "TRANSFER") {
        activityManager.goToFileTransfer();
      } else if (cmd == "JJWXC") {
        activityManager.goToJjwxc();
      } else if (cmd.startsWith("DETAIL")) {
#ifdef BOARD_HAS_PSRAM
        const int space = cmd.indexOf(' ');
        const String id = space > 0 ? cmd.substring(space + 1) : String();
        if (id.isEmpty()) {
          LOG_ERR("JJWXC", "serial CMD:DETAIL needs a novel id");
        } else {
          JjwxcActivity::requestDeepLink(id.c_str(), JjwxcActivity::DeepLinkAction::OpenBookDetail);
          activityManager.goToJjwxc();
        }
#endif
      } else if (cmd == "JJSEARCH") {
#ifdef BOARD_HAS_PSRAM
        JjwxcActivity::requestSearchDeepLink("\xE6\x88\x91");
        activityManager.goToJjwxc();
#endif
      } else if (cmd == "WIFI") {
        activityManager.pushActivity(std::make_unique<WifiSelectionActivity>(renderer, mappedInputManager, false));
      } else if (cmd == "SLEEP") {
        activityManager.goToSleep(false);
      } else if (cmd == "OTAPROBE") {
        // Diagnostics only: fetch the release manifest, then measure every OTA
        // download route and print the winner. Requires an active Wi-Fi link.
        auto updater = makeUniqueNoThrow<OtaUpdater>();
        if (!updater) {
          logSerial.printf("OTAPROBE_ERR oom\n");
        } else {
          const auto result = updater->checkForUpdate(OtaUpdater::Channel::Stable);
          logSerial.printf("OTAPROBE check=%d\n", static_cast<int>(result));
          if (result == OtaUpdater::OK) updater->debugProbeDownloadMirrors();
        }
      } else if (cmd.startsWith("CAT ")) {
        String path = cmd.substring(4);
        path.trim();
        HalFile f;
        if (!Storage.openFileForRead("DBG", path.c_str(), f)) {
          logSerial.printf("CAT_ERR open %s\n", path.c_str());
        } else {
          logSerial.printf("CAT_START:%s:%u\n", path.c_str(), static_cast<unsigned>(f.size()));
          uint8_t buf[512];
          size_t total = 0;
          while (total < 16384) {
            const int n = f.read(buf, sizeof(buf));
            if (n <= 0) break;
            logSerial.write(buf, n);
            total += static_cast<size_t>(n);
          }
          f.close();
          logSerial.printf("\nCAT_END:%u\n", static_cast<unsigned>(total));
        }
      } else if (cmd.startsWith("LS")) {
        String path = "/";
        const int space = cmd.indexOf(' ');
        if (space > 0) path = cmd.substring(space + 1);
        path.trim();
        auto files = Storage.listFiles(path.c_str(), 400);
        logSerial.printf("LS_START:%s:%u\n", path.c_str(), static_cast<unsigned>(files.size()));
        for (auto& name : files) logSerial.printf("  %s\n", name.c_str());
        logSerial.printf("LS_END\n");
      } else if (cmd == "PAGENEXT") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugPageTurn(true);
      } else if (cmd == "PAGEPREV") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugPageTurn(false);
      } else if (cmd == "PAGEDUMP") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugDumpPage();
      } else if (cmd == "STRESSCOMMENT") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugOpenStressComment();
      } else if (cmd == "FAKECOMMENT") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugOpenFakeJjwxcComment();
      } else if (cmd == "COMMENT") {
        if (auto* activity = activityManager.currentActivityPtr()) activity->debugOpenJjwxcComment();
      } else if (cmd == "EMOJI") {
        // On-device probe for the bundled emoji fallback: the message page draws
        // through drawText(), so this exercises font + per-run split.
        activityManager.goToFullScreenMessage("Emoji \xF0\x9F\x98\x80\xF0\x9F\x91\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xF0\x9F\x9A\x80 \xE4\xB8\xAD\xE6\x96\x87",
                                              EpdFontFamily::BOLD, "Noto Emoji subset / fallback", false);
      }
#if defined(CROSSPOINT_MUPDF)
      else if (cmd == "PDF") {
        for (int i = 0; i <= 4; ++i) {
          const std::string cache = "/.crosspoint/pdf_test/page_" + std::to_string(i) + ".bmp";
          if (Storage.exists(cache.c_str())) Storage.remove(cache.c_str());
        }
        PdfReaderActivity::debugInitialPage = 0;
        LOG_ERR("PDF", "serial CMD:PDF opening the test PDF");
        activityManager.pushActivity(std::make_unique<PdfReaderActivity>(
            renderer, mappedInputManager, "/BOOKS/test.pdf", false));
      } else if (cmd == "PDFNEXT") {
        auto* pdfActivity = static_cast<PdfReaderActivity*>(activityManager.currentActivityPtr());
        if (pdfActivity) {
          LOG_ERR("PDF", "serial CMD:PDFNEXT");
          pdfActivity->pageTurn(true);
          pdfActivity->requestUpdate();
        }
      } else if (cmd.startsWith("PDFNOTE")) {
        int page = 0;
        const int space = cmd.indexOf(' ');
        if (space > 0) page = cmd.substring(space + 1).toInt() - 1;
        if (page < 0) page = 0;
        PdfReaderActivity::debugSeedNotePage = page;
        PdfReaderActivity::debugInitialPage = page;
        LOG_ERR("PDF", "serial CMD:PDFNOTE seeding a note on page %d", page + 1);
        activityManager.pushActivity(std::make_unique<PdfReaderActivity>(
            renderer, mappedInputManager, "/BOOKS/test.pdf", false));
      } else if (cmd.startsWith("PDFHL")) {
        int page = 0;
        const int space = cmd.indexOf(' ');
        if (space > 0) page = cmd.substring(space + 1).toInt() - 1;
        if (page < 0) page = 0;
        PdfReaderActivity::debugSeedHighlightPage = page;
        PdfReaderActivity::debugInitialPage = page;
        LOG_ERR("PDF", "serial CMD:PDFHL seeding a highlight on page %d", page + 1);
        activityManager.pushActivity(std::make_unique<PdfReaderActivity>(
            renderer, mappedInputManager, "/BOOKS/test.pdf", false));
      } else if (cmd == "SELFTEST") {
        PdfReaderActivity::debugSelfTest = true;
        PdfReaderActivity::debugInitialPage = -1;
        LOG_ERR("PDF", "serial CMD:SELFTEST walking every page");
        activityManager.pushActivity(std::make_unique<PdfReaderActivity>(
            renderer, mappedInputManager, "/BOOKS/test.pdf", false));
      } else if (cmd == "REFLOW") {
        PdfReaderActivity::debugReflowEnabled = true;
        LOG_ERR("PDF", "serial CMD:REFLOW enabled for the next page render");
      } else if (cmd == "PDFCACHE") {
        // Rebuild the whole big-PDF page cache from page 1.  Debug-only: it
        // drops the cache directory (meta/progress included) and leaves
        // debugInitialPage at -1 so loop() does the background caching.
        Storage.removeDir("/.crosspoint/pdf_test");
        PdfReaderActivity::debugInitialPage = -1;
        LOG_ERR("PDF", "serial CMD:PDFCACHE rebuilding the big-PDF cache");
        activityManager.pushActivity(std::make_unique<PdfReaderActivity>(
            renderer, mappedInputManager, "/BOOKS/test.pdf", false));
      } else if (cmd.startsWith("PDFBIG")) {
        // Optional 1-based page for a focused render test: "CMD:PDFBIG 68".
        // The stale cache entry is removed so renderPage really re-renders it.
        int page = 0;
        const int space = cmd.indexOf(' ');
        if (space > 0) page = cmd.substring(space + 1).toInt() - 1;
        if (page < 0) page = 0;
        PdfReaderActivity::debugInitialPage = page;
        // Same debug-only cache path the PDF command uses (fnv of the book).
        const std::string stale = "/.crosspoint/pdf_test/page_" + std::to_string(page) + ".bmp";
        if (Storage.exists(stale.c_str())) Storage.remove(stale.c_str());
        LOG_ERR("PDF", "serial CMD:PDFBIG opening big PDF at page %d", page + 1);
        activityManager.pushActivity(std::make_unique<PdfReaderActivity>(
            renderer, mappedInputManager, "/BOOKS/test.pdf", false));
      }
#endif
    }
  }

  // Check for any real user activity (button, touch, or tilt).
  static unsigned long lastActivityTime = millis();
  if (mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased() || gpio.wasTouchActivity() ||
      halTiltSensor.hadActivity()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }
  // preventAutoSleep() is intentionally NOT folded into the activity check above:
  // it only short-circuits the deep-sleep timer below, not the inactivity clock
  // that drives auto-downclock. Standby (a clock face) wants deep sleep blocked
  // but still benefits from the framework dropping CPU to LOW_POWER_FREQ.

  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  if (handleX4ProFrontlightDoubleClick()) return;

  const bool x4ProDoubleClickPwrLight = BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight;

#if FREEINK_CAP_TOUCH
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && x4ProDoubleClickPwrLight) {
    if (lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
      lastX4ProPowerClickAt = 0;
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
    // A release held too long to be a double-click candidate (but still within
    // the normal Confirm press duration) never reaches handleX4ProFrontlightDoubleClick's
    // click tracking above, so it needs its own Confirm check here.
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power) &&
        gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS &&
        gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration()) {
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
  }
#endif

  // Same deferral for SLEEP: getPowerButtonDuration() drops to 10ms so a quick
  // tap sleeps the device, which otherwise fires on button-down and never lets
  // a second click land. Sleep only once the double-click window has passed.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && x4ProDoubleClickPwrLight &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && !activityManager.preventAutoSleep() && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  // On X4 Pro with SLEEP, a press still within the click window is a
  // double-click candidate — let it be released and evaluated above instead
  // of sleeping on button-down.
  const bool x4ProAwaitingClickWindow = x4ProDoubleClickPwrLight &&
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP &&
                                        gpio.getPowerButtonHeldTime() <= X4PRO_POWER_CLICK_MAX_HOLD_MS;

  if (!x4ProAwaitingClickWindow && powerReleasedSinceWake && millis() >= allowSleepAt &&
      gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::ToggleFrontlight) {
    toggleFrontlight();
  }
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::Refresh ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
       mappedInputManager.wasReleased(MappedInputManager::Button::Power))) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }
#ifndef CROSSPOINT_EMULATED
  if (gpio.wasInputModalityChanged() && gpio.hasTouch() && UITheme::getInstance().hasMainTabs() &&
      !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }
#endif

  const unsigned long activityStartTime = millis();
  const bool readerWasActive = activityManager.isReaderActivity();
  activityManager.loop();
  updateBluetoothLifecycle();
  const bool readerIsActive = activityManager.isReaderActivity();
  const unsigned long activityDuration = millis() - activityStartTime;

  if (readerWasActive && !readerIsActive) {
    if (!READING_STATS.saveToFile()) {
      LOG_ERR("RST", "Failed to save reading stats after reader exit");
    }
    if (!ACHIEVEMENTS.saveToFile()) {
      LOG_ERR("ACH", "Failed to save achievements after reader exit");
    }
  } else if (readerIsActive && (millis() - lastActivityTime) >= READING_STATS_CHECKPOINT_IDLE_MS &&
             !activityManager.skipLoopDelay() && !activityManager.preventAutoSleep() &&
             READING_STATS.shouldSaveCheckpoint()) {
    activityManager.cancelIdleRender();
    RenderLock lock;
    if (!READING_STATS.saveToFile()) {
      LOG_ERR("RST", "Failed to save idle reading checkpoint");
    }
  }

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  bool skipLoopDelay = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      // Let rendering advance without treating lock contention as idle.
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.rawInputActive()) break;
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
