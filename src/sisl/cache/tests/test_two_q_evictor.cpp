/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sisl/cache/two_q_evictor.hpp>

// ── test helpers ────────────────────────────────────────────────────────────────

// Each TestEntry owns a CacheRecord (ValueEntryBase). Must have a stable address while
// it is registered in the evictor (intrusive list hooks).
struct TestEntry {
    sisl::CacheRecord record;
    uint64_t key{0};
    uint32_t size{0};
    uint64_t hash{0};
    bool evicted{false};

    TestEntry(uint64_t k, uint32_t sz, uint64_t h) : key(k), size(sz), hash(h) {}
};

class TwoQEvictorTest : public ::testing::Test {
protected:
    static constexpr int64_t MAX_SIZE       = 4096; // bytes
    static constexpr uint32_t NUM_PARTITIONS = 2;
    static constexpr float HOT_PCT          = 0.50f;
    static constexpr float HIGH_WM          = 0.90f; // 3686 bytes
    static constexpr float LOW_WM           = 0.60f; // 2458 bytes

    std::unique_ptr< sisl::TwoQEvictor > evictor_;
    std::vector< std::unique_ptr< TestEntry > > entries_;
    std::atomic< uint32_t > evict_count_{0};
    std::atomic< uint32_t > cold_evict_count_{0};

    void SetUp() override {
        sisl::TwoQEvictor::Config cfg{
            .max_size       = MAX_SIZE,
            .num_partitions = NUM_PARTITIONS,
            .hot_pct        = HOT_PCT,
            .high_wm_pct    = HIGH_WM,
            .low_wm_pct     = LOW_WM,
        };

        evictor_ = std::make_unique< sisl::TwoQEvictor >(
            cfg,
            [this](sisl::CacheRecord& rec) {
                // Mark entry as evicted. We find it by scanning entries_ (fine for tests).
                for (auto& e : entries_) {
                    if (&e->record == &rec) {
                        e->evicted = true;
                        break;
                    }
                }
                evict_count_.fetch_add(1, std::memory_order_relaxed);
            },
            [this](sisl::CacheRecord& /*rec*/) {
                cold_evict_count_.fetch_add(1, std::memory_order_relaxed);
            });
    }

    void TearDown() override {
        // Remove all non-evicted entries before destroying the evictor, so the intrusive
        // list hooks are properly unlinked.
        for (auto& e : entries_) {
            if (!e->evicted && e->record.member_hook_.is_linked()) {
                evictor_->remove_record(e->hash, e->record);
            }
        }
        evictor_.reset();
    }

    TestEntry& add_cold(uint64_t key, uint32_t size, uint64_t hash = 0) {
        auto e = std::make_unique< TestEntry >(key, size, hash);
        evictor_->add_to_cold(hash, e->record, size);
        auto& ref = *e;
        entries_.push_back(std::move(e));
        return ref;
    }

    TestEntry& add_hot(uint64_t key, uint32_t size, uint64_t hash = 0) {
        auto e = std::make_unique< TestEntry >(key, size, hash);
        evictor_->add_to_hot(hash, e->record, size);
        auto& ref = *e;
        entries_.push_back(std::move(e));
        return ref;
    }
};

// ── basic add_to_cold / add_to_hot ──────────────────────────────────────────────

TEST_F(TwoQEvictorTest, AddToColdTracksSize) {
    add_cold(1, 100);
    EXPECT_EQ(evictor_->total_size(), 100);
}

TEST_F(TwoQEvictorTest, AddToHotTracksSize) {
    add_hot(1, 200);
    EXPECT_EQ(evictor_->total_size(), 200);
}

TEST_F(TwoQEvictorTest, MultipleColdEntries) {
    add_cold(1, 100, 0);
    add_cold(2, 100, 1);
    add_cold(3, 100, 0);
    EXPECT_EQ(evictor_->total_size(), 300);
}

// ── remove_record ───────────────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, RemoveColdEntry) {
    auto& e = add_cold(1, 128);
    evictor_->remove_record(e.hash, e.record);
    e.evicted = true; // so TearDown doesn't try again
    EXPECT_EQ(evictor_->total_size(), 0);
}

TEST_F(TwoQEvictorTest, RemoveHotEntry) {
    auto& e = add_hot(1, 128);
    evictor_->remove_record(e.hash, e.record);
    e.evicted = true;
    EXPECT_EQ(evictor_->total_size(), 0);
}

