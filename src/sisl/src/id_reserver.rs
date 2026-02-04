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

use crate::bitset::{Bitset, NPOS};

/// A thread-safe ID reservation system using a bitset to track reserved IDs
///
/// The IdReserver provides atomic operations for reserving and unreserving IDs,
/// with automatic expansion when needed. It's designed for scenarios where you
/// need to allocate unique identifiers from a pool and track their usage.
///
/// # Example (async)
///
/// ```no_run
/// use sisl::IdReserver;
/// #[tokio::main]
/// async fn main() {
///     let reserver = IdReserver::new(Some(1024));
///     let id1 = reserver.reserve().await.unwrap();
///     let id2 = reserver.reserve().await.unwrap();
///     assert!(reserver.is_reserved(id1).await);
///     assert!(reserver.is_reserved(id2).await);
///     reserver.unreserve(id1).await.unwrap();
///     assert!(!reserver.is_reserved(id1).await);
/// }
/// ```
pub struct IdReserver {
    /// AsyncMutex protecting the bitset from concurrent access
    inner: AsyncMutex<IdReserverInner>,
}

/// Internal data structure protected by mutex
struct IdReserverInner {
    /// Bitset tracking which IDs are reserved (set=reserved, unset=available)
    reserved_bits: Bitset,
}

impl IdReserver {
    /// Create a new IdReserver with estimated capacity
    ///
    /// # Arguments
    /// * `estimated_ids` - Optional initial capacity. If None, defaults to 1024
    ///
    /// # Panics
    /// Panics if estimated_ids is Some(0)
    pub fn new(estimated_ids: Option<u32>) -> Self {
        let capacity = estimated_ids.unwrap_or(1024);
        assert!(capacity > 0, "estimated_ids must be greater than 0");

        Self { inner: AsyncMutex::new(IdReserverInner { reserved_bits: Bitset::new(capacity as u64, 0) }) }
    }

    /// Create an IdReserver from serialized data
    ///
    /// # Arguments
    /// * `data` - Byte slice containing serialized bitset data
    ///
    /// # Returns
    /// Result containing the IdReserver or an error if deserialization fails
    pub fn from_bytes(data: &[u8]) -> Result<Self, String> {
        use iomgr::IOBuffer;
        let mut buf = IOBuffer::new(data.len());
        buf.as_mut_slice().copy_from_slice(data);
        let (bitset, _set_count) = Bitset::load(buf).map_err(|e| format!("Failed to deserialize bitset: {}", e))?;

        Ok(Self { inner: AsyncMutex::new(IdReserverInner { reserved_bits: bitset }) })
    }

    /// Reserve the next available ID
    ///
    /// Since the current bitset implementation doesn't support expanding,
    /// this method will create a new larger bitset and copy existing data when
    /// needed.
    ///
    /// # Returns
    /// Result containing the reserved ID or an error if no IDs are available
    ///
    /// # Example (async)
    /// ```no_run
    /// use sisl::IdReserver;
    /// #[tokio::main]
    /// async fn main() {
    ///     let reserver = IdReserver::new(Some(64));
    ///     let id = reserver.reserve().await.unwrap();
    ///     assert!(id < 64);
    /// }
    /// ```
    pub async fn reserve(&self) -> Result<u32, String> {
        let mut inner = self.inner.lock().await;

        let next_bit = inner.reserved_bits.get_next_reset_bit(0);
        let id = if next_bit == NPOS {
            // We ran out of room, expand the bitset efficiently using resize
            let current_size = inner.reserved_bits.size();
            if current_size == 0 {
                return Err("Bitset has zero size".to_string());
            }

            inner
                .reserved_bits
                .resize(current_size * 2, false)
                .map_err(|e| format!("Failed to resize bitset: {}", e))?;

            current_size
        } else {
            next_bit
        };

        inner.reserved_bits.set_bit(id);
        Ok(id as u32)
    }

    /// Reserve a specific ID
    ///
    /// If the ID is beyond the current capacity, the bitset will be expanded.
    ///
    /// # Arguments
    /// * `id` - The ID to reserve
    ///
    /// # Returns
    /// Result indicating success or failure
    ///
    /// # Errors
    /// Returns an error if the ID is already reserved
    pub async fn reserve_specific(&self, id: u32) -> Result<(), String> {
        let mut inner = self.inner.lock().await;

        // Expand bitset if needed using efficient resize
        if id as u64 >= inner.reserved_bits.size() {
            let new_size = ((id as u64 + 1) * 2).max(inner.reserved_bits.size() * 2);
            inner.reserved_bits.resize(new_size, false).map_err(|e| format!("Failed to resize bitset: {}", e))?;
        }

        if inner.reserved_bits.get_bitval(id as u64) {
            return Err(format!("ID {} is already reserved", id));
        }

        inner.reserved_bits.set_bit(id as u64);
        Ok(())
    }

