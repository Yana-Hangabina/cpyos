#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

enum class ClockSyncState : uint8_t {
  Idle,
  Syncing,
  Succeeded,
  Failed,
};

class HalClock;
extern HalClock halClock;

class HalClock {
  bool _available = false;
  bool _autoSyncEnabled = true;
  bool _autoSyncAttempted = false;
  std::time_t _utcOffsetSeconds = 0;
  ClockSyncState _syncState = ClockSyncState::Idle;
  bool _sntpInitialized = false;
  bool _useChinaServers = false;
  uint32_t _syncStartMs = 0;
  void stopSntp();

 public:
  void begin();
  void update();

  std::time_t nowUtc() const;
  bool hasValidTime() const;
  bool setUtcTime(std::time_t epoch);
  void setAutoSyncEnabled(bool enabled);
  void setUseChinaServers(bool china);
  bool requestSync();
  bool syncNow(uint32_t timeoutMs = 10000);
  ClockSyncState syncState() const { return _syncState; }

  bool isAvailable() const { return _available; }
  void setTimezone(const char *posixTz);
  bool localTime(struct tm &out) const;
  bool getTime(uint8_t& hour, uint8_t& minute) const;
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool formatTime(char *buf, size_t bufSize, bool use12Hour = false) const;
  bool formatTime(char* buf, size_t bufSize,
                  uint8_t utcOffsetQuarterHoursBiased,
                  bool use12Hour = false) const;
  bool formatDate(char* buf, size_t bufSize,
                  uint8_t utcOffsetQuarterHoursBiased = 48) const;
  bool syncFromNTP();
};
