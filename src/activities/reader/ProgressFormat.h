#pragma once

#include <cstdint>
#include <optional>

// Pure progress.bin byte-format logic, deliberately free of Arduino/HAL includes so it's
// host-testable without hardware (see test/progress_format/).
namespace EpubReaderUtils {

struct SavedProgress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  std::optional<uint32_t> visibleTextOffset;
};

struct SerializedProgress {
  uint8_t data[10] = {};
  size_t len = 6;
};

// Encodes progress.bin's bytes. Inverse of parseProgress below -- keeping both directions in one
// place means the byte layout only needs to be described once. Does not validate range; callers
// with a HAL to log through (EpubReaderUtils::saveProgress) do that before calling this.
inline SerializedProgress serializeProgress(const int spineIndex, const int pageNumber, const int pageCount,
                                            const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  SerializedProgress out;
  out.data[0] = spineIndex & 0xFF;
  out.data[1] = (spineIndex >> 8) & 0xFF;
  out.data[2] = pageNumber & 0xFF;
  out.data[3] = (pageNumber >> 8) & 0xFF;
  out.data[4] = pageCount & 0xFF;
  out.data[5] = (pageCount >> 8) & 0xFF;
  if (visibleTextOffset.has_value()) {
    const uint32_t offset = *visibleTextOffset;
    out.data[6] = offset & 0xFF;
    out.data[7] = (offset >> 8) & 0xFF;
    out.data[8] = (offset >> 16) & 0xFF;
    out.data[9] = (offset >> 24) & 0xFF;
    out.len = 10;
  }
  return out;
}

// Parses the raw bytes of progress.bin. Length discriminates the format -- no version byte:
//   4 bytes:  spineIndex, pageNumber
//   6 bytes:  + pageCount
//   10 bytes: + visibleTextOffset (LE uint32)
// A shorter file parses fine with the missing fields left at their defaults (visibleTextOffset
// unset), so the reader falls back to its page-fraction reposition path for that file with no
// separate migration step.
inline std::optional<SavedProgress> parseProgress(const uint8_t* data, const int len) {
  if (len != 4 && len != 6 && len != 10) {
    return std::nullopt;
  }
  SavedProgress progress;
  progress.spineIndex = data[0] | (data[1] << 8);
  progress.pageNumber = data[2] | (data[3] << 8);
  if (len >= 6) {
    progress.pageCount = data[4] | (data[5] << 8);
  }
  if (len == 10) {
    progress.visibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                 (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
  }
  return progress;
}

}  // namespace EpubReaderUtils
