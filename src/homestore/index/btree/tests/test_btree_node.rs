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

//! Btree Node Tests
//!
//! Tests for btree node operations (put, get, remove, update, etc.)
//! Parameterized over different key/value types and node variants.
//!
//! Design matches C++ TYPED_TEST pattern:
//! - Generic test implementations (test_sequential_insert, etc.)
//! - NodeTestConfig trait (like C++ TestType structs)
//! - Macro to instantiate tests for all variants

use std::collections::BTreeMap;
use std::marker::PhantomData;
use std::vec::Vec;
use triomphe::Arc as TArc;

use rand::{Rng, SeedableRng, rngs::StdRng};

use crate::{
    index::btree::btree_node::{
        Node, NodeCore, LockType, InternalLockGuard, NodeOps, SIMPLE_NODE_OPS, VAR_KEY_NODE_OPS, VAR_VALUE_NODE_OPS,
        VAR_OBJ_NODE_OPS, PREFIX_COMPRESS_NODE_OPS,
    },
    index::btree::btree_kvs::{BtreeKey, BtreeValue, ValueOrOverflow},
    index::btree::BtreeError, // Re-exported from btree_types at btree module level
    index::btree::detail::btree_req::BtreePutType,
};

const NODE_SIZE: usize = 4096;

//================================================================================
// Test Value Trait
//================================================================================

/// Trait for test key/value types that can generate sequential test values
trait TestValue: Clone + PartialEq + std::fmt::Debug {
    fn generate(counter: &mut u64) -> Self;
}

impl TestValue for u32 {
    fn generate(counter: &mut u64) -> Self {
        let val = *counter as u32;
        *counter = counter.wrapping_add(1);
        val
    }
}

impl TestValue for u64 {
    fn generate(counter: &mut u64) -> Self {
        let val = *counter;
        *counter = counter.wrapping_add(1);
        val
    }
}

//================================================================================
// Node Variant Abstraction
//================================================================================

/// Trait to abstract over node variants (SimpleNode, VarKeyNode, etc.)
trait NodeVariant {
    fn node_variant_id() -> u8;
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>() -> &'static dyn NodeOps<K, V>;
}

struct SimpleNodeVariant;
impl NodeVariant for SimpleNodeVariant {
    fn node_variant_id() -> u8 { 0 }
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>() -> &'static dyn NodeOps<K, V> { &SIMPLE_NODE_OPS }
}

struct VarKeyNodeVariant;
impl NodeVariant for VarKeyNodeVariant {
    fn node_variant_id() -> u8 { 1 }
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>() -> &'static dyn NodeOps<K, V> {
        &VAR_KEY_NODE_OPS
    }
}

struct VarValueNodeVariant;
impl NodeVariant for VarValueNodeVariant {
    fn node_variant_id() -> u8 { 2 }
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>() -> &'static dyn NodeOps<K, V> {
        &VAR_VALUE_NODE_OPS
    }
}

struct VarObjNodeVariant;
impl NodeVariant for VarObjNodeVariant {
    fn node_variant_id() -> u8 { 3 }
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>() -> &'static dyn NodeOps<K, V> {
        &VAR_OBJ_NODE_OPS
    }
}

struct PrefixCompressNodeVariant;
impl NodeVariant for PrefixCompressNodeVariant {
    fn node_variant_id() -> u8 { 4 }
    fn get_node_ops<K: BtreeKey + 'static, V: BtreeValue + 'static>() -> &'static dyn NodeOps<K, V> {
        &PREFIX_COMPRESS_NODE_OPS
    }
}

//================================================================================
// Node Test Configuration (matches C++ TestType pattern)
//================================================================================

/// Trait that bundles Key type, Value type, and Node variant
/// (Matches C++ test type structs like FixedLenNodeTest, VarKeySizeNodeTest, etc.)
trait NodeTestConfig {
    type K: BtreeKey + TestValue + 'static;
    type V: BtreeValue + TestValue + 'static;
    type Variant: NodeVariant;
}

/// Fixed-length node test (SimpleNode with u32 key, u64 value)
struct FixedLenNodeTest;
impl NodeTestConfig for FixedLenNodeTest {
    type K = u32;
    type V = u64;
    type Variant = SimpleNodeVariant;
}

