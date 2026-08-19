#include "stats/ReadingStatsManager.h"

#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"

namespace {
// Returned when a row cannot be read, so callers still get a valid reference.
const BookStatEntry kEmptyBook{};
}  // namespace

void ReadingStatsManager::invalidateCache() {
  for (uint8_t s = 0; s < STATS_BOOK_CACHE_SLOTS; ++s) {
    cacheSlotFor[s] = STATS_INVALID_BOOK;
    cacheUsedAt[s] = 0;
  }
  cacheClock = 0;
}

void ReadingStatsManager::refreshSummary(uint16_t index, const BookStatEntry& entry) {
  BookStatIndexEntry& summary = bookIndex[index];
  summary.cacheKeyHash = stats::hashCacheKey(entry.cacheKey);
  summary.totalReadingMs = entry.totalReadingMs;
  summary.lastReadDay = entry.lastReadDay;
  summary.progressPercent = entry.progressPercent;
}

bool ReadingStatsManager::readEntry(uint16_t diskSlot, BookStatEntry& out) const {
  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) return false;
  bool ok = f.seek(entryOffset(diskSlot));
  if (ok) ok = f.read(&out, sizeof(BookStatEntry)) == static_cast<int>(sizeof(BookStatEntry));
  f.close();
  return ok;
}

bool ReadingStatsManager::writeEntry(uint16_t diskSlot, const BookStatEntry& entry) {
  // O_RDWR|O_CREAT, not openFileForWrite -- that carries O_TRUNC and would
  // discard every other book's record on each single-row update.
  FsFile f = Storage.open(STATS_FILE_PATH, O_RDWR | O_CREAT);
  if (!f) {
    LOG_ERR("STATS", "Could not open stats file to update slot %u", static_cast<unsigned>(diskSlot));
    return false;
  }
  bool ok = f.seek(entryOffset(diskSlot));
  if (ok) ok = f.write(&entry, sizeof(BookStatEntry)) == sizeof(BookStatEntry);
  f.flush();
  f.close();
  if (!ok) LOG_ERR("STATS", "Failed writing book entry at slot %u", static_cast<unsigned>(diskSlot));
  return ok;
}

bool ReadingStatsManager::appendEntry(const BookStatEntry& entry, uint16_t& diskSlotOut) {
  const uint16_t slot = static_cast<uint16_t>(global.bookCountTotal);
  if (!writeEntry(slot, entry)) return false;
  global.bookCountTotal++;
  global.bookCount = static_cast<uint8_t>(std::min<uint32_t>(global.bookCountTotal, 255u));
  diskSlotOut = slot;
  return true;
}

const BookStatEntry& ReadingStatsManager::getBook(uint16_t index) {
  if (index >= bookIndex.size()) return kEmptyBook;
  const uint16_t diskSlot = bookIndex[index].diskSlot;

  for (uint8_t s = 0; s < STATS_BOOK_CACHE_SLOTS; ++s) {
    if (cacheSlotFor[s] == diskSlot) {
      cacheUsedAt[s] = ++cacheClock;
      return cache[s];
    }
  }

  uint8_t victim = 0;
  for (uint8_t s = 1; s < STATS_BOOK_CACHE_SLOTS; ++s) {
    if (cacheUsedAt[s] < cacheUsedAt[victim]) victim = s;
  }

  if (!readEntry(diskSlot, cache[victim])) {
    LOG_ERR("STATS", "Failed reading book entry at slot %u", static_cast<unsigned>(diskSlot));
    cacheSlotFor[victim] = STATS_INVALID_BOOK;
    cacheUsedAt[victim] = 0;
    return kEmptyBook;
  }
  cacheSlotFor[victim] = diskSlot;
  cacheUsedAt[victim] = ++cacheClock;
  return cache[victim];
}

void ReadingStatsManager::forEachEntry(void* ctx, EntryVisitor visit) {
  if (bookIndex.empty()) return;
  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) {
    LOG_ERR("STATS", "Could not open stats file to stream entries");
    return;
  }
  for (uint16_t i = 0; i < bookIndex.size(); ++i) {
    if (!f.seek(entryOffset(bookIndex[i].diskSlot))) break;
    if (f.read(&scratch, sizeof(BookStatEntry)) != static_cast<int>(sizeof(BookStatEntry))) break;
    visit(ctx, i, scratch);
  }
  f.close();
}

