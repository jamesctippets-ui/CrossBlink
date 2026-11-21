#include "StudyFlashcardsActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"

namespace {
constexpr unsigned kSaveEvery = 5;  // debounce schedule writes; the progress log itself is appended per rating
}

void StudyFlashcardsActivity::onEnter() {
  Activity::onEnter();
  state = State::Error;
  if (allocLineBuffer() && cards.open(std::string(study::kStudyRoot) + "/" + track.key + "/cards.jsonl", true)) {
    srs.load(track.key);
    buildSession();
    state = session.empty() ? State::Empty : State::Front;
    if (state == State::Front && !loadCurrent()) state = State::Error;
  }
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

bool StudyFlashcardsActivity::onTap(const int id) {
  switch (id) {
    case HitReveal:
      if (state != State::Front) return false;
      state = State::Back;
      return true;
    case HitGotIt:
    case HitMissed:
      if (state != State::Back) return false;
      rate(id == HitGotIt ? study::kQualityGotIt : study::kQualityMissed);
      return true;
    case HitMore:
      buildSession();
      state = session.empty() ? State::Empty : (loadCurrent() ? State::Front : State::Error);
      return true;
    case HitDone:
      finish();
      return false;
    default:
      return false;
  }
}

void StudyFlashcardsActivity::render(RenderLock&&) {
  clearHits();
  char subtitle[64];
  if (state == State::Front || state == State::Back) {
    snprintf(subtitle, sizeof(subtitle), "%u / %u", static_cast<unsigned>(position + 1),
             static_cast<unsigned>(session.size()));
  } else {
    snprintf(subtitle, sizeof(subtitle), "%s", categoryName() != nullptr ? categoryName() : track.label.c_str());
  }
  const auto layout = StudyUi::beginScreen(renderer, mappedInput, tr(STR_STUDY_FLASHCARDS), subtitle);
  const Rect& c = layout.content;
  const int body = StudyUi::bodyFont();
  const int title = StudyUi::titleFont();
  const int buttonHeight = renderer.getLineHeight(body) + StudyUi::kBoxPadding * 3;
  const Rect bottomRow{c.x, c.y + c.height - buttonHeight, c.width, buttonHeight};
  const int halfWidth = (c.width - StudyUi::kBoxGap) / 2;
  const Rect leftButton{bottomRow.x, bottomRow.y, halfWidth, buttonHeight};
  const Rect rightButton{bottomRow.x + halfWidth + StudyUi::kBoxGap, bottomRow.y, halfWidth, buttonHeight};
  const int textBottom = bottomRow.y - StudyUi::kBoxGap;
  const int lineHeight = renderer.getLineHeight(body);

  switch (state) {
    case State::Front: {
      const int maxLines = (textBottom - c.y) / lineHeight;
      StudyUi::drawParagraph(renderer, title, c.x, c.y, c.width, card.front().c_str(), maxLines);
      // The whole card area reveals the answer, plus an explicit button.
      addHit(Rect{c.x, c.y, c.width, textBottom - c.y}, HitReveal);
      StudyUi::drawBox(renderer, bottomRow, body, tr(STR_STUDY_SHOW_ANSWER), 1, StudyUi::BoxStyle::Selected, true);
      addHit(bottomRow, HitReveal);
      break;
    }
    case State::Back: {
      int y = c.y;
      y += StudyUi::drawParagraph(renderer, title, c.x, y, c.width, card.term.c_str(), 3, EpdFontFamily::BOLD);
      y += StudyUi::kBoxGap;
      int remaining = (textBottom - y) / lineHeight;
      const int used = StudyUi::drawParagraph(renderer, body, c.x, y, c.width, card.def.c_str(), remaining);
      y += used + StudyUi::kBoxGap;
      remaining = (textBottom - y) / lineHeight;
      if (!card.example.empty() && remaining > 0) {
        StudyUi::drawParagraph(renderer, body, c.x, y, c.width, card.example.c_str(), remaining, EpdFontFamily::ITALIC);
      }
      StudyUi::drawBox(renderer, leftButton, body, tr(STR_STUDY_DONT_GOT_IT), 1, StudyUi::BoxStyle::Normal, true);
      StudyUi::drawBox(renderer, rightButton, body, tr(STR_STUDY_GOT_IT), 1, StudyUi::BoxStyle::Selected, true);
      addHit(leftButton, HitMissed);
      addHit(rightButton, HitGotIt);
      break;
    }
    case State::Summary: {
      char line[96];
      snprintf(line, sizeof(line), tr(STR_STUDY_CARDS_SUMMARY_FORMAT), gotIt, missed);
      UITheme::drawCenteredWrappedText(renderer, c, title, c.y + c.height / 4, tr(STR_STUDY_SESSION_DONE), 2, true,
                                       EpdFontFamily::BOLD);
      UITheme::drawCenteredWrappedText(renderer, c, body, c.y + c.height / 4 + lineHeight * 3, line, 3);
      StudyUi::drawBox(renderer, leftButton, body, tr(STR_STUDY_DONE), 1, StudyUi::BoxStyle::Normal, true);
      StudyUi::drawBox(renderer, rightButton, body, tr(STR_STUDY_STUDY_MORE), 1, StudyUi::BoxStyle::Selected, true);
      addHit(leftButton, HitDone);
      addHit(rightButton, HitMore);
      break;
    }
    case State::Empty:
    case State::Error:
      UITheme::drawCenteredWrappedText(renderer, c, body, c.y + c.height / 3,
                                       state == State::Empty ? tr(STR_STUDY_NOTHING_HERE) : tr(STR_STUDY_LOAD_FAILED),
                                       4);
      StudyUi::drawBox(renderer, bottomRow, body, tr(STR_STUDY_DONE), 1, StudyUi::BoxStyle::Normal, true);
      addHit(bottomRow, HitDone);
      break;
  }
  renderer.displayBuffer();
}
