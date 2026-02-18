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
 ************************************************************************* */

use std::sync::atomic::{AtomicUsize, Ordering};

use iomgr::AsyncRwLock;
use serde_json::{json, Value as JsonValue};

use crate::bitset::{AtomicBitset, NPOS};

/// Status information for a stream tracker entry
#[derive(Debug, Clone, PartialEq)]
pub struct StreamStatus {
    pub is_out_of_range: bool,
    pub is_hole: bool,
    pub is_active: bool,
    pub is_completed: bool,
}

impl Default for StreamStatus {
    fn default() -> Self {
        Self {
            is_out_of_range: false,
            is_hole: false,
            is_active: false,
            is_completed: false,
        }
    }
}

/// Constants for stream tracker
const ALLOC_BLK_SIZE: usize = 10000;
const COMPACTION_THRESHOLD: usize = ALLOC_BLK_SIZE / 2;

/// A thread-safe stream tracker that manages ordered processing of stream data
///
/// The StreamTracker maintains ordered processing of items in a stream,
/// tracking which items are active, completed, and providing truncation
/// capabilities.
///
/// Generic parameters:
/// - T: The data type being tracked (must be Clone)
/// - AUTO_TRUNCATE: Whether to automatically truncate when completion threshold is reached
pub struct StreamTracker<T: Clone, const AUTO_TRUNCATE: bool = false> {
    /// Thread-safe data protected by AsyncRwLock
    inner: AsyncRwLock<StreamTrackerInner<T>>,

    /// Atomic counter for completions since last truncate (used for
    /// auto-truncate)
    completed_count_since_last_truncate: AtomicUsize,

    /// Truncation frequency threshold
    truncate_on_count: usize,

    /// PhantomData to ensure T is used
    _phantom: std::marker::PhantomData<T>,
}

/// Internal data structure protected by RwLock
struct StreamTrackerInner<T: Clone> {
    /// Bitset tracking completed slots
    comp_slot_bits: AtomicBitset,

    /// Bitset tracking active (created but not necessarily completed) slots
    active_slot_bits: AtomicBitset,

    /// Vector storing the actual data
    slot_data: Vec<Option<T>>,

    /// Number of slots to skip at the beginning (for truncation optimization)
    data_skip_count: usize,

    /// Total allocated slots
    allocated_slots: usize,

    /// Reference index - the base index this tracker is managing from
    slot_ref_idx: i64,
}

impl<T: Clone, const AUTO_TRUNCATE: bool> StreamTracker<T, AUTO_TRUNCATE> {
    /// Create a new StreamTracker with given name and starting index
    pub fn new(_name: &str, start_idx: i64) -> Self {
        let slot_ref_idx = start_idx + 1;
        let allocated_slots = ALLOC_BLK_SIZE;

        let mut slot_data = Vec::with_capacity(allocated_slots);
        slot_data.resize(allocated_slots, None);

        Self {
            inner: AsyncRwLock::new(StreamTrackerInner {
                comp_slot_bits: AtomicBitset::new(allocated_slots as u64, 0),
                active_slot_bits: AtomicBitset::new(allocated_slots as u64, 1),
                slot_data,
                data_skip_count: 0,
                allocated_slots,
                slot_ref_idx,
            }),
            completed_count_since_last_truncate: AtomicUsize::new(0),
            truncate_on_count: 1000,
            _phantom: std::marker::PhantomData,
        }
    }

    /// Reinitialize with a new starting index
    pub async fn reinit(&self, start_idx: i64) {
        let mut inner = self.inner.write().await;
        inner.slot_ref_idx = start_idx + 1;
    }

    /// Create and immediately complete an entry
    pub async fn create_and_complete(&self, idx: i64, data: T) -> i64 {
        self.do_update(idx, data, true, |_| true).await
    }

    /// Create an entry (not completed)
    pub async fn create(&self, idx: i64, data: T) -> i64 { self.do_update(idx, data, true, |_| false).await }

