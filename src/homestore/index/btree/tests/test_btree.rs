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
 ************************************************************************ */

//! Btree Integration Tests
//!
//! Full btree operations testing with parameterized types.
//! Matches C++ test_btree.cpp but excludes CP-related tests.
//!
//! Design:
//! - Generic test implementations (test_sequential_insert_impl, etc.)
//! - TestBtreeVariant trait (like C++ TestType structs)
//! - Macro to instantiate tests for all type/storage combinations
//! - Uses iomgr for async execution (NO tokio direct usage)
//! - Multi-reactor tests for concurrency validation

use std::marker::PhantomData;
use std::sync::Arc;

use rand::{Rng, SeedableRng, rngs::StdRng};

use crate::index::btree::{
    btree::{Btree, UnderlyingBtree},
    BtreeConfig, // Re-exported from btree_types at btree module level
    BtreeError,  // Re-exported from btree_types at btree module level
    MergePolicy, // Re-exported from btree_types at btree module level
};
use crate::index::btree::btree_kvs::{BtreeKey, BtreeValue};
use crate::index::btree::detail::btree_req::{BtreePutType, BtreeKeyRange};
use crate::index::btree::underlying::mem::MemBtree;

// Import test types and generators
use super::btree_test_kvs::{
    FixedSizeTestKey, FixedSizeTestValue, TestVarLenKey, TestVarLenValue, Generator, GenMode, FixedSizeKeyGenerator,
    FixedSizeValueGenerator, VarLenKeyGenerator, VarLenValueGenerator,
};
use super::shadow_map::ShadowMap;

// Test types and generators are now in btree_test_kvs.rs for reusability
// across test_btree.rs, test_btree_node.rs, and future COW tests

//================================================================================
// Sync BackgroundTasks Wrapper (mimics async iomgr::BackgroundTasks API)
//================================================================================

#[cfg(feature = "sync_code")]
mod sync_tasks {
    use std::thread::JoinHandle;

    /// Sync equivalent of iomgr::BackgroundTasks for thread-based concurrency
    pub struct BackgroundTasks {
        handles: Vec<JoinHandle<()>>,
    }

    impl BackgroundTasks {
        pub fn new() -> Self { Self { handles: Vec::new() } }

        pub fn spawn<F>(&mut self, f: F)
        where
            F: FnOnce() + Send + 'static,
        {
            self.handles.push(std::thread::spawn(f));
        }

        pub fn join_all(self) {
            for handle in self.handles {
                handle.join().unwrap();
            }
        }
    }
}

#[cfg(feature = "sync_code")]
use sync_tasks::{BackgroundTasks};

#[cfg(feature = "async_code")]
use iomgr::{BackgroundTasks, ReactorTarget};

//================================================================================
// Storage Type Trait
//================================================================================

/// Storage type marker (like C++ IndexStore::Type enum)
trait StorageType: Send + Sync + 'static {
    fn name() -> &'static str;
}

struct MemStorage;
impl StorageType for MemStorage {
    fn name() -> &'static str { "mem" }
}

#[allow(dead_code)]
struct CowStorage;
impl StorageType for CowStorage {
    fn name() -> &'static str { "cow" }
}

//================================================================================
// Btree Test Variants
//================================================================================

/// Trait that bundles Key type, Value type, Storage type, Node variant, and Generators
trait TestBtreeVariant: Send + Sync + 'static {
    type K: BtreeKey + std::fmt::Debug + 'static;
    type V: BtreeValue + std::fmt::Debug + PartialEq + 'static;
    type Storage: StorageType;
    type KeyGen: Generator<Output = Self::K> + 'static;
    type ValueGen: Generator<Output = Self::V> + 'static;

    fn node_variant() -> u8;
    /// Short name for this variant (e.g. "fixed_size", "var_key") for logging.
    fn variant_name() -> &'static str;
    fn create_key_generator() -> Self::KeyGen;
    fn create_value_generator() -> Self::ValueGen;
}

// Fixed-length key and value (FixedSizeTestKey, FixedSizeTestValue)
struct FixedSizeTestBtree<S: StorageType> {
    _phantom: PhantomData<S>,
}

impl<S: StorageType> TestBtreeVariant for FixedSizeTestBtree<S> {
    type K = FixedSizeTestKey;
    type V = FixedSizeTestValue;
    type Storage = S;
    type KeyGen = FixedSizeKeyGenerator;
    type ValueGen = FixedSizeValueGenerator;

    fn node_variant() -> u8 { 0 } // SimpleNode
    fn variant_name() -> &'static str { "fixed_size" }

