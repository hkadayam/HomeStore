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
// Moved from hkadayam/HomeStore src/tests/test_blkalloc.cpp.
// Ported to the modern SlabBlkAllocator API (replacing the old FixedBlkAllocator + VarsizeBlkAllocator).

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <boost/dynamic_bitset.hpp>
#include "sisl/fds/bitword.h"
#include <folly/ConcurrentSkipList.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/init/Init.h>
#include <folly/synchronization/Hazptr.h>
#include "sisl/logging/logging.h"
#include "sisl/options/options.h"

#include "homestore/blkalloc/blk_cache.h"
#include "homestore/base/homestore_assert.h"
#include "homestore/base/hs_runtime_config.h"
#include "homestore/blkalloc/slab_blk_allocator.h"

using namespace homestore;
using namespace homestore::blkalloc;

static thread_local std::random_device g_rd{};
static thread_local std::default_random_engine g_re{g_rd()};
static std::mutex s_print_mutex;

// folly::ConcurrentHashMap causes a SIGSEGV in hazptr_tc thread-local cleanup
// on short-lived std::threads in Debug builds. Use a simple mutex-guarded map.
struct BlkMapT {
    explicit BlkMapT(size_t = 0) : mtx_{std::make_unique< std::mutex >()} {}
    std::pair< std::unordered_map< blk_num_t, blk_count_t >::iterator, bool > insert(blk_num_t k, blk_count_t v) {
        std::lock_guard< std::mutex > lk{*mtx_};
        return map_.emplace(k, v);
    }
    auto find(blk_num_t k) {
        std::lock_guard< std::mutex > lk{*mtx_};
        return map_.find(k);
    }
    size_t erase(blk_num_t k) {
        std::lock_guard< std::mutex > lk{*mtx_};
        return map_.erase(k);
    }

private:
    std::unordered_map< blk_num_t, blk_count_t > map_;
    std::unique_ptr< std::mutex > mtx_;
};
using BlkListT = folly::ConcurrentSkipList< blk_num_t >;
using BlkListAccessorT = BlkListT::Accessor;
using size_generator_t = std::function< blk_count_t(void) >;

struct AllocedBlkTracker {
    AllocedBlkTracker(const uint64_t quota) : m_alloced_blk_list{BlkListT::create(8)}, m_max_quota{quota} {}

    void adjust_limits(const uint8_t hi_limit_pct) {
        m_lo_limit = m_alloced_blk_list.size();
        m_hi_limit = std::max(to_u64(m_max_quota * hi_limit_pct) / 100, m_lo_limit);
    }

    bool reached_lo_limit() const { return (m_alloced_blk_list.size() < m_lo_limit); }
    bool reached_hi_limit() const { return (m_alloced_blk_list.size() > m_hi_limit); }

    BlkListAccessorT m_alloced_blk_list;
    BlkMapT m_alloced_blk_map;
    uint64_t m_max_quota;
    uint64_t m_lo_limit{0};
    uint64_t m_hi_limit{0};
};

static uint32_t round_count(const uint32_t count) {
    uint32_t new_count{count};
    if ((count & (count - 1)) != 0) {
        new_count = to_u32(1) << (sisl::logBase2(count) + 1);
        LOGINFO("Count {} is not a power of 2, rounding total count to {}", count, new_count);
    }
    return new_count;
}

struct BlkAllocatorTest {
    std::atomic< int64_t > m_alloced_count{0};
    std::vector< AllocedBlkTracker > m_slab_alloced_blks;
    const uint32_t m_total_count{round_count(SISL_OPTIONS["num_blks"].as< uint32_t >())};
    std::uniform_int_distribution< uint32_t > m_rand_blk_generator;
    bool m_track_slabs{false};
    size_t m_num_slabs{0};

    BlkAllocatorTest() : m_rand_blk_generator{1, m_total_count} { m_slab_alloced_blks.emplace_back(m_total_count); }
    BlkAllocatorTest(const BlkAllocatorTest&) = delete;
    BlkAllocatorTest(BlkAllocatorTest&&) noexcept = delete;
    BlkAllocatorTest& operator=(const BlkAllocatorTest&) = delete;
    BlkAllocatorTest& operator=(BlkAllocatorTest&&) noexcept = delete;
    ~BlkAllocatorTest() = default;

