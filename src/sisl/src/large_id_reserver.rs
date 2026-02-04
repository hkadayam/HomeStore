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

use iomgr::AsyncMutex;
use std::collections::BTreeMap;

/// A thread-safe ID reservation system using interval map for efficient ID tracking
///
/// The LargeIDReserver is optimized for scenarios where IDs are allocated and freed
/// frequently, and you need to efficiently find gaps in the ID space. It uses an
/// interval map to coalesce consecutive reserved IDs, making it more memory-efficient
/// than bitset-based approaches for sparse ID allocations.
///
/// Unlike IdReserver which uses a bitset, LargeIDReserver:
/// - Is more memory-efficient for sparse ID ranges
/// - Has a fixed maximum capacity (no auto-expansion)
/// - Supports u64 ID space
/// - Coalesces consecutive IDs into intervals
///
/// # Implementation
///
/// Uses `BTreeMap<u64, u64>` where each entry `(start, end)` represents a
/// right-open interval `[start, end)` of reserved IDs. This is similar to
/// Boost's `interval_set` from the ICL library.
///
/// # Example
///
/// ```no_run
/// use sisl::LargeIDReserver;
/// #[tokio::main]
/// async fn main() {
///     let reserver = LargeIDReserver::new(1000);
///     let id1 = reserver.reserve().await.unwrap();
///     let id2 = reserver.reserve().await.unwrap();
///     assert!(reserver.is_reserved(id1).await);
///     assert!(reserver.is_reserved(id2).await);
///     reserver.unreserve(id1).await;
///     assert!(!reserver.is_reserved(id1).await);
/// }
/// ```
pub struct LargeIDReserver {
    inner: AsyncMutex<LargeIDReserverInner>,
    max: u64,
}

struct LargeIDReserverInner {
    /// Interval map: start -> end (right-open intervals [start, end))
    /// Each entry represents a contiguous range of reserved IDs
    intervals: BTreeMap<u64, u64>,
}

impl LargeIDReserver {
    /// Out of bounds value returned when no IDs are available
    pub const OUT_OF_BOUNDS: u64 = u64::MAX;

    /// Create a new LargeIDReserver with a fixed maximum capacity
    ///
    /// # Arguments
    /// * `max_count` - Maximum number of IDs that can be allocated (0 to max_count-1)
    ///
    /// # Panics
    /// Panics if max_count is 0
    pub fn new(max_count: u64) -> Self {
        assert!(max_count > 0, "max_count must be greater than 0");
        Self {
            inner: AsyncMutex::new(LargeIDReserverInner {
                intervals: BTreeMap::new(),
            }),
            max: max_count,
        }
    }

    /// Reserve the next available ID
    ///
    /// Finds the first gap in the reserved ID space and allocates it.
    ///
    /// # Returns
    /// The reserved ID, or OUT_OF_BOUNDS if no IDs are available
    pub async fn reserve(&self) -> u64 {
        let mut inner = self.inner.lock().await;
        let id = Self::find_next(&inner.intervals, self.max);
        
        if id >= self.max {
            return Self::OUT_OF_BOUNDS;
        }

        Self::insert_interval(&mut inner.intervals, id, id + 1);
        id
    }

    /// Reserve a specific ID
    ///
    /// # Arguments
    /// * `id` - The ID to reserve
    ///
    /// # Panics
    /// Panics if the ID is already reserved (debug builds only)
    pub async fn reserve_specific(&self, id: u64) {
        let mut inner = self.inner.lock().await;
        
        debug_assert!(
            !Self::is_reserved_internal(&inner.intervals, id),
            "Reserving an already reserved id={}",
            id
        );
        
        Self::insert_interval(&mut inner.intervals, id, id + 1);
    }

    /// Unreserve a previously reserved ID
    ///
    /// # Arguments
    /// * `id` - The ID to unreserve
    ///
    /// # Panics
    /// Panics if the ID is out of bounds (debug builds only)
    pub async fn unreserve(&self, id: u64) {
        debug_assert!(id < self.max, "Unreserving an id which was out of bounds: id={}, max={}", id, self.max);
        
        let mut inner = self.inner.lock().await;
        Self::remove_interval(&mut inner.intervals, id, id + 1);
    }

