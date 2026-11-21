#include "StudyBookActivity.h"

#include <CrossPointSettings.h>
#include <Logging.h>

#include <algorithm>

#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"

namespace {
constexpr int kCornerRadius = 6;

EpdFontFamily::Style toFontStyle(const study::TextStyle style) {
  switch (style) {
    case study::TextStyle::Bold:
      return EpdFontFamily::BOLD;
    case study::TextStyle::Italic:
      return EpdFontFamily::ITALIC;
    case study::TextStyle::Regular:
      break;
  }
  return EpdFontFamily::REGULAR;
}

// Letter shown in front of an option: A for id 0, B for id 1, and so on.
void optionMarker(const int id, char (&out)[4]) {
  out[0] = static_cast<char>('A' + (id >= 0 ? id % 26 : 0));
  out[1] = '.';
  out[2] = '\0';
}
}  // namespace

bool StudyBookActivity::focusVisible = false;

int StudyBookActivity::measureText(void* ctx, const char* text, const study::TextStyle style) {
  const auto* self = static_cast<const StudyBookActivity*>(ctx);
  return self->renderer.getTextWidth(self->fontId, text, toFontStyle(style));
}

void StudyBookActivity::onEnter() {
  Activity::onEnter();
  exiting = false;
  stopIndex = 0;
  blockCount = 0;
  layout.clear();

  // Same page geometry as the TXT reader: reader margins inside the bezel-safe area, with room for the reader's top
  // and bottom status bars. The built-in reader font is used even when the reader has an SD card font selected,
  // because SD fonts are only loaded while a book is open.
  fontId = SETTINGS.getBuiltInReaderFontId();
  const int lineHeight =
      std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * SETTINGS.getReaderLineCompression() + 0.5f));
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  left += SETTINGS.screenMarginHorizontal;
  right += SETTINGS.screenMarginHorizontal;
  const int topBar = ReaderUtils::getTopStatusBarReservedHeight(renderer);
  top += topBar > 0 ? std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                               topBar + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING)
                    : SETTINGS.screenMarginVertical;
  bottom += ReaderUtils::getReaderFooterReservedHeight(renderer, false);

  textLeft = left;
  textTop = top;
  metrics.width = renderer.getScreenWidth() - left - right;
  metrics.height = renderer.getScreenHeight() - top - bottom;
  metrics.lineHeight = lineHeight;
  metrics.choiceInset = std::max(4, lineHeight / 4);
  metrics.optionIndent = renderer.getTextWidth(fontId, "M.", EpdFontFamily::BOLD) + lineHeight / 2;
  metrics.actionPadding = std::max(4, lineHeight / 3);
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
}

void StudyBookActivity::beginPage() { blockCount = 0; }

void StudyBookActivity::addBlock(const char* text, const study::TextStyle style, const study::BlockKind kind,
                                 const int id, const bool gapBefore) {
  if (text == nullptr || text[0] == '\0') return;
  if (blockCount >= kMaxBlocks) {
    LOG_ERR("STUDY", "Page block limit reached; dropping a block");
    return;
  }
  study::PageBlock& block = blocks[blockCount++];
  block.text = text;
  block.style = style;
  block.kind = kind;
  block.id = static_cast<int16_t>(id);
  block.gapBefore = gapBefore;
}

void StudyBookActivity::addText(const char* text, const study::TextStyle style, const bool gapBefore) {
  addBlock(text, style, study::BlockKind::Text, -1, gapBefore);
}

void StudyBookActivity::addOption(const char* text, const int id) {
  addBlock(text, study::TextStyle::Regular, study::BlockKind::Option, id, true);
}

void StudyBookActivity::addAction(const char* text, const int id) {
  addBlock(text, study::TextStyle::Bold, study::BlockKind::Action, id, true);
}

void StudyBookActivity::endPage(const bool focusLast) {
  if (!layout.build(blocks, blockCount, metrics, &StudyBookActivity::measureText, this)) {
    LOG_ERR("STUDY", "Page area %dx%d is too small to lay out", metrics.width, metrics.height);
  }
  stopIndex = focusLast && layout.stopCount() > 0 ? layout.stopCount() - 1 : 0;
}

void StudyBookActivity::focusChoice(const int id) {
  for (size_t i = 0; i < blockCount; ++i) {
    if (blocks[i].id != id || blocks[i].kind == study::BlockKind::Text) continue;
    const size_t stop = layout.stopOfBlock(i);
    if (stop < layout.stopCount()) stopIndex = stop;
    return;
  }
}

