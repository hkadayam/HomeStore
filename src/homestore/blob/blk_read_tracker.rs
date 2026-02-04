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

use std::sync::Arc;
use dashmap::DashMap;
use futures::channel::oneshot;
use crate::common::blk::BlkId;

/// Waiter structure that signals completion via a oneshot channel
/// 
/// When the BlkTrackWaiter is dropped (RAII pattern), it sends a signal through
/// the channel. Multiple aligned ranges can share the same waiter via Arc, and
/// the signal is only sent when the last Arc reference is dropped.
pub struct BlkTrackWaiter {
    sender: Option<oneshot::Sender<()>>,
}

impl BlkTrackWaiter {
    /// Create a new waiter with the given channel sender
    pub fn new(sender: oneshot::Sender<()>) -> Self {
        Self {
            sender: Some(sender),
        }
    }
}

impl Drop for BlkTrackWaiter {
    fn drop(&mut self) {
        if let Some(sender) = self.sender.take() {
            // Send completion signal (ignore error if receiver was dropped)
            let _ = sender.send(());
        }
    }
}

/// Track record for a base-aligned block ID
/// 
/// Stores reference count and list of waiters for blocks that fall within
/// the same aligned range.
#[derive(Clone)]
struct BlkTrackRecord {
    /// The base-aligned block ID (key)
    key: BlkId,
    /// Reference count (number of concurrent reads)
    ref_cnt: i64,
    /// List of waiters waiting for this block range to become available
    waiters: Vec<Arc<BlkTrackWaiter>>,
}

impl BlkTrackRecord {
    fn new(key: BlkId) -> Self {
        Self {
            key,
            ref_cnt: 0,
            waiters: Vec::with_capacity(8), // Small vector optimization similar to folly::small_vector
        }
    }
}

/// Block Read Tracker
/// 
/// Tracks concurrent reads on block IDs to ensure safe deletion and modification.
/// Uses a concurrent hash map (DashMap) for lock-free reads and per-shard locking for writes.
/// 
/// # Examples
/// 
/// ```ignore
/// let tracker = BlkReadTracker::new();
/// 
/// // Mark blocks as being read
/// tracker.insert(&blkid);
/// 
/// // ... perform read operation ...
/// 
/// // Mark read as complete
/// tracker.remove(&blkid);
/// 
/// // Wait for all reads on a block to complete before freeing
/// tracker.wait_on(&blkid).await?;
/// println!("All reads completed, safe to free block");
/// free_block(&blkid);
/// ```
pub struct BlkReadTracker {
    /// Concurrent hash map of pending reads, keyed by base-aligned BlkId
    pending_reads_map: DashMap<BlkId, BlkTrackRecord>,
    /// Number of entries per record (alignment factor)
    entries_per_record: u16,
}

impl BlkReadTracker {
    const EXPECTED_NUM_RECORDS: usize = 1000;
    const DEFAULT_ENTRIES_PER_RECORD: u16 = 8;

    /// Create a new BlkReadTracker with default settings
    pub fn new() -> Self {
        Self {
            pending_reads_map: DashMap::with_capacity(Self::EXPECTED_NUM_RECORDS),
            entries_per_record: Self::DEFAULT_ENTRIES_PER_RECORD,
        }
    }

    /// Get the number of entries per record (alignment factor)
    pub fn entries_per_record(&self) -> u16 {
        self.entries_per_record
    }

    /// Set the number of entries per record (alignment factor)
    /// 
    /// This affects how block IDs are aligned into base records.
    pub fn set_entries_per_record(&mut self, num_entries: u16) {
        self.entries_per_record = num_entries;
    }

    /// Insert a block ID into the read tracker
    /// 
    /// Increments the reference count for the given block ID, indicating
    /// that a read operation is in progress.
    /// 
    /// # Arguments
    /// * `blkid` - The block ID being read
    pub fn insert(&self, blkid: &BlkId) {
        self.merge(blkid, 1, None);
    }

    /// Remove a block ID from the read tracker
    /// 
    /// Decrements the reference count for the given block ID. If the reference
    /// count reaches zero, any waiters on this block will be notified.
    /// 
    /// # Arguments
    /// * `blkid` - The block ID that finished reading
    pub fn remove(&self, blkid: &BlkId) {
        self.merge(blkid, -1, None);
    }

