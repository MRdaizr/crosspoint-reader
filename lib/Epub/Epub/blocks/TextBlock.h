#pragma once

#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page.
//
// TextBlock is deliberately backed by one allocation. A line can contain
// hundreds of CJK tokens, and keeping one std::string plus several vectors per
// token creates a large amount of allocator metadata and fragmentation on the
// ESP32. The arena also becomes the on-disk payload, so deserializing a page
// does not recreate a collection of small heap objects.
class TextBlock final : public Block {
 private:
  uint16_t numWords = 0;
  uint16_t textBytes = 0;
  bool focusPresent = false;
  bool isValid = false;
  BlockStyle blockStyle;
  std::unique_ptr<uint8_t[]> arena;

  const uint16_t* textOffsets = nullptr;
  const int16_t* xPositions = nullptr;
  const uint8_t* styles = nullptr;
  const uint8_t* focusBoundaries = nullptr;
  const uint16_t* focusSuffixPositions = nullptr;
  const char* textData = nullptr;
  std::vector<std::string> rubyTexts;

  static size_t arenaSize(uint16_t wordCount, uint16_t textSize, bool hasFocus);
  void bindArenaPointers();
  TextBlock() = default;

 public:
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& word_xpos,
                     const std::vector<EpdFontFamily::Style>& word_styles,
                      const std::vector<uint8_t>& focus_boundary,
                      const std::vector<uint16_t>& focus_suffix_x,
                      const BlockStyle& blockStyle = BlockStyle(),
                      std::vector<std::string> rubyTexts = {});
  ~TextBlock() override = default;

  void setBlockStyle(const BlockStyle& style) { blockStyle = style; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }

  bool isEmpty() override { return numWords == 0; }
  size_t wordCount() const { return numWords; }
  bool valid() const { return isValid; }
  bool hasFocus() const { return focusPresent; }
  bool hasRuby() const;
  int getRubyShift(int ascender) const { return hasRuby() ? (ascender / 2) : 0; }
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }

  const char* wordText(size_t index) const;
  size_t wordTextLen(size_t index) const;
  int16_t wordXpos(size_t index) const { return index < numWords ? xPositions[index] : 0; }
  EpdFontFamily::Style wordStyle(size_t index) const {
    return index < numWords ? static_cast<EpdFontFamily::Style>(styles[index]) : EpdFontFamily::REGULAR;
  }
  uint8_t focusBoundary(size_t index) const {
    return focusPresent && index < numWords ? focusBoundaries[index] : 0;
  }
  uint16_t focusSuffixX(size_t index) const {
    return focusPresent && index < numWords ? focusSuffixPositions[index] : 0;
  }

  // Given a renderer, works out where to break the words into lines.
  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};
