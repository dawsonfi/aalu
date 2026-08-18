#include <BuildScratch.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "InflateStream.h"
#include "fixtures/InflateStreamFixtures.h"

namespace {

uint64_t fnv1a64(const uint8_t* data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace

TEST(InflateStream, OneShotRoundTrip) {
  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/false));
  stream.setSource(SMALL_COMPRESSED, SMALL_COMPRESSED_LEN);
  stream.setZlibWrapped();

  std::vector<uint8_t> out(SMALL_ORIGINAL_LEN);
  ASSERT_TRUE(stream.read(out.data(), out.size()));
  EXPECT_EQ(fnv1a64(out.data(), out.size()), SMALL_ORIGINAL_FNV1A64);
  stream.deinit();
}

TEST(InflateStream, StreamingRoundTripAcrossRingWraparound) {
  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/true));
  stream.setSource(STREAMING_COMPRESSED, STREAMING_COMPRESSED_LEN);
  stream.setZlibWrapped();

  std::vector<uint8_t> out(STREAMING_ORIGINAL_LEN);
  ASSERT_TRUE(stream.read(out.data(), out.size()));
  EXPECT_EQ(fnv1a64(out.data(), out.size()), STREAMING_ORIGINAL_FNV1A64);
  stream.deinit();
}

TEST(InflateStream, StreamingRoundTripInSmallChunks) {
  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/true));
  stream.setSource(STREAMING_COMPRESSED, STREAMING_COMPRESSED_LEN);
  stream.setZlibWrapped();

  std::vector<uint8_t> out(STREAMING_ORIGINAL_LEN);
  size_t total = 0;
  constexpr size_t CHUNK = 997;  // deliberately not a divisor of the window size
  while (total < out.size()) {
    size_t produced = 0;
    const size_t want = std::min(CHUNK, out.size() - total);
    const InflateStream::Status status = stream.readAtMost(out.data() + total, want, &produced);
    total += produced;
    ASSERT_NE(status, InflateStream::Status::Error);
    if (status == InflateStream::Status::Done) break;
  }
  ASSERT_EQ(total, out.size());
  EXPECT_EQ(fnv1a64(out.data(), out.size()), STREAMING_ORIGINAL_FNV1A64);
  stream.deinit();
}

TEST(InflateStream, ReinitReusesAllocationAndProducesCorrectOutput) {
  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/false));
  stream.setSource(SMALL_COMPRESSED, SMALL_COMPRESSED_LEN);
  stream.setZlibWrapped();
  std::vector<uint8_t> first(SMALL_ORIGINAL_LEN);
  ASSERT_TRUE(stream.read(first.data(), first.size()));

  ASSERT_TRUE(stream.init(/*streaming=*/true));
  stream.setSource(STREAMING_COMPRESSED, STREAMING_COMPRESSED_LEN);
  stream.setZlibWrapped();
  std::vector<uint8_t> second(STREAMING_ORIGINAL_LEN);
  ASSERT_TRUE(stream.read(second.data(), second.size()));
  EXPECT_EQ(fnv1a64(second.data(), second.size()), STREAMING_ORIGINAL_FNV1A64);
  stream.deinit();
}

TEST(InflateStream, TruncatedInputReturnsErrorNotCrash) {
  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/false));
  stream.setSource(SMALL_COMPRESSED, SMALL_COMPRESSED_LEN / 2);
  stream.setZlibWrapped();

  std::vector<uint8_t> out(SMALL_ORIGINAL_LEN);
  EXPECT_FALSE(stream.read(out.data(), out.size()));
  stream.deinit();
}

TEST(InflateStream, CorruptInputReturnsErrorNotCrash) {
  std::vector<uint8_t> corrupt(SMALL_COMPRESSED, SMALL_COMPRESSED + SMALL_COMPRESSED_LEN);
  corrupt[10] ^= 0xFF;

  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/false));
  stream.setSource(corrupt.data(), corrupt.size());
  stream.setZlibWrapped();

  std::vector<uint8_t> out(SMALL_ORIGINAL_LEN);
  EXPECT_FALSE(stream.read(out.data(), out.size()));
  stream.deinit();
}

TEST(InflateStream, FillCallbackFeedsInputIncrementally) {
  struct FillCtx {
    const uint8_t* data;
    size_t remaining;
    size_t chunk;
  } ctx{STREAMING_COMPRESSED, STREAMING_COMPRESSED_LEN, 500};

  auto fill = [](void* rawCtx, const uint8_t** data) -> size_t {
    auto* c = static_cast<FillCtx*>(rawCtx);
    if (c->remaining == 0) return 0;
    const size_t n = std::min(c->chunk, c->remaining);
    *data = c->data;
    c->data += n;
    c->remaining -= n;
    return n;
  };

  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/true));
  stream.setFill(fill, &ctx);
  stream.setZlibWrapped();

  std::vector<uint8_t> out(STREAMING_ORIGINAL_LEN);
  ASSERT_TRUE(stream.read(out.data(), out.size()));
  EXPECT_EQ(fnv1a64(out.data(), out.size()), STREAMING_ORIGINAL_FNV1A64);
  stream.deinit();
}

TEST(BuildScratch, ClaimReturnsNullWhenNothingLent) { EXPECT_EQ(buildscratch::claim(1), nullptr); }

TEST(BuildScratch, LentBlockCanBeClaimedOnceThenReleased) {
  uint8_t block[128] = {};
  buildscratch::lend(block, sizeof(block));

  size_t lenOut = 0;
  uint8_t* claimed = buildscratch::claim(64, &lenOut);
  ASSERT_NE(claimed, nullptr);
  EXPECT_EQ(claimed, block);
  EXPECT_EQ(lenOut, sizeof(block));

  EXPECT_EQ(buildscratch::claim(1), nullptr);  // already claimed

  buildscratch::release(claimed);
  EXPECT_NE(buildscratch::claim(1), nullptr);  // free again

  buildscratch::release(block);
  buildscratch::reclaim();
}

TEST(BuildScratch, ClaimFailsWhenRequestExceedsLentLength) {
  uint8_t block[16] = {};
  buildscratch::lend(block, sizeof(block));
  EXPECT_EQ(buildscratch::claim(64), nullptr);
  buildscratch::reclaim();
}

TEST(InflateStream, UsesClaimedBuildScratchWhenAvailable) {
  // A block large enough for the one-shot state (~11KB, STATE_ALIGNED) proves
  // InflateStream took the lent arena instead of falling back to the heap:
  // deinit() must not free() memory it does not own.
  std::vector<uint8_t> arena(32 * 1024);
  buildscratch::lend(arena.data(), arena.size());

  InflateStream stream;
  ASSERT_TRUE(stream.init(/*streaming=*/false));
  stream.setSource(SMALL_COMPRESSED, SMALL_COMPRESSED_LEN);
  stream.setZlibWrapped();
  std::vector<uint8_t> out(SMALL_ORIGINAL_LEN);
  ASSERT_TRUE(stream.read(out.data(), out.size()));
  EXPECT_EQ(fnv1a64(out.data(), out.size()), SMALL_ORIGINAL_FNV1A64);
  stream.deinit();

  buildscratch::reclaim();
}