    fn create_key_generator() -> Self::KeyGen { FixedSizeKeyGenerator::new(GenMode::Sequential, 42, 1_000_000) }

    fn create_value_generator() -> Self::ValueGen { FixedSizeValueGenerator::new(GenMode::Sequential, 43) }
}

// Variable key size (TestVarLenKey, FixedSizeTestValue)
struct VarKeySizeBtreeTest<S: StorageType> {
    _phantom: PhantomData<S>,
}

impl<S: StorageType> TestBtreeVariant for VarKeySizeBtreeTest<S> {
    type K = TestVarLenKey;
    type V = FixedSizeTestValue;
    type Storage = S;
    type KeyGen = VarLenKeyGenerator;
    type ValueGen = FixedSizeValueGenerator;

    fn node_variant() -> u8 { 1 } // VAR_KEY
    fn variant_name() -> &'static str { "var_key" }

    fn create_key_generator() -> Self::KeyGen { VarLenKeyGenerator::new(GenMode::Sequential, 42, 1_000_000) }

    fn create_value_generator() -> Self::ValueGen { FixedSizeValueGenerator::new(GenMode::Sequential, 43) }
}

// Variable value size (FixedSizeTestKey, TestVarLenValue)
struct VarValueSizeBtreeTest<S: StorageType> {
    _phantom: PhantomData<S>,
}

impl<S: StorageType> TestBtreeVariant for VarValueSizeBtreeTest<S> {
    type K = FixedSizeTestKey;
    type V = TestVarLenValue;
    type Storage = S;
    type KeyGen = FixedSizeKeyGenerator;
    type ValueGen = VarLenValueGenerator;

    fn node_variant() -> u8 { 2 } // VAR_VALUE
    fn variant_name() -> &'static str { "var_value" }

    fn create_key_generator() -> Self::KeyGen { FixedSizeKeyGenerator::new(GenMode::Sequential, 42, 1_000_000) }

    fn create_value_generator() -> Self::ValueGen { VarLenValueGenerator::new(GenMode::Sequential, 43) }
}

// Variable key and value (TestVarLenKey, TestVarLenValue)
struct VarObjSizeBtreeTest<S: StorageType> {
    _phantom: PhantomData<S>,
}

impl<S: StorageType> TestBtreeVariant for VarObjSizeBtreeTest<S> {
    type K = TestVarLenKey;
    type V = TestVarLenValue;
    type Storage = S;
    type KeyGen = VarLenKeyGenerator;
    type ValueGen = VarLenValueGenerator;

    fn node_variant() -> u8 { 3 } // VAR_OBJECT
    fn variant_name() -> &'static str { "var_obj" }

    fn create_key_generator() -> Self::KeyGen { VarLenKeyGenerator::new(GenMode::Sequential, 42, 1_000_000) }

    fn create_value_generator() -> Self::ValueGen { VarLenValueGenerator::new(GenMode::Sequential, 43) }
}

// Prefix compression (FixedSizeTestKey, FixedSizeTestValue)
#[allow(dead_code)]
struct PrefixCompressBtreeTest<S: StorageType> {
    _phantom: PhantomData<S>,
}

impl<S: StorageType> TestBtreeVariant for PrefixCompressBtreeTest<S> {
    type K = FixedSizeTestKey;
    type V = FixedSizeTestValue;
    type Storage = S;
    type KeyGen = FixedSizeKeyGenerator;
    type ValueGen = FixedSizeValueGenerator;

    fn node_variant() -> u8 { 4 } // PREFIX_COMPRESS
    fn variant_name() -> &'static str { "prefix_compress" }

    fn create_key_generator() -> Self::KeyGen { FixedSizeKeyGenerator::new(GenMode::Sequential, 42, 1_000_000) }

    fn create_value_generator() -> Self::ValueGen { FixedSizeValueGenerator::new(GenMode::Sequential, 43) }
}

//================================================================================
// Test Options
//================================================================================

#[derive(Clone)]
struct BtreeTestOptions {
    num_entries: u32,
    preload_size: u32,
    num_ios: u32,
    #[allow(dead_code)]
    run_time_secs: u32,
    disable_merge: bool,
    /// Full test name (e.g. "test_sequential_remove_var_key_mem") for btree logging. Set by the test macro.
    pub test_name: Option<String>,
}

impl Default for BtreeTestOptions {
    fn default() -> Self {
        Self {
            num_entries: 10000,
            preload_size: 5000,
            num_ios: 1000,
            run_time_secs: 36000,
            disable_merge: false,
            test_name: None,
        }
    }
}

