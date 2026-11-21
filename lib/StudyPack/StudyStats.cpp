#include "StudyStats.h"

#include <cstring>

namespace study {

namespace {

// Index just past `key` in line, or len when absent. Keys are matched with their quotes and colon, e.g. "\"t\":".
size_t findKey(const char* line, const size_t len, const char* key) {
  const size_t keyLen = strlen(key);
  for (size_t i = 0; i + keyLen <= len; ++i) {
    if (memcmp(line + i, key, keyLen) == 0) return i + keyLen;
  }
  return len;
}

// Reads an unsigned decimal at line[pos]. Returns false when there are no digits or it overflows 32 bits.
bool readUnsigned(const char* line, const size_t len, size_t pos, uint32_t& out) {
  uint64_t value = 0;
  bool digits = false;
  while (pos < len && line[pos] >= '0' && line[pos] <= '9') {
    value = value * 10 + static_cast<uint64_t>(line[pos] - '0');
    if (value > UINT32_MAX) return false;
    digits = true;
    ++pos;
  }
  out = static_cast<uint32_t>(value);
  return digits;
}

}  // namespace

bool parseLogEvent(const char* line, const size_t len, LogEvent& out) {
  if (line == nullptr || len == 0) return false;
  const size_t kindPos = findKey(line, len, "\"k\":\"");
  if (kindPos + 1 >= len || line[kindPos + 1] != '"') return false;
  const char kind = line[kindPos];
  if (kind != 'c' && kind != 'q') return false;

  const size_t trackPos = findKey(line, len, "\"tr\":\"");
  size_t n = 0;
  while (trackPos + n < len && line[trackPos + n] != '"') {
    if (n >= kMaxTrackKeyLen) return false;
    out.track[n] = line[trackPos + n];
    ++n;
  }
  if (n == 0 || trackPos + n >= len) return false;  // no track, or no closing quote
  out.track[n] = '\0';

  uint32_t value = 0;
  if (kind == 'c') {
    if (!readUnsigned(line, len, findKey(line, len, "\"q\":"), value) || value < 1 || value > 5) return false;
    out.kind = LogEvent::Kind::Card;
    out.ok = value >= 3;
  } else {
    if (!readUnsigned(line, len, findKey(line, len, "\"ok\":"), value) || value > 1) return false;
    out.kind = LogEvent::Kind::Question;
    out.ok = value == 1;
  }
  out.t = readUnsigned(line, len, findKey(line, len, "\"t\":"), value) ? value : 0;
  return true;
}

StudyTally::StudyTally(const uint32_t now, const int32_t utcOffsetSeconds) : now(now), offset(utcOffsetSeconds) {
  today = now > 0 ? localDay(now) : -1;
}

int32_t StudyTally::localDay(const uint32_t t) const {
  const int64_t local = static_cast<int64_t>(t) + offset;
  return local < 0 ? -1 : static_cast<int32_t>(local / kSecondsPerDay);
}

void StudyTally::add(const LogEvent& event) {
  const bool card = event.kind == LogEvent::Kind::Card;
  if (card) {
    ++sums.cardReviews;
    if (event.ok) ++sums.cardsGotIt;
  } else {
    ++sums.questions;
    if (event.ok) ++sums.questionsCorrect;
  }
  if (today < 0 || event.t == 0) return;
  const int32_t day = localDay(event.t);
  if (day < 0 || day > today) return;  // logged with a clock that was ahead of this one
  const int32_t ago = today - day;
  if (ago == 0) {
    if (card) {
      ++sums.todayCards;
    } else {
      ++sums.todayQuestions;
    }
  }
  if (ago < kStreakWindowDays) activeDays[ago / 8] = static_cast<uint8_t>(activeDays[ago / 8] | (1u << (ago % 8)));
}

void StudyTally::addSchedule(const SrsEntry& entry, TrackStudyStats& track) const {
  if (entry.last == 0 && entry.reps == 0) return;  // never rated
  if (track.cardsSeen < UINT16_MAX) ++track.cardsSeen;
  if (now > 0 && entry.due <= now && track.cardsDue < UINT16_MAX) ++track.cardsDue;
}

StudyTotals StudyTally::totals() const {
  StudyTotals out = sums;
  out.streakDays = 0;
  if (today < 0) return out;
  const auto active = [this](const int ago) { return (activeDays[ago / 8] & (1u << (ago % 8))) != 0; };
  // A streak still counts until the end of the day after the last study day.
  int ago = active(0) ? 0 : 1;
  while (ago < kStreakWindowDays && active(ago)) {
    ++out.streakDays;
    ++ago;
  }
  return out;
}

}  // namespace study
