#include "src/activities/reader/ProgressFormat.h"

#include <gtest/gtest.h>

using EpubReaderUtils::parseProgress;
using EpubReaderUtils::serializeProgress;

TEST(ProgressFormatTest, RejectsInvalidLengths) {
  uint8_t data[10] = {};
  EXPECT_FALSE(parseProgress(data, 0).has_value());
  EXPECT_FALSE(parseProgress(data, 5).has_value());
  EXPECT_FALSE(parseProgress(data, 7).has_value());
  EXPECT_FALSE(parseProgress(data, 11).has_value());
}

TEST(ProgressFormatTest, ParsesLegacyFourByteFormat) {
  const uint8_t data[4] = {0x34, 0x12, 0x78, 0x56};  // spine=0x1234, page=0x5678
  const auto progress = parseProgress(data, 4);
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->spineIndex, 0x1234);
  EXPECT_EQ(progress->pageNumber, 0x5678);
  EXPECT_EQ(progress->pageCount, 0);
  EXPECT_FALSE(progress->visibleTextOffset.has_value());
}

TEST(ProgressFormatTest, ParsesSixByteFormatWithPageCount) {
  const uint8_t data[6] = {0x01, 0x00, 0x02, 0x00, 0x64, 0x00};  // spine=1, page=2, count=100
  const auto progress = parseProgress(data, 6);
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->spineIndex, 1);
  EXPECT_EQ(progress->pageNumber, 2);
  EXPECT_EQ(progress->pageCount, 100);
  EXPECT_FALSE(progress->visibleTextOffset.has_value());
}

TEST(ProgressFormatTest, ParsesTenByteFormatWithVisibleTextOffset) {
  // spine=1, page=2, count=100, offset=0x12345678
  const uint8_t data[10] = {0x01, 0x00, 0x02, 0x00, 0x64, 0x00, 0x78, 0x56, 0x34, 0x12};
  const auto progress = parseProgress(data, 10);
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->spineIndex, 1);
  EXPECT_EQ(progress->pageNumber, 2);
  EXPECT_EQ(progress->pageCount, 100);
  ASSERT_TRUE(progress->visibleTextOffset.has_value());
  EXPECT_EQ(*progress->visibleTextOffset, 0x12345678u);
}

TEST(ProgressFormatTest, RoundTripsMaxUint16Fields) {
  const uint8_t data[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const auto progress = parseProgress(data, 6);
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->spineIndex, 0xFFFF);
  EXPECT_EQ(progress->pageNumber, 0xFFFF);
  EXPECT_EQ(progress->pageCount, 0xFFFF);
}

TEST(ProgressFormatTest, RoundTripsMaxUint32Offset) {
  const uint8_t data[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  const auto progress = parseProgress(data, 10);
  ASSERT_TRUE(progress.has_value());
  ASSERT_TRUE(progress->visibleTextOffset.has_value());
  EXPECT_EQ(*progress->visibleTextOffset, 0xFFFFFFFFu);
}

TEST(ProgressFormatTest, SerializeWithoutOffsetProducesSixBytes) {
  const auto serialized = serializeProgress(1, 2, 100);
  EXPECT_EQ(serialized.len, 6u);
  const auto progress = parseProgress(serialized.data, static_cast<int>(serialized.len));
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->spineIndex, 1);
  EXPECT_EQ(progress->pageNumber, 2);
  EXPECT_EQ(progress->pageCount, 100);
  EXPECT_FALSE(progress->visibleTextOffset.has_value());
}

TEST(ProgressFormatTest, SerializeWithOffsetProducesTenBytesAndRoundTrips) {
  const auto serialized = serializeProgress(1234, 56, 789, 0xDEADBEEF);
  EXPECT_EQ(serialized.len, 10u);
  const auto progress = parseProgress(serialized.data, static_cast<int>(serialized.len));
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->spineIndex, 1234);
  EXPECT_EQ(progress->pageNumber, 56);
  EXPECT_EQ(progress->pageCount, 789);
  ASSERT_TRUE(progress->visibleTextOffset.has_value());
  EXPECT_EQ(*progress->visibleTextOffset, 0xDEADBEEFu);
}