    /// Update an existing entry with a processor function
    pub async fn update<F>(&self, idx: i64, data: T, processor: F) -> i64
    where
        F: FnOnce(&mut T) -> bool,
    {
        self.do_update(idx, data, false, processor).await
    }

    /// Mark a range of entries as completed
    pub async fn complete(&self, start_idx: i64, end_idx: i64) {
        let inner = self.inner.read().await;
        let start_bit = (start_idx - inner.slot_ref_idx) as u64;
        let count = (end_idx - start_idx + 1) as u64;
        drop(inner);

        // Need to get write access for mutations
        let mut inner = self.inner.write().await;
        inner.comp_slot_bits.set_bits(start_bit, count);
    }

    /// Rollback active entries from new_end_idx onwards
    pub async fn rollback(&self, new_end_idx: i64) -> Result<(), String> {
        let mut inner = self.inner.write().await;

        if new_end_idx < inner.slot_ref_idx
            || new_end_idx >= (inner.slot_ref_idx + inner.active_slot_bits.size() as i64)
        {
            return Err("Slot idx is not in range".to_string());
        }

        let new_end_bit = (new_end_idx - inner.slot_ref_idx) as u64;
        let reset_start = new_end_bit + 1;
        let reset_count = inner.active_slot_bits.size() - reset_start;

        if reset_count > 0 {
            inner.active_slot_bits.reset_bits(reset_start, reset_count);
            inner.comp_slot_bits.reset_bits(reset_start, reset_count);
        }

        Ok(())
    }

    /// Get a reference to the data at the specified index
    pub async fn at(&self, idx: i64) -> Result<T, String> {
        let inner = self.inner.read().await;

        if idx < inner.slot_ref_idx {
            return Err("Slot idx is not in range".to_string());
        }

        let nbit = (idx - inner.slot_ref_idx) as usize;
        if !inner.active_slot_bits.get_bitval(nbit as u64) {
            return Err("Slot idx is not in range".to_string());
        }

        if let Some(ref data) = inner.slot_data[nbit + inner.data_skip_count] {
            Ok(data.clone())
        } else {
            Err("Slot data is None".to_string())
        }
    }

    /// Get the status of an entry at the specified index
    pub async fn status(&self, idx: i64) -> StreamStatus {
        let mut status = StreamStatus::default();
        let inner = self.inner.read().await;

        if idx < inner.slot_ref_idx {
            status.is_out_of_range = true;
        } else {
            let nbit = (idx - inner.slot_ref_idx) as u64;
            if inner.comp_slot_bits.get_bitval(nbit) {
                status.is_completed = true;
            } else if inner.active_slot_bits.get_bitval(nbit) {
                status.is_active = true;
            } else {
                status.is_hole = true;
            }
        }

        status
    }

    /// Truncate entries up to the specified index
    pub async fn truncate_to(&self, idx: i64) -> usize {
        let mut inner = self.inner.write().await;
        let upto_bit = idx - inner.slot_ref_idx + 1;

        if upto_bit <= 0 {
            return (inner.slot_ref_idx - 1) as usize;
        }

        self.do_truncate(&mut inner, upto_bit as usize)
    }

    /// Auto-truncate up to the first incomplete entry
    pub async fn truncate(&self) -> usize {
        if AUTO_TRUNCATE && self.completed_count_since_last_truncate.load(Ordering::Acquire) == 0 {
            return 0;
        }

        let mut inner = self.inner.write().await;

        // Find the first incomplete bit
        let first_incomplete_bit = inner.comp_slot_bits.get_next_reset_bit(0);
        let upto_bit = if first_incomplete_bit == NPOS {
            // All bits are completed
            inner.allocated_slots
        } else if first_incomplete_bit == 0 {
            // Nothing is completed
            return (inner.slot_ref_idx - 1) as usize;
        } else {
            first_incomplete_bit as usize
        };

        self.do_truncate(&mut inner, upto_bit)
    }

    /// Find the highest consecutive completed index starting from
    /// search_hint_idx
    pub async fn completed_upto(&self, search_hint_idx: i64) -> i64 {
        let inner = self.inner.read().await;
        self.upto(&inner, true, search_hint_idx)
    }

