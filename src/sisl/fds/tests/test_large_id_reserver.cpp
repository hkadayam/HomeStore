#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>
#include <sisl/fds/large_id_reserver.h>

using namespace sisl;

TEST(LargeIDReserverTest, BasicReserveAndUnreserve) {
    LargeIDReserver reserver{100};

    auto id0 = reserver.reserve();
    ASSERT_EQ(id0, 0u);
    EXPECT_TRUE(reserver.is_reserved(0));
    EXPECT_EQ(reserver.reserved_count(), 1u);

    auto id1 = reserver.reserve();
    ASSERT_EQ(id1, 1u);
    EXPECT_TRUE(reserver.is_reserved(1));
    EXPECT_EQ(reserver.reserved_count(), 2u);

    reserver.unreserve(0);
    EXPECT_FALSE(reserver.is_reserved(0));
    EXPECT_TRUE(reserver.is_reserved(1));
    EXPECT_EQ(reserver.reserved_count(), 1u);

    // Next reserve should reuse id 0 (lowest available).
    auto id2 = reserver.reserve();
    ASSERT_EQ(id2, 0u);
    EXPECT_EQ(reserver.reserved_count(), 2u);
}

TEST(LargeIDReserverTest, ExplicitReserve) {
    LargeIDReserver reserver{1000};

    reserver.reserve(uint64_t{42});
    EXPECT_TRUE(reserver.is_reserved(42));
    EXPECT_FALSE(reserver.is_reserved(0));

    // Auto-reserve should pick 0 (still the lowest).
    auto id = reserver.reserve();
    EXPECT_EQ(id, 0u);

    reserver.reserve(uint64_t{1});
    // Now 0, 1, 42 are reserved.  Next auto should be 2.
    auto id2 = reserver.reserve();
    EXPECT_EQ(id2, 2u);
    EXPECT_EQ(reserver.reserved_count(), 4u);
}

TEST(LargeIDReserverTest, SequentialExhaustion) {
    constexpr uint64_t max_ids = 10;
    LargeIDReserver reserver{max_ids};

    for (uint64_t i = 0; i < max_ids; ++i) {
        auto id = reserver.reserve();
        ASSERT_EQ(id, i);
    }
    EXPECT_EQ(reserver.reserved_count(), max_ids);

    // Space exhausted.
    auto id = reserver.reserve();
    EXPECT_EQ(id, LargeIDReserver::out_of_bounds);

    // Unreserve one in the middle, then reserve again.
    reserver.unreserve(5);
    auto reclaimed = reserver.reserve();
    EXPECT_EQ(reclaimed, 5u);
}

TEST(LargeIDReserverTest, GapReuse) {
    LargeIDReserver reserver{1000};

    // Reserve 0..9
    for (uint64_t i = 0; i < 10; ++i) {
        reserver.reserve(i);
    }

    // Create a gap by unreserving 3, 4, 5
    reserver.unreserve(3);
    reserver.unreserve(4);
    reserver.unreserve(5);
    EXPECT_EQ(reserver.reserved_count(), 7u);

    // Auto-reserve should fill the gap starting at 3.
    EXPECT_EQ(reserver.reserve(), 3u);
    EXPECT_EQ(reserver.reserve(), 4u);
    EXPECT_EQ(reserver.reserve(), 5u);

    // Next should be 10 (past the original contiguous block).
    EXPECT_EQ(reserver.reserve(), 10u);
}

TEST(LargeIDReserverTest, SparseReservation) {
    LargeIDReserver reserver{10000};

    // Reserve sparse IDs far apart.
    reserver.reserve(uint64_t{100});
    reserver.reserve(uint64_t{500});
    reserver.reserve(uint64_t{9999});

    EXPECT_EQ(reserver.reserved_count(), 3u);

    // Auto-reserve fills from 0 upward, skipping reserved IDs.
    for (uint64_t i = 0; i < 100; ++i) {
        auto id = reserver.reserve();
        EXPECT_EQ(id, i);
    }
    // 100 is already reserved, so next should be 101.
    auto id = reserver.reserve();
    EXPECT_EQ(id, 101u);
}

TEST(LargeIDReserverTest, UnreserveAndReReserve) {
    LargeIDReserver reserver{100};

    // Reserve then unreserve the same ID repeatedly.
    for (int round = 0; round < 5; ++round) {
        reserver.reserve(uint64_t{42});
        EXPECT_TRUE(reserver.is_reserved(42));
        reserver.unreserve(42);
        EXPECT_FALSE(reserver.is_reserved(42));
    }
    EXPECT_EQ(reserver.reserved_count(), 0u);
}

TEST(LargeIDReserverTest, LargeMaxCount) {
    // Verify that a very large max_count doesn't cause issues (sparse domain).
    LargeIDReserver reserver{std::numeric_limits< uint32_t >::max()};

    std::vector< uint64_t > ids;
    for (int i = 0; i < 1000; ++i) {
        ids.push_back(reserver.reserve());
    }

    // All IDs should be unique.
    std::set< uint64_t > unique_ids(ids.begin(), ids.end());
    EXPECT_EQ(unique_ids.size(), 1000u);
    EXPECT_EQ(reserver.reserved_count(), 1000u);

    // Free all and verify.
    for (auto id : ids) {
        reserver.unreserve(id);
    }
    EXPECT_EQ(reserver.reserved_count(), 0u);
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