// ── promote_to_hot ──────────────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, PromoteColdToHot) {
    auto& e = add_cold(1, 128);
    EXPECT_FALSE(e.record.is_in_hot_queue());

    evictor_->promote_to_hot(e.hash, e.record);
    EXPECT_TRUE(e.record.is_in_hot_queue());

    // total_size unchanged — entry just moved queues
    EXPECT_EQ(evictor_->total_size(), 128);
}

TEST_F(TwoQEvictorTest, DoublePromoteIsNoop) {
    auto& e = add_cold(1, 128);
    evictor_->promote_to_hot(e.hash, e.record);
    EXPECT_TRUE(e.record.is_in_hot_queue());

    // Promoting again should be a no-op
    evictor_->promote_to_hot(e.hash, e.record);
    EXPECT_TRUE(e.record.is_in_hot_queue());
    EXPECT_EQ(evictor_->total_size(), 128);
}

// ── CacheRecord queue flags ─────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, ColdEntryHasCorrectFlags) {
    auto& e = add_cold(1, 64);
    EXPECT_FALSE(e.record.is_in_hot_queue());
}

TEST_F(TwoQEvictorTest, HotEntryHasCorrectFlags) {
    auto& e = add_hot(1, 64);
    EXPECT_TRUE(e.record.is_in_hot_queue());
}

// ── CLOCK bit ───────────────────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, ClockBitSetAndClear) {
    auto& e = add_hot(1, 64);

    e.record.set_clock_bit();
    EXPECT_TRUE(e.record.test_and_clear_clock_bit());  // was set → returns true, clears

    EXPECT_FALSE(e.record.test_and_clear_clock_bit()); // now clear → returns false
}

TEST_F(TwoQEvictorTest, ClockBitIdempotentSet) {
    auto& e = add_hot(1, 64);

    e.record.set_clock_bit();
    e.record.set_clock_bit(); // second set is a no-op
    EXPECT_TRUE(e.record.test_and_clear_clock_bit());
    EXPECT_FALSE(e.record.test_and_clear_clock_bit());
}

// ── COLD_ACCESSED bit ───────────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, ColdAccessedBitFirstAccess) {
    auto& e = add_cold(1, 64);
    // First set: bit was not set → returns false
    EXPECT_FALSE(e.record.test_and_set_cold_accessed());
}

TEST_F(TwoQEvictorTest, ColdAccessedBitSecondAccess) {
    auto& e = add_cold(1, 64);
    e.record.test_and_set_cold_accessed(); // first

    // Second access: bit was already set → returns true (caller should promote)
    EXPECT_TRUE(e.record.test_and_set_cold_accessed());
}

// ── acquire / release / evictability ────────────────────────────────────────────

TEST_F(TwoQEvictorTest, AcquireBlocksEviction) {
    auto& e = add_cold(1, 64);
    EXPECT_TRUE(e.record.is_evictable());

    e.record.acquire();
    EXPECT_FALSE(e.record.is_evictable());

    e.record.release();
    EXPECT_TRUE(e.record.is_evictable());
}

TEST_F(TwoQEvictorTest, MultipleAcquireRequiresMultipleRelease) {
    auto& e = add_cold(1, 64);
    e.record.acquire();
    e.record.acquire();
    EXPECT_FALSE(e.record.is_evictable());

    e.record.release();
    EXPECT_FALSE(e.record.is_evictable());

    e.record.release();
    EXPECT_TRUE(e.record.is_evictable());
}

// ── background eviction ─────────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, EvictionTriggeredAtHighWatermark) {
    // high_watermark = 0.90 * 4096 = 3686 bytes
    // low_watermark  = 0.60 * 4096 = 2458 bytes
    // Fill past high watermark to trigger background eviction.

    uint32_t entry_size = 256;
    uint32_t count = 16; // 16 * 256 = 4096 → above high watermark

    for (uint32_t i = 0; i < count; ++i) {
        add_cold(i, entry_size, i % NUM_PARTITIONS);
    }

    // Give the background evictor thread time to wake and evict
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // total_size should be at or below high watermark after eviction
    EXPECT_LE(evictor_->total_size(), static_cast< int64_t >(MAX_SIZE * HIGH_WM));
    EXPECT_GT(evict_count_.load(), 0u);
    EXPECT_GT(cold_evict_count_.load(), 0u);
}

