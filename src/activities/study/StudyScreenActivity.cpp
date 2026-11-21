#include "StudyScreenActivity.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <StudyPackStore.h>

void StudyScreenActivity::addHit(const Rect& rect, const int id) {
  if (hitCount >= kMaxHits) return;
  hits[hitCount++] = Hit{rect, id};
}

bool StudyScreenActivity::allocLineBuffer() {
  // Heap, not stack: pack lines run to ~1.6 KB and the loop task stack is small. Allocated once per screen visit.
  lineBuffer = makeUniqueNoThrow<char[]>(study::kLineBufferSize);
  if (!lineBuffer)
    LOG_ERR("STUDY", "Cannot allocate the %u byte line buffer", static_cast<unsigned>(study::kLineBufferSize));
  return lineBuffer != nullptr;
}

const char* StudyScreenActivity::categoryName() const {
  if (category >= 0 && category < static_cast<int>(track.categories.size())) return track.categories[category].c_str();
  return nullptr;
}

void StudyScreenActivity::shuffle(std::vector<uint16_t>& items) {
  for (size_t i = items.size(); i > 1; --i) {
    const size_t j = static_cast<size_t>(random(static_cast<long>(i)));
    const uint16_t tmp = items[i - 1];
    items[i - 1] = items[j];
    items[j] = tmp;
  }
}

void StudyScreenActivity::loop() {
  if (StudyUi::backRequested(mappedInput, renderer)) {
    onBack();
    return;
  }
  bool redraw = false;
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    RenderLock lock(*this);
    for (int i = 0; i < hitCount; ++i) {
      if (StudyUi::contains(hits[i].rect, x, y)) {
        redraw = onTap(hits[i].id);
        break;
      }
    }
  }
  if (onTick()) redraw = true;
  if (redraw) requestUpdate();
}
