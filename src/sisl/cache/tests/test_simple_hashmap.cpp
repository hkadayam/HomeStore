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

#include <gtest/gtest.h>
#include <sisl/cache/simple_hashmap.hpp>

// ── helpers ─────────────────────────────────────────────────────────────────────

struct TestValue {
    uint64_t id{0};
    std::string payload;
};

static uint64_t extract_key(const TestValue& v) { return v.id; }

using TestHashMap = sisl::SimpleHashMap< uint64_t, TestValue >;

// Track all hash operations reported through the access callback.
struct OpLog {
    uint32_t creates{0};
    uint32_t accesses{0};
    uint32_t deletes{0};
};

static thread_local OpLog t_op_log;

static void access_cb(const sisl::ValueEntryBase& /*entry*/, const uint64_t& /*key*/, const TestValue& /*value*/,
                       const sisl::hash_op_t op) {
    switch (op) {
    case sisl::hash_op_t::CREATE: ++t_op_log.creates; break;
    case sisl::hash_op_t::ACCESS: ++t_op_log.accesses; break;
    case sisl::hash_op_t::DELETE: ++t_op_log.deletes; break;
    default: break;
    }
}

class SimpleHashMapTest : public ::testing::Test {
protected:
    static constexpr uint32_t NUM_BUCKETS = 16;

    void SetUp() override { t_op_log = {}; }

    std::unique_ptr< TestHashMap > make_map(bool with_cb = true) {
        if (with_cb) {
            return std::make_unique< TestHashMap >(NUM_BUCKETS, extract_key, access_cb);
        }
        return std::make_unique< TestHashMap >(NUM_BUCKETS, extract_key);
    }
};

// ── basic insert / get / erase ──────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, InsertAndGet) {
    auto map = make_map();

    TestValue v{1, "hello"};
    EXPECT_TRUE(map->insert(1, v));
    EXPECT_EQ(t_op_log.creates, 1u);

    TestValue out;
    EXPECT_TRUE(map->get(1, out));
    EXPECT_EQ(out.id, 1u);
    EXPECT_EQ(out.payload, "hello");
    EXPECT_EQ(t_op_log.accesses, 1u);
}

TEST_F(SimpleHashMapTest, InsertDuplicate) {
    auto map = make_map();

    TestValue v1{1, "first"};
    TestValue v2{1, "second"};
    EXPECT_TRUE(map->insert(1, v1));
    EXPECT_FALSE(map->insert(1, v2));

    TestValue out;
    EXPECT_TRUE(map->get(1, out));
    EXPECT_EQ(out.payload, "first"); // original value preserved
}

TEST_F(SimpleHashMapTest, GetMissing) {
    auto map = make_map();
    TestValue out;
    EXPECT_FALSE(map->get(42, out));
}

TEST_F(SimpleHashMapTest, EraseExisting) {
    auto map = make_map();

    TestValue v{1, "data"};
    map->insert(1, v);

    TestValue out;
    EXPECT_TRUE(map->erase(1, out));
    EXPECT_EQ(out.id, 1u);
    EXPECT_EQ(out.payload, "data");
    EXPECT_EQ(t_op_log.deletes, 1u);

    // Gone after erase
    EXPECT_FALSE(map->get(1, out));
}

TEST_F(SimpleHashMapTest, EraseMissing) {
    auto map = make_map();
    TestValue out;
    EXPECT_FALSE(map->erase(999, out));
}

// ── upsert ──────────────────────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, UpsertInserts) {
    auto map = make_map();
    TestValue v{5, "five"};
    EXPECT_TRUE(map->upsert(5, v)); // returns true = new entry
    EXPECT_EQ(t_op_log.creates, 1u);

    TestValue out;
    EXPECT_TRUE(map->get(5, out));
    EXPECT_EQ(out.payload, "five");
}

TEST_F(SimpleHashMapTest, UpsertOverwrites) {
    auto map = make_map();
    TestValue v1{5, "five"};
    TestValue v2{5, "FIVE"};

    map->upsert(5, v1);
    EXPECT_FALSE(map->upsert(5, v2)); // returns false = existed

    TestValue out;
    map->get(5, out);
    EXPECT_EQ(out.payload, "FIVE"); // overwritten
}

// ── update ──────────────────────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, UpdateExisting) {
    auto map = make_map();
    TestValue v{10, "before"};
    map->insert(10, v);

    EXPECT_TRUE(map->update(10, [](TestValue& val) { val.payload = "after"; }));

    TestValue out;
    map->get(10, out);
    EXPECT_EQ(out.payload, "after");
}

