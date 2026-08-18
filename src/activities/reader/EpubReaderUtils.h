#pragma once

#include <Epub.h>
#include <Logging.h>

#include <cstdint>
#include <optional>

#include "ProgressFile.h"
#include "ProgressFormat.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, const int spineIndex, const int pageNumber, const int pageCount,
                         const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const SerializedProgress serialized = serializeProgress(spineIndex, pageNumber, pageCount, visibleTextOffset);
  if (!ProgressFile::writeAtomic(epub.getCachePath(), serialized.data, serialized.len)) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
