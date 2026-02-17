//! Concurrent Btree Performance Benchmark
//!
//! This benchmark measures raw concurrent btree performance with:
//! - Multiple reactors executing operations in parallel
//! - Random keys and values (no validation overhead)
//! - Mix of PUT and GET operations
//! - No assertions or shadow map tracking
//!
//! Run with:
//!   cargo run --release --example btree_concurrent_bench --no-default-features --features btree-only
//!
//! Profile with perf:
//!   cargo build --release --example btree_concurrent_bench --no-default-features --features btree-only
//!   perf record -F 999 -g target/release/examples/btree_concurrent_bench
//!   perf report
//!
//! Profile with flamegraph:
//!   cargo install flamegraph
//!   flamegraph target/release/examples/btree_concurrent_bench

use homestore::index::btree::{
    btree::Btree,
    BtreeConfig,  // Re-exported from btree_types at btree module level
    btree_kvs::{BtreeKey, BtreeValue},
    underlying::mem::MemBtree,
};
use iomgr::{init_iomgr, iomgr, BackgroundTasks, ReactorTarget};
use rand::{rngs::StdRng, Rng, SeedableRng};
use std::sync::Arc;
use std::time::Instant;

// Simple key type for benchmarking
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct BenchKey(u64);

impl std::fmt::Debug for BenchKey {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "K({})", self.0)
    }
}

impl BtreeKey for BenchKey {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);
    
    fn serialized_size(&self) -> u32 { 8 }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> std::io::Result<u32> {
        buf[0..8].copy_from_slice(&self.0.to_le_bytes());
        Ok(8)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> std::io::Result<Self> {
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&buf[0..8]);
        Ok(BenchKey(u64::from_le_bytes(bytes)))
    }
    
    fn get_max_size() -> u32 { 8 }
}

// Simple value type for benchmarking
#[derive(Clone, Copy)]
struct BenchValue(u64);

impl std::fmt::Debug for BenchValue {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "V({})", self.0)
    }
}

impl BtreeValue for BenchValue {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(8);
    
    fn serialized_size(&self) -> u32 { 8 }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> std::io::Result<u32> {
        buf[0..8].copy_from_slice(&self.0.to_le_bytes());
        Ok(8)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> std::io::Result<Self> {
        let mut bytes = [0u8; 8];
        bytes.copy_from_slice(&buf[0..8]);
        Ok(BenchValue(u64::from_le_bytes(bytes)))
    }
}

const NUM_REACTORS: usize = 8;
const TOTAL_OPS: u64 = 2_000_000; // 2M total operations
const KEY_RANGE: u64 = 1_000_000;  // 1M unique keys
const PRELOAD_SIZE: u64 = 100_000; // Preload 100K keys
const PUT_PERCENTAGE: u32 = 70;    // 70% puts, 30% gets

#[tokio::main]
async fn main() {
    println!("=== BTREE CONCURRENT PERFORMANCE BENCHMARK ===");
    println!("Configuration:");
    println!("  Reactors: {}", NUM_REACTORS);
    println!("  Total operations: {}", TOTAL_OPS);
    println!("  Key range: {}", KEY_RANGE);
    println!("  Preload: {}", PRELOAD_SIZE);
    println!("  Operation mix: {}% PUT, {}% GET", PUT_PERCENTAGE, 100 - PUT_PERCENTAGE);
    println!();

    // Initialize iomanager with N reactors
    init_iomgr(NUM_REACTORS).expect("Failed to initialize iomanager");

    // Create btree
    let mut config = BtreeConfig::new(4096, "benchmark".to_string());
    config.leaf_node_variant = 0; // SimpleNode for raw performance
    config.int_node_variant = 0;

    let storage = Box::new(MemBtree::new(config.node_size));
    let btree = Arc::new(
        Btree::<BenchKey, BenchValue>::new(config, storage, None)
            .await
            .unwrap(),
    );

    // Phase 1: Preload
    println!("Phase 1: Preloading {} keys...", PRELOAD_SIZE);
    let preload_start = Instant::now();
    for i in 0..PRELOAD_SIZE {
        let key = BenchKey(i);
        let value = BenchValue(i);
        btree.put_one(&key, &value, None).await.unwrap();
    }
    let preload_elapsed = preload_start.elapsed();
    let preload_ops_per_sec = PRELOAD_SIZE as f64 / preload_elapsed.as_secs_f64();
    println!("  Preload: {} ops in {:?} ({:.0} ops/sec)", PRELOAD_SIZE, preload_elapsed, preload_ops_per_sec);
    println!();

    // Phase 2: Concurrent operations across all reactors
    println!("Phase 2: Running {} concurrent operations across {} reactors...", TOTAL_OPS, NUM_REACTORS);
    let bg_tasks = BackgroundTasks::new();
    let ops_per_reactor = TOTAL_OPS / NUM_REACTORS as u64;

    let concurrent_start = Instant::now();

    for reactor_id in 0..NUM_REACTORS {
        let btree = Arc::clone(&btree);
        let start_op = reactor_id as u64 * ops_per_reactor;
        let end_op = if reactor_id == NUM_REACTORS - 1 {
            TOTAL_OPS // Last reactor takes remaining
        } else {
            start_op + ops_per_reactor
        };

        bg_tasks.spawn(ReactorTarget::Reactor(reactor_id), async move {
            let mut rng = StdRng::seed_from_u64(42 + reactor_id as u64);

            for _ in start_op..end_op {
                let key_id = rng.gen_range(0..KEY_RANGE);
                let op_choice = rng.gen_range(0..100);

                if op_choice < PUT_PERCENTAGE {
                    // PUT operation
                    let key = BenchKey(key_id);
                    let value = BenchValue(key_id);
                    let _ = btree.put_one(&key, &value, None).await;
                } else {
                    // GET operation
                    let key = BenchKey(key_id);
                    let _ = btree.get(&key).await;
                }
            }
        });
    }

    bg_tasks.join_all().await;
    let concurrent_elapsed = concurrent_start.elapsed();
    let concurrent_ops_per_sec = TOTAL_OPS as f64 / concurrent_elapsed.as_secs_f64();

    println!("  Concurrent: {} ops in {:?} ({:.0} ops/sec)", TOTAL_OPS, concurrent_elapsed, concurrent_ops_per_sec);
    println!();

    // Summary
    println!("=== PERFORMANCE SUMMARY ===");
    println!("Preload (sequential):   {:.0} ops/sec", preload_ops_per_sec);
    println!("Concurrent ({} reactors): {:.0} ops/sec", NUM_REACTORS, concurrent_ops_per_sec);
    println!("Speedup: {:.2}x", concurrent_ops_per_sec / preload_ops_per_sec);
    println!();

    // Shutdown
    let _ = iomgr::shutdown_iomgr().await;
}