TEST_F(SimpleHashMapTest, UpdateMissing) {
    auto map = make_map();
    EXPECT_FALSE(map->update(99, [](TestValue& val) { val.payload = "x"; }));
}

// ── upsert_or_delete ────────────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, UpsertOrDeleteCreates) {
    auto map = make_map();
    // When not found, the callback receives a default-constructed value.
    // Return false to keep it (don't delete).
    bool was_new = map->upsert_or_delete(20, [](TestValue& v, bool found) -> bool {
        EXPECT_FALSE(found);
        v.id = 20;
        v.payload = "created";
        return false; // keep
    });
    EXPECT_TRUE(was_new);

    TestValue out;
    EXPECT_TRUE(map->get(20, out));
    EXPECT_EQ(out.payload, "created");
}

TEST_F(SimpleHashMapTest, UpsertOrDeleteUpdates) {
    auto map = make_map();
    TestValue v{20, "original"};
    map->insert(20, v);

    bool was_new = map->upsert_or_delete(20, [](TestValue& v, bool found) -> bool {
        EXPECT_TRUE(found);
        v.payload = "updated";
        return false; // keep
    });
    EXPECT_FALSE(was_new);

    TestValue out;
    map->get(20, out);
    EXPECT_EQ(out.payload, "updated");
}

TEST_F(SimpleHashMapTest, UpsertOrDeleteDeletes) {
    auto map = make_map();
    TestValue v{20, "doomed"};
    map->insert(20, v);

    map->upsert_or_delete(20, [](TestValue& /*v*/, bool found) -> bool {
        EXPECT_TRUE(found);
        return true; // delete
    });

    TestValue out;
    EXPECT_FALSE(map->get(20, out));
}

// ── find_and_acquire / insert_and_acquire ────────────────────────────────────────

TEST_F(SimpleHashMapTest, FindAndAcquireHit) {
    auto map = make_map();
    TestValue v{7, "seven"};
    map->insert(7, v);

    size_t hash = TestHashMap::compute_hash(7);
    auto [entry, val_ptr] = map->find_and_acquire(hash, 7);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(val_ptr, nullptr);
    EXPECT_EQ(val_ptr->id, 7u);
    EXPECT_EQ(val_ptr->payload, "seven");

    // Entry's refcount should be non-zero (not evictable)
    EXPECT_FALSE(entry->is_evictable());

    // Release restores evictability
    entry->release();
    EXPECT_TRUE(entry->is_evictable());
}

TEST_F(SimpleHashMapTest, FindAndAcquireMiss) {
    auto map = make_map();
    size_t hash = TestHashMap::compute_hash(42);
    auto [entry, val_ptr] = map->find_and_acquire(hash, 42);
    EXPECT_EQ(entry, nullptr);
    EXPECT_EQ(val_ptr, nullptr);
}

TEST_F(SimpleHashMapTest, InsertAndAcquireNew) {
    auto map = make_map();
    TestValue v{8, "eight"};
    size_t hash = TestHashMap::compute_hash(8);

    auto* entry = map->insert_and_acquire(hash, 8, v);
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->is_evictable()); // refcount > 0

    entry->release();
    EXPECT_TRUE(entry->is_evictable());
}

TEST_F(SimpleHashMapTest, InsertAndAcquireDuplicate) {
    auto map = make_map();
    TestValue v{8, "eight"};
    map->insert(8, v);

    size_t hash = TestHashMap::compute_hash(8);
    auto* entry = map->insert_and_acquire(hash, 8, v);
    EXPECT_EQ(entry, nullptr); // duplicate
}

// ── bulk operations ─────────────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, BulkInsertGetErase) {
    auto map = make_map(false /* no access_cb */);
    constexpr uint32_t N = 1000;

    for (uint32_t i = 0; i < N; ++i) {
        TestValue v{i, std::to_string(i)};
        EXPECT_TRUE(map->insert(i, v));
    }

    // Verify all present
    for (uint32_t i = 0; i < N; ++i) {
        TestValue out;
        EXPECT_TRUE(map->get(i, out));
        EXPECT_EQ(out.id, i);
    }

    // Erase even keys
    for (uint32_t i = 0; i < N; i += 2) {
        TestValue out;
        EXPECT_TRUE(map->erase(i, out));
    }

    // Verify odds remain, evens gone
    for (uint32_t i = 0; i < N; ++i) {
        TestValue out;
        if (i % 2 == 0) {
            EXPECT_FALSE(map->get(i, out));
        } else {
            EXPECT_TRUE(map->get(i, out));
            EXPECT_EQ(out.id, i);
        }
    }
}

