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

//! Shadow Map for Btree Test Validation
//!
//! Provides an in-memory reference map to validate btree operations.
//! Can be used across all btree tests (test_btree.rs, test_btree_node.rs, COW tests).

use std::collections::BTreeMap;

/// Shadow map for tracking expected state during testing
pub struct ShadowMap<K: Ord, V> {
    map: BTreeMap<K, V>,
    max_entries: u32,
}

impl<K: Ord + Clone, V: Clone> ShadowMap<K, V> {
    pub fn new(max_entries: u32) -> Self {
        Self { map: BTreeMap::new(), max_entries }
    }
    
    pub fn insert(&mut self, k: K, v: V) -> Option<V> {
        self.map.insert(k, v)
    }
    
    pub fn remove(&mut self, k: &K) -> Option<V> {
        self.map.remove(k)
    }
    
    pub fn get(&self, k: &K) -> Option<&V> {
        self.map.get(k)
    }
    
    pub fn exists_in_range(&self, k: &K, start: &K, end: &K) -> bool {
        if k < start || k > end { return false; }
        self.map.contains_key(k)
    }
    
    pub fn size(&self) -> usize {
        self.map.len()
    }
    
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
    
    pub fn keys(&self) -> impl Iterator<Item = &K> {
        self.map.keys()
    }
    
    pub fn iter(&self) -> impl Iterator<Item = (&K, &V)> {
        self.map.iter()
    }
}
