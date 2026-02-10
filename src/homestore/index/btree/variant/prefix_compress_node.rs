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
 ****************** */

//! Prefix Compression Node Implementation
//!
//! This node variant implements dynamic prefix compression for keys with common prefixes,
//! optimized for database secondary indexes where keys share common row IDs.
//!
//! Memory layout:
//! [PersistentHeader][PrefixCompressHeader][Record₁][Record₂]...[free space]...[Data₂][Data₁]
//!                                         ^--- Entries grow right       Data grows left ↓
//!
//! Key features:
//! - Variable-length prefix sharing with refcounting
//! - Configurable prefix size (0 = dynamic, >0 = fixed split)
//! - On-failure compaction to reclaim fragmented space
//! - Logical copy for move operations (natural compression)

use super::super::btree::BtreeError;
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree_node::{NodeCore, NodeOps, PersistentHeader};
use super::super::detail::btree_req::BtreePutType;
use smallvec::{SmallVec, smallvec};
use std::io;

//================================================================================
// Helper Macros
//================================================================================

/// Simplifies BtreeError::Io creation
macro_rules! btree_io_err {
    ($kind:expr, $msg:expr) => {
        Err(BtreeError::Io(io::Error::new($kind, $msg)))
    };
}

//================================================================================
// Prefix Compression Node Header
//================================================================================

/// Prefix compression node header (comes after PersistentHeader)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct PrefixCompressHeader {
    pub tail_offset: u16, // Where free space starts (data grows left from here)
    pub hole_size: u16,   // Total fragmented space from deletions
}

impl PrefixCompressHeader {
    #[inline]
    pub const fn size() -> usize { std::mem::size_of::<Self>() }

    pub fn init(&mut self, node_data_size: u16) {
        self.tail_offset = node_data_size;
        self.hole_size = 0;
    }
}

//================================================================================
// Prefix Info Structure (Raw header in memory)
//================================================================================

/// Prefix information header (stored in node buffer)
/// Layout in memory: [refcount: u16][prefix_bytes...]
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct PrefixInfo {
    pub refcount: u16,
    // Followed by: prefix key bytes
}

impl PrefixInfo {
    #[inline]
    pub const fn header_size() -> usize { std::mem::size_of::<Self>() }

    #[inline]
    pub fn total_size(prefix_len: u16) -> usize { Self::header_size() + prefix_len as usize }
}

//================================================================================
// Record - Entry metadata (10 bytes on disk + slot_num in memory)
//================================================================================

/// On-disk serialized record format
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
struct RecordData {
    prefix_offset: u16,
    prefix_size: u16,
    suffix_size: u16,
    value_size: u16,
    kv_offset: u16,
}

/// In-memory record with slot tracking
#[derive(Debug, Clone, Copy)]
pub struct Record {
    pub slot_num: u32,
    pub data: RecordData,
}

impl Record {
    #[inline]
    const fn serialized_size() -> usize { std::mem::size_of::<RecordData>() }

    /// Create a new record with prefix and suffix+value
    fn create(
        ops: &PrefixCompressNodeOps,
        core: &NodeCore,
        slot_num: u32,
        prefix: &Prefix,
        suffix_kv: &SuffixAndValue,
    ) -> Self {
        ops.insert_record_slot(core, slot_num);
        let record = Self {
            slot_num,
            data: RecordData {
                prefix_offset: prefix.offset(),
                prefix_size: prefix.size(),
                suffix_size: suffix_kv.suffix_size,
                value_size: suffix_kv.value_size,
                kv_offset: suffix_kv.offset(),
            },
        };
        record.write(core);
        record
    }

    /// Load an existing record from a slot
    fn from_slot(core: &NodeCore, slot_num: u32) -> Self {
        let offset = std::mem::size_of::<PersistentHeader>()
            + PrefixCompressHeader::size()
            + (slot_num as usize * Self::serialized_size());

        unsafe {
            let ptr = core.phys_buf.as_ptr().add(offset) as *const RecordData;
            Self {
                slot_num,
                data: std::ptr::read_unaligned(ptr),
            }
        }
    }

    /// Write this record to its slot
    fn write(&self, core: &NodeCore) {
        let offset = std::mem::size_of::<PersistentHeader>()
            + PrefixCompressHeader::size()
            + (self.slot_num as usize * Self::serialized_size());

        unsafe {
            let ptr = core.phys_buf.as_ptr().add(offset) as *mut RecordData;
            std::ptr::write_unaligned(ptr, self.data);
        }
    }

