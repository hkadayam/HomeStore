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

//! Btree Key and Value Traits
//!
//! This module defines the traits for keys and values in the B-tree.
//! Corresponds to C++ BtreeKey and BtreeValue concepts.

use std::io;
use iomgr::IOBuffer;

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

    /// Serialize key to buffer (buffer already sliced to target position)
    ///
    /// # Arguments
    /// * `buf` - Target buffer (already positioned at write location)
    /// * `copy` - Hint: false = "use fastest method" (direct memcpy if safe)
    ///                  true = "always use safe serialization" (custom format)
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

    /// Serialize value to buffer (buffer already sliced to target position)
    ///
    /// # Arguments
    /// * `buf` - Target buffer (already positioned at write location)
    /// * `copy` - Hint: false = "use fastest method" (direct memcpy if safe)
    ///                  true = "always use safe serialization" (custom format)
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

    /// Serialize value to IOBuffer (optional - has default implementation)
    ///
    /// Default implementation allocates a new buffer and calls serialize_to.
    /// Users can override for optimization (zero-copy, buffer pooling, etc.)
    ///
    /// # Returns
    /// * IOBuffer containing serialized value
    fn serialize_to_iobuffer(&self) -> io::Result<IOBuffer> {
        let size = self.serialized_size() as usize;
        let mut buffer = IOBuffer::new(size);
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
    fn serialized_size(&self) -> u32 {
        8
    }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            // Safe path: proper endianness handling
            buf[..8].copy_from_slice(&self.to_le_bytes());
        } else {
            // Fast path: direct memcpy (assumes native endian matches or doesn't matter)
            unsafe {
                std::ptr::copy_nonoverlapping(
                    self as *const Self as *const u8,
                    buf.as_mut_ptr(),
                    8
                );
            }
        }
        Ok(8)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u64::from_le_bytes(buf[..8].try_into().unwrap()))
        } else {
            // Fast path: direct cast
            unsafe { Ok(*(buf.as_ptr() as *const u64)) }
        }
    }

    #[inline]
    fn get_max_size() -> u32 {
        8
    }
}

impl BtreeValue for u64 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);

    #[inline]
    fn serialized_size(&self) -> u32 {
        8
    }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            buf[..8].copy_from_slice(&self.to_le_bytes());
        } else {
            unsafe {
                std::ptr::copy_nonoverlapping(
                    self as *const Self as *const u8,
                    buf.as_mut_ptr(),
                    8
                );
            }
        }
        Ok(8)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u64::from_le_bytes(buf[..8].try_into().unwrap()))
        } else {
            unsafe { Ok(*(buf.as_ptr() as *const u64)) }
        }
    }
}

// Implementation for u32
impl BtreeKey for u32 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(4);

    #[inline]
    fn serialized_size(&self) -> u32 {
        4
    }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            buf[..4].copy_from_slice(&self.to_le_bytes());
        } else {
            unsafe {
                std::ptr::copy_nonoverlapping(
                    self as *const Self as *const u8,
                    buf.as_mut_ptr(),
                    4
                );
            }
        }
        Ok(4)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u32::from_le_bytes(buf[..4].try_into().unwrap()))
        } else {
            unsafe { Ok(*(buf.as_ptr() as *const u32)) }
        }
    }

    #[inline]
    fn get_max_size() -> u32 {
        4
    }
}

impl BtreeValue for u32 {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(4);

    #[inline]
    fn serialized_size(&self) -> u32 {
        4
    }

    #[inline]
    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> io::Result<u32> {
        if copy {
            buf[..4].copy_from_slice(&self.to_le_bytes());
        } else {
            unsafe {
                std::ptr::copy_nonoverlapping(
                    self as *const Self as *const u8,
                    buf.as_mut_ptr(),
                    4
                );
            }
        }
        Ok(4)
    }

    #[inline]
    fn deserialize_from(buf: &[u8], copy: bool) -> io::Result<Self> {
        if copy {
            Ok(u32::from_le_bytes(buf[..4].try_into().unwrap()))
        } else {
            unsafe { Ok(*(buf.as_ptr() as *const u32)) }
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
        assert_eq!(iobuf.len(), 4096); // IOBuffer aligns to 4096 bytes

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
