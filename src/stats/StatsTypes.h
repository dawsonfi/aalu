#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Highest book count a v8-or-older file could hold; still the migration bound.
static constexpr uint8_t STATS_LEGACY_MAX_BOOK_ENTRIES = 9;
// Resident full-entry slots. The list shows 3 rows, so 4 covers the visible
// window plus the row a detail view or session write is touching.
static constexpr uint8_t STATS_BOOK_CACHE_SLOTS = 4;
// Guard against a corrupt header allocating an unbounded index, not a product
// limit -- 4000 entries is ~64 KB of index and far past any real library.
static constexpr uint16_t STATS_MAX_INDEXED_BOOKS = 4000;
static constexpr uint16_t STATS_INVALID_BOOK = 0xFFFF;
static constexpr uint8_t STATS_SESSION_RING_SIZE = 7;
static constexpr uint16_t STATS_DAY_RING_SIZE = 365;
static constexpr uint8_t STATS_FILE_VERSION = 9;
static constexpr uint16_t STATS_DEFAULT_GOAL_MINUTES = 30;

/**
 * Achievement flags packed into GlobalStats.achievementBits (one bit each).
 * Tiered families (streak/books/pages/hours) plus two time-of-day badges are
 * derived from the persisted aggregates by stats::evaluateAchievements().
 * 16 of 32 bits are used; the remainder are reserved for future badges.
 */
enum AchievementBit : uint32_t {
  ACH_STREAK_3 = 1u << 0,     ///< Current/longest reading streak reached 3 days.
  ACH_STREAK_7 = 1u << 1,     ///< Reading streak reached 7 days.
  ACH_STREAK_30 = 1u << 2,    ///< Reading streak reached 30 days.
  ACH_STREAK_100 = 1u << 3,   ///< Reading streak reached 100 days.
  ACH_BOOKS_1 = 1u << 4,      ///< Finished the first book.
  ACH_BOOKS_10 = 1u << 5,     ///< Finished 10 books.
  ACH_BOOKS_25 = 1u << 6,     ///< Finished 25 books.
  ACH_BOOKS_50 = 1u << 7,     ///< Finished 50 books.
  ACH_PAGES_1K = 1u << 8,     ///< Turned 1,000 pages (lifetime).
  ACH_PAGES_10K = 1u << 9,    ///< Turned 10,000 pages (lifetime).
  ACH_PAGES_100K = 1u << 10,  ///< Turned 100,000 pages (lifetime).
  ACH_HOURS_10 = 1u << 11,    ///< Read for 10 hours (lifetime).
  ACH_HOURS_50 = 1u << 12,    ///< Read for 50 hours (lifetime).
  ACH_HOURS_100 = 1u << 13,   ///< Read for 100 hours (lifetime).
  ACH_EARLY_BIRD = 1u << 14,  ///< Ended a reading session between 04:00 and 08:00.
  ACH_NIGHT_OWL = 1u << 15,   ///< Ended a reading session between 22:00 and 03:00.
};

struct SpeedSample {
  uint16_t pagesPerHour;
  uint16_t minutesInSession;
};
static_assert(sizeof(SpeedSample) == 4, "SpeedSample layout changed -- bump STATS_FILE_VERSION");

struct BookStatEntry {
  char cacheKey[64];
  char title[64];
  char author[64];
  char bookPath[128];
  char thumbBmpPath[128];
  uint32_t totalReadingMs;
  uint32_t sessionCount;
  uint32_t totalPagesRead;
  uint32_t lastSessionMs;
  uint8_t progressPercent;
  uint8_t speedHead;
  uint8_t speedCount;
  uint8_t _pad0;
  SpeedSample speedSamples[4];
  uint16_t lastReadDay;
  uint16_t _pad1;
};
static_assert(sizeof(BookStatEntry) == 488, "BookStatEntry layout changed -- bump STATS_FILE_VERSION");
static_assert(offsetof(BookStatEntry, progressPercent) == 464, "v6 byte-prefix broken -- breaks migration");

