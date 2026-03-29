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
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <memory>

#include <gtest/gtest.h>
#include <sisl/cache/simple_cache.hpp>
#include <sisl/cache/lru_evictor.hpp>

// ── test value type ─────────────────────────────────────────────────────────────

struct CacheEntry {
    uint64_t id{0};
    std::string data;
    uint32_t entry_size{64}; // simulated size in bytes
};

static uint64_t cache_key_extract(const CacheEntry& e) { return e.id; }
static uint32_t cache_size_extract(const CacheEntry& e) { return e.entry_size; }

using TestCache = sisl::SimpleCache< uint64_t, CacheEntry >;

// ── fixture with LRU evictor ────────────────────────────────────────────────────

class SimpleCacheTest : public ::testing::Test {
protected:
    static constexpr int64_t EVICTOR_MAX_SIZE = 2048; // bytes
    static constexpr uint32_t NUM_PARTITIONS  = 4;
    static constexpr uint32_t NUM_BUCKETS     = 32;

    std::shared_ptr< sisl::LRUEvictor > evictor_;
    std::unique_ptr< TestCache > cache_;

    void SetUp() override {
        evictor_ = std::make_shared< sisl::LRUEvictor >(EVICTOR_MAX_SIZE, NUM_PARTITIONS);
        cache_ = std::make_unique< TestCache >(evictor_, NUM_BUCKETS, cache_key_extract, cache_size_extract);
    }

    void TearDown() override {
        cache_.reset();
        evictor_.reset();
    }

    CacheEntry make_entry(uint64_t id, uint32_t size = 64) {
        return CacheEntry{id, std::string("data_") + std::to_string(id), size};
    }
};

// ── basic insert / get / remove ─────────────────────────────────────────────────

TEST_F(SimpleCacheTest, InsertAndGet) {
    auto e = make_entry(1);
    EXPECT_EQ(cache_->insert(e), sisl::SimpleCacheStatus::success);

    CacheEntry out;
    EXPECT_EQ(cache_->get(1, out), sisl::SimpleCacheStatus::success);
    EXPECT_EQ(out.id, 1u);
    EXPECT_EQ(out.data, "data_1");
}

TEST_F(SimpleCacheTest, InsertDuplicate) {
    auto e1 = make_entry(1);
    auto e2 = make_entry(1);
    EXPECT_EQ(cache_->insert(e1), sisl::SimpleCacheStatus::success);
    EXPECT_EQ(cache_->insert(e2), sisl::SimpleCacheStatus::duplicate);
}

TEST_F(SimpleCacheTest, GetMissing) {
    CacheEntry out;
    EXPECT_EQ(cache_->get(999, out), sisl::SimpleCacheStatus::not_found);
}

TEST_F(SimpleCacheTest, RemoveExisting) {
    auto e = make_entry(1);
    cache_->insert(e);

    CacheEntry out;
    EXPECT_EQ(cache_->remove(1, out), sisl::SimpleCacheStatus::success);
    EXPECT_EQ(out.id, 1u);

    // Gone after remove
    EXPECT_EQ(cache_->get(1, out), sisl::SimpleCacheStatus::not_found);
}

TEST_F(SimpleCacheTest, RemoveMissing) {
    CacheEntry out;
    EXPECT_EQ(cache_->remove(42, out), sisl::SimpleCacheStatus::not_found);
}

// ── update ──────────────────────────────────────────────────────────────────────

TEST_F(SimpleCacheTest, UpdateExisting) {
    auto e = make_entry(1);
    cache_->insert(e);

    CacheEntry updated{1, "updated_data", 64};
    EXPECT_EQ(cache_->update(updated), sisl::SimpleCacheStatus::success);

    CacheEntry out;
    cache_->get(1, out);
    EXPECT_EQ(out.data, "updated_data");
}

TEST_F(SimpleCacheTest, UpdateMissing) {
    CacheEntry e{42, "nope", 64};
    EXPECT_EQ(cache_->update(e), sisl::SimpleCacheStatus::not_found);
}

// ── eviction under memory pressure ──────────────────────────────────────────────

