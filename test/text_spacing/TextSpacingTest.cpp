#include <CrossPointSettings.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/hyphenation/Hyphenator.h"

namespace {
size_t checks = 0;
void check(bool condition, const char* expression, int line) {
  ++checks;
  if (!condition) {
    std::cerr << "line " << line << ": " << expression << '\n';
    std::exit(1);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

using Lines = std::vector<std::unique_ptr<TextBlock>>;
Lines layout(ParsedText& text, const GfxRenderer& renderer, int width, int8_t tracking, uint8_t percent) {
  Lines lines;
  text.layoutAndExtractLines(
      renderer, 0, width, [&lines](std::unique_ptr<TextBlock> line, uint32_t) { lines.push_back(std::move(line)); },
      true, tracking, percent);
  for (const auto& line : lines) CHECK(line->valid());
  return lines;
}
BlockStyle leftStyle() {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  return style;
}

void settings() {
  const auto& fresh = SETTINGS;
  CHECK(fresh.paragraphIndentSpaces == 2);
  CHECK(fresh.characterSpacing == 2);
  CHECK(fresh.getCharacterSpacing() == 0);
  CHECK(fresh.wordSpacing == 100);
  CHECK(fresh.libraryUseMetadata == 1);
  CHECK(fresh.pluginSleepConnect == 0);
  ReaderRenderSpec spec;
  CHECK(spec.paragraphIndentSpaces == 2 && spec.characterSpacing == 0 && spec.wordSpacingPercent == 100);
  CHECK(CrossPointSettings::migrateParagraphIndentSpaces(false, 0, true) == 0);
  CHECK(CrossPointSettings::migrateParagraphIndentSpaces(false, 0, false) == 3);
  for (bool extra : {false, true}) {
    for (int spaces = 0; spaces <= 5; ++spaces)
      CHECK(CrossPointSettings::migrateParagraphIndentSpaces(true, spaces, extra) == spaces);
    CHECK(CrossPointSettings::migrateParagraphIndentSpaces(true, -1, extra) == 0);
    CHECK(CrossPointSettings::migrateParagraphIndentSpaces(true, 10, extra) == 5);
  }
  for (int percent = 50; percent <= 200; percent += 25)
    CHECK(CrossPointSettings::normalizeWordSpacing(percent) == percent);
  CHECK(CrossPointSettings::normalizeWordSpacing(-500) == 50);
  CHECK(CrossPointSettings::normalizeWordSpacing(500) == 200);
  CHECK(CrossPointSettings::normalizeWordSpacing(76) == 75);
  for (int index = 0; index <= 4; ++index) {
    SETTINGS.characterSpacing = index;
    CHECK(SETTINGS.getCharacterSpacing() == index - 2);
  }
  SETTINGS.characterSpacing = 2;
}

void indentation() {
  GfxRenderer renderer;
  for (bool extra : {false, true}) {
    for (int spaces = 0; spaces <= 5; ++spaces) {
      for (int cssIndent : {-8, 0, 12}) {
        BlockStyle style = leftStyle();
        style.textIndentDefined = true;
        style.textIndent = cssIndent;
        ParsedText text(extra, false, false, style, spaces);
        text.addWord("a", EpdFontFamily::REGULAR);
        auto lines = layout(text, renderer, 100, 2, 150);
        CHECK(lines.size() == 1);
        CHECK(lines[0]->wordXpos(0) == (cssIndent < 0 ? cssIndent : 6 * spaces));
      }
    }
  }
  ParsedText defaults(true, false, false, leftStyle());
  defaults.addWord("a", EpdFontFamily::REGULAR);
  CHECK(layout(defaults, renderer, 100, 0, 100)[0]->wordXpos(0) == 8);
}

void gapsAndBreaks() {
  GfxRenderer renderer;
  for (bool hyphenation : {false, true}) {
    for (int8_t tracking = -2; tracking <= 2; ++tracking) {
      for (int percent = 50; percent <= 200; percent += 25) {
        const int width = 16 + tracking;
        const int gap = 4 * percent / 100;
        ParsedText text(true, hyphenation, false, leftStyle(), 0);
        text.addWord("ab", EpdFontFamily::REGULAR);
        text.addWord("cd", EpdFontFamily::REGULAR);
        auto lines = layout(text, renderer, 100, tracking, percent);
        CHECK(lines.size() == 1 && lines[0]->wordCount() == 2);
        CHECK(lines[0]->wordXpos(1) == width + gap);
        CHECK(lines[0]->getBlockStyle().characterSpacing == tracking);
        ParsedText exact(true, hyphenation, false, leftStyle(), 0);
        exact.addWord("ab", EpdFontFamily::REGULAR);
        exact.addWord("cd", EpdFontFamily::REGULAR);
        CHECK(layout(exact, renderer, width * 2 + gap, tracking, percent).size() == 1);
        ParsedText narrow(true, hyphenation, false, leftStyle(), 0);
        narrow.addWord("ab", EpdFontFamily::REGULAR);
        narrow.addWord("cd", EpdFontFamily::REGULAR);
        CHECK(layout(narrow, renderer, width * 2 + gap - 1, tracking, percent).size() == 2);
      }
    }
  }
  ParsedText attached(true, false, false, leftStyle(), 0);
  attached.addWord("ab", EpdFontFamily::REGULAR);
  attached.addWord("cd", EpdFontFamily::ITALIC, false, true);
  CHECK(layout(attached, renderer, 100, 2, 200)[0]->wordXpos(1) == 20);
  ParsedText space(true, false, false, leftStyle(), 0);
  space.addWord("ab", EpdFontFamily::REGULAR);
  space.addWord(" ", EpdFontFamily::REGULAR, false, true);
  space.addWord("cd", EpdFontFamily::REGULAR, false, true);
  auto lines = layout(space, renderer, 100, 2, 150);
  CHECK(lines[0]->wordXpos(1) == 18);
  CHECK(lines[0]->wordXpos(2) == 24);
  CHECK(renderer.getTextAdvanceX(0,
                                 "a\xcc\x81"
                                 "b",
                                 EpdFontFamily::REGULAR, 2) == 18);
  CHECK(renderer.getTextAdvanceX(0, "a b", EpdFontFamily::REGULAR, 2) == 20);
  CHECK(renderer.getKerning(0, 'a', 0x0301, EpdFontFamily::REGULAR, 2) == 0);
  ParsedText combining(true, false, false, leftStyle(), 0);
  combining.addWord("a\xcc\xb8", EpdFontFamily::REGULAR);  // a + uncomposed overlay
  combining.addWord("b", EpdFontFamily::ITALIC, false, true);
  lines = layout(combining, renderer, 100, 2, 100);
  CHECK(lines[0]->wordXpos(1) == 10);  // one tracking gap between base glyphs
}

void cjkAndRtl() {
  GfxRenderer renderer;
  for (int8_t tracking = -2; tracking <= 2; ++tracking) {
    ParsedText cjk(true, false, false, leftStyle(), 0);
    cjk.addWord("中文日本語", EpdFontFamily::REGULAR);
    auto lines = layout(cjk, renderer, 100, tracking, 200);
    CHECK(lines.size() == 1 && lines[0]->wordCount() == 5);
    for (size_t i = 0; i < 5; ++i) CHECK(lines[0]->wordXpos(i) == static_cast<int>(i) * (8 + tracking));
    BlockStyle rtl;
    rtl.alignment = CssTextAlign::Right;
    rtl.isRtl = true;
    rtl.directionDefined = true;
    ParsedText text(true, false, false, rtl, 0);
    text.addWord("אב", EpdFontFamily::REGULAR);
    text.addWord("גד", EpdFontFamily::REGULAR);
    lines = layout(text, renderer, 100, tracking, 150);
    CHECK(lines.size() == 1 && lines[0]->wordCount() == 2);
    const int width = 16 + tracking;
    const int left = std::min(lines[0]->wordXpos(0), lines[0]->wordXpos(1));
    const int right = std::max(lines[0]->wordXpos(0), lines[0]->wordXpos(1));
    CHECK(right + width == 100);
    CHECK(right - left == width + 6);
  }
}

void focusAndRuby() {
  GfxRenderer renderer;
  renderer.captureDraws = true;
  renderer.draws.reserve(16);
  for (int8_t tracking = -2; tracking <= 2; ++tracking) {
    ParsedText text(true, false, true, leftStyle(), 0);
    text.addWord("reading", EpdFontFamily::REGULAR);
    text.addWord("next", EpdFontFamily::REGULAR);
    auto lines = layout(text, renderer, 200, tracking, 150);
    CHECK(lines.size() == 1 && lines[0]->focusBoundary(0) == 3);
    CHECK(lines[0]->focusSuffixX(0) == 24 + 3 * tracking);
    CHECK(lines[0]->wordXpos(1) == 56 + 6 * tracking + 6);
    renderer.draws.clear();
    lines[0]->render(renderer, 0, 0, 0);
    CHECK(renderer.draws.size() == 4);
    CHECK(renderer.draws[1].x == 24 + 3 * tracking);
    for (const auto& draw : renderer.draws) CHECK(draw.tracking == tracking);
    ParsedText ruby(true, false, false, leftStyle(), 0);
    ruby.addWord("日本語", EpdFontFamily::REGULAR);
    ruby.setRubyGroupAt(0, ruby.size(), "にほんご");
    lines = layout(ruby, renderer, 80, tracking, 100);
    CHECK(lines.size() == 1 && lines[0]->wordCount() == 3 && lines[0]->hasRuby());
    CHECK(lines[0]->wordXpos(1) - lines[0]->wordXpos(0) == 8 + tracking);
    CHECK(lines[0]->wordXpos(2) - lines[0]->wordXpos(1) == 8 + tracking);
    renderer.draws.clear();
    lines[0]->render(renderer, 0, 0, 0);
    CHECK(renderer.draws.size() == 4);
    for (const auto& draw : renderer.draws) CHECK(draw.tracking == tracking);
  }
}

void softFlushAndHyphenation() {
  GfxRenderer renderer;
  for (bool hyphenation : {false, true}) {
    ParsedText text(true, hyphenation, false, leftStyle(), 3);
    size_t words = 0, lines = 0;
    const auto collect = [&](std::unique_ptr<TextBlock> line, uint32_t offset) {
      CHECK(line->valid());
      CHECK(line->getBlockStyle().characterSpacing == 1);
      CHECK(line->wordXpos(0) == (lines == 0 ? 18 : 0));
      CHECK(offset == words * 5);
      words += line->wordCount();
      ++lines;
    };
    for (uint32_t i = 0; i < 100; ++i) text.addWord("word", EpdFontFamily::REGULAR, false, false, i * 5);
    text.layoutAndExtractLines(renderer, 0, 100, collect, false, 1, 150);
    CHECK(!text.isEmpty());
    for (uint32_t i = 100; i < 200; ++i) text.addWord("word", EpdFontFamily::REGULAR, false, false, i * 5);
    text.layoutAndExtractLines(renderer, 0, 100, collect, false, 1, 150);
    text.layoutAndExtractLines(renderer, 0, 100, collect, true, 1, 150);
    CHECK(text.isEmpty() && words == 200);
  }
  Hyphenator::setPreferredLanguage("en");
  for (int8_t tracking : {-2, 2}) {
    ParsedText text(true, true, true, leftStyle(), 0);
    text.addWord("internationalization", EpdFontFamily::REGULAR, false, false, 100);
    std::string rendered;
    uint32_t consumed = 0;
    text.layoutAndExtractLines(
        renderer, 0, 64,
        [&](std::unique_ptr<TextBlock> line, uint32_t offset) {
          CHECK(offset == 100 + consumed);
          for (size_t i = 0; i < line->wordCount(); ++i) {
            std::string token = line->wordText(i);
            if (!token.empty() && token.back() == '-') token.pop_back();
            rendered += token;
            consumed += token.size();
          }
        },
        true, tracking, 100);
    CHECK(rendered == "internationalization");
  }
}

void roundTrip() {
  GfxRenderer renderer;
  const char* path = "text-spacing-roundtrip.bin";
  CHECK(!Storage.exists(path));
  for (int8_t tracking : {-2, 2}) {
    ParsedText text(true, false, true, leftStyle(), 0);
    text.addWord("reading", EpdFontFamily::REGULAR);
    const size_t rubyStart = text.size();
    text.addWord("日本語", EpdFontFamily::REGULAR);
    text.setRubyGroupAt(rubyStart, text.size() - rubyStart, "にほんご");
    auto lines = layout(text, renderer, 200, tracking, 150);
    CHECK(lines.size() == 1);
    {
      HalFile file;
      CHECK(Storage.openFileForWrite("TEST", path, file));
      CHECK(lines[0]->serialize(file));
      CHECK(lines[0]->serialize(file));  // following block must stay aligned
    }
    {
      HalFile file;
      CHECK(Storage.openFileForRead("TEST", path, file));
      for (int blockIndex = 0; blockIndex < 2; ++blockIndex) {
        auto copy = TextBlock::deserialize(file);
        CHECK(copy && copy->valid());
        CHECK(copy->getBlockStyle().characterSpacing == tracking);
        CHECK(copy->wordCount() == lines[0]->wordCount());
        CHECK(copy->hasFocus() && copy->hasRuby());
        for (size_t i = 0; i < copy->wordCount(); ++i) {
          CHECK(std::string(copy->wordText(i)) == lines[0]->wordText(i));
          CHECK(copy->wordXpos(i) == lines[0]->wordXpos(i));
          CHECK(copy->focusSuffixX(i) == lines[0]->focusSuffixX(i));
        }
      }
      CHECK(file.position() == file.size());
    }
    {
      HalFile file;
      CHECK(file.open(path, "r+b"));
      CHECK(file.seekCur(file.size() - 1));
      CHECK(file.write(static_cast<uint8_t>(127)) == 1);
    }
    {
      HalFile file;
      CHECK(Storage.openFileForRead("TEST", path, file));
      CHECK(TextBlock::deserialize(file) != nullptr);
      CHECK(TextBlock::deserialize(file) == nullptr);
    }
    CHECK(Storage.remove(path));
  }
}
}  // namespace

int main() {
  settings();
  indentation();
  gapsAndBreaks();
  cjkAndRtl();
  focusAndRuby();
  softFlushAndHyphenation();
  roundTrip();
  std::cout << "Text spacing: " << checks << " checks passed\n";
}