// ── access callback tracking ────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, AccessCallbackCounts) {
    auto map = make_map();
    constexpr uint32_t N = 50;

    for (uint32_t i = 0; i < N; ++i) {
        TestValue v{i, ""};
        map->insert(i, v);
    }
    EXPECT_EQ(t_op_log.creates, N);

    // Read all entries
    for (uint32_t i = 0; i < N; ++i) {
        TestValue out;
        map->get(i, out);
    }
    EXPECT_EQ(t_op_log.accesses, N);

    // Delete all entries
    for (uint32_t i = 0; i < N; ++i) {
        TestValue out;
        map->erase(i, out);
    }
    EXPECT_EQ(t_op_log.deletes, N);
}

// ── no-callback constructor ─────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, NullAccessCallback) {
    auto map = make_map(false);
    TestValue v{1, "x"};
    EXPECT_TRUE(map->insert(1, v));

    TestValue out;
    EXPECT_TRUE(map->get(1, out));
    EXPECT_TRUE(map->erase(1, out));
    // Just verifying no crash with null callback
}

// ── concurrent insert/get ───────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, ConcurrentInsertGet) {
    auto map = make_map(false);
    constexpr uint32_t KEYS_PER_THREAD = 500;
    constexpr uint32_t NUM_THREADS = 4;

    std::vector< std::thread > threads;
    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&map, t]() {
            uint64_t base = t * KEYS_PER_THREAD;
            // Insert this thread's key range
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                TestValue v{base + i, std::to_string(base + i)};
                map->insert(base + i, v);
            }
            // Read back and verify
            for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
                TestValue out;
                EXPECT_TRUE(map->get(base + i, out));
                EXPECT_EQ(out.id, base + i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    // Verify all keys from all threads are present
    for (uint32_t t = 0; t < NUM_THREADS; ++t) {
        uint64_t base = t * KEYS_PER_THREAD;
        for (uint32_t i = 0; i < KEYS_PER_THREAD; ++i) {
            TestValue out;
            EXPECT_TRUE(map->get(base + i, out));
        }
    }
}

// ── concurrent insert/erase ─────────────────────────────────────────────────────

TEST_F(SimpleHashMapTest, ConcurrentInsertErase) {
    auto map = make_map(false);
    constexpr uint32_t N = 200;

    // Thread 0 inserts 0..N-1, thread 1 erases 0..N-1 concurrently.
    // Some erases will miss (not yet inserted) — that's expected.
    // After both finish, every key is either absent or present (no corruption).

    // Pre-insert all so erase has something to find
    for (uint32_t i = 0; i < N; ++i) {
        TestValue v{i, ""};
        map->insert(i, v);
    }

    std::thread inserter([&]() {
        for (uint32_t i = 0; i < N; ++i) {
            TestValue v{i + N, "new"};
            map->insert(i + N, v);
        }
    });

    std::thread eraser([&]() {
        for (uint32_t i = 0; i < N; ++i) {
            TestValue out;
            map->erase(i, out); // may or may not find it
        }
    });

    inserter.join();
    eraser.join();

    // Original keys (0..N-1) should all be erased
    for (uint32_t i = 0; i < N; ++i) {
        TestValue out;
        EXPECT_FALSE(map->get(i, out));
    }
    // New keys (N..2N-1) should all be present
    for (uint32_t i = N; i < 2 * N; ++i) {
        TestValue out;
        EXPECT_TRUE(map->get(i, out));
    }
}

// ── ValueEntryBase state tests (embedded in hashmap context) ────────────────────

TEST_F(SimpleHashMapTest, EntryStateAfterInsert) {
    auto map = make_map();
    TestValue v{100, "state_test"};
    size_t hash = TestHashMap::compute_hash(100);

    auto* entry = map->insert_and_acquire(hash, 100, v);
    ASSERT_NE(entry, nullptr);

    // Entry should not be invalidated
    EXPECT_FALSE(entry->is_invalidated());

    // Acquire again (refcount = 2)
    entry->acquire();
    EXPECT_FALSE(entry->is_evictable());

    // Release once (refcount = 1, still not evictable)
    entry->release();
    EXPECT_FALSE(entry->is_evictable());

    // Release again (refcount = 0, evictable)
    entry->release();
    EXPECT_TRUE(entry->is_evictable());
}

TEST_F(SimpleHashMapTest, EntryInvalidation) {
    auto map = make_map();
    TestValue v{200, "inv_test"};
    size_t hash = TestHashMap::compute_hash(200);

    auto* entry = map->insert_and_acquire(hash, 200, v);
    ASSERT_NE(entry, nullptr);

    EXPECT_FALSE(entry->is_invalidated());
    entry->invalidate();
    EXPECT_TRUE(entry->is_invalidated());

    entry->release();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}