size_t StudyBookActivity::currentPage() const {
  return stopIndex < layout.stopCount() ? layout.stop(stopIndex).page : 0;
}

bool StudyBookActivity::isFocused(const size_t block) const {
  return focusVisible && stopIndex < layout.stopCount() && layout.stop(stopIndex).block == static_cast<int>(block);
}

bool StudyBookActivity::exitRequested() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return true;
  if (mappedInput.wasBottomEdgeUpSwipe() || mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) return true;
  // The Sticky has no Back key, so holding Up or Down leaves, like holding Back in the reader. The release that
  // follows must not reach the screen underneath as a list move.
  for (const auto button : {MappedInputManager::Button::Up, MappedInputManager::Button::Down}) {
    if (mappedInput.isPressed(button) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      mappedInput.suppressNextSideRelease(button);
      return true;
    }
  }
  return false;
}

void StudyBookActivity::loop() {
  if (exiting) return;
  if (exitRequested()) {
    exiting = true;
    onBack();
    return;
  }

  Input input = Input::None;
  int tapY = 0;
  bool tapNext = false;
  bool tapPrevious = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    input = Input::Confirm;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    input = Input::Next;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    input = Input::Previous;
  } else {
    // Taps and sideways swipes follow the reader's page-turn zones and gesture settings.
    const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
    if (touch.tapped) {
      input = Input::Tap;
      tapY = touch.y;
      tapNext = touch.next;
      tapPrevious = touch.prev;
    } else if (touch.next) {
      input = Input::PageNext;
    } else if (touch.prev) {
      input = Input::PagePrevious;
    }
  }

  bool redraw = false;
  if (input != Input::None) {
    RenderLock lock(*this);
    redraw = handleInput(input, tapY, tapNext, tapPrevious);
  }
  if (onTick()) redraw = true;
  if (redraw) requestUpdate();
}

bool StudyBookActivity::handleInput(const Input input, const int tapY, const bool tapNext, const bool tapPrevious) {
  if (layout.stopCount() == 0) return false;
  const int focusedBlock = layout.stop(stopIndex).block;
  switch (input) {
    case Input::Confirm:
      if (focusedBlock >= 0) {
        // The first press only shows which choice OK would pick.
        if (!focusVisible) {
          focusVisible = true;
          return true;
        }
        return choose(static_cast<size_t>(focusedBlock));
      }
      focusVisible = true;
      return moveFocus(1);
    case Input::Next:
    case Input::Previous:
      if (focusedBlock >= 0 && !focusVisible) {
        focusVisible = true;
        return true;
      }
      focusVisible = true;
      return moveFocus(input == Input::Next ? 1 : -1);
    case Input::PageNext:
    case Input::PagePrevious:
      focusVisible = false;
      return turnPage(input == Input::PageNext ? 1 : -1);
    case Input::Tap: {
      const int block = layout.blockAt(currentPage(), tapY - textTop);
      const bool wasVisible = focusVisible;
      focusVisible = false;
      if (block >= 0) {
        const size_t stop = layout.stopOfBlock(static_cast<size_t>(block));
        if (stop < layout.stopCount()) stopIndex = stop;
        return choose(static_cast<size_t>(block)) || wasVisible;
      }
      if (tapNext) return turnPage(1) || wasVisible;
      if (tapPrevious) return turnPage(-1) || wasVisible;
      return wasVisible;
    }
    case Input::None:
      break;
  }
  return false;
}

bool StudyBookActivity::moveFocus(const int direction) {
  if (direction > 0) {
    if (stopIndex + 1 < layout.stopCount()) {
      ++stopIndex;
      return true;
    }
    return onPastEnd();
  }
  if (stopIndex > 0) {
    --stopIndex;
    return true;
  }
  return onBeforeStart();
}

bool StudyBookActivity::turnPage(const int direction) {
  const size_t page = currentPage();
  if (direction > 0) {
    if (page + 1 < layout.pageCount()) {
      stopIndex = std::min(layout.firstStopOnPage(page + 1), layout.stopCount() - 1);
      return true;
    }
    return onPastEnd();
  }
  if (page > 0) {
    stopIndex = std::min(layout.firstStopOnPage(page - 1), layout.stopCount() - 1);
    return true;
  }
  return onBeforeStart();
}

