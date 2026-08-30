#pragma once

#include <cstdint>

// All layout-affecting reader settings in one value object.  Section overloads
// accept this type so EPUB/TXT/XTC can converge on the same render contract
// without changing the existing on-disk cache header in this migration.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
};
