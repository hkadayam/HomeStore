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

//! Integration test for btree node operations
//!
//! This test only compiles the btree modules (no device/blob/checkpoint layer).
//! Run with: cargo test --test btree_node_tests
//!
//! Benefits:
//! - Fast compilation (only btree + iomgr)
//! - No dependencies on device layer
//! - Tests core node operations in isolation

// Include the test module from index/btree/tests/test_btree_node.rs
// This avoids code duplication while allowing the integration test to control imports
#[path = "../index/btree/tests/test_btree_node.rs"]
mod test_btree_node;
