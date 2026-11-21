#include "StudySrs.h"

#include <cmath>

namespace study {

namespace {
constexpr uint16_t kMinEase100 = 130;
constexpr int kMinTrustedYear = 2024;
}  // namespace

SrsEntry nextSrs(const SrsEntry& prev, const int quality, const uint32_t now) {
  SrsEntry next = prev;
  next.last = static_cast<uint8_t>(quality < 1 ? 1 : (quality > 5 ? 5 : quality));
  if (quality < 3) {
    const int ease = static_cast<int>(prev.ease100) - 20;
    next.ease100 = static_cast<uint16_t>(ease < kMinEase100 ? kMinEase100 : ease);
    next.intervalDays = 1;
    next.reps = 0;
    next.due = now;
    return next;
  }
  // Work in doubles like the app so rounding matches: ease + (0.1 - (5-q) * (0.08 + (5-q) * 0.02)).
  const double miss = 5.0 - quality;
  double ease = prev.ease100 / 100.0 + (0.1 - miss * (0.08 + miss * 0.02));
  if (ease < kMinEase100 / 100.0) ease = kMinEase100 / 100.0;
  next.ease100 = static_cast<uint16_t>(std::lround(ease * 100.0));
  next.reps = static_cast<uint8_t>(prev.reps < 255 ? prev.reps + 1 : 255);
  long interval = 1;
  if (next.reps == 2) {
    interval = 6;
  } else if (next.reps > 2) {
    interval = std::lround(prev.intervalDays * ease);
    if (interval < 1) interval = 1;
    if (interval > 65535) interval = 65535;
  }
  next.intervalDays = static_cast<uint16_t>(interval);
  next.due = now + static_cast<uint32_t>(interval) * kSecondsPerDay;
  return next;
}

int32_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

uint32_t unixSecondsUtc(const int year, const unsigned month, const unsigned day, const unsigned hour,
                        const unsigned minute) {
  if (year < kMinTrustedYear || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return 0;
  const int64_t days = daysFromCivil(year, month, day);
  return static_cast<uint32_t>(days * kSecondsPerDay + hour * 3600 + minute * 60);
}

}  // namespace study
