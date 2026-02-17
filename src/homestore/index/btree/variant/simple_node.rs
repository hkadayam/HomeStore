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

//! SimpleNode - Fixed-size key/value node variant
//!
//! Layout: [PersistentHeader][K0][V0][K1][V1]...
//! All entries are fixed size, stored contiguously.

use super::super::btree_node::{NodeCore, NodeOps, PersistentHeader};
use super::super::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow};
use super::super::btree_types::BtreeError;
use std::io;

/// Zero-sized type for SimpleNode operations
/// NO fields, NO memory overhead - just methods operating on &NodeCore
///
/// SimpleNode layout: [PersistentHeader][K0][V0][K1][V1]...
pub struct SimpleNodeOps;

impl<K: BtreeKey, V: BtreeValue> NodeOps<K, V> for SimpleNodeOps {
    fn init_new_node(&self, core: &NodeCore) {
        // SimpleNode: initialize nentries to 0 and edge to invalid
        let header = core.get_persistent_header_mut();
        header.set_nentries(0);
        header.edge_id = super::super::btree_node::EMPTY_BNODEID;
    }

    fn insert(&self, core: &NodeCore, idx: u32, key: &K, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        if !core.is_leaf() && (idx > nentries) {
            let edge_val = val.clone().expect_inline("SimpleNode requires inline values");
            core.update_edge(&edge_val);
            return Ok(());
        }

        if idx > nentries {
            return Err(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Insert index {} out of range (nentries={})", idx, nentries),
            )));
        }

        // Use fixed serialized sizes (SimpleNode requires fixed-size keys/values)
        let key_size = K::FIXED_SERIALIZED_SIZE.expect("SimpleNode requires fixed-size keys") as usize;
        let val_size = V::FIXED_SERIALIZED_SIZE.expect("SimpleNode requires fixed-size values") as usize;
        let entry_size = key_size + val_size;
        let data_start = PersistentHeader::size() as usize;

        // Calculate available space
        let node_size = core.node_size() as usize;
        let used_space = data_start + (nentries as usize) * entry_size;
        if used_space + entry_size > node_size {
            return Err(BtreeError::Io(io::Error::new(io::ErrorKind::OutOfMemory, "Node is full")));
        }

        // Shift entries to make space if inserting in the middle
        if idx < nentries {
            let insert_offset = data_start + (idx as usize) * entry_size;
            let num_to_move = (nentries - idx) as usize;
            let bytes_to_move = num_to_move * entry_size;

            unsafe {
                let src = core.phys_buf.as_ref().as_ptr().add(insert_offset) as *mut u8;
                let dst = src.add(entry_size);
                std::ptr::copy(src as *const u8, dst, bytes_to_move);
            }
        }

        // Serialize key and value using trait methods (copy=false for fastest path)
        let k_offset = data_start + (idx as usize) * entry_size;
        let v_offset = k_offset + key_size;

        unsafe {
            let base_ptr = core.phys_buf.as_ref().as_ptr() as *mut u8;
            let buf = std::slice::from_raw_parts_mut(base_ptr, node_size);

            key.serialize_to(&mut buf[k_offset..k_offset + key_size], true).map_err(BtreeError::Io)?;
            val.serialize_to(&mut buf[v_offset..v_offset + val_size]).map_err(BtreeError::Io)?;
        }

        // Update nentries
        core.get_persistent_header_mut().set_nentries(nentries + 1);
        core.inc_gen();
        Ok(())
    }

    fn update(&self, core: &NodeCore, idx: u32, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        if idx == nentries {
            let edge_val = val.clone().expect_inline("SimpleNode requires inline values");
            core.update_edge(&edge_val);
            return Ok(());
        }

        if idx > nentries {
            return Err(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Update index {} out of range (nentries={})", idx, nentries),
            )));
        }

        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let data_start = PersistentHeader::size() as usize;
        let entry_size = key_size + val_size;
        let v_offset = data_start + (idx as usize) * entry_size + key_size;

        unsafe {
            let base_ptr = core.phys_buf.as_ref().as_ptr() as *mut u8;
            let buf = std::slice::from_raw_parts_mut(base_ptr, core.node_size() as usize);
            val.serialize_to(&mut buf[v_offset..v_offset + val_size]).map_err(BtreeError::Io)?;
        }
        core.inc_gen();
        Ok(())
    }

    fn update_with_key(&self, core: &NodeCore, idx: u32, key: &K, val: &ValueOrOverflow<V>) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();

        // Handle edge case: set_edge for idx == nentries
        if idx == nentries {
            let edge_val = val.clone().expect_inline("SimpleNode requires inline values");
            core.update_edge(&edge_val);
            return Ok(());
        }

        if idx > nentries {
            return Err(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Update index {} out of range (nentries={})", idx, nentries),
            )));
        }

        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let data_start = PersistentHeader::size() as usize;
        let entry_size = key_size + val_size;
        let kv_offset = data_start + (idx as usize) * entry_size;

        unsafe {
            let base_ptr = core.phys_buf.as_ref().as_ptr() as *mut u8;
            let buf = std::slice::from_raw_parts_mut(base_ptr, core.node_size() as usize);

            // Update both key and value in place (C++ set_nth_obj)
            key.serialize_to(&mut buf[kv_offset..kv_offset + key_size], true).map_err(BtreeError::Io)?;
            val.serialize_to(&mut buf[kv_offset + key_size..kv_offset + key_size + val_size])
                .map_err(BtreeError::Io)?;
        }
        core.inc_gen();
        Ok(())
    }

    fn remove(&self, core: &NodeCore, idx: u32) -> Result<(), BtreeError> {
        <Self as NodeOps<K, V>>::remove_range(self, core, idx, idx)
    }

    fn remove_range(&self, core: &NodeCore, start_idx: u32, end_idx: u32) -> Result<(), BtreeError> {
        let nentries = core.get_persistent_header().nentries();
        if start_idx > nentries || end_idx > nentries {
            return Err(BtreeError::Io(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Remove range [{}, {}] out of range (nentries={})", start_idx, end_idx, nentries),
            )));
        }

        // Handle edge entry case
        if end_idx == nentries {
            debug_assert!(!core.is_leaf() && core.has_valid_edge(), "Removing edge entry requires valid edge");

            // Set the last key/value as edge entry
            // get_nth_value at (start_idx-1) and set_nth_value at nentries (edge)
            if start_idx > 0 {
                let edge_val = <Self as NodeOps<K, V>>::get_nth_value(self, core, start_idx - 1, false);
                let new_edge = edge_val.expect_inline("SimpleNode edge values cannot be overflow");
                core.update_edge(&new_edge);
            } else {
                core.invalidate_edge();
            }
            core.get_persistent_header_mut().set_nentries(start_idx - 1);
        } else {
            let key_size = K::FIXED_SERIALIZED_SIZE.unwrap() as usize;
            let val_size = V::FIXED_SERIALIZED_SIZE.unwrap() as usize;
            let entry_size = key_size + val_size;
            let data_start = PersistentHeader::size() as usize;

            // Normal case: shift entries
            let sz = ((nentries - end_idx - 1) as usize) * entry_size;
            if sz > 0 {
                let src_offset = data_start + ((end_idx + 1) as usize) * entry_size;
                let dst_offset = data_start + (start_idx as usize) * entry_size;

                unsafe {
                    let base_ptr = core.phys_buf.as_ref().as_ptr() as *mut u8;
                    std::ptr::copy(base_ptr.add(src_offset), base_ptr.add(dst_offset), sz);
                }
            }
            core.get_persistent_header_mut().set_nentries(nentries - (end_idx - start_idx + 1));
        }

        core.inc_gen();
        Ok(())
    }

    fn remove_all(&self, core: &NodeCore) {
        core.get_persistent_header_mut().set_nentries(0);
        core.get_persistent_header_mut().edge_id = super::super::btree_node::EMPTY_BNODEID;
        core.inc_gen();
    }

    fn get_all_kvs(&self, core: &NodeCore) -> Vec<(K, ValueOrOverflow<V>)> {
        let nentries = core.get_persistent_header().nentries();
        let mut result = Vec::with_capacity(nentries as usize);

        // SimpleNode layout: [PersistentHeader][K0][V0][K1][V1]...
        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let data_start = PersistentHeader::size() as usize;
        let entry_size = key_size + val_size;

        unsafe {
            let buf_u8 = std::slice::from_raw_parts(core.phys_buf.as_ref().as_ptr(), core.node_size() as usize);

            for i in 0..nentries {
                let k_offset = data_start + (i as usize) * entry_size;
                let v_offset = k_offset + key_size;

                let key = K::deserialize_from(&buf_u8[k_offset..k_offset + key_size], true)
                    .expect("Key deserialization failed");
                let val = V::deserialize_from(&buf_u8[v_offset..v_offset + val_size], true)
                    .expect("Value deserialization failed");

                result.push((key, ValueOrOverflow::Inline(val))); // SimpleNode: always inline (no overflow support)
            }
        }

        result
    }

    fn get_nth_key(&self, core: &NodeCore, idx: u32, copy: bool) -> K {
        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let data_start = PersistentHeader::size() as usize;
        let entry_size = key_size + val_size;
        let k_offset = data_start + (idx as usize) * entry_size;

        unsafe {
            let buf = std::slice::from_raw_parts(core.phys_buf.as_ref().as_ptr(), core.node_size() as usize);
            K::deserialize_from(&buf[k_offset..k_offset + key_size], copy).expect("Key deserialization failed")
        }
    }

    fn get_nth_value(&self, core: &NodeCore, idx: u32, copy: bool) -> ValueOrOverflow<V> {
        let key_size = K::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let val_size = V::FIXED_SERIALIZED_SIZE.unwrap() as usize;
        let data_start = PersistentHeader::size() as usize;
        let entry_size = key_size + val_size;
        let v_offset = data_start + (idx as usize) * entry_size + key_size;

        unsafe {
            let buf = std::slice::from_raw_parts(core.phys_buf.as_ref().as_ptr(), core.node_size() as usize);
            let value =
                V::deserialize_from(&buf[v_offset..v_offset + val_size], copy).expect("Value deserialization failed");
            ValueOrOverflow::Inline(value) // SimpleNode: always inline (no overflow support)
        }
    }

    fn is_nth_value_overflow(&self, _core: &NodeCore, _idx: u32) -> bool {
        false // SimpleNode: always inline, no overflow support
    }

    fn move_out_to_right_by_entries(&self, src_core: &NodeCore, dst_core: &NodeCore, mut nentries: u32) -> u32 {
        let src_nentries = src_core.get_persistent_header().nentries();
        let dst_nentries = dst_core.get_persistent_header().nentries();

        let entry_size = (K::FIXED_SERIALIZED_SIZE.unwrap() + V::FIXED_SERIALIZED_SIZE.unwrap()) as usize;
        let data_start = PersistentHeader::size() as usize;

        // Calculate available entries in dst
        let dst_available = self.get_available_entries::<K, V>(dst_core);
        nentries = nentries.min(src_nentries).min(dst_available);
        let sz = (nentries as usize) * entry_size;

        if sz != 0 {
            // Shift existing entries in dst to make room
            let dst_sz = (dst_nentries as usize) * entry_size;
            if dst_sz > 0 {
                unsafe {
                    let dst_ptr = dst_core.phys_buf.as_ref().as_ptr() as *mut u8;
                    let src_offset = data_start;
                    let dst_offset = data_start + sz;
                    std::ptr::copy(dst_ptr.add(src_offset), dst_ptr.add(dst_offset), dst_sz);
                }
            }

            // Copy entries from src to dst
            let src_start_idx = src_nentries - nentries;
            unsafe {
                let src_ptr = src_core.phys_buf.as_ref().as_ptr();
                let dst_ptr = dst_core.phys_buf.as_ref().as_ptr() as *mut u8;
                let src_offset = data_start + (src_start_idx as usize) * entry_size;
                std::ptr::copy_nonoverlapping(src_ptr.add(src_offset), dst_ptr.add(data_start), sz);
            }
        }

        // Update entry counts
        dst_core.get_persistent_header_mut().set_nentries(dst_nentries + nentries);
        src_core.get_persistent_header_mut().set_nentries(src_nentries - nentries);

        // Handle edge entry for interior nodes
        if !src_core.is_leaf() && src_core.get_persistent_header().edge_id != super::super::btree_node::EMPTY_BNODEID {
            dst_core.get_persistent_header_mut().edge_id = src_core.get_persistent_header().edge_id;
            src_core.get_persistent_header_mut().edge_id = super::super::btree_node::EMPTY_BNODEID;
        }

        dst_core.inc_gen();
        src_core.inc_gen();

        nentries
    }

    fn move_out_to_right_by_size(&self, src_core: &NodeCore, dst_core: &NodeCore, size: u32) -> u32 {
        let entry_size = (K::FIXED_SERIALIZED_SIZE.unwrap() + V::FIXED_SERIALIZED_SIZE.unwrap()) as u32;
        <Self as NodeOps<K, V>>::move_out_to_right_by_entries(self, src_core, dst_core, size / entry_size)
    }

    fn append_copy_in_upto_size(
        &self,
        dst_core: &NodeCore,
        src_core: &NodeCore,
        src_cursor: &mut u32,
        upto_size: u32,
        copy_only_if_fits: bool,
    ) -> bool {
        let entry_size = (K::FIXED_SERIALIZED_SIZE.unwrap() + V::FIXED_SERIALIZED_SIZE.unwrap()) as usize;
        let data_start = PersistentHeader::size() as usize;

        let dst_nentries = dst_core.get_persistent_header().nentries();
        let src_nentries = src_core.get_persistent_header().nentries();

        // No entries to copy
        if *src_cursor >= src_nentries {
            // Copy edge only if src has one and dst doesn't (hasn't been copied yet)
            if src_core.has_valid_edge() && !dst_core.has_valid_edge() {
                dst_core.set_edge(src_core.get_persistent_header().edge_id);
            }
            return false; // Source node copy exhausted
        }

        // Check if already at capacity
        let occupied = dst_nentries as usize * entry_size;
        if occupied >= upto_size as usize {
            return true; // Source has more, but dst is full.
        }

        let room = upto_size as usize - occupied;

        // If we don't have room for even one entry, return early to avoid infinite loop
        if room < entry_size {
            return true; // Source has more, but dst is full (no room for another entry)
        }

        if copy_only_if_fits {
            // Check if all remaining entries fit
            let remaining_size = ((src_nentries - *src_cursor) as usize) * entry_size;
            if remaining_size > room {
                return true; // Source has more, but dst can't take all at once.
            }
        }

        // Calculate how many entries we can copy
        let nentries = (room / entry_size).min((src_nentries - *src_cursor) as usize) as u32;
        let copy_size = (nentries as usize) * entry_size;

        // Copy entries
        unsafe {
            let src_ptr = src_core.phys_buf.as_ref().as_ptr();
            let dst_ptr = dst_core.phys_buf.as_ref().as_ptr() as *mut u8;
            let src_offset = data_start + (*src_cursor as usize) * entry_size;
            let dst_offset = data_start + (dst_nentries as usize) * entry_size;
            std::ptr::copy_nonoverlapping(src_ptr.add(src_offset), dst_ptr.add(dst_offset), copy_size);
        }

        *src_cursor += nentries;
        dst_core.get_persistent_header_mut().set_nentries(dst_nentries + nentries);
        dst_core.inc_gen();

        // Copy edge if we copied everything
        if src_core.has_valid_edge() && *src_cursor == src_nentries {
            dst_core.set_edge(src_core.get_persistent_header().edge_id);
        }

        *src_cursor < src_nentries
    }

    fn available_size(&self, core: &NodeCore) -> u32 {
        let entry_size = (K::FIXED_SERIALIZED_SIZE.unwrap() + V::FIXED_SERIALIZED_SIZE.unwrap()) as u32;
        let nentries = core.get_persistent_header().nentries();
        let data_size = core.node_size() - PersistentHeader::size() as u32;
        data_size - (nentries * entry_size)
    }

    fn has_room_for_put(
        &self,
        core: &NodeCore,
        put_type: super::super::detail::btree_req::BtreePutType,
        _key_size: u32,
        _value_size: u32,
    ) -> bool {
        // UPSERT/INSERT need available space, UPDATE doesn't (modifies in-place)
        use super::super::detail::btree_req::BtreePutType;
        match put_type {
            BtreePutType::Upsert | BtreePutType::Insert => self.get_available_entries::<K, V>(core) > 0,
            BtreePutType::Update => true,
        }
    }
}

impl SimpleNodeOps {
    /// Helper: Get number of available entries
    fn get_available_entries<K: BtreeKey, V: BtreeValue>(&self, core: &NodeCore) -> u32 {
        let entry_size = (K::FIXED_SERIALIZED_SIZE.unwrap() + V::FIXED_SERIALIZED_SIZE.unwrap()) as u32;
        let avail_size = (core.node_size() - PersistentHeader::size() as u32)
            - (core.get_persistent_header().nentries() * entry_size);
        avail_size / entry_size
    }
}
