#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Pure page-lookup math for Section's in-memory (still-building) visible-text offset LUT. Also
// used directly by tests to verify the algorithm. Section's on-disk lookup re-implements the same
// comparison inline as a streaming scan rather than calling this, since pageCount is
// caller-controlled (untrusted EPUB content) and materializing the on-disk LUT into a vector
// first would risk an unbounded allocation -- keep the two in sync if this logic ever changes.
namespace VisibleTextOffsetUtils {

// pageStartOffsets[i] is the visible-text offset the i-th page starts at (monotonically
// non-decreasing). Returns the last page whose start is <= target, or nullopt if target falls
// before the first page (empty input, or target < pageStartOffsets[0]).
//
// preferFirstAtOffset stops at the first page whose start exactly equals target instead of the
// last page in a run of equal starts (consecutive zero-width pages, e.g. image-only pages that
// don't advance the counter) -- for landing precisely on a bookmark rather than sliding past it.
inline std::optional<uint16_t> findPageForOffset(const std::vector<uint32_t>& pageStartOffsets, const uint32_t target,
                                                 const bool preferFirstAtOffset = false) {
  std::optional<uint16_t> best;
  for (size_t page = 0; page < pageStartOffsets.size(); page++) {
    if (pageStartOffsets[page] > target) break;
    best = static_cast<uint16_t>(page);
    if (preferFirstAtOffset && pageStartOffsets[page] == target) break;
  }
  return best;
}

}  // namespace VisibleTextOffsetUtils
