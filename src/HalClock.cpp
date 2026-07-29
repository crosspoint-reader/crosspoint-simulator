#include "HalClock.h"

#include <cstdio>
#include <ctime>

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
#if defined(SIMULATOR_DEVICE_X3) || defined(SIMULATOR_DEVICE_X4_PRO)
  _available = true;
#else
  _available = false;
#endif
}

void HalClock::update() {
  if (_autoSyncEnabled && !_autoSyncAttempted)
    requestSync();
}

std::time_t HalClock::nowUtc() const {
  const std::time_t hostNow = std::time(nullptr);
  return hostNow >= MIN_TRUSTED_EPOCH
             ? hostNow + _utcOffsetSeconds
             : 0;
}

bool HalClock::hasValidTime() const {
  return nowUtc() >= MIN_TRUSTED_EPOCH;
}

bool HalClock::setUtcTime(const std::time_t epoch) {
  const std::time_t hostNow = std::time(nullptr);
  if (epoch < MIN_TRUSTED_EPOCH || hostNow < MIN_TRUSTED_EPOCH)
    return false;
  _utcOffsetSeconds = epoch - hostNow;
  _syncState = ClockSyncState::Idle;
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
  _autoSyncAttempted = true;
  _syncState = ClockSyncState::Syncing;
  if (std::time(nullptr) < MIN_TRUSTED_EPOCH) {
    _syncState = ClockSyncState::Failed;
    return false;
  }
  _utcOffsetSeconds = 0;
  _syncState = ClockSyncState::Succeeded;
  return true;
}

bool HalClock::syncNow(uint32_t /*timeoutMs*/) {
  return requestSync();
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
