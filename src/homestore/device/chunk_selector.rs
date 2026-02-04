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

use std::cell::Cell;
use std::sync::Arc;
use parking_lot::RwLock;

use iomgr::{iomgr, ReactorLocal};

use super::chunk::Chunk;
use super::device_metadata::VDevSizeType;
use crate::common::BlkAllocHints;

/// Chunk selector types
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChunkSelectorType {
    RoundRobin,
    Random,
    MostAvailableSpace,
    OnlyOne,
    Custom,
}

impl ChunkSelectorType {
    pub fn from_u8(val: u8) -> Self {
        match val {
            0 => ChunkSelectorType::RoundRobin,
            1 => ChunkSelectorType::Random,
            2 => ChunkSelectorType::MostAvailableSpace,
            3 => ChunkSelectorType::OnlyOne,
            4 => ChunkSelectorType::Custom,
            _ => ChunkSelectorType::RoundRobin,
        }
    }

    pub fn to_u8(self) -> u8 {
        self as u8
    }
}

/// ChunkSelectorInner trait - the actual selector implementation
///
/// ChunkSelectors are **immutable after construction** - all chunks are provided
/// at creation time. This design avoids locking overhead in the hot path (select_chunk).
///
/// For dynamic chunk addition: Create a new ChunkSelectorInner instance with the updated
/// chunk list and replace the old selector. This is rare (only during vdev expansion),
/// so the overhead is acceptable vs adding mutex contention to every allocation.
pub trait ChunkSelectorInner: Send + Sync {
    /// Select a chunk for allocation
    /// First attempt at allocation - uses selector's strategy (round-robin, most-available, etc.)
    /// Returns a reference to avoid Arc cloning (no atomic overhead!)
    fn select_chunk(&self, nblks: u32, hints: &BlkAllocHints) -> Option<&Arc<Chunk>>;

    /// Get the next chunk after the given chunk_id
    /// Used for retry after allocation failure on a specific chunk
    /// Returns a reference to avoid Arc cloning (no atomic overhead!)
    fn get_chunk_after(&self, last_chunk_id: u32) -> Option<&Arc<Chunk>>;

    /// Get total number of chunks managed by this selector
    fn total_chunks(&self) -> usize;
}

/// ChunkSelector - storage wrapper for chunk selector implementations
///
/// Provides either static (lock-free) or dynamic (RwLock-protected) access
/// to the underlying selector based on VDevSizeType.
pub enum ChunkSelector {
    /// Static vdev: chunk selector never changes after creation (no lock needed)
    /// Direct access, zero overhead on select operations
    Static(Option<Box<dyn ChunkSelectorInner>>),
    
    /// Dynamic vdev: chunk selector can be updated during expand() (needs RwLock)
    /// Minimal overhead on select operations (shared read lock)
    Dynamic(RwLock<Option<Box<dyn ChunkSelectorInner>>>),
}

impl ChunkSelector {
    /// Create new ChunkSelector based on VDevSizeType
    pub fn new(size_type: VDevSizeType) -> Self {
        match size_type {
            VDevSizeType::Static => Self::Static(None),
            VDevSizeType::Dynamic => Self::Dynamic(RwLock::new(None)),
        }
    }
    
    /// Select a chunk for allocation (HOT PATH)
    ///
    /// For Static: Direct call, zero overhead
    /// For Dynamic: Read lock (shared), minimal overhead
    pub fn select_chunk(&self, nblks: u32, hints: &BlkAllocHints) -> Option<&Arc<Chunk>> {
        match self {
            Self::Static(selector) => {
                selector.as_ref()?.select_chunk(nblks, hints)
            }
            Self::Dynamic(selector) => {
                let guard = selector.read();
                // SAFETY: We extend the lifetime of the reference returned from the guard.
                // This is safe because:
                // 1. The ChunkSelectorInner is heap-allocated (Box)
                // 2. It won't be freed while VirtualDev exists
                // 3. Even if replaced during expand(), old selector stays alive via Arc in chunks
                unsafe {
                    std::mem::transmute::<Option<&Arc<Chunk>>, Option<&Arc<Chunk>>>(
                        guard.as_ref()?.select_chunk(nblks, hints)
                    )
                }
            }
        }
    }
    
