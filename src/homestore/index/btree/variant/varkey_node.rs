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

//! Variable-length key node operations
//!
//! This module implements the NodeOps trait for nodes with variable-length keys.
//! Corresponds to C++ varlen_node.hpp

use super::super::btree_kvs::{BtreeKey, BtreeValue};
use super::super::btree_node::{NodeCore, NodeOps};

/// Variable-length key node operations
/// 
/// Corresponds to C++ VarLenNode
pub struct VarKeyNodeOps;

impl<K: BtreeKey, V: BtreeValue> NodeOps<K, V> for VarKeyNodeOps {
    fn get_all_kvs(&self, _core: &NodeCore) -> Vec<(K, V)> {
        // TODO: Port C++ VarKeyNode::get_all_kvs logic
        panic!("VarKeyNode operations not yet implemented")
    }

    fn insert(&self, _core: &NodeCore, _idx: u32, _key: &K, _val: &V) -> Result<(), super::super::btree::BtreeError> {
        // TODO: Port C++ VarKeyNode::insert logic
        // Requires maintaining offset table and managing variable-length key storage
        Err(super::super::btree::BtreeError::Io(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "VarKeyNode operations not yet implemented",
        )))
    }

    fn remove(&self, _core: &NodeCore, _idx: u32) -> Result<(), super::super::btree::BtreeError> {
        // TODO: Port C++ VarKeyNode::remove logic
        Err(super::super::btree::BtreeError::Io(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "VarKeyNode operations not yet implemented",
        )))
    }

    fn get_nth_key(&self, _core: &NodeCore, _idx: u32, _copy: bool) -> K {
        // TODO: Port C++ VarKeyNode::get_nth_key
        panic!("VarKeyNode operations not yet implemented")
    }

    fn get_nth_value(&self, _core: &NodeCore, _idx: u32, _copy: bool) -> V {
        // TODO: Port C++ VarKeyNode::get_nth_value
        panic!("VarKeyNode operations not yet implemented")
    }

    fn update(&self, _core: &NodeCore, _idx: u32, _val: &V) -> Result<(), super::super::btree::BtreeError> {
        // TODO: Port C++ VarKeyNode::update logic
        Err(super::super::btree::BtreeError::Io(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "VarKeyNode operations not yet implemented",
        )))
    }

    fn remove_range(&self, _core: &NodeCore, _start_idx: u32, _end_idx: u32) -> Result<(), super::super::btree::BtreeError> {
        Err(super::super::btree::BtreeError::Io(std::io::Error::new(
            std::io::ErrorKind::Unsupported, 
            "VarKeyNode not yet implemented"
        )))
    }
    
    fn remove_all(&self, _core: &NodeCore) {
        panic!("VarKeyNode operations not yet implemented")
    }

    fn move_out_to_right_by_entries(&self, _src_core: &NodeCore, _dst_core: &NodeCore, _nentries: u32) -> u32 {
        panic!("VarKeyNode operations not yet implemented")
    }
    
    fn move_out_to_right_by_size(&self, _src_core: &NodeCore, _dst_core: &NodeCore, _size: u32) -> u32 {
        panic!("VarKeyNode operations not yet implemented")
    }
    
    fn append_copy_in_upto_size(&self, _dst_core: &NodeCore, _src_core: &NodeCore, _other_cursor: &mut u32, 
                                 _upto_size: u32, _copy_only_if_fits: bool) -> bool {
        panic!("VarKeyNode operations not yet implemented")
    }
    
    fn available_size(&self, _core: &NodeCore) -> u32 {
        panic!("VarKeyNode operations not yet implemented")
    }
    
    fn has_room_for_put(&self, _core: &NodeCore, _put_type: super::super::detail::btree_req::BtreePutType, 
                        _key_size: u32, _value_size: u32) -> bool {
        panic!("VarKeyNode operations not yet implemented")
    }
}
