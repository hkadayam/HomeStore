/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/
#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <random>
#include <vector>

#include "sisl/fds/enum.h"
#include "homestore/blk.h" // blk_count_t, blk_alloc_hints

namespace homestore {

class Chunk;

// ── ChunkSelectorType ─────────────────────────────────────────────────────────
// Mirrors Rust's ChunkSelectorType enum in chunk_selector.rs.
VENUM(ChunkSelectorType, uint8_t, RoundRobin = 0, Random = 1, MostAvailableSpace = 2, OnlyOne = 3, Custom = 4);

// ── IChunkSelector ────────────────────────────────────────────────────────────
// Abstract interface for chunk selection strategies.
// Implementations are immutable after construction; VirtualDev atomically
// replaces the instance on expand() via std::atomic_store.
// Mirrors Rust's ChunkSelectorInner trait.
class IChunkSelector {
public:
    virtual ~IChunkSelector() = default;

    /// Pick a chunk for a new allocation of `nblks` blocks.
    virtual std::shared_ptr< Chunk > select_chunk(blk_count_t nblks, const blk_alloc_hints& hints) const = 0;

    /// Return a different chunk to retry after `last_chunk_id` failed.
    /// Returns nullptr if no alternative exists.
    virtual std::shared_ptr< Chunk > get_chunk_after(uint32_t last_chunk_id) const = 0;

    /// Total number of chunks this selector manages.
    virtual size_t total_chunks() const = 0;
};

// ── Concrete implementations ──────────────────────────────────────────────────

/// Optimisation for single-chunk vdevs (most common in homestore deployments).
/// Mirrors Rust's OnlyOneChunkSelector.
class OnlyOneChunkSelector final : public IChunkSelector {
public:
    explicit OnlyOneChunkSelector(std::vector< std::shared_ptr< Chunk > > chunks) {
        assert(!chunks.empty());
        chunk_ = std::move(chunks[0]);
    }

    std::shared_ptr< Chunk > select_chunk(blk_count_t, const blk_alloc_hints&) const override {
        return chunk_;
    }
    std::shared_ptr< Chunk > get_chunk_after(uint32_t) const override {
        return nullptr; // no other chunk to retry with
    }
    size_t total_chunks() const override { return 1; }

private:
    std::shared_ptr< Chunk > chunk_;
};

/// Round-robin selection across all chunks.
/// Mirrors Rust's RoundRobinChunkSelector.
class RoundRobinChunkSelector final : public IChunkSelector {
public:
    explicit RoundRobinChunkSelector(std::vector< std::shared_ptr< Chunk > > chunks)
            : chunks_{std::move(chunks)} {}

    std::shared_ptr< Chunk > select_chunk(blk_count_t, const blk_alloc_hints&) const override {
        if (chunks_.empty()) { return nullptr; }
        const size_t idx = next_idx_.fetch_add(1, std::memory_order_relaxed) % chunks_.size();
        return chunks_[idx];
    }
    std::shared_ptr< Chunk > get_chunk_after(uint32_t last_chunk_id) const override {
        return find_next_after(last_chunk_id);
    }
    size_t total_chunks() const override { return chunks_.size(); }

private:
    std::shared_ptr< Chunk > find_next_after(uint32_t last_id) const {
        for (size_t i = 0; i < chunks_.size(); ++i) {
            if (chunks_[i]->chunk_id() == last_id) {
                return chunks_[(i + 1) % chunks_.size()];
            }
        }
        return chunks_.empty() ? nullptr : chunks_[0];
    }

    std::vector< std::shared_ptr< Chunk > > chunks_;
    mutable std::atomic< size_t >           next_idx_{0};
};

/// Uniform random selection across all chunks.
/// Mirrors Rust's RandomChunkSelector.
class RandomChunkSelector final : public IChunkSelector {
public:
    explicit RandomChunkSelector(std::vector< std::shared_ptr< Chunk > > chunks)
            : chunks_{std::move(chunks)}, rng_{std::random_device{}()} {}

    std::shared_ptr< Chunk > select_chunk(blk_count_t, const blk_alloc_hints&) const override {
        if (chunks_.empty()) { return nullptr; }
        std::uniform_int_distribution< size_t > dist{0, chunks_.size() - 1};
        return chunks_[dist(rng_)];
    }
    std::shared_ptr< Chunk > get_chunk_after(uint32_t last_chunk_id) const override {
        return round_robin_after(last_chunk_id);
    }
    size_t total_chunks() const override { return chunks_.size(); }

private:
    std::shared_ptr< Chunk > round_robin_after(uint32_t last_id) const {
        for (size_t i = 0; i < chunks_.size(); ++i) {
            if (chunks_[i]->chunk_id() == last_id) {
                return chunks_[(i + 1) % chunks_.size()];
            }
        }
        return chunks_.empty() ? nullptr : chunks_[0];
    }

    std::vector< std::shared_ptr< Chunk > > chunks_;
    mutable std::mt19937                    rng_;
};

/// Picks the chunk with the most available free blocks.
/// Mirrors Rust's MostAvailableSpaceSelector.
class MostAvailableSpaceSelector final : public IChunkSelector {
public:
    explicit MostAvailableSpaceSelector(std::vector< std::shared_ptr< Chunk > > chunks)
            : chunks_{std::move(chunks)} {}

    std::shared_ptr< Chunk > select_chunk(blk_count_t, const blk_alloc_hints&) const override {
        if (chunks_.empty()) { return nullptr; }
        // TODO: use chunk->blk_allocator()->available_blks() once blkalloc ported.
        return chunks_[0];
    }
    std::shared_ptr< Chunk > get_chunk_after(uint32_t last_chunk_id) const override {
        for (size_t i = 0; i < chunks_.size(); ++i) {
            if (chunks_[i]->chunk_id() == last_chunk_id) {
                return chunks_[(i + 1) % chunks_.size()];
            }
        }
        return chunks_.empty() ? nullptr : chunks_[0];
    }
    size_t total_chunks() const override { return chunks_.size(); }

private:
    std::vector< std::shared_ptr< Chunk > > chunks_;
};

} // namespace homestore
