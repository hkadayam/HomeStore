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
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sisl/metrics/metrics.h"
#include "sisl/logging/logging.h"
#include "sisl/fds/bitset.h"

#include "homestore/base/blk.h"
#include "bitmap_blk_allocator.h"
#include "sweep_service.h"
#include "segment_manager.h"
#include "homestore/base/homestore_assert.h"
#include "homestore/base/homestore_config.h"

namespace homestore {
namespace blkalloc {

enum class AllocMode : uint8_t {
    CompactAlloc,  // entire capacity fits in slab; no inmem_bm_; no sweep registration; ondisk_bm_ when persistent
    ExpandedAlloc, // large capacity; slab cache backed by inmem_bm_ + ondisk_bm_; registers with sweep service
};

// Per-slab cache sizing: max entries and refill-trigger threshold.
struct SlabConfig {
    blk_count_t slab_size{0};
    blk_num_t max_entries{0};
    float refill_threshold_pct{25.0f};
};

struct SlabBlkAllocConfig : public BlkAllocConfig {
    uint32_t phys_page_size_{4096};
    uint32_t num_segments_{1};
    bool use_slab_cache_{true}; // when false, alloc goes straight to bitmap scan; free never injects into slab
    AllocMode alloc_mode{AllocMode::ExpandedAlloc};
    std::array< SlabConfig, SlabCache::NUM_SLABS > slab_cfgs_{};

    SlabBlkAllocConfig() : SlabBlkAllocConfig{0, 0, 0, 0, false, ""} {}
    explicit SlabBlkAllocConfig(std::string const& name) : SlabBlkAllocConfig{0, 0, 0, 0, false, name} {}

    SlabBlkAllocConfig(uint32_t blk_size, uint32_t ppage_sz, uint32_t align_sz, uint64_t size, bool persistent,
                       std::string const& name) :
            BlkAllocConfig{blk_size, align_sz, size, persistent, name},
            phys_page_size_{ppage_sz},
            num_segments_{HS_DYNAMIC_CONFIG(blkallocator.max_segments)} {
        const blk_num_t total_cache_blks = static_cast< blk_num_t >(
            HS_DYNAMIC_CONFIG(blkallocator.free_blk_cache_count_by_vdev_percent) * capacity_ / 100.0);
        const blk_num_t num_portions = std::max< blk_num_t >((capacity_ - 1) / blks_per_portion_ + 1, 1u);
        const float refill_pct = HS_DYNAMIC_CONFIG(blkallocator.slab_refill_threshold_pct);

        // Auto-populate slab distribution from defaults if not configured. Flatbuffers does not allow
        // vector defaults in the schema, so the canonical default lives in
        // HomeStoreDynamicConfig::default_slab_distribution() and we copy it into the settings factory
        // here on first use.
        auto const& dist = HS_DYNAMIC_CONFIG(blkallocator.slab_distribution);
        if (dist.empty()) {
            HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
                auto& slab_pct_dist = s.blkallocator.slab_distribution;
                if (slab_pct_dist.empty()) {
                    auto const& defaults = HomeStoreDynamicConfig::default_slab_distribution();
                    slab_pct_dist.insert(slab_pct_dist.begin(), defaults.begin(), defaults.end());
                }
            });
            HS_SETTINGS_FACTORY().save();
        }

        slab_idx_t idx{0};
        for (auto const& pct : HS_DYNAMIC_CONFIG(blkallocator.slab_distribution)) {
            if (idx >= SlabCache::NUM_SLABS)
                break;
            auto& sc = slab_cfgs_[idx];
            sc.slab_size = static_cast< blk_count_t >(1) << idx;
            const blk_num_t total_entries = static_cast< blk_num_t >((total_cache_blks / sc.slab_size) * (pct / 100.0));
            sc.max_entries = total_entries / std::max< blk_num_t >(num_portions, 1u);
            sc.refill_threshold_pct = refill_pct;
            ++idx;
        }
    }

    SlabBlkAllocConfig(SlabBlkAllocConfig const&) = default;
    SlabBlkAllocConfig(SlabBlkAllocConfig&&) noexcept = delete;
    SlabBlkAllocConfig& operator=(SlabBlkAllocConfig const&) = delete;
    SlabBlkAllocConfig& operator=(SlabBlkAllocConfig&&) noexcept = delete;
    ~SlabBlkAllocConfig() override = default;

    blk_num_t get_blks_per_phys_page() const { return phys_page_size_ / blk_size_; }
    blk_num_t max_cache_blks_per_portion() const {
        blk_num_t total{0};
        for (auto const& sc : slab_cfgs_) {
            total += sc.max_entries * sc.slab_size;
        }
        return total;
    }

    std::string to_string() const override {
        return fmt::format("{} PhysPageSize={} NumSegments={} MaxCachePerPortion={}", BlkAllocConfig::to_string(),
                           phys_page_size_, num_segments_, max_cache_blks_per_portion());
    }
};