bool ReadingStatsManager::load() {
  bookIndex.clear();
  invalidateCache();

  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) {
    LOG_DBG("STATS", "No stats file found, starting fresh");
    global = GlobalStats{};
    global.version = STATS_FILE_VERSION;
    global.goalTarget = STATS_DEFAULT_GOAL_MINUTES;
    return true;
  }

  uint8_t fileVersion = 0;
  f.read(&fileVersion, 1);
  f.seek(0);

  if (fileVersion < STATS_FILE_VERSION) {
    f.close();
    return migrateFrom(fileVersion);
  }

  f.read(&global, sizeof(GlobalStats));
  global.version = STATS_FILE_VERSION;

  uint32_t count = global.bookCountTotal;
  if (count > STATS_MAX_INDEXED_BOOKS) {
    LOG_ERR("STATS", "bookCountTotal %u exceeds %u, clamping", static_cast<unsigned>(count),
            static_cast<unsigned>(STATS_MAX_INDEXED_BOOKS));
    count = STATS_MAX_INDEXED_BOOKS;
  }

  bookIndex.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (f.read(&scratch, sizeof(BookStatEntry)) != static_cast<int>(sizeof(BookStatEntry))) {
      LOG_ERR("STATS", "Truncated stats file: got %u of %u entries", static_cast<unsigned>(i),
              static_cast<unsigned>(count));
      break;
    }
    bookIndex.push_back(BookStatIndexEntry{});
    bookIndex.back().diskSlot = static_cast<uint16_t>(i);
    refreshSummary(static_cast<uint16_t>(bookIndex.size() - 1), scratch);
  }
  f.close();

  if (bookIndex.size() != global.bookCountTotal) {
    global.bookCountTotal = static_cast<uint32_t>(bookIndex.size());
    global.bookCount = static_cast<uint8_t>(std::min<size_t>(bookIndex.size(), 255u));
    saveGlobal();
  }

  sortByProgress();
  return true;
}

// v4..v8 held at most STATS_LEGACY_MAX_BOOK_ENTRIES rows, so the whole old
// library fits in one bounded transient buffer; the file is then rewritten in
// v9 layout. Each older layout is a strict prefix of the current struct, so
// reading the old byte count into a zero-initialised struct leaves the new
// tail at 0. GlobalStats sizes: v4=40, v5/v6=44, v7=808, v8=812.
bool ReadingStatsManager::migrateFrom(uint8_t fileVersion) {
  LOG_INF("STATS", "Migrating stats %d -> %d", fileVersion, STATS_FILE_VERSION);

  FsFile f;
  if (!Storage.openFileForRead("STATS", STATS_FILE_PATH, f)) return false;

  global = GlobalStats{};
  size_t globalSize;
  if (fileVersion == 4) {
    globalSize = 40;
  } else if (fileVersion == 7) {
    globalSize = 808;
  } else if (fileVersion == 8) {
    globalSize = 812;
  } else {
    globalSize = 44;  // v5, v6
  }
  f.read(&global, globalSize);
  global.version = STATS_FILE_VERSION;

  const uint8_t legacyCount = std::min<uint8_t>(global.bookCount, STATS_LEGACY_MAX_BOOK_ENTRIES);
  // 9 x 488 B transient; far past the 256-byte stack budget, and only alive for
  // the duration of this one-shot migration.
  auto legacy = makeUniqueNoThrow<BookStatEntry[]>(STATS_LEGACY_MAX_BOOK_ENTRIES);
  if (!legacy) {
    LOG_ERR("STATS", "malloc failed: %u", static_cast<unsigned>(STATS_LEGACY_MAX_BOOK_ENTRIES * sizeof(BookStatEntry)));
    f.close();
    return false;
  }
  memset(legacy.get(), 0, STATS_LEGACY_MAX_BOOK_ENTRIES * sizeof(BookStatEntry));

  for (uint8_t i = 0; i < legacyCount; ++i) {
    if (fileVersion == 7 || fileVersion == 8) {
      f.read(&legacy[i], 488);
    } else if (fileVersion == 5) {
      // v5 entry was 464 bytes. lastSessionMs (v6) is a new 4-byte field, so
      // read up to totalPagesRead then land the v5 tail at its new offset.
      f.read(&legacy[i], 460);
      f.read(&legacy[i].progressPercent, 4);
    } else if (fileVersion == 6) {
      // v6 entry was 468 bytes; v7 keeps that exact prefix.
      f.read(&legacy[i], 468);
    } else {
      f.read(&legacy[i], 396);  // v4
    }
  }
  f.close();

  if (global.goalTarget == 0) global.goalTarget = STATS_DEFAULT_GOAL_MINUTES;
  stats::evaluateAchievements(global, -1);

  global.bookCountTotal = legacyCount;
  global.bookCount = legacyCount;

  FsFile out;
  if (!Storage.openFileForWrite("STATS", STATS_FILE_PATH, out)) {
    LOG_ERR("STATS", "Could not open stats file to write migrated layout");
    return false;
  }
  out.write(&global, sizeof(GlobalStats));
  for (uint8_t i = 0; i < legacyCount; ++i) {
    out.write(&legacy[i], sizeof(BookStatEntry));
  }
  out.flush();
  out.close();

  bookIndex.reserve(legacyCount);
  for (uint8_t i = 0; i < legacyCount; ++i) {
    bookIndex.push_back(BookStatIndexEntry{});
    bookIndex.back().diskSlot = i;
    refreshSummary(i, legacy[i]);
  }
  invalidateCache();
  sortByProgress();
  return true;
}

