#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "StudyPageLayout.h"

using namespace study;

namespace {
// Fixed-width font: every UTF-8 character is 10 px wide, bold is 12 px.
int measureFixed(void*, const char* text, const TextStyle style) {
  int chars = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++chars;
  }
  return chars * (style == TextStyle::Bold ? 12 : 10);
}

PageMetrics metrics(const int widthChars, const int heightLines) {
  PageMetrics m;
  m.width = widthChars * 10;
  m.lineHeight = 20;
  m.height = heightLines * 20;
  return m;
}

std::string lineText(const PageLayout& layout, const PageBlock* blocks, const size_t index) {
  const PageLine& line = layout.line(index);
  return std::string(blocks[line.block].text + line.start, line.length);
}

// All the words of every line joined by single spaces, to check that nothing was dropped.
std::string joinedWords(const PageLayout& layout, const PageBlock* blocks) {
  std::string out;
  for (size_t i = 0; i < layout.lineCount(); ++i) {
    const std::string text = lineText(layout, blocks, i);
    if (text.empty()) continue;
    if (!out.empty()) out += ' ';
    out += text;
  }
  return out;
}
}  // namespace

TEST(StudyPageLayout, WrapsAtWordBoundaries) {
  const PageBlock blocks[] = {{"the quick brown fox jumps over the lazy dog"}};
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 1, metrics(15, 10), measureFixed, nullptr));
  ASSERT_EQ(layout.lineCount(), 3u);
  EXPECT_EQ(lineText(layout, blocks, 0), "the quick brown");
  EXPECT_EQ(lineText(layout, blocks, 1), "fox jumps over");
  EXPECT_EQ(lineText(layout, blocks, 2), "the lazy dog");
  EXPECT_EQ(layout.pageCount(), 1u);
}

TEST(StudyPageLayout, LongTextContinuesOnMorePagesWithNothingCutOff) {
  std::string text;
  for (int i = 0; i < 120; ++i) text += "word" + std::to_string(i) + " ";
  const PageBlock blocks[] = {{text.c_str()}};
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 1, metrics(20, 5), measureFixed, nullptr));
  EXPECT_GT(layout.pageCount(), 3u);
  // Every word comes back, in order.
  std::string expected = text;
  expected.pop_back();
  EXPECT_EQ(joinedWords(layout, blocks), expected);
  // Each page holds at most five lines, and lines on a page are stacked from the top.
  for (size_t page = 0; page < layout.pageCount(); ++page) {
    EXPECT_LE(layout.endLine(page) - layout.firstLine(page), 5u);
    EXPECT_EQ(layout.line(layout.firstLine(page)).y, 0);
  }
}

TEST(StudyPageLayout, BreaksAWordWiderThanTheLineInsteadOfTruncating) {
  const PageBlock blocks[] = {{"see https://example.com/a/very/long/path/that/does/not/fit ok"}};
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 1, metrics(12, 10), measureFixed, nullptr));
  std::string all;
  for (size_t i = 0; i < layout.lineCount(); ++i) {
    const std::string text = lineText(layout, blocks, i);
    EXPECT_LE(measureFixed(nullptr, text.c_str(), TextStyle::Regular), 120) << text;
    for (const char c : text)
      if (c != ' ') all += c;
  }
  EXPECT_EQ(all, "seehttps://example.com/a/very/long/path/that/does/not/fitok");
}

TEST(StudyPageLayout, NeverSplitsAUtf8Character) {
  const PageBlock blocks[] = {{"\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"}};  // five e-acute, one word
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 1, metrics(2, 10), measureFixed, nullptr));
  ASSERT_EQ(layout.lineCount(), 3u);
  EXPECT_EQ(layout.line(0).length, 4u);
  EXPECT_EQ(layout.line(1).length, 4u);
  EXPECT_EQ(layout.line(2).length, 2u);
}

TEST(StudyPageLayout, KeepsNewlinesAsParagraphBreaks) {
  const PageBlock blocks[] = {{"first\n\nsecond\r\nthird\n"}};
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 1, metrics(20, 10), measureFixed, nullptr));
  ASSERT_EQ(layout.lineCount(), 4u);
  EXPECT_EQ(lineText(layout, blocks, 0), "first");
  EXPECT_EQ(lineText(layout, blocks, 1), "");
  EXPECT_EQ(lineText(layout, blocks, 2), "second");
  EXPECT_EQ(lineText(layout, blocks, 3), "third");
}