class BlkAllocMetrics : public sisl::MetricsGroup {
public:
    explicit BlkAllocMetrics(const char* inst_name) : sisl::MetricsGroup("BlkAlloc", inst_name) {
        REGISTER_COUNTER(num_alloc, "Number of blks alloc attempts");
        REGISTER_COUNTER(num_alloc_failure, "Number of blk alloc failures");
        REGISTER_COUNTER(num_alloc_partial, "Number of blk alloc partial allocations");
        REGISTER_COUNTER(num_retries, "Number of times it retried because of empty cache");
        REGISTER_COUNTER(num_blks_alloc_direct, "Number of blks alloc directly from bitmap");
        REGISTER_HISTOGRAM(frag_pct_distribution, "Distribution of fragmentation percentage");

        register_me_to_farm();
    }

    BlkAllocMetrics(BlkAllocMetrics const&) = delete;
    BlkAllocMetrics(BlkAllocMetrics&&) noexcept = delete;
    BlkAllocMetrics& operator=(BlkAllocMetrics const&) = delete;
    BlkAllocMetrics& operator=(BlkAllocMetrics&&) noexcept = delete;
    ~BlkAllocMetrics() { deregister_me_from_farm(); }
};

///
/// SlabBlkAllocator — segment/portion-based allocator with per-portion slab caches.
///
/// Owns one SegmentManager (portion layout + per-portion slab caches + sweep cursors).
/// Contains:
///   inmem_bm_   — BitmapBlkAllocator: in-memory free/used state. Null for CompactAlloc.
///   ondisk_bm_  — BitmapBlkAllocator: durable state; null if !persistent.
///
/// Persistence is entirely the caller's responsibility. The caller:
///   - constructs with a IoBufShared (from the meta service) for recovery, or nullopt for a fresh start.
///   - calls acquire_buffer() around the CP flush; the returned BufferGuard holds the serialized
///     IoBufShared and releases the ondisk commit buffer on destruction.
///   - stores the IoBufShared (via guard.buf()) through whichever meta service it chooses.
///
/// Alloc path: slab_cache → fill_cache → inmem_bm_.alloc() direct scan.
/// Free path:  CompactAlloc→slab try_free; ExpandedAlloc→slab try_free + bitmap for remainder.
/// Commit:     ondisk_bm_.commit() — CP-safe set in the ondisk bitmap.
///
class SlabBlkAllocator : public BlkAllocator {
public:
    // buf: nullopt for a fresh allocator; a serialized IoBufShared (from the meta service) for recovery.
    SlabBlkAllocator(SlabBlkAllocConfig const& cfg, std::optional< sisl::IoBufShared > buf, chunk_num_t chunk_id);
    SlabBlkAllocator(SlabBlkAllocator const&) = delete;
    SlabBlkAllocator(SlabBlkAllocator&&) noexcept = delete;
    SlabBlkAllocator& operator=(SlabBlkAllocator const&) = delete;
    SlabBlkAllocator& operator=(SlabBlkAllocator&&) noexcept = delete;
    ~SlabBlkAllocator() override;

    BlkAllocStatus alloc_contiguous(BlkId& bid) override;
    BlkAllocStatus alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkIds& out_blkids) override;
    void free(BlkId const& bid) override;

    // During recovery: commits into both ondisk_bm_ and inmem_bm_.
    // After recovery_completed(): only commits into ondisk_bm_; debug-asserts block is already in inmem_bm_.
    BlkAllocStatus commit(BlkId const& bid) override;

    // Delegates to ondisk_bm_; returns empty no-op guard if !persistent.
    BufferGuard acquire_buffer() override;

    blk_num_t available_blks() const override;
    blk_num_t get_used_blks() const override;
    bool is_blk_alloced(BlkId const& b, bool use_lock = false) const override;
    bool is_blk_alloced_on_disk(BlkId const& b, bool use_lock = false) const;
    std::string to_string() const override;
    void reset() override {}
    void recovery_completed() override;
    nlohmann::json get_status(int log_level) const override;

private:
    // Populates slab caches from ondisk_bm_ (persistent) or all blocks (non-persistent). Called from constructor.
    void load();

    // Uses BitmapBlkAllocator::scan_free_blks() to fill a portion's slab cache from inmem_bm_.
    // Used as the refill callback registered with blkalloc::SweepService.
    void fill_cache_for_portion(InmemPortion& portion);

    SlabBlkAllocConfig cfg_;
    SegmentManager seg_mgr_;
    unique< BitmapBlkAllocator > inmem_bm_;  // null for CompactAlloc
    unique< BitmapBlkAllocator > ondisk_bm_; // null if !persistent
    BlkAllocMetrics metrics_;
    // RCU-protected recovery flag: non-null while recovery is in progress, null after recovery_completed().
    bool* recovering_{nullptr};

    std::atomic< int64_t > alloced_blk_count_{0};

    // Registration with the module-scoped sweep service; null for CompactAlloc (no background refill needed).
    // Destroying the handle drains any in-flight refill workers targeting this allocator's portions.
    std::shared_ptr< SweepService::AllocatorHandle > sweep_handle_;
};

} // namespace blkalloc
} // namespace homestore
