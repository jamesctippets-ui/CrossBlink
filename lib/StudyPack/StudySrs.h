#pragma once

#include <cstdint>

// SM-2 spaced repetition, matching Cert Study Hub's nextSrsEntry() (src/js/03_helpers.js in the Study-App repo) so a
// card scheduled on the reader and in the app agree. The app recomputes its own schedule from the progress log, so
// this only has to be good enough to pick which cards to show next.
namespace study {

constexpr uint32_t kSecondsPerDay = 86400;
constexpr int kQualityGotIt = 4;
constexpr int kQualityMissed = 1;

struct SrsEntry {
  uint32_t due = 0;           // Unix seconds; 0 with reps 0 and ease 250 means "never rated"
  uint16_t intervalDays = 0;  // current interval
  uint16_t ease100 = 250;     // ease factor x100 (the app's 2.5 is 250)
  uint8_t reps = 0;           // successful reviews in a row
  uint8_t last = 0;           // last quality given, 1-5 (0 = never rated)
};

// Returns the entry after rating a card with quality 1-5 at Unix time now. A quality below 3 resets the card.
SrsEntry nextSrs(const SrsEntry& prev, int quality, uint32_t now);

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int32_t daysFromCivil(int year, unsigned month, unsigned day);

// Unix seconds for a UTC date and time, or 0 when the date is clearly not a set clock (before 2024).
uint32_t unixSecondsUtc(int year, unsigned month, unsigned day, unsigned hour, unsigned minute);

}  // namespace study
