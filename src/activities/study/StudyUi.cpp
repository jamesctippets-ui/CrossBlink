#include "StudyUi.h"

#include <string>
#include <vector>

#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"

namespace StudyUi {

namespace {
constexpr int kCornerRadius = 8;
}

int bodyFont() { return uiScaleSpec().bodyFontId; }
int titleFont() { return uiScaleSpec().titleFontId; }

Layout beginScreen(GfxRenderer& renderer, const MappedInputManager& input, const char* title, const char* subtitle) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  Layout layout;
  layout.header = TouchHeaderBackButton::headerRect(renderer, input);
  if (input.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, layout.header, title, false, 0, subtitle);
  } else {
    GUI.drawHeader(renderer, layout.header, title, subtitle);
  }
  const int top = layout.header.y + layout.header.height + metrics.verticalSpacing;
  const int side = metrics.contentSidePadding;
  layout.content = Rect{side, top, renderer.getScreenWidth() - side * 2, renderer.getScreenHeight() - top - side};
  return layout;
}

bool backRequested(const MappedInputManager& input, const GfxRenderer& renderer) {
  return TouchHeaderBackButton::wasTapped(input, renderer) || input.wasReleased(MappedInputManager::Button::Back);
}

int drawParagraph(const GfxRenderer& renderer, const int fontId, const int x, int y, const int width, const char* text,
                  const int maxLines, const EpdFontFamily::Style style, const bool black) {
  if (text == nullptr || text[0] == '\0' || maxLines <= 0) return 0;
  const auto lines = renderer.wrappedText(fontId, text, width, maxLines, style);
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    renderer.drawText(fontId, x, y, line.c_str(), black, style);
    y += lineHeight;
  }
  return lineHeight * static_cast<int>(lines.size());
}

int boxHeight(const GfxRenderer& renderer, const int fontId, const int width, const char* text, const int maxLines) {
  const auto lines = renderer.wrappedText(fontId, text != nullptr ? text : "", width - kBoxPadding * 2, maxLines);
  const int count = lines.empty() ? 1 : static_cast<int>(lines.size());
  return count * renderer.getLineHeight(fontId) + kBoxPadding * 2;
}

void drawBox(const GfxRenderer& renderer, const Rect& rect, const int fontId, const char* text, const int maxLines,
             const BoxStyle style, const bool center) {
  bool black = true;
  switch (style) {
    case BoxStyle::Normal:
      renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, kCornerRadius, true);
      break;
    case BoxStyle::Selected:
      renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, kCornerRadius, Color::Black);
      black = false;
      break;
    case BoxStyle::Correct:
      renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, kCornerRadius, Color::Black);
      renderer.drawRoundedRect(rect.x + 3, rect.y + 3, rect.width - 6, rect.height - 6, 2, kCornerRadius, false);
      black = false;
      break;
    case BoxStyle::Dimmed:
      renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, kCornerRadius, Color::LightGray);
      renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, kCornerRadius, true);
      break;
  }
  if (text == nullptr || text[0] == '\0') return;
  const int innerWidth = rect.width - kBoxPadding * 2;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int fitLines = (rect.height - kBoxPadding * 2) / (lineHeight > 0 ? lineHeight : 1);
  const int lines = fitLines < maxLines ? fitLines : maxLines;
  const auto wrapped = renderer.wrappedText(fontId, text, innerWidth, lines > 0 ? lines : 1);
  int y = rect.y + (rect.height - lineHeight * static_cast<int>(wrapped.size())) / 2;
  for (const auto& line : wrapped) {
    int x = rect.x + kBoxPadding;
    if (center) x = rect.x + (rect.width - renderer.getTextWidth(fontId, line.c_str())) / 2;
    renderer.drawText(fontId, x, y, line.c_str(), black);
    y += lineHeight;
  }
}

}  // namespace StudyUi