/// Variable key size node test (VarKeyNode with u32 key, u64 value)
struct VarKeySizeNodeTest;
impl NodeTestConfig for VarKeySizeNodeTest {
    type K = u32;
    type V = u64;
    type Variant = VarKeyNodeVariant;
}

/// Variable value size node test (VarValueNode with u32 key, u64 value)
struct VarValueSizeNodeTest;
impl NodeTestConfig for VarValueSizeNodeTest {
    type K = u32;
    type V = u64;
    type Variant = VarValueNodeVariant;
}

/// Variable obj size node test (VarObjNode with u32 key, u64 value)
struct VarObjSizeNodeTest;
impl NodeTestConfig for VarObjSizeNodeTest {
    type K = u32;
    type V = u64;
    type Variant = VarObjNodeVariant;
}

/// Prefix compression node test (PrefixCompressNode with u32 key, u64 value)
struct PrefixCompressNodeTest;
impl NodeTestConfig for PrefixCompressNodeTest {
    type K = u32;
    type V = u64;
    type Variant = PrefixCompressNodeVariant;
}

//================================================================================
// Generic Node Test Fixture
//================================================================================

struct NodeTest<K, V, N>
where
    K: BtreeKey + TestValue + 'static,
    V: BtreeValue + TestValue + 'static,
    N: NodeVariant,
{
    node: Node,
    shadow_map: BTreeMap<K, V>,
    index_to_key: Vec<K>,
    value_counter: u64,
    _phantom: PhantomData<N>,
}

