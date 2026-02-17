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
 */

//! Common structures and implementation for variable-length nodes
//!
//! This module defines the record structures, headers, and generic implementation
//! used by all variable-length node variants (VarKey, VarValue, VarObj).
//!
//! Uses compile-time policy-based design via VarRecordOps trait.
//!
//! Memory layout:
//! [PersistentHeader][VarNodeHeader][Record0][Record1]...[RecordN] <--free--> [DataN]...[Data1][Data0]
//!                                   ^--- Records grow right               ^--- Data grows left (tail_arena_offset)
//!
//! Record structures use full u16 fields (no bit masking) for performance.

use super::super::btree_node::{NodeCore, NodeOps, PersistentHeader, EMPTY_BNODEID};
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree_types::BtreeError;
use super::super::detail::btree_req::BtreePutType;
use std::io;

//================================================================================
// Variable-Length Node Header
//================================================================================

/// Variable-length node header (comes after PersistentHeader)
/// Layout: [PersistentHeader][VarNodeHeader][Records...] <-free space-> [...Data]
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct VarNodeHeader {
    pub tail_arena_offset: u16, // Offset where next obj will be written (grows down from end)
    pub available_space: u16,   // Total free bytes in node
}

impl VarNodeHeader {
    #[inline]
    pub const fn size() -> u16 { std::mem::size_of::<Self>() as u16 }

    pub fn init(&mut self, node_size: u16) {
        self.tail_arena_offset = node_size;
        self.available_space = node_size - PersistentHeader::size() - Self::size();
    }
}

//================================================================================
// Record Structures with modular-bitfield for elegant bit manipulation
//================================================================================

/// Common base for all varlen records - just the obj_offset field
/// Used for generic access to offset without caring about record type
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct RecordHeader {
    obj_offset: u16,
}

impl RecordHeader {
    #[inline]
    pub fn obj_offset(&self) -> u16 { unsafe { std::ptr::addr_of!(self.obj_offset).read_unaligned() } }

    #[inline]
    pub fn set_obj_offset(&mut self, offset: u16) {
        unsafe {
            std::ptr::addr_of_mut!(self.obj_offset).write_unaligned(offset);
        }
    }
}

/// VarKey record (4 bytes: obj_offset 2 + key_len 2)
/// Variable-length key, fixed-size value
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct VarKeyRecord {
    pub header: RecordHeader,
    key_len: u16,
}

impl VarKeyRecord {
    #[inline]
    pub const fn size() -> usize { std::mem::size_of::<Self>() }

    #[inline]
    pub fn from_bytes_mut(bytes: &mut [u8]) -> &mut Self {
        assert_eq!(bytes.len(), Self::size());
        unsafe { &mut *(bytes.as_mut_ptr() as *mut Self) }
    }

    #[inline]
    pub fn key_len(&self) -> u16 { unsafe { std::ptr::addr_of!(self.key_len).read_unaligned() } }

    #[inline]
    pub fn set_key_len(&mut self, len: u16) {
        unsafe {
            std::ptr::addr_of_mut!(self.key_len).write_unaligned(len);
        }
    }
}

/// VarValue record (4 bytes: obj_offset 16 bits + value_len 15 bits + overflow 1 bit)
/// Fixed-size key, variable-length value with overflow support
/// overflow flag is packed in MSB of value_len field
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct VarValueRecord {
    pub header: RecordHeader,
    value_len_packed: u16, // 15 bits len + 1 bit overflow (MSB)
}

impl VarValueRecord {
    const OVERFLOW_BIT: u16 = 0x8000;
    const VALUE_LEN_MASK: u16 = 0x7FFF;

    #[inline]
    pub const fn size() -> usize { 4 }

    #[inline]
    pub fn from_bytes_mut(bytes: &mut [u8]) -> &mut Self {
        assert_eq!(bytes.len(), Self::size());
        unsafe { &mut *(bytes.as_mut_ptr() as *mut Self) }
    }

    #[inline]
    pub fn value_len(&self) -> u16 {
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        packed & Self::VALUE_LEN_MASK
    }

    #[inline]
    pub fn is_overflow(&self) -> bool {
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        (packed & Self::OVERFLOW_BIT) != 0
    }

    #[inline]
    pub fn obj_offset(&self) -> u16 { self.header.obj_offset() }

    #[inline]
    pub fn set_obj_offset(&mut self, offset: u16) { self.header.set_obj_offset(offset); }

    #[inline]
    pub fn set_value_len(&mut self, len: u16) {
        debug_assert!(len <= Self::VALUE_LEN_MASK, "value_len too large");
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        unsafe {
            std::ptr::addr_of_mut!(self.value_len_packed)
                .write_unaligned((packed & Self::OVERFLOW_BIT) | (len & Self::VALUE_LEN_MASK));
        }
    }

    #[inline]
    pub fn set_is_overflow(&mut self, is_overflow: bool) {
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        let new_packed = if is_overflow { packed | Self::OVERFLOW_BIT } else { packed & !Self::OVERFLOW_BIT };
        unsafe {
            std::ptr::addr_of_mut!(self.value_len_packed).write_unaligned(new_packed);
        }
    }

    #[inline]
    pub fn get_value_len_tuple(&self) -> (usize, bool) { (self.value_len() as usize, self.is_overflow()) }