    /// Find the highest consecutive active index starting from search_hint_idx
    pub async fn active_upto(&self, search_hint_idx: i64) -> i64 {
        let inner = self.inner.read().await;
        self.upto(&inner, false, search_hint_idx)
    }

    /// Iterate over all contiguous completed entries starting from start_idx
    pub async fn foreach_contiguous_completed<F>(&self, start_idx: i64, mut callback: F)
    where
        F: FnMut(i64, i64, &T) -> bool,
    {
        let inner = self.inner.read().await;
        let upto = self.upto(&inner, true, start_idx);

        for idx in start_idx..=upto {
            if let Some(ref data) = inner.slot_data[(idx - inner.slot_ref_idx) as usize + inner.data_skip_count] {
                if !callback(idx, upto, data) {
                    break;
                }
            }
        }
    }

    /// Iterate over all contiguous active entries starting from start_idx
    pub async fn foreach_contiguous_active<F>(&self, start_idx: i64, mut callback: F)
    where
        F: FnMut(i64, i64, &T) -> bool,
    {
        let inner = self.inner.read().await;
        let upto = self.upto(&inner, false, start_idx);

        for idx in start_idx..=upto {
            if let Some(ref data) = inner.slot_data[(idx - inner.slot_ref_idx) as usize + inner.data_skip_count] {
                if !callback(idx, upto, data) {
                    break;
                }
            }
        }
    }

    /// Iterate over all completed entries (including non-contiguous) starting
    /// from start_idx
    pub async fn foreach_all_completed<F>(&self, start_idx: i64, mut callback: F)
    where
        F: FnMut(i64, &T) -> bool,
    {
        let inner = self.inner.read().await;
        let mut search_bit = std::cmp::max(0, start_idx - inner.slot_ref_idx) as u64;

        loop {
            search_bit = inner.comp_slot_bits.get_next_set_bit(search_bit);
            if search_bit == NPOS {
                break;
            }

            let idx = search_bit as i64 + inner.slot_ref_idx;
            if let Some(ref data) = inner.slot_data[search_bit as usize + inner.data_skip_count] {
                if !callback(idx, data) {
                    break;
                }
            }
            search_bit += 1;
        }
    }

    /// Iterate over all active entries (including non-contiguous) starting from
    /// start_idx
    pub async fn foreach_all_active<F>(&self, start_idx: i64, mut callback: F)
    where
        F: FnMut(i64, &T) -> bool,
    {
        let inner = self.inner.read().await;
        let mut search_bit = std::cmp::max(0, start_idx - inner.slot_ref_idx) as u64;

        loop {
            search_bit = inner.active_slot_bits.get_next_set_bit(search_bit);
            if search_bit == NPOS {
                break;
            }

            let idx = search_bit as i64 + inner.slot_ref_idx;
            if let Some(ref data) = inner.slot_data[search_bit as usize + inner.data_skip_count] {
                if !callback(idx, data) {
                    break;
                }
            }
            search_bit += 1;
        }
    }

    /// Get detailed status information as JSON
    pub async fn get_status(&self, verbosity: u8) -> JsonValue {
        let inner = self.inner.read().await;
        let mut status = json!({
            "start": inner.slot_ref_idx,
            "completed_upto": self.completed_upto(0).await,
            "active_upto": self.active_upto(0).await
        });

        if verbosity >= 2 {
            status["allocated_count"] = json!(inner.allocated_slots);
            if AUTO_TRUNCATE {
                status["completed_since_last_truncate"] =
                    json!(self.completed_count_since_last_truncate.load(Ordering::Relaxed));
            }
            status["truncate_frequency"] = json!(self.truncate_on_count);
            status["garbage_count"] = json!(inner.data_skip_count);
        }

        status
    }
}