    /// Check if this is a standalone record (no suffix)
    #[inline]
    fn is_standalone(&self) -> bool { self.data.suffix_size == 0 }
}

//================================================================================
// Prefix - Manages prefix data with refcounting
//================================================================================

/// Handle to a prefix stored in the node buffer
#[derive(Debug, Clone, Copy)]
pub struct Prefix {
    offset: u16,
    size: u16,
}

impl Prefix {
    /// Create prefix handle with pre-allocated offset
    fn new(offset: u16, size: u16) -> Self { Self { offset, size } }

    /// Load existing prefix from a record
    fn from_record(record: Record) -> Self {
        Self {
            offset: record.data.prefix_offset,
            size: record.data.prefix_size,
        }
    }

    /// Write prefix (refcount + data) to pre-allocated buffer location
    fn write(&self, ops: &PrefixCompressNodeOps, core: &NodeCore, key: &[u8]) {
        let base = std::mem::size_of::<PersistentHeader>();

        // Write PrefixInfo header
        unsafe {
            let ptr = core.phys_buf.as_ptr().add(base + self.offset as usize) as *mut PrefixInfo;
            std::ptr::write_unaligned(ptr, PrefixInfo { refcount: 1 });
        }

        // Write prefix data
        ops.write_bytes_at(core, self.offset as usize + PrefixInfo::header_size(), key);
    }

    /// Calculate total space needed for prefix
    #[inline]
    fn space_needed(key_len: usize) -> usize { PrefixInfo::total_size(key_len as u16) }

    /// Update only the refcount (for inc/dec operations)
    fn write_refcount(&self, core: &NodeCore, refcount: u16) {
        let base = std::mem::size_of::<PersistentHeader>();
        unsafe {
            let ptr = core.phys_buf.as_ptr().add(base + self.offset as usize) as *mut PrefixInfo;
            (*ptr).refcount = refcount;
        }
    }

    /// Get current refcount
    fn refcount(&self, core: &NodeCore) -> u16 {
        let base = std::mem::size_of::<PersistentHeader>();
        unsafe {
            let ptr = core.phys_buf.as_ptr().add(base + self.offset as usize) as *const PrefixInfo;
            (*ptr).refcount
        }
    }

    /// Increment refcount
    fn inc_refcount(&self, core: &NodeCore) {
        let current = self.refcount(core);
        self.write_refcount(core, current + 1);
    }

    /// Decrement refcount and return new value
    fn dec_refcount(&self, core: &NodeCore) -> u16 {
        let new_count = self.refcount(core) - 1;
        self.write_refcount(core, new_count);
        new_count
    }

    /// Get prefix key bytes
    fn prefix_key<'a>(&self, core: &'a NodeCore) -> &'a [u8] {
        let base = std::mem::size_of::<PersistentHeader>();
        let data_offset = base + self.offset as usize + PrefixInfo::header_size();
        unsafe { std::slice::from_raw_parts(core.phys_buf.as_ptr().add(data_offset), self.size as usize) }
    }

    /// Get offset
    #[inline]
    fn offset(&self) -> u16 { self.offset }

    /// Get size
    #[inline]
    fn size(&self) -> u16 { self.size }

    /// Get total size (header + data)
    #[inline]
    fn total_size(&self) -> usize { PrefixInfo::total_size(self.size) }
}

//================================================================================
// SuffixAndValue - Manages suffix+value data
//================================================================================

/// Handle to suffix and value data in the node buffer
#[derive(Debug, Clone, Copy)]
pub struct SuffixAndValue {
    offset: u16,
    suffix_size: u16,
    value_size: u16,
}

impl SuffixAndValue {
    /// Create suffix+value handle with pre-allocated offset
    fn new(offset: u16, suffix_size: u16, value_size: u16) -> Self { Self { offset, suffix_size, value_size } }

    /// Write suffix+value to pre-allocated buffer location
    fn write<V: BtreeValue>(
        &self,
        ops: &PrefixCompressNodeOps,
        core: &NodeCore,
        suffix: &[u8],
        val: &ValueOrOverflow<V>,
    ) {
        ops.write_bytes_at(core, self.offset as usize, suffix);
        ops.write_value_at(core, self.offset as usize + suffix.len(), val, self.value_size as usize);
    }

    /// Calculate total space needed for suffix+value
    #[inline]
    fn space_needed(suffix_len: usize, value_size: usize) -> usize { suffix_len + value_size }

