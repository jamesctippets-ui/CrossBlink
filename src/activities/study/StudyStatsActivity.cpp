#include "StudyStatsActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <StudyPackStore.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "StudyHomeActivity.h"
#include "StudyTrackActivity.h"
#include "components/UITheme.h"

namespace {

constexpr int kTileCount = 3;
constexpr int kCornerRadius = 8;

// SETTINGS.clockUtcOffsetQ is biased quarter hours: 48 = UTC+0, 0 = UTC-12.
int32_t utcOffsetSeconds() { return (static_cast<int32_t>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60; }

void drawTile(const GfxRenderer& renderer, const Rect& rect, const char* value, const char* label) {
  const int title = StudyUi::titleFont();
  const int body = StudyUi::bodyFont();
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, kCornerRadius, true);
  const int valueHeight = renderer.getLineHeight(title);
  const int labelHeight = renderer.getLineHeight(body);
  const int top = rect.y + (rect.height - valueHeight - labelHeight) / 2;
  const auto valueText = renderer.truncatedText(title, value, rect.width - StudyUi::kBoxPadding, EpdFontFamily::BOLD);
  renderer.drawText(title,
                    rect.x + (rect.width - renderer.getTextWidth(title, valueText.c_str(), EpdFontFamily::BOLD)) / 2,
                    top, valueText.c_str(), true, EpdFontFamily::BOLD);
  const auto labelText = renderer.truncatedText(body, label, rect.width - StudyUi::kBoxPadding);
  renderer.drawText(body, rect.x + (rect.width - renderer.getTextWidth(body, labelText.c_str())) / 2, top + valueHeight,
                    labelText.c_str());
}

}  // namespace

void StudyStatsActivity::onEnter() {
  Activity::onEnter();
  reload();
  requestUpdate();
}

void StudyStatsActivity::onExit() {
  tracks.clear();
  tracks.shrink_to_fit();
  perTrack.clear();
  perTrack.shrink_to_fit();
  order.clear();
  order.shrink_to_fit();
  Activity::onExit();
}

void StudyStatsActivity::reload() {
  // Read everything into locals first (SD access), then swap in under the render lock so a render in progress never
  // sees half-updated vectors.
  std::vector<study::TrackInfo> newTracks;
  std::vector<study::TrackStudyStats> newPerTrack;
  const bool pack = study::loadManifest(newTracks);
  study::StudyTally tally(study::nowUnixSeconds(), utcOffsetSeconds());
  study::loadStudyStats(newTracks, tally, newPerTrack);

  std::vector<uint16_t> newOrder;
  newOrder.reserve(newTracks.size());
  for (size_t i = 0; i < newTracks.size() && i < UINT16_MAX; ++i) newOrder.push_back(static_cast<uint16_t>(i));
  std::stable_sort(newOrder.begin(), newOrder.end(), [&newPerTrack](const uint16_t a, const uint16_t b) {
    return newPerTrack[a].activity() > newPerTrack[b].activity();
  });

  uint32_t seen = 0;
  uint32_t due = 0;
  for (const auto& t : newPerTrack) {
    seen += t.cardsSeen;
    due += t.cardsDue;
  }

  RenderLock lock(*this);
  tracks.swap(newTracks);
  perTrack.swap(newPerTrack);
  order.swap(newOrder);
  totals = tally.totals();
  clockSet = tally.clockSet();
  hasPack = pack && !tracks.empty();
  cardsSeen = seen;
  cardsDue = due;
  LOG_DBG("STUDY", "Stats: %u tracks, %u card reviews, %u questions, %u due, streak %u",
          static_cast<unsigned>(tracks.size()), static_cast<unsigned>(totals.cardReviews),
          static_cast<unsigned>(totals.questions), static_cast<unsigned>(cardsDue),
          static_cast<unsigned>(totals.streakDays));
}

void StudyStatsActivity::loop() {
  // This page opens with a swipe to the left on the Dashboard home, so a swipe back to the right closes it.
  if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Right) {
    finish();
    return;
  }
  StudyScreenActivity::loop();
}

void StudyStatsActivity::openStudyHome() {
  auto next = makeUniqueNoThrow<StudyHomeActivity>(renderer, mappedInput);
  if (!next) {
    LOG_ERR("STUDY", "Cannot allocate the Study activity");
    return;
  }
  startActivityForResult(std::move(next), [this](const ActivityResult&) {
    reload();
    requestUpdate();
  });
}

void StudyStatsActivity::openTrack(const size_t index) {
  if (index >= tracks.size()) return;
  auto next = makeUniqueNoThrow<StudyTrackActivity>(renderer, mappedInput, tracks[index]);
  if (!next) {
    LOG_ERR("STUDY", "Cannot allocate the track menu");
    return;
  }
  startActivityForResult(std::move(next), [this](const ActivityResult&) {
    reload();
    requestUpdate();
  });
}

bool StudyStatsActivity::onTap(const int id) {
  // onTap runs under the render lock; opening a screen only queues the change, the reload happens in its callback.
  if (id == HitOpenStudy) {
    openStudyHome();
  } else if (id >= HitTrack) {
    openTrack(static_cast<size_t>(id - HitTrack));
  }
  return false;
}

