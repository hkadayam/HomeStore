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

use std::sync::atomic::{AtomicU64, Ordering};
use bytes::Bytes;

#[cfg(feature = "sync_code")]
use parking_lot::RwLock;

#[cfg(feature = "async_code")]
use iomgr::AsyncRwLock as RwLock;

#[cfg(feature = "async_code")]
use async_trait::async_trait;

use super::types::{LogStoreError, LogStoreId, LogStoreSeqNum, Result};
use super::LogStore;

/// Number of shards for reducing write contention
/// Power of 2 for fast modulo via bitwise AND
const NUM_SHARDS: usize = 16;
const SHARD_MASK: u64 = (NUM_SHARDS - 1) as u64;

/// Shard containing logs and metadata
struct Shard {
    /// Storage for this shard
    logs: Vec<Option<Bytes>>,
    /// First valid sequence number for this shard (after truncation)
    start_seq: u64,
}

impl Shard {
    fn new(start_seq: u64) -> Self {
        Self {
            logs: Vec::new(),
            start_seq,
        }
    }
}

/// In-memory LogStore implementation using sharded Vec<Option<Bytes>>
///
/// This implementation provides:
/// - Append-only semantics (always true)
/// - Concurrent-safe operations via sharded RwLocks (reduces contention)
/// - Efficient truncation via Vec::drain (single memmove per shard)
/// - Efficient rollback via Vec::truncate (no memmove)
/// - Sharding design with Bytes reference-counting for high concurrency
///
/// Performance characteristics:
/// - Append: ~30-50ns with reduced contention (sharded write locks)
/// - Read: ~10-30ns (read lock + array index + refcount++)
/// - Truncate: O(n) memmove per shard, rare operation
/// - Rollback: O(1) tail drop per shard, no memmove
///
/// Sharding strategy:
/// - Route based on seq_num % NUM_SHARDS
/// - Each shard has independent RwLock protecting both Vec and start_seq
/// - Multiple writers can append to different shards concurrently
///
/// Locking design:
/// - Each shard's RwLock protects both logs Vec AND start_seq (simple!)
/// - next_seq is atomic (lock-free sequence allocation before shard lock)
/// - global_start_seq mirrors shards' start_seq for lock-free seq_bounds()
pub struct MemLogStore {
    /// Store identifier
    store_id: LogStoreId,

    /// Storage: Sharded Shard structs protected by RwLocks
    /// - Each shard contains Vec<Option<Bytes>> and start_seq
    /// - Bytes is reference-counted, so clone is cheap (~5-10ns)
    /// - Option allows sparse entries (None = gap or not yet written)
    /// - Sharding reduces write contention for concurrent appends
    shards: [RwLock<Shard>; NUM_SHARDS],

    /// Global start_seq mirror (for lock-free seq_bounds access)
    /// Updated atomically when truncate() completes
    global_start_seq: AtomicU64,

    /// Next sequence number to allocate (for append)
    /// Atomic for lock-free sequence allocation
    next_seq: AtomicU64,
}

impl MemLogStore {
    /// Create a new MemLogStore
    ///
    /// # Arguments
    /// * `store_id` - Identifier for this log store
    /// * `start_seq` - Initial sequence number to start from
    pub fn new(store_id: LogStoreId, start_seq: LogStoreSeqNum) -> Self {
        // Initialize shards array - each shard gets same initial start_seq
        let shards = std::array::from_fn(|_| RwLock::new(Shard::new(start_seq)));
        
        Self {
            store_id,
            shards,
            global_start_seq: AtomicU64::new(start_seq),
            next_seq: AtomicU64::new(start_seq),
        }
    }
    
    /// Get shard index for a given sequence number
    #[inline]
    fn shard_idx(seq_num: LogStoreSeqNum) -> usize {
        (seq_num & SHARD_MASK) as usize
    }