    /// Get suffix key bytes
    fn suffix_key<'a>(&self, core: &'a NodeCore) -> &'a [u8] {
        let base = std::mem::size_of::<PersistentHeader>();
        unsafe {
            std::slice::from_raw_parts(
                core.phys_buf.as_ptr().add(base + self.offset as usize),
                self.suffix_size as usize,
            )
        }
    }

    /// Get value
    fn value<V: BtreeValue>(&self, core: &NodeCore) -> V {
        let base = std::mem::size_of::<PersistentHeader>();
        let val_offset = base + self.offset as usize + self.suffix_size as usize;
        unsafe {
            let val_slice =
                std::slice::from_raw_parts(core.phys_buf.as_ptr().add(val_offset), self.value_size as usize);
            V::deserialize_from(val_slice, true).expect("Value deserialization should not fail")
        }
    }

    /// Get offset
    #[inline]
    fn offset(&self) -> u16 { self.offset }
}

//================================================================================
// Prefix Compression Node Operations
//================================================================================
pub struct PrefixCompressNodeOps {
    expected_prefix_size: u16, // Config: 0 = dynamic, >0 = fixed split point
    min_prefix_threshold: u16, // Minimum bytes to consider prefix sharing (default: 3)
}

impl PrefixCompressNodeOps {
    pub const fn new(expected_prefix_size: u16) -> Self {
        Self {
            expected_prefix_size,
            min_prefix_threshold: 3,
        }
    }
}