bool ReadingStatsManager::saveGlobal() {
  FsFile f = Storage.open(STATS_FILE_PATH, O_RDWR | O_CREAT);
  if (!f) {
    LOG_ERR("STATS", "Could not open stats file for write");
    return false;
  }
  bool ok = f.seek(0);
  if (ok) ok = f.write(&global, sizeof(GlobalStats)) == sizeof(GlobalStats);
  f.flush();
  f.close();
  if (!ok) LOG_ERR("STATS", "Failed writing stats header");
  return ok;
}

// Matches on the resident hash first, confirming against the full cacheKey from
// disk only on a hash hit, so a lookup costs at most one entry read.
int ReadingStatsManager::findBook(const char* cacheKey) {
  const uint32_t wanted = stats::hashCacheKey(cacheKey);
  for (uint16_t i = 0; i < bookIndex.size(); ++i) {
    if (bookIndex[i].cacheKeyHash != wanted) continue;
    if (!readEntry(bookIndex[i].diskSlot, scratch)) continue;
    if (strncmp(scratch.cacheKey, cacheKey, sizeof(scratch.cacheKey)) == 0) return i;
  }
  return -1;
}

// Sorts the resident index only -- on-disk entries never move, so reordering
// the list is free of SD I/O. std::stable_sort keeps equal-progress rows in
// their existing relative order instead of shuffling them each save.
void ReadingStatsManager::sortByProgress() {
  std::stable_sort(bookIndex.begin(), bookIndex.end(), [](const BookStatIndexEntry& a, const BookStatIndexEntry& b) {
    return a.progressPercent > b.progressPercent;
  });
}