    #[inline]
    pub fn set_value_len_tuple(&mut self, len: u16, is_overflow: bool) {
        self.set_value_len(len);
        self.set_is_overflow(is_overflow);
    }
}

/// VarObj record (6 bytes: obj_offset 16 + key_len 16 + value_len 15 + overflow 1 bit)
/// Variable-length key and value with overflow support
/// overflow flag is packed in MSB of value_len field
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct VarObjRecord {
    pub header: RecordHeader,
    key_len: u16,
    value_len_packed: u16, // 15 bits len + 1 bit overflow (MSB)
}

impl VarObjRecord {
    const OVERFLOW_BIT: u16 = 0x8000;
    const VALUE_LEN_MASK: u16 = 0x7FFF;

    #[inline]
    pub const fn size() -> usize { 6 }

    #[inline]
    pub fn from_bytes_mut(bytes: &mut [u8]) -> &mut Self {
        assert_eq!(bytes.len(), Self::size());
        unsafe { &mut *(bytes.as_mut_ptr() as *mut Self) }
    }

    #[inline]
    pub fn key_len(&self) -> u16 { unsafe { std::ptr::addr_of!(self.key_len).read_unaligned() } }

    #[inline]
    pub fn set_key_len(&mut self, len: u16) {
        unsafe {
            std::ptr::addr_of_mut!(self.key_len).write_unaligned(len);
        }
    }

    #[inline]
    pub fn value_len(&self) -> u16 {
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        packed & Self::VALUE_LEN_MASK
    }

    #[inline]
    pub fn is_overflow(&self) -> bool {
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        (packed & Self::OVERFLOW_BIT) != 0
    }

    #[inline]
    pub fn set_value_len(&mut self, len: u16) {
        debug_assert!(len <= Self::VALUE_LEN_MASK, "value_len too large");
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        unsafe {
            std::ptr::addr_of_mut!(self.value_len_packed)
                .write_unaligned((packed & Self::OVERFLOW_BIT) | (len & Self::VALUE_LEN_MASK));
        }
    }

    #[inline]
    pub fn set_is_overflow(&mut self, is_overflow: bool) {
        let packed = unsafe { std::ptr::addr_of!(self.value_len_packed).read_unaligned() };
        let new_packed = if is_overflow { packed | Self::OVERFLOW_BIT } else { packed & !Self::OVERFLOW_BIT };
        unsafe {
            std::ptr::addr_of_mut!(self.value_len_packed).write_unaligned(new_packed);
        }
    }

    #[inline]
    pub fn get_value_len_tuple(&self) -> (usize, bool) { (self.value_len() as usize, self.is_overflow()) }

    #[inline]
    pub fn set_value_len_tuple(&mut self, len: u16, is_overflow: bool) {
        self.set_value_len(len);
        self.set_is_overflow(is_overflow);
    }
}

//================================================================================
// VarRecordOps Trait - Policy for variable length node's record accessors
//================================================================================

/// Trait for variant-specific record operations (the "policy")
///
/// Each variable-length variant (VarKey, VarValue, VarObj) implements this trait
/// to provide variant-specific behavior for record sizes and metadata access.
pub trait VarRecordOps: Send + Sync + 'static {
    /// Size of one record entry in bytes
    fn record_size(&self) -> usize;

    /// Get key size for entry at index
    fn get_key_size(&self, core: &NodeCore, idx: u32) -> usize;

    /// Get value size for entry at index
    /// This returns the stored size (could be OVERFLOW_REFERENCE_SIZE if overflow)
    fn get_value_size(&self, core: &NodeCore, idx: u32) -> usize;

    /// Check if value at index is overflow
    fn is_value_overflow(&self, core: &NodeCore, idx: u32) -> bool;

    /// Set key length in record metadata
    fn set_key_len(&self, core: &NodeCore, idx: u32, len: usize);

    /// Set value length in record metadata (with overflow bit if needed)
    fn set_value_len(&self, core: &NodeCore, idx: u32, len: usize, is_overflow: bool);

    /// Node variant type (1=VAR_KEY, 2=VAR_VALUE, 3=VAR_OBJECT)
    fn node_variant_type(&self) -> u8;
}

//================================================================================
// Helper functions (used by VarNodeOps and policy implementations)
//================================================================================

/// Get immutable reference to var node header
#[inline]
pub fn get_var_header(core: &NodeCore) -> &VarNodeHeader {
    let offset = PersistentHeader::size();
    unsafe {
        let ptr = core.phys_buf.as_ref().as_ptr().add(offset as usize) as *const VarNodeHeader;
        &*ptr
    }
}

/// Get mutable reference to var node header
#[inline]
pub fn get_var_header_mut(core: &NodeCore) -> &mut VarNodeHeader {
    let offset = PersistentHeader::size();
    unsafe {
        let ptr = core.phys_buf.as_ref().as_ptr().add(offset as usize) as *mut VarNodeHeader;
        &mut *ptr
    }
}

/// Get pointer to record at index
#[inline]
pub fn get_record_ptr(core: &NodeCore, idx: u32, record_size: usize) -> *const u8 {
    let offset = PersistentHeader::size() + VarNodeHeader::size() + (idx as u16 * record_size as u16);
    unsafe { core.phys_buf.as_ref().as_ptr().add(offset as usize) }
}

