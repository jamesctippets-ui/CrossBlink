#include "StudyTrackActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "StudyFlashcardsActivity.h"
#include "StudyMatchActivity.h"
#include "StudyQuizActivity.h"

void StudyTrackActivity::onEnter() {
  buildRows();
  StudyListActivity::onEnter();
}

const char* StudyTrackActivity::subtitle() const {
  if (category >= 0 && category < static_cast<int>(track.categories.size())) {
    return track.categories[category].c_str();
  }
  return track.subtitle.empty() ? nullptr : track.subtitle.c_str();
}

void StudyTrackActivity::buildRows() {
  snprintf(cardsValue, sizeof(cardsValue), tr(STR_STUDY_CARDS_FORMAT), track.cards);
  snprintf(quizValue, sizeof(quizValue), tr(STR_STUDY_QUESTIONS_FORMAT),
           static_cast<unsigned>(StudyQuizActivity::kQuizLength));
  const unsigned examLength = StudyQuizActivity::examLengthFor(track);
  if (track.exam.minutes > 0) {
    snprintf(examValue, sizeof(examValue), tr(STR_STUDY_EXAM_FORMAT), examLength,
             static_cast<unsigned>(track.exam.minutes));
  } else {
    snprintf(examValue, sizeof(examValue), tr(STR_STUDY_QUESTIONS_FORMAT), examLength);
  }

  rows.assign(RowCount, freeink::ui::ListItem{});
  rows[RowFlashcards].label = tr(STR_STUDY_FLASHCARDS);
  rows[RowFlashcards].value = cardsValue;
  rows[RowFlashcards].enabled = track.cards > 0;
  rows[RowQuiz].label = tr(STR_STUDY_QUIZ);
  rows[RowQuiz].value = quizValue;
  rows[RowQuiz].enabled = track.questions > 0;
  rows[RowPracticeTest].label = tr(STR_STUDY_PRACTICE_TEST);
  rows[RowPracticeTest].value = examValue;
  rows[RowPracticeTest].enabled = track.questions > 0;
  rows[RowMatch].label = tr(STR_STUDY_MATCH);
  rows[RowMatch].enabled = track.cards >= StudyMatchActivity::kPairs;
  rows[RowCategory].label = tr(STR_STUDY_CATEGORY);
  rows[RowCategory].value = category >= 0 && category < static_cast<int>(track.categories.size())
                                ? track.categories[category].c_str()
                                : tr(STR_STUDY_ALL_CATEGORIES);
  rows[RowCategory].enabled = !track.categories.empty();
}

void StudyTrackActivity::onRowChosen(const int index) {
  if (index == RowCategory) {
    // Cycle All -> each category -> All. Tracks have at most a handful of categories.
    category = category + 1 >= static_cast<int>(track.categories.size()) ? -1 : category + 1;
    buildRows();
    refreshRows();
    return;
  }
  if (index >= 0 && index < RowCount && !rows[index].enabled) return;
  openMode(index);
}

void StudyTrackActivity::openMode(const int row) {
  std::unique_ptr<Activity> next;
  switch (row) {
    case RowFlashcards:
      next = makeUniqueNoThrow<StudyFlashcardsActivity>(renderer, mappedInput, track, category);
      break;
    case RowQuiz:
      next =
          makeUniqueNoThrow<StudyQuizActivity>(renderer, mappedInput, track, category, StudyQuizActivity::Mode::Quiz);
      break;
    case RowPracticeTest:
      next = makeUniqueNoThrow<StudyQuizActivity>(renderer, mappedInput, track, category,
                                                  StudyQuizActivity::Mode::PracticeTest);
      break;
    case RowMatch:
      next = makeUniqueNoThrow<StudyMatchActivity>(renderer, mappedInput, track, category);
      break;
    default:
      return;
  }
  if (!next) {
    LOG_ERR("STUDY", "Cannot allocate study mode %d", row);
    return;
  }
  startActivityForResult(std::move(next), [this](const ActivityResult&) { requestUpdate(); });
}
