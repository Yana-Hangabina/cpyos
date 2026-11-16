#include "HalClock.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <WiFi.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include "plus_rtc.h"

HalClock halClock;

namespace {
constexpr std::time_t MIN_TRUSTED_EPOCH = 1704016800;

bool toUtc(const std::time_t epoch, std::tm &result) {
#if defined(_WIN32)
  return gmtime_s(&result, &epoch) == 0;
#else
  return gmtime_r(&epoch, &result) != nullptr;
#endif
}
} // namespace

void HalClock::begin() {
  // The PCF8563 keeps time across power loss. Seed the system clock from it so
  // the reader/standby screens have a date before any network sync.
  struct tm rtc{};
  if (PlusRtc_Read(&rtc)) {
    const std::time_t epoch = timegm(&rtc);
    if (epoch >= MIN_TRUSTED_EPOCH) {
      struct timeval tv{};
      tv.tv_sec = epoch;
      settimeofday(&tv, nullptr);
      _available = true;
      ESP_LOGI("PLUS-RTC", "system clock seeded from PCF8563");
      return;
    }
  }
  _available = std::time(nullptr) >= MIN_TRUSTED_EPOCH;
}

void HalClock::update() {
  if (_autoSyncEnabled && !_autoSyncAttempted) {
    _autoSyncAttempted = true;
    requestSync();
  }
  if (_syncState != ClockSyncState::Syncing) return;
  if (!esp_sntp_enabled()) {
    // NetworkStartup::suspendTimeSync() stopped it for a radio session.
    stopSntp();
    _syncState = ClockSyncState::Failed;
    return;
  }
  if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(0)) == ESP_OK) {
    const std::time_t now = std::time(nullptr);
    if (now >= MIN_TRUSTED_EPOCH && setUtcTime(now)) {
      _syncState = ClockSyncState::Succeeded;
      ESP_LOGI("PLUS-RTC", "SNTP synchronized and written to PCF8563");
    } else {
      _syncState = ClockSyncState::Failed;
    }
    stopSntp();
  } else if (millis() - _syncStartMs > 45000) {
    ESP_LOGW("PLUS-RTC", "SNTP timed out");
    stopSntp();
    _syncState = ClockSyncState::Failed;
  }
}

void HalClock::stopSntp() {
  if (!_sntpInitialized) return;
  esp_netif_sntp_deinit();
  _sntpInitialized = false;
}

void HalClock::setUseChinaServers(bool china) {
  if (_sntpInitialized) stopSntp();
  _useChinaServers = china;
}

std::time_t HalClock::nowUtc() const {
  const std::time_t hostNow = std::time(nullptr);
  return hostNow >= MIN_TRUSTED_EPOCH ? hostNow + _utcOffsetSeconds : 0;
}

bool HalClock::hasValidTime() const {
  return nowUtc() >= MIN_TRUSTED_EPOCH;
}

bool HalClock::setUtcTime(const std::time_t epoch) {
  if (epoch < MIN_TRUSTED_EPOCH)
    return false;
  struct timeval tv{};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
  _utcOffsetSeconds = 0;
  _available = true;
  _syncState = ClockSyncState::Idle;
  struct tm utc{};
  if (gmtime_r(&epoch, &utc) != nullptr) {
    PlusRtc_Write(&utc);
  }
  return true;
}

void HalClock::setAutoSyncEnabled(const bool enabled) {
  if (enabled && !_autoSyncEnabled)
    _autoSyncAttempted = false;
  _autoSyncEnabled = enabled;
  if (!enabled && _syncState == ClockSyncState::Syncing)
    _syncState = ClockSyncState::Idle;
}

bool HalClock::requestSync() {
  if (_syncState == ClockSyncState::Syncing) return true;
  if (WiFi.status() != WL_CONNECTED) {
    _syncState = ClockSyncState::Failed;
    return false;
  }
  if (!_sntpInitialized) {
    // This target builds with CONFIG_LWIP_SNTP_MAX_SERVERS=1.
    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(_useChinaServers ? "ntp.aliyun.com" : "pool.ntp.org");
    config.start = false;
    config.smooth_sync = false;
    if (esp_netif_sntp_init(&config) != ESP_OK) {
      ESP_LOGE("PLUS-RTC", "SNTP init failed");
      _syncState = ClockSyncState::Failed;
      return false;
    }
    _sntpInitialized = true;
  }
  if (esp_netif_sntp_start() != ESP_OK) {
    ESP_LOGE("PLUS-RTC", "SNTP start failed");
    stopSntp();
    _syncState = ClockSyncState::Failed;
    return false;
  }
  _syncStartMs = millis();
  _syncState = ClockSyncState::Syncing;
  ESP_LOGI("PLUS-RTC", "SNTP sync started");
  return true;
}