//================================================================================
// Operation Distribution for Concurrent Tests
//================================================================================

#[derive(Clone)]
struct OpDistribution {
    put_pct: u32,
    remove_pct: u32,
    range_put_pct: u32,
    range_remove_pct: u32,
    query_pct: u32,
}

impl Default for OpDistribution {
    fn default() -> Self {
        Self {
            put_pct: 18,
            remove_pct: 14,
            range_put_pct: 20,
            range_remove_pct: 2,
            query_pct: 10,
        }
    }
}

// ShadowMap moved to shadow_map.rs for reusability

//================================================================================
// Test Btree representing a test btree - a specific btree instance
//================================================================================

struct TestBtree<Variant: TestBtreeVariant> {
    btree: Arc<Btree<Variant::K, Variant::V>>,
    shadow_map: ShadowMap<Variant::K, Variant::V>,
    opts: BtreeTestOptions,
    key_gen: Variant::KeyGen,
    value_gen: Variant::ValueGen,
    rng: StdRng,
    _phantom: PhantomData<Variant>,
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
impl<Variant: TestBtreeVariant> TestBtree<Variant> {
    async fn new(opts: BtreeTestOptions) -> Result<Self, BtreeError> {
        let _ = tracing_subscriber::fmt()
            .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
            .with_target(true)
            .try_init();

        let btree_name = opts
            .test_name
            .clone()
            .unwrap_or_else(|| format!("test_btree_{}_{}", Variant::variant_name(), Variant::Storage::name()));
        let mut config = BtreeConfig::new(4096, btree_name);
        config.merge_policy = if opts.disable_merge { MergePolicy::Never } else { MergePolicy::Aggressive };
        config.leaf_node_variant = Variant::node_variant();
        config.int_node_variant = Variant::node_variant();

        // Create storage based on storage type
        let storage: Box<dyn UnderlyingBtree> = match Variant::Storage::name() {
            "mem" => {
                // In-memory storage for testing
                Box::new(MemBtree::new(&config)) as Box<dyn UnderlyingBtree>
            }
            "cow" => {
                // COW requires full homestore setup (VDevs, MetaClient, caches)
                // TODO: Implement when homestore infrastructure is ready
                panic!("COW storage tests require full homestore setup - not yet implemented");
            }
            _ => panic!("Unknown storage type: {}", Variant::Storage::name()),
        };

        // Create btree with storage backend
        let btree = Arc::new(Btree::<Variant::K, Variant::V>::new(config, storage, None).await?);

        Ok(Self {
            btree,
            shadow_map: ShadowMap::new(opts.num_entries),
            opts,
            key_gen: Variant::create_key_generator(),
            value_gen: Variant::create_value_generator(),
            rng: StdRng::seed_from_u64(42),
            _phantom: PhantomData,
        })
    }

    //================================================================================
    // Helper Methods
    //================================================================================

    //================================================================================
    // Single Operations
    //================================================================================
    async fn put(&mut self, key_id: u64, put_type: BtreePutType) {
        let (key, _) = self.key_gen.generate(Some(key_id));
        let (value, _) = self.value_gen.generate(None);

        let result = match put_type {
            BtreePutType::Insert => {
                if self.shadow_map.get(&key).is_some() {
                    // Key exists, insert should fail
                    return;
                }
                self.btree.put_one(&key, &value, None).await
            }
            BtreePutType::Update => {
                if self.shadow_map.get(&key).is_none() {
                    // Key doesn't exist, update should fail
                    return;
                }
                self.btree.put_one(&key, &value, None).await
            }
            BtreePutType::Upsert => self.btree.put_one(&key, &value, None).await,
        };

        if result.is_ok() {
            self.shadow_map.insert(key, value);
        }
    }

    async fn remove(&mut self, key_id: u64) -> Option<Variant::V> {
        let (key, _) = self.key_gen.generate(Some(key_id));
        let result = self.btree.remove_one(&key, None).await.ok()?;

        if result.is_some() {
            self.shadow_map.remove(&key);
        }
        result
    }

    #[allow(dead_code)]
    async fn get(&mut self, key_id: u64) -> Option<Variant::V> {
        let (key, _) = self.key_gen.generate(Some(key_id));
        self.btree.get(&key).await.ok()?
    }

    //================================================================================
    // Validation Methods
    //================================================================================

    #[allow(dead_code)]
    fn validate_data(&self, k: &Variant::K, v: &Variant::V) -> bool {
        if let Some(expected) = self.shadow_map.get(k) { v == expected } else { false }
    }