// Resident per-book summary. Carries exactly the fields the stats list needs to
// count, filter and sort every book, so those whole-library sweeps never read
// the SD card; only the handful of rows actually drawn fault in the full
// 488-byte entry. 16 bytes here versus 488 is what lets the book list grow
// without a cap on a device with no PSRAM.
struct BookStatIndexEntry {
  uint32_t cacheKeyHash;
  uint32_t totalReadingMs;
  uint16_t diskSlot;
  uint16_t lastReadDay;
  uint8_t progressPercent;
  uint8_t _pad[3];
};
static_assert(sizeof(BookStatIndexEntry) == 16, "BookStatIndexEntry must stay compact -- it scales with library size");

namespace stats {

// FNV-1a over the cacheKey. Collisions are resolved by comparing the full
// cacheKey from the on-disk entry, so this only needs to be cheap and spread.
inline uint32_t hashCacheKey(const char* cacheKey) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < 64 && cacheKey[i] != '\0'; ++i) {
    h ^= static_cast<uint8_t>(cacheKey[i]);
    h *= 16777619u;
  }
  return h;
}

}  // namespace stats

struct GlobalStats {
  uint8_t version;
  uint8_t sessionRingHead;
  uint8_t sessionRingCount;
  uint8_t bookCount;
  uint16_t totalBooksFinished;
  uint8_t goalType;
  uint8_t petStage;
  uint32_t totalReadingMs;
  uint32_t totalSessionCount;
  uint32_t sessionRing[STATS_SESSION_RING_SIZE];
  uint16_t dayRingStartDay;
  uint16_t dayRingHead;
  uint16_t currentStreakDays;
  uint16_t longestStreakDays;
  uint16_t lastReadDay;
  uint16_t firstEverDay;
  uint16_t goalTarget;
  uint16_t goalAchievedDays;
  uint16_t bestDayMinutes;
  uint16_t lifetimeActiveDays;
  uint32_t achievementBits;
  uint32_t totalPagesLifetime;
  uint16_t petXp;
  uint8_t petHunger;
  uint8_t petHappiness;
  uint16_t dayMinutes[STATS_DAY_RING_SIZE];
  uint16_t lastSyncedDay;
  // Epoch (seconds) of the last reading session recorded with a valid clock.
  // Baseline timestamp for the pet's hourly stat decay; 0 when never set (the
  // X4 has no RTC, so this is only stamped once time() is NTP-synced).
  uint32_t petLastReadEpoch;
  // Authoritative book count since v9. `bookCount` above is a uint8_t that
  // saturates at 255, so it can no longer express the real total; it is kept
  // only so the v4..v8 migration prefixes stay byte-identical.
  uint32_t bookCountTotal;
};
static_assert(sizeof(GlobalStats) == 816, "GlobalStats layout changed -- bump STATS_FILE_VERSION");
static_assert(offsetof(GlobalStats, petLastReadEpoch) == 808,
              "petLastReadEpoch must append at the v7 tail for migration");
static_assert(offsetof(GlobalStats, bookCountTotal) == 812, "bookCountTotal must append at the v8 tail for migration");
static_assert(offsetof(GlobalStats, totalReadingMs) == 8, "v6 byte-prefix broken -- breaks migration");
static_assert(offsetof(GlobalStats, sessionRing) == 16, "v6 byte-prefix broken -- breaks migration");
static_assert(offsetof(GlobalStats, dayMinutes) == 76, "day-ring offset moved -- update migration");

namespace stats {

inline uint16_t saturatingAddU16(uint16_t a, uint16_t b) {
  const uint32_t s = static_cast<uint32_t>(a) + b;
  return s > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu) : static_cast<uint16_t>(s);
}

inline bool epochValid(int64_t epochSeconds) { return epochSeconds >= 1700000000; }

inline int utcOffsetSeconds(uint8_t clockUtcOffsetQ) { return (static_cast<int>(clockUtcOffsetQ) - 48) * 900; }

inline uint16_t dayNumber(int64_t epochSeconds, int offsetSeconds) {
  const int64_t local = epochSeconds + offsetSeconds;
  if (local < 0) return 0;
  return static_cast<uint16_t>(local / 86400);
}

