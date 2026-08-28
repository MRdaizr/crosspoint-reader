#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace {
constexpr uint16_t MAX_WORDS = 10000;
constexpr uint16_t MAX_RUBY_BYTES = 1024;
}

size_t TextBlock::arenaSize(const uint16_t wordCount, const uint16_t textSize, const bool hasFocus) {
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  return size + textSize;
}

void TextBlock::bindArenaPointers() {
  if (!arena || numWords == 0) {
    textOffsets = nullptr;
    xPositions = nullptr;
    styles = nullptr;
    focusBoundaries = nullptr;
    focusSuffixPositions = nullptr;
    textData = nullptr;
    return;
  }

  uint8_t* base = arena.get();
  textOffsets = reinterpret_cast<const uint16_t*>(base);
  base += static_cast<size_t>(numWords) * sizeof(uint16_t);
  xPositions = reinterpret_cast<const int16_t*>(base);
  base += static_cast<size_t>(numWords) * sizeof(int16_t);
  if (focusPresent) {
    focusSuffixPositions = reinterpret_cast<const uint16_t*>(base);
    base += static_cast<size_t>(numWords) * sizeof(uint16_t);
  } else {
    focusSuffixPositions = nullptr;
  }
  styles = base;
  base += static_cast<size_t>(numWords) * sizeof(uint8_t);
  if (focusPresent) {
    focusBoundaries = base;
    base += static_cast<size_t>(numWords) * sizeof(uint8_t);
  } else {
    focusBoundaries = nullptr;
  }
  textData = reinterpret_cast<const char*>(base);
}

TextBlock::TextBlock(const std::vector<std::string>& inputWords, const std::vector<int16_t>& inputXpos,
                     const std::vector<EpdFontFamily::Style>& inputStyles,
                     const std::vector<uint8_t>& inputFocusBoundary,
                     const std::vector<uint16_t>& inputFocusSuffixX, const BlockStyle& style,
                     std::vector<std::string> inputRubyTexts)
    : focusPresent(!inputFocusBoundary.empty()), blockStyle(style), rubyTexts(std::move(inputRubyTexts)) {
  if (inputWords.size() != inputXpos.size() || inputWords.size() != inputStyles.size() ||
      (focusPresent &&
       (inputWords.size() != inputFocusBoundary.size() || inputWords.size() != inputFocusSuffixX.size())) ||
      (!rubyTexts.empty() && rubyTexts.size() != inputWords.size()) ||
      inputWords.size() > MAX_WORDS || inputWords.size() > std::numeric_limits<uint16_t>::max()) {
    LOG_ERR("TXB", "Invalid text block vectors (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)",
            static_cast<uint32_t>(inputWords.size()), static_cast<uint32_t>(inputXpos.size()),
            static_cast<uint32_t>(inputStyles.size()), static_cast<uint32_t>(inputFocusBoundary.size()),
            static_cast<uint32_t>(inputFocusSuffixX.size()));
    return;
  }

  size_t requiredTextBytes = 0;
  for (const auto& word : inputWords) {
    if (word.size() > std::numeric_limits<uint16_t>::max() - 1 ||
        requiredTextBytes > std::numeric_limits<uint16_t>::max() - (word.size() + 1)) {
      LOG_ERR("TXB", "Text block payload exceeds 65535 bytes");
      return;
    }
    requiredTextBytes += word.size() + 1;
  }

  numWords = static_cast<uint16_t>(inputWords.size());
  textBytes = static_cast<uint16_t>(requiredTextBytes);
  if (numWords == 0) {
    isValid = true;
    return;
  }

  arena = makeUniqueNoThrow<uint8_t[]>(arenaSize(numWords, textBytes, focusPresent));
  if (!arena) {
    LOG_ERR("TXB", "Insufficient heap for text block arena (%u words, %u bytes)", numWords, textBytes);
    numWords = 0;
    textBytes = 0;
    return;
  }
  bindArenaPointers();

  uint8_t* base = arena.get();
  auto* offsets = reinterpret_cast<uint16_t*>(base);
  base += static_cast<size_t>(numWords) * sizeof(uint16_t);
  auto* xpos = reinterpret_cast<int16_t*>(base);
  base += static_cast<size_t>(numWords) * sizeof(int16_t);
  uint16_t* suffixX = nullptr;
  if (focusPresent) {
    suffixX = reinterpret_cast<uint16_t*>(base);
    base += static_cast<size_t>(numWords) * sizeof(uint16_t);
  }
  auto* stylesOut = base;
  base += static_cast<size_t>(numWords) * sizeof(uint8_t);
  auto* boundaries = focusPresent ? base : nullptr;
  if (focusPresent) base += static_cast<size_t>(numWords) * sizeof(uint8_t);
  char* textOut = reinterpret_cast<char*>(base);

  size_t textOffset = 0;
  for (size_t i = 0; i < inputWords.size(); ++i) {
    offsets[i] = static_cast<uint16_t>(textOffset);
    xpos[i] = inputXpos[i];
    stylesOut[i] = static_cast<uint8_t>(inputStyles[i]);
    if (focusPresent) {
      suffixX[i] = inputFocusSuffixX[i];
      boundaries[i] = inputFocusBoundary[i];
    }
    const auto& word = inputWords[i];
    std::memcpy(textOut + textOffset, word.data(), word.size());
    textOffset += word.size();
    textOut[textOffset++] = '\0';
  }
  for (const auto& ruby : rubyTexts) {
    if (ruby.size() > MAX_RUBY_BYTES) {
      LOG_ERR("TXB", "Ruby annotation exceeds %u bytes", MAX_RUBY_BYTES);
      isValid = false;
      return;
    }
  }
  if (!hasRuby()) {
    rubyTexts.clear();
  }
  isValid = true;
}