/// Get mutable pointer to record at index
#[inline]
pub fn get_record_ptr_mut(core: &NodeCore, idx: u32, record_size: usize) -> *mut u8 {
    let offset = PersistentHeader::size() + VarNodeHeader::size() + (idx as u16 * record_size as u16);
    unsafe { core.phys_buf.as_ref().as_ptr().add(offset as usize) as *mut u8 }
}

/// Get pointer to actual key/value data from record pointer
#[inline]
pub fn get_obj_ptr(core: &NodeCore, rec_ptr: *const u8) -> *const u8 {
    let rec = unsafe { &*(rec_ptr as *const RecordHeader) };
    let offset = rec.obj_offset() as usize;
    unsafe { core.phys_buf.as_ref().as_ptr().add(offset) }
}

/// Get mutable pointer to actual key/value data from record pointer
#[inline]
pub fn get_obj_ptr_mut(core: &NodeCore, rec_ptr: *mut u8) -> *mut u8 {
    let rec = unsafe { &*(rec_ptr as *const RecordHeader) };
    let offset = rec.obj_offset() as usize;
    unsafe { core.phys_buf.as_ref().as_ptr().add(offset) as *mut u8 }
}

/// Get free space in tail arena (contiguous space for new data)
pub fn get_arena_free_space(core: &NodeCore, rec_size: usize) -> usize {
    let var_hdr = get_var_header(core);
    let nentries = core.get_persistent_header().nentries();
    let records_end: u16 = PersistentHeader::size() + VarNodeHeader::size() + (nentries as u16 * rec_size as u16);

    if var_hdr.tail_arena_offset <= records_end {
        0
    } else {
        (var_hdr.tail_arena_offset - records_end) as usize
    }
}

//================================================================================
// VarNodeOps - Generic implementation for all variable-length variants
//================================================================================

/// Generic variable-length node operations
///
/// Implements NodeOps ONCE for all variable-length variants using policy-based design.
/// The R type parameter provides variant-specific behavior via VarRecordOps trait.
pub struct VarNodeOps<R> {
    pub(crate) record_ops: R,
}

impl<R> VarNodeOps<R> {
    pub const fn new(record_ops: R) -> Self { Self { record_ops } }
}

impl<R> VarNodeOps<R>
where
    R: VarRecordOps,
{
    /// Get key size - uses compile-time constant if available, otherwise reads from record
    #[inline]
    fn get_key_size<K: BtreeKey>(&self, core: &NodeCore, idx: u32) -> usize {
        if let Some(size) = K::FIXED_SERIALIZED_SIZE {
            size as usize
        } else {
            self.record_ops.get_key_size(core, idx)
        }
    }

    /// Get value size - uses compile-time constant if available, otherwise reads from record
    #[inline]
    fn get_value_size<V: BtreeValue>(&self, core: &NodeCore, idx: u32) -> usize {
        if let Some(size) = V::FIXED_SERIALIZED_SIZE {
            size as usize
        } else {
            self.record_ops.get_value_size(core, idx)
        }
    }
}

