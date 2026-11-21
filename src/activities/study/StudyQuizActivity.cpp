#include "StudyQuizActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "components/UITheme.h"

namespace {
constexpr int kOptionMaxLines = 3;
constexpr int kOptionMinLines = 2;
constexpr int kQuestionMinLines = 3;
}  // namespace

unsigned StudyQuizActivity::examLengthFor(const study::TrackInfo& track) {
  const unsigned wanted = track.exam.length > 0 ? track.exam.length : kDefaultExamLength;
  return wanted < track.questions ? wanted : track.questions;
}

void StudyQuizActivity::onEnter() {
  Activity::onEnter();
  state = State::Error;
  if (allocLineBuffer() && questions.open(std::string(study::kStudyRoot) + "/" + track.key + "/questions.jsonl")) {
    buildSession();
    if (order.empty()) {
      state = State::Empty;
    } else if (loadQuestion(order[0])) {
      state = State::Question;
    }
  }
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
        return true;
      }
      return false;
    }
    case State::Feedback:
      if (id != HitNext) return false;
      advance();
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
        return true;
      }
      if (id == HitRetry) {
        buildSession();
        state = !order.empty() && loadQuestion(order[0]) ? State::Question : State::Error;
        return true;
      }
      return false;
    case State::Review:
      if (id != HitNext) return false;
      ++reviewIndex;
      while (reviewIndex < missed.size() && !loadQuestion(order[missed[reviewIndex]])) ++reviewIndex;
      if (reviewIndex >= missed.size()) state = State::Results;
      return true;
    case State::Empty:
    case State::Error:
      if (id == HitDone) finish();
      return false;
  }
  return false;
}

void StudyQuizActivity::render(RenderLock&&) {
  clearHits();
  char subtitle[64];
  const char* title = mode == Mode::Quiz ? tr(STR_STUDY_QUIZ) : tr(STR_STUDY_PRACTICE_TEST);
  if (state == State::Question || state == State::Feedback) {
    const unsigned shown = static_cast<unsigned>(position + 1);
    const unsigned total = static_cast<unsigned>(order.size());
    if (mode == Mode::PracticeTest && shownMinutesLeft >= 0) {
      snprintf(subtitle, sizeof(subtitle), tr(STR_STUDY_PROGRESS_TIME_FORMAT), shown, total,
               static_cast<unsigned>(shownMinutesLeft));
    } else {
      snprintf(subtitle, sizeof(subtitle), "%u / %u", shown, total);
    }
  } else if (state == State::Review) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_STUDY_REVIEW_FORMAT), static_cast<unsigned>(reviewIndex + 1),
             static_cast<unsigned>(missed.size()));
  } else {
    snprintf(subtitle, sizeof(subtitle), "%s", categoryName() != nullptr ? categoryName() : track.label.c_str());
  }
  const auto layout = StudyUi::beginScreen(renderer, mappedInput, title, subtitle);
  switch (state) {
    case State::Question:
      renderQuestion(layout.content);
      break;
    case State::Feedback:
      renderFeedback(layout.content);
      break;
    case State::Results:
      renderResults(layout.content);
      break;
    case State::Review:
      renderReview(layout.content);
      break;
    case State::Empty:
    case State::Error: {
      const Rect& c = layout.content;
      const int body = StudyUi::bodyFont();
      UITheme::drawCenteredWrappedText(renderer, c, body, c.y + c.height / 3,
                                       state == State::Empty ? tr(STR_STUDY_NOTHING_HERE) : tr(STR_STUDY_LOAD_FAILED),
                                       4);
      const int h = renderer.getLineHeight(body) + StudyUi::kBoxPadding * 3;
      const Rect button{c.x, c.y + c.height - h, c.width, h};
      StudyUi::drawBox(renderer, button, body, tr(STR_STUDY_DONE), 1, StudyUi::BoxStyle::Normal, true);
      addHit(button, HitDone);
      break;
    }
  }
  renderer.displayBuffer();
}