impl<K, V, N> NodeTest<K, V, N>
where
    K: BtreeKey + TestValue + 'static,
    V: BtreeValue + TestValue + 'static,
    N: NodeVariant,
{
    async fn new() -> Self {
        let node_core = NodeCore::new(/* node_id= */ 1, /* is_leaf= */ true, NODE_SIZE as u32);

        // Set the node variant in persistent header
        {
            let header = node_core.get_persistent_header_mut();
            header.node_variant = N::node_variant_id();
        }

        // Initialize node with the appropriate NodeOps based on variant N
        N::get_node_ops::<K, V>().init_new_node(&node_core);

        let node_core = TArc::new(node_core);
        let node = unsafe {
            let write_guard = node_core.lock.write_lock().await;
            let write_guard = std::mem::transmute(write_guard);
            Node {
                core: node_core,
                lock_type: LockType::Write,
                _guard: InternalLockGuard::Write(write_guard),
            }
        };

        Self {
            node,
            shadow_map: BTreeMap::new(),
            index_to_key: Vec::new(),
            value_counter: 1000,
            _phantom: PhantomData,
        }
    }

    fn has_room(&self) -> bool {
        self.node.available_size::<K, V>() > (K::FIXED_SERIALIZED_SIZE.unwrap() + V::FIXED_SERIALIZED_SIZE.unwrap())
    }

    fn put(&mut self, k: K, put_type: BtreePutType) {
        let value = V::generate(&mut self.value_counter);

        let expected_success = match put_type {
            BtreePutType::Insert => !self.shadow_map.contains_key(&k),
            BtreePutType::Update => self.shadow_map.contains_key(&k),
            BtreePutType::Upsert => true,
        };

        let value_for_shadow = value.clone();
        let (found, idx) = self.node.find::<K, V>(&k);

        let result = match put_type {
            BtreePutType::Insert => {
                if found {
                    Err(BtreeError::KeyAlreadyExists)
                } else {
                    let val_ref = ValueOrOverflow::Inline(value);
                    self.node.insert::<K, V>(idx, &k, &val_ref)
                }
            }
            BtreePutType::Update => {
                if !found {
                    Err(BtreeError::KeyNotFound)
                } else {
                    let val_ref = ValueOrOverflow::Inline(value);
                    self.node.update::<K, V>(idx, &val_ref)
                }
            }
            BtreePutType::Upsert => {
                if found {
                    let val_ref = ValueOrOverflow::Inline(value);
                    self.node.update::<K, V>(idx, &val_ref)
                } else {
                    let val_ref = ValueOrOverflow::Inline(value);
                    self.node.insert::<K, V>(idx, &k, &val_ref)
                }
            }
        };

        let mut update_maps = |key: K, value: V| {
            self.shadow_map.insert(key.clone(), value);
            if idx == self.index_to_key.len() as u32 {
                self.index_to_key.push(key);
            } else {
                self.index_to_key.insert(idx as usize, key);
            }
        };

        match put_type {
            BtreePutType::Insert => {
                if expected_success {
                    assert!(result.is_ok(), "Expected INSERT of key {:?} to succeed", k);
                    update_maps(k, value_for_shadow);
                } else {
                    assert!(result.is_err(), "Expected INSERT of existing key {:?} to fail", k);
                }
            }
            BtreePutType::Update => {
                if expected_success {
                    assert!(result.is_ok(), "Expected UPDATE of key {:?} to succeed", k);
                    update_maps(k, value_for_shadow);
                } else {
                    assert!(result.is_err(), "Expected UPDATE of non-existing key {:?} to fail", k);
                }
            }
            BtreePutType::Upsert => {
                assert!(result.is_ok(), "Expected UPSERT of key {:?} to succeed", k);
                update_maps(k, value_for_shadow);
            }
        }
    }

    fn remove(&mut self, k: K) {
        let shadow_found = self.shadow_map.contains_key(&k);
        let (found, idx) = self.node.find::<K, V>(&k);

        if found {
            let result = self.node.remove::<K, V>(idx);
            assert!(result.is_ok(), "Expected remove of key {:?} to succeed", k);
            assert!(shadow_found, "Found key {:?} in node but not in shadow map", k);
            self.shadow_map.remove(&k);
            self.index_to_key.remove(idx as usize);
        } else {
            assert!(!shadow_found, "Key {:?} in shadow map but not found in node", k);
        }
    }

    fn update(&mut self, k: K) {
        let value = V::generate(&mut self.value_counter);
        let (found, idx) = self.node.find::<K, V>(&k);

        let expected_success = self.shadow_map.contains_key(&k);
        assert_eq!(found, expected_success, "find() mismatch for key {:?}", k);

        if found {
            let val_ref = ValueOrOverflow::Inline(value.clone());
            let result = self.node.update::<K, V>(idx, &val_ref);
            assert!(result.is_ok(), "Expected update of key {:?} to succeed", k);
            self.shadow_map.insert(k, value);
        }
    }

    fn validate_get_all(&self) {
        let kvs = self.node.get_all_kvs::<K, V>();
        assert_eq!(
            kvs.len(),
            self.shadow_map.len(),
            "get_all returned {} entries, expected {}",
            kvs.len(),
            self.shadow_map.len()
        );

        for (k, val_ref) in &kvs {
            let shadow_v = self.shadow_map.get(k).expect(&format!("Key {:?} from node not found in shadow map", k));
            // In tests, values are always inline (no overflow)
            let v = val_ref.clone().expect_inline("Unexpected overflow in test");
            assert_eq!(&v, shadow_v, "Value mismatch for key {:?}", k);
        }
    }

    fn validate_specific(&self, k: K) {
        let (found, idx) = self.node.find::<K, V>(&k);
        let shadow_found = self.shadow_map.contains_key(&k);

        assert_eq!(found, shadow_found, "find() returned {}, expected {} for key {:?}", found, shadow_found, k);

        if found {
            let node_value = self.node.get_nth_value::<K, V>(idx, /* copy= */ true);
            let shadow_value = self.shadow_map.get(&k).unwrap();
            let inline_value = node_value.expect_inline("Test values should always be inline");
            assert_eq!(&inline_value, shadow_value, "Value mismatch for key {:?}", k);
        }
    }

    fn print(&self) {
        println!("Node entries: {}, shadow_map size: {}", self.node.total_entries(), self.shadow_map.len());
    }

    fn dump_node(&self) {
        println!("\n=== NODE DUMP ===");
        println!("{}", self.node.to_string::<K, V>());
        println!("=================\n");
    }

    /// Helper to put a list of keys
    fn put_list(&mut self, keys: &[u64]) {
        for &k_seed in keys {
            let key = K::generate(&mut k_seed.clone());
            self.put(key, BtreePutType::Insert);
        }
    }

    /// Helper to put a range of keys using multi_put
    fn put_range(&mut self, start: u64, count: u32) {
        for i in 0..count {
            let key = K::generate(&mut (start + i as u64));
            self.put(key, BtreePutType::Upsert);
        }
    }

    fn remove_range(&mut self, start_idx: u32, end_idx: u32) {
        let result = self.node.remove_range::<K, V>(start_idx, end_idx);
        assert!(result.is_ok(), "remove_range [{}, {}] failed", start_idx, end_idx);

        for idx in start_idx..=end_idx {
            if let Some(key) = self.index_to_key.get(idx as usize) {
                self.shadow_map.remove(key);
            }
        }
        self.index_to_key.drain(start_idx as usize..=end_idx as usize);
    }
}