impl<K, V, R> NodeOps<K, V> for VarNodeOps<R>
where
    K: BtreeKey + 'static,
    V: BtreeValue + 'static,
    R: VarRecordOps,
{
    fn init_new_node(&self, core: &NodeCore) {
        let header = core.get_persistent_header_mut();
        header.set_nentries(0);
        header.edge_id = EMPTY_BNODEID;
        header.node_variant = self.record_ops.node_variant_type();

        let var_hdr = get_var_header_mut(core);
        var_hdr.init(core.node_size() as u16);
    }

    fn get_all_kvs(&self, core: &NodeCore) -> Vec<(K, ValueOrOverflow<V>)> {
        let nentries = core.get_persistent_header().nentries();
        let mut result = Vec::with_capacity(nentries as usize);

        for i in 0..nentries {
            let key = <Self as NodeOps<K, V>>::get_nth_key(self, core, i, true);
            let val = <Self as NodeOps<K, V>>::get_nth_value(self, core, i, true);
            result.push((key, val));
        }
        result
    }

    fn insert(&self, core: &NodeCore, idx: u32, key: &K, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        if idx > nentries {
            return Err(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Insert index {} out of range (nentries={})", idx, nentries),
            )));
        }

        // Get sizes for NEW entry being inserted
        let key_size = key.serialized_size() as usize;
        let val_size = val.serialized_size();
        let obj_size = key_size + val_size;
        let rec_size = self.record_ops.record_size();
        let to_insert_size = obj_size + rec_size;

        // Check if we have enough space
        let avail_space = get_var_header(core).available_space;
        if to_insert_size > avail_space as usize {
            return Err(BtreeError::Io(io::Error::new(
                io::ErrorKind::OutOfMemory,
                format!("insert failed size={} avail={}", to_insert_size, avail_space),
            )));
        }

        // Compact if needed to get contiguous space in tail arena
        let arena_free = Self::get_arena_free_space(core, rec_size);
        if to_insert_size > arena_free {
            self.compact::<K, V>(core)?;
            debug_assert!(
                to_insert_size <= Self::get_arena_free_space(core, rec_size),
                "Should have space after compaction"
            );
        }

        // Shift records right to make room for new record (C++ line 469-471)
        if idx < nentries {
            let src = get_record_ptr_mut(core, idx, rec_size);
            let dst = unsafe { src.add(rec_size) };
            let bytes_to_move = (nentries - idx) as usize * rec_size;
            unsafe {
                std::ptr::copy(src, dst, bytes_to_move);
            }
        }

        // Update tail offset and available space (C++ lines 473-476)
        let var_hdr = get_var_header_mut(core);
        debug_assert!(var_hdr.tail_arena_offset as usize >= obj_size);
        var_hdr.tail_arena_offset -= obj_size as u16;
        var_hdr.available_space -= to_insert_size as u16;

        // Create new record metadata (C++ lines 478-481)
        let rec_ptr = get_record_ptr_mut(core, idx, rec_size);
        unsafe {
            let base_rec = &mut *(rec_ptr as *mut RecordHeader);
            base_rec.set_obj_offset(var_hdr.tail_arena_offset);
        }
        self.record_ops.set_key_len(core, idx, key_size);
        self.record_ops.set_value_len(core, idx, val_size, val.is_overflow());

        // Serialize key and value/reference directly into tail arena (C++ lines 483-487)
        let data_ptr = unsafe { core.phys_buf.as_ref().as_ptr().add(var_hdr.tail_arena_offset as usize) } as *mut u8;
        let data_slice = unsafe { std::slice::from_raw_parts_mut(data_ptr, obj_size) };
        key.serialize_to(&mut data_slice[..key_size], true).map_err(BtreeError::Io)?;
        val.serialize_to(&mut data_slice[key_size..]).map_err(BtreeError::Io)?;

        // Increment entries and generation (C++ lines 489-491)
        core.get_persistent_header_mut().set_nentries(nentries + 1);
        core.inc_gen();
        Ok(())
    }

    fn remove(&self, core: &NodeCore, idx: u32) -> Result<(), BtreeError> {
        NodeOps::<K, V>::remove_range(self, core, idx, idx)
    }

    fn get_nth_key(&self, core: &NodeCore, idx: u32, copy: bool) -> K {
        debug_assert!(idx < core.get_persistent_header().nentries(), "Index {} out of bounds", idx);

        let rec_ptr = get_record_ptr(core, idx, self.record_ops.record_size());
        let obj_ptr = get_obj_ptr(core, rec_ptr);
        let key_size = self.get_key_size::<K>(core, idx);

        let key_slice = unsafe { std::slice::from_raw_parts(obj_ptr, key_size) };
        K::deserialize_from(key_slice, copy).expect("Failed to deserialize key")
    }

    fn get_nth_value(&self, core: &NodeCore, idx: u32, copy: bool) -> ValueOrOverflow<V> {
        let nentries = core.get_persistent_header().nentries();

        // Handle edge case for interior nodes
        if idx == nentries {
            debug_assert!(!core.is_leaf(), "get_nth_value out-of-bound for leaf");
            debug_assert!(core.has_valid_edge(), "get_nth_value out-of-bound, no edge");
            // For interior nodes, edge value is stored in header (always inline BNodeId)
            let edge_value = V::deserialize_from(&core.get_persistent_header().edge_id.to_le_bytes(), true)
                .expect("Failed to deserialize edge value");
            return ValueOrOverflow::Inline(edge_value);
        }

        debug_assert!(idx < nentries, "Index {} out of bounds", idx);

        let rec_ptr = get_record_ptr(core, idx, self.record_ops.record_size());
        let obj_ptr = get_obj_ptr(core, rec_ptr);
        let key_size = self.get_key_size::<K>(core, idx);
        let val_size = self.get_value_size::<V>(core, idx);

        let val_ptr = unsafe { obj_ptr.add(key_size) };
        let val_slice = unsafe { std::slice::from_raw_parts(val_ptr, val_size) };

        // Deserialize value or overflow reference
        ValueOrOverflow::deserialize_from(val_slice, self.record_ops.is_value_overflow(core, idx), copy)
            .expect("Failed to deserialize value")
    }

    fn is_nth_value_overflow(&self, core: &NodeCore, idx: u32) -> bool {
        let nentries = core.get_persistent_header().nentries();

        // Handle edge case for interior nodes - edge is always inline BNodeId
        if idx == nentries {
            debug_assert!(!core.is_leaf(), "is_nth_value_overflow out-of-bound for leaf");
            debug_assert!(core.has_valid_edge(), "is_nth_value_overflow out-of-bound, no edge");
            return false;
        }

        debug_assert!(idx < nentries, "Index {} out of bounds", idx);
        self.record_ops.is_value_overflow(core, idx)
    }

    fn update(&self, core: &NodeCore, idx: u32, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();

        // Handle edge value update for interior nodes (C++ lines 96-100)
        if idx == nentries {
            debug_assert!(!core.is_leaf(), "Edge update only for interior nodes");
            let edge_val = val.clone().expect_inline("Interior node values (BNodeId) cannot be overflow references");
            core.update_edge(&edge_val);
            return Ok(());
        }

        let new_val_size = val.serialized_size();
        let current_gen = core.node_gen();

        // Get CURRENT sizes
        let cur_key_size = self.get_key_size::<K>(core, idx);
        let cur_val_size = self.get_value_size::<V>(core, idx);
        let cur_obj_size = cur_key_size + cur_val_size;
        let new_obj_size = cur_key_size + new_val_size;

        // If new value fits in current space, do in-place update
        if cur_obj_size >= new_obj_size {
            let rec_ptr = get_record_ptr_mut(core, idx, self.record_ops.record_size());
            let obj_ptr = get_obj_ptr_mut(core, rec_ptr);

            // Serialize ONLY the value, starting after the key
            let obj_slice = unsafe { std::slice::from_raw_parts_mut(obj_ptr, cur_obj_size) };
            val.serialize_to(&mut obj_slice[cur_key_size..cur_key_size + new_val_size])
                .map_err(BtreeError::Io)?;

            // Update record metadata
            self.record_ops.set_value_len(core, idx, new_val_size, val.is_overflow());

            // Reclaim freed space if value shrunk
            if cur_obj_size > new_obj_size {
                let var_hdr = get_var_header_mut(core);
                var_hdr.available_space += (cur_obj_size - new_obj_size) as u16;
            }
        } else {
            // Value grew, need to remove and re-insert
            let key = NodeOps::<K, V>::get_nth_key(self, core, idx, true);
            NodeOps::<K, V>::remove_range(self, core, idx, idx)?;
            self.insert(core, idx, &key, val)?;
        }

        core.set_node_gen(current_gen + 1);
        Ok(())
    }

    fn update_with_key(&self, core: &NodeCore, idx: u32, key: &K, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        debug_assert!(idx <= nentries, "Update index out of bounds");

        // Handle edge value update for interior nodes (C++ lines 113-118)
        if idx == nentries {
            debug_assert!(!core.is_leaf(), "Edge update only for interior nodes");
            // For interior nodes, value is always inline BNodeId
            let edge_val = val.clone().expect_inline("Interior node values (BNodeId) cannot be overflow references");
            core.update_edge(&edge_val);
            return Ok(());
        }

        // Get new key and value sizes from NEW instances being updated
        let new_key_size = key.serialized_size() as usize;
        let new_val_size = val.serialized_size();
        let new_obj_size = new_key_size + new_val_size;
        let current_gen = core.node_gen();

        // Get CURRENT object size from EXISTING entry (no instance yet)
        let cur_key_size = self.get_key_size::<K>(core, idx);
        let cur_val_size = self.get_value_size::<V>(core, idx);
        let cur_obj_size = cur_key_size + cur_val_size;

        // If new size fits in current space, do in-place update (C++ lines 124-141)
        if cur_obj_size >= new_obj_size {
            let rec_ptr = get_record_ptr_mut(core, idx, self.record_ops.record_size());
            let obj_ptr = get_obj_ptr_mut(core, rec_ptr);

            // Serialize key and value/reference directly into node buffer (C++ lines 135-137)
            let obj_slice = unsafe { std::slice::from_raw_parts_mut(obj_ptr, cur_obj_size) };
            key.serialize_to(&mut obj_slice[..new_key_size], true).map_err(BtreeError::Io)?;
            val.serialize_to(&mut obj_slice[new_key_size..new_key_size + new_val_size])
                .map_err(BtreeError::Io)?;

            // Update record metadata (C++ lines 138-140)
            self.record_ops.set_key_len(core, idx, new_key_size);
            self.record_ops.set_value_len(core, idx, new_val_size, val.is_overflow());

            // Reclaim freed space (C++ line 140)
            let var_hdr = get_var_header_mut(core);
            var_hdr.available_space += (cur_obj_size - new_obj_size) as u16;
        } else {
            // Size increased, need to remove and re-insert
            NodeOps::<K, V>::remove_range(self, core, idx, idx)?;
            self.insert(core, idx, key, val)?;
        }

        core.set_node_gen(current_gen + 1);
        Ok(())
    }

    fn remove_range(&self, core: &NodeCore, start_idx: u32, end_idx: u32) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        debug_assert!(start_idx <= nentries && end_idx <= nentries, "Remove range out of bounds");
        debug_assert!(start_idx <= end_idx, "Invalid range");

        let rec_size = self.record_ops.record_size();
        let num_to_remove = end_idx - start_idx + 1;
        let current_gen = core.node_gen();

        // Special case: removing up to and including the edge (C++ lines 156-166)
        if end_idx == nentries {
            debug_assert!(!core.is_leaf() && core.has_valid_edge(), "Edge removal requires valid edge");

            // Move the previous value to edge (C++ lines 159-161)
            if start_idx > 0 {
                // Extract the BNodeId from value. For interior nodes, value is always inline BNodeId
                let last_val = NodeOps::<K, V>::get_nth_value(self, core, start_idx - 1, false)
                    .expect_inline("Interior node values (BNodeId) cannot be overflow references");
                core.update_edge(&last_val);
            }

            // Reclaim space from removed entries (C++ lines 163-165)
            for i in (start_idx - 1)..nentries {
                let key_size = self.get_key_size::<K>(core, i);
                let val_size = self.get_value_size::<V>(core, i);
                let var_hdr = get_var_header_mut(core);
                var_hdr.available_space += (key_size + val_size + rec_size) as u16;
            }

            // Update entry count (C++ line 166)
            core.get_persistent_header_mut().set_nentries(start_idx - 1);
        } else {
            // Normal case: remove entries in the middle (C++ lines 167-176)

            // Reclaim space from removed entries (C++ lines 169-171)
            for i in start_idx..=end_idx {
                let key_size = self.get_key_size::<K>(core, i);
                let val_size = self.get_value_size::<V>(core, i);
                let var_hdr = get_var_header_mut(core);
                var_hdr.available_space += (key_size + val_size + rec_size) as u16;
            }

            // Shift remaining records left to fill gap
            let src = get_record_ptr_mut(core, end_idx + 1, rec_size);
            let dst = get_record_ptr_mut(core, start_idx, rec_size);
            let bytes_to_move = (nentries - end_idx - 1) as usize * rec_size;
            unsafe {
                std::ptr::copy(src, dst, bytes_to_move);
            }

            // Update entry count (C++ line 175)
            core.get_persistent_header_mut().set_nentries(nentries - num_to_remove);
        }

        core.set_node_gen(current_gen + 1);
        Ok(())
    }

    fn remove_all(&self, core: &NodeCore) {
        core.get_persistent_header_mut().set_nentries(0);
        core.get_persistent_header_mut().edge_id = EMPTY_BNODEID;
        core.inc_gen();

        let var_hdr = get_var_header_mut(core);
        let node_size = core.node_size() as u16;
        var_hdr.tail_arena_offset = node_size;
        var_hdr.available_space = node_size - PersistentHeader::size() - VarNodeHeader::size();
    }

    fn move_out_to_right_by_entries(&self, src_core: &NodeCore, dst_core: &NodeCore, mut nentries: u32) -> u32 {
        let this_gen = src_core.node_gen();
        let other_gen = dst_core.node_gen();

        let src_nentries = src_core.get_persistent_header().nentries();
        nentries = nentries.min(src_nentries);
        if nentries == 0 {
            return 0;
        }

        // Move entries from end of src to beginning of dst (C++ lines 211-227)
        let start_idx = src_nentries - 1;
        let end_idx = src_nentries - nentries;
        let mut idx = start_idx;
        let mut full_move = false;
        let mut moved_count = 0;

        loop {
            // Get key and value blobs for this entry (C++ lines 217-218)
            let key_size = self.get_key_size::<K>(src_core, idx);
            let val_size = self.get_value_size::<V>(src_core, idx);
            let rec_ptr = get_record_ptr(src_core, idx, self.record_ops.record_size());
            let obj_ptr = get_obj_ptr(src_core, rec_ptr);

            // Try to insert at beginning of dst (index 0) (C++ line 220)
            let key_slice = unsafe { std::slice::from_raw_parts(obj_ptr, key_size) };
            let val_slice = unsafe { std::slice::from_raw_parts(obj_ptr.add(key_size), val_size) };

            let key = K::deserialize_from(key_slice, false).expect("Failed to deserialize key");
            let val = ValueOrOverflow::<V>::deserialize_from(
                val_slice,
                self.record_ops.is_value_overflow(src_core, idx),
                false,
            )
            .expect("Failed to deserialize value");

            if self.insert(dst_core, 0, &key, &val).is_err() {
                break;
            }
            moved_count += 1;

            if idx == 0 {
                full_move = true;
                break;
            }
            if idx < end_idx {
                break;
            }
            idx -= 1;
        }

        // Handle edge transfer for interior nodes (C++ lines 229-233)
        if !src_core.is_leaf() && dst_core.get_persistent_header().nentries() != 0 {
            let edge_id = src_core.get_persistent_header().edge_id;
            if edge_id != EMPTY_BNODEID {
                dst_core.set_edge(edge_id);
                src_core.invalidate_edge();
            }
        }

        // Remove moved entries from source (C++ line 234)
        let remove_start = if full_move { 0 } else { idx + 1 };
        NodeOps::<K, V>::remove_range(self, src_core, remove_start, start_idx).ok();

        // Reset generation counters (C++ lines 239-240)
        src_core.set_node_gen(this_gen + 1);
        dst_core.set_node_gen(other_gen + 1);

        moved_count
    }

    fn move_out_to_right_by_size(&self, src_core: &NodeCore, dst_core: &NodeCore, mut size_to_move: u32) -> u32 {
        let this_gen = src_core.node_gen();
        let other_gen = dst_core.node_gen();
        let mut nmoved = 0;

        let src_nentries = src_core.get_persistent_header().nentries();
        if src_nentries == 0 {
            return 0;
        }

        let mut idx = src_nentries - 1;
        let rec_size = self.record_ops.record_size();

        // Move entries from end of src to beginning of dst until size threshold (C++ lines 251-266)
        loop {
            let key_size = self.get_key_size::<K>(src_core, idx);
            let val_size = self.get_value_size::<V>(src_core, idx);
            let entry_size = (key_size + val_size + rec_size) as u32;

            // Check if we've reached threshold
            if entry_size > size_to_move {
                break;
            }

            // Get key and value
            let rec_ptr = get_record_ptr(src_core, idx, rec_size);
            let obj_ptr = get_obj_ptr(src_core, rec_ptr);
            let key_slice = unsafe { std::slice::from_raw_parts(obj_ptr, key_size) };
            let val_slice = unsafe { std::slice::from_raw_parts(obj_ptr.add(key_size), val_size) };

            let key = K::deserialize_from(key_slice, false).expect("Failed to deserialize key");
            let val = ValueOrOverflow::<V>::deserialize_from(
                val_slice,
                self.record_ops.is_value_overflow(src_core, idx),
                false,
            )
            .expect("Failed to deserialize value");

            // Insert at beginning of dst
            if self.insert(dst_core, 0, &key, &val).is_err() {
                break;
            }

            if idx == 0 {
                nmoved += 1;
                break;
            }
            idx -= 1;
            nmoved += 1;
            size_to_move -= entry_size;
        }

        // Remove moved entries from source (C++ line 267)
        if nmoved > 0 {
            let remove_start = if idx == 0 && nmoved > 0 { 0 } else { idx + 1 };
            NodeOps::<K, V>::remove_range(self, src_core, remove_start, src_nentries - 1).ok();
        }

        // Handle edge transfer for interior nodes (C++ lines 269-273)
        if !src_core.is_leaf() && dst_core.get_persistent_header().nentries() != 0 {
            let edge_id = src_core.get_persistent_header().edge_id;
            if edge_id != EMPTY_BNODEID {
                dst_core.set_edge(edge_id);
                src_core.invalidate_edge();
            }
        }

        // Reset generation counters (C++ lines 278-279)
        src_core.set_node_gen(this_gen + 1);
        dst_core.set_node_gen(other_gen + 1);

        nmoved
    }

    fn append_copy_in_upto_size(
        &self,
        dst_core: &NodeCore,
        src_core: &NodeCore,
        other_cursor: &mut u32,
        upto_size: u32,
        copy_only_if_fits: bool,
    ) -> bool {
        // Check if dst already at size limit (C++ line 298)
        let dst_occupied = self.occupied_size(dst_core);
        if dst_occupied >= upto_size as usize {
            return false;
        }

        // Check if src is empty (C++ line 299)
        let src_nentries = src_core.get_persistent_header().nentries();
        if src_nentries == 0 {
            return true;
        }

        let room = upto_size - dst_occupied as u32;

        // If copy_only_if_fits, verify all remaining entries fit (C++ lines 302-304)
        if copy_only_if_fits {
            let entries_size = self.get_entries_size::<K, V>(src_core, *other_cursor, src_nentries);
            if entries_size > room {
                return false;
            }
        }

        // Copy entries by size (C++ lines 305-306)
        let ncopied = self.copy_by_size::<K, V>(dst_core, src_core, *other_cursor, room);
        *other_cursor += ncopied;

        // Verify full copy if required (C++ lines 308-311)
        if copy_only_if_fits {
            debug_assert_eq!(*other_cursor, src_nentries, "Expected to copy all entries but didn't");
        }
        true
    }

    fn available_size(&self, core: &NodeCore) -> u32 {
        let var_hdr = get_var_header(core);
        var_hdr.available_space as u32
    }

    fn has_room_for_put(&self, core: &NodeCore, put_type: BtreePutType, key_size: u32, value_size: u32) -> bool {
        let mut needed_size = key_size + value_size;
        if put_type == BtreePutType::Insert || put_type == BtreePutType::Upsert {
            needed_size += self.record_ops.record_size() as u32;
        }
        NodeOps::<K, V>::available_size(self, core) >= needed_size
    }
}