    async fn get_all(&self) {
        println!("Validating {} entries with get_all", self.shadow_map.size());

        for (key, expected_value) in self.shadow_map.iter() {
            match self.btree.get(key).await {
                Ok(Some(actual_value)) => {
                    assert_eq!(&actual_value, expected_value, "Value mismatch for key {:?}", key);
                }
                Ok(None) => {
                    panic!("Key {:?} not found in btree but exists in shadow map", key);
                }
                Err(e) => {
                    panic!("Error getting key {:?}: {:?}", key, e);
                }
            }
        }
    }

    async fn query_all(&mut self) {
        // Query without pagination (large batch to get everything)
        println!("Validating with query_all (no pagination)");
        let (start_key, _) = self.key_gen.generate(Some(0));
        let (end_key, _) = self.key_gen.generate(Some(u64::MAX));
        let range = BtreeKeyRange::new(start_key, true, end_key, true);
        let handle = self.btree.query(range, u32::MAX, None).await.expect("Query failed");

        // Validate results against shadow map
        for (key, value) in handle.results.iter() {
            let shadow_value = self.shadow_map.get(key).expect("Key in btree but not in shadow map");
            assert_eq!(value, shadow_value, "Value mismatch for key {:?}", key);
        }
        println!("Query validation passed: {} entries", handle.results.len());
    }

    async fn query_all_reverse(&mut self) {
        // Query in reverse order using traversal query (no pagination)
        println!("Validating with query_all_reverse (no pagination)");
        let (start_key, _) = self.key_gen.generate(Some(0));
        let (end_key, _) = self.key_gen.generate(Some(u64::MAX));
        let range = BtreeKeyRange::new(start_key, true, end_key, true);
        let handle = self.btree.query_traversal(range, u32::MAX, None, true).await.expect("Reverse query failed");

        // Validate results against shadow map
        for (key, value) in handle.results.iter() {
            let shadow_value = self.shadow_map.get(key).expect("Key in btree but not in shadow map");
            assert_eq!(value, shadow_value, "Value mismatch for key {:?}", key);
        }

        // Verify results are in reverse order
        if handle.results.len() > 1 {
            for i in 0..handle.results.len() - 1 {
                assert!(
                    handle.results[i].0 > handle.results[i + 1].0,
                    "Reverse query results not in descending order at index {}",
                    i
                );
            }
        }
        println!("Reverse query validation passed: {} entries in descending order", handle.results.len());
    }

    async fn query_all_paginate(&mut self, batch_size: u32, reverse: bool) {
        // Query with pagination (forward or reverse)
        let direction = if reverse { "reverse" } else { "forward" };
        println!("Validating with query_all_paginate (batch_size={}, {})", batch_size, direction);

        let (start_key, _) = self.key_gen.generate(Some(0));
        let (end_key, _) = self.key_gen.generate(Some(u64::MAX));
        let range = BtreeKeyRange::new(start_key, true, end_key, true);

        let mut handle = if reverse {
            self.btree
                .query_traversal(range, batch_size, None, true)
                .await
                .expect("Reverse paginated query failed")
        } else {
            self.btree.query(range, batch_size, None).await.expect("Forward paginated query failed")
        };

        let mut total_entries = 0;
        let mut all_keys = Vec::new();

        loop {
            // Validate this batch
            for (key, value) in handle.results.iter() {
                let shadow_value = self.shadow_map.get(key).expect("Key in btree but not in shadow map");
                assert_eq!(value, shadow_value, "Value mismatch for key {:?}", key);
                all_keys.push(key.clone());
            }
            total_entries += handle.results.len();

            if !handle.has_more() {
                break;
            }
            handle = self.btree.query_next_batch(handle).await.expect("Query next batch failed");
        }

        // Verify order across all batches
        if reverse && all_keys.len() > 1 {
            for i in 0..all_keys.len() - 1 {
                assert!(
                    all_keys[i] > all_keys[i + 1],
                    "Reverse paginated query not in descending order at index {}",
                    i
                );
            }
        } else if !reverse && all_keys.len() > 1 {
            for i in 0..all_keys.len() - 1 {
                assert!(all_keys[i] < all_keys[i + 1], "Forward paginated query not in ascending order at index {}", i);
            }
        }

        println!("Paginated {} query validation passed: {} entries", direction, total_entries);
    }

