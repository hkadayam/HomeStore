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

#include <cstdint>
#include <string>

#include <sisl/fds/buffer.h>
#include <nlohmann/json.hpp>

#include <homestore/homestore_decl.hpp>
#include <homestore/blk.h>
#include "common/homestore_config.hpp"
#include "common/homestore_assert.hpp"

namespace homestore {
#define BLKALLOC_LOG(level, msg, ...) HS_SUBMOD_LOG(level, blkalloc, , "blkalloc", get_name(), msg, ##__VA_ARGS__)
#define BLKALLOC_DBG_ASSERT(cond, msg, ...)                                                                            \
    HS_SUBMOD_ASSERT(DEBUG_ASSERT_FMT, cond, , "blkalloc", get_name(), msg, ##__VA_ARGS__)
#define BLKALLOC_REL_ASSERT(cond, msg, ...)                                                                            \
    HS_SUBMOD_ASSERT(RELEASE_ASSERT_FMT, cond, , "blkalloc", get_name(), msg, ##__VA_ARGS__)
#define BLKALLOC_LOG_ASSERT(cond, msg, ...)                                                                            \
    HS_SUBMOD_ASSERT(LOGMSG_ASSERT_FMT, cond, , "blkalloc", get_name(), msg, ##__VA_ARGS__)

#define BLKALLOC_REL_ASSERT_CMP(val1, cmp, val2, ...)                                                                  \
    HS_SUBMOD_ASSERT_CMP(RELEASE_ASSERT_CMP, val1, cmp, val2, , "blkalloc", get_name(), ##__VA_ARGS__)
#define BLKALLOC_DBG_ASSERT_CMP(val1, cmp, val2, ...)                                                                  \
    HS_SUBMOD_ASSERT_CMP(DEBUG_ASSERT_CMP, val1, cmp, val2, , "blkalloc", get_name(), ##__VA_ARGS__)
#define BLKALLOC_LOG_ASSERT_CMP(val1, cmp, val2, ...)                                                                  \
    HS_SUBMOD_ASSERT_CMP(LOGMSG_ASSERT_CMP, val1, cmp, val2, , "blkalloc", get_name(), ##__VA_ARGS__)

struct BlkAllocConfig {
    friend class BlkAllocator;

public:
    uint32_t blk_size_{0};
    uint32_t align_size_{0};
    blk_num_t capacity_{0};
    blk_num_t blks_per_portion_{0};
    bool persistent_{false};
    std::string unique_name_;

public:
    BlkAllocConfig() = default;
    BlkAllocConfig(uint32_t blk_size, uint32_t align_size, uint64_t size, bool persistent,
                   const std::string& name = "") :
            blk_size_{blk_size},
            align_size_{align_size},
            capacity_{static_cast< blk_num_t >(size / blk_size)},
            blks_per_portion_{std::min(HS_DYNAMIC_CONFIG(blkallocator.num_blks_per_portion), capacity_)},
            persistent_{persistent},
            unique_name_{name} {}

    BlkAllocConfig(BlkAllocConfig const&) = default;
    BlkAllocConfig(BlkAllocConfig&&) noexcept = delete;
    BlkAllocConfig& operator=(BlkAllocConfig const&) = default;
    BlkAllocConfig& operator=(BlkAllocConfig&&) noexcept = delete;
    virtual ~BlkAllocConfig() = default;

    virtual std::string to_string() const {
        return fmt::format("BlkSize={} TotalBlks={} BlksPerPortion={} persistent={}", in_bytes(blk_size_),
                           in_bytes(capacity_), blks_per_portion_, persistent_);
    }
};

///
/// BlkAllocator — abstract interface for all block allocators.
///
/// alloc/free:        allocate and release blocks.
/// commit:            mark a blkid as durably allocated in the persistent layer (no-op for inmem allocators).
///                    CP-safe: calls arriving while acquire_buffer() is held are buffered and replayed on
///                    release_buffer().
/// acquire_buffer:    serialize the current persistent bitmap into a ByteArray for a CP flush.
///                    New commits arriving while the buffer is held accumulate in an internal list.
/// release_buffer:    drain the accumulated commit list back into the persistent bitmap.
///
class BlkAllocator {
public:
    BlkAllocator(BlkAllocConfig const& cfg, chunk_num_t id = 0) :
            name_{cfg.unique_name_},
            blk_size_{cfg.blk_size_},
            align_size_{cfg.align_size_},
            num_blks_{cfg.capacity_},
            chunk_id_{id} {}
    BlkAllocator(BlkAllocator const&) = delete;
    BlkAllocator(BlkAllocator&&) noexcept = delete;
    BlkAllocator& operator=(BlkAllocator const&) = delete;
    BlkAllocator& operator=(BlkAllocator&&) noexcept = delete;
    virtual ~BlkAllocator() = default;

    virtual BlkAllocStatus alloc_contiguous(BlkId& bid) = 0;
    virtual BlkAllocStatus alloc(blk_count_t nblks, blk_alloc_hints const& hints, BlkIds& out_blkids) = 0;
    virtual void free(BlkId const& id) = 0;

    virtual BlkAllocStatus commit(BlkId const& bid) = 0;

    virtual blk_num_t available_blks() const = 0;
    virtual blk_num_t get_used_blks() const = 0;
    virtual bool is_blk_alloced(BlkId const& b, bool use_lock = false) const = 0;
    virtual void recovery_completed() = 0;
    virtual void reset() = 0;

    virtual std::string to_string() const = 0;
    virtual nlohmann::json get_status(int log_level) const = 0;

    /// RAII buffer handle returned by acquire_buffer().
    /// Holds the serialized bitmap ByteArray for a CP flush. On destruction the allocator's
    /// pending commit list is drained back into the bitmap.
    class BufferGuard {
    public:
        ~BufferGuard() { if (release_fn_) release_fn_(); }
        BufferGuard(BufferGuard&&) = default;            // std::function is empty after move → no-op dtor
        BufferGuard& operator=(BufferGuard&&) = delete;
        BufferGuard(BufferGuard const&) = delete;
        BufferGuard& operator=(BufferGuard const&) = delete;
        sisl::ByteArray const& buf() const { return buf_; }

    private:
        friend class BlkAllocator;
        BufferGuard(sisl::ByteArray buf, std::function< void() > release_fn) :
                buf_{std::move(buf)}, release_fn_{std::move(release_fn)} {}
        sisl::ByteArray buf_;
        std::function< void() > release_fn_;
    };

    virtual BufferGuard acquire_buffer() = 0;

    uint32_t get_align_size() const { return align_size_; }
    blk_num_t get_total_blks() const { return num_blks_; }
    const std::string& get_name() const { return name_; }
    uint32_t get_blk_size() const { return blk_size_; }

protected:
    static BufferGuard make_buffer_guard(sisl::ByteArray buf, std::function< void() > release_fn) {
        return BufferGuard{std::move(buf), std::move(release_fn)};
    }

    const std::string name_;
    const uint32_t blk_size_{0};
    const uint32_t align_size_{0};
    const blk_num_t num_blks_{0};
    const chunk_num_t chunk_id_{0};
};

} // namespace homestore
