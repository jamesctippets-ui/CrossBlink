#include "StudyMatchActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "components/UITheme.h"

namespace {
constexpr int kTermLines = 2;
constexpr uint8_t kAllMatched = (1u << StudyMatchActivity::kPairs) - 1;
}  // namespace

void StudyMatchActivity::onEnter() {
  Activity::onEnter();
  state = State::Error;
  if (allocLineBuffer() && cards.open(std::string(study::kStudyRoot) + "/" + track.key + "/cards.jsonl")) {
    pool.clear();
    pool.reserve(cards.count());
    for (size_t i = 0; i < cards.count() && i < 65535; ++i) {
      if (inCategory(cards.categoryAt(i))) pool.push_back(static_cast<uint16_t>(i));
    }
    if (pool.size() < kPairs) {
      state = State::Empty;
    } else {
      shuffle(pool);
      poolPos = 0;
      state = dealRound() ? State::Playing : State::Error;
    }
  }
  requestUpdate();
}

void StudyMatchActivity::onExit() {
  cards.close();
  pool.clear();
  pool.shrink_to_fit();
  for (auto& s : terms) s.clear();
  for (auto& s : defs) s.clear();
  freeLineBuffer();
  Activity::onExit();
}

bool StudyMatchActivity::dealRound() {
  study::Card card;
  uint8_t dealt = 0;
  size_t attempts = 0;
  while (dealt < kPairs && attempts < pool.size()) {
    if (poolPos >= pool.size()) {  // used every card: reshuffle and go round again
      shuffle(pool);
      poolPos = 0;
    }
    ++attempts;
    const size_t len = cards.readLine(pool[poolPos++], lineBuffer.get(), study::kLineBufferSize);
    if (len == 0 || !study::parseCardLine(lineBuffer.get(), len, card)) continue;
    bool duplicate = false;
    for (uint8_t i = 0; i < dealt; ++i) duplicate = duplicate || terms[i] == card.term;
    if (duplicate) continue;
    terms[dealt] = card.term;
    defs[dealt] = card.front();
    ++dealt;
  }
  if (dealt < kPairs) {
    LOG_ERR("STUDY", "Could only deal %u match pairs", static_cast<unsigned>(dealt));
    return false;
  }
  for (uint8_t i = 0; i < kPairs; ++i) defOrder[i] = i;
  for (uint8_t i = kPairs; i > 1; --i) {
    const uint8_t j = static_cast<uint8_t>(random(i));
    const uint8_t tmp = defOrder[i - 1];
    defOrder[i - 1] = defOrder[j];
    defOrder[j] = tmp;
  }
  matched = 0;
  selectedTerm = -1;
  selectedSlot = -1;
  lastWrong = false;
  mistakes = 0;
  return true;
}

void StudyMatchActivity::checkPair() {
  if (selectedTerm < 0 || selectedSlot < 0) return;
  if (defOrder[selectedSlot] == selectedTerm) {
    matched = static_cast<uint8_t>(matched | (1u << selectedTerm));
    lastWrong = false;
  } else {
    ++mistakes;
    lastWrong = true;
  }
  selectedTerm = -1;
  selectedSlot = -1;
  if (matched == kAllMatched) state = State::RoundDone;
}

bool StudyMatchActivity::onTap(const int id) {
  if (id == HitDone) {
    finish();
    return false;
  }
  if (id == HitNextRound && state == State::RoundDone) {
    state = dealRound() ? State::Playing : State::Error;
    return true;
  }
  if (state != State::Playing) return false;
  if (id >= HitTerm && id < HitTerm + kPairs) {
    const int pair = id - HitTerm;
    if (matched & (1u << pair)) return false;
    lastWrong = false;
    selectedTerm = selectedTerm == pair ? -1 : pair;
    checkPair();
    return true;
  }
  if (id >= HitDef && id < HitDef + kPairs) {
    const int slot = id - HitDef;
    if (matched & (1u << defOrder[slot])) return false;
    lastWrong = false;
    selectedSlot = selectedSlot == slot ? -1 : slot;
    checkPair();
    return true;
  }
  return false;
}

