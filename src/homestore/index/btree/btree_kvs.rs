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

//! Btree Key and Value Traits
//!
//! This module defines the traits for keys and values in the B-tree.
//! Corresponds to C++ BtreeKey and BtreeValue concepts.

use std::io;

use super::btree_node::BNodeId;
use super::btree_types::{BtreeError, BtreeBuffer};

//================================================================================
// Core Traits
//================================================================================

/// Trait for B-tree keys (matches C++ BtreeKey concept)
///
/// Keys must be:
/// - Sized: Known size at compile time
/// - Send + Sync: Thread-safe
/// - Clone: Can be copied
/// - Serializable: Can convert to/from bytes
/// - Comparable: Can be ordered
/// - Debug: Can be formatted for logging/tracing
pub trait BtreeKey: Sized + Send + Sync + Clone + Ord + PartialOrd + Eq + PartialEq + std::fmt::Debug {
    /// Fixed serialized size for this type (None for variable-sized keys)
    /// SimpleNode requires Some(size) - it only works with fixed-size keys
    const FIXED_SERIALIZED_SIZE: Option<u32>;

    /// Get the serialized size of this key in bytes
    fn serialized_size(&self) -> u32;

    /// Runtime fixed size check - returns Some(size) if this instance has fixed size
    /// Default implementation uses compile-time constant
    /// Types with runtime-determined fixed sizes (like DbKey) override this
    fn fixed_serialized_size(&self) -> Option<u32> { Self::FIXED_SERIALIZED_SIZE }

    /// Serialize key to buffer (buffer already sliced to target position)
    ///
    /// # Arguments
    /// * `buf` - Target buffer (already positioned at write location)
    /// * `copy` - Hint: false = "use fastest method" (direct memcpy if safe) true = "always use safe serialization"
    ///   (custom format)
    ///
    /// The implementation decides how to interpret this hint.
    ///
    /// # Returns
    /// * Number of bytes written
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32>;

    /// Deserialize key from buffer (buffer already sliced to source position)
    ///
    /// # Arguments
    /// * `buf` - Source buffer (already positioned at read location)
    /// * `copy` - Hint: false = "cast if possible", true = "always deserialize"
    ///
    /// # Returns
    /// * Deserialized key
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self>;

    /// Get the maximum serialized size for this key type (matches C++ K::get_max_size())
    ///
    /// For fixed-size keys, this is the same as FIXED_SERIALIZED_SIZE.
    /// For variable-size keys, this is the maximum possible size.
    fn get_max_size() -> u32;
}

/// Trait for B-tree values (matches C++ BtreeValue concept)
///
/// Values must be:
/// - Sized: Known size at compile time
/// - Send + Sync: Thread-safe
/// - Clone: Can be copied
/// - Serializable: Can convert to/from bytes
/// - Debug: Can be formatted for logging/tracing
pub trait BtreeValue: Sized + Send + Sync + Clone + std::fmt::Debug {
    /// Fixed serialized size for this type (None for variable-sized values)
    /// SimpleNode requires Some(size) - it only works with fixed-size values
    const FIXED_SERIALIZED_SIZE: Option<u32>;

    /// Get the serialized size of this value in bytes
    fn serialized_size(&self) -> u32;

    /// Runtime fixed size check - returns Some(size) if this instance has fixed size
    /// Default implementation uses compile-time constant
    /// Types with runtime-determined fixed sizes (like DbValue) override this
    fn fixed_serialized_size(&self) -> Option<u32> { Self::FIXED_SERIALIZED_SIZE }

    /// Serialize value to buffer (buffer already sliced to target position)
    ///
    /// # Arguments
    /// * `buf` - Target buffer (already positioned at write location)
    /// * `copy` - Hint: false = "use fastest method" (direct memcpy if safe) true = "always use safe serialization"
    ///   (custom format)
    ///
    /// The implementation decides how to interpret this hint.
    ///
    /// # Returns
    /// * Number of bytes written
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32>;