//================================================================================
// Private helper methods for VarNodeOps
//================================================================================
impl<R: VarRecordOps> VarNodeOps<R> {
    /// Compact the node to reclaim fragmented space (C++ lines 504-568)
    fn compact<K: BtreeKey, V: BtreeValue>(&self, core: &NodeCore) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        let rec_size = self.record_ops.record_size();

        if nentries == 0 {
            // No entries, reset to full space (C++ lines 515-520)
            let node_size = core.phys_buf.len();
            let var_hdr = get_var_header_mut(core);
            var_hdr.tail_arena_offset = node_size as u16;
            var_hdr.available_space = node_size as u16 - PersistentHeader::size() as u16 - VarNodeHeader::size() as u16;
            return Ok(());
        }

        // Build sorted list of records by offset (descending order) (C++ lines 521-534)
        #[derive(Clone, Copy)]
        struct RecordInfo {
            obj_offset: u16,
            orig_index: u32,
        }

        let mut records: Vec<RecordInfo> = (0..nentries)
            .map(|idx| {
                let rec_ptr = get_record_ptr(core, idx, rec_size);
                let obj_offset = unsafe { (*(rec_ptr as *const RecordHeader)).obj_offset() };
                RecordInfo { obj_offset, orig_index: idx }
            })
            .collect();