    /// Get next chunk after a failed allocation (HOT PATH)
    pub fn get_chunk_after(&self, last_chunk_id: u32) -> Option<&Arc<Chunk>> {
        match self {
            Self::Static(selector) => {
                selector.as_ref()?.get_chunk_after(last_chunk_id)
            }
            Self::Dynamic(selector) => {
                let guard = selector.read();
                // SAFETY: Same reasoning as select_chunk above
                unsafe {
                    std::mem::transmute::<Option<&Arc<Chunk>>, Option<&Arc<Chunk>>>(
                        guard.as_ref()?.get_chunk_after(last_chunk_id)
                    )
                }
            }
        }
    }
    
    /// Get total number of chunks
    pub fn total_chunks(&self) -> usize {
        match self {
            Self::Static(selector) => {
                selector.as_ref().map(|s| s.total_chunks()).unwrap_or(0)
            }
            Self::Dynamic(selector) => {
                selector.read().as_ref().map(|s| s.total_chunks()).unwrap_or(0)
            }
        }
    }
    
    /// Update the selector (called during initialization or expand)
    pub fn update(&mut self, selector: Box<dyn ChunkSelectorInner>) {
        match self {
            Self::Static(ref mut s) => {
                *s = Some(selector);
            }
            Self::Dynamic(s) => {
                *s.write() = Some(selector);
            }
        }
    }
    
    /// Check if this is a dynamic selector
    pub fn is_dynamic(&self) -> bool {
        matches!(self, Self::Dynamic(_))
    }
}

/// Round-robin chunk selector
///
/// Cycles through chunks in round-robin fashion using reactor-local indices.
/// Each reactor maintains its own index, avoiding contention across threads.
pub struct RoundRobinChunkSelector {
    chunks: Vec<Arc<Chunk>>,
    // Reactor-local index for round-robin selection (no mutex needed!)
    next_index: ReactorLocal<Cell<usize>>,
}

impl RoundRobinChunkSelector {
    /// Create a new round-robin chunk selector
    ///
    /// All chunks must be provided at construction time.
    /// The chunk list is immutable after construction.
    pub fn new(chunks: Vec<Arc<Chunk>>) -> Self {
        let num_reactors = iomgr::iomgr().num_reactors();
        Self {
            chunks,
            next_index: ReactorLocal::new(num_reactors, || Cell::new(0)),
        }
    }
}

impl ChunkSelectorInner for RoundRobinChunkSelector {
    fn select_chunk(&self, _nblks: u32, _hints: &BlkAllocHints) -> Option<&Arc<Chunk>> {
        if self.chunks.is_empty() {
            return None;
        }

        // Get reactor-local index
        let index_cell = self.next_index.get();
        let mut index = index_cell.get();

        // Wrap around if needed
        if index >= self.chunks.len() {
            index = 0;
        }

        // Advance for next call
        index_cell.set((index + 1) % self.chunks.len());

        // Return reference - no Arc clone!
        Some(&self.chunks[index])
    }

    fn get_chunk_after(&self, last_chunk_id: u32) -> Option<&Arc<Chunk>> {
        if self.chunks.is_empty() {
            return None;
        }

        // Find the chunk with last_chunk_id
        let last_pos = self.chunks.iter().position(|c| c.chunk_id() == last_chunk_id);

        match last_pos {
            Some(pos) => {
                // Return the next chunk (wrap around if at end)
                let next_pos = (pos + 1) % self.chunks.len();
                Some(&self.chunks[next_pos])
            }
            None => {
                // Chunk not found, return first chunk
                Some(&self.chunks[0])
            }
        }
    }

    fn total_chunks(&self) -> usize {
        self.chunks.len()
    }
}

/// Most-available-space chunk selector
///
/// Always selects the chunk with the most available space.
/// More expensive than round-robin but better for avoiding fragmentation.
pub struct MostAvailableSpaceSelector {
    chunks: Vec<Arc<Chunk>>,
}

impl MostAvailableSpaceSelector {
    pub fn new(chunks: Vec<Arc<Chunk>>) -> Self {
        Self { chunks }
    }
}