struct CivilDate {
  int year;
  int month;
  int day;
};

inline CivilDate civilFromDays(int z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{y + static_cast<int>(m <= 2), static_cast<int>(m), static_cast<int>(d)};
}

inline int daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

inline int weekdayMon(int z) { return (z % 7 + 3) % 7; }

inline int daysInMonth(int y, int m) {
  static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return leap ? 29 : 28;
  }
  return dim[(m - 1) % 12];
}

inline uint16_t minutesOnDay(const GlobalStats& g, uint16_t day) {
  if (g.lastReadDay == 0 || day > g.lastReadDay) return 0;
  const uint16_t back = static_cast<uint16_t>(g.lastReadDay - day);
  if (back >= STATS_DAY_RING_SIZE) return 0;
  int idx = static_cast<int>(g.dayRingHead) - static_cast<int>(back);
  idx %= STATS_DAY_RING_SIZE;
  if (idx < 0) idx += STATS_DAY_RING_SIZE;
  return g.dayMinutes[idx];
}

inline bool streakAlive(const GlobalStats& g, uint16_t today) {
  if (g.lastReadDay == 0) return false;
  if (today <= g.lastReadDay) return true;
  return static_cast<uint16_t>(today - g.lastReadDay) <= 1;
}

inline uint32_t sumLastNDays(const GlobalStats& g, uint16_t today, uint16_t n) {
  uint32_t total = 0;
  for (uint16_t i = 0; i < n && i < STATS_DAY_RING_SIZE; ++i) {
    if (today < i) break;
    total += minutesOnDay(g, static_cast<uint16_t>(today - i));
  }
  return total;
}

inline uint32_t evaluateAchievements(GlobalStats& g, int sessionEndHour) {
  uint32_t bits = 0;
  if (g.longestStreakDays >= 3) bits |= ACH_STREAK_3;
  if (g.longestStreakDays >= 7) bits |= ACH_STREAK_7;
  if (g.longestStreakDays >= 30) bits |= ACH_STREAK_30;
  if (g.longestStreakDays >= 100) bits |= ACH_STREAK_100;
  if (g.totalBooksFinished >= 1) bits |= ACH_BOOKS_1;
  if (g.totalBooksFinished >= 10) bits |= ACH_BOOKS_10;
  if (g.totalBooksFinished >= 25) bits |= ACH_BOOKS_25;
  if (g.totalBooksFinished >= 50) bits |= ACH_BOOKS_50;
  if (g.totalPagesLifetime >= 1000) bits |= ACH_PAGES_1K;
  if (g.totalPagesLifetime >= 10000) bits |= ACH_PAGES_10K;
  if (g.totalPagesLifetime >= 100000) bits |= ACH_PAGES_100K;
  if (g.totalReadingMs >= 10u * 3600000u) bits |= ACH_HOURS_10;
  if (g.totalReadingMs >= 50u * 3600000u) bits |= ACH_HOURS_50;
  if (g.totalReadingMs >= 100u * 3600000u) bits |= ACH_HOURS_100;
  if (sessionEndHour >= 4 && sessionEndHour < 8) bits |= ACH_EARLY_BIRD;
  if (sessionEndHour >= 22 || (sessionEndHour >= 0 && sessionEndHour < 3)) bits |= ACH_NIGHT_OWL;
  const uint32_t newly = bits & ~g.achievementBits;
  g.achievementBits |= bits;
  return newly;
}

inline uint8_t petClampUp(uint8_t value, int delta) {
  const int r = static_cast<int>(value) + delta;
  return r > 100 ? 100 : static_cast<uint8_t>(r);
}

inline constexpr uint16_t kPetStageThresholds[] = {300, 700, 1300, 2200, 3500, 5500, 8500, 13000, 20000, 30000};
inline constexpr uint8_t kPetStageMax = sizeof(kPetStageThresholds) / sizeof(kPetStageThresholds[0]);
inline constexpr uint8_t kPetStageCount = kPetStageMax + 1;
inline constexpr uint16_t kPetMaxXp = kPetStageThresholds[kPetStageMax - 1];

