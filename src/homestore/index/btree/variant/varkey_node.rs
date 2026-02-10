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

//! VarKey Node - Variable-length keys, fixed-size values
//!
//! This module provides the policy for VarKey nodes.
//! Corresponds to C++ VarKeySizeNode in varlen_node.hpp

use super::varlen_node_common::{VarNodeOps, VarRecordOps, VarKeyRecord, get_record_ptr, get_record_ptr_mut};
use super::super::btree_node::NodeCore;

/// Zero-sized policy for VarKey nodes
///
/// VarKey = variable-length keys + fixed-size values
/// Record format: [obj_offset:16, key_len:16] = 4 bytes
pub struct VarKeyRecordOps;

impl VarRecordOps for VarKeyRecordOps {
    #[inline]
    fn record_size(&self) -> usize { VarKeyRecord::size() }

    #[inline]
    fn node_variant_type(&self) -> u8 {
        1 // VAR_KEY
    }

    #[inline]
    fn get_key_size(&self, core: &NodeCore, idx: u32) -> usize {
        let ptr = get_record_ptr(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts(ptr, self.record_size()) };
        let record = unsafe { &*(slice.as_ptr() as *const VarKeyRecord) };
        record.key_len() as usize
    }

    #[inline]
    fn get_value_size(&self, _core: &NodeCore, _idx: u32) -> usize {
        // VarKey = variable key, FIXED value
        // Value size is determined by V type at compile time
        // This will panic if V doesn't have FIXED_SERIALIZED_SIZE - that's correct!
        panic!("VarKey get_value_size should use type's FIXED_SERIALIZED_SIZE - call site bug")
    }

    #[inline]
    fn is_value_overflow(&self, _core: &NodeCore, _idx: u32) -> bool {
        // VarKey has FIXED values, overflow not supported
        false
    }

    #[inline]
    fn set_key_len(&self, core: &NodeCore, idx: u32, len: usize) {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        let slice = unsafe { std::slice::from_raw_parts_mut(ptr, self.record_size()) };
        let record = VarKeyRecord::from_bytes_mut(slice);
        record.set_key_len(len as u16);
    }

    #[inline]
    fn set_value_len(&self, _core: &NodeCore, _idx: u32, _len: usize, _is_overflow: bool) {
        // No-op for VarKey (value size is fixed, part of V type)
        // Value length is not stored in record metadata
    }
}

/// Public type alias for VarKey node operations
///
/// This is the type used in static singletons and node dispatch
pub type VarKeyNodeOps = VarNodeOps<VarKeyRecordOps>;
