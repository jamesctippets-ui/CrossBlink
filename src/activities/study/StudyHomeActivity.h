#pragma once

#include <StudyPackParse.h>

#include <string>
#include <vector>

#include "StudyListActivity.h"

// Home -> Study: lists the cert tracks in the study pack on the SD card (/study/manifest.json).
class StudyHomeActivity final : public StudyListActivity {
 public:
  StudyHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : StudyListActivity("StudyHome", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 protected:
  const char* title() const override;
  void onRowChosen(int index) override;
  const char* emptyMessage() const override;
  const char* emptyHint() const override;

 private:
  std::vector<study::TrackInfo> tracks;
  std::vector<std::string> counts;  // "233 cards · 201 questions", one per row
};