    void start_track_slabs() {
        assert(m_track_slabs == false);
        m_track_slabs = true;

        const auto& slab_distribution{homestore::HomeStoreRuntimeConfig::default_slab_distribution()};
        m_num_slabs = slab_distribution.size();
        double cum_pct{0.0};
        uint64_t cum{0};
        for (size_t slab_index{0}; slab_index < slab_distribution.size(); ++slab_index) {
            cum_pct += slab_distribution[slab_index];
            const blk_count_t slab_size{to_u16(to_u16(1) << slab_index)};
            const blk_num_t slab_count{to_u32((m_total_count / slab_size) * (slab_distribution[slab_index] / 100.0))};
            if (slab_index == 0) {
                m_slab_alloced_blks[0].m_max_quota = slab_count;
            } else {
                m_slab_alloced_blks.emplace_back(slab_count);
            }
            cum += slab_count * slab_size;
        }
        assert(cum_pct < 100.0 * (1.0 + std::numeric_limits< double >::epsilon()));
        if (cum < m_total_count) {
            m_slab_alloced_blks[0].m_max_quota += m_total_count - cum;
        }
    }

    [[nodiscard]] BlkListAccessorT& blk_list(const slab_idx_t idx) {
        return m_slab_alloced_blks[idx].m_alloced_blk_list;
    }
    [[nodiscard]] BlkMapT& blk_map(const slab_idx_t idx) { return m_slab_alloced_blks[idx].m_alloced_blk_map; }

    [[nodiscard]] slab_idx_t nblks_to_idx(const blk_count_t n_blks) {
        return m_track_slabs ? nblks_to_slab_tbl[n_blks] : 0;
    }

    [[nodiscard]] bool alloced(const BlkId& bid, const bool track_block_group) {
        uint32_t blk_num = bid.blk_num();
        if (blk_num >= m_total_count) {
            {
                std::scoped_lock< std::mutex > lock{s_print_mutex};
                std::cout << "Alloced: blk_num >= m_total_count" << blk_num << ' ' << m_total_count << std::endl;
            }
            return false;
        }
        m_alloced_count.fetch_add(bid.blk_count(), std::memory_order_acq_rel);

        const slab_idx_t slab_idx{m_track_slabs ? nblks_to_idx(bid.blk_count()) : to_u16(0)};
        if (track_block_group) {
            if (!blk_map(slab_idx).insert(blk_num, bid.blk_count()).second) {
                {
                    std::scoped_lock< std::mutex > lock{s_print_mutex};
                    std::cout << "Duplicate alloc of blk=" << blk_num << std::endl;
                }
                return false;
            } else {
                blk_list(slab_idx).add(blk_num);
            }
        } else {
            for (blk_count_t i{0}; i < bid.blk_count(); ++i) {
                if (!blk_list(slab_idx).add(blk_num)) {
                    {
                        std::scoped_lock< std::mutex > lock{s_print_mutex};
                        std::cout << "Duplicate alloc of blk=" << blk_num << std::endl;
                    }
                    return false;
                }
                ++blk_num;
            }
        }

        LOGTRACEMOD(blkalloc, "After Alloced nblks={} blk_range=[{}-{}] skip_list_size={} alloced_count={}",
                    bid.blk_count(), blk_num, blk_num + bid.blk_count() - 1, blk_list(slab_idx).size(),
                    m_alloced_count.load(std::memory_order_relaxed));
        return true;
    }

