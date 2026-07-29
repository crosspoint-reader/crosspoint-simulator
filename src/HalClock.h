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

 public:
  void begin();
  void update();

  std::time_t nowUtc() const;
  bool hasValidTime() const;
  bool setUtcTime(std::time_t epoch);
  void setAutoSyncEnabled(bool enabled);
  bool requestSync();
  bool syncNow(uint32_t timeoutMs = 10000);
  ClockSyncState syncState() const { return _syncState; }

  bool isAvailable() const { return _available; }
  bool getTime(uint8_t& hour, uint8_t& minute) const;
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool formatTime(char* buf, size_t bufSize,
                  uint8_t utcOffsetQuarterHoursBiased = 48,
                  bool use12Hour = false) const;
  bool formatDate(char* buf, size_t bufSize,
                  uint8_t utcOffsetQuarterHoursBiased = 48) const;
  bool syncFromNTP();
};
