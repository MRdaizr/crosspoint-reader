#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"

class ParagraphSoftFlushTest : public testing::TestWithParam<bool> {};

TEST_P(ParagraphSoftFlushTest, IndentsOnlyTheFirstEmittedLineAcrossFlushes) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  style.textIndent = 12;
  ParsedText text(false, GetParam(), false, style);
  std::vector<std::unique_ptr<TextBlock>> lines;
  std::vector<uint32_t> offsets;
  const auto collect = [&](std::unique_ptr<TextBlock> line, uint32_t offset) {
    offsets.push_back(offset);
    lines.push_back(std::move(line));
  };

  for (uint32_t i = 0; i < 20; ++i) {
    text.addWord("word", EpdFontFamily::REGULAR, false, false, i * 5);
  }
  text.layoutAndExtractLines(renderer, 0, 100, collect, false);
  ASSERT_FALSE(lines.empty());
  ASSERT_GT(text.size(), 0u);
  EXPECT_EQ(lines.front()->wordXpos(0), 12);
  const size_t firstFlushLines = lines.size();
  for (uint32_t i = 20; i < 40; ++i) {
    text.addWord("word", EpdFontFamily::REGULAR, false, false, i * 5);
  }
  text.layoutAndExtractLines(renderer, 0, 100, collect, false);
  ASSERT_GT(lines.size(), firstFlushLines);
  EXPECT_EQ(lines[firstFlushLines]->wordXpos(0), 0);
  text.layoutAndExtractLines(renderer, 0, 100, collect, true);
  EXPECT_TRUE(text.isEmpty());

  size_t wordCount = 0;
  uint32_t expectedOffset = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    ASSERT_TRUE(lines[i]->valid());
    EXPECT_EQ(offsets[i], expectedOffset);
    EXPECT_EQ(lines[i]->wordXpos(0), i == 0 ? 12 : 0);
    wordCount += lines[i]->wordCount();
    expectedOffset += static_cast<uint32_t>(lines[i]->wordCount() * 5);
  }
  EXPECT_EQ(wordCount, 40u);
}

TEST_P(ParagraphSoftFlushTest, EmptySoftFlushDoesNotConsumeFirstLineIndent) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  style.textIndent = 12;
  ParsedText text(false, GetParam(), false, style);
  std::vector<std::unique_ptr<TextBlock>> lines;
  const auto collect = [&](std::unique_ptr<TextBlock> line, uint32_t) { lines.push_back(std::move(line)); };
  text.addWord("word", EpdFontFamily::REGULAR);
  text.layoutAndExtractLines(renderer, 0, 100, collect, false);
  EXPECT_TRUE(lines.empty());
  EXPECT_EQ(text.size(), 1u);
  text.layoutAndExtractLines(renderer, 0, 100, collect, true);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines.front()->wordXpos(0), 12);
}

INSTANTIATE_TEST_SUITE_P(LayoutModes, ParagraphSoftFlushTest, testing::Bool());
