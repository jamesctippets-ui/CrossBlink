#pragma once

#include <StudyPackParse.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "StudyUi.h"
#include "activities/Activity.h"

// Base for the interactive study screens. render() records tappable boxes with addHit(); loop() maps a tap to its id
// and calls onTap() while holding the render lock, so state never changes under a render in progress.
class StudyScreenActivity : public Activity {
 protected:
  StudyScreenActivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput, study::TrackInfo track,
                      int category)
      : Activity(std::move(name), renderer, mappedInput), track(std::move(track)), category(category) {}

  void loop() override;

  // Returns true when the screen should redraw.
  virtual bool onTap(int id) = 0;
  // Called every loop without the lock held; return true to redraw (timers).
  virtual bool onTick() { return false; }
  virtual void onBack() { finish(); }

  void clearHits() { hitCount = 0; }
  void addHit(const Rect& rect, int id);

  // A 4 KB line buffer for reading pack lines, allocated in onEnter and freed in onExit.
  bool allocLineBuffer();
  void freeLineBuffer() { lineBuffer.reset(); }

  const char* categoryName() const;
  bool inCategory(int8_t itemCategory) const { return category < 0 || itemCategory == category; }

  // Fisher-Yates shuffle using the hardware RNG.
  static void shuffle(std::vector<uint16_t>& items);

  study::TrackInfo track;
  int category;
  std::unique_ptr<char[]> lineBuffer;

 private:
  struct Hit {
    Rect rect;
    int id;
  };
  static constexpr int kMaxHits = 16;
  Hit hits[kMaxHits];
  int hitCount = 0;
};
