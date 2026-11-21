#pragma once

#include <StudyPackParse.h>
#include <StudyStats.h>

#include <cstdint>
#include <vector>

#include "StudyScreenActivity.h"

// Dashboard home -> swipe left: a Study page with stats from the reader's own study history (cards due, day streak,
// quiz accuracy, today's and all-time counts, and per-track progress). Tap a track to study it, or Open Study for the
// full track list. Swipe right or Back returns to Home.
class StudyStatsActivity final : public StudyScreenActivity {
 public:
  StudyStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : StudyScreenActivity("StudyStats", renderer, mappedInput, study::TrackInfo{}, -1) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 protected:
  bool onTap(int id) override;

 private:
  enum HitId : int { HitOpenStudy = 0, HitTrack = 100 };

  void reload();
  void openStudyHome();
  void openTrack(size_t index);

  std::vector<study::TrackInfo> tracks;
  std::vector<study::TrackStudyStats> perTrack;  // same order as tracks
  std::vector<uint16_t> order;                   // track indices, most studied first
  study::StudyTotals totals;
  uint32_t cardsSeen = 0;
  uint32_t cardsDue = 0;
  bool clockSet = false;
  bool hasPack = false;
};
