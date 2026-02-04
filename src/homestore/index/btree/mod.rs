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

//! Btree Module
//!
//! This module contains the btree implementation organized similar to C++:
//! - btree.rs - Main Btree struct definition and tree-level operations
//! - btree_node.rs - Node structure and operations
//! - btree_node_mgr.rs - Node management impl block + lock upgrades (like btree_node_mgr.ipp)
//! - detail/ - Split implementation files (mutate, query)
//! - underlying/ - Storage implementations (cow_btree, mem)

pub mod btree_node;      // Must come first (defines Node, NodeCore)
pub mod btree_kvs;       // Key/Value traits
pub mod variant;         // Node variant implementations (SimpleNode, VarKeyNode, etc.)
pub mod btree;           // Defines Btree struct
pub mod btree_node_mgr;  // Additional impl for Btree (must come after btree)
pub mod detail;
pub mod underlying;

// Re-export main types
pub use btree::*;
pub use btree_node::*;
pub use btree_kvs::*;    // Export key/value traits

// Note: btree_node_mgr methods are already part of Btree impl, no need to re-export

#[cfg(test)]
mod tests;
