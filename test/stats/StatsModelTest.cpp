#include <gtest/gtest.h>

#include <cstring>

#include "stats/StatsTypes.h"

// stats.bin v9 binary layout is load-bearing: any change must bump the file
// version + migration. These guard the exact on-disk sizes.
TEST(StatsTypes, V9StructSizes) {
  EXPECT_EQ(sizeof(SpeedSample), 4u);
  EXPECT_EQ(sizeof(BookStatEntry), 488u);
  EXPECT_EQ(sizeof(GlobalStats), 816u);
}

TEST(StatsTypes, V9PrefixMatchesPriorVersions) {
  // Older layouts are strict prefixes so migration is a pure tail-append.
  // petLastReadEpoch sits at the v7 size (808); bookCountTotal at the v8 size.
  EXPECT_EQ(offsetof(GlobalStats, totalReadingMs), 8u);
  EXPECT_EQ(offsetof(GlobalStats, sessionRing), 16u);
  EXPECT_EQ(offsetof(GlobalStats, dayMinutes), 76u);
  EXPECT_EQ(offsetof(GlobalStats, petLastReadEpoch), 808u);
  EXPECT_EQ(offsetof(GlobalStats, bookCountTotal), 812u);
  EXPECT_EQ(offsetof(BookStatEntry, progressPercent), 464u);
}

// The resident index is what removes the old 9-book cap: it must stay small,
// because its size is multiplied by the whole library.
TEST(StatsTypes, IndexEntryStaysCompact) {
  EXPECT_EQ(sizeof(BookStatIndexEntry), 16u);
  // A 1000-book library must cost far less than holding full entries would.
  EXPECT_LT(1000u * sizeof(BookStatIndexEntry), 1000u * sizeof(BookStatEntry) / 25u);
}

TEST(StatsTypes, EntryOffsetsAreDenseAndSeekable) {
  // Row i must be reachable by arithmetic alone -- that is what makes paging
  // in a single row an O(1) seek instead of a scan.
  const uint32_t slot0 = sizeof(GlobalStats);
  const uint32_t slot7 = sizeof(GlobalStats) + 7u * sizeof(BookStatEntry);
  EXPECT_EQ(slot7 - slot0, 7u * 488u);
}

TEST(StatsHash, SameKeyHashesEqualAndDiffersAcrossKeys) {
  char a[64] = "epub_12345";
  char b[64] = "epub_12345";
  char c[64] = "epub_54321";
  EXPECT_EQ(stats::hashCacheKey(a), stats::hashCacheKey(b));
  EXPECT_NE(stats::hashCacheKey(a), stats::hashCacheKey(c));
}

TEST(StatsHash, EmptyKeyIsStable) {
  char empty[64] = "";
  EXPECT_EQ(stats::hashCacheKey(empty), stats::hashCacheKey(empty));
}

TEST(StatsHash, StopsAtNulAndIgnoresTrailingGarbage) {
  // Entries are memset then strncpy'd, but a short key must not pick up bytes
  // past the terminator or a reused slot would hash differently.
  char clean[64] = {};
  char dirty[64] = {};
  memcpy(clean, "epub_9", 6);
  memcpy(dirty, "epub_9", 6);
  memset(dirty + 7, 0x7F, 40);
  EXPECT_EQ(stats::hashCacheKey(clean), stats::hashCacheKey(dirty));
}

TEST(StatsStreak, FirstReadingStartsStreakAtOne) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 30);
  EXPECT_EQ(g.currentStreakDays, 1);
  EXPECT_EQ(g.longestStreakDays, 1);
  EXPECT_EQ(g.lastReadDay, 20000);
  EXPECT_EQ(g.firstEverDay, 20000);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 30);
}

TEST(StatsStreak, ConsecutiveDaysExtendStreak) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20001, 20);
  EXPECT_EQ(g.currentStreakDays, 2);
  EXPECT_EQ(g.longestStreakDays, 2);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 10);
  EXPECT_EQ(stats::minutesOnDay(g, 20001), 20);
}