void ReadingStatsManager::beginSession(const char* cacheKey, const char* title, const char* author,
                                       const char* bookPath, const char* thumbBmpPath, uint8_t progressPercent) {
  sessionStartTick = xTaskGetTickCount();
  sessionActive = true;

  int idx = findBook(cacheKey);
  if (idx == -1) {
    if (bookIndex.size() >= STATS_MAX_INDEXED_BOOKS) {
      LOG_ERR("STATS", "Book index full at %u entries, not tracking this book",
              static_cast<unsigned>(bookIndex.size()));
      sessionActive = false;
      sessionBookIndex = STATS_INVALID_BOOK;
      return;
    }

    memset(&scratch, 0, sizeof(BookStatEntry));
    strncpy(scratch.cacheKey, cacheKey, sizeof(scratch.cacheKey) - 1);
    strncpy(scratch.title, title, sizeof(scratch.title) - 1);
    strncpy(scratch.author, author, sizeof(scratch.author) - 1);
    strncpy(scratch.bookPath, bookPath, sizeof(scratch.bookPath) - 1);
    strncpy(scratch.thumbBmpPath, thumbBmpPath, sizeof(scratch.thumbBmpPath) - 1);

    uint16_t diskSlot = 0;
    if (!appendEntry(scratch, diskSlot)) {
      sessionActive = false;
      sessionBookIndex = STATS_INVALID_BOOK;
      return;
    }
    bookIndex.push_back(BookStatIndexEntry{});
    bookIndex.back().diskSlot = diskSlot;
    sessionBookIndex = static_cast<uint16_t>(bookIndex.size() - 1);
    refreshSummary(sessionBookIndex, scratch);
    saveGlobal();
  } else {
    sessionBookIndex = static_cast<uint16_t>(idx);
    if (!readEntry(bookIndex[sessionBookIndex].diskSlot, scratch)) {
      sessionActive = false;
      sessionBookIndex = STATS_INVALID_BOOK;
      return;
    }
    strncpy(scratch.bookPath, bookPath, sizeof(scratch.bookPath) - 1);
    strncpy(scratch.thumbBmpPath, thumbBmpPath, sizeof(scratch.thumbBmpPath) - 1);
    // NOTE: progressPercent is intentionally NOT overwritten here. The value
    // passed in is byte-weighted at chapter-start (read from progress.bin),
    // which is less precise than the page-precise value endSession() saves on
    // exit. Overwriting on every entry would cause the Stats screen to flicker
    // back to a coarser percentage every time the user opens the book.
    writeEntry(bookIndex[sessionBookIndex].diskSlot, scratch);
    invalidateCache();
  }
}

void ReadingStatsManager::endSession(uint8_t progressPercent, uint32_t sessionPagesTurned) {
  if (!sessionActive) return;
  sessionActive = false;

  const uint32_t elapsedMs = (xTaskGetTickCount() - sessionStartTick) * portTICK_PERIOD_MS;
  // The "long enough" gate exists only to keep a 5-second "peek" from
  // overwriting the user-visible Last Session row. Global totals and per-book
  // running totals always count - otherwise All Time and Finished Books stay
  // at zero for users whose typical sessions are under three minutes.
  const bool longEnoughForLastSession = (elapsedMs >= STATS_MIN_SESSION_MS);

  const bool haveBook = sessionBookIndex < bookIndex.size();
  const uint16_t diskSlot = haveBook ? bookIndex[sessionBookIndex].diskSlot : 0;
  if (haveBook && !readEntry(diskSlot, scratch)) {
    LOG_ERR("STATS", "Failed reading book entry at slot %u on session end", static_cast<unsigned>(diskSlot));
    return;
  }

  if (haveBook) {
    // Always reflect the user's true current position. The reader is the only
    // caller now (the deep-sleep safety-net endSession(0, 0) was removed in
    // main.cpp:enterDeepSleep) and it always passes a precise, page-accurate
    // value, so a smaller percentage means the user really did navigate
    // backward. The "Finished Books" lifetime counter only ever increments,
    // so regressing from 100% does not decrement it.
    if (progressPercent == 100 && scratch.progressPercent < 100) {
      global.totalBooksFinished++;
    }
    scratch.progressPercent = progressPercent;

    if (longEnoughForLastSession) {
      scratch.lastSessionMs = elapsedMs;
    }

    scratch.totalReadingMs += elapsedMs;
    scratch.sessionCount++;
    scratch.totalPagesRead += sessionPagesTurned;
  }

  global.totalReadingMs += elapsedMs;
  global.totalSessionCount++;
  global.totalPagesLifetime += sessionPagesTurned;
  global.sessionRing[global.sessionRingHead] = elapsedMs;
  global.sessionRingHead = (global.sessionRingHead + 1) % STATS_SESSION_RING_SIZE;
  if (global.sessionRingCount < STATS_SESSION_RING_SIZE) {
    global.sessionRingCount++;
  }

  // Per-day history, streak and goal only accrue when a real wall-clock day is
  // known. The X4 has no RTC, so time() is only valid after one NTP sync;
  // epochValid() gates on that. Reuse the long-session gate so a brief peek
  // does not register a reading day.
  const int64_t nowEpoch = static_cast<int64_t>(time(nullptr));
  if (longEnoughForLastSession && stats::epochValid(nowEpoch)) {
    const uint16_t today = stats::dayNumber(nowEpoch, stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ));
    const uint16_t minutes = static_cast<uint16_t>(elapsedMs / 60000UL);
    stats::updatePet(global, nowEpoch, sessionPagesTurned);
    stats::recordReadingDay(global, today, minutes);
    if (haveBook) {
      BookStatEntry& b = scratch;
      b.lastReadDay = today;
      if (sessionPagesTurned > 0 && elapsedMs > 0) {
        const uint32_t pph = static_cast<uint32_t>(sessionPagesTurned) * 3600000UL / elapsedMs;
        b.speedSamples[b.speedHead % 4].pagesPerHour = pph > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(pph);
        b.speedSamples[b.speedHead % 4].minutesInSession = minutes;
        b.speedHead = static_cast<uint8_t>((b.speedHead + 1) % 4);
        if (b.speedCount < 4) b.speedCount++;
      }
    }
  }

  int sessionEndHour = -1;
  if (stats::epochValid(nowEpoch)) {
    const int offset = stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ);
    const int64_t localEpoch = nowEpoch + offset;
    sessionEndHour = static_cast<int>((localEpoch % 86400) / 3600);
    global.lastSyncedDay = stats::dayNumber(nowEpoch, offset);
  }
  stats::evaluateAchievements(global, sessionEndHour);

  if (haveBook) {
    writeEntry(diskSlot, scratch);
    refreshSummary(sessionBookIndex, scratch);
    invalidateCache();
  }
  saveGlobal();
  sortByProgress();
}

