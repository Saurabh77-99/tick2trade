#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

#include "hft/common/endian.hpp"
#include "hft/common/flat_hash_map.hpp"
#include "hft/common/timing.hpp"
#include "hft/sim/rng.hpp"

using namespace hft;

TEST(Endian, RoundTrip) {
    std::uint8_t buf[8];
    store_be16(buf, 0x1234);
    EXPECT_EQ(buf[0], 0x12);
    EXPECT_EQ(buf[1], 0x34);
    EXPECT_EQ(load_be16(buf), 0x1234);

    store_be32(buf, 0xDEADBEEF);
    EXPECT_EQ(buf[0], 0xDE);
    EXPECT_EQ(load_be32(buf), 0xDEADBEEFu);

    store_be64(buf, 0x0102030405060708ull);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[7], 0x08);
    EXPECT_EQ(load_be64(buf), 0x0102030405060708ull);

    const std::uint64_t ts = 57'599'999'999'999ull;  // 15:59:59.999999999
    store_be48(buf, ts);
    EXPECT_EQ(load_be48(buf), ts);
}

TEST(FlatHashMap, BasicOperations) {
    FlatHashMap m(16);
    EXPECT_EQ(m.find(42), FlatHashMap::kNotFound);
    EXPECT_TRUE(m.insert(42, 7));
    EXPECT_FALSE(m.insert(42, 9));  // no overwrite
    EXPECT_EQ(m.find(42), 7u);
    EXPECT_EQ(m.erase(42), 7u);
    EXPECT_EQ(m.erase(42), FlatHashMap::kNotFound);
    EXPECT_EQ(m.size(), 0u);
}

// Random operations checked against std::unordered_map. Small capacity forces
// long probe chains, wrap-around at the end of the table, and rehashing,
// which is where backward-shift deletion bugs hide.
TEST(FlatHashMap, MatchesStdUnorderedMap) {
    for (std::uint64_t seed : {1, 2, 3}) {
        FlatHashMap m(8);
        std::unordered_map<std::uint64_t, std::uint32_t> ref;
        sim::Rng rng(seed);
        for (int i = 0; i < 200'000; ++i) {
            const std::uint64_t key = rng.below(5000);  // dense key space = many collisions
            const auto op = rng.below(3);
            if (op == 0) {
                const bool inserted = ref.emplace(key, std::uint32_t(i)).second;
                ASSERT_EQ(m.insert(key, std::uint32_t(i)), inserted);
            } else if (op == 1) {
                auto it = ref.find(key);
                ASSERT_EQ(m.find(key), it == ref.end() ? FlatHashMap::kNotFound : it->second);
            } else {
                auto it = ref.find(key);
                const std::uint32_t expect = it == ref.end() ? FlatHashMap::kNotFound : it->second;
                if (it != ref.end()) ref.erase(it);
                ASSERT_EQ(m.erase(key), expect);
            }
            ASSERT_EQ(m.size(), ref.size());
        }
        for (const auto& [k, v] : ref) ASSERT_EQ(m.find(k), v);
    }
}

TEST(LatencyHistogram, Percentiles) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 10'000; ++v) h.record(v);
    EXPECT_EQ(h.count(), 10'000u);
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 10'000u);
    EXPECT_NEAR(h.mean(), 5000.5, 0.01);
    // Log-linear buckets give ~3% precision, and never under-report.
    const auto p50 = h.percentile(0.50);
    const auto p99 = h.percentile(0.99);
    EXPECT_GE(p50, 5000u);
    EXPECT_LE(p50, 5000u * 103 / 100);
    EXPECT_GE(p99, 9900u);
    EXPECT_LE(p99, 10'000u);
}

TEST(LatencyHistogram, BucketsAreContiguous) {
    // Every value maps to a bucket whose upper edge is >= the value, and
    // bucket indices never decrease as values increase.
    int prev = -1;
    for (std::uint64_t v = 0; v < 100'000; ++v) {
        const int idx = LatencyHistogram::index_of(v);
        ASSERT_GE(idx, prev);
        ASSERT_GE(LatencyHistogram::upper_edge(idx), v);
        prev = idx;
    }
    EXPECT_LT(LatencyHistogram::index_of(~std::uint64_t{0}), LatencyHistogram::kBuckets);
}