void StudyQuizActivity::renderQuestion(const Rect& c) {
  const int body = StudyUi::bodyFont();
  const int lineHeight = renderer.getLineHeight(body);
  const bool needsSubmit = mode == Mode::PracticeTest || question.type == study::QuestionType::Multi;
  const int buttonHeight = lineHeight + StudyUi::kBoxPadding * 3;
  const int bottom = needsSubmit ? c.y + c.height - buttonHeight - StudyUi::kBoxGap : c.y + c.height;

  // Fit options first at up to 3 lines each; if the question would get fewer than 3 lines, squeeze options to 2.
  int optionLines = kOptionMaxLines;
  int heights[study::kMaxOptions] = {};
  int optionsHeight = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    optionsHeight = 0;
    for (uint8_t i = 0; i < question.optionCount; ++i) {
      heights[i] = StudyUi::boxHeight(renderer, body, c.width, question.options[i].c_str(), optionLines);
      optionsHeight += heights[i] + StudyUi::kBoxGap;
    }
    if ((bottom - c.y - optionsHeight) / lineHeight >= kQuestionMinLines) break;
    optionLines = kOptionMinLines;
  }

  int y = c.y;
  if (question.type == study::QuestionType::Multi) {
    y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, tr(STR_STUDY_SELECT_ALL), 1, EpdFontFamily::ITALIC);
  }
  const int questionLines = (bottom - y - optionsHeight) / lineHeight;
  y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, question.text.c_str(),
                              questionLines > 1 ? questionLines : 1, EpdFontFamily::BOLD);
  y += StudyUi::kBoxGap;
  for (uint8_t i = 0; i < question.optionCount; ++i) {
    const Rect box{c.x, y, c.width, heights[i]};
    const bool picked = (selection & (1u << i)) != 0;
    StudyUi::drawBox(renderer, box, body, question.options[i].c_str(), optionLines,
                     picked ? StudyUi::BoxStyle::Selected : StudyUi::BoxStyle::Normal);
    addHit(box, i);
    y += heights[i] + StudyUi::kBoxGap;
  }
  if (needsSubmit) {
    const Rect button{c.x, c.y + c.height - buttonHeight, c.width, buttonHeight};
    const char* label = mode == Mode::Quiz ? tr(STR_STUDY_CHECK) : tr(STR_STUDY_NEXT);
    StudyUi::drawBox(renderer, button, body, label, 1,
                     selection != 0 ? StudyUi::BoxStyle::Selected : StudyUi::BoxStyle::Dimmed, true);
    addHit(button, HitSubmit);
  }
}

void StudyQuizActivity::renderFeedback(const Rect& c) {
  const int body = StudyUi::bodyFont();
  const int lineHeight = renderer.getLineHeight(body);
  const int buttonHeight = lineHeight + StudyUi::kBoxPadding * 3;
  const int bottom = c.y + c.height - buttonHeight - StudyUi::kBoxGap;
  const bool right = study::isAnswerCorrect(question, selection);
  int y = c.y;
  y += StudyUi::drawParagraph(renderer, StudyUi::titleFont(), c.x, y, c.width,
                              right ? tr(STR_STUDY_CORRECT) : tr(STR_STUDY_INCORRECT), 1, EpdFontFamily::BOLD);
  y += StudyUi::kBoxGap;
  y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, question.text.c_str(), 3);
  y += StudyUi::kBoxGap;
  if (!right) {
    const std::string chosen = std::string(tr(STR_STUDY_YOUR_ANSWER)) + " " + optionsText(selection);
    y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, chosen.c_str(), 3);
  }
  const std::string answer = std::string(tr(STR_STUDY_RIGHT_ANSWER)) + " " + optionsText(question.answerMask);
  y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, answer.c_str(), 4, EpdFontFamily::BOLD);
  y += StudyUi::kBoxGap;
  const int whyLines = (bottom - y) / lineHeight;
  if (whyLines > 0) StudyUi::drawParagraph(renderer, body, c.x, y, c.width, question.why.c_str(), whyLines);
  const Rect button{c.x, c.y + c.height - buttonHeight, c.width, buttonHeight};
  const bool last = position + 1 >= order.size();
  StudyUi::drawBox(renderer, button, body, last ? tr(STR_STUDY_SEE_RESULTS) : tr(STR_STUDY_NEXT), 1,
                   StudyUi::BoxStyle::Selected, true);
  addHit(button, HitNext);
}

