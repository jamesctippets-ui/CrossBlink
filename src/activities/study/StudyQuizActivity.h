#pragma once

#include <StudyPackStore.h>

#include <vector>

#include "StudyBookActivity.h"

// Quiz (short, answer shown after each question) and Practice test (the track's real length and time limit, no
// feedback until the end, then score, pass mark and a review of misses), read like a book: each question, its
// feedback and each reviewed miss is a short chapter that continues over as many pages as it needs. Every answer is
// logged for the app.
class StudyQuizActivity final : public StudyBookActivity {
 public:
  enum class Mode : uint8_t { Quiz, PracticeTest };
  static constexpr unsigned kQuizLength = 10;
  static constexpr unsigned kDefaultExamLength = 50;

  static unsigned examLengthFor(const study::TrackInfo& track);

  StudyQuizActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track, int category,
                    Mode mode)
      : StudyBookActivity("StudyQuiz", renderer, mappedInput, std::move(track), category), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  bool preventAutoSleep() override { return mode == Mode::PracticeTest && state == State::Question; }

 protected:
  bool onTap(int id) override;
  bool onTick() override;
  bool onPastEnd() override;
  bool onBeforeStart() override;
  bool choicePicked(int id) const override;
  bool choiceEnabled(int id) const override;
  void statusTitle(char* out, size_t size) const override;
  float statusProgress() const override;

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
  // Text composed for the current page; the page borrows it until the next buildPage().
  std::string chosenLine;
  std::string answerLine;
  char scoreLine[96] = "";
  char passLine[96] = "";
  char reviewLabel[64] = "";

  void buildSession();
  bool loadQuestion(uint16_t lineIndex);
  void submitCurrent();
  void advance();
  void finishSession();
  unsigned score() const;
  long minutesLeft() const;
  std::string optionsText(uint8_t mask) const;

  const char* modeTitle() const;
  void buildPage(bool focusLast = false);
  void buildQuestionPage();
  void buildAnswerPage(bool feedback);
  void buildResultsPage();
};
