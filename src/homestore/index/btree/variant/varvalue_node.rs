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

//! VarValue Node - Fixed-size keys, variable-length values
//!
//! This module provides the policy for VarValue nodes.
//! Corresponds to C++ VarValueSizeNode in varlen_node.hpp

use super::varlen_node_common::{
    VarNodeOps, VarRecordOps, VarValueRecord,
    get_record_ptr, get_record_ptr_mut,
};
use super::super::btree_node::NodeCore;

/// Zero-sized policy for VarValue nodes
/// 
/// VarValue = fixed-size keys + variable-length values
/// Record format: [obj_offset:16, value_len:16] = 4 bytes
pub struct VarValueRecordOps;

impl VarRecordOps for VarValueRecordOps {
    #[inline]
    fn record_size(&self) -> usize { 
        VarValueRecord::size() 
    }
    
    #[inline]
    fn node_variant_type(&self) -> u8 { 
        2  // VAR_VALUE
    }
    
    #[inline]
    fn get_key_size(&self, _core: &NodeCore, _idx: u32) -> usize {
        // VarValue = FIXED key, variable value
        // Key size is determined by K type at compile time
        // This will panic if K doesn't have FIXED_SERIALIZED_SIZE - that's correct!
        panic!("VarValue get_key_size should use type's FIXED_SERIALIZED_SIZE - call site bug")
    }
    
    #[inline]
    fn get_value_size(&self, core: &NodeCore, idx: u32) -> usize {
        let ptr = get_record_ptr(core, idx, self.record_size());
        unsafe { 
            (*(ptr as *const VarValueRecord)).value_len as usize 
        }
    }
    
    #[inline]
    fn set_key_len(&self, _core: &NodeCore, _idx: u32, _len: usize) {
        // No-op for VarValue (key size is fixed, part of K type)
        // Key length is not stored in record metadata
    }
    
    #[inline]
    fn set_value_len(&self, core: &NodeCore, idx: u32, len: usize) {
        let ptr = get_record_ptr_mut(core, idx, self.record_size());
        unsafe { 
            (*(ptr as *mut VarValueRecord)).value_len = len as u16;
        }
    }
}

/// Public type alias for VarValue node operations
/// 
/// This is the type used in static singletons and node dispatch
pub type VarValueNodeOps = VarNodeOps<VarValueRecordOps>;