// Private implementation methods
impl<T: Clone, const AUTO_TRUNCATE: bool> StreamTracker<T, AUTO_TRUNCATE> {
    /// Internal method to update an entry
    async fn do_update<F>(&self, idx: i64, data: T, replace: bool, processor: F) -> i64
    where
        F: FnOnce(&mut T) -> bool,
    {
        let mut need_truncate = false;

        // Check if we need to resize first (read lock)
        {
            let inner = self.inner.read().await;
            if idx < inner.slot_ref_idx {
                return inner.slot_ref_idx - 1;
            }

            let nbit = (idx - inner.slot_ref_idx) as usize;
            if nbit >= inner.allocated_slots {
                // Need resize outside of read lock
                drop(inner);
                self.do_resize(nbit + 1).await;
            }
        }

        // Actual update under write lock
        let mut inner = self.inner.write().await;
        let nbit = (idx - inner.slot_ref_idx) as usize;
        let data_idx = nbit + inner.data_skip_count;

        // Update the data
        if replace || inner.slot_data[data_idx].is_none() {
            inner.slot_data[data_idx] = Some(data);
            inner.active_slot_bits.set_bit(nbit as u64);
        }

        // Process the data and check for completion
        let mut completed = false;
        if let Some(ref mut slot_data_item) = inner.slot_data[data_idx] {
            completed = processor(slot_data_item);
        }

        if completed {
            inner.comp_slot_bits.set_bit(nbit as u64);

            if AUTO_TRUNCATE {
                let count = self.completed_count_since_last_truncate.fetch_add(1, Ordering::AcqRel);
                if count >= self.truncate_on_count {
                    need_truncate = true;
                }
            }
        }

        let ret = inner.slot_ref_idx - 1;
        drop(inner); // Release write lock

        if need_truncate { self.truncate().await as i64 } else { ret }
    }

    /// Resize the internal storage
    async fn do_resize(&self, atleast_count: usize) {
        let mut inner = self.inner.write().await;

        if atleast_count <= inner.allocated_slots {
            return;
        }

        let new_count = std::cmp::max(inner.allocated_slots * 2, atleast_count);

        // Resize the data vector
        let mut new_slot_data = Vec::with_capacity(new_count);

        // Copy existing data (handling skip count)
        for _i in 0..inner.data_skip_count {
            new_slot_data.push(None);
        }

        for i in inner.data_skip_count..inner.allocated_slots + inner.data_skip_count {
            if i < inner.slot_data.len() {
                new_slot_data.push(inner.slot_data[i].clone());
            } else {
                new_slot_data.push(None);
            }
        }

        // Fill remaining slots
        new_slot_data.resize(new_count, None);

        inner.slot_data = new_slot_data;
        inner.allocated_slots = new_count;
        inner.data_skip_count = 0;

        // Resize bitsets
        inner.active_slot_bits.resize(new_count as u64, false).unwrap();
        inner.comp_slot_bits.resize(new_count as u64, false).unwrap();
    }

    /// Internal truncation implementation
    fn do_truncate(&self, inner: &mut StreamTrackerInner<T>, upto_bit: usize) -> usize {
        // Shrink the bitsets
        inner.comp_slot_bits.shrink_head(upto_bit as u64).unwrap();
        inner.active_slot_bits.shrink_head(upto_bit as u64).unwrap();

        // Update skip count and allocated slots
        inner.data_skip_count += upto_bit;
        inner.allocated_slots -= upto_bit;

        // Compact data if threshold is reached (equivalent to C++ memmove)
        if inner.data_skip_count > COMPACTION_THRESHOLD {
            // Use Vec::drain to efficiently remove elements from the front
            // This is equivalent to C++ memmove - it shifts remaining elements left
            // in-place
            inner.slot_data.drain(0..inner.data_skip_count);
            inner.data_skip_count = 0;
        }

        inner.slot_ref_idx += upto_bit as i64;

        (inner.slot_ref_idx - 1) as usize
    }