inline uint8_t petStageForXp(uint16_t xp) {
  uint8_t stage = 0;
  for (uint8_t i = 0; i < kPetStageMax; ++i) {
    if (xp >= kPetStageThresholds[i]) stage = static_cast<uint8_t>(i + 1);
  }
  return stage;
}

inline uint16_t petXpFloorForStage(uint8_t stage) {
  if (stage == 0) return 0;
  if (stage > kPetStageMax) stage = kPetStageMax;
  return kPetStageThresholds[stage - 1];
}

inline uint16_t petXpNextForStage(uint8_t stage) { return stage >= kPetStageMax ? 0 : kPetStageThresholds[stage]; }

inline uint8_t petLevelForXp(uint16_t xp) {
  if (xp >= kPetMaxXp) return 100;
  return static_cast<uint8_t>(1 + (static_cast<uint32_t>(xp) * 99u) / kPetMaxXp);
}

// Deterministic hash so a given calendar hour always yields the same decay --
// the pet screen recomputes decay on every render and must not flicker.
inline uint32_t petHash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Random-feeling but reproducible 2..4 percent for a given hour index and stat
// (0 = hunger, 1 = happiness).
inline int petHourlyDecayPct(uint32_t hourIndex, uint8_t statSel) {
  return 2 + static_cast<int>(petHash32(hourIndex * 2u + statSel) % 3u);
}

// Decays a 0..100 stat across the half-open hour window [fromHour, toHour),
// removing 2-4% of the remaining value each hour. Integer-only (the ESP32-C3
// has no FPU); the ceil on each decrement keeps a low stat moving to 0.
inline uint8_t petDecayStat(uint8_t baseline, uint32_t fromHour, uint32_t toHour, uint8_t statSel) {
  if (toHour <= fromHour) return baseline;
  uint32_t hours = toHour - fromHour;
  if (hours > 720) hours = 720;  // ~30 days at >=2%/h already floors the stat
  int v = baseline;
  for (uint32_t i = 0; i < hours && v > 0; ++i) {
    v -= (v * petHourlyDecayPct(fromHour + i, statSel) + 99) / 100;
    if (v < 0) v = 0;
  }
  return static_cast<uint8_t>(v);
}

// Hunger/happiness after decaying the stored baseline for the hours elapsed
// since the last read. No-op without a valid clock or baseline (no RTC).
inline void petEffectiveStats(const GlobalStats& g, int64_t nowEpoch, uint8_t& hunger, uint8_t& happiness) {
  hunger = g.petHunger;
  happiness = g.petHappiness;
  if (!epochValid(nowEpoch) || g.petLastReadEpoch == 0 || nowEpoch <= static_cast<int64_t>(g.petLastReadEpoch)) return;
  const uint32_t fromHour = g.petLastReadEpoch / 3600u;
  const uint32_t toHour = static_cast<uint32_t>(nowEpoch / 3600);
  hunger = petDecayStat(g.petHunger, fromHour, toHour, 0);
  happiness = petDecayStat(g.petHappiness, fromHour, toHour, 1);
}

inline uint16_t petXpForPages(uint32_t pages, uint32_t seed) {
  if (pages == 0) return 0;
  const uint32_t rateMilliPerPage = 500u + (petHash32(seed) % 1001u);
  const uint64_t xp = (static_cast<uint64_t>(pages) * rateMilliPerPage + 500u) / 1000u;
  return xp > 0xFFFFu ? static_cast<uint16_t>(0xFFFFu) : static_cast<uint16_t>(xp);
}

