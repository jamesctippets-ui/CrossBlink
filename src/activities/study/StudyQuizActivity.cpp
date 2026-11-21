#include "StudyQuizActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

unsigned StudyQuizActivity::examLengthFor(const study::TrackInfo& track) {
  const unsigned wanted = track.exam.length > 0 ? track.exam.length : kDefaultExamLength;
  return wanted < track.questions ? wanted : track.questions;
}

void StudyQuizActivity::onEnter() {
  StudyBookActivity::onEnter();
  state = State::Error;
  if (allocLineBuffer() && questions.open(std::string(study::kStudyRoot) + "/" + track.key + "/questions.jsonl")) {
    buildSession();
    if (order.empty()) {
      state = State::Empty;
    } else if (loadQuestion(order[0])) {
      state = State::Question;
    }
  }
  buildPage();
  requestUpdate();
}

void StudyQuizActivity::onExit() {
  questions.close();
  order.clear();
  order.shrink_to_fit();
  chosenMasks.clear();
  chosenMasks.shrink_to_fit();
  answerMasks.clear();
  answerMasks.shrink_to_fit();
  missed.clear();
  missed.shrink_to_fit();
  freeLineBuffer();
  Activity::onExit();
}

void StudyQuizActivity::buildSession() {
  std::vector<uint16_t> pool;
  pool.reserve(questions.count());
  for (size_t i = 0; i < questions.count() && i < 65535; ++i) {
    if (inCategory(questions.categoryAt(i))) pool.push_back(static_cast<uint16_t>(i));
  }
  shuffle(pool);
  const unsigned wanted = mode == Mode::Quiz ? kQuizLength : examLengthFor(track);
  if (pool.size() > wanted) pool.resize(wanted);
  order = std::move(pool);
  chosenMasks.assign(order.size(), 0);
  answerMasks.assign(order.size(), 0);
  missed.clear();
  position = 0;
  reviewIndex = 0;
  selection = 0;
  startMs = millis();
  limitMs = mode == Mode::PracticeTest ? static_cast<unsigned long>(track.exam.minutes) * 60000UL : 0;
  shownMinutesLeft = minutesLeft();
}

bool StudyQuizActivity::loadQuestion(const uint16_t lineIndex) {
  const size_t len = questions.readLine(lineIndex, lineBuffer.get(), study::kLineBufferSize);
  if (len == 0 || !study::parseQuestionLine(lineBuffer.get(), len, question)) {
    LOG_ERR("STUDY", "Question line %u is unreadable", static_cast<unsigned>(lineIndex));
    return false;
  }
  return true;
}

long StudyQuizActivity::minutesLeft() const {
  if (limitMs == 0) return -1;
  const unsigned long elapsed = millis() - startMs;
  if (elapsed >= limitMs) return 0;
  return static_cast<long>((limitMs - elapsed + 59999UL) / 60000UL);
}

unsigned StudyQuizActivity::score() const {
  unsigned right = 0;
  for (size_t i = 0; i < order.size(); ++i) {
    if (chosenMasks[i] != 0 && chosenMasks[i] == answerMasks[i]) ++right;
  }
  return right;
}

std::string StudyQuizActivity::optionsText(const uint8_t mask) const {
  std::string text;
  for (uint8_t i = 0; i < question.optionCount; ++i) {
    if (!(mask & (1u << i))) continue;
    if (!text.empty()) text += "; ";
    text += question.options[i];
  }
  return text;
}

void StudyQuizActivity::submitCurrent() {
  if (selection == 0 || position >= order.size()) return;
  chosenMasks[position] = selection;
  answerMasks[position] = question.answerMask;
  log.logQuestion(track.key, question.id, study::isAnswerCorrect(question, selection));
}

void StudyQuizActivity::advance() {
  ++position;
  selection = 0;
  // A line that fails to read is skipped, and counts as unanswered.
  while (position < order.size() && !loadQuestion(order[position])) ++position;
  if (position >= order.size()) {
    finishSession();
  } else {
    state = State::Question;
  }
}

void StudyQuizActivity::finishSession() {
  missed.clear();
  for (size_t i = 0; i < order.size(); ++i) {
    if (chosenMasks[i] == 0 || chosenMasks[i] != answerMasks[i]) missed.push_back(static_cast<uint16_t>(i));
  }
  state = State::Results;
}

bool StudyQuizActivity::onTick() {
  if (mode != Mode::PracticeTest || state != State::Question || limitMs == 0) return false;
  const long left = minutesLeft();
  if (left == shownMinutesLeft) return false;
  RenderLock lock(*this);
  shownMinutesLeft = left;
  if (left == 0) {
    LOG_INF("STUDY", "Practice test time is up at question %u", static_cast<unsigned>(position + 1));
    finishSession();
    buildPage();
  }
  return true;
}