void StudyQuizActivity::renderResults(const Rect& c) {
  const int body = StudyUi::bodyFont();
  const int title = StudyUi::titleFont();
  const int lineHeight = renderer.getLineHeight(body);
  const unsigned right = score();
  const unsigned total = static_cast<unsigned>(order.size());
  const unsigned pct = total > 0 ? (right * 100 + total / 2) / total : 0;
  char line[96];
  int y = c.y + c.height / 6;
  if (mode == Mode::PracticeTest && shownMinutesLeft == 0) {
    y += UITheme::drawCenteredWrappedText(renderer, c, body, y, tr(STR_STUDY_TIME_UP), 1);
    y += StudyUi::kBoxGap;
  }
  snprintf(line, sizeof(line), tr(STR_STUDY_SCORE_FORMAT), right, total, pct);
  y += UITheme::drawCenteredWrappedText(renderer, c, title, y, line, 2, true, EpdFontFamily::BOLD);
  y += StudyUi::kBoxGap;
  if (mode == Mode::PracticeTest && track.exam.passPct > 0) {
    snprintf(line, sizeof(line), pct >= track.exam.passPct ? tr(STR_STUDY_PASS_FORMAT) : tr(STR_STUDY_FAIL_FORMAT),
             static_cast<unsigned>(track.exam.passPct));
    y += UITheme::drawCenteredWrappedText(renderer, c, body, y, line, 2);
  }

  const int buttonHeight = lineHeight + StudyUi::kBoxPadding * 3;
  const int halfWidth = (c.width - StudyUi::kBoxGap) / 2;
  const Rect left{c.x, c.y + c.height - buttonHeight, halfWidth, buttonHeight};
  const Rect rightBox{c.x + halfWidth + StudyUi::kBoxGap, left.y, halfWidth, buttonHeight};
  StudyUi::drawBox(renderer, left, body, tr(STR_STUDY_DONE), 1, StudyUi::BoxStyle::Normal, true);
  addHit(left, HitDone);
  if (!missed.empty()) {
    snprintf(line, sizeof(line), tr(STR_STUDY_REVIEW_MISSED_FORMAT), static_cast<unsigned>(missed.size()));
    StudyUi::drawBox(renderer, rightBox, body, line, 2, StudyUi::BoxStyle::Selected, true);
    addHit(rightBox, HitReview);
  } else {
    StudyUi::drawBox(renderer, rightBox, body, tr(STR_STUDY_TRY_AGAIN), 1, StudyUi::BoxStyle::Selected, true);
    addHit(rightBox, HitRetry);
  }
}

void StudyQuizActivity::renderReview(const Rect& c) {
  const int body = StudyUi::bodyFont();
  const int lineHeight = renderer.getLineHeight(body);
  const int buttonHeight = lineHeight + StudyUi::kBoxPadding * 3;
  const int bottom = c.y + c.height - buttonHeight - StudyUi::kBoxGap;
  const size_t pos = missed[reviewIndex];
  int y = c.y;
  y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, question.text.c_str(), 5, EpdFontFamily::BOLD);
  y += StudyUi::kBoxGap;
  const std::string chosen =
      std::string(tr(STR_STUDY_YOUR_ANSWER)) + " " +
      (chosenMasks[pos] != 0 ? optionsText(chosenMasks[pos]) : std::string(tr(STR_STUDY_NO_ANSWER)));
  y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, chosen.c_str(), 3);
  const std::string answer = std::string(tr(STR_STUDY_RIGHT_ANSWER)) + " " + optionsText(question.answerMask);
  y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, answer.c_str(), 4, EpdFontFamily::BOLD);
  y += StudyUi::kBoxGap;
  const int whyLines = (bottom - y) / lineHeight;
  if (whyLines > 0) StudyUi::drawParagraph(renderer, body, c.x, y, c.width, question.why.c_str(), whyLines);
  const Rect button{c.x, c.y + c.height - buttonHeight, c.width, buttonHeight};
  const bool last = reviewIndex + 1 >= missed.size();
  StudyUi::drawBox(renderer, button, body, last ? tr(STR_STUDY_SEE_RESULTS) : tr(STR_STUDY_NEXT), 1,
                   StudyUi::BoxStyle::Selected, true);
  addHit(button, HitNext);
}
