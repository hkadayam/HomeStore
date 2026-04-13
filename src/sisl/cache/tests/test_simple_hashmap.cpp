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
//
// Tests for the refactored SimpleHashMap<K, V>:
//   - non-refcounted V (plain hashmap usage; trait is no-op)
//   - refcounted V via a custom trait specialisation
//   - erase_if_no_reference, update_or_erase, find/insert/upsert/erase
//
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sisl/cache/simple_hashmap.h>

// ── non-refcounted V ────────────────────────────────────────────────────────

struct PlainValue {
    uint64_t    id{0};
    std::string payload;
};

static uint64_t plain_extract_key(PlainValue const& v) { return v.id; }

using PlainMap = sisl::SimpleHashMap< uint64_t, PlainValue >;

class PlainHashMapTest : public ::testing::Test {
protected:
    static constexpr uint32_t NUM_BUCKETS = 16;
    std::unique_ptr< PlainMap > map_;

    void SetUp() override { map_ = std::make_unique< PlainMap >(NUM_BUCKETS, plain_extract_key); }
    void TearDown() override { map_.reset(); }
};

// ── basic insert / find / erase ────────────────────────────────────────────

TEST_F(PlainHashMapTest, InsertAndFind) {
    auto h = map_->insert(1, PlainValue{1, "hello"});
    ASSERT_TRUE(h);
    EXPECT_EQ(h->id, 1u);
    EXPECT_EQ(h->payload, "hello");

    h = {}; // release

    auto found = map_->find(1);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id, 1u);
    EXPECT_EQ(found->payload, "hello");
}

TEST_F(PlainHashMapTest, InsertDuplicate) {
    auto h1 = map_->insert(1, PlainValue{1, "first"});
    ASSERT_TRUE(h1);
    h1 = {};

    auto h2 = map_->insert(1, PlainValue{1, "second"});
    EXPECT_FALSE(h2);

    auto found = map_->find(1);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->payload, "first"); // original preserved
}

TEST_F(PlainHashMapTest, FindMissing) {
    auto h = map_->find(42);
    EXPECT_FALSE(h);
}

TEST_F(PlainHashMapTest, EraseExisting) {
    { map_->insert(1, PlainValue{1, "data"}); }
    EXPECT_TRUE(map_->erase(1));

    auto h = map_->find(1);
    EXPECT_FALSE(h);
}

TEST_F(PlainHashMapTest, EraseMissing) { EXPECT_FALSE(map_->erase(999)); }

// ── upsert ─────────────────────────────────────────────────────────────────

TEST_F(PlainHashMapTest, UpsertInserts) {
    auto h = map_->upsert(5, PlainValue{5, "five"});
    ASSERT_TRUE(h);
    EXPECT_EQ(h->payload, "five");
    h = {};

    auto f = map_->find(5);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->payload, "five");
}

TEST_F(PlainHashMapTest, UpsertOverwrites) {
    { map_->upsert(5, PlainValue{5, "five"}); }
    auto h = map_->upsert(5, PlainValue{5, "FIVE"});
    ASSERT_TRUE(h);
    EXPECT_EQ(h->payload, "FIVE");
}

// ── update_or_erase ────────────────────────────────────────────────────────

TEST_F(PlainHashMapTest, UpdateOrEraseCreates) {
    bool was_new = map_->update_or_erase(20, [](PlainValue& v, bool found) {
        EXPECT_FALSE(found);
        v.id      = 20;
        v.payload = "created";
        return PlainMap::UpdateAction::Keep;
    });
    EXPECT_TRUE(was_new);

    auto h = map_->find(20);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->payload, "created");
}

TEST_F(PlainHashMapTest, UpdateOrEraseUpdates) {
    { map_->insert(20, PlainValue{20, "original"}); }

    bool was_new = map_->update_or_erase(20, [](PlainValue& v, bool found) {
        EXPECT_TRUE(found);
        v.payload = "updated";
        return PlainMap::UpdateAction::Keep;
    });
    EXPECT_FALSE(was_new);

    auto h = map_->find(20);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->payload, "updated");
}

TEST_F(PlainHashMapTest, UpdateOrEraseDeletes) {
    { map_->insert(20, PlainValue{20, "doomed"}); }

    map_->update_or_erase(20, [](PlainValue&, bool found) {
        EXPECT_TRUE(found);
        return PlainMap::UpdateAction::Erase;
    });

    auto h = map_->find(20);
    EXPECT_FALSE(h);
}

// ── erase_if_no_reference (non-refcounted V → always succeeds) ─────────────

TEST_F(PlainHashMapTest, EraseIfNoReferenceAlwaysSucceedsNonRefcounted) {
    { map_->insert(7, PlainValue{7, "seven"}); }
    EXPECT_TRUE(map_->erase_if_no_reference(7));

    auto h = map_->find(7);
    EXPECT_FALSE(h);
}

// ── bulk operations ───────────────────────────────────────────────────────

TEST_F(PlainHashMapTest, BulkInsertFindErase) {
    constexpr uint32_t N = 1000;

    for (uint32_t i = 0; i < N; ++i) {
        auto h = map_->insert(i, PlainValue{i, std::to_string(i)});
        EXPECT_TRUE(h);
    }

    for (uint32_t i = 0; i < N; ++i) {
        auto h = map_->find(i);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->id, i);
    }

    // Erase even keys
    for (uint32_t i = 0; i < N; i += 2) {
        EXPECT_TRUE(map_->erase(i));
    }

    for (uint32_t i = 0; i < N; ++i) {
        auto h = map_->find(i);
        if (i % 2 == 0) {
            EXPECT_FALSE(h);
        } else {
            ASSERT_TRUE(h);
            EXPECT_EQ(h->id, i);
        }
    }
}