    /// Wait for all reads on the given block ID to complete
    /// 
    /// Returns a future that resolves immediately if no reads are pending (fast path),
    /// or waits for all reads to complete (slow path). Multiple callers can wait on
    /// the same block concurrently - each gets their own awaitable future.
    /// 
    /// For blocks spanning multiple aligned ranges, the future resolves only when
    /// ALL ranges are free.
    /// 
    /// # Arguments
    /// * `blkid` - The block ID to wait on
    /// 
    /// # Returns
    /// A future that completes when all reads on the block are done
    /// 
    /// # Examples
    /// 
    /// ```ignore
    /// // Wait for block to be free before freeing it
    /// tracker.wait_on(&blkid).await?;
    /// free_block(&blkid);
    /// ```
    pub fn wait_on(&self, blkid: &BlkId) -> impl std::future::Future<Output = Result<(), oneshot::Canceled>> {
        use futures::future::{Either, ready};
        
        // Fast path: check if already free (zero allocation)
        if self.is_free(blkid) {
            return Either::Left(ready(Ok(())));
        }
        
        // Slow path: create channel and wait
        let (tx, rx) = oneshot::channel();
        let waiter = Arc::new(BlkTrackWaiter::new(tx));
        self.merge(blkid, 0, Some(waiter));
        Either::Right(rx)
    }
    
    /// Fast check if a block is free (no pending reads)
    /// 
    /// This performs lock-free reads to check all aligned ranges the block spans.
    fn is_free(&self, blkid: &BlkId) -> bool {
        let entries_per_record = self.entries_per_record as u32;
        let cur_base_blk_num = (blkid.blk_num() / entries_per_record) * entries_per_record;
        let last_base_blk_num = ((blkid.blk_num() + blkid.blk_count() as u32 - 1) / entries_per_record) * entries_per_record;
        
        let mut current = cur_base_blk_num;
        while current <= last_base_blk_num {
            let base_blkid = BlkId::new(current, self.entries_per_record, blkid.chunk_num());
            
            // Check if this range has pending reads
            if let Some(entry) = self.pending_reads_map.get(&base_blkid) {
                if entry.ref_cnt > 0 {
                    return false; // Still has pending reads
                }
            }
            // No entry means no pending reads for this range
            
            current += entries_per_record;
        }
        
        true // All ranges are free
    }

    /// Core merge operation that handles insert, remove, and wait_on
    /// 
    /// # Arguments
    /// * `blkid` - The block ID to operate on
    /// * `new_ref_count` - Change in reference count (positive for insert, negative for remove, 0 for wait)
    /// * `waiter` - Optional waiter to add (only for wait_on operations, Arc-wrapped for multi-range support)
    fn merge(&self, blkid: &BlkId, new_ref_count: i64, waiter: Option<Arc<BlkTrackWaiter>>) {
        debug_assert!(
            (new_ref_count != 0 && waiter.is_none()) || (new_ref_count == 0 && waiter.is_some()),
            "Invalid waiter: new_ref_count={}, waiter is_some={}", 
            new_ref_count, 
            waiter.is_some()
        );

        // Calculate aligned base block numbers
        let entries_per_record = self.entries_per_record as u32;
        let cur_base_blk_num = (blkid.blk_num() / entries_per_record) * entries_per_record;
        let last_base_blk_num = ((blkid.blk_num() + blkid.blk_count() as u32 - 1) / entries_per_record) * entries_per_record;

        let mut current = cur_base_blk_num;
        while current <= last_base_blk_num {
            let base_blkid = BlkId::new(current, self.entries_per_record, blkid.chunk_num());

            if new_ref_count > 0 {
                // Insert operation - upsert or update
                self.pending_reads_map
                    .entry(base_blkid)
                    .and_modify(|rec| rec.ref_cnt += new_ref_count)
                    .or_insert_with(|| {
                        let mut rec = BlkTrackRecord::new(base_blkid);
                        rec.ref_cnt = new_ref_count;
                        rec
                    });
            } else if new_ref_count < 0 {
                // Remove operation - decrement and potentially delete
                if let Some(mut entry) = self.pending_reads_map.get_mut(&base_blkid) {
                    debug_assert!(entry.ref_cnt > 0, 
                        "Decrement a ref count (blk: {:?}) which does not exist or is already zero", 
                        base_blkid);
                    
                    entry.ref_cnt += new_ref_count;
                    
                    if entry.ref_cnt == 0 {
                        // Need to trigger waiters and remove entry
                        let waiters = std::mem::take(&mut entry.waiters);
                        drop(entry); // Release lock before removing
                        
                        self.pending_reads_map.remove(&base_blkid);
                        
                        // Waiters will be dropped here, triggering their callbacks
                        drop(waiters);
                    }
                }
            } else {
                // Wait operation - add waiter
                if let Some(mut entry) = self.pending_reads_map.get_mut(&base_blkid) {
                    entry.waiters.push(waiter.clone().unwrap());
                }
                // If no record is found for a wait-on operation, it means no one is holding
                // a reference for this waiter and the callback will be called automatically
                // when the waiter is dropped at the end of this function
            }

            current += entries_per_record;
        }
    }