    /// Deserialize value from buffer (buffer already sliced to source position)
    ///
    /// # Arguments
    /// * `buf` - Source buffer (already positioned at read location)
    /// * `copy` - Hint: false = "cast if possible", true = "always deserialize"
    ///
    /// # Returns
    /// * Deserialized value
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self>;

    /// Serialize value to BtreeBuffer (optional - has default implementation)
    ///
    /// Default implementation allocates a new buffer and calls serialize_to.
    /// Users can override for optimization (zero-copy, buffer pooling, etc.)
    ///
    /// # Returns
    /// * BtreeBuffer containing serialized value
    fn serialize_to_iobuffer(&self) -> io::Result<BtreeBuffer> {
        let size = self.serialized_size() as usize;
        let mut buffer = BtreeBuffer::new(size);
        self.serialize_to(&mut buffer.as_mut_slice(), false)?;
        Ok(buffer)
    }
}

//================================================================================
// Standard Implementations
//================================================================================

// Implementation for u64 (common case)
impl BtreeKey for u64 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);

    #[inline]
    fn serialized_size(&self) -> u32 { 8 }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            // Safe path: proper endianness handling
            buf[..8].copy_from_slice(&self.to_le_bytes());
        } else {
            // Fast path: direct memcpy (assumes native endian matches or doesn't matter)
            unsafe {
                std::ptr::copy_nonoverlapping(self as *const Self as *const u8, buf.as_mut_ptr(), 8);
            }
        }
        Ok(8)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u64::from_le_bytes(buf[..8].try_into().unwrap()))
        } else {
            unsafe { Ok(buf.as_ptr().cast::<u64>().read_unaligned()) }
        }
    }

    #[inline]
    fn get_max_size() -> u32 { 8 }
}

impl BtreeValue for u64 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);

    #[inline]
    fn serialized_size(&self) -> u32 { 8 }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            buf[..8].copy_from_slice(&self.to_le_bytes());
        } else {
            unsafe {
                std::ptr::copy_nonoverlapping(self as *const Self as *const u8, buf.as_mut_ptr(), 8);
            }
        }
        Ok(8)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u64::from_le_bytes(buf[..8].try_into().unwrap()))
        } else {
            unsafe { Ok(buf.as_ptr().cast::<u64>().read_unaligned()) }
        }
    }
}

// Implementation for u32
impl BtreeKey for u32 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(4);

    #[inline]
    fn serialized_size(&self) -> u32 { 4 }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            buf[..4].copy_from_slice(&self.to_le_bytes());
        } else {
            unsafe {
                std::ptr::copy_nonoverlapping(self as *const Self as *const u8, buf.as_mut_ptr(), 4);
            }
        }
        Ok(4)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u32::from_le_bytes(buf[..4].try_into().unwrap()))
        } else {
            unsafe { Ok(buf.as_ptr().cast::<u32>().read_unaligned()) }
        }
    }

    #[inline]
    fn get_max_size() -> u32 { 4 }
}

impl BtreeValue for u32 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(4);

    #[inline]
    fn serialized_size(&self) -> u32 { 4 }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            buf[..4].copy_from_slice(&self.to_le_bytes());
        } else {
            unsafe {
                std::ptr::copy_nonoverlapping(self as *const Self as *const u8, buf.as_mut_ptr(), 4);
            }
        }
        Ok(4)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u32::from_le_bytes(buf[..4].try_into().unwrap()))
        } else {
            unsafe { Ok(buf.as_ptr().cast::<u32>().read_unaligned()) }
        }
    }
}

//================================================================================
// Value Reference (for overflow support)
//================================================================================

/// Value that may be inline or stored in overflow node
/// Returned by NodeOps::get_nth_value() when value might be overflow
#[derive(Debug, Clone)]
pub enum ValueOrOverflow<V> {
    Inline(V),                                            // Value is stored inline in the node
    OverflowRef { node_id: BNodeId, overflow_size: u32 }, // Value is stored in overflow node
}

