#pragma once

// Reader-menu position remains available while child screens temporarily
// release the current Section to reclaim its pagination memory.
struct ChapterPosition {
  int pageIndex = 0;   // zero-based page within the chapter
  int totalPages = 0;  // best-known chapter page count, zero while unknown

  constexpr int displayPage() const { return pageIndex + 1; }
  constexpr bool hasTotal() const { return totalPages > 0; }
  constexpr float chapterFraction() const {
    return hasTotal() ? static_cast<float>(pageIndex) / static_cast<float>(totalPages) : 0.0f;
  }
};