bool TextBlock::hasRuby() const {
  for (const auto& ruby : rubyTexts) {
    if (!ruby.empty()) return true;
  }
  return false;
}

const char* TextBlock::wordText(const size_t index) const {
  if (!isValid || index >= numWords || !textOffsets || !textData) return "";
  return textData + textOffsets[index];
}

size_t TextBlock::wordTextLen(const size_t index) const {
  if (!isValid || index >= numWords || !textOffsets) return 0;
  const size_t start = textOffsets[index];
  const size_t end = (index + 1 < numWords) ? textOffsets[index + 1] : textBytes;
  return end > start ? end - start - 1 : 0;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid text block");
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);
  for (size_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const size_t wordLen = wordTextLen(i);
    const int wordX = wordXpos(i) + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
        BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = focusBoundary(i);

    const int rubyShift = getRubyShift(ascender);
    int wordY = y + rubyShift;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (boundary > 0) {
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), wordLen, sizeof(boldBuf) - 1});
      std::memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, wordY, boldBuf, true, boldStyle, baseDir);
      renderer.drawText(fontId, wordX + focusSuffixX(i), wordY, word + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, wordX, wordY, word, true, currentStyle, baseDir);
    }

    // Draw one compact ruby annotation above the complete base-token group.
    // Followers carry RUBY_CONTINUE and are intentionally not drawn again.
    if (i < rubyTexts.size() && !rubyTexts[i].empty() &&
        (currentStyle & EpdFontFamily::RUBY_CONTINUE) == 0) {
      size_t groupEnd = i + 1;
      int baseLeft = wordX;
      int baseRight = wordX + renderer.getTextAdvanceX(fontId, word, currentStyle);
      while (groupEnd < numWords && (wordStyle(groupEnd) & EpdFontFamily::RUBY_CONTINUE) != 0) {
        const char* nextWord = wordText(groupEnd);
        const int nextX = wordXpos(groupEnd) + x;
        const int nextRight = nextX + renderer.getTextAdvanceX(fontId, nextWord, wordStyle(groupEnd));
        baseLeft = std::min(baseLeft, nextX);
        baseRight = std::max(baseRight, nextRight);
        ++groupEnd;
      }
      const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
      int rubyX = baseLeft + (baseRight - baseLeft - rubyWidth) / 2;
      rubyX = std::max(0, std::min(rubyX, renderer.getScreenWidth() - rubyWidth));
      const int rubyY = wordY - ascender;
      renderer.drawText(fontId, rubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::SUP, baseDir);
    }

    if (!scanning && (currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      int underlineWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);
      const int underlineY = wordY + ascender + 2;
      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        underlineWidth = (underlineWidth + 1) / 2;
      }
      renderer.drawLine(wordX, underlineY, wordX + underlineWidth, underlineY, true);
    }
    if (!scanning && (currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
      int strikeWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);
      const int strikeY = wordY + ascender * 4 / 5;
      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        strikeWidth = (strikeWidth + 1) / 2;
      }
      renderer.drawLine(wordX, strikeY, wordX + strikeWidth, strikeY, 2, true);
    }
  }
}