impl<V: BtreeValue> ValueOrOverflow<V> {
    /// Size of overflow reference when serialized: [node_id: 8 bytes][overflow_size: 4 bytes]
    pub const OVERFLOW_REFERENCE_SIZE: usize = 12;
    /// Get the serialized size this will occupy in the node
    #[inline]
    pub fn serialized_size(&self) -> usize {
        match self {
            ValueOrOverflow::Inline(v) => v.serialized_size() as usize,
            ValueOrOverflow::OverflowRef { .. } => Self::OVERFLOW_REFERENCE_SIZE,
        }
    }

    /// Serialize to buffer (either the value itself or the overflow reference)
    #[inline]
    pub fn serialize_to(&self, buf: &mut [u8]) -> std::io::Result<()> {
        match self {
            ValueOrOverflow::Inline(v) => {
                v.serialize_to(buf, true)?;
                Ok(())
            }
            ValueOrOverflow::OverflowRef { node_id, overflow_size } => {
                // Serialize overflow reference: [node_id: 8 bytes][overflow_size: 4 bytes]
                assert!(buf.len() >= Self::OVERFLOW_REFERENCE_SIZE);
                buf[0..8].copy_from_slice(&node_id.to_le_bytes());
                buf[8..12].copy_from_slice(&overflow_size.to_le_bytes());
                Ok(())
            }
        }
    }

    /// Check if this is an overflow reference
    #[inline]
    pub fn is_overflow(&self) -> bool { matches!(self, ValueOrOverflow::OverflowRef { .. }) }

    /// Deserialize from buffer (either the value itself or the overflow reference)
    #[inline]
    pub fn deserialize_from(buf: &[u8], is_overflow: bool, copy: bool) -> std::io::Result<Self> {
        if is_overflow {
            // Deserialize overflow reference: [node_id: 8 bytes][overflow_size: 4 bytes]
            assert!(buf.len() >= Self::OVERFLOW_REFERENCE_SIZE);
            let node_id = u64::from_le_bytes(buf[0..8].try_into().unwrap());
            let overflow_size = u32::from_le_bytes(buf[8..12].try_into().unwrap());
            Ok(ValueOrOverflow::OverflowRef { node_id, overflow_size })
        } else {
            let value = V::deserialize_from(buf, copy)?;
            Ok(ValueOrOverflow::Inline(value))
        }
    }

    /// Deserialize inline value from buffer (wraps result in ValueOrOverflow::Inline)
    #[inline]
    pub fn deserialize_inline(buf: &[u8], copy: bool) -> std::io::Result<Self> {
        let value = V::deserialize_from(buf, copy)?;
        Ok(ValueOrOverflow::Inline(value))
    }

    /// Extract inline value, panicking if this is an overflow reference
    #[inline]
    pub fn expect_inline(self, msg: &str) -> V {
        match self {
            ValueOrOverflow::Inline(v) => v,
            ValueOrOverflow::OverflowRef { .. } => panic!("{}", msg),
        }
    }

    /// Extract inline value, panicking with a default message if this is an overflow reference
    #[inline]
    pub fn unwrap_inline(self) -> V { self.expect_inline("called unwrap_inline() on an OverflowRef value") }

    /// Extract overflow node_id, panicking with a custom message if this is an inline value
    #[inline]
    pub fn expect_overflow(self, msg: &str) -> BNodeId {
        match self {
            ValueOrOverflow::Inline(_) => panic!("{}", msg),
            ValueOrOverflow::OverflowRef { node_id, .. } => node_id,
        }
    }

    /// Extract overflow node_id, panicking with a default message if this is an inline value
    #[inline]
    pub fn unwrap_overflow(self) -> BNodeId { self.expect_overflow("called unwrap_overflow() on an Inline value") }