        // Sort by obj_offset descending (C++ lines 532-534)
        records.sort_by(|a, b| b.obj_offset.cmp(&a.obj_offset));

        // Compact by moving objects to eliminate gaps (C++ lines 536-562)
        let node_size = core.phys_buf.len();
        let mut last_offset = node_size as u16;

        for rec_info in records.iter() {
            let idx = rec_info.orig_index;
            let key_size = self.get_key_size::<K>(core, idx);
            let val_size = self.get_value_size::<V>(core, idx);
            let total_kv_len = key_size + val_size;

            let sparse_space = last_offset - (rec_info.obj_offset + total_kv_len as u16);
            if sparse_space > 0 {
                // Move data up to eliminate gap (C++ lines 546-555)
                let rec_ptr_mut = get_record_ptr_mut(core, idx, rec_size);
                let old_ptr = get_obj_ptr_mut(core, rec_ptr_mut);
                let new_ptr = unsafe { old_ptr.add(sparse_space as usize) };
                unsafe {
                    std::ptr::copy(old_ptr, new_ptr, total_kv_len);
                }

                // Update record's offset (C++ lines 551-553)
                let rec_ptr = get_record_ptr_mut(core, idx, rec_size);
                unsafe {
                    let base_rec = &mut *(rec_ptr as *mut RecordHeader);
                    let new_offset = base_rec.obj_offset() + sparse_space;
                    base_rec.set_obj_offset(new_offset);
                }
                last_offset = unsafe { (*(rec_ptr as *const RecordHeader)).obj_offset() };
            } else {
                debug_assert_eq!(sparse_space, 0);
                last_offset = rec_info.obj_offset;
            }
        }

