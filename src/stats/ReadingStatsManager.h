#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "Logging.h"
#include "stats/StatsTypes.h"

static constexpr uint32_t STATS_MIN_SESSION_MS = 3UL * 60UL * 1000UL;
static constexpr const char* STATS_FILE_PATH = "/.crosspoint/stats.bin";

// -----------------------------------------------------------------------
// ReadingStatsManager singleton
// -----------------------------------------------------------------------
class ReadingStatsManager {
 public:
  static ReadingStatsManager& getInstance() {
    static ReadingStatsManager instance;
    return instance;
  }

  bool load();

  void beginSession(const char* cacheKey, const char* title, const char* author, const char* bookPath,
                    const char* thumbBmpPath, uint8_t progressPercent);

  void endSession(uint8_t progressPercent, uint32_t sessionPagesTurned);

  const GlobalStats& getGlobal() const { return global; }
  uint16_t getBookCount() const { return static_cast<uint16_t>(bookIndex.size()); }

  // Resident summary for every book, in display order. Use this for counting,
  // filtering and sorting -- it never touches the SD card.
  const BookStatIndexEntry& getBookSummary(uint16_t index) const { return bookIndex[index]; }

  // Full entry for one row, faulted in from stats.bin through a small cache.
  //
  // The returned reference is only guaranteed valid until the next getBook()
  // call, which may evict its slot. Callers that hold it across further
  // getBook() calls must copy it first.
  const BookStatEntry& getBook(uint16_t index);

  // Streams every book through `visit` in display order using a single file
  // open. For whole-library passes that need fields the summary doesn't carry
  // (paths, titles); a plain function pointer keeps this off std::function.
  using EntryVisitor = void (*)(void* ctx, uint16_t index, const BookStatEntry& entry);
  void forEachEntry(void* ctx, EntryVisitor visit);

  // Removes the entry at `index` from the display index, compacts it out of
  // stats.bin, and persists the new state. Returns false if the index is
  // out of range. Lifetime counters (totalReadingMs, totalSessionCount,
  // totalBooksFinished) are intentionally left untouched -- removing a stats
  // row hides the book from the list, it does not retroactively erase the
  // time the user spent reading it.
  bool removeBook(uint16_t index);
  uint32_t getLast7SessionsMs() const;
  uint8_t getLast7SessionCount() const { return global.sessionRingCount; }

  void reset();
  void setDailyGoalMinutes(uint16_t minutes);

 private:
  ReadingStatsManager() = default;

  bool saveGlobal();
  bool migrateFrom(uint8_t fileVersion);
  bool writeEntry(uint16_t diskSlot, const BookStatEntry& entry);
  bool readEntry(uint16_t diskSlot, BookStatEntry& out) const;
  bool appendEntry(const BookStatEntry& entry, uint16_t& diskSlotOut);
  int findBook(const char* cacheKey);
  void sortByProgress();
  void refreshSummary(uint16_t index, const BookStatEntry& entry);
  void invalidateCache();

  static uint32_t entryOffset(uint16_t diskSlot) {
    return sizeof(GlobalStats) + static_cast<uint32_t>(diskSlot) * sizeof(BookStatEntry);
  }

  GlobalStats global{};

  // Display order; index i maps to on-disk slot bookIndex[i].diskSlot.
  std::vector<BookStatIndexEntry> bookIndex;

  BookStatEntry cache[STATS_BOOK_CACHE_SLOTS]{};
  uint16_t cacheSlotFor[STATS_BOOK_CACHE_SLOTS] = {STATS_INVALID_BOOK, STATS_INVALID_BOOK, STATS_INVALID_BOOK,
                                                   STATS_INVALID_BOOK};
  uint32_t cacheUsedAt[STATS_BOOK_CACHE_SLOTS] = {0, 0, 0, 0};
  uint32_t cacheClock = 0;

  // Scratch for whole-library streaming passes (load, remove-compact). 488
  // bytes is far past the 256-byte stack budget, so it lives here rather than
  // being built on a task stack.
  BookStatEntry scratch{};

  TickType_t sessionStartTick = 0;
  bool sessionActive = false;
  uint16_t sessionBookIndex = STATS_INVALID_BOOK;
};

#define StatsManager ReadingStatsManager::getInstance()
