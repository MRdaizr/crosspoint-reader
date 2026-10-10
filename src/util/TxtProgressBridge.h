#pragma once
#include <TxtSourceMap.h>

#include <string>

// The caller holds the render lock. No reader/session is started by this bridge.
class TxtProgressBridge {
 public:
  enum class Result { Ready, Migrated, NoProgress, NeedsCompatibility, IoError, Cancelled };
  TxtProgressBridge(const std::string& sourcePath, const std::string& unifiedCachePath, const TxtSourceMap& sourceMap);
  Result migrateToUnified(const TxtToHtml::Callbacks* callbacks = nullptr);
  // Call ONLY AFTER a successful unified 10-byte progress save, before releasing
  // the reader's EPUB. pageHint is advisory; the source byte offset is authoritative.
  bool mirrorUnifiedVisibleOffset(uint32_t visibleOffset, uint32_t legacyPageHint,
                                  const TxtToHtml::Callbacks* callbacks = nullptr);

 private:
  std::string sourcePath, unifiedCachePath, legacyCachePath;
  const TxtSourceMap& sourceMap;
  bool legacyOffset(const uint8_t* data, size_t size, uint32_t& offset, uint32_t& page, bool allowIndex,
                    const TxtToHtml::Callbacks* callbacks) const;
  bool legacyIndexOffset(uint32_t requestedPage, bool lastPage, uint32_t& offset, uint32_t& page,
                         const TxtToHtml::Callbacks* callbacks) const;
};
