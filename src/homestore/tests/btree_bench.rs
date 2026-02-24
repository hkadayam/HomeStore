//! Concurrent Btree Performance Benchmark
//!
//! This benchmark measures raw concurrent btree performance with:
//! - Multiple workers (reactors in async, threads in sync) executing operations in parallel
//! - Random keys and values (no validation overhead)
//! - Mix of PUT and GET operations
//! - No assertions or shadow map tracking
//!
//! Run with (sync):
//!   cargo test --release --package homestore --test btree_bench --no-default-features --features sync_code,inmem,btree-only -- 8 2000000 1000000 100000 70 32 128 1
//!
//! Run with (async):
//!   cargo test --release --package homestore --test btree_bench --features async_code,inmem,btree-only -- 8 2000000 1000000 100000 70 32 128 1
//!
//! Run with multiple btrees (like RocksDB Column Families):
//!   cargo test --release --package homestore --test btree_bench --no-default-features --features sync_code,inmem,btree-only -- 16 4000000 1000000 100000 3 32 128 8
//!
//! Parameters (positional):
//!   <num-workers>     Number of workers (default: 8)
//!   <total-ops>       Total operations (default: 2000000)
//!   <key-range>       Key range (default: 1000000)
//!   <preload>         Preload size (default: 100000)
//!   <put-pct>         PUT percentage 0-100 (default: 70)
//!   <key-size>        Key size in bytes (default: 32)
//!   <value-size>      Value size in bytes (default: 128)
//!   <num-btrees>      Number of independent btrees (default: 1)

use homestore::index::btree::{
    btree::{Btree, UnderlyingBtree},
    BtreeConfig,  // Re-exported from btree_types at btree module level
    btree_kvs::{BtreeKey, BtreeValue},
    underlying::mem::MemBtree,
};
use rand::{rngs::StdRng, Rng, SeedableRng};
use std::sync::Arc;
use std::time::Instant;

#[derive(Debug, Clone)]
struct BenchArgs {
    num_workers: usize,
    total_ops: u64,
    key_range: u64,
    preload: u64,
    put_pct: u32,
    key_size: usize,
    value_size: usize,
    num_btrees: usize,
}

impl BenchArgs {
    fn parse() -> Self {
        let args: Vec<String> = std::env::args().collect();
        
        Self {
            num_workers: args.get(1).and_then(|s| s.parse().ok()).unwrap_or(8),
            total_ops: args.get(2).and_then(|s| s.parse().ok()).unwrap_or(2_000_000),
            key_range: args.get(3).and_then(|s| s.parse().ok()).unwrap_or(1_000_000),
            preload: args.get(4).and_then(|s| s.parse().ok()).unwrap_or(100_000),
            put_pct: args.get(5).and_then(|s| s.parse().ok()).unwrap_or(70),
            key_size: args.get(6).and_then(|s| s.parse().ok()).unwrap_or(32),
            value_size: args.get(7).and_then(|s| s.parse().ok()).unwrap_or(128),
            num_btrees: args.get(8).and_then(|s| s.parse().ok()).unwrap_or(1),
        }
    }
}

//================================================================================
// Sync BackgroundTasks Wrapper (mimics async iomgr::BackgroundTasks API)
//================================================================================

#[cfg(feature = "sync_code")]
mod sync_tasks {
    use std::thread::JoinHandle;
    
    pub struct BackgroundTasks {
        handles: Vec<JoinHandle<()>>,
    }
    
    pub struct ReactorTarget(pub usize);
    
    impl ReactorTarget {
        pub fn Reactor(id: usize) -> Self {
            Self(id)
        }
    }
    
    impl BackgroundTasks {
        pub fn new() -> Self {
            Self { handles: Vec::new() }
        }
        
        pub fn spawn<F>(&mut self, _target: ReactorTarget, f: F)
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
use sync_tasks::{BackgroundTasks, ReactorTarget};

#[cfg(feature = "async_code")]
use iomgr::{init_iomgr, iomgr, BackgroundTasks, ReactorTarget};

// Fixed-size key type with configurable size for realistic benchmarking
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
struct BenchKey([u8; 32]);

impl BenchKey {
    fn new(id: u64, _size: usize) -> Self {
        let mut data = [0u8; 32];
        // Store the ID in the first 8 bytes for uniqueness and ordering
        data[0..8].copy_from_slice(&id.to_le_bytes());
        // Fill the rest with a pattern based on the ID for variety
        for i in 8..32 {
            data[i] = ((id.wrapping_mul(31).wrapping_add(i as u64)) % 256) as u8;
        }
        BenchKey(data)
    }
}

impl std::fmt::Debug for BenchKey {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let id = u64::from_le_bytes(self.0[0..8].try_into().unwrap());
        write!(f, "K({}, {}B)", id, self.0.len())
    }
}

impl BtreeKey for BenchKey {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(32);
    
