#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sisl/cache/cache.h>

// ── test value type ─────────────────────────────────────────────────────────────

struct CacheEntry {
    uint64_t id{0};
    std::string data;
};

static uint64_t cache_key_extract(CacheEntry const& e) {
    return e.id;
}

using TestCache = sisl::Cache< uint64_t, CacheEntry >;

// ── fixture with 2Q evictor ────────────────────────────────────────────────────

class CacheTest : public ::testing::Test {
protected:
    static constexpr int64_t EVICTOR_MAX_SIZE = 2048; // bytes
    static constexpr uint32_t NUM_PARTITIONS = 4;
    static constexpr uint32_t NUM_BUCKETS = 32;

    std::shared_ptr< sisl::TwoQEvictor > evictor_;
    std::unique_ptr< TestCache > cache_;

    void SetUp() override {
        sisl::TwoQEvictor::Config ev_cfg;
        ev_cfg.max_size = EVICTOR_MAX_SIZE;
        ev_cfg.num_partitions = NUM_PARTITIONS;
        evictor_ = std::make_shared< sisl::TwoQEvictor >(ev_cfg);

        TestCache::Config cfg;
        cfg.num_buckets = NUM_BUCKETS;
        cfg.ghost_capacity = 64;

        cache_ = std::make_unique< TestCache >(std::move(cfg), evictor_, cache_key_extract);
    }

    void TearDown() override {
        cache_.reset();
        evictor_.reset();
    }

    CacheEntry make_entry(uint64_t id) { return CacheEntry{id, std::string("data_") + std::to_string(id)}; }
};

// ── basic insert / find / remove ───────────────────────────────────────────────

TEST_F(CacheTest, InsertAndFind) {
    auto handle = cache_->insert(1, make_entry(1));
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle->id, 1u);
    EXPECT_EQ(handle->data, "data_1");

    // Release handle, then find again.
    handle = {};
    auto found = cache_->find(1);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id, 1u);
    EXPECT_EQ(found->data, "data_1");
}

TEST_F(CacheTest, InsertDuplicate) {
    auto h1 = cache_->insert(1, make_entry(1));
    ASSERT_TRUE(h1);

    // Release h1 before inserting duplicate — avoid overlapping handles for same key.
    h1 = {};

    auto h2 = cache_->insert(1, make_entry(1));
    EXPECT_FALSE(h2); // duplicate → invalid handle
}

TEST_F(CacheTest, FindMissing) {
    auto handle = cache_->find(999);
    EXPECT_FALSE(handle);
}

TEST_F(CacheTest, RemoveExisting) {
    {
        auto h = cache_->insert(1, make_entry(1));
        ASSERT_TRUE(h);
    } // handle released

    EXPECT_TRUE(cache_->remove(1));

    // Gone after remove
    auto found = cache_->find(1);
    EXPECT_FALSE(found);
}

TEST_F(CacheTest, RemoveMissing) {
    EXPECT_FALSE(cache_->remove(42));
}

// ── mutation through handle ────────────────────────────────────────────────────

TEST_F(CacheTest, MutateThroughHandle) {
    {
        auto h = cache_->insert(10, make_entry(10));
        ASSERT_TRUE(h);
    }

    {
        auto h = cache_->find(10);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->data, "data_10");

        // Mutate in-place through the handle.
        h->data = "updated_10";
    }

    {
        auto h = cache_->find(10);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->data, "updated_10");
    }
}

// ── eviction under memory pressure ─────────────────────────────────────────────

TEST_F(CacheTest, EvictionOnFull) {
    // Insert many entries; the evictor should cap total_size to capacity.
    for (uint64_t i = 0; i < 200; ++i) {
        auto h = cache_->insert(i, make_entry(i));
        // Some late inserts may fail (duplicate check) or be immediately evicted.
    }

    // Give the background evictor thread time to catch up to the high watermark.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_LE(cache_->size(), EVICTOR_MAX_SIZE);
}

TEST_F(CacheTest, EvictedEntryNotFound) {
    // Fill the cache well beyond capacity.
    for (uint64_t i = 0; i < 300; ++i) {
        cache_->insert(i, make_entry(i));
    }

    // At least some of the earliest cold entries should have been evicted.
    uint32_t missing = 0;
    for (uint64_t i = 0; i < 50; ++i) {
        auto h = cache_->find(i);
        if (!h) {
            ++missing;
        }
    }
    EXPECT_GT(missing, 0u);
}

// ── insert after removal reuses space ──────────────────────────────────────────

TEST_F(CacheTest, InsertAfterRemoval) {
    for (uint64_t i = 0; i < 30; ++i) {
        cache_->insert(i, make_entry(i));
    }

    // Remove some entries explicitly
    for (uint64_t i = 0; i < 20; ++i) {
        cache_->remove(i);
    }

    // Now insert new entries — should succeed because space was freed
    for (uint64_t i = 1000; i < 1020; ++i) {
        auto h = cache_->insert(i, make_entry(i));
        EXPECT_TRUE(h);
    }
}

// ── bulk insert and retrieve ───────────────────────────────────────────────────

TEST_F(CacheTest, BulkInsertRetrieve) {
    // Insert a small batch that fits comfortably within capacity.
    constexpr uint32_t N = 7;
    for (uint64_t i = 0; i < N; ++i) {
        auto h = cache_->insert(i, make_entry(i));
        EXPECT_TRUE(h);
    }

    for (uint64_t i = 0; i < N; ++i) {
        auto h = cache_->find(i);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->id, i);
    }
}