TEST(StatsStreak, SameDayAccumulatesMinutesNoStreakChange) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20000, 25);
  EXPECT_EQ(g.currentStreakDays, 1);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 35);
}

TEST(StatsStreak, GapResetsCurrentButKeepsLongest) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20001, 10);
  stats::recordReadingDay(g, 20002, 10);  // streak now 3
  stats::recordReadingDay(g, 20005, 10);  // 3-day gap breaks it
  EXPECT_EQ(g.currentStreakDays, 1);
  EXPECT_EQ(g.longestStreakDays, 3);
  EXPECT_EQ(stats::minutesOnDay(g, 20003), 0);  // skipped days read as blank
  EXPECT_EQ(stats::minutesOnDay(g, 20004), 0);
  EXPECT_EQ(stats::minutesOnDay(g, 20005), 10);
}

TEST(StatsStreak, AliveOnlyWithinOneDay) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  EXPECT_TRUE(stats::streakAlive(g, 20000));
  EXPECT_TRUE(stats::streakAlive(g, 20001));
  EXPECT_FALSE(stats::streakAlive(g, 20002));
}

TEST(StatsDayRing, SaturatesMinutesAtU16Max) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 65000);
  stats::recordReadingDay(g, 20000, 2000);
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 65535);
}

TEST(StatsDayRing, JumpBeyondWindowZeroesHistory) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 50);
  stats::recordReadingDay(g, 20400, 10);  // > 365-day jump
  EXPECT_EQ(stats::minutesOnDay(g, 20000), 0);
  EXPECT_EQ(stats::minutesOnDay(g, 20400), 10);
  EXPECT_EQ(g.currentStreakDays, 1);
}

TEST(StatsDayRing, BackwardClockDoesNotZeroValidHistory) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20001, 40);
  stats::recordReadingDay(g, 20000, 5);  // clock corrected backward
  EXPECT_EQ(stats::minutesOnDay(g, 20001), 40);
  EXPECT_EQ(g.lastReadDay, 20001);  // anchor never moves backward
}

TEST(StatsAggregates, TracksBestDayAndActiveDays) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 30);
  stats::recordReadingDay(g, 20001, 90);
  stats::recordReadingDay(g, 20002, 15);
  EXPECT_EQ(g.bestDayMinutes, 90);
  EXPECT_EQ(g.lifetimeActiveDays, 3);
}

TEST(StatsGoal, CountsDistinctGoalMetDays) {
  GlobalStats g{};
  g.goalTarget = 30;
  stats::recordReadingDay(g, 20000, 30);  // meets exactly
  stats::recordReadingDay(g, 20001, 10);  // below
  stats::recordReadingDay(g, 20002, 45);  // meets
  EXPECT_EQ(g.goalAchievedDays, 2);
}

TEST(StatsGoal, SameDayCrossingCountsOnce) {
  GlobalStats g{};
  g.goalTarget = 30;
  stats::recordReadingDay(g, 20000, 20);  // below
  stats::recordReadingDay(g, 20000, 20);  // now 40, crosses the goal
  stats::recordReadingDay(g, 20000, 10);  // still above, must not double-count
  EXPECT_EQ(g.goalAchievedDays, 1);
}

TEST(StatsClock, RejectsUnsyncedEpoch) {
  EXPECT_FALSE(stats::epochValid(0));
  EXPECT_FALSE(stats::epochValid(1699999999));
  EXPECT_TRUE(stats::epochValid(1700000000));
}

TEST(StatsClock, OffsetSecondsFromQuarterHourSetting) {
  EXPECT_EQ(stats::utcOffsetSeconds(48), 0);      // UTC+0
  EXPECT_EQ(stats::utcOffsetSeconds(52), 3600);   // +4 quarter-hours = +1h
  EXPECT_EQ(stats::utcOffsetSeconds(44), -3600);  // -1h
}

