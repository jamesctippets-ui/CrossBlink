#pragma once

#include <cstddef>
#include <cstdint>

#include "StudyPackParse.h"
#include "StudySrs.h"

// Study stats for the Dashboard's Study page, built from what the reader already keeps: the progress log it appends
// to (/study/progress.jsonl) and its flashcard schedules (/study/.srs/<track>.bin). Pure logic, no storage, so the
// native tests cover it. StudyPackStore feeds it.
namespace study {

// One parsed progress.jsonl event. Header lines and lines of other kinds are not events.
struct LogEvent {
  enum class Kind : uint8_t { Card, Question };
  Kind kind = Kind::Card;
  char track[kMaxTrackKeyLen + 1] = {};
  bool ok = false;  // card rated "Got it" (quality 3 or more), or question answered right
  uint32_t t = 0;   // Unix seconds; 0 when the line has none (clock was not set)
};

// Reads the "k", "tr", "q"/"ok" and "t" keys the reader writes (formatCardEvent / formatQuestionEvent). Returns false
// for the header line, other kinds and malformed lines.
bool parseLogEvent(const char* line, size_t len, LogEvent& out);

// Counts across every track.
struct StudyTotals {
  uint32_t cardReviews = 0;
  uint32_t cardsGotIt = 0;
  uint32_t questions = 0;
  uint32_t questionsCorrect = 0;
  uint32_t todayCards = 0;
  uint32_t todayQuestions = 0;
  uint16_t streakDays = 0;  // consecutive days with any study, ending today (or yesterday when nothing yet today)
};

// Per-track counts.
struct TrackStudyStats {
  uint16_t cardsSeen = 0;  // cards rated at least once on this reader
  uint16_t cardsDue = 0;   // rated cards whose next review is now or past
  uint32_t questions = 0;
  uint32_t questionsCorrect = 0;
  uint32_t cardReviews = 0;

  uint32_t activity() const { return questions + cardReviews; }
};

// Whole percent, rounded down; 0 when there is nothing to divide.
inline unsigned percentOf(const uint32_t part, const uint32_t whole) {
  return whole == 0 ? 0u : static_cast<unsigned>((static_cast<uint64_t>(part) * 100u) / whole);
}

// Accumulates events into totals, today's counts and the day streak. "Today" is the local day of `now` shifted by the
// user's UTC offset. With now == 0 (clock not set) only the all-time counts are kept.
class StudyTally {
 public:
  StudyTally(uint32_t now, int32_t utcOffsetSeconds);

  void add(const LogEvent& event);
  // Counts one schedule entry toward a track's seen and due cards.
  void addSchedule(const SrsEntry& entry, TrackStudyStats& track) const;

  // Totals with the streak filled in.
  StudyTotals totals() const;
  bool clockSet() const { return today >= 0; }

  static constexpr int kStreakWindowDays = 256;

 private:
  int32_t localDay(uint32_t t) const;

  uint32_t now;
  int32_t offset;
  int32_t today = -1;
  StudyTotals sums;
  uint8_t activeDays[kStreakWindowDays / 8] = {};  // bit d = studied d days before today
};

}  // namespace study
