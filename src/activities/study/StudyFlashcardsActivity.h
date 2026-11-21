#pragma once

#include <StudyPackStore.h>

#include <vector>

#include "StudyBookActivity.h"

// Flashcards with the app's SM-2 schedule, read like a book: the prompt, then turn the page (or Show answer) for the
// answer, then Got it / Don't got it. Each rating is appended to /study/progress.jsonl so the app can merge it.
class StudyFlashcardsActivity final : public StudyBookActivity {
 public:
  static constexpr size_t kSessionLength = 20;

  StudyFlashcardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track, int category)
      : StudyBookActivity("StudyFlashcards", renderer, mappedInput, std::move(track), category) {}

  void onEnter() override;
  void onExit() override;

 protected:
  bool onTap(int id) override;
  bool onPastEnd() override;
  bool onBeforeStart() override;
  void statusTitle(char* out, size_t size) const override;
  float statusProgress() const override;

 private:
  enum class State : uint8_t { Front, Back, Summary, Empty, Error };
  enum HitId : int { HitReveal = 1, HitGotIt, HitMissed, HitMore, HitDone };

  study::JsonlFile cards;
  study::SrsStore srs;
  study::ProgressLog log;
  study::Card card;
  std::vector<uint16_t> session;  // line indexes, in study order
  size_t position = 0;
  unsigned gotIt = 0;
  unsigned missed = 0;
  unsigned ratingsSinceSave = 0;
  State state = State::Front;
  char summary[96] = "";

  void buildPage(bool focusLast = false);
  void buildSession();
  bool loadCurrent();
  void rate(int quality);
};
