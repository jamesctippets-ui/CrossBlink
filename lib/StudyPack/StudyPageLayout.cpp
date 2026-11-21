#include "StudyPageLayout.h"

#include <cstring>

namespace study {

size_t utf8SequenceLength(const unsigned char lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

void PageLayout::clear() {
  lines.clear();
  pageStarts.clear();
  stops.clear();
  kinds.clear();
}

int PageLayout::measure(const char* text, const size_t start, const size_t length, const TextStyle style,
                        const MeasureTextFn fn, void* ctx) {
  scratch.assign(text + start, length);
  return fn(ctx, scratch.c_str(), style);
}

void PageLayout::wrapBlock(const char* text, const int width, const TextStyle style, const MeasureTextFn fn,
                           void* ctx) {
  segments.clear();
  size_t n = strlen(text);
  if (n > 0xFFFF) n = 0xFFFF;  // offsets are 16-bit; pack lines are a few KB at most
  const auto emit = [this](const size_t start, const size_t end) {
    segments.push_back(Segment{static_cast<uint16_t>(start), static_cast<uint16_t>(end - start)});
  };

  size_t pos = 0;
  while (pos < n) {
    size_t paraEnd = pos;
    while (paraEnd < n && text[paraEnd] != '\n') ++paraEnd;
    const size_t next = paraEnd + 1;
    if (paraEnd > pos && text[paraEnd - 1] == '\r') --paraEnd;

    size_t lineStart = pos;
    while (lineStart < paraEnd && text[lineStart] == ' ') ++lineStart;
    if (lineStart >= paraEnd) {
      // A blank line between paragraphs keeps its space; a trailing one is dropped.
      if (next < n) emit(lineStart, lineStart);
      pos = next;
      continue;
    }

    size_t lineEnd = lineStart;  // end of the words that fit so far
    size_t cursor = lineStart;
    while (cursor < paraEnd) {
      size_t wordStart = cursor;
      while (wordStart < paraEnd && text[wordStart] == ' ') ++wordStart;
      if (wordStart >= paraEnd) break;
      size_t wordEnd = wordStart;
      while (wordEnd < paraEnd && text[wordEnd] != ' ') ++wordEnd;

      if (measure(text, lineStart, wordEnd - lineStart, style, fn, ctx) <= width) {
        lineEnd = wordEnd;
        cursor = wordEnd;
        continue;
      }
      if (lineEnd > lineStart) {
        // The word moves to the next line.
        emit(lineStart, lineEnd);
        lineStart = wordStart;
        lineEnd = wordStart;
        cursor = wordStart;
        continue;
      }
      // One word wider than the line (a long URL or command): break it between characters instead of cutting it.
      size_t cut = lineStart + utf8SequenceLength(static_cast<unsigned char>(text[lineStart]));
      if (cut > wordEnd) cut = wordEnd;
      while (cut < wordEnd) {
        size_t step = cut + utf8SequenceLength(static_cast<unsigned char>(text[cut]));
        if (step > wordEnd) step = wordEnd;
        if (measure(text, lineStart, step - lineStart, style, fn, ctx) > width) break;
        cut = step;
      }
      emit(lineStart, cut);
      lineStart = cut;
      lineEnd = cut;
      cursor = cut;
    }
    if (lineEnd > lineStart) emit(lineStart, lineEnd);
    pos = next;
  }
}

bool PageLayout::build(const PageBlock* blocks, const size_t count, const PageMetrics& metrics,
                       const MeasureTextFn measureFn, void* ctx) {
  clear();
  metrics_ = metrics;
  pageStarts.push_back(0);
  kinds.reserve(count);
  for (size_t i = 0; i < count; ++i) kinds.push_back(blocks[i].kind);
  if (metrics.width <= 0 || metrics.height <= 0 || metrics.lineHeight <= 0 || measureFn == nullptr) {
    stops.push_back(Stop{0, -1});
    return false;
  }

  const int lineHeight = metrics.lineHeight;
  int y = 0;
  const auto newPage = [this, &y] {
    pageStarts.push_back(static_cast<uint16_t>(lines.size()));
    y = 0;
  };
  const auto placeLine = [this, &y, &newPage, lineHeight, &metrics](const size_t block, const Segment& seg) {
    if (y > 0 && y + lineHeight > metrics.height) newPage();
    lines.push_back(PageLine{static_cast<uint16_t>(block), seg.start, seg.length, static_cast<int16_t>(y)});
    y += lineHeight;
  };

  lines.reserve(64);
  for (size_t i = 0; i < count && i < 0xFFFF; ++i) {
    const PageBlock& block = blocks[i];
    if (block.text == nullptr || block.text[0] == '\0') continue;
    int wrapWidth = metrics.width;
    if (block.kind == BlockKind::Option) wrapWidth -= metrics.choiceInset * 2 + metrics.optionIndent;
    if (block.kind == BlockKind::Action) wrapWidth -= metrics.choiceInset * 2;
    if (wrapWidth < 1) wrapWidth = 1;
    wrapBlock(block.text, wrapWidth, block.style, measureFn, ctx);
    if (segments.empty()) continue;

    int gap = block.gapBefore && y > 0 ? lineHeight / 2 : 0;
    if (block.kind == BlockKind::Text) {
      y += gap;
      for (const auto& seg : segments) placeLine(i, seg);
      continue;
    }

    // Options and buttons stay on one page when they fit on one.
    const int padding = block.kind == BlockKind::Action ? metrics.actionPadding : 0;
    const int height = static_cast<int>(segments.size()) * lineHeight + padding * 2;
    if (y > 0 && y + gap + height > metrics.height && height <= metrics.height) {
      newPage();
      gap = 0;
    }
    y += gap + padding;
    for (const auto& seg : segments) placeLine(i, seg);
    y += padding;
  }
  buildStops();
  return true;
}

size_t PageLayout::firstLine(const size_t page) const {
  if (page >= pageStarts.size()) return lines.size();
  return pageStarts[page];
}

size_t PageLayout::endLine(const size_t page) const {
  if (page + 1 >= pageStarts.size()) return lines.size();
  return pageStarts[page + 1];
}

bool PageLayout::isChoice(const size_t block) const { return block < kinds.size() && kinds[block] != BlockKind::Text; }

bool PageLayout::blockSpan(const size_t page, const size_t block, int& top, int& bottom) const {
  bool found = false;
  for (size_t i = firstLine(page); i < endLine(page); ++i) {
    if (lines[i].block != block) continue;
    const int lineTop = lines[i].y;
    if (!found) top = lineTop;
    bottom = lineTop + metrics_.lineHeight;
    found = true;
  }
  if (found && block < kinds.size() && kinds[block] == BlockKind::Action) {
    top -= metrics_.actionPadding;
    bottom += metrics_.actionPadding;
  }
  return found;
}

int PageLayout::blockAt(const size_t page, const int y) const {
  // Half the paragraph gap on each side counts too, so taps between two options still land on one.
  const int slack = metrics_.lineHeight / 4;
  int lastBlock = -1;
  for (size_t i = firstLine(page); i < endLine(page); ++i) {
    const int block = lines[i].block;
    if (block == lastBlock || !isChoice(block)) continue;
    lastBlock = block;
    int top = 0;
    int bottom = 0;
    if (blockSpan(page, block, top, bottom) && y >= top - slack && y < bottom + slack) return block;
  }
  return -1;
}

void PageLayout::buildStops() {
  stops.clear();
  int lastBlock = -1;
  for (size_t page = 0; page < pageCount(); ++page) {
    const size_t before = stops.size();
    for (size_t i = firstLine(page); i < endLine(page); ++i) {
      const int block = lines[i].block;
      if (block == lastBlock || !isChoice(block)) continue;
      lastBlock = block;
      stops.push_back(Stop{static_cast<uint16_t>(page), static_cast<int16_t>(block)});
    }
    if (stops.size() == before) stops.push_back(Stop{static_cast<uint16_t>(page), -1});
  }
}

size_t PageLayout::firstStopOnPage(const size_t page) const {
  for (size_t i = 0; i < stops.size(); ++i) {
    if (stops[i].page == page) return i;
  }
  return stops.size();
}

size_t PageLayout::stopOfBlock(const size_t block) const {
  for (size_t i = 0; i < stops.size(); ++i) {
    if (stops[i].block == static_cast<int>(block)) return i;
  }
  return stops.size();
}

}  // namespace study