    /// Build a ValueOrOverflow from a value, writing to overflow storage if needed
    ///
    /// Decides whether to store the value inline or in overflow storage based on size threshold.
    /// If the value exceeds the threshold, it's serialized and written to overflow storage.
    ///
    /// # Arguments
    /// * `storage` - The underlying storage interface
    /// * `value` - The value to store
    /// * `overflow_threshold` - Size threshold in bytes (values larger than this go to overflow)
    ///
    /// # Returns
    /// * `ValueOrOverflow::Inline(value.clone())` if size <= threshold
    /// * `ValueOrOverflow::OverflowRef { node_id, overflow_size }` if size > threshold
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
    pub async fn build<S>(storage: &S, value: &V, overflow_threshold: u32) -> Result<Self, BtreeError>
    where
        S: super::btree::UnderlyingBtree + ?Sized,
    {
        let value_size = value.serialized_size();

        if value_size > overflow_threshold {
            // Serialize value to IOBuffer (no clone needed)
            let iobuf = value.serialize_to_iobuffer().map_err(BtreeError::Io)?;

            // Write to overflow storage
            let node_id = storage.write_overflow(iobuf).await?;
            Ok(ValueOrOverflow::OverflowRef { node_id, overflow_size: value_size })
        } else {
            // Store inline (clone only for inline case)
            Ok(ValueOrOverflow::Inline(value.clone()))
        }
    }

    /// Resolve a ValueOrOverflow to an actual value
    ///
    /// If the value is inline, returns it directly (with optional clone).
    /// If the value is an overflow reference, reads from overflow storage and deserializes.
    ///
    /// # Arguments
    /// * `storage` - The underlying storage interface
    /// * `copy` - Whether to copy the value during deserialization
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
    pub async fn resolve<S>(self, storage: &S, copy: bool) -> Result<V, BtreeError>
    where
        S: super::btree::UnderlyingBtree + ?Sized,
    {
        match self {
            ValueOrOverflow::Inline(v) => Ok(v),
            ValueOrOverflow::OverflowRef { node_id, overflow_size: _ } => {
                // Read overflow node and deserialize value
                let iobuf = storage.read_overflow(node_id).await?;
                V::deserialize_from(iobuf.as_slice(), copy).map_err(BtreeError::Io)
            }
        }
    }
}

// TODO: Add implementations for other common types:
// - i64, i32
// - String (variable length)
// - Vec<u8> (variable length)
// - Custom struct support via derive macro

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_u64_serialization() {
        let key: u64 = 0x0123456789ABCDEF;
        let mut buf = vec![0u8; 8];

        let written = <u64 as BtreeKey>::serialize_to(&key, &mut buf, true).unwrap();
        assert_eq!(written, 8);

        let restored = <u64 as BtreeKey>::deserialize_from(&buf, true).unwrap();
        assert_eq!(key, restored);
    }

    #[test]
    fn test_u64_serialization_nocopy() {
        let key: u64 = 0x0123456789ABCDEF;
        let mut buf = vec![0u8; 8];

        let written = <u64 as BtreeKey>::serialize_to(&key, &mut buf, false).unwrap();
        assert_eq!(written, 8);

        let restored = <u64 as BtreeKey>::deserialize_from(&buf, false).unwrap();
        assert_eq!(key, restored);
    }

    #[test]
    fn test_u64_iobuffer() {
        let key: u64 = 42;
        let iobuf = <u64 as BtreeValue>::serialize_to_iobuffer(&key).unwrap();
        assert_eq!(iobuf.len(), 8); // IOBuffer aligns to 4096 bytes

        let restored = <u64 as BtreeValue>::deserialize_from(iobuf.as_slice(), true).unwrap();
        assert_eq!(key, restored);
    }

    #[test]
    fn test_u32_compare() {
        let k1: u32 = 10;
        let k2: u32 = 20;
        let k3: u32 = 10;

        assert_eq!(k1.cmp(&k2), std::cmp::Ordering::Less);
        assert_eq!(k2.cmp(&k1), std::cmp::Ordering::Greater);
        assert_eq!(k1.cmp(&k3), std::cmp::Ordering::Equal);
    }
}