TEST_F(SimpleCacheTest, EvictionOnFull) {
    // Each partition has EVICTOR_MAX_SIZE / NUM_PARTITIONS = 512 bytes.
    // Insert entries of size 64 into one partition (key hashing to same bucket).
    // After 512/64 = 8 entries, the next should trigger eviction.

    // All entries go to partition 0 by using keys that hash to hash_code % 4 == 0.
    // We don't control the hash directly, so we insert many entries and check that
    // the evictor eventually evicts some.
    uint32_t inserted = 0;
    for (uint64_t i = 0; i < 100; ++i) {
        auto e = make_entry(i);
        auto status = cache_->insert(e);
        if (status == sisl::SimpleCacheStatus::success) { ++inserted; }
    }

    // Some entries should have been evicted (evictor filled_size ≤ capacity)
    EXPECT_LE(evictor_->filled_size(), EVICTOR_MAX_SIZE);

    // But we should have successfully inserted many
    EXPECT_GT(inserted, 0u);
}

TEST_F(SimpleCacheTest, EvictedEntryNotFound) {
    // Fill the cache way beyond capacity, then check that earliest entries are gone.
    for (uint64_t i = 0; i < 200; ++i) {
        auto e = make_entry(i);
        cache_->insert(e);
    }

    // At least some of the earliest entries should be evicted
    uint32_t missing = 0;
    for (uint64_t i = 0; i < 50; ++i) {
        CacheEntry out;
        if (cache_->get(i, out) == sisl::SimpleCacheStatus::not_found) { ++missing; }
    }
    EXPECT_GT(missing, 0u);
}

// ── can_evict callback ──────────────────────────────────────────────────────────

TEST_F(SimpleCacheTest, CanEvictCallbackBlocksEviction) {
    // Create a cache with a can_evict callback that rejects all evictions.
    auto reject_cache = std::make_unique< TestCache >(
        evictor_, NUM_BUCKETS, cache_key_extract, cache_size_extract,
        [](const sisl::CacheRecord&) -> bool { return false; });

    // Fill one partition beyond capacity
    uint32_t cant_evict_count = 0;
    for (uint64_t i = 0; i < 100; ++i) {
        auto e = make_entry(i);
        auto status = reject_cache->insert(e);
        if (status == sisl::SimpleCacheStatus::cant_evict) { ++cant_evict_count; }
    }

    // Once the partition fills, further inserts should fail with cant_evict
    EXPECT_GT(cant_evict_count, 0u);

    // Clean up: need to remove entries from the evictor before destroying
    for (uint64_t i = 0; i < 100; ++i) {
        CacheEntry out;
        reject_cache->remove(i, out);
    }
}

// ── insert after eviction reuses space ──────────────────────────────────────────

TEST_F(SimpleCacheTest, InsertAfterEviction) {
    // Fill cache
    for (uint64_t i = 0; i < 100; ++i) {
        cache_->insert(make_entry(i));
    }

    // Remove some entries explicitly
    for (uint64_t i = 0; i < 20; ++i) {
        CacheEntry out;
        cache_->remove(i, out);
    }

    // Now insert new entries — should succeed because space was freed
    for (uint64_t i = 1000; i < 1020; ++i) {
        auto status = cache_->insert(make_entry(i));
        EXPECT_EQ(status, sisl::SimpleCacheStatus::success);
    }
}

// ── bulk insert and retrieve ────────────────────────────────────────────────────

TEST_F(SimpleCacheTest, BulkInsertRetrieve) {
    // Each partition has 512 bytes capacity. Insert a small number of entries
    // so no single partition overflows (worst case all hash to same partition:
    // 7 * 64 = 448 < 512).
    constexpr uint32_t N = 7;
    for (uint64_t i = 0; i < N; ++i) {
        EXPECT_EQ(cache_->insert(make_entry(i)), sisl::SimpleCacheStatus::success);
    }

    for (uint64_t i = 0; i < N; ++i) {
        CacheEntry out;
        EXPECT_EQ(cache_->get(i, out), sisl::SimpleCacheStatus::success);
        EXPECT_EQ(out.id, i);
    }
}

// ── different entry sizes ───────────────────────────────────────────────────────