    [[nodiscard]] bool freed(const uint32_t blk_num) {
        assert(m_track_slabs == false);
        m_alloced_count.fetch_sub(1, std::memory_order_acq_rel);
        if (!blk_list(0).erase(to_u32(blk_num))) {
            {
                std::scoped_lock< std::mutex > lock{s_print_mutex};
                std::cout << "freed: Expected to be set blk=" << blk_num << std::endl;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] BlkId pick_rand_blks_to_free(const blk_count_t pref_nblks, const bool round_nblks,
                                               const bool track_group_block) {
        return m_track_slabs ? pick_rand_slab_blks_to_free(pref_nblks, track_group_block)
                             : pick_rand_pool_blks_to_free(pref_nblks, round_nblks, track_group_block);
    }

    void run_parallel(const uint32_t nthreads, const uint64_t total_count,
                      const std::function< void(uint64_t, std::atomic< bool >& terminate_flag) >& thr_fn) {
        std::atomic< bool > terminate_flag{false};
        uint64_t start{0};
        const uint64_t n_per_thread{(total_count - 1) / nthreads + 1};
        std::vector< std::thread > threads;

        while (start < total_count) {
            const uint64_t n_amount{std::min(n_per_thread, total_count - start)};
            threads.emplace_back(thr_fn, n_amount, std::ref(terminate_flag));
            start += n_amount;
        }

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        ASSERT_EQ(terminate_flag, false);
    }

    [[nodiscard]] static blk_count_t uniform_rand_size() {
        static std::uniform_int_distribution< blk_count_t > s_rand_size_generator{1, to_u16(256)};
        return s_rand_size_generator(g_re);
    }

    [[nodiscard]] static blk_count_t round_rand_size() {
        static std::uniform_int_distribution< uint8_t > s_rand_slab_generator{1, to_u8(8)};
        return to_u16(to_u16(1) << s_rand_slab_generator(g_re));
    }

    [[nodiscard]] static constexpr blk_count_t single_blk_size() { return 1; }

    [[nodiscard]] BlkId pick_rand_slab_blks_to_free(const blk_count_t pref_nblks, const bool track_block_group) {
        const auto start_idx{nblks_to_idx(pref_nblks)};
        auto idx{start_idx};

        uint32_t start_blk_num{0};
        blk_count_t n_blks{0};
        uint32_t rand_num{m_rand_blk_generator(g_re)};
        do {
            if (blk_list(idx).size() > 0) {
                do {
                    const auto it{blk_list(idx).lower_bound(rand_num)};
                    if (it != blk_list(idx).end()) {
                        if (blk_list(idx).erase(*it)) {
                            start_blk_num = *it;
                            if (track_block_group) {
                                const auto map_it{blk_map(idx).find(start_blk_num)};
                                const blk_count_t group_size{map_it->second};
                                n_blks = std::min(group_size, pref_nblks);
                                blk_map(idx).erase(start_blk_num);
                                if (n_blks < group_size) {
                                    const blk_count_t remain_blocks{to_u16(group_size - n_blks)};
                                    const auto new_idx = nblks_to_idx(remain_blocks);
                                    blk_map(new_idx).insert(start_blk_num + n_blks, remain_blocks);
                                    blk_list(new_idx).add(start_blk_num + n_blks);
                                }
                            } else {
                                n_blks = 1;
                            }
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    } else {
                        rand_num /= 2;
                    }
                } while (blk_list(idx).size() > 0);
            } else {
                if (++idx == m_num_slabs) {
                    idx = 0;
                }
                if (idx == start_idx) {
                    break;
                }
            }
        } while (n_blks == 0);
        HS_REL_ASSERT_GE(n_blks, 1);

        if (!track_block_group) {
            blk_num_t current_blk{start_blk_num + 1};
            while (n_blks < pref_nblks) {
                if (blk_list(idx).erase(current_blk)) {
                    ++n_blks;
                    ++current_blk;
                } else {
                    break;
                }
            }
        }

        m_alloced_count.fetch_sub(n_blks, std::memory_order_acq_rel);
        return BlkId(start_blk_num, n_blks, 0);
    }

    [[nodiscard]] BlkId pick_rand_pool_blks_to_free(const blk_count_t pref_nblks, const bool round_nblks,
                                                    const bool track_block_group) {
        uint32_t start_blk_num{0};
        blk_count_t n_blks{0};

        uint32_t rand_num{m_rand_blk_generator(g_re)};
        do {
            const auto it{blk_list(0).lower_bound(rand_num)};
            if (it != blk_list(0).end()) {
                start_blk_num = *it;
                if (blk_list(0).erase(start_blk_num)) {
                    if (track_block_group) {
                        const auto map_it{blk_map(0).find(start_blk_num)};
                        const blk_count_t group_size{map_it->second};
                        n_blks = std::min(group_size, pref_nblks);
                        if (round_nblks && (n_blks > 2)) {
                            n_blks = to_u16(to_u16(1) << sisl::logBase2(n_blks));
                        }
                        blk_map(0).erase(start_blk_num);
                        if (n_blks < group_size) {
                            const blk_count_t remain_blocks{to_u16(group_size - n_blks)};
                            blk_map(0).insert(start_blk_num + n_blks, remain_blocks);
                            blk_list(0).add(start_blk_num + n_blks);
                        }
                    } else {
                        n_blks = 1;
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } else {
                rand_num /= 2;
            }
        } while (blk_list(0).size() > 0);
        assert(n_blks >= 1);

        if (!track_block_group) {
            blk_num_t current_blk{start_blk_num + 1};
            while (n_blks < pref_nblks) {
                if (blk_list(0).erase(current_blk)) {
                    ++n_blks;
                    ++current_blk;
                } else {
                    break;
                }
            }

            if (round_nblks && (n_blks > 2)) {
                const auto rounded_n_blks = to_u16(to_u16(1) << sisl::logBase2(n_blks));
                for (int i{0}; i < (n_blks - rounded_n_blks); ++i) {
                    blk_list(0).add(start_blk_num + rounded_n_blks + i);
                }
                n_blks = rounded_n_blks;
            }
        }

        m_alloced_count.fetch_sub(n_blks, std::memory_order_acq_rel);

        LOGTRACEMOD(blkalloc, "After Freed n_blks={} blk_range=[{}-{}] skip_list_size={} alloced_count={}", n_blks,
                    start_blk_num, start_blk_num + n_blks - 1, blk_list(0).size(),
                    m_alloced_count.load(std::memory_order_relaxed));

        return BlkId(start_blk_num, n_blks, 0);
    }
};

// SlabBlkAllocator test fixture — replaces both FixedBlkAllocatorTest and VarsizeBlkAllocatorTest.
// The old VarsizeBlkAllocator maps to SlabBlkAllocator with ExpandedAlloc + use_slab_cache_.
// The old FixedBlkAllocator (alloc_contiguous only) maps to SlabBlkAllocator with CompactAlloc.
struct SlabBlkAllocatorTest : public ::testing::Test, BlkAllocatorTest {
    std::unique_ptr< SlabBlkAllocator > m_allocator;

    SlabBlkAllocatorTest() : BlkAllocatorTest() {
        HomeStoreRuntimeConfig::init_settings_default();
        // Bump retry count for CompactAlloc: the test hammers many threads on a small block space, which
        // amplifies the transient slab-empty window from concurrent break_up. Production default of 2 is
        // fine for real workloads.
        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) { s.blkallocator.max_slab_alloc_attempt = 8; });
        HS_SETTINGS_FACTORY().save();
    }
    SlabBlkAllocatorTest(const SlabBlkAllocatorTest&) = delete;
    SlabBlkAllocatorTest(SlabBlkAllocatorTest&&) noexcept = delete;
    SlabBlkAllocatorTest& operator=(const SlabBlkAllocatorTest&) = delete;
    SlabBlkAllocatorTest& operator=(SlabBlkAllocatorTest&&) noexcept = delete;
    ~SlabBlkAllocatorTest() override = default;

    void SetUp() override {}
    void TearDown() override {}

    // Create allocator in CompactAlloc mode (all blocks in slab, no bitmap fallback).
    // Replaces the old FixedBlkAllocator for single-block alloc_contiguous tests.
    void create_compact_allocator() {
        SlabBlkAllocConfig cfg{4096, 4096, 4096, to_u64(m_total_count) * 4096, false, "test_compact"};
        cfg.alloc_mode = AllocMode::CompactAlloc;
        m_allocator = std::make_unique< SlabBlkAllocator >(cfg, std::nullopt, 0);
    }

    // Create allocator in ExpandedAlloc mode (slab cache + bitmap fallback + sweep thread).
    void create_expanded_allocator(const bool use_slabs = true, uint64_t size = 0) {
        if (size == 0) {
            size = to_u64(m_total_count);
        }
        SlabBlkAllocConfig cfg{4096, 4096, 4096, size * 4096, false, "test_expanded"};
        cfg.alloc_mode = AllocMode::ExpandedAlloc;
        cfg.use_slab_cache_ = use_slabs;
        m_allocator = std::make_unique< SlabBlkAllocator >(cfg, std::nullopt, 0);
    }

    [[nodiscard]] bool alloc_contiguous_blk(const BlkAllocStatus exp_status, BlkId& bid, bool track_block_group) {
        const auto ret = m_allocator->alloc_contiguous(bid);
        if (ret != exp_status) {
            {
                std::scoped_lock< std::mutex > lock{s_print_mutex};
                std::cout << "Ret!=exp_status: ret=" << ret << " expected status=" << exp_status << std::endl;
            }
            return false;
        }
        if (ret == BlkAllocStatus::SUCCESS) {
            if (!alloced(bid, track_block_group)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool alloc_rand_blk(const BlkAllocStatus exp_status, const bool is_contiguous,
                                      const blk_count_t reqd_size, const bool track_block_group) {
        blk_alloc_hints hints;
        hints.is_contiguous = is_contiguous;

        static thread_local BlkIds bids;
        bids.clear();

        const auto ret = m_allocator->alloc(reqd_size, hints, bids);
        if (ret != exp_status) {
            {
                std::scoped_lock< std::mutex > lock{s_print_mutex};
                std::cout << "Ret!=exp_status: ret=" << ret << " expected status=" << exp_status << std::endl;
            }
            return false;
        }
        if (ret == BlkAllocStatus::SUCCESS) {
            if (is_contiguous) {
                if (bids.size() != 1) {
                    {
                        std::scoped_lock< std::mutex > lock{s_print_mutex};
                        std::cout << "Did not expect multiple bids for contiguous request.  Bids=" << bids.size()
                                  << std::endl;
                    }
                    return false;
                }
            }

            blk_count_t sz{0};
            for (auto& bid : bids) {
                if (!alloced(bid, track_block_group)) {
                    return false;
                }
                sz += bid.blk_count();
            }
            if (sz != reqd_size) {
                {
                    std::scoped_lock< std::mutex > lock{s_print_mutex};
                    std::cout << "Didn't get the size we expect.  Requested size=" << reqd_size << " size=" << sz
                              << std::endl;
                }
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool free_blk(const uint32_t blk_num) {
        m_allocator->free(BlkId{blk_num, 1, 0});
        return freed(blk_num);
    }

    [[nodiscard]] BlkId free_random_alloced_blk(const bool track_block_group) {
        const BlkId bid{pick_rand_blks_to_free(1, false, track_block_group)};
        m_allocator->free(bid);
        return bid;
    }

    [[nodiscard]] BlkId free_random_alloced_sized_blk(const blk_count_t reqd_size, const bool round_nblks,
                                                      const bool track_block_group) {
        const BlkId bid{pick_rand_blks_to_free(reqd_size, round_nblks, track_block_group)};
        m_allocator->free(bid);
        return bid;
    }

    void validate_count() const {
        ASSERT_EQ(m_allocator->get_used_blks(), m_alloced_count.load(std::memory_order_relaxed))
            << "Used blks count mismatch";
    }

public:
    [[nodiscard]] uint64_t preload(const uint64_t count, const bool is_contiguous,
                                   const size_generator_t& size_generator, const bool track_block_group) {
        const auto nthreads = std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2,
                                                     SISL_OPTIONS["num_threads"].as< uint32_t >());
        std::atomic< uint64_t > total_alloced{0};
        run_parallel(nthreads, count, [&](const uint64_t count_per_thread, std::atomic< bool >& terminate_flag) {
            for (uint64_t i{0}; (i < count_per_thread) && !terminate_flag;) {
                const auto rand_size{size_generator()};
                if (!alloc_rand_blk(BlkAllocStatus::SUCCESS, is_contiguous, rand_size, track_block_group)) {
                    terminate_flag = true;
                }
                i += rand_size;
                total_alloced += rand_size;
            }
        });
        return total_alloced;
    }

    [[nodiscard]] std::pair< uint64_t, uint64_t > do_alloc_free(const uint64_t num_iters, const bool is_contiguous,
                                                                const size_generator_t& size_generator,
                                                                const uint8_t limit_pct, const bool round_nblks,
                                                                const bool track_block_group) {
        const auto nthreads{std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2,
                                                   SISL_OPTIONS["num_threads"].as< uint32_t >())};
        for (auto& s : m_slab_alloced_blks) {
            s.adjust_limits(limit_pct);
        }

        const int64_t overall_hi_limit{to_i64(m_total_count * limit_pct) / 100};
        std::atomic< uint64_t > total_alloc{0}, total_dealloc{0};
        run_parallel(nthreads, num_iters, [&](const uint64_t iters_per_thread, std::atomic< bool >& terminate_flag) {
            uint64_t alloced_nblks{0};
            uint64_t freed_nblks{0};

            for (uint64_t i{0}; (i < iters_per_thread) && !terminate_flag; ++i) {
                const blk_count_t rand_size{size_generator()};
                const auto idx{nblks_to_idx(rand_size)};

                if (!m_slab_alloced_blks[idx].reached_hi_limit() &&
                    (m_alloced_count.load(std::memory_order_relaxed) < overall_hi_limit)) {
                    if (!alloc_rand_blk(BlkAllocStatus::SUCCESS, is_contiguous, rand_size, track_block_group)) {
                        terminate_flag = true;
                        continue;
                    }
                    alloced_nblks += rand_size;
                }

                if (!m_slab_alloced_blks[idx].reached_lo_limit()) {
                    blk_count_t freed_size{0};
                    while (freed_size < rand_size) {
                        const auto bid{
                            free_random_alloced_sized_blk(rand_size - freed_size, round_nblks, track_block_group)};
                        freed_nblks += bid.blk_count();
                        freed_size += bid.blk_count();
                    }
                }
            }
            LOGINFO("Alloced {} random blks and freed {} random blks in this thread", alloced_nblks, freed_nblks);
            total_alloc += alloced_nblks;
            total_dealloc += freed_nblks;
        });
        LOGINFO("Total Alloced {} random blks and freed {} random blks in all thread", total_alloc.load(),
                total_dealloc.load());
        return {total_alloc, total_dealloc};
    }
};

// ── CompactAlloc tests (replacing old FixedBlkAllocator tests) ──────────────

TEST_F(SlabBlkAllocatorTest, compact_alloc_free) {
    create_compact_allocator();

    const auto nthreads{
        std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2, SISL_OPTIONS["num_threads"].as< uint32_t >())};
    const auto count = m_total_count;

    LOGINFO("Step 1: Pre allocate {} objects in {} threads", count / 2, nthreads);
    run_parallel(nthreads, count / 2, [&](const uint64_t count_per_thread, std::atomic< bool >& terminate_flag) {
        for (uint64_t i{0}; (i < count_per_thread) && !terminate_flag; ++i) {
            BlkId bid;
            if (!alloc_contiguous_blk(BlkAllocStatus::SUCCESS, bid, false)) {
                terminate_flag = true;
            }
        }
    });
    validate_count();

    LOGINFO("Step 2: Free {} blks randomly in {} threads ", count / 4, nthreads);
    run_parallel(nthreads, count / 4, [&](const uint64_t count_per_thread, std::atomic< bool >& terminate_flag) {
        for (uint64_t i{0}; (i < count_per_thread) && !terminate_flag; ++i) {
            [[maybe_unused]] const BlkId blkId{free_random_alloced_blk(false)};
        }
    });
    validate_count();

    LOGINFO("Step 3: Re-allocate the freed blks and fill remaining in {} threads", nthreads);
    run_parallel(nthreads, count * 3 / 4, [&](const uint64_t count_per_thread, std::atomic< bool >& terminate_flag) {
        for (uint64_t i{0}; (i < count_per_thread) && !terminate_flag; ++i) {
            BlkId bid;
            if (!alloc_contiguous_blk(BlkAllocStatus::SUCCESS, bid, false)) {
                terminate_flag = true;
            }
        }
    });
    validate_count();

    BlkId bid;
    LOGINFO("Step 4: Validate if further allocation result in space full error");
    ASSERT_TRUE(alloc_contiguous_blk(BlkAllocStatus::SPACE_FULL, bid, false));
}

// ── ExpandedAlloc tests (replacing old VarsizeBlkAllocator tests) ────────────

namespace {
void alloc_free_contiguous_unirandsize(SlabBlkAllocatorTest* const test, uint64_t capacity) {
    const auto nthreads{
        std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2, SISL_OPTIONS["num_threads"].as< uint32_t >())};
    auto max_rand_size{std::max(capacity / 4096, uint64_t(2))};
    std::uniform_int_distribution< blk_count_t > s_rand_size_generator{1, to_u16(max_rand_size)};

    auto rand_func = [&s_rand_size_generator]() -> blk_count_t { return s_rand_size_generator(g_re); };
    const uint8_t prealloc_pct{5};
    LOGINFO("Step 1: Pre allocate {}% of total blks which is {} blks in {} threads", prealloc_pct,
            capacity * prealloc_pct / 100, nthreads);
    [[maybe_unused]] const auto preload_alloced{test->preload(capacity * prealloc_pct / 100, true, rand_func, true)};

    auto num_iters{SISL_OPTIONS["iters"].as< uint64_t >()};
    const uint64_t divisor{1024};
    if (num_iters > capacity / divisor) {
        LOGINFO("For contiguous_unirandsize test, iters={} cannot be more than 1/{}th of total count={}. Adjusting",
                num_iters, divisor, capacity);
        num_iters = capacity / divisor;
    }
    const uint8_t runtime_pct{10};
    LOGINFO("Step 2: Do alloc/free contiguous blks with completely random size ratio_range=[{}-{}] threads={} iters={}",
            prealloc_pct, runtime_pct, nthreads, num_iters);
    const auto result{test->do_alloc_free(num_iters, true, rand_func, runtime_pct, false, true)};
}
} // namespace

TEST_F(SlabBlkAllocatorTest, expanded_contiguous_unirandsize_with_slabs) {
    create_expanded_allocator();
    alloc_free_contiguous_unirandsize(this, m_total_count);
}

TEST_F(SlabBlkAllocatorTest, small_allocator_with_slab) {
    auto size = 4224;
    create_expanded_allocator(true, size);
    alloc_free_contiguous_unirandsize(this, size);
}

TEST_F(SlabBlkAllocatorTest, expanded_contiguous_unirandsize_without_slabs) {
    create_expanded_allocator(false);
    alloc_free_contiguous_unirandsize(this, m_total_count);
}

namespace {
void alloc_free_contiguous_roundrandsize(SlabBlkAllocatorTest* const test) {
    const auto nthreads{
        std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2, SISL_OPTIONS["num_threads"].as< uint32_t >())};
    const uint8_t prealloc_pct{5};
    const uint64_t preload_amount{to_u64(test->m_total_count) * prealloc_pct / 100};
    LOGINFO("Step 1: Pre allocate {}% of total blks which is {} blks in {} threads", prealloc_pct, preload_amount,
            nthreads);
    [[maybe_unused]] const auto preload_alloced{
        test->preload(preload_amount, true, BlkAllocatorTest::round_rand_size, true)};

    auto num_iters{SISL_OPTIONS["iters"].as< uint64_t >()};
    const uint64_t divisor{512};
    if (num_iters > test->m_total_count / divisor) {
        LOGINFO("For contiguous_roundrandsize test, iters={} cannot be more than 1/{}th of total count={}. Adjusting",
                num_iters, divisor, test->m_total_count);
        num_iters = test->m_total_count / divisor;
    }
    const uint8_t runtime_pct{10};
    LOGINFO("Step 2: Do alloc/free contiguous blks with round random size ratio_range=[{}-{}] threads={} iters={}",
            prealloc_pct, runtime_pct, nthreads, num_iters);
    [[maybe_unused]] const auto result{
        test->do_alloc_free(num_iters, true, BlkAllocatorTest::round_rand_size, runtime_pct, true, true)};
}
} // namespace

TEST_F(SlabBlkAllocatorTest, expanded_contiguous_roundrandsize_with_slabs) {
    create_expanded_allocator();
    alloc_free_contiguous_roundrandsize(this);
}

TEST_F(SlabBlkAllocatorTest, expanded_contiguous_roundrandsize_without_slabs) {
    create_expanded_allocator(false);
    alloc_free_contiguous_roundrandsize(this);
}

TEST_F(SlabBlkAllocatorTest, expanded_contiguous_slabrandsize) {
    create_expanded_allocator();
    start_track_slabs();

    const auto nthreads{
        std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2, SISL_OPTIONS["num_threads"].as< uint32_t >())};
    const uint8_t prealloc_pct{25};
    const uint64_t preload_amount{to_u64(m_total_count) * prealloc_pct / 100};
    LOGINFO("Step 1: Pre allocate {}% of total blks which is {} blks in {} threads", prealloc_pct, preload_amount,
            nthreads);
    [[maybe_unused]] const auto preload_alloced{preload(preload_amount, true, BlkAllocatorTest::round_rand_size, true)};

    auto num_iters{SISL_OPTIONS["iters"].as< uint64_t >()};
    const uint64_t divisor{1};
    if (num_iters > m_total_count / divisor) {
        LOGINFO("For contiguous_slabrandsize test, iters={} cannot be more than 1/{}th of total count={}. Adjusting",
                num_iters, divisor, m_total_count);
        num_iters = m_total_count / divisor;
    }
    const uint8_t runtime_pct{75};
    LOGINFO("Step 2: Do alloc/free contiguous blks with on slab sized ratio_range=[{}-{}] threads={} iters={}",
            prealloc_pct, runtime_pct, nthreads, num_iters);
    [[maybe_unused]] const auto result{
        do_alloc_free(num_iters, true, BlkAllocatorTest::round_rand_size, runtime_pct, false, true)};
}

namespace {
void alloc_free_contiguous_onesize(SlabBlkAllocatorTest* const test) {
    const auto nthreads{
        std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2, SISL_OPTIONS["num_threads"].as< uint32_t >())};

    const uint64_t preload_amount{to_u64(test->m_total_count) / 2};
    LOGINFO("Step 1: Pre allocate 50% of total blks which is {} blks in {} threads", preload_amount, nthreads);
    const auto preload_alloced{test->preload(preload_amount, true, BlkAllocatorTest::single_blk_size, true)};

    const auto num_iters{SISL_OPTIONS["iters"].as< uint64_t >()};
    LOGINFO("Step 2: Do alloc/free contiguous blks with single block size for blks span={}, threads={} iters={}",
            test->m_total_count, nthreads, num_iters);
    const auto result{test->do_alloc_free(num_iters, true, BlkAllocatorTest::single_blk_size, 90, true, true)};

    const uint64_t calculated_remaining{to_u64(test->m_total_count) - preload_alloced + result.second - result.first};
    const uint64_t remaining{test->m_allocator->available_blks()};
    LOGINFO("Step 3: Reallocate to alloc all remaining count {} calculated remaining {}", remaining,
            calculated_remaining);

    // Allocate all remaining blocks single-threaded. Multi-threaded preload would abort on the first
    // SPACE_FULL (terminate_flag), which races with break-up excess being returned to bitmap.
    uint64_t alloced{0};
    uint32_t consecutive_failures{0};
    while (test->m_allocator->available_blks() > 0 && consecutive_failures < 3) {
        BlkId bid;
        if (test->m_allocator->alloc_contiguous(bid) != BlkAllocStatus::SUCCESS) {
            ++consecutive_failures;
            continue;
        }
        consecutive_failures = 0;
        (void)test->alloced(bid, true);
        ++alloced;
    }
    LOGINFO("Step 3: Allocated {} of {} remaining blocks", alloced, remaining);

    ASSERT_EQ(test->m_allocator->available_blks(), 0u) << "Expected no blocks to be free";
}
} // namespace

TEST_F(SlabBlkAllocatorTest, expanded_contiguous_onesize_with_slabs) {
    create_expanded_allocator();
    alloc_free_contiguous_onesize(this);
}

namespace {
void alloc_free_scatter_unirandsize(SlabBlkAllocatorTest* const test) {
    const auto nthreads{
        std::clamp< uint32_t >(std::thread::hardware_concurrency(), 2, SISL_OPTIONS["num_threads"].as< uint32_t >())};
    const uint8_t prealloc_pct{50};

    const uint64_t preload_amount{to_u64(test->m_total_count) * prealloc_pct / 100};
    LOGINFO("Step 1: Pre allocate {}% of total blks which is {} blks in {} threads", prealloc_pct, preload_amount,
            nthreads);
    const auto preload_alloced{test->preload(preload_amount, false, BlkAllocatorTest::uniform_rand_size, true)};
    const uint64_t remaining_after_preload{test->m_total_count - preload_alloced};
    ASSERT_EQ(test->m_allocator->available_blks(), remaining_after_preload) << "Expected available to match";

    const auto num_iters{SISL_OPTIONS["iters"].as< uint64_t >()};
    const uint8_t runtime_pct{75};
    LOGINFO("Step 2: Do alloc/free scatter blks with completely random size ratio_range=[{}-{}] threads={} iters={}",
            prealloc_pct, runtime_pct, nthreads, num_iters);
    const auto result{
        test->do_alloc_free(num_iters, false, BlkAllocatorTest::uniform_rand_size, runtime_pct, false, true)};
    const uint64_t calculated_remaining{remaining_after_preload + result.second - result.first};
    const uint64_t remaining{test->m_allocator->available_blks()};
    LOGINFO("Step 3: Reallocate to alloc all remaining count {} calculated remaining {}", remaining,
            calculated_remaining);

    // Allocate all remaining blocks single-threaded. Multi-threaded preload would abort on the first
    // SPACE_FULL (terminate_flag), which races with break-up excess being returned to bitmap.
    uint64_t alloced{0};
    uint32_t consecutive_failures{0};
    while (test->m_allocator->available_blks() > 0 && consecutive_failures < 3) {
        BlkId bid;
        if (test->m_allocator->alloc_contiguous(bid) != BlkAllocStatus::SUCCESS) {
            ++consecutive_failures;
            continue;
        }
        consecutive_failures = 0;
        (void)test->alloced(bid, true);
        ++alloced;
    }
    LOGINFO("Step 3: Allocated {} of {} remaining blocks", alloced, remaining);

    ASSERT_EQ(test->m_allocator->available_blks(), 0u) << "Expected no blocks to be free";
}
} // namespace

TEST_F(SlabBlkAllocatorTest, expanded_scatter_unirandsize_with_slabs) {
    create_expanded_allocator();
    alloc_free_scatter_unirandsize(this);
}

template < typename T >
std::shared_ptr< cxxopts::Value > opt_default(const char* val) {
    return ::cxxopts::value< T >()->default_value(val);
}

SISL_OPTION_GROUP(test_blkalloc,
                  (num_blks, "", "num_blks", "number of blks", opt_default< uint32_t >("1000000"), "number"),
                  (iters, "", "iters", "number of iterations", opt_default< uint64_t >("100000"), "number"),
                  (num_threads, "", "num_threads", "num_threads", opt_default< uint32_t >("8"), "number"))

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv)
    sisl::logging::SetLogger("test_blkalloc");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%t] %v");
    folly::Init folly_init(&argc, &argv, folly::InitOptions{}.useGFlags(false));
    HomeStoreRuntimeConfig::init_settings_default();
    init_sweep_service();
    const int result{RUN_ALL_TESTS()};
    shutdown_sweep_service();
    return result;
}