TEST(StatsClock, DerivesLocalDayNumberFromEpoch) {
  // 2024-01-02 00:00:00 UTC = epoch 1704153600 = day 19724.
  EXPECT_EQ(stats::dayNumber(1704153600, 0), 19724);
  // 23:00 UTC on day 19723; a +2h offset rolls it into local day 19724.
  EXPECT_EQ(stats::dayNumber(1704153600 - 3600, 0), 19723);
  EXPECT_EQ(stats::dayNumber(1704153600 - 3600, 7200), 19724);
}

TEST(StatsPeriods, SumsLastNDaysInclusiveOfToday) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);
  stats::recordReadingDay(g, 20001, 20);
  stats::recordReadingDay(g, 20002, 30);
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 1), 30u);
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 2), 50u);
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 7), 60u);
}

TEST(StatsPeriods, CountsUnreadDaysAsZeroWithinWindow) {
  GlobalStats g{};
  stats::recordReadingDay(g, 20000, 10);             // last read two days before "today"
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 1), 0u);   // nothing today
  EXPECT_EQ(stats::sumLastNDays(g, 20002, 7), 10u);  // the read 2 days ago still counts
}

TEST(StatsAchievements, UnlocksFromLifetimeAggregates) {
  GlobalStats g{};
  g.totalReadingMs = 10u * 3600000u;  // 10 hours
  g.totalBooksFinished = 1;
  g.totalPagesLifetime = 1000;
  g.longestStreakDays = 7;
  const uint32_t newly = stats::evaluateAchievements(g, -1);
  EXPECT_TRUE(newly & ACH_HOURS_10);
  EXPECT_TRUE(newly & ACH_BOOKS_1);
  EXPECT_TRUE(newly & ACH_PAGES_1K);
  EXPECT_TRUE(newly & ACH_STREAK_7);
  EXPECT_FALSE(newly & ACH_HOURS_50);
  EXPECT_TRUE(g.achievementBits & ACH_HOURS_10);
}

TEST(StatsAchievements, IsIdempotent) {
  GlobalStats g{};
  g.longestStreakDays = 30;
  stats::evaluateAchievements(g, -1);
  EXPECT_EQ(stats::evaluateAchievements(g, -1), 0u);  // nothing new the second pass
  EXPECT_TRUE(g.achievementBits & ACH_STREAK_30);
  EXPECT_TRUE(g.achievementBits & ACH_STREAK_7);  // lower tiers also set
}

TEST(StatsAchievements, TimeOfDayBadgesNeedTheRightHour) {
  GlobalStats g{};
  EXPECT_FALSE(stats::evaluateAchievements(g, 12) & ACH_EARLY_BIRD);
  EXPECT_TRUE(stats::evaluateAchievements(g, 5) & ACH_EARLY_BIRD);
  EXPECT_TRUE(stats::evaluateAchievements(g, 23) & ACH_NIGHT_OWL);
  EXPECT_FALSE(stats::evaluateAchievements(g, -1) & ACH_NIGHT_OWL);  // unknown hour
}

constexpr int64_t kEpoch = 1750000000;  // a valid post-NTP epoch (year 2025)

TEST(StatsPet, ReadingRefillsAndStampsBaseline) {
  GlobalStats g{};
  stats::updatePet(g, kEpoch, 10);  // first feed with a synced clock
  EXPECT_EQ(g.petHunger, 20);
  EXPECT_EQ(g.petHappiness, 12);
  EXPECT_EQ(g.petLastReadEpoch, static_cast<uint32_t>(kEpoch));
}