bool StudyQuizActivity::onTap(const int id) {
  switch (state) {
    case State::Question: {
      if (id >= 0 && id < question.optionCount) {
        const uint8_t bit = static_cast<uint8_t>(1u << id);
        if (question.type == study::QuestionType::Multi) {
          selection = static_cast<uint8_t>(selection ^ bit);
          return true;
        }
        selection = bit;
        if (mode == Mode::Quiz) {
          submitCurrent();
          state = State::Feedback;
          buildPage();
        } else {
          // Practice test: the pick is marked and OK on Next moves on, so a choice can still be changed.
          focusChoice(HitSubmit);
        }
        return true;
      }
      if (id == HitSubmit && selection != 0) {
        submitCurrent();
        if (mode == Mode::Quiz) {
          state = State::Feedback;
        } else {
          advance();
        }
        buildPage();
        return true;
      }
      return false;
    }
    case State::Feedback:
      if (id != HitNext) return false;
      advance();
      buildPage();
      return true;
    case State::Results:
      if (id == HitDone) {
        finish();
        return false;
      }
      if (id == HitReview && !missed.empty()) {
        reviewIndex = 0;
        if (!loadQuestion(order[missed[0]])) return false;
        state = State::Review;
        buildPage();
        return true;
      }
      if (id == HitRetry) {
        buildSession();
        state = !order.empty() && loadQuestion(order[0]) ? State::Question : State::Error;
        buildPage();
        return true;
      }
      return false;
    case State::Review:
      if (id != HitNext) return false;
      ++reviewIndex;
      while (reviewIndex < missed.size() && !loadQuestion(order[missed[reviewIndex]])) ++reviewIndex;
      if (reviewIndex >= missed.size()) state = State::Results;
      buildPage();
      return true;
    case State::Empty:
    case State::Error:
      if (id == HitDone) finish();
      return false;
  }
  return false;
}

bool StudyQuizActivity::onPastEnd() {
  // Turning past the last page of feedback or a reviewed miss goes on, like the Next button.
  if (state == State::Feedback || state == State::Review) return onTap(HitNext);
  return false;
}

bool StudyQuizActivity::onBeforeStart() {
  // In the review, paging back from a miss goes to the previous one.
  if (state != State::Review || reviewIndex == 0) return false;
  size_t index = reviewIndex;
  while (index > 0) {
    --index;
    if (loadQuestion(order[missed[index]])) {
      reviewIndex = index;
      buildPage(true);
      return true;
    }
  }
  // Nothing earlier could be read; reload the current one so the page still matches.
  loadQuestion(order[missed[reviewIndex]]);
  return false;
}

bool StudyQuizActivity::choicePicked(const int id) const {
  return state == State::Question && id >= 0 && id < question.optionCount && (selection & (1u << id)) != 0;
}

bool StudyQuizActivity::choiceEnabled(const int id) const { return id != HitSubmit || selection != 0; }

const char* StudyQuizActivity::modeTitle() const {
  return mode == Mode::Quiz ? tr(STR_STUDY_QUIZ) : tr(STR_STUDY_PRACTICE_TEST);
}

void StudyQuizActivity::statusTitle(char* out, const size_t size) const {
  char progress[64];
  if (state == State::Question || state == State::Feedback) {
    const unsigned shown = static_cast<unsigned>(position + 1);
    const unsigned total = static_cast<unsigned>(order.size());
    if (mode == Mode::PracticeTest && shownMinutesLeft >= 0) {
      snprintf(progress, sizeof(progress), tr(STR_STUDY_PROGRESS_TIME_FORMAT), shown, total,
               static_cast<unsigned>(shownMinutesLeft));
    } else {
      snprintf(progress, sizeof(progress), "%u / %u", shown, total);
    }
    snprintf(out, size, "%s %s", modeTitle(), progress);
  } else if (state == State::Review) {
    snprintf(progress, sizeof(progress), tr(STR_STUDY_REVIEW_FORMAT), static_cast<unsigned>(reviewIndex + 1),
             static_cast<unsigned>(missed.size()));
    snprintf(out, size, "%s, %s", modeTitle(), progress);
  } else if (categoryName() != nullptr) {
    snprintf(out, size, "%s, %s", modeTitle(), categoryName());
  } else {
    snprintf(out, size, "%s", modeTitle());
  }
}