    /// Get an iterator over log entries starting from start_seq
    ///
    /// # Arguments
    /// * `start_seq` - Sequence number to start iteration from
    ///
    /// # Returns
    /// * `LogIter` - Iterator over (seq_num, Bytes) tuples
    ///
    /// Note: This creates a snapshot of the log at the time of call.
    /// Subsequent appends/truncates won't affect the iterator.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
    pub async fn iter(&self, start_seq: LogStoreSeqNum) -> LogIter {
        let store_next = self.next_seq.load(Ordering::Acquire);
        
        // Take snapshot of all shards
        #[cfg(feature = "async_code")]
        let shard_snapshots: Vec<Shard> = {
            let mut snapshots = Vec::with_capacity(NUM_SHARDS);
            for i in 0..NUM_SHARDS {
                let shard = self.shards[i].read().await;
                snapshots.push(Shard {
                    logs: shard.logs.clone(),
                    start_seq: shard.start_seq,
                });
            }
            snapshots
        };
        
        #[cfg(feature = "sync_code")]
        let shard_snapshots: Vec<Shard> = {
            let mut snapshots = Vec::with_capacity(NUM_SHARDS);
            for i in 0..NUM_SHARDS {
                let shard = self.shards[i].read();
                snapshots.push(Shard {
                    logs: shard.logs.clone(),
                    start_seq: shard.start_seq,
                });
            }
            snapshots
        };

        // Use first shard's start_seq as the global start
        let store_start = shard_snapshots.get(0).map(|s| s.start_seq).unwrap_or(start_seq);

        LogIter {
            shard_snapshots,
            current_seq: start_seq.max(store_start),
            end_seq: store_next,
        }
    }
}

#[cfg_attr(feature = "async_code", async_trait)]
#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
impl LogStore for MemLogStore {
    fn is_append_mode(&self) -> bool {
        true // MemLogStore is always append-only
    }

    async fn append(&self, data: Bytes) -> Result<LogStoreSeqNum> {
        let seq = self.next_seq.fetch_add(1, Ordering::AcqRel);

        // Route to shard based on sequence number
        let shard_idx = Self::shard_idx(seq);
        let mut shard = self.shards[shard_idx].write().await;

        // Calculate index within shard (start_seq protected by same lock)
        let global_idx = (seq - shard.start_seq) as usize;
        let shard_local_idx = global_idx / NUM_SHARDS;
        
        // Ensure capacity in this shard
        if shard_local_idx >= shard.logs.len() {
            shard.logs.resize(shard_local_idx + 1, None);
        }

        // Store (Bytes clone is cheap refcount++)
        shard.logs[shard_local_idx] = Some(data);

        Ok(seq)
    }

    async fn read(&self, seq_num: LogStoreSeqNum) -> Result<Bytes> {
        // Route to correct shard and acquire lock
        let shard_idx = Self::shard_idx(seq_num);
        let shard = self.shards[shard_idx].read().await;
        
        // Check truncation (start_seq protected by same shard lock - no race!)
        if seq_num < shard.start_seq {
            return Err(LogStoreError::Truncated(seq_num));
        }
        
        // Calculate index within shard
        let global_idx = (seq_num - shard.start_seq) as usize;
        let shard_local_idx = global_idx / NUM_SHARDS;

        shard.logs.get(shard_local_idx)
            .and_then(|opt| opt.as_ref())
            .map(|b| b.clone()) // Cheap refcount++
            .ok_or(LogStoreError::NotFound(seq_num))
    }

