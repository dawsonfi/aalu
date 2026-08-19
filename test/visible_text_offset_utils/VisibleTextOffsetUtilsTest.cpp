#include <gtest/gtest.h>

#include "lib/Epub/Epub/VisibleTextOffsetUtils.h"

using VisibleTextOffsetUtils::findPageForOffset;

TEST(VisibleTextOffsetUtilsTest, EmptyInputReturnsNullopt) {
  const std::vector<uint32_t> offsets;
  EXPECT_FALSE(findPageForOffset(offsets, 0).has_value());
}

TEST(VisibleTextOffsetUtilsTest, TargetBeforeFirstPageReturnsNullopt) {
  const std::vector<uint32_t> offsets = {10, 20, 30};
  EXPECT_FALSE(findPageForOffset(offsets, 5).has_value());
}

TEST(VisibleTextOffsetUtilsTest, FindsExactMatch) {
  const std::vector<uint32_t> offsets = {0, 10, 20, 30};
  const auto page = findPageForOffset(offsets, 20);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, 2);
}

TEST(VisibleTextOffsetUtilsTest, FindsLastPageAtOrBelowTarget) {
  const std::vector<uint32_t> offsets = {0, 10, 20, 30};
  const auto page = findPageForOffset(offsets, 25);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, 2);  // page 2 starts at 20, page 3 starts at 30 (> 25)
}

TEST(VisibleTextOffsetUtilsTest, TargetPastLastPageReturnsLastPage) {
  const std::vector<uint32_t> offsets = {0, 10, 20, 30};
  const auto page = findPageForOffset(offsets, 1000);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, 3);
}

TEST(VisibleTextOffsetUtilsTest, DefaultPrefersLastPageInATieRun) {
  // Pages 2 and 3 are zero-width (e.g. image-only) and both start at offset 20.
  const std::vector<uint32_t> offsets = {0, 10, 20, 20, 20, 30};
  const auto page = findPageForOffset(offsets, 20, /*preferFirstAtOffset=*/false);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, 4);
}

TEST(VisibleTextOffsetUtilsTest, PreferFirstAtOffsetStopsAtFirstOfATieRun) {
  const std::vector<uint32_t> offsets = {0, 10, 20, 20, 20, 30};
  const auto page = findPageForOffset(offsets, 20, /*preferFirstAtOffset=*/true);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, 2);
}

TEST(VisibleTextOffsetUtilsTest, SinglePageAlwaysMatches) {
  const std::vector<uint32_t> offsets = {0};
  const auto page = findPageForOffset(offsets, 0);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, 0);
}