    fn serialized_size(&self) -> u32 { 32 }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> std::io::Result<u32> {
        buf[0..32].copy_from_slice(&self.0);
        Ok(32)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> std::io::Result<Self> {
        let mut data = [0u8; 32];
        data.copy_from_slice(&buf[0..32]);
        Ok(BenchKey(data))
    }
    
    fn get_max_size() -> u32 { 32 }
}

// Fixed-size value type with configurable size for realistic benchmarking
#[derive(Clone)]
struct BenchValue([u8; 128]);

impl BenchValue {
    fn new(id: u64, _size: usize) -> Self {
        let mut data = [0u8; 128];
        // Store the ID in the first 8 bytes
        data[0..8].copy_from_slice(&id.to_le_bytes());
        // Fill the rest with a pattern based on the ID
        for i in 8..128 {
            data[i] = ((id.wrapping_mul(37).wrapping_add(i as u64)) % 256) as u8;
        }
        BenchValue(data)
    }
}

impl std::fmt::Debug for BenchValue {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let id = u64::from_le_bytes(self.0[0..8].try_into().unwrap());
        write!(f, "V({}, {}B)", id, self.0.len())
    }
}

impl BtreeValue for BenchValue {
    const FIXED_SERIALIZED_SIZE: Option<u32> = Some(128);
    
    fn serialized_size(&self) -> u32 { 128 }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> std::io::Result<u32> {
        buf[0..128].copy_from_slice(&self.0);
        Ok(128)
    }
    