TEST(StatsPet, HourlyDecayIsTwoToFourPercent) {
  EXPECT_EQ(stats::petDecayStat(100, 5000, 5000, 0), 100);  // no hours elapsed
  const uint8_t oneHour = stats::petDecayStat(100, 5000, 5001, 0);
  EXPECT_GE(oneHour, 96);  // 100 minus 2..4
  EXPECT_LE(oneHour, 98);
  EXPECT_LT(stats::petDecayStat(100, 5000, 5020, 0), oneHour);  // more hours decay further
  EXPECT_EQ(stats::petDecayStat(100, 0, 720, 0), 0);            // ~30 days unread -> starved
  // Deterministic so the pet screen recomputes identically across renders.
  EXPECT_EQ(stats::petDecayStat(100, 5000, 5037, 1), stats::petDecayStat(100, 5000, 5037, 1));
}

TEST(StatsPet, EffectiveStatsDecayBetweenReads) {
  GlobalStats g{};
  g.petHunger = 90;
  g.petHappiness = 90;
  g.petLastReadEpoch = static_cast<uint32_t>(kEpoch);
  uint8_t h, hp;
  stats::petEffectiveStats(g, kEpoch + 12 * 3600, h, hp);  // 12 hours later
  EXPECT_LT(h, 90);
  EXPECT_LT(hp, 90);
  // Without a valid clock (no RTC yet) the stored baseline shows unchanged.
  stats::petEffectiveStats(g, 1000, h, hp);
  EXPECT_EQ(h, 90);
  EXPECT_EQ(hp, 90);
}

TEST(StatsPet, NoClockRefillsButDoesNotDecayOrStamp) {
  GlobalStats g{};
  g.petHunger = 80;
  g.petHappiness = 80;
  g.petLastReadEpoch = static_cast<uint32_t>(kEpoch);
  stats::updatePet(g, 1000, 10);  // invalid epoch: reading still rewards, no decay
  EXPECT_EQ(g.petHunger, 100);
  EXPECT_EQ(g.petHappiness, 92);
  EXPECT_GT(g.petXp, 0);                                         // page-based XP applies without a synced clock
  EXPECT_EQ(g.petLastReadEpoch, static_cast<uint32_t>(kEpoch));  // baseline untouched
}

TEST(StatsPet, FeedingCapsAtHundred) {
  GlobalStats g{};
  g.petHunger = 95;
  g.petHappiness = 95;
  g.petLastReadEpoch = static_cast<uint32_t>(kEpoch);
  stats::updatePet(g, kEpoch, 10);  // same instant: no decay, +20/+12 capped
  EXPECT_EQ(g.petHunger, 100);
  EXPECT_EQ(g.petHappiness, 100);
}

TEST(StatsPet, StageGrowsWithXp) {
  EXPECT_EQ(stats::petStageForXp(0), 0);
  EXPECT_EQ(stats::petStageForXp(299), 0);
  EXPECT_EQ(stats::petStageForXp(300), 1);
  EXPECT_EQ(stats::petStageForXp(1300), 3);  // tiger cub
  EXPECT_EQ(stats::petStageForXp(5500), 6);  // dragon egg
  EXPECT_EQ(stats::petStageForXp(29999), 9);
  EXPECT_EQ(stats::petStageForXp(30000), 10);  // elder dragon (max)
  EXPECT_EQ(stats::petStageForXp(65535), stats::kPetStageMax);
}

TEST(StatsPet, XpBarBoundsMatchStages) {
  for (uint16_t xp : {uint16_t{0}, uint16_t{299}, uint16_t{300}, uint16_t{3499}, uint16_t{5500}, uint16_t{13000},
                      uint16_t{29999}, uint16_t{30000}, uint16_t{65535}}) {
    const uint8_t stage = stats::petStageForXp(xp);
    const uint16_t floor = stats::petXpFloorForStage(stage);
    const uint16_t next = stats::petXpNextForStage(stage);
    EXPECT_LE(floor, xp);
    if (stage >= stats::kPetStageMax) {
      EXPECT_EQ(next, 0);  // max stage has no next threshold
    } else {
      EXPECT_LT(xp, next);
      EXPECT_LT(floor, next);
    }
  }
  EXPECT_EQ(stats::petXpFloorForStage(0), 0);
  EXPECT_EQ(stats::petXpNextForStage(0), 300);
  EXPECT_EQ(stats::petXpFloorForStage(9), 20000);
  EXPECT_EQ(stats::petXpNextForStage(9), 30000);
  EXPECT_EQ(stats::petXpNextForStage(stats::kPetStageMax), 0);
}