bool ReadingStatsManager::removeBook(uint16_t index) {
  if (index >= bookIndex.size()) return false;

  // If the active session is for this book, drop the binding so endSession
  // doesn't write back to a stale slot after the shift.
  if (sessionActive && sessionBookIndex == index) {
    sessionActive = false;
    sessionBookIndex = STATS_INVALID_BOOK;
  }

  const uint16_t removedSlot = bookIndex[index].diskSlot;

  // Entries after the hole shift down one slot on disk so the file stays a
  // dense array indexable by slot. Only ever runs on an explicit user delete.
  for (uint16_t slot = removedSlot; slot + 1 < global.bookCountTotal; ++slot) {
    if (!readEntry(static_cast<uint16_t>(slot + 1), scratch)) {
      LOG_ERR("STATS", "Failed compacting stats at slot %u", static_cast<unsigned>(slot + 1));
      return false;
    }
    if (!writeEntry(slot, scratch)) return false;
  }

  bookIndex.erase(bookIndex.begin() + index);
  for (auto& summary : bookIndex) {
    if (summary.diskSlot > removedSlot) summary.diskSlot--;
  }
  if (sessionActive && sessionBookIndex != STATS_INVALID_BOOK && sessionBookIndex > index) {
    sessionBookIndex--;
  }

  global.bookCountTotal--;
  global.bookCount = static_cast<uint8_t>(std::min<uint32_t>(global.bookCountTotal, 255u));
  invalidateCache();

  // The last record is now a duplicate of its predecessor, but load() only ever
  // reads bookCountTotal entries so it is inert, and the next appended book
  // overwrites it. Not worth a truncate round-trip on the SD card.
  return saveGlobal();
}

uint32_t ReadingStatsManager::getLast7SessionsMs() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < global.sessionRingCount; ++i) {
    total += global.sessionRing[i];
  }
  return total;
}

void ReadingStatsManager::reset() {
  global = GlobalStats{};
  global.version = STATS_FILE_VERSION;
  global.goalTarget = STATS_DEFAULT_GOAL_MINUTES;
  bookIndex.clear();
  invalidateCache();
  sessionActive = false;
  sessionBookIndex = STATS_INVALID_BOOK;

  // Truncating rewrite: drops every book record along with the counters.
  FsFile f;
  if (!Storage.openFileForWrite("STATS", STATS_FILE_PATH, f)) {
    LOG_ERR("STATS", "Could not open stats file to reset");
    return;
  }
  f.write(&global, sizeof(GlobalStats));
  f.flush();
  f.close();
}

void ReadingStatsManager::setDailyGoalMinutes(uint16_t minutes) {
  global.goalTarget = minutes;
  saveGlobal();
}