//================================================================================
// Generic Test Implementations (like C++ TYPED_TEST)
//================================================================================

async fn test_sequential_insert<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    for i in 0..100 {
        if !test.has_room() {
            break;
        }
        test.put(C::K::generate(&mut (i as u64)), BtreePutType::Insert);
    }

    test.print();
    test.dump_node();
    test.validate_get_all();
}

async fn test_simple_insert<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    // Test occupied size tracking
    let oc = test.node.occupied_size::<C::K, C::V>();
    test.put(C::K::generate(&mut 1u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 2u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 3u64), BtreePutType::Insert);
    test.remove(C::K::generate(&mut 2u64));
    test.remove(C::K::generate(&mut 1u64));
    test.remove(C::K::generate(&mut 3u64));
    let oc2 = test.node.occupied_size::<C::K, C::V>();
    assert_eq!(oc, oc2, "Occupied size cannot be more than original size");

    test.put(C::K::generate(&mut 1u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 2u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 3u64), BtreePutType::Insert);
    test.remove(C::K::generate(&mut 3u64));
    test.remove(C::K::generate(&mut 2u64));
    test.remove(C::K::generate(&mut 1u64));
    assert_eq!(oc, oc2, "Occupied size must be the same as original size");

    test.put(C::K::generate(&mut 2u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 1u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 4u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 3u64), BtreePutType::Insert);
    for i in 5..=50 {
        test.put(C::K::generate(&mut (i as u64)), BtreePutType::Insert);
    }

    test.dump_node();
    test.validate_get_all();
}

async fn test_reverse_insert<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    for i in (0..100).rev() {
        if !test.has_room() {
            break;
        }
        test.put(C::K::generate(&mut (i as u64)), BtreePutType::Insert);
    }

    test.print();
    test.validate_get_all();
    test.dump_node();
}

async fn test_remove<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    test.put(C::K::generate(&mut 0u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 1u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 2u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 100u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 101u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 99u64), BtreePutType::Insert);

    test.remove(C::K::generate(&mut 0u64));
    test.remove(C::K::generate(&mut 0u64)); // Remove non-existing
    test.remove(C::K::generate(&mut 1u64));
    test.remove(C::K::generate(&mut 2u64));
    test.remove(C::K::generate(&mut 99u64));

    test.print();
    test.validate_get_all();
    test.dump_node();
}

async fn test_update<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    test.put(C::K::generate(&mut 1u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 2u64), BtreePutType::Insert);
    test.put(C::K::generate(&mut 3u64), BtreePutType::Insert);

    test.update(C::K::generate(&mut 1u64));
    test.update(C::K::generate(&mut 2u64));
    test.update(C::K::generate(&mut 3u64));
    test.update(C::K::generate(&mut 999u64)); // Update non-existing

    test.validate_get_all();
    test.validate_specific(C::K::generate(&mut 1u64));
    test.validate_specific(C::K::generate(&mut 2u64));
    test.validate_specific(C::K::generate(&mut 3u64));
    test.dump_node();
}

async fn test_mixed_operations<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    let operations = [(0, 0u64), (1, 5), (2, 3), (0, 25), (1, 10), (2, 15), (0, 12), (1, 20), (2, 8), (0, 35)];

    for (op, key_seed) in operations.iter() {
        if !test.has_room() {
            break;
        }

        let key = C::K::generate(&mut key_seed.clone());
        match op {
            0 => test.put(key, BtreePutType::Upsert),
            1 => test.remove(key),
            2 => test.update(key),
            _ => unreachable!(),
        }
    }

    test.print();
    test.validate_get_all();
    test.dump_node();
}

async fn test_remove_range_index<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    for i in 0..20 {
        test.put(C::K::generate(&mut (i as u64)), BtreePutType::Insert);
    }
    test.print();

    // Remove middle range [5,10]
    test.remove_range(5, 10);
    test.print();
    test.validate_get_all();

    // Remove from start [0,5] (but 5 was already removed, so effective range shrinks)
    test.remove_range(0, 5);
    test.print();
    test.validate_get_all();
    test.dump_node();
}