TEST(StatsPet, XpPerPageStaysInHalfToOneAndHalfBand) {
  for (uint32_t seed = 0; seed < 256; ++seed) {
    const uint16_t xp = stats::petXpForPages(1000, seed);  // ~1 XP/page, rolled 0.5x..1.5x
    EXPECT_GE(xp, 500u);
    EXPECT_LE(xp, 1500u);
  }
  EXPECT_EQ(stats::petXpForPages(0, 42), 0u);                             // no pages, no XP
  EXPECT_EQ(stats::petXpForPages(250, 7), stats::petXpForPages(250, 7));  // reproducible
}

TEST(StatsPet, FeedingGrantsPageBasedXp) {
  GlobalStats g{};
  stats::updatePet(g, kEpoch, 1000);  // one ~1000-page sitting
  EXPECT_GE(g.petXp, 500);
  EXPECT_LE(g.petXp, 1500);
  EXPECT_EQ(g.petStage, stats::petStageForXp(g.petXp));
}

TEST(StatsPet, EnoughReadingMaxesTheStage) {
  GlobalStats g{};
  for (int session = 0; session < 100; ++session) {
    stats::updatePet(g, kEpoch + session * 86400, 1000);  // 100 sittings, ~1000 pages each
  }
  EXPECT_GE(g.petXp, 30000);  // >= 50000 even at the 0.5x floor
  EXPECT_EQ(g.petStage, stats::kPetStageMax);
}

TEST(StatsPet, LevelSpansOneToHundred) {
  EXPECT_EQ(stats::petLevelForXp(0), 1);
  EXPECT_EQ(stats::petLevelForXp(stats::kPetMaxXp), 100);
  EXPECT_EQ(stats::petLevelForXp(65535), 100);  // saturates, never exceeds 100
  EXPECT_GE(stats::petLevelForXp(15000), 49);
  EXPECT_LE(stats::petLevelForXp(15000), 51);
  uint8_t prev = 0;
  for (uint32_t xp = 0; xp <= 30000; xp += 250) {
    const uint8_t lv = stats::petLevelForXp(static_cast<uint16_t>(xp));
    EXPECT_GE(lv, prev);  // monotonic non-decreasing
    EXPECT_GE(lv, 1);
    EXPECT_LE(lv, 100);
    prev = lv;
  }
}

TEST(StatsCalendar, CivilFromDaysMatchesKnownDates) {
  const auto a = stats::civilFromDays(19724);  // 2024-01-02
  EXPECT_EQ(a.year, 2024);
  EXPECT_EQ(a.month, 1);
  EXPECT_EQ(a.day, 2);
  const auto b = stats::civilFromDays(0);  // 1970-01-01
  EXPECT_EQ(b.year, 1970);
  EXPECT_EQ(b.month, 1);
  EXPECT_EQ(b.day, 1);
}

TEST(StatsCalendar, DaysFromCivilRoundTrips) {
  EXPECT_EQ(stats::daysFromCivil(2024, 1, 2), 19724);
  EXPECT_EQ(stats::daysFromCivil(1970, 1, 1), 0);
}

TEST(StatsCalendar, WeekdayMonIsZeroBasedMonday) {
  EXPECT_EQ(stats::weekdayMon(19724), 1);  // 2024-01-02 was a Tuesday
}

TEST(StatsCalendar, DaysInMonthHandlesLeapYears) {
  EXPECT_EQ(stats::daysInMonth(2024, 2), 29);
  EXPECT_EQ(stats::daysInMonth(2023, 2), 28);
  EXPECT_EQ(stats::daysInMonth(2024, 4), 30);
  EXPECT_EQ(stats::daysInMonth(2024, 1), 31);
}