    /// Find the highest consecutive index with the specified status
    fn upto(&self, inner: &StreamTrackerInner<T>, completed: bool, search_hint_idx: i64) -> i64 {
        let search_start_bit = std::cmp::max(0, search_hint_idx - inner.slot_ref_idx) as u64;

        let first_incomplete_bit = if completed {
            inner.comp_slot_bits.get_next_reset_bit(search_start_bit)
        } else {
            inner.active_slot_bits.get_next_reset_bit(search_start_bit)
        };

        if first_incomplete_bit == NPOS {
            inner.slot_ref_idx + inner.allocated_slots as i64 - 1
        } else {
            inner.slot_ref_idx + first_incomplete_bit as i64 - 1
        }
    }
}

// Convenience type aliases
pub type StreamTrackerAutoTruncate<T> = StreamTracker<T, true>;
pub type StreamTrackerManual<T> = StreamTracker<T, false>;

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_stream_tracker_basic() {
        let tracker = StreamTracker::<i32, false>::new("test", 0);

        // Create some entries
        tracker.create(1, 10).await;
        tracker.create(2, 20).await;
        tracker.create(3, 30).await;

        // Check they exist
        assert_eq!(tracker.at(1).await.unwrap(), 10);
        assert_eq!(tracker.at(2).await.unwrap(), 20);
        assert_eq!(tracker.at(3).await.unwrap(), 30);

        // Check status
        let status1 = tracker.status(1).await;
        assert!(status1.is_active);
        assert!(!status1.is_completed);

        // Complete some entries
        tracker.complete(1, 2).await;

        let status1_after = tracker.status(1).await;
        assert!(status1_after.is_completed);

        let status2_after = tracker.status(2).await;
        assert!(status2_after.is_completed);

        let status3_after = tracker.status(3).await;
        assert!(status3_after.is_active);
        assert!(!status3_after.is_completed);
    }

    #[tokio::test]
    async fn test_stream_tracker_truncate() {
        let tracker = StreamTracker::<String, false>::new("test", 0);

        // Create and complete some entries
        tracker.create_and_complete(1, "first".to_string()).await;
        tracker.create_and_complete(2, "second".to_string()).await;
        tracker.create_and_complete(3, "third".to_string()).await;

        // Truncate up to index 2
        let truncated_upto = tracker.truncate_to(2).await;
        assert_eq!(truncated_upto, 2);

        // Check that entries before truncation are out of range
        let status1 = tracker.status(1).await;
        assert!(status1.is_out_of_range);

        let status2 = tracker.status(2).await;
        assert!(status2.is_out_of_range);

        // Entry 3 should still be accessible
        assert_eq!(tracker.at(3).await.unwrap(), "third");
    }

    #[tokio::test]
    async fn test_stream_tracker_rollback() {
        let tracker = StreamTracker::<i32, false>::new("test", 0);

        tracker.create(1, 10).await;
        tracker.create(2, 20).await;
        tracker.create(3, 30).await;
        tracker.create(4, 40).await;

        // Rollback from index 2
        tracker.rollback(2).await.unwrap();

        // Entries 1 and 2 should still be active
        assert!(tracker.status(1).await.is_active);
        assert!(tracker.status(2).await.is_active);

        // Entries 3 and 4 should be holes
        assert!(tracker.status(3).await.is_hole);
        assert!(tracker.status(4).await.is_hole);
    }

    #[tokio::test]
    async fn test_stream_tracker_iteration() {
        let tracker = StreamTracker::<i32, false>::new("test", 0);

        tracker.create_and_complete(1, 10).await;
        tracker.create_and_complete(2, 20).await;
        tracker.create(3, 30).await; // Active but not completed
        tracker.create_and_complete(4, 40).await;

        let mut completed_items = Vec::new();
        tracker
            .foreach_all_completed(1, |idx, data| {
                completed_items.push((idx, *data));
                true
            })
            .await;

        assert_eq!(completed_items.len(), 3);
        assert!(completed_items.contains(&(1, 10)));
        assert!(completed_items.contains(&(2, 20)));
        assert!(completed_items.contains(&(4, 40)));
        assert!(!completed_items.contains(&(3, 30)));
    }
}
