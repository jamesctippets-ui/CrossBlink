#pragma once

#include <StudyPageLayout.h>

#include <cstddef>
#include <string>

#include "StudyScreenActivity.h"

// Base for the study screens that read like a book: the reader's font, margins, line spacing and status bars, with
// long questions and answers continuing on the next page instead of being cut off.
//
// Buttons (the Sticky has Up, Down and OK): Down/Right moves to the next page or the next choice on the page, Up/Left
// goes back, OK picks the highlighted choice (or turns the page when there is none), and holding Up or Down leaves.
// Back leaves on devices that have it. Touch: tap a choice, tap or swipe sideways to turn pages like the reader, swipe
// up to leave.
class StudyBookActivity : public StudyScreenActivity {
 protected:
  using StudyScreenActivity::StudyScreenActivity;

  // Measures the page area from the reader settings. Subclasses call it before building their first page.
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Page content. Text is borrowed until the next beginPage(), so it must live in members, not temporaries.
  void beginPage();
  void addText(const char* text, study::TextStyle style = study::TextStyle::Regular, bool gapBefore = true);
  void addOption(const char* text, int id);  // drawn with letter A, B, C... from id 0, 1, 2...
  void addAction(const char* text, int id);
  // Lays the page out and puts the focus on its first stop, or its last one when focusLast is true.
  void endPage(bool focusLast = false);
  // Moves the focus to a choice on the current layout.
  void focusChoice(int id);

  // Hooks, called with the render lock held. A choice tapped or confirmed with OK goes to onTap(id).
  virtual bool onPastEnd() { return false; }      // Next pressed on the last page or choice
  virtual bool onBeforeStart() { return false; }  // Previous pressed on the first page or choice
  virtual bool choicePicked(int id) const {
    (void)id;
    return false;
  }
  virtual bool choiceEnabled(int id) const {
    (void)id;
    return true;
  }
  // Status bar chapter title (e.g. "Quiz 3 / 10") and progress through the session, 0-100.
  virtual void statusTitle(char* out, size_t size) const = 0;
  virtual float statusProgress() const { return 0; }

  // StudyScreenActivity's tap routing is replaced by loop() here.
  bool onTap(int id) override = 0;

 private:
  enum class Input : uint8_t { None, Confirm, Next, Previous, PageNext, PagePrevious, Tap };
  static constexpr size_t kMaxBlocks = 16;

  study::PageBlock blocks[kMaxBlocks];
  size_t blockCount = 0;
  study::PageLayout layout;
  study::PageMetrics metrics;
  std::string lineScratch;  // one line copied out for drawText, which needs a NUL terminator
  size_t stopIndex = 0;
  // The focus frame shows once a button is used and hides again on a tap. Shared by all study pages, so a button
  // reader keeps seeing it from one screen to the next.
  static bool focusVisible;
  bool exiting = false;
  int fontId = 0;
  int textLeft = 0;
  int textTop = 0;
  int pagesUntilFullRefresh = 0;

  void addBlock(const char* text, study::TextStyle style, study::BlockKind kind, int id, bool gapBefore);
  size_t currentPage() const;
  bool exitRequested();
  bool handleInput(Input input, int tapY, bool tapNext, bool tapPrevious);
  bool moveFocus(int direction);
  bool turnPage(int direction);
  bool choose(size_t block);
  bool isFocused(size_t block) const;
  void drawChoiceFrames(size_t page) const;
  void drawLines(size_t page);
  void drawStatusBars(size_t page) const;

  static int measureText(void* ctx, const char* text, study::TextStyle style);
};