bool TextBlock::serialize(HalFile& file) const {
  if (!isValid || numWords > MAX_WORDS) {
    LOG_ERR("TXB", "Serialization failed: invalid text block");
    return false;
  }
  serialization::writePod(file, numWords);
  serialization::writePod(file, static_cast<uint8_t>(focusPresent ? 1 : 0));
  serialization::writePod(file, textBytes);
  const size_t bytes = arenaSize(numWords, textBytes, focusPresent);
  if (bytes > 0 && file.write(arena.get(), bytes) != bytes) {
    LOG_ERR("TXB", "Serialization failed: short arena write");
    return false;
  }

  for (size_t i = 0; i < numWords; ++i) {
    const std::string& ruby = i < rubyTexts.size() ? rubyTexts[i] : std::string();
    if (ruby.size() > MAX_RUBY_BYTES) {
      LOG_ERR("TXB", "Serialization failed: ruby annotation too large");
      return false;
    }
    serialization::writeString(file, ruby);
  }

  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);
  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wordCount = 0;
  uint8_t hasFocus = 0;
  uint16_t serializedTextBytes = 0;
  serialization::readPod(file, wordCount);
  serialization::readPod(file, hasFocus);
  serialization::readPod(file, serializedTextBytes);
  if (wordCount > MAX_WORDS || hasFocus > 1) {
    LOG_ERR("TXB", "Deserialization failed: invalid header (words=%u, focus=%u)", wordCount, hasFocus);
    return nullptr;
  }
  if (wordCount == 0 && serializedTextBytes != 0) {
    LOG_ERR("TXB", "Deserialization failed: text payload for empty block");
    return nullptr;
  }

  auto block = std::unique_ptr<TextBlock>(new (std::nothrow) TextBlock());
  if (!block) return nullptr;
  block->numWords = wordCount;
  block->textBytes = serializedTextBytes;
  block->focusPresent = hasFocus != 0;
  if (wordCount > 0) {
    block->arena = makeUniqueNoThrow<uint8_t[]>(arenaSize(wordCount, serializedTextBytes, block->focusPresent));
    if (!block->arena) {
      LOG_ERR("TXB", "Deserialization failed: text block arena allocation");
      return nullptr;
    }
    block->bindArenaPointers();
    const size_t bytes = arenaSize(wordCount, serializedTextBytes, block->focusPresent);
    if (file.read(block->arena.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("TXB", "Deserialization failed: short arena read");
      return nullptr;
    }
    // Validate every offset before exposing the block to the renderer. This
    // also guarantees that each token has a NUL terminator inside the arena.
    for (size_t i = 0; i < wordCount; ++i) {
      const size_t start = block->textOffsets[i];
      const size_t end = (i + 1 < wordCount) ? block->textOffsets[i + 1] : serializedTextBytes;
      if (start >= serializedTextBytes || end <= start || end > serializedTextBytes ||
          block->textData[end - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: invalid text offsets");
        return nullptr;
      }
    }
  } else {
    block->isValid = true;
  }

  block->rubyTexts.resize(wordCount);
  for (auto& ruby : block->rubyTexts) {
    uint32_t rubyBytes = 0;
    serialization::readPod(file, rubyBytes);
    if (rubyBytes > MAX_RUBY_BYTES) {
      LOG_ERR("TXB", "Deserialization failed: ruby annotation too large");
      return nullptr;
    }
    ruby.resize(rubyBytes);
    if (rubyBytes > 0 && file.read(&ruby[0], rubyBytes) != static_cast<int>(rubyBytes)) {
      LOG_ERR("TXB", "Deserialization failed: short ruby annotation read");
      return nullptr;
    }
  }
  if (!block->hasRuby()) {
    block->rubyTexts.clear();
  }

  serialization::readPod(file, block->blockStyle.alignment);
  serialization::readPod(file, block->blockStyle.textAlignDefined);
  serialization::readPod(file, block->blockStyle.marginTop);
  serialization::readPod(file, block->blockStyle.marginBottom);
  serialization::readPod(file, block->blockStyle.marginLeft);
  serialization::readPod(file, block->blockStyle.marginRight);
  serialization::readPod(file, block->blockStyle.paddingTop);
  serialization::readPod(file, block->blockStyle.paddingBottom);
  serialization::readPod(file, block->blockStyle.paddingLeft);
  serialization::readPod(file, block->blockStyle.paddingRight);
  serialization::readPod(file, block->blockStyle.textIndent);
  serialization::readPod(file, block->blockStyle.textIndentDefined);
  serialization::readPod(file, block->blockStyle.isRtl);
  serialization::readPod(file, block->blockStyle.directionDefined);
  block->isValid = true;
  return block;
}