        // Update tail arena offset (C++ line 563)
        let var_hdr = get_var_header_mut(core);
        var_hdr.tail_arena_offset = last_offset;
        Ok(())
    }

    /// Helper to compute arena free space (contiguous free space at the end of records area)
    fn get_arena_free_space(core: &NodeCore, rec_size: usize) -> usize {
        let var_hdr = get_var_header(core);
        let nentries = core.get_persistent_header().nentries();
        let tail = var_hdr.tail_arena_offset;
        let records_end = PersistentHeader::size() + VarNodeHeader::size() + (nentries as u16 * rec_size as u16);
        tail.saturating_sub(records_end) as usize
    }

    /// Copy entries by size from src to dst (C++ lines 315-341)
    fn copy_by_size<K: BtreeKey + 'static, V: BtreeValue + 'static>(
        &self,
        dst_core: &NodeCore,
        src_core: &NodeCore,
        start_idx: u32,
        mut copy_size: u32,
    ) -> u32 {
        let this_gen = dst_core.node_gen();
        let src_nentries = src_core.get_persistent_header().nentries();
        let rec_size = self.record_ops.record_size();

        let mut idx = start_idx;
        let mut n = 0;

        // Copy entries while we have room (C++ lines 321-333)
        while idx < src_nentries {
            let key_size = self.get_key_size::<K>(src_core, idx);
            let val_size = self.get_value_size::<V>(src_core, idx);
            let entry_size = (key_size + val_size + rec_size) as u32;

            // Check if we've reached size threshold (C++ line 326)
            if entry_size > copy_size {
                break;
            }

            // Get key and value (C++ lines 322-323)
            let rec_ptr = get_record_ptr(src_core, idx, rec_size);
            let obj_ptr = get_obj_ptr(src_core, rec_ptr);
            let key_slice = unsafe { std::slice::from_raw_parts(obj_ptr, key_size) };
            let val_slice = unsafe { std::slice::from_raw_parts(obj_ptr.add(key_size), val_size) };

            let key = K::deserialize_from(key_slice, false).expect("Failed to deserialize key");
            let val = ValueOrOverflow::<V>::deserialize_from(
                val_slice,
                self.record_ops.is_value_overflow(src_core, idx),
                false,
            )
            .expect("Failed to deserialize value");

            // Insert at end of dst (C++ line 328)
            let dst_nentries = dst_core.get_persistent_header().nentries();
            if self.insert(dst_core, dst_nentries, &key, &val).is_err() {
                break;
            }

            n += 1;
            idx += 1;
            copy_size -= entry_size;
        }

        // Reset generation (C++ line 334)
        dst_core.set_node_gen(this_gen + 1);

        // Copy edge if we copied everything (C++ lines 337-339)
        if !src_core.is_leaf() && src_core.has_valid_edge() && (start_idx + n) == src_nentries {
            let edge_id = src_core.get_persistent_header().edge_id;
            if edge_id != EMPTY_BNODEID {
                dst_core.set_edge(edge_id);
            }
        }

        n
    }

    /// Get size of entries in range (C++ lines 284-294)
    fn get_entries_size<K: BtreeKey, V: BtreeValue>(&self, core: &NodeCore, start_idx: u32, end_idx: u32) -> u32 {
        let nentries = core.get_persistent_header().nentries();

        // Fast path for full node (C++ lines 285-287)
        if start_idx == 0 && end_idx == nentries {
            return (self.occupied_size(core) - size_of::<VarNodeHeader>()) as u32;
        }

        // Sum up individual entry sizes (C++ lines 289-293)
        let rec_size = self.record_ops.record_size();
        let mut cum_size = 0;
        for i in start_idx..end_idx {
            let key_size = self.get_key_size::<K>(core, i);
            let val_size = self.get_value_size::<V>(core, i);
            cum_size += key_size + val_size + rec_size;
        }
        cum_size as u32
    }

    /// Compute occupied size (header + records + data)
    fn occupied_size(&self, core: &NodeCore) -> usize {
        let var_hdr = get_var_header(core);
        let nentries = core.get_persistent_header().nentries();
        let rec_size = self.record_ops.record_size();

        // Occupied = var_header + all_records + data_area_used
        let records_size = nentries as usize * rec_size;
        let node_size = core.node_size() as usize;
        let data_used = node_size - var_hdr.tail_arena_offset as usize;

        size_of::<VarNodeHeader>() + records_size + data_used
    }
}
