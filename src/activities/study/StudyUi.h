#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"

// Drawing and hit-testing shared by the study screens (flashcards, quiz, match). These screens are mostly large
// tappable boxes holding wrapped text, which the FreeInkUI list component does not cover.
namespace StudyUi {

enum class BoxStyle : uint8_t {
  Normal,    // outlined
  Selected,  // filled black, white text
  Correct,   // filled black, white text, thicker frame (same look as Selected on 1-bit panels)
  Dimmed,    // light grey fill: a matched pair or a wrong pick
};

struct Layout {
  Rect header;   // title row with the touch Back arrow
  Rect content;  // area below the header, inside the side padding
};

int bodyFont();
int titleFont();

// Clears the screen and draws the header. Returns where content goes.
Layout beginScreen(GfxRenderer& renderer, const MappedInputManager& input, const char* title,
                   const char* subtitle = nullptr);

// True when the header's Back arrow was tapped or the Back button released.
bool backRequested(const MappedInputManager& input, const GfxRenderer& renderer);

// Word-wraps text into the given width starting at y (top of the first line). Returns the height used.
int drawParagraph(const GfxRenderer& renderer, int fontId, int x, int y, int width, const char* text, int maxLines,
                  EpdFontFamily::Style style = EpdFontFamily::REGULAR, bool black = true);

// Height a box needs for text wrapped to at most maxLines.
int boxHeight(const GfxRenderer& renderer, int fontId, int width, const char* text, int maxLines);

// Draws a rounded box with the text wrapped and left-aligned (centered when center is true) inside.
void drawBox(const GfxRenderer& renderer, const Rect& rect, int fontId, const char* text, int maxLines, BoxStyle style,
             bool center = false);

inline bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

constexpr int kBoxPadding = 10;
constexpr int kBoxGap = 10;

}  // namespace StudyUi
