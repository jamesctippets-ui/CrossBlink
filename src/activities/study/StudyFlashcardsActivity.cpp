#include "StudyFlashcardsActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

namespace {
constexpr unsigned kSaveEvery = 5;  // debounce schedule writes; the progress log itself is appended per rating
}

void StudyFlashcardsActivity::onEnter() {
  StudyBookActivity::onEnter();
  state = State::Error;
  if (allocLineBuffer() && cards.open(std::string(study::kStudyRoot) + "/" + track.key + "/cards.jsonl", true)) {
    srs.load(track.key);
    buildSession();
    state = session.empty() ? State::Empty : State::Front;
    if (state == State::Front && !loadCurrent()) state = State::Error;
  }
  buildPage();
  requestUpdate();
}

void StudyFlashcardsActivity::onExit() {
  srs.save();
  cards.close();
  session.clear();
  session.shrink_to_fit();
  freeLineBuffer();
  Activity::onExit();
}

void StudyFlashcardsActivity::buildSession() {
  struct Candidate {
    uint16_t index;
    uint32_t due;  // 0 = never rated, which sorts first like the app's NEVER_RATED
  };
  std::vector<uint16_t> pool;
  pool.reserve(cards.count());
  for (size_t i = 0; i < cards.count() && i < 65535; ++i) {
    if (inCategory(cards.categoryAt(i))) pool.push_back(static_cast<uint16_t>(i));
  }
  shuffle(pool);  // ties keep a random order, as the app's seeded shuffle does
  std::vector<Candidate> ranked;
  ranked.reserve(pool.size());
  for (const uint16_t index : pool) {
    const study::SrsEntry* entry = srs.find(cards.idAt(index));
    const uint32_t due = entry != nullptr ? (entry->due > 0 ? entry->due : 1) : 0;
    ranked.push_back(Candidate{index, due});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Candidate& a, const Candidate& b) { return a.due < b.due; });
  session.clear();
  const size_t take = std::min(kSessionLength, ranked.size());
  session.reserve(take);
  for (size_t i = 0; i < take; ++i) session.push_back(ranked[i].index);
  position = 0;
  gotIt = 0;
  missed = 0;
}

bool StudyFlashcardsActivity::loadCurrent() {
  if (position >= session.size()) return false;
  const size_t len = cards.readLine(session[position], lineBuffer.get(), study::kLineBufferSize);
  if (len == 0 || !study::parseCardLine(lineBuffer.get(), len, card)) {
    LOG_ERR("STUDY", "Card line %u is unreadable", static_cast<unsigned>(session[position]));
    return false;
  }
  return true;
}

void StudyFlashcardsActivity::rate(const int quality) {
  const study::SrsEntry* prev = srs.find(card.id);
  const study::SrsEntry next =
      study::nextSrs(prev != nullptr ? *prev : study::SrsEntry{}, quality, study::nowUnixSeconds());
  srs.set(card.id, next);
  log.logCard(track.key, card.id, quality);
  if (quality >= 3) {
    ++gotIt;
  } else {
    ++missed;
  }
  if (++ratingsSinceSave >= kSaveEvery) {
    srs.save();
    ratingsSinceSave = 0;
  }
  ++position;
  // Skip a card that fails to read rather than ending the session.
  while (position < session.size() && !loadCurrent()) ++position;
  state = position < session.size() ? State::Front : State::Summary;
  if (state == State::Summary) srs.save();
}

void StudyFlashcardsActivity::buildPage(const bool focusLast) {
  beginPage();
  switch (state) {
    case State::Front:
      addText(card.front().c_str(), study::TextStyle::Regular, false);
      addAction(tr(STR_STUDY_SHOW_ANSWER), HitReveal);
      break;
    case State::Back:
      addText(card.term.c_str(), study::TextStyle::Bold, false);
      addText(card.def.c_str());
      addText(card.example.c_str(), study::TextStyle::Italic);
      // Don't got it comes first so a stray OK press does not inflate the score.
      addAction(tr(STR_STUDY_DONT_GOT_IT), HitMissed);
      addAction(tr(STR_STUDY_GOT_IT), HitGotIt);
      break;
    case State::Summary:
      snprintf(summary, sizeof(summary), tr(STR_STUDY_CARDS_SUMMARY_FORMAT), gotIt, missed);
      addText(tr(STR_STUDY_SESSION_DONE), study::TextStyle::Bold, false);
      addText(summary);
      addAction(tr(STR_STUDY_STUDY_MORE), HitMore);
      addAction(tr(STR_STUDY_DONE), HitDone);
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

bool StudyFlashcardsActivity::onTap(const int id) {
  switch (id) {
    case HitReveal:
      if (state != State::Front) return false;
      state = State::Back;
      buildPage();
      return true;
    case HitGotIt:
    case HitMissed:
      if (state != State::Back) return false;
      rate(id == HitGotIt ? study::kQualityGotIt : study::kQualityMissed);
      buildPage();
      return true;
    case HitMore:
      buildSession();
      state = session.empty() ? State::Empty : (loadCurrent() ? State::Front : State::Error);
      buildPage();
      return true;
    case HitDone:
      finish();
      return false;
    default:
      return false;
  }
}

bool StudyFlashcardsActivity::onPastEnd() {
  // Turning past the prompt shows the answer, like turning a flashcard over.
  return onTap(HitReveal);
}

bool StudyFlashcardsActivity::onBeforeStart() {
  // Paging back from the answer shows the prompt again, with the focus on Show answer.
  if (state != State::Back) return false;
  state = State::Front;
  buildPage(true);
  return true;
}

void StudyFlashcardsActivity::statusTitle(char* out, const size_t size) const {
  if (state == State::Front || state == State::Back) {
    snprintf(out, size, "%s %u / %u", tr(STR_STUDY_FLASHCARDS), static_cast<unsigned>(position + 1),
             static_cast<unsigned>(session.size()));
  } else if (categoryName() != nullptr) {
    snprintf(out, size, "%s, %s", tr(STR_STUDY_FLASHCARDS), categoryName());
  } else {
    snprintf(out, size, "%s", tr(STR_STUDY_FLASHCARDS));
  }
}

float StudyFlashcardsActivity::statusProgress() const {
  if (state == State::Summary) return 100.0f;
  if (session.empty()) return 0.0f;
  return static_cast<float>(position) * 100.0f / static_cast<float>(session.size());
}