TEST(StudyPageLayout, OptionsUseTheNarrowerWidthAndStayOnOnePage) {
  PageMetrics m = metrics(20, 6);
  m.choiceInset = 10;
  m.optionIndent = 30;  // option text gets 200 - 20 - 30 = 150 px = 15 chars
  const PageBlock blocks[] = {
      {"line one of the question text"},                                  // 2 lines
      {"aaaa bbbb cccc dddd", TextStyle::Regular, BlockKind::Option, 0},  // 2 lines at 15 chars
      {"eeee ffff gggg hhhh", TextStyle::Regular, BlockKind::Option, 1},  // 2 lines: does not fit after the first
  };
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 3, m, measureFixed, nullptr));
  // Page 1: 2 question lines, a 10 px gap, option A (2 lines) = 90 px of 120. Option B (10 + 40) moves on.
  ASSERT_EQ(layout.pageCount(), 2u);
  EXPECT_EQ(lineText(layout, blocks, 2), "aaaa bbbb cccc");
  EXPECT_EQ(layout.line(layout.firstLine(1)).block, 2);
  EXPECT_EQ(layout.line(layout.firstLine(1)).y, 0);
}

TEST(StudyPageLayout, StopsVisitEachChoiceAndEachPlainPage) {
  std::string longText;
  for (int i = 0; i < 30; ++i) longText += "lorem ipsum ";
  const PageBlock blocks[] = {
      {longText.c_str()},
      {"yes", TextStyle::Regular, BlockKind::Option, 7},
      {"no", TextStyle::Regular, BlockKind::Option, 8},
      {"Next", TextStyle::Bold, BlockKind::Action, 100},
  };
  PageMetrics m = metrics(20, 6);
  m.actionPadding = 5;
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 4, m, measureFixed, nullptr));
  ASSERT_GE(layout.pageCount(), 3u);
  // Every page before the last choice page is a plain stop; then one stop per choice.
  const size_t choices = 3;
  ASSERT_GE(layout.stopCount(), choices + 1);
  for (size_t i = 0; i + choices < layout.stopCount(); ++i) {
    EXPECT_EQ(layout.stop(i).block, -1);
    EXPECT_EQ(layout.stop(i).page, i);
  }
  EXPECT_EQ(layout.stop(layout.stopCount() - 3).block, 1);
  EXPECT_EQ(layout.stop(layout.stopCount() - 2).block, 2);
  EXPECT_EQ(layout.stop(layout.stopCount() - 1).block, 3);
  EXPECT_EQ(layout.stopOfBlock(2), layout.stopCount() - 2);
  EXPECT_EQ(layout.stopOfBlock(0), layout.stopCount());
}

TEST(StudyPageLayout, HitTestFindsTheChoiceUnderATap) {
  const PageBlock blocks[] = {
      {"Question?", TextStyle::Bold},
      {"alpha", TextStyle::Regular, BlockKind::Option, 0},
      {"beta", TextStyle::Regular, BlockKind::Option, 1},
      {"Check", TextStyle::Bold, BlockKind::Action, 100},
  };
  PageMetrics m = metrics(20, 20);
  m.actionPadding = 6;
  PageLayout layout;
  ASSERT_TRUE(layout.build(blocks, 4, m, measureFixed, nullptr));
  int top = 0;
  int bottom = 0;
  ASSERT_TRUE(layout.blockSpan(0, 1, top, bottom));
  EXPECT_EQ(top, 30);  // 20 px question line + 10 px gap
  EXPECT_EQ(bottom, 50);
  EXPECT_EQ(layout.blockAt(0, 35), 1);
  EXPECT_EQ(layout.blockAt(0, 65), 2);
  EXPECT_EQ(layout.blockAt(0, 5), -1);  // the question is not a choice
  ASSERT_TRUE(layout.blockSpan(0, 3, top, bottom));
  EXPECT_EQ(bottom - top, 20 + 12);  // the button's padding is part of its frame
  EXPECT_EQ(layout.blockAt(0, top + 2), 3);
}

TEST(StudyPageLayout, UnusableMetricsLeaveOneEmptyPage) {
  const PageBlock blocks[] = {{"text"}};
  PageLayout layout;
  EXPECT_FALSE(layout.build(blocks, 1, PageMetrics{}, measureFixed, nullptr));
  EXPECT_EQ(layout.pageCount(), 1u);
  EXPECT_EQ(layout.lineCount(), 0u);
  ASSERT_EQ(layout.stopCount(), 1u);
  EXPECT_EQ(layout.stop(0).block, -1);
}