async fn test_move<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;
    let mut test2 = NodeTest::<C::K, C::V, C::Variant>::new().await;

    // Put entries into node1
    let keys = vec![0u64, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    test.put_list(&keys);
    test.print();

    let list_size = keys.len() as u32;

    // Full node move to right
    let moved = test.node.move_out_to_right_by_entries::<C::K, C::V>(&test2.node, list_size);
    assert_eq!(moved, list_size, "Should move all entries");
    assert_eq!(test.node.total_entries(), 0, "Source node should be empty");
    assert_eq!(test2.node.total_entries(), list_size, "Dest node should have all entries");

    // Move shadow map entries
    for &k in &keys {
        let mut k_seed = k;
        let key = C::K::generate(&mut k_seed);
        if let Some(val) = test.shadow_map.remove(&key) {
            test2.shadow_map.insert(key, val);
        }
    }
    test2.validate_get_all();

    // Full copy back - use NODE_SIZE to ensure enough space for varlen nodes
    let mut cursor = 0u32;
    let has_more = test.node.append_copy_in_upto_size::<C::K, C::V>(
        &test2.node,
        &mut cursor,
        NODE_SIZE as u32,
        /* copy_only_if_fits= */ true,
    );
    assert!(!has_more, "append_copy_in should succeed");
    assert_eq!(cursor, test2.node.total_entries(), "Cursor should be at end");
    assert_eq!(test.node.total_entries(), list_size, "Node1 should have all entries");

    // Restore shadow map
    for &k in &keys {
        let mut k_seed = k;
        let key = C::K::generate(&mut k_seed);
        if let Some(val) = test2.shadow_map.get(&key) {
            test.shadow_map.insert(key, val.clone());
        }
    }
    test.validate_get_all();

    // Test remove_all
    test2.node.remove_all::<C::K, C::V>();
    assert_eq!(test2.node.total_entries(), 0, "Node should be empty after remove_all");
    test2.shadow_map.clear();
    test2.validate_get_all();
    test.dump_node();
}

async fn test_range_put_get<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;

    // Put ranges: [0,4], [5,9], [10,14], ... [35,39]
    for i in (0..40).step_by(5) {
        test.put_range(i, 5);
    }

    test.validate_get_all();
    test.dump_node();
}

async fn test_random_insert_remove_update<C: NodeTestConfig>() {
    let mut test = NodeTest::<C::K, C::V, C::Variant>::new().await;
    let mut rng = StdRng::seed_from_u64(42);

    // Phase 1: Fill node with random keys
    let mut num_inserted = 0;
    while test.has_room() {
        let rand_key = rng.gen_range(0..6000);
        let key = C::K::generate(&mut (rand_key as u64));
        test.put(key, BtreePutType::Insert);
        num_inserted += 1;
    }
    println!("After random insertion of {} objects", num_inserted);
    test.print();
    test.validate_get_all();

    // Phase 2: Remove half randomly
    let to_remove = num_inserted / 2;
    for _ in 0..to_remove {
        if test.shadow_map.is_empty() {
            break;
        }

        // Pick a random index and remove that entry
        let idx = rng.gen_range(0..test.shadow_map.len());
        if let Some(key) = test.shadow_map.keys().nth(idx).cloned() {
            test.remove(key);
        }
    }
    println!("After random removal of {} objects", to_remove);
    test.print();
    test.validate_get_all();

    // Phase 3: Update half randomly
    let to_update = test.shadow_map.len() / 2;
    for _ in 0..to_update {
        if test.shadow_map.is_empty() {
            break;
        }

        let idx = rng.gen_range(0..test.shadow_map.len());
        if let Some(key) = test.shadow_map.keys().nth(idx).cloned() {
            test.update(key);
        }
    }
    println!("After update of {} entries", to_update);
    test.print();
    test.validate_get_all();
    test.dump_node();
}

//================================================================================
// Test Instantiations for All Variants
//================================================================================
// To add a new test: write the generic test_foo<C>() above, then add instantiations below
// To add a new variant: add instantiations for all tests below

macro_rules! instantiate_typed_test {
    ($test_fn:ident, $variant:ty) => {
        paste::paste! {
            #[iomgr::iomanager_test]
            async fn [<$test_fn _ $variant:snake>]() {
                $test_fn::<$variant>().await;
            }
        }
    };
}