TEST_F(TwoQEvictorTest, PinnedEntriesNotEvicted) {
    uint32_t entry_size = 256;
    uint32_t count = 16;

    for (uint32_t i = 0; i < count; ++i) {
        auto& e = add_cold(i, entry_size, i % NUM_PARTITIONS);
        e.record.acquire(); // pin all entries
    }

    // Even above high watermark, pinned entries cannot be evicted
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Nothing should have been evicted
    EXPECT_EQ(evict_count_.load(), 0u);

    // Release all
    for (auto& e : entries_) {
        e->record.release();
    }
}

// ── hot entries demoted to cold before eviction ─────────────────────────────────

TEST_F(TwoQEvictorTest, HotEntryDemotedToColdBeforeEviction) {
    // Fill hot queue past watermark. The evictor should demote hot entries (whose CLOCK_BIT is
    // clear) to cold, then evict from cold tail.
    uint32_t entry_size = 256;
    uint32_t count = 16;

    for (uint32_t i = 0; i < count; ++i) {
        add_hot(i, entry_size, i % NUM_PARTITIONS);
        // Don't set clock bit → entries eligible for demotion
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // After eviction, total size should be below or at high watermark
    EXPECT_LE(evictor_->total_size(), static_cast< int64_t >(MAX_SIZE * HIGH_WM));
}

TEST_F(TwoQEvictorTest, ClockBitGivesSecondChance) {
    // Entries with CLOCK_BIT set get a second chance (bit cleared, not demoted).
    uint32_t entry_size = 256;

    for (uint32_t i = 0; i < 16; ++i) {
        auto& e = add_hot(i, entry_size, i % NUM_PARTITIONS);
        e.record.set_clock_bit(); // all get second chance
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The first sweep clears the clock bits. The second sweep demotes.
    // So entries should eventually be evicted, but after a delay.
    // We mainly verify no crash and that eviction does eventually complete.
    // Give a bit more time for the second sweep.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_LE(evictor_->total_size(), static_cast< int64_t >(MAX_SIZE * HIGH_WM));
}

// ── mixed hot + cold ────────────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, MixedHotColdEviction) {
    // Half hot, half cold — verify eviction works with both queues populated.
    uint32_t entry_size = 128;
    for (uint32_t i = 0; i < 16; ++i) {
        if (i % 2 == 0) {
            add_hot(i, entry_size, i % NUM_PARTITIONS);
        } else {
            add_cold(i, entry_size, i % NUM_PARTITIONS);
        }
    }

    // 16 * 128 = 2048 — below high watermark (3686). No eviction expected.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(evict_count_.load(), 0u);
    EXPECT_EQ(evictor_->total_size(), 16 * 128);
}

// ── concurrent operations ───────────────────────────────────────────────────────

TEST_F(TwoQEvictorTest, ConcurrentAddRemove) {
    constexpr uint32_t OPS = 50;
    constexpr uint32_t NUM_THREADS = 4;

    std::vector< std::thread > threads;
    std::vector< std::vector< std::unique_ptr< TestEntry > > > per_thread(NUM_THREADS);

    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t, &per_thread]() {
            auto& my_entries = per_thread[t];
            for (uint32_t i = 0; i < OPS; ++i) {
                auto e = std::make_unique< TestEntry >(t * OPS + i, 8, (t * OPS + i) % NUM_PARTITIONS);
                evictor_->add_to_cold(e->hash, e->record, e->size);
                my_entries.push_back(std::move(e));
            }
            // Remove all
            for (auto& e : my_entries) {
                evictor_->remove_record(e->hash, e->record);
                e->evicted = true;
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(evictor_->total_size(), 0);
}

TEST_F(TwoQEvictorTest, ConcurrentPromote) {
    // Multiple threads promoting cold entries concurrently — double-promote guard must not crash.
    constexpr uint32_t N = 20;

    for (uint32_t i = 0; i < N; ++i) {
        add_cold(i, 16, i % NUM_PARTITIONS);
    }

    std::vector< std::thread > threads;
    for (uint32_t t = 0; t < 4; ++t) {
        threads.emplace_back([this]() {
            for (auto& e : entries_) {
                evictor_->promote_to_hot(e->hash, e->record);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // All entries should be in hot queue
    for (auto& e : entries_) {
        EXPECT_TRUE(e->record.is_in_hot_queue());
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}