inline void updatePet(GlobalStats& g, int64_t nowEpoch, uint32_t sessionPagesTurned) {
  // Decay for the hours since the last read, then refill for this session and
  // re-stamp the baseline. Decay/stamp only with a valid clock; the refill and
  // XP always apply so reading still rewards the pet when time() is unsynced.
  if (epochValid(nowEpoch)) {
    uint8_t hunger, happiness;
    petEffectiveStats(g, nowEpoch, hunger, happiness);
    g.petHunger = hunger;
    g.petHappiness = happiness;
    g.petLastReadEpoch = static_cast<uint32_t>(nowEpoch);
  }
  g.petHunger = petClampUp(g.petHunger, 20);
  g.petHappiness = petClampUp(g.petHappiness, 12);
  const uint32_t xpSeed = static_cast<uint32_t>(nowEpoch) + g.totalSessionCount * 7919u + sessionPagesTurned;
  g.petXp = saturatingAddU16(g.petXp, petXpForPages(sessionPagesTurned, xpSeed));
  g.petStage = petStageForXp(g.petXp);
}

inline void maybeCountGoalCrossing(GlobalStats& g, uint16_t prevMinutes, uint16_t newMinutes) {
  if (g.goalTarget == 0) return;
  if (newMinutes >= g.goalTarget && prevMinutes < g.goalTarget && g.goalAchievedDays < 0xFFFF) {
    g.goalAchievedDays++;
  }
}

inline void recordReadingDay(GlobalStats& g, uint16_t day, uint16_t minutes) {
  if (minutes == 0) return;

  if (g.lastReadDay == 0) {
    g.dayRingHead = 0;
    g.dayMinutes[0] = minutes;
    g.lastReadDay = day;
    g.firstEverDay = day;
    g.dayRingStartDay = day;
    g.currentStreakDays = 1;
    g.longestStreakDays = 1;
    g.lifetimeActiveDays = 1;
    g.bestDayMinutes = minutes;
    maybeCountGoalCrossing(g, 0, minutes);
    return;
  }

  if (day == g.lastReadDay) {
    const uint16_t prev = g.dayMinutes[g.dayRingHead];
    const uint16_t now = saturatingAddU16(prev, minutes);
    g.dayMinutes[g.dayRingHead] = now;
    if (now > g.bestDayMinutes) g.bestDayMinutes = now;
    maybeCountGoalCrossing(g, prev, now);
    return;
  }

  if (day < g.lastReadDay) {
    const uint16_t back = static_cast<uint16_t>(g.lastReadDay - day);
    if (back < STATS_DAY_RING_SIZE) {
      int idx = static_cast<int>(g.dayRingHead) - static_cast<int>(back);
      idx %= STATS_DAY_RING_SIZE;
      if (idx < 0) idx += STATS_DAY_RING_SIZE;
      g.dayMinutes[idx] = saturatingAddU16(g.dayMinutes[idx], minutes);
    }
    return;
  }

  const uint16_t delta = static_cast<uint16_t>(day - g.lastReadDay);
  if (delta >= STATS_DAY_RING_SIZE) {
    memset(g.dayMinutes, 0, sizeof(g.dayMinutes));
    g.dayRingHead = 0;
    g.dayMinutes[0] = minutes;
  } else {
    for (uint16_t k = 0; k < delta; ++k) {
      g.dayRingHead = static_cast<uint16_t>((g.dayRingHead + 1) % STATS_DAY_RING_SIZE);
      g.dayMinutes[g.dayRingHead] = 0;
    }
    g.dayMinutes[g.dayRingHead] = minutes;
  }

  if (delta == 1) {
    if (g.currentStreakDays < 0xFFFF) g.currentStreakDays++;
  } else {
    g.currentStreakDays = 1;
  }
  if (g.currentStreakDays > g.longestStreakDays) g.longestStreakDays = g.currentStreakDays;

  g.lastReadDay = day;
  if (g.lifetimeActiveDays < 0xFFFF) g.lifetimeActiveDays++;
  if (minutes > g.bestDayMinutes) g.bestDayMinutes = minutes;
  if (day > STATS_DAY_RING_SIZE - 1) {
    const uint16_t oldest = static_cast<uint16_t>(day - (STATS_DAY_RING_SIZE - 1));
    g.dayRingStartDay = (oldest < g.firstEverDay) ? g.firstEverDay : oldest;
  } else {
    g.dayRingStartDay = g.firstEverDay;
  }
  maybeCountGoalCrossing(g, 0, minutes);
}

}  // namespace stats