    /// Unreserve a previously reserved ID
    ///
    /// # Arguments
    /// * `id` - The ID to unreserve
    ///
    /// # Returns
    /// Result indicating success or failure
    ///
    /// # Errors
    /// Returns an error if the ID is out of bounds
    pub async fn unreserve(&self, id: u32) -> Result<(), String> {
        let mut inner = self.inner.lock().await;

        if id as u64 >= inner.reserved_bits.size() {
            return Err(format!("ID {} is out of bounds (max: {})", id, inner.reserved_bits.size() - 1));
        }

        inner.reserved_bits.reset_bit(id as u64);
        Ok(())
    }

    /// Check if an ID is reserved
    ///
    /// # Arguments
    /// * `id` - The ID to check
    ///
    /// # Returns
    /// true if the ID is reserved, false otherwise (including out of bounds
    /// IDs)
    pub async fn is_reserved(&self, id: u32) -> bool {
        let inner = self.inner.lock().await;

        if id as u64 >= inner.reserved_bits.size() {
            return false; // Out of bounds IDs are considered not reserved
        }

        inner.reserved_bits.get_bitval(id as u64)
    }

    /// Get the current capacity (maximum ID + 1)
    pub async fn capacity(&self) -> u64 {
        let inner = self.inner.lock().await;
        inner.reserved_bits.size()
    }

    /// Get the number of reserved IDs
    pub async fn reserved_count(&self) -> u64 {
        let inner = self.inner.lock().await;
        inner.reserved_bits.get_set_count(0, None)
    }

    /// Serialize the IdReserver to bytes
    ///
    /// # Returns
    /// Vector of bytes representing the serialized bitset
    pub async fn serialize(&self) -> Vec<u8> {
        let inner = self.inner.lock().await;
        inner.reserved_bits.data().to_vec()
    }

    /// Find the first reserved ID
    ///
    /// # Returns
    /// Option containing the first reserved ID, or None if no IDs are reserved
    pub async fn first_reserved_id(&self) -> Option<u32> {
        let inner = self.inner.lock().await;
        let first_bit = inner.reserved_bits.get_next_set_bit(0);

        if first_bit == NPOS {
            None
        } else {
            Some(first_bit as u32)
        }
    }

    /// Find the next reserved ID after the given ID
    ///
    /// # Arguments
    /// * `last_id` - The ID to search after
    ///
    /// # Returns
    /// Option containing the next reserved ID, or None if no more reserved IDs
    /// exist
    pub async fn next_reserved_id(&self, last_id: u32) -> Option<u32> {
        let inner = self.inner.lock().await;
        let next_bit = inner.reserved_bits.get_next_set_bit(last_id as u64 + 1);

        if next_bit == NPOS {
            None
        } else {
            Some(next_bit as u32)
        }
    }

    // Iterator over all reserved IDs disabled in async version.
    // Original API: reserved_ids() -> Iterator
    // Use first_reserved_id()/next_reserved_id() to traverse.
}

/// Iterator over reserved IDs in an IdReserver
// pub struct ReservedIdIterator<'a> { reserver: &'a IdReserver, current_id:
// Option<u32> } impl<'a> Iterator for ReservedIdIterator<'a> { type Item = u32;
// fn next(&mut self) -> Option<Self::Item> { None } }

// Implement Debug for better debugging experience
// Debug implementation omitted for async mutex (would require async lock in
// fmt).