// ── concurrent operations ─────────────────────────────────────────────────

TEST_F(PlainHashMapTest, ConcurrentInsertFind) {
    constexpr uint32_t KEYS_PER_THREAD = 500;
    constexpr uint32_t NUM_THREADS     = 4;

    std::vector< std::thread > threads;
    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t]() {
            uint64_t base = t * KEYS_PER_THREAD;
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                map_->insert(base + i, PlainValue{base + i, std::to_string(base + i)});
            }
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                auto h = map_->find(base + i);
                EXPECT_TRUE(h);
                if (h) { EXPECT_EQ(h->id, base + i); }
            }
        });
    }
    for (auto& th : threads) th.join();

    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        uint64_t base = t * KEYS_PER_THREAD;
        for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
            auto h = map_->find(base + i);
            EXPECT_TRUE(h);
        }
    }
}

TEST_F(PlainHashMapTest, ConcurrentInsertErase) {
    constexpr uint32_t N = 200;

    for (uint32_t i = 0; i < N; ++i) {
        map_->insert(i, PlainValue{i, ""});
    }

    std::thread inserter([&]() {
        for (uint32_t i = 0; i < N; ++i) {
            map_->insert(i + N, PlainValue{i + N, "new"});
        }
    });

    std::thread eraser([&]() {
        for (uint32_t i = 0; i < N; ++i) {
            map_->erase(i);
        }
    });

    inserter.join();
    eraser.join();

    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_FALSE(map_->find(i));
    }
    for (uint32_t i = N; i < 2 * N; ++i) {
        EXPECT_TRUE(map_->find(i));
    }
}

// ── refcounted V via custom trait specialisation ───────────────────────────
// Defines a value type that exposes a refcount and specialises HashmapTraits
// to drive it.  Verifies that erase_if_no_reference honours the refcount.

struct RefcountedValue {
    uint64_t                       id{0};
    mutable std::atomic< uint32_t > refcount{0};

    RefcountedValue() = default;
    explicit RefcountedValue(uint64_t i) : id{i} {}
    RefcountedValue(RefcountedValue const&)            = delete;
    RefcountedValue& operator=(RefcountedValue const&) = delete;
    RefcountedValue(RefcountedValue&& o) noexcept : id{o.id} {
        refcount.store(o.refcount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    RefcountedValue& operator=(RefcountedValue&& o) noexcept {
        id = o.id;
        refcount.store(o.refcount.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

namespace sisl {
template <>
struct HashmapTraits< RefcountedValue > {
    static constexpr bool refcounted = true;
    static void           acquire(RefcountedValue& v) { v.refcount.fetch_add(1, std::memory_order_relaxed); }
    static void           release(RefcountedValue& v) { v.refcount.fetch_sub(1, std::memory_order_release); }
    // No-handles iff refcount == 0.  Map membership is NOT a reference.
    static bool           is_unreferenced(RefcountedValue const& v) {
        return v.refcount.load(std::memory_order_acquire) == 0;
    }
};
} // namespace sisl

static uint64_t refcounted_extract_key(RefcountedValue const& v) { return v.id; }

using RefcountedMap = sisl::SimpleHashMap< uint64_t, RefcountedValue >;

TEST(RefcountedHashMapTest, AcquireOnInsertAndFind) {
    RefcountedMap map{16, refcounted_extract_key};

    {
        auto h = map.insert(1, RefcountedValue{1});
        ASSERT_TRUE(h);
        // Refcount counts only outstanding handles → 1 handle = refcount 1.
        EXPECT_EQ(h->refcount.load(), 1u);
    }
    // Handle released → refcount back to 0 (entry still in map).

    {
        auto h = map.find(1);
        ASSERT_TRUE(h);
        EXPECT_EQ(h->refcount.load(), 1u);
    }
}

TEST(RefcountedHashMapTest, EraseIfNoReferenceWithLiveHandleFails) {
    RefcountedMap map{16, refcounted_extract_key};
    auto          h = map.insert(1, RefcountedValue{1});
    ASSERT_TRUE(h);

    // Handle is alive → refcount = 1 → erase_if_no_reference must fail
    EXPECT_FALSE(map.erase_if_no_reference(1));

    auto found = map.find(1);
    EXPECT_TRUE(found);
}

TEST(RefcountedHashMapTest, EraseIfNoReferenceWithoutHandleSucceeds) {
    RefcountedMap map{16, refcounted_extract_key};
    { map.insert(1, RefcountedValue{1}); }
    // Handle dropped → only map's ref left → erase_if_no_reference succeeds
    EXPECT_TRUE(map.erase_if_no_reference(1));

    auto found = map.find(1);
    EXPECT_FALSE(found);
}

TEST(RefcountedHashMapTest, EraseDropsMapRef) {
    RefcountedMap map{16, refcounted_extract_key};
    { map.insert(1, RefcountedValue{1}); }

    EXPECT_TRUE(map.erase(1));
    EXPECT_FALSE(map.find(1));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