    fn deserialize_from(buf: &[u8], _copy: bool) -> std::io::Result<Self> {
        let mut data = [0u8; 128];
        data.copy_from_slice(&buf[0..128]);
        Ok(BenchValue(data))
    }
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_code"), async(feature = "async_code"))]
async fn run_benchmark(args: BenchArgs) {
    println!("=== BTREE CONCURRENT PERFORMANCE BENCHMARK ===");
    #[cfg(feature = "async_code")]
    println!("Mode: Async (reactors)");
    #[cfg(feature = "sync_code")]
    println!("Mode: Sync (threads)");
    println!("Configuration:");
    println!("  Workers: {}", args.num_workers);
    println!("  Btrees: {}", args.num_btrees);
    println!("  Total operations: {}", args.total_ops);
    println!("  Key range: {}", args.key_range);
    println!("  Key size: {} bytes", args.key_size);
    println!("  Value size: {} bytes", args.value_size);
    println!("  Preload per btree: {}", args.preload / args.num_btrees as u64);
    println!("  Operation mix: {}% PUT, {}% GET", args.put_pct, 100 - args.put_pct);
    println!();

    // Initialize iomanager with N reactors (async only)
    #[cfg(feature = "async_code")]
    init_iomgr(args.num_workers).expect("Failed to initialize iomanager");

    // Create multiple btrees (like RocksDB Column Families)
    let node_size = 16384;  // 16KB nodes (typical for modern databases)
    let mut btrees = Vec::new();
    
    println!("Creating {} btrees...", args.num_btrees);
    for i in 0..args.num_btrees {
        let mut config = BtreeConfig::new(node_size, format!("benchmark_{}", i));
        config.leaf_node_variant = 0; // SimpleNode for raw performance
        config.int_node_variant = 0;

        let storage: Box<dyn UnderlyingBtree> = Box::new(MemBtree::new(&config));
        let btree = Arc::new(
            Btree::<BenchKey, BenchValue>::new(config, storage, None)
                .await
                .unwrap(),
        );
        btrees.push(btree);
    }

    // Phase 1: Preload each btree
    let preload_per_btree = args.preload / args.num_btrees as u64;
    println!("Phase 1: Preloading {} keys per btree ({} total)...", preload_per_btree, args.preload);
    let preload_start = Instant::now();
    
    for (btree_idx, btree) in btrees.iter().enumerate() {
        let start_key = btree_idx as u64 * preload_per_btree;
        for i in 0..preload_per_btree {
            let key_id = start_key + i;
            let key = BenchKey::new(key_id, args.key_size);
            let value = BenchValue::new(key_id, args.value_size);
            btree.put_one(&key, &value, None).await.unwrap();
        }
    }
    
    let preload_elapsed = preload_start.elapsed();
    let preload_ops_per_sec = args.preload as f64 / preload_elapsed.as_secs_f64();
    println!("  Preload: {} ops in {:?} ({:.0} ops/sec)", args.preload, preload_elapsed, preload_ops_per_sec);
    println!();

    // Phase 2: Concurrent operations across all workers
    println!("Phase 2: Running {} concurrent operations across {} workers ({} btrees)...", 
             args.total_ops, args.num_workers, args.num_btrees);
    
    #[cfg(feature = "sync_code")]
    let mut bg_tasks = BackgroundTasks::new();
    
    #[cfg(feature = "async_code")]
    let bg_tasks = BackgroundTasks::new();
    
    let ops_per_worker = args.total_ops / args.num_workers as u64;

    let concurrent_start = Instant::now();

    for worker_id in 0..args.num_workers {
        // Each worker is assigned to a specific btree (round-robin)
        // This simulates RocksDB's Column Family isolation
        let btree_idx = worker_id % args.num_btrees;
        let btree = Arc::clone(&btrees[btree_idx]);
        
        let start_op = worker_id as u64 * ops_per_worker;
        let end_op = if worker_id == args.num_workers - 1 {
            args.total_ops // Last worker takes remaining
        } else {
            start_op + ops_per_worker
        };
        let key_range = args.key_range;
        let put_pct = args.put_pct;
        let key_size = args.key_size;
        let value_size = args.value_size;

        #[cfg(feature = "async_code")]
        bg_tasks.spawn(ReactorTarget::Reactor(worker_id), async move {
            let mut rng = StdRng::seed_from_u64(42 + worker_id as u64);

            for _ in start_op..end_op {
                let key_id = rng.gen_range(0..key_range);
                let op_choice = rng.gen_range(0..100);

                if op_choice < put_pct {
                    // PUT operation
                    let key = BenchKey::new(key_id, key_size);
                    let value = BenchValue::new(key_id, value_size);
                    let _ = btree.put_one(&key, &value, None).await;
                } else {
                    // GET operation
                    let key = BenchKey::new(key_id, key_size);
                    let _ = btree.get(&key).await;
                }
            }
        });
        
        #[cfg(feature = "sync_code")]
        bg_tasks.spawn(ReactorTarget::Reactor(worker_id), move || {
            let mut rng = StdRng::seed_from_u64(42 + worker_id as u64);

            for _ in start_op..end_op {
                let key_id = rng.gen_range(0..key_range);
                let op_choice = rng.gen_range(0..100);

                if op_choice < put_pct {
                    // PUT operation
                    let key = BenchKey::new(key_id, key_size);
                    let value = BenchValue::new(key_id, value_size);
                    let _ = btree.put_one(&key, &value, None);
                } else {
                    // GET operation
                    let key = BenchKey::new(key_id, key_size);
                    let _ = btree.get(&key);
                }
            }
        });
    }

    bg_tasks.join_all().await;
    let concurrent_elapsed = concurrent_start.elapsed();
    let concurrent_ops_per_sec = args.total_ops as f64 / concurrent_elapsed.as_secs_f64();

    println!("  Concurrent: {} ops in {:?} ({:.0} ops/sec)", args.total_ops, concurrent_elapsed, concurrent_ops_per_sec);
    println!();

    // Summary
    println!("=== PERFORMANCE SUMMARY ===");
    println!("Preload (sequential):   {:.0} ops/sec", preload_ops_per_sec);
    println!("Concurrent ({} workers): {:.0} ops/sec", args.num_workers, concurrent_ops_per_sec);
    println!("Speedup: {:.2}x", concurrent_ops_per_sec / preload_ops_per_sec);
    println!();

    // Shutdown (async only)
    #[cfg(feature = "async_code")]
    {
        let _ = iomgr::shutdown_iomgr().await;
    }
}

fn main() {
    let args = BenchArgs::parse();
    
    #[cfg(feature = "async_code")]
    {
        tokio::runtime::Runtime::new()
            .unwrap()
            .block_on(run_benchmark(args));
    }
    
    #[cfg(feature = "sync_code")]
    {
        run_benchmark(args);
    }
}