// Thread safety: IdReserver is thread-safe due to internal Mutex
unsafe impl Send for IdReserver {}
unsafe impl Sync for IdReserver {}
#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_id_reserver_basic() {
        let reserver = IdReserver::new(Some(64));

        // Reserve some IDs
        let id1 = reserver.reserve().await.unwrap();
        let id2 = reserver.reserve().await.unwrap();
        let id3 = reserver.reserve().await.unwrap();

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
    async fn test_id_reserver_specific_reservation() {
        let reserver = IdReserver::new(Some(64));

        // Reserve specific IDs
        reserver.reserve_specific(5).await.unwrap();
        reserver.reserve_specific(10).await.unwrap();
        reserver.reserve_specific(15).await.unwrap();

        assert!(reserver.is_reserved(5).await);
        assert!(reserver.is_reserved(10).await);
        assert!(reserver.is_reserved(15).await);

        // Try to reserve an already reserved ID
        assert!(reserver.reserve_specific(5).await.is_err());

        // Reserve next available should skip reserved ones
        let next_id = reserver.reserve().await.unwrap();
        assert_eq!(next_id, 0); // First available
    }

    #[tokio::test]
    async fn test_id_reserver_unreserve() {
        let reserver = IdReserver::new(Some(64));

        let id1 = reserver.reserve().await.unwrap();
        let id2 = reserver.reserve().await.unwrap();

        assert!(reserver.is_reserved(id1).await);
        assert!(reserver.is_reserved(id2).await);

        // Unreserve id1
        reserver.unreserve(id1).await.unwrap();
        assert!(!reserver.is_reserved(id1).await);
        assert!(reserver.is_reserved(id2).await);

        // Reserve again should reuse the unreserved ID
        let id3 = reserver.reserve().await.unwrap();
        assert_eq!(id3, id1); // Should reuse the unreserved ID
    }

    #[tokio::test]
    async fn test_id_reserver_expansion() {
        let reserver = IdReserver::new(Some(4)); // Small initial size

        // Reserve more IDs than initial capacity using reserve()
        let mut ids = Vec::new();
        for _ in 0..10 {
            ids.push(reserver.reserve().await.unwrap());
        }

        // All IDs should be unique and consecutive
        for (i, &id) in ids.iter().enumerate() {
            assert_eq!(id, i as u32);
            assert!(reserver.is_reserved(id).await);
        }

        // Capacity should have expanded
        assert!(reserver.capacity().await >= 10);

        // Test expansion with reserve_specific beyond current capacity
        reserver.reserve_specific(100).await.unwrap();
        assert!(reserver.is_reserved(100).await);
        assert!(reserver.capacity().await > 100);
    }

    #[tokio::test]
    async fn test_id_reserver_iteration() {
        let reserver = IdReserver::new(Some(64));

        // Reserve some non-consecutive IDs
        reserver.reserve_specific(2).await.unwrap();
        reserver.reserve_specific(5).await.unwrap();
        reserver.reserve_specific(8).await.unwrap();
        reserver.reserve_specific(12).await.unwrap();

        // Collect all reserved IDs manually using first/next methods
        let mut reserved_ids = Vec::new();
        let mut cur = reserver.first_reserved_id().await;
        while let Some(id) = cur {
            reserved_ids.push(id);
            cur = reserver.next_reserved_id(id).await;
        }
        assert_eq!(reserved_ids, vec![2, 5, 8, 12]);

        // Test first_reserved_id and next_reserved_id
        assert_eq!(reserver.first_reserved_id().await, Some(2));
        assert_eq!(reserver.next_reserved_id(2).await, Some(5));
        assert_eq!(reserver.next_reserved_id(5).await, Some(8));
        assert_eq!(reserver.next_reserved_id(8).await, Some(12));
        assert_eq!(reserver.next_reserved_id(12).await, None);
    }

    #[tokio::test]
    async fn test_id_reserver_serialization() {
        let reserver1 = IdReserver::new(Some(64));

        // Reserve some IDs
        reserver1.reserve_specific(5).await.unwrap();
        reserver1.reserve_specific(10).await.unwrap();
        reserver1.reserve_specific(20).await.unwrap();

        // Serialize
        let serialized = reserver1.serialize().await;

        // Deserialize into a new reserver
        let reserver2 = IdReserver::from_bytes(&serialized).unwrap();

        // Check that the state is preserved
        assert!(reserver2.is_reserved(5).await);
        assert!(reserver2.is_reserved(10).await);
        assert!(reserver2.is_reserved(20).await);
        assert!(!reserver2.is_reserved(15).await);

        // Check that the capacity is preserved
        assert_eq!(reserver2.capacity().await, reserver1.capacity().await);
        assert_eq!(reserver2.reserved_count().await, reserver1.reserved_count().await);
    }

    #[tokio::test]
    async fn test_id_reserver_error_cases() {
        let reserver = IdReserver::new(Some(64));

        // Test out of bounds unreservation
        assert!(reserver.unreserve(100).await.is_err());

        // Test out of bounds check (should return false, not error)
        assert!(!reserver.is_reserved(100).await);

        // Test double reservation of specific ID
        reserver.reserve_specific(5).await.unwrap();
        assert!(reserver.reserve_specific(5).await.is_err());
    }

    #[tokio::test]
    async fn test_id_reserver_counts() {
        let reserver = IdReserver::new(Some(64));

        assert_eq!(reserver.reserved_count().await, 0);
        assert_eq!(reserver.capacity().await, 64);

        reserver.reserve_specific(5).await.unwrap();
        reserver.reserve_specific(10).await.unwrap();

        assert_eq!(reserver.reserved_count().await, 2);

        reserver.unreserve(5).await.unwrap();
        assert_eq!(reserver.reserved_count().await, 1);
    }

    #[test]
    #[should_panic(expected = "estimated_ids must be greater than 0")]
    fn test_id_reserver_zero_capacity() { IdReserver::new(Some(0)); }
}
