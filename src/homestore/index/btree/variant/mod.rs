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

//! Node variant implementations
//! 
//! Different node types for different key/value layouts:
//! - SimpleNode: Fixed-size keys and values
//! - VarKeyNode: Variable-length keys, fixed-size values
//! - VarValueNode: Fixed-size keys, variable-length values
//! - VarObjNode: Variable-length keys and values
//! - PrefixCompressNode: Variable-length with prefix compression (for secondary indexes)

pub mod simple_node;
pub mod varlen_node_common;
pub mod varkey_node;
pub mod varvalue_node;
pub mod varobj_node;
pub mod prefix_compress_node;

pub use simple_node::SimpleNodeOps;
pub use varkey_node::{VarKeyNodeOps, VarKeyRecordOps};
pub use varvalue_node::{VarValueNodeOps, VarValueRecordOps};
pub use varobj_node::{VarObjNodeOps, VarObjRecordOps};
pub use prefix_compress_node::PrefixCompressNodeOps;

// Re-export varlen common structures and implementation for convenience
pub use varlen_node_common::{
    VarNodeHeader, BtreeObjRecord, 
    VarKeyRecord, VarValueRecord, VarObjRecord,
    VarRecordOps, VarNodeOps,
    get_var_header, get_var_header_mut,
    get_record_ptr, get_record_ptr_mut,
    get_obj_ptr, get_obj_ptr_mut,
    get_arena_free_space,
};