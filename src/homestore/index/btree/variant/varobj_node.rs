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
 ************************************************* */

//! VarObj Node - Variable-length keys and values
//!
//! This module provides the policy for VarObj nodes.
//! Corresponds to C++ VarObjSizeNode in varlen_node.hpp

use super::varlen_node_common::{VarNodeOps, VarRecordOps, VarObjRecord, get_record_ptr_mut};
use super::super::btree_node::NodeCore;

/// Zero-sized policy for VarObj nodes
///
/// VarObj = variable-length keys + variable-length values
/// Record format: [obj_offset:16, key_len:16, value_len:16] = 6 bytes
pub struct VarObjRecordOps;

impl VarRecordOps for VarObjRecordOps {
    #[inline]
    fn record_size(&self) -> usize { VarObjRecord::size() }

    #[inline]
    fn node_variant_type(&self) -> u8 {
        3 // VAR_OBJECT
    }

    #[inline]
    fn get_key_size(&self, core: &NodeCore, idx: u32) -> usize {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts_mut(ptr, self.record_size()) };
        let record = VarObjRecord::from_bytes_mut(slice);
        record.key_len() as usize
    }

    #[inline]
    fn get_value_size(&self, core: &NodeCore, idx: u32) -> usize {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts_mut(ptr, self.record_size()) };
        let record = VarObjRecord::from_bytes_mut(slice);
        record.value_len() as usize
    }

    #[inline]
    fn is_value_overflow(&self, core: &NodeCore, idx: u32) -> bool {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts_mut(ptr, self.record_size()) };
        let record = VarObjRecord::from_bytes_mut(slice);
        record.is_overflow()
    }

    #[inline]
    fn set_key_len(&self, core: &NodeCore, idx: u32, len: usize) {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts_mut(ptr, self.record_size()) };
        let record = VarObjRecord::from_bytes_mut(slice);
        record.set_key_len(len as u16);
    }

    #[inline]
    fn set_value_len(&self, core: &NodeCore, idx: u32, len: usize, is_overflow: bool) {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts_mut(ptr, self.record_size()) };
        let record = VarObjRecord::from_bytes_mut(slice);
        record.set_value_len_tuple(len as u16, is_overflow);
    }
}

/// Public type alias for VarObj node operations
///
/// This is the type used in static singletons and node dispatch
pub type VarObjNodeOps = VarNodeOps<VarObjRecordOps>;
