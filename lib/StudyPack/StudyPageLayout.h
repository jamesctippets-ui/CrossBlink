#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Book-style page layout for the study screens: word-wraps a short list of blocks (question text, options, buttons)
// into lines and splits them into pages, so long questions and answers continue on the next page instead of being
// cut off. Pure logic: text width comes from a callback, so host tests can use a fixed-width font.
namespace study {

enum class TextStyle : uint8_t { Regular, Bold, Italic };

enum class BlockKind : uint8_t {
  Text,    // a paragraph; may continue on the next page
  Option,  // an answer option with a letter in a hanging indent; kept on one page when it fits
  Action,  // a button line such as "Next"; kept on one page when it fits
};

struct PageBlock {
  const char* text = nullptr;  // borrowed, NUL-terminated; must outlive the layout
  TextStyle style = TextStyle::Regular;
  BlockKind kind = BlockKind::Text;
  int16_t id = -1;        // reported when an Option or Action is chosen
  bool gapBefore = true;  // half a line of space above, except at the top of a page
};

struct PageLine {
  uint16_t block = 0;
  uint16_t start = 0;   // byte offset into the block's text
  uint16_t length = 0;  // bytes
  int16_t y = 0;        // top of the line, relative to the top of the text area
};

struct PageMetrics {
  int width = 0;   // text area
  int height = 0;  // text area
  int lineHeight = 1;
  int choiceInset = 0;    // left and right inset of Option and Action blocks (room for the focus frame)
  int optionIndent = 0;   // width of the option letter column
  int actionPadding = 0;  // extra space above and below an Action's text (its frame)
};

// Returns the width in pixels of a NUL-terminated string.
using MeasureTextFn = int (*)(void* ctx, const char* text, TextStyle style);

class PageLayout {
 public:
  // A focus stop for button navigation: each choice is a stop on the page where it starts, and a page without
  // choices is one stop of its own.
  struct Stop {
    uint16_t page = 0;
    int16_t block = -1;  // block index, or -1 for a plain page
  };

  // Lays out the blocks. Returns false (and leaves one empty page) when the metrics are unusable.
  bool build(const PageBlock* blocks, size_t count, const PageMetrics& metrics, MeasureTextFn measure, void* ctx);
  void clear();

  size_t pageCount() const { return pageStarts.empty() ? 1 : pageStarts.size(); }
  size_t lineCount() const { return lines.size(); }
  const PageLine& line(size_t index) const { return lines[index]; }
  // Lines of a page are [firstLine(page), endLine(page)).
  size_t firstLine(size_t page) const;
  size_t endLine(size_t page) const;

  // Vertical extent of a block on a page, including an Action's padding. False when the block is not on the page.
  bool blockSpan(size_t page, size_t block, int& top, int& bottom) const;
  // Index of the Option or Action block at y on the page, or -1.
  int blockAt(size_t page, int y) const;

  size_t stopCount() const { return stops.size(); }
  const Stop& stop(size_t index) const { return stops[index]; }
  // First stop on the page, or stopCount() when the page has none.
  size_t firstStopOnPage(size_t page) const;
  // Stop of the given block, or stopCount() when it has none.
  size_t stopOfBlock(size_t block) const;

  const PageMetrics& metrics() const { return metrics_; }

 private:
  struct Segment {
    uint16_t start;
    uint16_t length;
  };

  std::vector<PageLine> lines;
  std::vector<uint16_t> pageStarts;  // first line of each page
  std::vector<Stop> stops;
  std::vector<BlockKind> kinds;   // per block, for hit tests and stops
  std::vector<Segment> segments;  // scratch for one block
  std::string scratch;            // scratch for measuring a substring
  PageMetrics metrics_;

  void wrapBlock(const char* text, int width, TextStyle style, MeasureTextFn measure, void* ctx);
  int measure(const char* text, size_t start, size_t length, TextStyle style, MeasureTextFn fn, void* ctx);
  bool isChoice(size_t block) const;
  void buildStops();
};

// Byte length of the UTF-8 sequence that starts with this byte (1 for stray continuation bytes).
size_t utf8SequenceLength(unsigned char lead);

}  // namespace study
