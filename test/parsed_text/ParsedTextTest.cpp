#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/hyphenation/Hyphenator.h"

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

TEST(WordStoreLayoutTest, LongCjkParagraphSurvivesSeveralChunkRetirements) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(true, false, false, style);
  std::string source;
  for (int i = 0; i < 600; ++i) source += "中文日本語";
  text.addWord(source, EpdFontFamily::REGULAR);
  std::string rendered;
  size_t lines = 0;
  uint32_t expectedOffset = 0;
  text.layoutAndExtractLines(renderer, 0, 48, [&](std::unique_ptr<TextBlock> line, uint32_t offset) {
    ASSERT_TRUE(line->valid());
    EXPECT_EQ(expectedOffset, offset);
    for (size_t i = 0; i < line->wordCount(); ++i) {
      rendered += line->wordText(i);
      ++expectedOffset;
    }
    ++lines;
  });
  EXPECT_EQ(source, rendered);
  EXPECT_GT(lines, 100u);
  EXPECT_TRUE(text.isEmpty());
}

TEST(WordStoreLayoutTest, RubyGroupsAndFocusMetadataSurviveSoftFlush) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(true, false, true, style);
  for (unsigned i = 0; i < 500; ++i) text.addWord("reading", EpdFontFamily::REGULAR);
  const size_t rubyStart = text.size();
  text.addWord("日本語", EpdFontFamily::REGULAR, true);
  text.setRubyGroupAt(rubyStart, text.size() - rubyStart, "にほんご");
  std::vector<std::unique_ptr<TextBlock>> lines;
  const auto collect = [&](std::unique_ptr<TextBlock> line, uint32_t) { lines.push_back(std::move(line)); };
  text.layoutAndExtractLines(renderer, 0, 128, collect, false);
  text.addWord("ending", EpdFontFamily::REGULAR);
  text.layoutAndExtractLines(renderer, 0, 128, collect);
  size_t english = 0, rubyLeaders = 0, rubyFollowers = 0;
  std::string japanese;
  for (const auto& line : lines) {
    ASSERT_TRUE(line->valid());
    for (size_t i = 0; i < line->wordCount(); ++i) {
      const std::string word = line->wordText(i);
      if (word == "reading") {
        ++english;
        EXPECT_EQ(3u, line->focusBoundary(i));
      } else if (word != "ending") {
        japanese += word;
        EXPECT_NE(0, line->wordStyle(i) & EpdFontFamily::UNDERLINE);
        if ((line->wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) != 0)
          ++rubyFollowers;
        else {
          ASSERT_LT(i, line->getRubyTexts().size());
          EXPECT_EQ("にほんご", line->getRubyTexts()[i]);
          ++rubyLeaders;
        }
      }
    }
  }
  EXPECT_EQ(500u, english);
  EXPECT_EQ("日本語", japanese);
  EXPECT_EQ(1u, rubyLeaders);
  EXPECT_EQ(2u, rubyFollowers);
  EXPECT_TRUE(text.isEmpty());
}

TEST(WordStoreLayoutTest, HyphenatedFocusWordRetainsCompleteTextAndOffsets) {
  Hyphenator::setPreferredLanguage("en");
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(true, true, true, style);
  const std::string source = "internationalization";
  text.addWord(source, EpdFontFamily::REGULAR, false, false, 100);
  std::string rendered;
  uint32_t consumed = 0;
  size_t lines = 0;
  text.layoutAndExtractLines(renderer, 0, 64, [&](std::unique_ptr<TextBlock> line, uint32_t offset) {
    ASSERT_TRUE(line->valid());
    EXPECT_EQ(100 + consumed, offset);
    for (size_t i = 0; i < line->wordCount(); ++i) {
      std::string token = line->wordText(i);
      if (!token.empty() && token.back() == '-') token.pop_back();
      consumed += static_cast<uint32_t>(token.size());
      rendered += token;
    }
    ++lines;
  });
  EXPECT_EQ(source, rendered);
  EXPECT_GT(lines, 1u);
  EXPECT_TRUE(text.isEmpty());
}

TEST(WordStoreLayoutTest, SoftHyphensAndPunctuationStayUtf8Safe) {
  Hyphenator::setPreferredLanguage("en");
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(true, true, false, style);
  text.addWord(
      "abcd\xC2\xAD"
      "efgh",
      EpdFontFamily::REGULAR);
  std::vector<std::string> words;
  text.layoutAndExtractLines(renderer, 0, 40, [&](std::unique_ptr<TextBlock> line, uint32_t) {
    for (size_t i = 0; i < line->wordCount(); ++i) words.emplace_back(line->wordText(i));
  });
  ASSERT_EQ(2u, words.size());
  EXPECT_EQ("abcd-", words[0]);
  EXPECT_EQ("efgh", words[1]);
}