    async fn truncate(&self, upto_seq: LogStoreSeqNum) -> Result<()> {
        // CRITICAL: Acquire all shard write locks in order to prevent races
        // We use a fixed ordering (0..NUM_SHARDS) to prevent deadlocks
        #[cfg(feature = "async_code")]
        let mut shard_guards: Vec<_> = {
            let mut guards = Vec::with_capacity(NUM_SHARDS);
            for i in 0..NUM_SHARDS {
                guards.push(self.shards[i].write().await);
            }
            guards
        };
        
        #[cfg(feature = "sync_code")]
        let mut shard_guards: Vec<_> = {
            let mut guards = Vec::with_capacity(NUM_SHARDS);
            for i in 0..NUM_SHARDS {
                guards.push(self.shards[i].write());
            }
            guards
        };

        // Check if already truncated (use first shard's start_seq as representative)
        let start = shard_guards[0].start_seq;
        if upto_seq < start {
            return Ok(()); // Already truncated
        }

        // Calculate how many entries to remove from each shard
        let entries_to_remove = (upto_seq - start + 1) as usize;
        let full_rounds = entries_to_remove / NUM_SHARDS;
        let partial_shards = entries_to_remove % NUM_SHARDS;

        // Truncate each shard and update its start_seq
        for (shard_idx, shard) in shard_guards.iter_mut().enumerate() {
            let remove_from_shard = if shard_idx < partial_shards {
                full_rounds + 1
            } else {
                full_rounds
            };
            
            if remove_from_shard > 0 {
                if remove_from_shard <= shard.logs.len() {
                    shard.logs.drain(0..remove_from_shard);
                } else {
                    shard.logs.clear();
                }
            }
            
            // Update this shard's start_seq
            shard.start_seq = upto_seq + 1;
        }

        // Update global mirror for lock-free seq_bounds() access
        self.global_start_seq.store(upto_seq + 1, Ordering::Release);

        Ok(())
    }

    async fn rollback(&self, to_seq: LogStoreSeqNum) -> Result<()> {
        let next = self.next_seq.load(Ordering::Acquire);

        // CRITICAL: Acquire all shard write locks in order to prevent races
        #[cfg(feature = "async_code")]
        let mut shard_guards: Vec<_> = {
            let mut guards = Vec::with_capacity(NUM_SHARDS);
            for i in 0..NUM_SHARDS {
                guards.push(self.shards[i].write().await);
            }
            guards
        };
        
        #[cfg(feature = "sync_code")]
        let mut shard_guards: Vec<_> = {
            let mut guards = Vec::with_capacity(NUM_SHARDS);
            for i in 0..NUM_SHARDS {
                guards.push(self.shards[i].write());
            }
            guards
        };

        // Check bounds (use first shard's start_seq as representative)
        let start = shard_guards[0].start_seq;
        if to_seq >= next - 1 || to_seq < start {
            return Err(LogStoreError::InvalidSeq(to_seq));
        }

        // Now safe to update next_seq (all shards locked, no concurrent appends)
        self.next_seq.store(to_seq + 1, Ordering::Release);

        // Calculate new length for each shard
        let total_entries = (to_seq - start + 1) as usize;
        let full_rounds = total_entries / NUM_SHARDS;
        let partial_shards = total_entries % NUM_SHARDS;

        // Truncate each shard from the back
        for (shard_idx, shard) in shard_guards.iter_mut().enumerate() {
            let new_shard_len = if shard_idx < partial_shards {
                full_rounds + 1
            } else {
                full_rounds
            };
            
            shard.logs.truncate(new_shard_len);
        }

        Ok(())
    }

    fn seq_bounds(&self) -> (LogStoreSeqNum, LogStoreSeqNum) {
        // Use global mirror for lock-free access
        (
            self.global_start_seq.load(Ordering::Acquire),
            self.next_seq.load(Ordering::Acquire),
        )
    }

    fn store_id(&self) -> LogStoreId {
        self.store_id
    }
}

/// Iterator over log entries (snapshot-based, sharded)
pub struct LogIter {
    shard_snapshots: Vec<Shard>,
    current_seq: LogStoreSeqNum,
    end_seq: LogStoreSeqNum,
}

impl Iterator for LogIter {
    type Item = (LogStoreSeqNum, Bytes);

    fn next(&mut self) -> Option<Self::Item> {
        while self.current_seq < self.end_seq {
            let seq = self.current_seq;
            self.current_seq += 1;
            
            // Route to correct shard
            let shard_idx = (seq & SHARD_MASK) as usize;
            
            if let Some(shard) = self.shard_snapshots.get(shard_idx) {
                // Check if this seq is in range for this shard
                if seq < shard.start_seq {
                    continue; // Truncated
                }
                
                let global_idx = (seq - shard.start_seq) as usize;
                let shard_local_idx = global_idx / NUM_SHARDS;
                
                if let Some(Some(ref bytes)) = shard.logs.get(shard_local_idx) {
                    return Some((seq, bytes.clone())); // Cheap refcount++
                }
            }
        }
        None
    }
}