    /// Check if an ID is reserved
    ///
    /// # Arguments
    /// * `id` - The ID to check
    ///
    /// # Returns
    /// true if the ID is reserved, false otherwise
    pub async fn is_reserved(&self, id: u64) -> bool {
        let inner = self.inner.lock().await;
        Self::is_reserved_internal(&inner.intervals, id)
    }

    /// Get the maximum capacity
    pub fn max(&self) -> u64 {
        self.max
    }

    /// Get the number of reserved IDs
    pub async fn reserved_count(&self) -> usize {
        let inner = self.inner.lock().await;
        inner.intervals.values().zip(inner.intervals.keys())
            .map(|(end, start)| (end - start) as usize)
            .sum()
    }

    // Internal helper methods

    fn is_reserved_internal(intervals: &BTreeMap<u64, u64>, id: u64) -> bool {
        // Find the first interval with start <= id
        if let Some((&start, &end)) = intervals.range(..=id).next_back() {
            id >= start && id < end
        } else {
            false
        }
    }

    fn find_next(intervals: &BTreeMap<u64, u64>, max: u64) -> u64 {
        let mut next = 0;
        
        for (&start, &end) in intervals.iter() {
            if next < start {
                // Found a gap before this interval
                return next;
            }
            // Update next to after this interval
            next = end;
        }
        
        // Return next (either a gap after all intervals, or max if full)
        if next < max { next } else { max }
    }

    fn insert_interval(intervals: &mut BTreeMap<u64, u64>, start: u64, end: u64) {
        // Find overlapping/adjacent intervals and merge them
        let mut merged_start = start;
        let mut merged_end = end;
        let mut to_remove = Vec::new();
        
        // Collect all intervals that might overlap or be adjacent
        // We need to check intervals before start (adjacent/overlapping from left)
        // and intervals from start onwards (overlapping/adjacent from right)
        let candidates: Vec<(u64, u64)> = intervals
            .range(..)
            .filter_map(|(&s, &e)| {
                // Include if: overlapping or adjacent
                if (s <= merged_end && e >= merged_start) || e == merged_start || s == merged_end {
                    Some((s, e))
                } else {
                    None
                }
            })
            .collect();
        
        // Merge all candidates
        for (s, e) in candidates {
            merged_start = merged_start.min(s);
            merged_end = merged_end.max(e);
            to_remove.push(s);
        }
        
        // Remove old intervals
        for key in to_remove {
            intervals.remove(&key);
        }
        
        // Insert merged interval
        intervals.insert(merged_start, merged_end);
    }

    fn remove_interval(intervals: &mut BTreeMap<u64, u64>, start: u64, end: u64) {
        let mut to_add = Vec::new();
        let mut to_remove = Vec::new();
        
        // Find overlapping intervals
        for (&s, &e) in intervals.range(..=start).rev().take(1).chain(intervals.range(start..)) {
            if e <= start || s >= end {
                // No overlap
                continue;
            }
            
            // This interval overlaps with [start, end)
            to_remove.push(s);
            
            // Keep the part before [start, end)
            if s < start {
                to_add.push((s, start));
            }
            
            // Keep the part after [start, end)
            if e > end {
                to_add.push((end, e));
            }
        }
        
        // Remove overlapping intervals
        for key in to_remove {
            intervals.remove(&key);
        }
        
        // Add remaining parts
        for (s, e) in to_add {
            intervals.insert(s, e);
        }
    }
}

unsafe impl Send for LargeIDReserver {}
unsafe impl Sync for LargeIDReserver {}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_large_id_reserver_basic() {
        let reserver = LargeIDReserver::new(1000);

        // Reserve some IDs
        let id1 = reserver.reserve().await;
        let id2 = reserver.reserve().await;
        let id3 = reserver.reserve().await;

        // IDs should be consecutive starting from 0
        assert_eq!(id1, 0);
        assert_eq!(id2, 1);
        assert_eq!(id3, 2);