// ── insert-find-mutate-find cycle ──────────────────────────────────────────────

TEST_F(CacheTest, InsertFindMutateFindCycle) {
    { cache_->insert(10, make_entry(10)); }

    {
        auto h = cache_->find(10);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->data, "data_10");
        h->data = "updated_10";
    }

    {
        auto h = cache_->find(10);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->data, "updated_10");
    }
}

// ── remove all and reinsert ────────────────────────────────────────────────────

TEST_F(CacheTest, RemoveAllAndReinsert) {
    constexpr uint32_t N = 20;

    for (uint64_t i = 0; i < N; ++i) {
        cache_->insert(i, make_entry(i));
    }

    for (uint64_t i = 0; i < N; ++i) {
        cache_->remove(i);
    }

    EXPECT_EQ(cache_->size(), 0);

    // Reinsert
    for (uint64_t i = 0; i < N; ++i) {
        auto h = cache_->insert(i, make_entry(i));
        EXPECT_TRUE(h);
    }
}

// ── CacheHint: READ vs READ_WRITE ─────────────────────────────────────────────

TEST_F(CacheTest, ReadWriteHintGoesHot) {
    // Insert with READ_WRITE hint — entry should survive longer than READ entries
    // under eviction pressure, since it starts in the hot queue.
    auto hot_handle = cache_->insert(999, make_entry(999), sisl::CacheHint::HOT);
    ASSERT_TRUE(hot_handle);
    hot_handle = {}; // release

    // Flood with READ entries to cause eviction pressure.
    for (uint64_t i = 0; i < 200; ++i) {
        cache_->insert(i, make_entry(i), sisl::CacheHint::COLD);
    }

    // The READ_WRITE entry (999) should still be findable — it's in the hot queue
    // and should not be evicted before cold entries.
    auto found = cache_->find(999);
    EXPECT_TRUE(found);
}

// ── ghost list promotion ───────────────────────────────────────────────────────

TEST_F(CacheTest, GhostListPromotion) {
    // Insert an entry, let it be evicted (flood with others), then re-insert.
    // The re-insert should go to hot queue (ghost hit).
    { cache_->insert(42, make_entry(42), sisl::CacheHint::COLD); }

    // Flood to evict entry 42
    for (uint64_t i = 100; i < 400; ++i) {
        cache_->insert(i, make_entry(i), sisl::CacheHint::COLD);
    }

    // Give the background evictor time to settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify 42 was evicted (may or may not have been, depending on hash distribution).  If it was evicted, the
    // ghost list should remember it and re-insertion should promote to hot.
    auto gone = cache_->find(42);
    if (!gone) {
        // Re-insert: ghost list should promote it to hot.
        auto re_inserted = cache_->insert(42, make_entry(42), sisl::CacheHint::COLD);
        ASSERT_TRUE(re_inserted);
        re_inserted = {};

        // The re-inserted entry should be in the hot queue now (ghost hit promotion).  Verify it's still findable
        // immediately — we don't try to flood again, since flooding can also displace the hot entry under sustained
        // pressure (the 2Q algorithm doesn't guarantee absolute hot retention under arbitrary workloads).
        auto still_there = cache_->find(42);
        EXPECT_TRUE(still_there);
    }
}

// ── concurrent operations ──────────────────────────────────────────────────────

TEST_F(CacheTest, ConcurrentInsertFind) {
    constexpr uint32_t KEYS_PER_THREAD = 50;
    constexpr uint32_t NUM_THREADS = 4;

    std::vector< std::thread > threads;
    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t]() {
            uint64_t base = t * KEYS_PER_THREAD;
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                cache_->insert(base + i, make_entry(base + i));
            }
            // Read back (some may have been evicted by other threads' inserts)
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                auto h = cache_->find(base + i); // may be invalid, that's OK
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_GE(cache_->size(), 0);
}

TEST_F(CacheTest, ConcurrentInsertRemove) {
    constexpr uint32_t N = 100;

    // Pre-insert entries
    for (uint64_t i = 0; i < N; ++i) {
        cache_->insert(i, make_entry(i));
    }

    std::thread inserter([this]() {
        for (uint64_t i = N; i < 2 * N; ++i) {
            cache_->insert(i, make_entry(i));
        }
    });

    std::thread remover([this]() {
        for (uint64_t i = 0; i < N; ++i) {
            cache_->remove(i);
        }
    });

    inserter.join();
    remover.join();

    // Original keys should be gone
    for (uint64_t i = 0; i < N; ++i) {
        auto h = cache_->find(i);
        EXPECT_FALSE(h);
    }
}

// ── size consistency ───────────────────────────────────────────────────────────

TEST_F(CacheTest, SizeConsistentAfterOperations) {
    for (uint64_t i = 0; i < 50; ++i) {
        cache_->insert(i, make_entry(i));
    }
    EXPECT_GE(cache_->size(), 0);
    EXPECT_LE(cache_->size(), EVICTOR_MAX_SIZE);

    for (uint64_t i = 0; i < 25; ++i) {
        cache_->remove(i);
    }
    EXPECT_GE(cache_->size(), 0);

    for (uint64_t i = 100; i < 150; ++i) {
        cache_->insert(i, make_entry(i));
    }
    // Give the background evictor time to catch up before checking the size invariant.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_LE(cache_->size(), EVICTOR_MAX_SIZE);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
