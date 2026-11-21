#pragma once

#include <StudyPackParse.h>

#include <string>

#include "StudyListActivity.h"

// One cert track: pick Flashcards, Quiz or Practice test, and optionally narrow to one category.
class StudyTrackActivity final : public StudyListActivity {
 public:
  StudyTrackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track)
      : StudyListActivity("StudyTrack", renderer, mappedInput), track(std::move(track)) {}

  void onEnter() override;

 protected:
  const char* title() const override { return track.label.c_str(); }
  const char* subtitle() const override;
  void onRowChosen(int index) override;

 private:
  enum Row : int { RowFlashcards, RowQuiz, RowPracticeTest, RowCategory, RowCount };

  study::TrackInfo track;
  int category = -1;  // -1 = all categories
  char cardsValue[24] = {};
  char quizValue[24] = {};
  char examValue[32] = {};

  void buildRows();
  void openMode(int row);
};