// FixedLenNodeTest
instantiate_typed_test!(test_sequential_insert, FixedLenNodeTest);
instantiate_typed_test!(test_simple_insert, FixedLenNodeTest);
instantiate_typed_test!(test_reverse_insert, FixedLenNodeTest);
instantiate_typed_test!(test_remove, FixedLenNodeTest);
instantiate_typed_test!(test_update, FixedLenNodeTest);
instantiate_typed_test!(test_mixed_operations, FixedLenNodeTest);
instantiate_typed_test!(test_remove_range_index, FixedLenNodeTest);
instantiate_typed_test!(test_move, FixedLenNodeTest);
instantiate_typed_test!(test_range_put_get, FixedLenNodeTest);
instantiate_typed_test!(test_random_insert_remove_update, FixedLenNodeTest);

// VarKeySizeNodeTest
instantiate_typed_test!(test_sequential_insert, VarKeySizeNodeTest);
instantiate_typed_test!(test_simple_insert, VarKeySizeNodeTest);
instantiate_typed_test!(test_reverse_insert, VarKeySizeNodeTest);
instantiate_typed_test!(test_remove, VarKeySizeNodeTest);
instantiate_typed_test!(test_update, VarKeySizeNodeTest);
instantiate_typed_test!(test_mixed_operations, VarKeySizeNodeTest);
instantiate_typed_test!(test_remove_range_index, VarKeySizeNodeTest);
instantiate_typed_test!(test_move, VarKeySizeNodeTest);
instantiate_typed_test!(test_range_put_get, VarKeySizeNodeTest);
instantiate_typed_test!(test_random_insert_remove_update, VarKeySizeNodeTest);

// VarValueSizeNodeTest
instantiate_typed_test!(test_sequential_insert, VarValueSizeNodeTest);
instantiate_typed_test!(test_simple_insert, VarValueSizeNodeTest);
instantiate_typed_test!(test_reverse_insert, VarValueSizeNodeTest);
instantiate_typed_test!(test_remove, VarValueSizeNodeTest);
instantiate_typed_test!(test_update, VarValueSizeNodeTest);
instantiate_typed_test!(test_mixed_operations, VarValueSizeNodeTest);
instantiate_typed_test!(test_remove_range_index, VarValueSizeNodeTest);
instantiate_typed_test!(test_move, VarValueSizeNodeTest);
instantiate_typed_test!(test_range_put_get, VarValueSizeNodeTest);
instantiate_typed_test!(test_random_insert_remove_update, VarValueSizeNodeTest);

// VarObjSizeNodeTest
instantiate_typed_test!(test_sequential_insert, VarObjSizeNodeTest);
instantiate_typed_test!(test_simple_insert, VarObjSizeNodeTest);
instantiate_typed_test!(test_reverse_insert, VarObjSizeNodeTest);
instantiate_typed_test!(test_remove, VarObjSizeNodeTest);
instantiate_typed_test!(test_update, VarObjSizeNodeTest);
instantiate_typed_test!(test_mixed_operations, VarObjSizeNodeTest);
instantiate_typed_test!(test_remove_range_index, VarObjSizeNodeTest);
instantiate_typed_test!(test_move, VarObjSizeNodeTest);
instantiate_typed_test!(test_range_put_get, VarObjSizeNodeTest);
instantiate_typed_test!(test_random_insert_remove_update, VarObjSizeNodeTest);

// PrefixCompressNodeTest
/*instantiate_typed_test!(test_sequential_insert, PrefixCompressNodeTest);
instantiate_typed_test!(test_simple_insert, PrefixCompressNodeTest);
instantiate_typed_test!(test_reverse_insert, PrefixCompressNodeTest);
instantiate_typed_test!(test_remove, PrefixCompressNodeTest);
instantiate_typed_test!(test_update, PrefixCompressNodeTest);
instantiate_typed_test!(test_mixed_operations, PrefixCompressNodeTest);
instantiate_typed_test!(test_remove_range_index, PrefixCompressNodeTest);
instantiate_typed_test!(test_move, PrefixCompressNodeTest);
instantiate_typed_test!(test_range_put_get, PrefixCompressNodeTest);
instantiate_typed_test!(test_random_insert_remove_update, PrefixCompressNodeTest);
*/
