#pragma once

#include <StudyPackStore.h>

#include <vector>

#include "StudyScreenActivity.h"

// Flashcards with the app's SM-2 schedule: show the prompt, tap to reveal, then Got it / Don't got it. Each rating is
// appended to /study/progress.jsonl so the app can merge it.
class StudyFlashcardsActivity final : public StudyScreenActivity {
 public:
  static constexpr size_t kSessionLength = 20;

  StudyFlashcardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track, int category)
      : StudyScreenActivity("StudyFlashcards", renderer, mappedInput, std::move(track), category) {}

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 protected:
  bool onTap(int id) override;

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

  void buildSession();
  bool loadCurrent();
  void rate(int quality);
};