        // Check reservation status
        assert!(reserver.is_reserved(id1).await);
        assert!(reserver.is_reserved(id2).await);
        assert!(reserver.is_reserved(id3).await);
        assert!(!reserver.is_reserved(10).await);
    }

    #[tokio::test]
    async fn test_large_id_reserver_specific_reservation() {
        let reserver = LargeIDReserver::new(1000);

        // Reserve specific IDs
        reserver.reserve_specific(5).await;
        reserver.reserve_specific(10).await;
        reserver.reserve_specific(15).await;

        assert!(reserver.is_reserved(5).await);
        assert!(reserver.is_reserved(10).await);
        assert!(reserver.is_reserved(15).await);
        assert!(!reserver.is_reserved(7).await);

        // Reserve next available should skip reserved ones
        let next_id = reserver.reserve().await;
        assert_eq!(next_id, 0); // First available
    }

    #[tokio::test]
    async fn test_large_id_reserver_unreserve() {
        let reserver = LargeIDReserver::new(1000);

        let id1 = reserver.reserve().await;
        let id2 = reserver.reserve().await;
        let id3 = reserver.reserve().await;

        assert!(reserver.is_reserved(id1).await);
        assert!(reserver.is_reserved(id2).await);
        assert!(reserver.is_reserved(id3).await);

        // Unreserve id2 (middle one)
        reserver.unreserve(id2).await;
        assert!(reserver.is_reserved(id1).await);
        assert!(!reserver.is_reserved(id2).await);
        assert!(reserver.is_reserved(id3).await);

        // Reserve again should reuse the unreserved ID
        let id4 = reserver.reserve().await;
        assert_eq!(id4, id2); // Should reuse the gap
    }

    #[tokio::test]
    async fn test_large_id_reserver_interval_coalescing() {
        let reserver = LargeIDReserver::new(1000);

        // Reserve consecutive IDs
        for i in 0..10 {
            reserver.reserve_specific(i).await;
        }

        // Should coalesce into a single interval
        assert_eq!(reserver.reserved_count().await, 10);

        // All should be reserved
        for i in 0..10 {
            assert!(reserver.is_reserved(i).await);
        }
        assert!(!reserver.is_reserved(10).await);
    }

    #[tokio::test]
    async fn test_large_id_reserver_find_gaps() {
        let reserver = LargeIDReserver::new(1000);

        // Reserve IDs with gaps
        reserver.reserve_specific(0).await;
        reserver.reserve_specific(1).await;
        reserver.reserve_specific(5).await;
        reserver.reserve_specific(6).await;
        reserver.reserve_specific(10).await;

        // Next reserve should find first gap
        let next = reserver.reserve().await;
        assert_eq!(next, 2);

        let next = reserver.reserve().await;
        assert_eq!(next, 3);

        let next = reserver.reserve().await;
        assert_eq!(next, 4);

        let next = reserver.reserve().await;
        assert_eq!(next, 7);
    }

    #[tokio::test]
    async fn test_large_id_reserver_max_capacity() {
        let reserver = LargeIDReserver::new(10);

        // Reserve all IDs
        for _ in 0..10 {
            let id = reserver.reserve().await;
            assert!(id < 10);
        }

        // Next reserve should return OUT_OF_BOUNDS
        let id = reserver.reserve().await;
        assert_eq!(id, LargeIDReserver::OUT_OF_BOUNDS);
    }

    #[tokio::test]
    async fn test_large_id_reserver_interval_splitting() {
        let reserver = LargeIDReserver::new(1000);

        // Reserve a range
        for i in 10..20 {
            reserver.reserve_specific(i).await;
        }

        // Unreserve from the middle
        reserver.unreserve(15).await;

        // Should split the interval
        assert!(reserver.is_reserved(14).await);
        assert!(!reserver.is_reserved(15).await);
        assert!(reserver.is_reserved(16).await);
    }

    #[tokio::test]
    async fn test_adjacent_interval_merging() {
        let reserver = LargeIDReserver::new(1000);

        // Reserve ID 1, then ID 2 - should merge into [1, 3)
        reserver.reserve_specific(1).await;
        assert_eq!(reserver.reserved_count().await, 1);
        
        reserver.reserve_specific(2).await;
        assert_eq!(reserver.reserved_count().await, 2);
        
        // Both should be reserved
        assert!(reserver.is_reserved(1).await);
        assert!(reserver.is_reserved(2).await);
        assert!(!reserver.is_reserved(0).await);
        assert!(!reserver.is_reserved(3).await);
    }

    #[test]
    #[should_panic(expected = "max_count must be greater than 0")]
    fn test_large_id_reserver_zero_capacity() {
        LargeIDReserver::new(0);
    }
}
