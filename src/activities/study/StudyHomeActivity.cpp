#include "StudyHomeActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <StudyPackStore.h>

#include <cstdio>

#include "StudyTrackActivity.h"

void StudyHomeActivity::onEnter() {
  study::loadManifest(tracks);
  counts.clear();
  counts.reserve(tracks.size());
  rows.assign(tracks.size(), freeink::ui::ListItem{});
  for (size_t i = 0; i < tracks.size(); ++i) {
    char buf[64];
    snprintf(buf, sizeof(buf), tr(STR_STUDY_TRACK_COUNTS_FORMAT), tracks[i].cards, tracks[i].questions);
    counts.emplace_back(buf);
  }
  for (size_t i = 0; i < tracks.size(); ++i) {
    rows[i].label = tracks[i].label.c_str();
    rows[i].subtitle = tracks[i].subtitle.empty() ? nullptr : tracks[i].subtitle.c_str();
    rows[i].value = counts[i].c_str();
    rows[i].actionValue = static_cast<int16_t>(i);
  }
  StudyListActivity::onEnter();
}

void StudyHomeActivity::onExit() {
  rows.clear();
  counts.clear();
  tracks.clear();
  tracks.shrink_to_fit();
  StudyListActivity::onExit();
}

const char* StudyHomeActivity::title() const { return tr(STR_STUDY); }
const char* StudyHomeActivity::emptyMessage() const { return tr(STR_STUDY_NO_PACK); }
const char* StudyHomeActivity::emptyHint() const { return tr(STR_STUDY_NO_PACK_HINT); }

void StudyHomeActivity::onRowChosen(const int index) {
  if (index < 0 || index >= static_cast<int>(tracks.size())) return;
  auto next = makeUniqueNoThrow<StudyTrackActivity>(renderer, mappedInput, tracks[index]);
  if (!next) {
    LOG_ERR("STUDY", "Cannot allocate the track menu");
    return;
  }
  startActivityForResult(std::move(next), [this](const ActivityResult&) { requestUpdate(); });
}
