#pragma once

#include <StudyPackStore.h>

#include <vector>

#include "StudyScreenActivity.h"

// Quiz (short, answer shown after each question) and Practice test (the track's real length and time limit, no
// feedback until the end, then score, pass mark and a review of misses). Every answer is logged for the app.
class StudyQuizActivity final : public StudyScreenActivity {
 public:
  enum class Mode : uint8_t { Quiz, PracticeTest };
  static constexpr unsigned kQuizLength = 10;
  static constexpr unsigned kDefaultExamLength = 50;

  static unsigned examLengthFor(const study::TrackInfo& track);

  StudyQuizActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track, int category,
                    Mode mode)
      : StudyScreenActivity("StudyQuiz", renderer, mappedInput, std::move(track), category), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return mode == Mode::PracticeTest && state == State::Question; }

 protected:
  bool onTap(int id) override;
  bool onTick() override;

 private:
  enum class State : uint8_t { Question, Feedback, Results, Review, Empty, Error };
  // Option boxes use ids 0..kMaxOptions-1.
  enum HitId : int { HitSubmit = 100, HitNext, HitReview, HitDone, HitRetry };

  Mode mode;
  State state = State::Question;
  study::JsonlFile questions;
  study::ProgressLog log;
  study::Question question;
  std::vector<uint16_t> order;       // line indexes for this session
  std::vector<uint8_t> chosenMasks;  // per position; 0 = unanswered
  std::vector<uint8_t> answerMasks;  // per position, saved when answered for the score and review
  std::vector<uint16_t> missed;      // positions answered wrong or left blank (filled at the end)
  size_t position = 0;
  size_t reviewIndex = 0;
  uint8_t selection = 0;  // options picked on the current question
  unsigned long startMs = 0;
  unsigned long limitMs = 0;
  long shownMinutesLeft = -1;

  void buildSession();
  bool loadQuestion(uint16_t lineIndex);
  void submitCurrent();
  void advance();
  void finishSession();
  unsigned score() const;
  long minutesLeft() const;
  std::string optionsText(uint8_t mask) const;

  void renderQuestion(const Rect& c);
  void renderFeedback(const Rect& c);
  void renderResults(const Rect& c);
  void renderReview(const Rect& c);
};