void StudyMatchActivity::render(RenderLock&&) {
  clearHits();
  const auto layout = StudyUi::beginScreen(renderer, mappedInput, tr(STR_STUDY_MATCH),
                                           categoryName() != nullptr ? categoryName() : track.label.c_str());
  const Rect& c = layout.content;
  const int body = StudyUi::bodyFont();
  const int lineHeight = renderer.getLineHeight(body);
  const int buttonHeight = lineHeight + StudyUi::kBoxPadding * 3;

  if (state == State::Empty || state == State::Error) {
    UITheme::drawCenteredWrappedText(renderer, c, body, c.y + c.height / 3,
                                     state == State::Empty ? tr(STR_STUDY_NOTHING_HERE) : tr(STR_STUDY_LOAD_FAILED), 4);
    const Rect button{c.x, c.y + c.height - buttonHeight, c.width, buttonHeight};
    StudyUi::drawBox(renderer, button, body, tr(STR_STUDY_DONE), 1, StudyUi::BoxStyle::Normal, true);
    addHit(button, HitDone);
    renderer.displayBuffer();
    return;
  }

  // Terms: a 2 x 2 grid of short boxes.
  const int gap = StudyUi::kBoxGap;
  const int termWidth = (c.width - gap) / 2;
  const int termHeight = lineHeight * kTermLines + StudyUi::kBoxPadding * 2;
  for (uint8_t i = 0; i < kPairs; ++i) {
    const Rect box{c.x + (i % 2) * (termWidth + gap), c.y + (i / 2) * (termHeight + gap), termWidth, termHeight};
    StudyUi::BoxStyle style = StudyUi::BoxStyle::Normal;
    if (matched & (1u << i)) {
      style = StudyUi::BoxStyle::Dimmed;
    } else if (selectedTerm == i) {
      style = StudyUi::BoxStyle::Selected;
    }
    StudyUi::drawBox(renderer, box, body, terms[i].c_str(), kTermLines, style, true);
    addHit(box, HitTerm + i);
  }

  // Status line and the bottom row are reserved; definitions share what is left equally.
  const int statusY = c.y + c.height - buttonHeight - gap - lineHeight;
  const int defsTop = c.y + 2 * (termHeight + gap) + gap;
  const int defHeight = (statusY - gap - defsTop - gap * (kPairs - 1)) / kPairs;
  const int defLines = (defHeight - StudyUi::kBoxPadding * 2) / (lineHeight > 0 ? lineHeight : 1);
  for (uint8_t slot = 0; slot < kPairs; ++slot) {
    const uint8_t pair = defOrder[slot];
    const Rect box{c.x, defsTop + slot * (defHeight + gap), c.width, defHeight};
    StudyUi::BoxStyle style = StudyUi::BoxStyle::Normal;
    if (matched & (1u << pair)) {
      style = StudyUi::BoxStyle::Dimmed;
    } else if (selectedSlot == slot) {
      style = StudyUi::BoxStyle::Selected;
    }
    StudyUi::drawBox(renderer, box, body, defs[pair].c_str(), defLines > 0 ? defLines : 1, style);
    addHit(box, HitDef + slot);
  }

  char status[96] = "";
  if (state == State::RoundDone) {
    snprintf(status, sizeof(status), tr(STR_STUDY_ROUND_DONE_FORMAT), mistakes);
  } else if (lastWrong) {
    snprintf(status, sizeof(status), "%s", tr(STR_STUDY_NOT_A_MATCH));
  }
  if (status[0] != '\0') {
    UITheme::drawCenteredText(renderer, c, body, statusY, status, true, EpdFontFamily::BOLD);
  }

  const int halfWidth = (c.width - gap) / 2;
  const Rect left{c.x, c.y + c.height - buttonHeight, halfWidth, buttonHeight};
  const Rect right{c.x + halfWidth + gap, left.y, halfWidth, buttonHeight};
  StudyUi::drawBox(renderer, left, body, tr(STR_STUDY_DONE), 1, StudyUi::BoxStyle::Normal, true);
  addHit(left, HitDone);
  StudyUi::drawBox(renderer, right, body, tr(STR_STUDY_NEXT_ROUND), 1,
                   state == State::RoundDone ? StudyUi::BoxStyle::Selected : StudyUi::BoxStyle::Dimmed, true);
  if (state == State::RoundDone) addHit(right, HitNextRound);
  renderer.displayBuffer();
}