impl<K, V> NodeOps<K, V> for PrefixCompressNodeOps
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
{
    fn init_new_node(&self, core: &NodeCore) {
        let node_data_size = (core.node_size() - std::mem::size_of::<PersistentHeader>() as u32) as u16;
        let header = self.get_header_mut(core);
        header.init(node_data_size);
    }

    fn get_all_kvs(&self, core: &NodeCore) -> Vec<(K, ValueOrOverflow<V>)> {
        let nentries = core.nentries();
        let mut result = Vec::with_capacity(nentries as usize);

        for idx in 0..nentries {
            let key: K = <Self as NodeOps<K, V>>::get_nth_key(self, core, idx, true);
            let val = <Self as NodeOps<K, V>>::get_nth_value(self, core, idx, true);
            result.push((key, val));
        }

        result
    }

    fn insert(&self, core: &NodeCore, idx: u32, key: &K, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let key_size = key.serialized_size() as usize;
        let val_size = val.serialized_size();

        // Serialize key to temp buffer for matching (stack allocation for small keys)
        let mut key_buf: SmallVec<[u8; 128]> = smallvec![0u8; key_size];
        let _ = key.serialize_to(&mut key_buf, false);

        // Check if we have space, compact if needed
        if !self.has_space_for::<K, V>(core, key_size, val_size) {
            let header = self.get_header(core);
            if header.hole_size > 0 {
                self.compact(core)?;
                // Retry after compaction
                if !self.has_space_for::<K, V>(core, key_size, val_size) {
                    return btree_io_err!(io::ErrorKind::OutOfMemory, "Not enough space even after compaction");
                }
            } else {
                return btree_io_err!(io::ErrorKind::OutOfMemory, "Not enough space for insertion");
            }
        }

        // Find best prefix match with neighbors
        if let Some((prefix, _match_len, _match_idx)) = self.find_best_prefix_match(core, &key_buf, idx) {
            self.insert_with_prefix(core, idx, prefix, &key_buf, val);
        } else if self.expected_prefix_size > 0 && key_size > self.expected_prefix_size as usize {
            // No suitable match, insert as standalone or with configured split
            self.insert_with_split(core, idx, &key_buf, val, self.expected_prefix_size);
        } else {
            self.insert_standalone(core, idx, &key_buf, val);
        }

        core.inc_gen();
        Ok(())
    }

    fn update(&self, core: &NodeCore, idx: u32, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        // PrefixCompressNode: For simplicity, do remove+insert for updates
        // (Only fetch key when actually needed for re-insertion)
        let key: K = <Self as NodeOps<K, V>>::get_nth_key(self, core, idx, true);
        <Self as NodeOps<K, V>>::remove(self, core, idx)?;
        <Self as NodeOps<K, V>>::insert(self, core, idx, &key, val)?;
        Ok(())
    }

    fn update_with_key(&self, core: &NodeCore, idx: u32, key: &K, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        <Self as NodeOps<K, V>>::remove(self, core, idx)?;
        <Self as NodeOps<K, V>>::insert(self, core, idx, key, val)?;
        Ok(())
    }

    fn remove(&self, core: &NodeCore, idx: u32) -> Result<(), BtreeError> {
        let nentries = core.nentries();
        if idx >= nentries {
            return btree_io_err!(
                io::ErrorKind::InvalidInput,
                format!("Index {} out of bounds (total: {})", idx, nentries)
            );
        }

        let record = Record::from_slot(core, idx);
        let header = self.get_header_mut(core);

        // Handle prefix data
        if !record.is_standalone() {
            // Shared prefix: decrement refcount
            let prefix = Prefix::from_record(record);
            let new_refcount = prefix.dec_refcount(core);

            if new_refcount == 0 {
                // Free prefix data
                header.hole_size += prefix.total_size() as u16;
            }

            // Free suffix+value
            let kv_size = (record.data.suffix_size + record.data.value_size) as u16;
            header.hole_size += kv_size;
        } else {
            // Standalone: free full key+value (prefix + value)
            let prefix = Prefix::from_record(record);
            header.hole_size += (prefix.total_size() + record.data.value_size as usize) as u16;
        }

        // Remove record slot
        self.remove_record_slot(core, idx);

        core.inc_gen();
        Ok(())
    }

    fn remove_range(&self, core: &NodeCore, start_idx: u32, end_idx: u32) -> Result<(), BtreeError> {
        for idx in (start_idx..=end_idx).rev() {
            <Self as NodeOps<K, V>>::remove(self, core, idx)?;
        }
        Ok(())
    }

    fn remove_all(&self, core: &NodeCore) {
        let nentries = core.nentries();
        for _ in 0..nentries {
            <Self as NodeOps<K, V>>::remove(self, core, 0).ok();
        }

        // Reset header
        let node_data_size = (core.node_size() - std::mem::size_of::<PersistentHeader>() as u32) as u16;
        let header = self.get_header_mut(core);
        header.init(node_data_size);

        core.inc_gen();
    }

    fn get_nth_key(&self, core: &NodeCore, idx: u32, _copy: bool) -> K {
        let record = Record::from_slot(core, idx);

        if record.data.suffix_size == 0 {
            // Standalone: full key at prefix_offset (skip refcount)
            let key_data = self.get_prefix_data(core, record.data.prefix_offset, record.data.prefix_size);
            K::deserialize_from(key_data, true).expect("Key deserialization failed")
        } else {
            // Compressed: reconstruct from prefix + suffix
            let prefix_data = self.get_prefix_data(core, record.data.prefix_offset, record.data.prefix_size);
            let suffix_data = self.get_data_slice(core, record.data.kv_offset, record.data.suffix_size);

            // Combine prefix and suffix (stack allocation for small keys)
            let total_len = (record.data.prefix_size + record.data.suffix_size) as usize;
            let mut combined: SmallVec<[u8; 128]> = smallvec![0u8; total_len];
            combined[..record.data.prefix_size as usize].copy_from_slice(prefix_data);
            combined[record.data.prefix_size as usize..].copy_from_slice(suffix_data);

            K::deserialize_from(&combined, true).expect("Key deserialization failed")
        }
    }

    fn get_nth_value(&self, core: &NodeCore, idx: u32, _copy: bool) -> ValueOrOverflow<V> {
        let record = Record::from_slot(core, idx);

        let value_offset = if record.data.suffix_size == 0 {
            // Standalone: value immediately after key (skip refcount)
            record.data.prefix_offset + PrefixInfo::header_size() as u16 + record.data.prefix_size
        } else {
            // Compressed: value after suffix
            record.data.kv_offset + record.data.suffix_size
        };

        let value_data = self.get_data_slice(core, value_offset, record.data.value_size);
        let value = V::deserialize_from(value_data, true).expect("Value deserialization failed");

        // PrefixCompressNode: always inline (no overflow support yet)
        ValueOrOverflow::Inline(value)
    }

    fn is_nth_value_overflow(&self, _core: &NodeCore, _idx: u32) -> bool {
        // PrefixCompressNode: always inline, no overflow support yet
        false
    }

    fn move_out_to_right_by_entries(&self, core: &NodeCore, other: &NodeCore, num_entries: u32) -> u32 {
        let nentries = core.nentries();
        if nentries == 0 || num_entries == 0 {
            return 0;
        }

        let to_move = std::cmp::min(num_entries, nentries);
        let start_idx = nentries - to_move;

        let mut moved = 0;
        for idx in (start_idx..nentries).rev() {
            let key: K = <Self as NodeOps<K, V>>::get_nth_key(self, core, idx, true);
            let val_ref = <Self as NodeOps<K, V>>::get_nth_value(self, core, idx, true);

            // Insert into other node (will compress naturally)
            if <Self as NodeOps<K, V>>::insert(self, other, 0, &key, &val_ref).is_ok() {
                <Self as NodeOps<K, V>>::remove(self, core, idx).ok();
                moved += 1;
            } else {
                break;
            }
        }

        moved
    }

    fn move_out_to_right_by_size(&self, core: &NodeCore, other: &NodeCore, size_to_move: u32) -> u32 {
        let nentries = core.nentries();
        if nentries == 0 {
            return 0;
        }

        let mut moved = 0;
        let mut moved_size = 0u32;
        let mut idx = nentries - 1;

        loop {
            let key: K = <Self as NodeOps<K, V>>::get_nth_key(self, core, idx, true);
            let val_ref = <Self as NodeOps<K, V>>::get_nth_value(self, core, idx, true);

            let val_size = val_ref.serialized_size();
            let record_size = key.serialized_size() + val_size as u32 + Record::serialized_size() as u32;

            if moved_size + record_size > size_to_move && moved > 0 {
                break;
            }

            // Insert into other node
            if <Self as NodeOps<K, V>>::insert(self, other, 0, &key, &val_ref).is_ok() {
                <Self as NodeOps<K, V>>::remove(self, core, idx).ok();
                moved += 1;
                moved_size += record_size;

                if idx == 0 {
                    break;
                }
                idx -= 1;
            } else {
                break;
            }
        }

        moved
    }

    fn append_copy_in_upto_size(
        &self,
        core: &NodeCore,
        other: &NodeCore,
        other_cursor: &mut u32,
        upto_size: u32,
        copy_only_if_fits: bool,
    ) -> bool {
        let other_nentries = other.nentries();
        if *other_cursor >= other_nentries {
            return true;
        }

        let mut copied_size = 0u32;
        let start_cursor = *other_cursor;

        while *other_cursor < other_nentries {
            let key: K = <Self as NodeOps<K, V>>::get_nth_key(self, other, *other_cursor, true);
            let val_ref = <Self as NodeOps<K, V>>::get_nth_value(self, other, *other_cursor, true);

            let val_size = val_ref.serialized_size();
            let record_size = key.serialized_size() + val_size as u32 + Record::serialized_size() as u32;

            if copied_size + record_size > upto_size {
                if copy_only_if_fits && *other_cursor > start_cursor {
                    // Revert
                    *other_cursor = start_cursor;
                    return false;
                }
                break;
            }

            if <Self as NodeOps<K, V>>::insert(self, core, core.nentries(), &key, &val_ref).is_err() {
                break;
            }

            copied_size += record_size;
            *other_cursor += 1;
        }

        *other_cursor == other_nentries || copied_size > 0
    }

    fn available_size(&self, core: &NodeCore) -> u32 {
        let header = self.get_header(core);
        let immediate = self.immediate_available_space(core);
        immediate + header.hole_size as u32
    }

    fn has_room_for_put(&self, core: &NodeCore, _put_type: BtreePutType, key_size: u32, value_size: u32) -> bool {
        self.has_space_for::<K, V>(core, key_size as usize, value_size as usize)
    }
}