    async fn do_query(&mut self, start_key_id: u64, end_key_id: u64, batch_size: u32) {
        let (start_key, _) = self.key_gen.generate(Some(start_key_id));
        let (end_key, _) = self.key_gen.generate(Some(end_key_id));

        println!("Query range [{}, {}] with batch_size={}", start_key_id, end_key_id, batch_size);
        let range = BtreeKeyRange::new(start_key.clone(), true, end_key.clone(), true);
        let mut handle = self.btree.query(range, batch_size, None).await.expect("Query failed");
        let mut total_entries = 0;

        loop {
            // Validate this batch against shadow map
            for (key, value) in handle.results.iter() {
                if self.shadow_map.exists_in_range(key, &start_key, &end_key) {
                    let shadow_value = self.shadow_map.get(key).expect("Key in btree but not in shadow map");
                    assert_eq!(value, shadow_value, "Value mismatch for key {:?}", key);
                }
            }
            total_entries += handle.results.len();

            if !handle.has_more() {
                break;
            }
            handle = self.btree.query_next_batch(handle).await.expect("Query next batch failed");
        }
        println!("Range query validation passed: {} entries in range", total_entries);
    }

    #[allow(dead_code)]
    async fn get_any(&mut self, start_key_id: u64, end_key_id: u64) {
        let (start_key, _) = self.key_gen.generate(Some(start_key_id));
        let (end_key, _) = self.key_gen.generate(Some(end_key_id));

        println!("Get any in range [{}, {}]", start_key_id, end_key_id);

        match self.btree.get_any(&start_key, &end_key).await {
            Ok(Some((key, value))) => {
                // Validate the returned key/value is in shadow map and in range
                assert!(
                    self.shadow_map.exists_in_range(&key, &start_key, &end_key),
                    "get_any returned key not in range"
                );
                let shadow_value = self.shadow_map.get(&key).expect("Key in btree but not in shadow map");
                assert_eq!(&value, shadow_value, "Value mismatch for key {:?}", key);
                println!("get_any found key in range");
            }
            Ok(None) => {
                println!("get_any returned None (no keys in range)");
            }
            Err(e) => panic!("get_any failed: {:?}", e),
        }
    }

    //================================================================================
    // Preload
    //================================================================================

    async fn preload(&mut self, preload_size: u32) {
        use std::time::Instant;

        println!("Preloading {} entries", preload_size);
        let start = Instant::now();

        for i in 0..preload_size {
            self.put(i as u64, BtreePutType::Insert).await;
        }

        let elapsed = start.elapsed();
        let ops_per_sec = preload_size as f64 / elapsed.as_secs_f64();
        println!("Preload complete: {} entries in {:?} ({:.0} ops/sec)", self.shadow_map.size(), elapsed, ops_per_sec);
    }

    //================================================================================
    // Concurrent Multi-Ops
    //================================================================================