impl ChunkSelectorInner for MostAvailableSpaceSelector {
    fn select_chunk(&self, _nblks: u32, _hints: &BlkAllocHints) -> Option<&Arc<Chunk>> {
        if self.chunks.is_empty() {
            return None;
        }

        // Find chunk with most available blocks - returns reference directly
        self.chunks
            .iter()
            .max_by_key(|c| c.blk_allocator().available_blks())
    }

    fn get_chunk_after(&self, last_chunk_id: u32) -> Option<&Arc<Chunk>> {
        if self.chunks.is_empty() {
            return None;
        }

        // Find the chunk with last_chunk_id
        let last_pos = self.chunks.iter().position(|c| c.chunk_id() == last_chunk_id);

        match last_pos {
            Some(pos) => {
                // Return the next chunk (wrap around if at end)
                let next_pos = (pos + 1) % self.chunks.len();
                Some(&self.chunks[next_pos])
            }
            None => {
                // Chunk not found, return first chunk
                Some(&self.chunks[0])
            }
        }
    }

    fn total_chunks(&self) -> usize {
        self.chunks.len()
    }
}

/// Random chunk selector
///
/// Randomly selects a chunk on each allocation.
/// Uses reactor-local state for pseudo-random selection.
pub struct RandomChunkSelector {
    chunks: Vec<Arc<Chunk>>,
    // Reactor-local state for random selection (seed + counter)
    rng_state: ReactorLocal<Cell<u64>>,
}

impl RandomChunkSelector {
    pub fn new(chunks: Vec<Arc<Chunk>>) -> Self {
        use std::time::{SystemTime, UNIX_EPOCH};
        
        // Generate initial seed from system time
        let seed = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos() as u64;
        
        let num_reactors = iomgr::iomgr().num_reactors();
        Self {
            chunks,
            rng_state: ReactorLocal::new(num_reactors, move || Cell::new(seed)),
        }
    }
    
    /// Simple fast random number generator (xorshift64)
    fn next_random(&self) -> u64 {
        let state_cell = self.rng_state.get();
        let mut x = state_cell.get();
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_cell.set(x);
        x
    }
}

impl ChunkSelectorInner for RandomChunkSelector {
    fn select_chunk(&self, _nblks: u32, _hints: &BlkAllocHints) -> Option<&Arc<Chunk>> {
        if self.chunks.is_empty() {
            return None;
        }

        let random = self.next_random();
        let index = (random as usize) % self.chunks.len();
        Some(&self.chunks[index])
    }

    fn get_chunk_after(&self, last_chunk_id: u32) -> Option<&Arc<Chunk>> {
        if self.chunks.is_empty() {
            return None;
        }

        // Find the chunk with last_chunk_id
        let last_pos = self.chunks.iter().position(|c| c.chunk_id() == last_chunk_id);

        match last_pos {
            Some(pos) => {
                // Return the next chunk (wrap around if at end)
                let next_pos = (pos + 1) % self.chunks.len();
                Some(&self.chunks[next_pos])
            }
            None => {
                // Chunk not found, return first chunk
                Some(&self.chunks[0])
            }
        }
    }

    fn total_chunks(&self) -> usize {
        self.chunks.len()
    }
}

/// Only-one chunk selector
///
/// Optimized for the common case of exactly 1 chunk.
/// Avoids all random calculations, modulo operations, and indexing.
/// This is the most efficient selector and used by most homestore deployments.
pub struct OnlyOneChunkSelector {
    chunk: Arc<Chunk>,
}

impl OnlyOneChunkSelector {
    pub fn new(chunks: Vec<Arc<Chunk>>) -> Self {
        assert_eq!(chunks.len(), 1, "OnlyOneChunkSelector requires exactly 1 chunk, got {}", chunks.len());
        Self {
            chunk: chunks.into_iter().next().unwrap(),
        }
    }
}

impl ChunkSelectorInner for OnlyOneChunkSelector {
    #[inline]
    fn select_chunk(&self, _nblks: u32, _hints: &BlkAllocHints) -> Option<&Arc<Chunk>> {
        // True zero-cost: just return a reference (no atomic operations!)
        Some(&self.chunk)
    }

    #[inline]
    fn get_chunk_after(&self, _last_chunk_id: u32) -> Option<&Arc<Chunk>> {
        // Only one chunk, so retry always returns the same chunk
        Some(&self.chunk)
    }

    #[inline]
    fn total_chunks(&self) -> usize {
        1
    }
}