//================================================================================
// Helper Methods
//================================================================================

impl PrefixCompressNodeOps {
    // Header access
    #[inline]
    fn get_header<'a>(&self, core: &'a NodeCore) -> &'a PrefixCompressHeader {
        let offset = std::mem::size_of::<PersistentHeader>();
        unsafe { &*(core.phys_buf.as_ptr().add(offset) as *const PrefixCompressHeader) }
    }

    #[inline]
    fn get_header_mut<'a>(&self, core: &'a NodeCore) -> &'a mut PrefixCompressHeader {
        let offset = std::mem::size_of::<PersistentHeader>();
        unsafe { &mut *(core.phys_buf.as_ptr().add(offset) as *mut PrefixCompressHeader) }
    }

    // Prefix data access
    #[inline]
    fn get_prefix_data<'a>(&self, core: &'a NodeCore, offset: u16, len: u16) -> &'a [u8] {
        let base_offset = std::mem::size_of::<PersistentHeader>();
        let prefix_start = base_offset + offset as usize + PrefixInfo::header_size();
        unsafe { std::slice::from_raw_parts(core.phys_buf.as_ptr().add(prefix_start), len as usize) }
    }

    #[inline]
    fn get_data_slice<'a>(&self, core: &'a NodeCore, offset: u16, len: u16) -> &'a [u8] {
        let base_offset = std::mem::size_of::<PersistentHeader>();
        unsafe { std::slice::from_raw_parts(core.phys_buf.as_ptr().add(base_offset + offset as usize), len as usize) }
    }

    fn immediate_available_space(&self, core: &NodeCore) -> u32 {
        let header = self.get_header(core);
        let entries_end = PrefixCompressHeader::size() + (core.nentries() as usize * Record::serialized_size());

        if header.tail_offset as usize > entries_end {
            (header.tail_offset as usize - entries_end) as u32
        } else {
            0
        }
    }

    fn has_space_for<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self,
        core: &NodeCore,
        key_len: usize,
        val_len: usize,
    ) -> bool {
        let needed = Record::serialized_size() + PrefixInfo::total_size(key_len as u16) + val_len;
        <Self as NodeOps<K, V>>::available_size(self, core) >= needed as u32
    }

    // Find best prefix match with neighbors and return ready-to-use Prefix
    fn find_best_prefix_match(&self, core: &NodeCore, key: &[u8], idx: u32) -> Option<(Prefix, usize, u32)> {
        let nentries = core.nentries();
        let mut best_match: Option<(Prefix, usize, u32)> = None;

        // Check previous record
        if idx > 0 {
            if let Some((prefix, match_len)) = self.check_prefix_match(core, idx - 1, key) {
                best_match = Some((prefix, match_len, idx - 1));
            }
        }

        // Check next record
        if idx < nentries {
            if let Some((prefix, match_len)) = self.check_prefix_match(core, idx, key) {
                if match_len > best_match.as_ref().map(|(_, len, _)| *len).unwrap_or(0) {
                    best_match = Some((prefix, match_len, idx));
                }
            }
        }

        // If we found a match, prepare the prefix for sharing
        best_match.map(|(_prefix, match_len, match_idx)| {
            let prepared_prefix = self.prepare_prefix_for_sharing(core, match_idx, match_len, key);
            (prepared_prefix, match_len, match_idx)
        })
    }

    fn check_prefix_match(&self, core: &NodeCore, idx: u32, key: &[u8]) -> Option<(Prefix, usize)> {
        let record = Record::from_slot(core, idx);
        let prefix = Prefix::from_record(record);
        let compare_key = prefix.prefix_key(core);

        // Count common prefix bytes
        let min_len = std::cmp::min(key.len(), compare_key.len());
        let mut match_len = 0;
        for i in 0..min_len {
            if key[i] == compare_key[i] {
                match_len += 1;
            } else {
                break;
            }
        }

        if record.is_standalone() {
            // Standalone: can create new prefix if match >= threshold
            if match_len >= self.min_prefix_threshold as usize {
                return Some((prefix, match_len));
            }
        } else {
            // Already compressed: only reuse if we match the FULL prefix
            if match_len == prefix.size() as usize && match_len >= self.min_prefix_threshold as usize {
                return Some((prefix, match_len));
            }
        }

        None
    }

    // Prepare a prefix for sharing: convert standalone to shared if needed
    fn prepare_prefix_for_sharing(&self, core: &NodeCore, match_idx: u32, match_len: usize, _key: &[u8]) -> Prefix {
        let mut record = Record::from_slot(core, match_idx);
        let prefix = Prefix::from_record(record);

        if record.is_standalone() {
            // Standalone: just reinterpret the existing data layout
            // Data is already: [refcount][prefix][suffix][value]
            // We just adjust the record to mark where suffix starts

            let suffix_len = record.data.prefix_size - match_len as u16;
            record.data.prefix_size = match_len as u16;
            record.data.suffix_size = suffix_len;
            record.data.kv_offset = record.data.prefix_offset + PrefixInfo::header_size() as u16 + match_len as u16;
            record.write(core);

            // Increment refcount (now shared)
            prefix.inc_refcount(core);
        }

        prefix
    }

    // Insert with shared prefix
    fn insert_with_prefix<V: BtreeValue>(
        &self,
        core: &NodeCore,
        idx: u32,
        prefix: Prefix,
        key_buf: &[u8],
        val: &ValueOrOverflow<V>,
    ) {
        // Increment refcount on shared prefix and write to buffer
        prefix.inc_refcount(core);

        // Allocate space only for suffix+value and write suffix+value
        let suffix_key = &key_buf[prefix.size() as usize..];
        let val_size = val.serialized_size();
        let sv_offset = self.alloc_space(core, SuffixAndValue::space_needed(suffix_key.len(), val_size));

        let suffix_kv = SuffixAndValue::new(sv_offset, suffix_key.len() as u16, val_size as u16);
        suffix_kv.write(self, core, suffix_key, val);

        // Create record
        Record::create(self, core, idx, &prefix, &suffix_kv);
    }

    // Insert with configured split point
    fn insert_with_split<V: BtreeValue>(
        &self,
        core: &NodeCore,
        idx: u32,
        key_buf: &[u8],
        val: &ValueOrOverflow<V>,
        split_point: u16,
    ) {
        let split = split_point as usize;
        let val_size = val.serialized_size();

        // Allocate and write prefix
        let prefix_offset = self.alloc_space(core, Prefix::space_needed(split));
        let prefix = Prefix::new(prefix_offset, split as u16);
        prefix.write(self, core, &key_buf[..split]);

        // Allocate and write suffix+value
        let suffix_key = &key_buf[split..];
        let sv_offset = self.alloc_space(core, SuffixAndValue::space_needed(suffix_key.len(), val_size));
        let suffix_kv = SuffixAndValue::new(sv_offset, suffix_key.len() as u16, val_size as u16);
        suffix_kv.write(self, core, suffix_key, val);

        // Create record
        Record::create(self, core, idx, &prefix, &suffix_kv);
    }

    // Insert standalone (no compression)
    // Allocates [refcount][key][value] all contiguous
    fn insert_standalone<V: BtreeValue>(&self, core: &NodeCore, idx: u32, key_buf: &[u8], val: &ValueOrOverflow<V>) {
        let val_size = val.serialized_size();

        // Allocate space
        let total_size = Prefix::space_needed(key_buf.len()) + val_size;
        let prefix_offset = self.alloc_space(core, total_size);
        let value_offset = prefix_offset + Prefix::space_needed(key_buf.len()) as u16;

        // Create and write prefix (full key) and value
        let prefix = Prefix::new(prefix_offset, key_buf.len() as u16);
        prefix.write(self, core, key_buf);

        let suffix_kv = SuffixAndValue::new(value_offset, 0, val_size as u16); // Empty suffix
        suffix_kv.write(self, core, &[], val);

        // Create record
        Record::create(self, core, idx, &prefix, &suffix_kv);
    }

    // Write raw bytes at offset
    #[inline]
    fn write_bytes_at(&self, core: &NodeCore, offset: usize, data: &[u8]) {
        let base = std::mem::size_of::<PersistentHeader>();
        unsafe {
            let dst = core.phys_buf.as_ptr().add(base + offset) as *mut u8;
            std::ptr::copy_nonoverlapping(data.as_ptr(), dst, data.len());
        }
    }

    // Write value or overflow reference by serializing directly at offset
    #[inline]
    fn write_value_at<V: BtreeValue>(&self, core: &NodeCore, offset: usize, val: &ValueOrOverflow<V>, size: usize) {
        let base = std::mem::size_of::<PersistentHeader>();
        unsafe {
            let dst = core.phys_buf.as_ptr().add(base + offset) as *mut u8;
            let dst_slice = std::slice::from_raw_parts_mut(dst, size);
            let _ = val.serialize_to(dst_slice);
        }
    }

    // Allocate data from tail (space check done before calling this)
    fn alloc_space(&self, core: &NodeCore, size: usize) -> u16 {
        let header = self.get_header_mut(core);
        header.tail_offset -= size as u16;
        header.tail_offset
    }

    // Insert record slot (shifts existing records to make room)
    fn insert_record_slot(&self, core: &NodeCore, idx: u32) {
        let nentries = core.nentries();
        let base = std::mem::size_of::<PersistentHeader>();

        // Shift entries to make room
        if idx < nentries {
            let src_offset = base + PrefixCompressHeader::size() + (idx as usize * Record::serialized_size());
            let dst_offset = src_offset + Record::serialized_size();
            let count = (nentries - idx) as usize * Record::serialized_size();

            unsafe {
                std::ptr::copy(
                    core.phys_buf.as_ptr().add(src_offset),
                    core.phys_buf.as_ptr().add(dst_offset) as *mut u8,
                    count,
                );
            }
        }

        // Increment record count
        core.set_nentries(nentries + 1);
    }

    // Remove record slot
    fn remove_record_slot(&self, core: &NodeCore, idx: u32) {
        let nentries = core.nentries();
        let base = std::mem::size_of::<PersistentHeader>();

        // Shift entries
        if idx < nentries - 1 {
            let src_offset = base + PrefixCompressHeader::size() + ((idx + 1) as usize * Record::serialized_size());
            let dst_offset = base + PrefixCompressHeader::size() + (idx as usize * Record::serialized_size());
            let count = (nentries - idx - 1) as usize * Record::serialized_size();

            unsafe {
                std::ptr::copy(
                    core.phys_buf.as_ptr().add(src_offset),
                    core.phys_buf.as_ptr().add(dst_offset) as *mut u8,
                    count,
                );
            }
        }

        // Decrement record count
        core.set_nentries(nentries - 1);
    }

    // Compact to reclaim fragmented space
    fn compact(&self, core: &NodeCore) -> Result<(), BtreeError> {
        use std::collections::HashMap;

        let nentries = core.nentries();
        if nentries == 0 {
            return Ok(());
        }

        // Track prefix relocations (old_offset -> new_offset)
        let mut prefix_map: HashMap<u16, u16> = HashMap::new();

        // Collect all records and their data
        let mut records_data: Vec<(Record, Option<Vec<u8>>, Vec<u8>)> = Vec::new();

        for idx in 0..nentries {
            let record = Record::from_slot(core, idx);

            if record.is_standalone() {
                // Standalone: copy [refcount][key][value]
                let prefix = Prefix::from_record(record);
                let total_size = prefix.total_size() + record.data.value_size as usize;
                let data = self.get_data_slice(core, record.data.prefix_offset, total_size as u16).to_vec();
                records_data.push((record, None, data));
            } else {
                // Compressed: handle prefix and suffix+value separately
                let prefix = Prefix::from_record(record);

                // Copy prefix data if not already tracked
                let prefix_data = if !prefix_map.contains_key(&prefix.offset()) {
                    let prefix_total = prefix.total_size();
                    Some(self.get_data_slice(core, prefix.offset(), prefix_total as u16).to_vec())
                } else {
                    None
                };

                // Copy suffix+value
                let sv_size = record.data.suffix_size + record.data.value_size;
                let sv_data = self.get_data_slice(core, record.data.kv_offset, sv_size).to_vec();

                records_data.push((record, prefix_data, sv_data));
            }
        }

        // Reset tail to start compaction from the end
        let node_data_size = (core.node_size() - std::mem::size_of::<PersistentHeader>() as u32) as u16;
        let header = self.get_header_mut(core);
        header.tail_offset = node_data_size;
        header.hole_size = 0;

        // Rewrite all data compactly
        let mut updated_records: Vec<Record> = Vec::new();

        for (mut record, prefix_data_opt, sv_data) in records_data {
            if record.is_standalone() {
                // Allocate and write standalone data
                let new_offset = self.alloc_space(core, sv_data.len());
                self.write_bytes_at(core, new_offset as usize, &sv_data);

                record.data.prefix_offset = new_offset;
                updated_records.push(record);
            } else {
                // Handle shared prefix
                let old_prefix_offset = record.data.prefix_offset;
                let new_prefix_offset = if let Some(prefix_data) = prefix_data_opt {
                    // First time seeing this prefix - allocate and write
                    let new_offset = self.alloc_space(core, prefix_data.len());
                    self.write_bytes_at(core, new_offset as usize, &prefix_data);
                    prefix_map.insert(old_prefix_offset, new_offset);
                    new_offset
                } else {
                    // Prefix already relocated
                    *prefix_map.get(&old_prefix_offset).unwrap()
                };

                // Allocate and write suffix+value
                let new_sv_offset = self.alloc_space(core, sv_data.len());
                self.write_bytes_at(core, new_sv_offset as usize, &sv_data);

                record.data.prefix_offset = new_prefix_offset;
                record.data.kv_offset = new_sv_offset;
                updated_records.push(record);
            }
        }

        // Write all updated records back
        for record in updated_records {
            record.write(core);
        }

        core.inc_gen();
        Ok(())
    }
}