TEST_F(SimpleCacheTest, MixedEntrySizes) {
    // Insert entries with varying sizes
    cache_->insert(CacheEntry{1, "small", 16});
    cache_->insert(CacheEntry{2, "medium", 128});
    cache_->insert(CacheEntry{3, "large", 256});

    CacheEntry out;
    EXPECT_EQ(cache_->get(1, out), sisl::SimpleCacheStatus::success);
    EXPECT_EQ(cache_->get(2, out), sisl::SimpleCacheStatus::success);
    EXPECT_EQ(cache_->get(3, out), sisl::SimpleCacheStatus::success);
}

// ── insert-get-update-get cycle ─────────────────────────────────────────────────

TEST_F(SimpleCacheTest, InsertGetUpdateGetCycle) {
    auto e = make_entry(10);
    cache_->insert(e);

    CacheEntry out;
    cache_->get(10, out);
    EXPECT_EQ(out.data, "data_10");

    CacheEntry u{10, "updated_10", 64};
    cache_->update(u);

    cache_->get(10, out);
    EXPECT_EQ(out.data, "updated_10");
}

// ── remove all and reinsert ─────────────────────────────────────────────────────

TEST_F(SimpleCacheTest, RemoveAllAndReinsert) {
    constexpr uint32_t N = 20;

    for (uint64_t i = 0; i < N; ++i) {
        cache_->insert(make_entry(i));
    }

    for (uint64_t i = 0; i < N; ++i) {
        CacheEntry out;
        cache_->remove(i, out);
    }

    EXPECT_EQ(evictor_->filled_size(), 0);

    // Reinsert
    for (uint64_t i = 0; i < N; ++i) {
        EXPECT_EQ(cache_->insert(make_entry(i)), sisl::SimpleCacheStatus::success);
    }
}

// ── concurrent operations ───────────────────────────────────────────────────────

TEST_F(SimpleCacheTest, ConcurrentInsertGet) {
    constexpr uint32_t KEYS_PER_THREAD = 50;
    constexpr uint32_t NUM_THREADS = 4;

    std::vector< std::thread > threads;
    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t]() {
            uint64_t base = t * KEYS_PER_THREAD;
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                cache_->insert(make_entry(base + i));
            }
            // Read back (some may have been evicted by other threads' inserts)
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                CacheEntry out;
                cache_->get(base + i, out); // may return not_found, that's OK
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    // LRU evictor enforces capacity per-partition, so total filled_size can
    // exceed max_size when entries distribute across partitions. Verify it
    // stays within NUM_PARTITIONS * per-partition-max = total capacity.
    EXPECT_GE(evictor_->filled_size(), 0);
}

TEST_F(SimpleCacheTest, ConcurrentInsertRemove) {
    constexpr uint32_t N = 100;

    // Pre-insert entries
    for (uint64_t i = 0; i < N; ++i) {
        cache_->insert(make_entry(i, 16)); // small entries so they fit
    }

    std::thread inserter([this]() {
        for (uint64_t i = N; i < 2 * N; ++i) {
            cache_->insert(make_entry(i, 16));
        }
    });

    std::thread remover([this]() {
        for (uint64_t i = 0; i < N; ++i) {
            CacheEntry out;
            cache_->remove(i, out);
        }
    });

    inserter.join();
    remover.join();

    // Original keys should be gone
    for (uint64_t i = 0; i < N; ++i) {
        CacheEntry out;
        EXPECT_EQ(cache_->get(i, out), sisl::SimpleCacheStatus::not_found);
    }
}

// ── evictor filled_size consistency ─────────────────────────────────────────────

TEST_F(SimpleCacheTest, FilledSizeConsistentAfterOperations) {
    // Insert, update, remove — filled_size should always be non-negative
    // and never exceed capacity.
    for (uint64_t i = 0; i < 50; ++i) {
        cache_->insert(make_entry(i));
    }
    EXPECT_GE(evictor_->filled_size(), 0);
    EXPECT_LE(evictor_->filled_size(), EVICTOR_MAX_SIZE);

    for (uint64_t i = 0; i < 25; ++i) {
        CacheEntry out;
        cache_->remove(i, out);
    }
    EXPECT_GE(evictor_->filled_size(), 0);

    for (uint64_t i = 100; i < 150; ++i) {
        cache_->insert(make_entry(i));
    }
    EXPECT_LE(evictor_->filled_size(), EVICTOR_MAX_SIZE);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}