bool StudyBookActivity::choose(const size_t block) {
  if (block >= blockCount) return false;
  const int id = blocks[block].id;
  if (blocks[block].kind == study::BlockKind::Action && !choiceEnabled(id)) return false;
  return onTap(id);
}

void StudyBookActivity::render(RenderLock&&) {
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  const size_t page = currentPage();
  drawChoiceFrames(page);
  drawLines(page);
  drawStatusBars(page);
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
}

void StudyBookActivity::drawChoiceFrames(const size_t page) const {
  const int lineHeight = metrics.lineHeight;
  int lastBlock = -1;
  for (size_t i = layout.firstLine(page); i < layout.endLine(page); ++i) {
    const size_t b = layout.line(i).block;
    if (static_cast<int>(b) == lastBlock || b >= blockCount) continue;
    lastBlock = static_cast<int>(b);
    const study::PageBlock& block = blocks[b];
    if (block.kind == study::BlockKind::Text) continue;
    int top = 0;
    int bottom = 0;
    if (!layout.blockSpan(page, b, top, bottom)) continue;
    const bool focused = isFocused(b);

    if (block.kind == study::BlockKind::Action) {
      const int x = textLeft + metrics.choiceInset;
      const int width = metrics.width - metrics.choiceInset * 2;
      const int y = textTop + top;
      const int height = bottom - top;
      if (!choiceEnabled(block.id)) {
        renderer.fillRoundedRect(x, y, width, height, kCornerRadius, Color::LightGray);
        renderer.drawRoundedRect(x, y, width, height, 1, kCornerRadius, true);
      } else if (focused) {
        renderer.fillRoundedRect(x, y, width, height, kCornerRadius, Color::Black);
      } else {
        renderer.drawRoundedRect(x, y, width, height, 2, kCornerRadius, true);
      }
      continue;
    }

    // Option: a focus frame around the whole option, and its letter in the hanging indent (inverted when picked).
    const int pad = std::max(2, lineHeight / 6);
    if (focused) {
      renderer.drawRoundedRect(textLeft, textTop + top - pad, metrics.width, bottom - top + pad * 2, 2, kCornerRadius,
                               true);
    }
    const bool startsHere = i == 0 || layout.line(i - 1).block != b;
    if (!startsHere) continue;
    char marker[4];
    optionMarker(block.id, marker);
    const int markerX = textLeft + metrics.choiceInset;
    const int markerY = textTop + layout.line(i).y;
    const bool picked = choicePicked(block.id);
    if (picked) {
      renderer.fillRoundedRect(markerX - 3, markerY, metrics.optionIndent - lineHeight / 4, lineHeight, kCornerRadius,
                               Color::Black);
    }
    renderer.drawText(fontId, markerX, markerY, marker, !picked, EpdFontFamily::BOLD);
  }
}

void StudyBookActivity::drawLines(const size_t page) {
  for (size_t i = layout.firstLine(page); i < layout.endLine(page); ++i) {
    const study::PageLine& line = layout.line(i);
    if (line.length == 0 || line.block >= blockCount) continue;
    const study::PageBlock& block = blocks[line.block];
    lineScratch.assign(block.text + line.start, line.length);
    const auto style = toFontStyle(block.style);
    int x = textLeft;
    bool black = true;
    switch (block.kind) {
      case study::BlockKind::Text:
        break;
      case study::BlockKind::Option:
        x += metrics.choiceInset + metrics.optionIndent;
        break;
      case study::BlockKind::Action: {
        const int width = renderer.getTextWidth(fontId, lineScratch.c_str(), style);
        x += (metrics.width - width) / 2;
        black = !(isFocused(line.block) && choiceEnabled(block.id));
        break;
      }
    }
    renderer.drawText(fontId, x, textTop + line.y, lineScratch.c_str(), black, style);
  }
}

void StudyBookActivity::drawStatusBars(const size_t page) const {
  char title[96];
  title[0] = '\0';
  statusTitle(title, sizeof(title));
  ReaderStatusBarContent content;
  content.bookProgress = statusProgress();
  content.chapterPage = static_cast<int>(page) + 1;
  content.chapterPageCount = static_cast<int>(layout.pageCount());
  content.bookTitle = track.label.c_str();
  content.chapterTitle = title;
  content.darkMode = ReaderUtils::readerDarkModeEnabled();
  GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content);
  GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Bottom, content);
}