bool HalClock::syncNow(uint32_t timeoutMs) {
  if (!requestSync()) return false;
  const bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
  if (ok) {
    const std::time_t now = std::time(nullptr);
    if (now >= MIN_TRUSTED_EPOCH && setUtcTime(now)) {
      _syncState = ClockSyncState::Succeeded;
    } else {
      _syncState = ClockSyncState::Failed;
    }
  } else {
    _syncState = ClockSyncState::Failed;
  }
  stopSntp();
  return _syncState == ClockSyncState::Succeeded;
}

void HalClock::setTimezone(const char *posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
}

bool HalClock::localTime(struct tm &out) const {
  if (!_available)
    return false;

  const std::time_t now = nowUtc();
#if defined(_WIN32)
  return localtime_s(&out, &now) == 0;
#else
  return localtime_r(&now, &out) != nullptr;
#endif
}

bool HalClock::formatTime(char *buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u))
    return false;

  std::tm local{};
  if (!localTime(local))
    return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0)
      hour12 = 12;
    std::snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min,
                  pm ? "PM" : "AM");
  } else {
    std::snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::getTime(uint8_t &hour, uint8_t &minute) const {
  if (!_available)
    return false;

  const std::time_t now = nowUtc();
  std::tm utcTime{};
  if (!now || !toUtc(now, utcTime))
    return false;
  hour = static_cast<uint8_t>(utcTime.tm_hour);
  minute = static_cast<uint8_t>(utcTime.tm_min);
  return true;
}

bool HalClock::getDateTime(uint16_t &year, uint8_t &month, uint8_t &day,
                           uint8_t &hour, uint8_t &minute) const {
  if (!_available)
    return false;

  const std::time_t now = nowUtc();
  std::tm utcTime{};
  if (!now || !toUtc(now, utcTime))
    return false;
  year = static_cast<uint16_t>(utcTime.tm_year + 1900);
  month = static_cast<uint8_t>(utcTime.tm_mon + 1);
  day = static_cast<uint8_t>(utcTime.tm_mday);
  hour = static_cast<uint8_t>(utcTime.tm_hour);
  minute = static_cast<uint8_t>(utcTime.tm_min);
  return true;
}

bool HalClock::formatTime(char *buf, size_t bufSize,
                          uint8_t utcOffsetQuarterHoursBiased,
                          bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u))
    return false;

  uint8_t hour;
  uint8_t minute;
  if (!getTime(hour, minute))
    return false;

  if (utcOffsetQuarterHoursBiased > 104)
    utcOffsetQuarterHoursBiased = 104;
  const int offsetQuarterHours =
      static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) +
                     offsetQuarterHours * 15;
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0)
      hour12 = 12;
    std::snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    std::snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::formatDate(char *buf, size_t bufSize,
                          uint8_t utcOffsetQuarterHoursBiased) const {
  if (bufSize < 13u || !_available)
    return false;

  if (utcOffsetQuarterHoursBiased > 104)
    utcOffsetQuarterHoursBiased = 104;
  const int offsetQuarterHours =
      static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  const std::time_t utcNow = nowUtc();
  if (!utcNow)
    return false;
  const std::time_t now =
      utcNow +
      static_cast<std::time_t>(offsetQuarterHours) * 15 * 60;
  std::tm utcTime{};
  if (!toUtc(now, utcTime))
    return false;
  if (std::strftime(buf, bufSize, "%b %e, %Y", &utcTime) == 0)
    return false;
  if (buf[0] != '\0') {
    for (char *p = buf; *p != '\0'; ++p) {
      if (*p == ' ' && *(p + 1) == ' ') {
        ++p;
        while (*p != '\0') {
          *(p - 1) = *p;
          ++p;
        }
        *(p - 1) = '\0';
        break;
      }
    }
  }
  return true;
}

bool HalClock::syncFromNTP() { return _available && syncNow(); }