float StudyQuizActivity::statusProgress() const {
  switch (state) {
    case State::Question:
    case State::Feedback:
      return order.empty() ? 0.0f : static_cast<float>(position) * 100.0f / static_cast<float>(order.size());
    case State::Review:
      return missed.empty() ? 100.0f : static_cast<float>(reviewIndex) * 100.0f / static_cast<float>(missed.size());
    case State::Results:
      return 100.0f;
    case State::Empty:
    case State::Error:
      break;
  }
  return 0.0f;
}

void StudyQuizActivity::buildPage(const bool focusLast) {
  beginPage();
  switch (state) {
    case State::Question:
      buildQuestionPage();
      break;
    case State::Feedback:
      buildAnswerPage(true);
      break;
    case State::Review:
      buildAnswerPage(false);
      break;
    case State::Results:
      buildResultsPage();
      break;
    case State::Empty:
    case State::Error:
      addText(state == State::Empty ? tr(STR_STUDY_NOTHING_HERE) : tr(STR_STUDY_LOAD_FAILED), study::TextStyle::Regular,
              false);
      addAction(tr(STR_STUDY_DONE), HitDone);
      break;
  }
  endPage(focusLast);
}

void StudyQuizActivity::buildQuestionPage() {
  const bool multi = question.type == study::QuestionType::Multi;
  if (multi) addText(tr(STR_STUDY_SELECT_ALL), study::TextStyle::Italic, false);
  addText(question.text.c_str(), study::TextStyle::Bold, multi);
  for (uint8_t i = 0; i < question.optionCount; ++i) addOption(question.options[i].c_str(), i);
  if (mode == Mode::PracticeTest || multi) {
    addAction(mode == Mode::Quiz ? tr(STR_STUDY_CHECK) : tr(STR_STUDY_NEXT), HitSubmit);
  }
}

void StudyQuizActivity::buildAnswerPage(const bool feedback) {
  const uint8_t chosenMask = feedback ? selection : chosenMasks[missed[reviewIndex]];
  const bool right = chosenMask != 0 && study::isAnswerCorrect(question, chosenMask);
  if (feedback) {
    addText(right ? tr(STR_STUDY_CORRECT) : tr(STR_STUDY_INCORRECT), study::TextStyle::Bold, false);
    addText(question.text.c_str());
  } else {
    addText(question.text.c_str(), study::TextStyle::Bold, false);
  }
  if (!right) {
    chosenLine = std::string(tr(STR_STUDY_YOUR_ANSWER)) + " " +
                 (chosenMask != 0 ? optionsText(chosenMask) : std::string(tr(STR_STUDY_NO_ANSWER)));
    addText(chosenLine.c_str());
  }
  answerLine = std::string(tr(STR_STUDY_RIGHT_ANSWER)) + " " + optionsText(question.answerMask);
  addText(answerLine.c_str(), study::TextStyle::Bold);
  addText(question.why.c_str());
  const bool last = feedback ? position + 1 >= order.size() : reviewIndex + 1 >= missed.size();
  addAction(last ? tr(STR_STUDY_SEE_RESULTS) : tr(STR_STUDY_NEXT), HitNext);
}

void StudyQuizActivity::buildResultsPage() {
  const unsigned right = score();
  const unsigned total = static_cast<unsigned>(order.size());
  const unsigned pct = total > 0 ? (right * 100 + total / 2) / total : 0;
  const bool timeUp = mode == Mode::PracticeTest && shownMinutesLeft == 0;
  if (timeUp) addText(tr(STR_STUDY_TIME_UP), study::TextStyle::Italic, false);
  snprintf(scoreLine, sizeof(scoreLine), tr(STR_STUDY_SCORE_FORMAT), right, total, pct);
  addText(scoreLine, study::TextStyle::Bold, timeUp);
  if (mode == Mode::PracticeTest && track.exam.passPct > 0) {
    snprintf(passLine, sizeof(passLine),
             pct >= track.exam.passPct ? tr(STR_STUDY_PASS_FORMAT) : tr(STR_STUDY_FAIL_FORMAT),
             static_cast<unsigned>(track.exam.passPct));
    addText(passLine);
  }
  if (!missed.empty()) {
    snprintf(reviewLabel, sizeof(reviewLabel), tr(STR_STUDY_REVIEW_MISSED_FORMAT),
             static_cast<unsigned>(missed.size()));
    addAction(reviewLabel, HitReview);
  } else {
    addAction(tr(STR_STUDY_TRY_AGAIN), HitRetry);
  }
  addAction(tr(STR_STUDY_DONE), HitDone);
}