    /// Get the size of the pending reads map
    pub fn size(&self) -> usize {
        self.pending_reads_map.len()
    }
}

impl Default for BlkReadTracker {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_insert_remove() {
        let tracker = BlkReadTracker::new();
        let blkid = BlkId::new(0, 1, 0);

        // Insert should create a record
        tracker.insert(&blkid);
        assert_eq!(tracker.size(), 1);

        // Remove should delete the record
        tracker.remove(&blkid);
        assert_eq!(tracker.size(), 0);
    }

    #[test]
    fn test_multiple_refs() {
        let tracker = BlkReadTracker::new();
        let blkid = BlkId::new(0, 1, 0);

        // Multiple inserts should increment ref count
        tracker.insert(&blkid);
        tracker.insert(&blkid);
        assert_eq!(tracker.size(), 1);

        // First remove should keep the record
        tracker.remove(&blkid);
        assert_eq!(tracker.size(), 1);

        // Second remove should delete the record
        tracker.remove(&blkid);
        assert_eq!(tracker.size(), 0);
    }

    #[iomgr::iomanager_test]
    async fn test_wait_on_no_pending_reads() {
        let tracker = BlkReadTracker::new();
        let blkid = BlkId::new(0, 1, 0);

        // Should complete immediately since no pending reads (fast path)
        tracker.wait_on(&blkid).await.expect("Wait should succeed");
    }

    #[iomgr::iomanager_test]
    async fn test_wait_on_with_pending_reads() {
        let tracker = Arc::new(BlkReadTracker::new());
        let blkid = BlkId::new(0, 1, 0);

        // Insert a read
        tracker.insert(&blkid);

        // Create a wait future
        let wait_future = tracker.wait_on(&blkid);
        
        // Remove the read immediately - this should trigger any waiters
        tracker.remove(&blkid);
        
        // Now await should complete
        wait_future.await.expect("Wait should succeed");
    }

    #[test]
    fn test_alignment() {
        let mut tracker = BlkReadTracker::new();
        tracker.set_entries_per_record(16);

        // Block 17 should align to base 16
        let blkid1 = BlkId::new(17, 1, 0);
        tracker.insert(&blkid1);

        // Block 20 should also align to base 16
        let blkid2 = BlkId::new(20, 1, 0);
        tracker.insert(&blkid2);

        // Should only have 1 record (both aligned to same base)
        assert_eq!(tracker.size(), 1);
    }

    #[iomgr::iomanager_test]
    async fn test_spanning_aligned_ranges() {
        let mut tracker = BlkReadTracker::new();
        tracker.set_entries_per_record(16);
        
        // Block spans multiple aligned ranges: 10-25 crosses base 0, 16
        let blkid = BlkId::new(10, 16, 0);
        
        // Insert a read on this spanning block
        tracker.insert(&blkid);
        
        let tracker_arc = Arc::new(tracker);
        
        // Create wait future
        let wait_future = tracker_arc.wait_on(&blkid);
        
        // Remove the read - should unblock the waiter
        tracker_arc.remove(&blkid);
        
        // Wait should complete
        wait_future.await.expect("Wait should succeed");
    }
}

