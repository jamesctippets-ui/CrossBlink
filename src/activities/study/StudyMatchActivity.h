#pragma once

#include <StudyPackStore.h>

#include <string>
#include <vector>

#include "StudyScreenActivity.h"

// Match: 4 terms and their 4 definitions (the blanked prompt where the pack has one), shuffled. Tap a term, then its
// definition, in either order. Like the app's Match game it is a self-test and is not logged.
class StudyMatchActivity final : public StudyScreenActivity {
 public:
  static constexpr uint8_t kPairs = 4;

  StudyMatchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track, int category)
      : StudyScreenActivity("StudyMatch", renderer, mappedInput, std::move(track), category) {}

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 protected:
  bool onTap(int id) override;

 private:
  enum class State : uint8_t { Playing, RoundDone, Empty, Error };
  enum HitId : int { HitTerm = 0, HitDef = 10, HitNextRound = 100, HitDone };

  State state = State::Playing;
  study::JsonlFile cards;
  std::vector<uint16_t> pool;  // card lines in the chosen category
  size_t poolPos = 0;          // next unused card in the shuffled pool
  std::string terms[kPairs];
  std::string defs[kPairs];
  uint8_t defOrder[kPairs] = {0, 1, 2, 3};  // slot -> pair
  uint8_t matched = 0;                      // bit per pair
  int selectedTerm = -1;
  int selectedSlot = -1;
  bool lastWrong = false;
  unsigned mistakes = 0;

  bool dealRound();
  void checkPair();
};
