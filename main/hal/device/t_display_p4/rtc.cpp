/*
 * @Description: T-Display-P4 PCF8563 RTC 实现
 * @Author: LILYGO_L
 * @Date: 2026-08-28 00:00:00
 * @LastEditTime: 2026-09-02 17:53:12
 * @License: GPL 3.0
 */
#include <sys/time.h>

#include <ctime>

#include "base/logger.h"
#include "hal/device/common/device_utils.h"
#include "hal/device/t_display_p4/device.h"
#include "nvs.h"

namespace lilygo_box::hal {

// SurvivalOS: the PCF8563 holds UTC, so the clock stays right across summer
// time changes (the phone reboots on every wake from deep sleep and reads it
// again). Older firmware wrote local time; an NVS flag says which it is, and
// the first read after the update converts it once.
namespace {

constexpr char kClockNamespace[] = "svos_clock";
constexpr char kRtcUtcKey[] = "rtc_utc";

// -1 not known yet (NVS not ready), 0 local time (old firmware), 1 UTC.
int g_rtc_holds_utc = -1;

int RtcHoldsUtc() {
  if (g_rtc_holds_utc >= 0) {
    return g_rtc_holds_utc;
  }
  nvs_handle_t handle = 0;
  if (nvs_open(kClockNamespace, NVS_READONLY, &handle) == ESP_OK) {
    uint8_t value = 0;
    g_rtc_holds_utc = nvs_get_u8(handle, kRtcUtcKey, &value) == ESP_OK && value == 1 ? 1 : 0;
    nvs_close(handle);
  } else if (nvs_open(kClockNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    // The namespace does not exist yet: NVS works, the flag was never set.
    nvs_close(handle);
    g_rtc_holds_utc = 0;
  }
  return g_rtc_holds_utc;
}

void MarkRtcUtc() {
  nvs_handle_t handle = 0;
  if (nvs_open(kClockNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    if (nvs_set_u8(handle, kRtcUtcKey, 1) == ESP_OK && nvs_commit(handle) == ESP_OK) {
      g_rtc_holds_utc = 1;
    }
    nvs_close(handle);
  }
}

}  // namespace

static int64_t UnixTimeFromUtc(int year, int month, int day, int hour, int minute, int second) {
  // Days since 1970-01-01 in the proleptic Gregorian calendar (H. Hinnant).
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const int year_of_era = year - era * 400;
  const int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const int64_t days = static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

bool TDisplayP4Device::ReadRtcStatus(RtcStatus* status) {
  if (status == nullptr) {
    return false;
  }

  *status = RtcStatus();
  if (!driver_.IsPcf8563Ready() && !driver_.InitPcf8563()) {
    LogMessage(
        LogLevel::kWarning, __FILE__, __LINE__, "Pcf8563 init retry failed\n");
    return false;
  }

  cpp_bus_driver::Pcf8563x::Time time;
  if (!driver_.chip().pcf8563->GetTime(time)) {
    return false;
  }

  status->ready = true;
  status->clock_integrity = driver_.chip().pcf8563->CheckClockIntegrityFlag();
  status->year = static_cast<uint16_t>(time.year) + 2000;
  status->month = time.month;
  status->day = time.day;
  status->week = static_cast<uint8_t>(time.week);
  status->hour = time.hour;
  status->minute = time.minute;
  status->second = time.second;
  const bool valid = status->month >= 1 && status->month <= 12 && status->day >= 1 &&
                     status->day <= 31 && status->hour <= 23 && status->minute <= 59 &&
                     status->second <= 59;
  const int holds_utc = RtcHoldsUtc();
  if (holds_utc == 1 && valid) {
    // UTC in the chip, local time for everyone who asks.
    const std::time_t unix_time = static_cast<std::time_t>(UnixTimeFromUtc(status->year,
        status->month, status->day, status->hour, status->minute, status->second));
    std::tm local_time = {};
    if (localtime_r(&unix_time, &local_time) != nullptr) {
      status->year = static_cast<uint16_t>(local_time.tm_year + 1900);
      status->month = static_cast<uint8_t>(local_time.tm_mon + 1);
      status->day = static_cast<uint8_t>(local_time.tm_mday);
      status->week = static_cast<uint8_t>(local_time.tm_wday);
      status->hour = static_cast<uint8_t>(local_time.tm_hour);
      status->minute = static_cast<uint8_t>(local_time.tm_min);
      status->second = static_cast<uint8_t>(local_time.tm_sec);
    }
  } else if (holds_utc == 0 && valid && status->clock_integrity) {
    // Written as local time by older firmware: rewrite it as UTC once.
    std::tm local_time = {};
    local_time.tm_year = status->year - 1900;
    local_time.tm_mon = status->month - 1;
    local_time.tm_mday = status->day;
    local_time.tm_hour = status->hour;
    local_time.tm_min = status->minute;
    local_time.tm_sec = status->second;
    local_time.tm_isdst = -1;
    const std::time_t unix_time = std::mktime(&local_time);
    if (unix_time != static_cast<std::time_t>(-1) && WriteRtcUnixTime(unix_time)) {
      LogMessage(LogLevel::kInfo, __FILE__, __LINE__, "RTC now keeps UTC\n");
    }
  }
  return true;
}

void TDisplayP4Device::SyncClockFromGnss(int year, int month, int day, int hour,
    int minute, int second) {
  if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 60) {
    return;
  }
  const int64_t unix_time = UnixTimeFromUtc(year, month, day, hour, minute, second);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t difference = unix_time > now ? unix_time - now : now - unix_time;
  // Within two seconds is right already; the RTC also needs no rewrite then.
  if (difference < 2) {
    return;
  }
  timeval system_time = {};
  system_time.tv_sec = static_cast<std::time_t>(unix_time);
  if (settimeofday(&system_time, nullptr) == 0) {
    WriteRtcUnixTime(unix_time);
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Clock set from GPS (was %lld s off)\n", static_cast<long long>(difference));
  }
}

bool TDisplayP4Device::WriteRtcUnixTime(int64_t unix_time) {
  if (unix_time <= device_utils::kValidUnixTimeThreshold ||
      (!driver_.IsPcf8563Ready() && !driver_.InitPcf8563())) {
    return false;
  }

  const std::time_t time_value = static_cast<std::time_t>(unix_time);
  std::tm local_time = {};  // UTC, see ReadRtcStatus()
  if (gmtime_r(&time_value, &local_time) == nullptr ||
      local_time.tm_year + 1900 < 2000 || local_time.tm_year + 1900 > 2099) {
    return false;
  }

  cpp_bus_driver::Pcf8563x::Time rtc_time;
  rtc_time.year = static_cast<uint8_t>(local_time.tm_year + 1900 - 2000);
  rtc_time.month = static_cast<uint8_t>(local_time.tm_mon + 1);
  rtc_time.day = static_cast<uint8_t>(local_time.tm_mday);
  rtc_time.week =
      static_cast<cpp_bus_driver::Pcf8563x::Week>(local_time.tm_wday);
  rtc_time.hour = static_cast<uint8_t>(local_time.tm_hour);
  rtc_time.minute = static_cast<uint8_t>(local_time.tm_min);
  rtc_time.second = static_cast<uint8_t>(local_time.tm_sec);
  const bool written = driver_.chip().pcf8563->SetTime(rtc_time) &&
                       driver_.chip().pcf8563->ClearClockIntegrityFlag();
  if (written) {
    MarkRtcUtc();
  }
  return written;
}

}  // namespace lilygo_box::hal