void StudyStatsActivity::render(RenderLock&&) {
  clearHits();
  const auto layout = StudyUi::beginScreen(renderer, mappedInput, tr(STR_STUDY));
  const Rect& c = layout.content;
  const int body = StudyUi::bodyFont();
  const int title = StudyUi::titleFont();
  const int bodyLine = renderer.getLineHeight(body);
  const int titleLine = renderer.getLineHeight(title);
  const int gap = StudyUi::kBoxGap;
  const int pad = StudyUi::kBoxPadding;

  if (!hasPack) {
    UITheme::drawCenteredWrappedText(renderer, c, title, c.y + c.height / 4, tr(STR_STUDY_NO_PACK), 2, true,
                                     EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, c, body, c.y + c.height / 4 + titleLine * 3, tr(STR_STUDY_NO_PACK_HINT),
                                     6);
    renderer.displayBuffer();
    return;
  }

  // Headline tiles. A dash stands in where the number needs the clock or has nothing behind it yet.
  char due[16] = "-";
  char streak[16] = "-";
  char accuracy[16] = "-";
  if (clockSet) {
    snprintf(due, sizeof(due), "%u", static_cast<unsigned>(cardsDue));
    snprintf(streak, sizeof(streak), "%u", static_cast<unsigned>(totals.streakDays));
  }
  if (totals.questions > 0) {
    snprintf(accuracy, sizeof(accuracy), "%u%%", study::percentOf(totals.questionsCorrect, totals.questions));
  }
  const char* values[kTileCount] = {due, streak, accuracy};
  const char* labels[kTileCount] = {tr(STR_STUDY_STATS_DUE), tr(STR_STUDY_STATS_STREAK), tr(STR_STUDY_STATS_ACCURACY)};
  const int tileWidth = (c.width - gap * (kTileCount - 1)) / kTileCount;
  const int tileHeight = titleLine + bodyLine + pad * 3;
  for (int i = 0; i < kTileCount; ++i) {
    drawTile(renderer, Rect{c.x + i * (tileWidth + gap), c.y, tileWidth, tileHeight}, values[i], labels[i]);
  }
  int y = c.y + tileHeight + gap * 2;

  // Today and all-time counts.
  char line[96];
  if (totals.cardReviews == 0 && totals.questions == 0 && cardsSeen == 0) {
    y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, tr(STR_STUDY_STATS_NOTHING_YET), 3);
  } else {
    if (clockSet) {
      snprintf(line, sizeof(line), tr(STR_STUDY_STATS_TODAY_FORMAT), static_cast<unsigned>(totals.todayCards),
               static_cast<unsigned>(totals.todayQuestions));
      y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, line, 2);
    }
    snprintf(line, sizeof(line), tr(STR_STUDY_STATS_ALL_TIME_FORMAT), static_cast<unsigned>(totals.cardReviews),
             static_cast<unsigned>(totals.questions));
    y += StudyUi::drawParagraph(renderer, body, c.x, y, c.width, line, 2);
  }
  y += gap * 2;

  // Bottom button.
  const int buttonHeight = bodyLine + pad * 3;
  const Rect button{c.x, c.y + c.height - buttonHeight, c.width, buttonHeight};
  StudyUi::drawBox(renderer, button, body, tr(STR_STUDY_STATS_OPEN), 1, StudyUi::BoxStyle::Selected, true);
  addHit(button, HitOpenStudy);

  // Tracks, most studied first, as many as fit above the button.
  const int listBottom = button.y - gap;
  const int rowHeight = bodyLine * 2 + pad * 2;
  if (y + titleLine + rowHeight <= listBottom) {
    renderer.drawText(title, c.x, y, tr(STR_STUDY_STATS_TRACKS), true, EpdFontFamily::BOLD);
    y += titleLine + pad;
  }
  for (const uint16_t index : order) {
    if (y + rowHeight > listBottom) break;
    const study::TrackInfo& info = tracks[index];
    const study::TrackStudyStats& stats = perTrack[index];
    const Rect row{c.x, y, c.width, rowHeight};

    char percent[16] = "";
    if (stats.questions > 0) {
      snprintf(percent, sizeof(percent), "%u%%", study::percentOf(stats.questionsCorrect, stats.questions));
    }
    const int percentWidth = percent[0] != '\0' ? renderer.getTextWidth(body, percent, EpdFontFamily::BOLD) : 0;
    const int textY = y + pad;
    const auto label =
        renderer.truncatedText(body, info.label.c_str(), c.width - percentWidth - gap, EpdFontFamily::BOLD);
    renderer.drawText(body, c.x, textY, label.c_str(), true, EpdFontFamily::BOLD);
    if (percentWidth > 0) {
      renderer.drawText(body, c.x + c.width - percentWidth, textY, percent, true, EpdFontFamily::BOLD);
    }
    if (clockSet) {
      snprintf(line, sizeof(line), tr(STR_STUDY_STATS_TRACK_FORMAT), static_cast<unsigned>(stats.cardsSeen),
               static_cast<unsigned>(info.cards), static_cast<unsigned>(stats.cardsDue));
    } else {
      snprintf(line, sizeof(line), tr(STR_STUDY_STATS_TRACK_NO_CLOCK_FORMAT), static_cast<unsigned>(stats.cardsSeen),
               static_cast<unsigned>(info.cards));
    }
    const auto detail = renderer.truncatedText(body, line, c.width);
    renderer.drawText(body, c.x, textY + bodyLine, detail.c_str(), true);
    renderer.drawLine(c.x, y + rowHeight - 1, c.x + c.width - 1, y + rowHeight - 1, true);
    addHit(row, HitTrack + index);
    y += rowHeight;
  }

  renderer.displayBuffer();
}