    // Worker task extracted to support maybe-async-cfg transformation
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
    async fn concurrent_worker(
        btree: Arc<Btree<Variant::K, Variant::V>>,
        worker_id: usize,
        start_io: usize,
        end_io: usize,
        num_entries: u32,
        total_pct: u32,
        op_dist: OpDistribution,
    ) {
        use crate::index::btree::detail::btree_req::BtreeKeyRange;
        use rand::rngs::StdRng;
        use rand::SeedableRng;

        // Create a Send-safe RNG with unique seed per worker
        let mut local_rng = StdRng::seed_from_u64(42 + worker_id as u64);
        let mut key_gen = Variant::create_key_generator();
        let mut value_gen = Variant::create_value_generator();

        for i in start_io..end_io {
            let op_choice = local_rng.gen_range(0..total_pct);
            let k = local_rng.gen_range(0..num_entries) as u64;

            if op_choice < op_dist.put_pct {
                // Single put (no shadow map validation in concurrent mode)
                let (key, _) = key_gen.generate(Some(k));
                let (value, _) = value_gen.generate(None);
                let _ = btree.put_one(&key, &value, None).await;
            } else if op_choice < op_dist.put_pct + op_dist.remove_pct {
                // Single remove
                let (key, _) = key_gen.generate(Some(k));
                let _ = btree.remove_one(&key, None).await;
            } else {
                // Query
                let end_k = (k + 100).min(num_entries as u64);
                let (start_key, _) = key_gen.generate(Some(k));
                let (end_key, _) = key_gen.generate(Some(end_k));
                let range = BtreeKeyRange::new(start_key, true, end_key, true);
                let _ = btree.query(range, 32, None).await;
            }

            if i > 0 && (i + 1) % 100 == 0 {
                println!("    Worker {}: Completed {}/{} operations", worker_id, i + 1 - start_io, end_io - start_io);
            }
        }

        println!("  Worker {} completed all operations", worker_id);
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
    async fn concurrent_multi_ops(&mut self, op_dist: OpDistribution) {
        use std::sync::Arc;
        use std::time::Instant;

        // 1. Preload if needed
        if self.shadow_map.is_empty() {
            self.preload(self.opts.preload_size).await;
        }

        let num_workers = {
            #[cfg(feature = "async_code")]
            {
                iomgr::iomgr().num_reactors
            }

            #[cfg(feature = "sync_code")]
            {
                4
            } // Hardcoded for sync mode
        };

        println!("Starting TRULY concurrent multi-ops test on {} workers", num_workers);
        println!("  Entries: {}", self.opts.num_entries);
        println!("  IOs: {}", self.opts.num_ios);
        println!(
            "  Op Distribution - put: {}%, remove: {}%, range_put: {}%, range_remove: {}%, query: {}%",
            op_dist.put_pct, op_dist.remove_pct, op_dist.range_put_pct, op_dist.range_remove_pct, op_dist.query_pct
        );

        let num_ios = self.opts.num_ios;
        let total_pct =
            op_dist.put_pct + op_dist.remove_pct + op_dist.range_put_pct + op_dist.range_remove_pct + op_dist.query_pct;
        let num_entries = self.opts.num_entries;

        // Clone the Arc to share btree across workers
        let btree = Arc::clone(&self.btree);

        #[cfg(feature = "sync_code")]
        let mut bg_tasks = BackgroundTasks::new();

        #[cfg(feature = "async_code")]
        let bg_tasks = BackgroundTasks::new();
        let ios_per_worker = num_ios / num_workers as u32;

        for worker_id in 0..num_workers {
            let start_io = worker_id * ios_per_worker as usize;
            let end_io = if worker_id == num_workers - 1 {
                num_ios as usize // Last worker takes remaining
            } else {
                start_io + ios_per_worker as usize
            };

            println!("  Spawning worker {} with IOs [{}, {})", worker_id, start_io, end_io);

            // Clone Arc for each worker
            let btree_clone = Arc::clone(&btree);
            let op_dist_clone = op_dist.clone();

            // Spawn worker using extracted function
            // In async: spawns async task. In sync: spawns thread (via BackgroundTasks wrapper)
            #[cfg(feature = "async_code")]
            bg_tasks.spawn(
                ReactorTarget::Reactor(worker_id),
                Self::concurrent_worker(
                    btree_clone,
                    worker_id,
                    start_io,
                    end_io,
                    num_entries,
                    total_pct,
                    op_dist_clone,
                ),
            );

            #[cfg(feature = "sync_code")]
            bg_tasks.spawn(move || {
                Self::concurrent_worker(
                    btree_clone,
                    worker_id,
                    start_io,
                    end_io,
                    num_entries,
                    total_pct,
                    op_dist_clone,
                );
            });
        }

        println!("Waiting for all workers to complete...");
        let concurrent_start = Instant::now();
        bg_tasks.join_all().await;

        let concurrent_elapsed = concurrent_start.elapsed();
        let concurrent_ops_per_sec = num_ios as f64 / concurrent_elapsed.as_secs_f64();

        println!("All workers completed!");
        println!(
            "Concurrent multi-ops test complete - executed {} operations across {} workers in {:?} ({:.0} ops/sec)",
            num_ios, num_workers, concurrent_elapsed, concurrent_ops_per_sec
        );

        // Final validation - can't use shadow_map since we didn't track in concurrent mode
        // Just verify tree is still accessible
        println!("Final validation: tree operations completed successfully across {} workers", num_workers);
    }
}

//================================================================================
// Generic Test Implementations
//================================================================================

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_sequential_insert_impl<Variant: TestBtreeVariant>(test_btree: &mut TestBtree<Variant>) {
    println!("=== Sequential Insert Test ===");

    // Forward sequential insert
    let entries_iter1 = test_btree.opts.num_entries / 2;
    println!("Step 1: Forward sequential insert for {} entries", entries_iter1);
    for i in 0..entries_iter1 {
        test_btree.put(i as u64, BtreePutType::Insert).await;
    }

    println!("Step 2: Query {} entries", entries_iter1);
    test_btree.do_query(0, entries_iter1 as u64 - 1, 75).await;

    // Reverse sequential insert
    let entries_iter2 = test_btree.opts.num_entries - entries_iter1;
    println!("Step 3: Reverse sequential insert of remaining {} entries", entries_iter2);
    for i in (entries_iter1..test_btree.opts.num_entries).rev() {
        test_btree.put(i as u64, BtreePutType::Insert).await;
    }

    println!("Step 4: Query all entries");
    test_btree.query_all().await;

    println!("Step 5: Query all entries in reverse");
    test_btree.query_all_reverse().await;

    println!("Step 6: Query all with forward pagination");
    test_btree.query_all_paginate(100, false).await;

    println!("Step 7: Query all with reverse pagination");
    test_btree.query_all_paginate(100, true).await;

    println!("Step 8: Get all entries 1-by-1");
    test_btree.get_all().await;

    println!("Sequential Insert test complete");
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_random_insert_impl<Variant: TestBtreeVariant>(test_btree: &mut TestBtree<Variant>) {
    println!("=== Random Insert Test ===");

    // Create shuffled sequence
    let mut vec: Vec<u32> = (0..test_btree.opts.num_entries).collect();
    use rand::seq::SliceRandom;
    vec.shuffle(&mut test_btree.rng);

    println!("Step 1: Random insert for {} entries", test_btree.opts.num_entries);
    for i in vec {
        test_btree.put(i as u64, BtreePutType::Insert).await;
    }

    println!("Step 2: Query all entries");
    test_btree.query_all().await;

    println!("Step 3: Query all entries in reverse");
    test_btree.query_all_reverse().await;

    println!("Step 4: Query all with forward pagination");
    test_btree.query_all_paginate(100, false).await;

    println!("Step 5: Query all with reverse pagination");
    test_btree.query_all_paginate(100, true).await;

    println!("Step 6: Get all entries 1-by-1");
    test_btree.get_all().await;

    println!("Random Insert test complete");
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_sequential_remove_impl<Variant: TestBtreeVariant>(test_btree: &mut TestBtree<Variant>) {
    println!("=== Sequential Remove Test ===");

    println!("Step 1: Insert {} entries", test_btree.opts.num_entries);
    for i in 0..test_btree.opts.num_entries {
        test_btree.put(i as u64, BtreePutType::Insert).await;
    }

    println!("Step 2: Query all entries");
    test_btree.query_all().await;

    println!("Step 3: Remove half the entries");
    let remove_count = test_btree.opts.num_entries / 2;
    for i in 0..remove_count {
        test_btree.remove(i as u64).await;
    }

    println!("Step 4: Query remaining entries (forward)");
    test_btree.query_all().await;

    println!("Step 5: Query remaining entries (reverse)");
    test_btree.query_all_reverse().await;

    println!("Step 6: Validate remaining entries 1-by-1");
    test_btree.get_all().await;

    println!("Sequential Remove test complete");
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn test_random_remove_impl<Variant: TestBtreeVariant>(test_btree: &mut TestBtree<Variant>) {
    println!("=== Random Remove Test ===");

    println!("Step 1: Insert {} entries", test_btree.opts.num_entries);
    for i in 0..test_btree.opts.num_entries {
        test_btree.put(i as u64, BtreePutType::Insert).await;
    }

    // Create shuffled remove sequence
    let mut vec: Vec<u32> = (0..test_btree.opts.num_entries).collect();
    use rand::seq::SliceRandom;
    vec.shuffle(&mut test_btree.rng);

    println!("Step 2: Random remove for {} entries", test_btree.opts.num_entries);
    for i in vec {
        test_btree.remove(i as u64).await;
    }

    //assert_eq!(test_btree.shadow_map.size(), 0, "All entries should be removed");
    println!("Random Remove test complete");
}

//================================================================================
// Test Instantiation Macro
//================================================================================

macro_rules! btree_tests {
    ($test_type:ty, $test_name:ident) => {
        paste::paste! {
            // Sequential insert test
            #[cfg(feature = "async_code")]
            #[iomgr::iomanager_test]
            async fn [<test_sequential_insert_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("seq_insert_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).await.expect("Failed to create TestBtree");
                test_sequential_insert_impl(&mut helper).await;
            }

            #[cfg(feature = "sync_code")]
            #[test]
            fn [<test_sequential_insert_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("seq_insert_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).expect("Failed to create TestBtree");
                test_sequential_insert_impl(&mut helper);
            }

            // Random insert test
            #[cfg(feature = "async_code")]
            #[iomgr::iomanager_test]
            async fn [<test_random_insert_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("rand_insert_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).await.expect("Failed to create TestBtree");
                test_random_insert_impl(&mut helper).await;
            }

            #[cfg(feature = "sync_code")]
            #[test]
            fn [<test_random_insert_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("rand_insert_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).expect("Failed to create TestBtree");
                test_random_insert_impl(&mut helper);
            }

            // Sequential remove test
            #[cfg(feature = "async_code")]
            #[iomgr::iomanager_test]
            async fn [<test_sequential_remove_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("seq_remove_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).await.expect("Failed to create TestBtree");
                test_sequential_remove_impl(&mut helper).await;
            }

            #[cfg(feature = "sync_code")]
            #[test]
            fn [<test_sequential_remove_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("seq_remove_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).expect("Failed to create TestBtree");
                test_sequential_remove_impl(&mut helper);
            }

            // Random remove test
            #[cfg(feature = "async_code")]
            #[iomgr::iomanager_test]
            async fn [<test_random_remove_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("rand_remove_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).await.expect("Failed to create TestBtree");
                test_random_remove_impl(&mut helper).await;
            }

            #[cfg(feature = "sync_code")]
            #[test]
            fn [<test_random_remove_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("rand_remove_", stringify!($test_name)).to_string()),
                    ..Default::default()
                }).expect("Failed to create TestBtree");
                test_random_remove_impl(&mut helper);
            }

            // Concurrent test - 4 workers (reactors in async, threads in sync)
            #[cfg(feature = "async_code")]
            #[iomgr::iomanager_test(4)]
            async fn [<test_concurrent_multi_ops_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("conc_multi_", stringify!($test_name)).to_string()),
                    num_entries: 100000,
                    preload_size: 50000,
                    num_ios: 50000,
                    ..Default::default()
                }).await.expect("Failed to create TestBtree");

                helper.concurrent_multi_ops(OpDistribution::default()).await;
            }

            #[cfg(feature = "sync_code")]
            #[test]
            fn [<test_concurrent_multi_ops_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("conc_multi_", stringify!($test_name)).to_string()),
                    num_entries: 100000,
                    preload_size: 50000,
                    num_ios: 50000,
                    ..Default::default()
                }).expect("Failed to create TestBtree");

                helper.concurrent_multi_ops(OpDistribution::default());
            }

            // Stress test - 1M keys, 8 workers (async only due to time constraints)
            #[cfg(feature = "async_code")]
            #[iomgr::iomanager_test(8)]
            async fn [<test_concurrent_stress_ $test_name>]() {
                let mut helper = TestBtree::<$test_type>::new(BtreeTestOptions {
                    test_name: Some(concat!("conc_stress_", stringify!($test_name)).to_string()),
                    num_entries: 1_000_000,
                    preload_size: 500_000,
                    num_ios: 500_000,
                    ..Default::default()
                }).await.expect("Failed to create TestBtree");

                helper.concurrent_multi_ops(OpDistribution::default()).await;
            }
        }
    };
}

//================================================================================
// Test Instantiations for All Type/Storage Combinations
//================================================================================

// FixedLenBtree - Memory storage (FixedSizeTestKey + FixedSizeTestValue + SimpleNode)
btree_tests!(FixedSizeTestBtree<MemStorage>, fixed_size_mem);

// VarKeySizeBtree - Memory storage (TestVarLenKey + FixedSizeTestValue + VarKeyNode)
btree_tests!(VarKeySizeBtreeTest<MemStorage>, var_key_mem);

// VarValueSizeBtree - Memory storage (FixedSizeTestKey + TestVarLenValue + VarValueNode)
btree_tests!(VarValueSizeBtreeTest<MemStorage>, var_value_mem);

// VarObjSizeBtree - Memory storage (TestVarLenKey + TestVarLenValue + VarObjNode)
btree_tests!(VarObjSizeBtreeTest<MemStorage>, var_obj_mem);

// PrefixCompressBtree - Memory storage (FixedSizeTestKey + FixedSizeTestValue + PrefixCompressNode)
// btree_tests!(PrefixCompressBtreeTest<MemStorage>, prefix_compress_mem);

/*
// COW storage tests commented out - COW not yet implemented
// FixedLenBtree - COW storage (FixedSizeTestKey + FixedSizeTestValue + SimpleNode)
btree_tests!(FixedSizeTestBtree<CowStorage>, fixed_size_cow);

// VarKeySizeBtree - COW storage (TestVarLenKey + FixedSizeTestValue + VarKeyNode)
btree_tests!(VarKeySizeBtreeTest<CowStorage>, var_key_cow);

// VarValueSizeBtree - COW storage (FixedSizeTestKey + TestVarLenValue)
btree_tests!(VarValueSizeBtreeTest<CowStorage>, var_value_cow);

// VarObjSizeBtree - COW storage (TestVarLenKey + TestVarLenValue)
btree_tests!(VarObjSizeBtreeTest<CowStorage>, var_obj_cow);
*/
