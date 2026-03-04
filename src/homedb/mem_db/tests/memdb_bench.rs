//! Concurrent MemDB Performance Benchmark
//!
//! This benchmark measures raw concurrent MemDB performance with:
//! - Multiple workers (reactors in async, threads in sync) executing operations in parallel
//! - Random keys and values (no validation overhead)
//! - Mix of PUT and GET operations
//! - No assertions or validation overhead
//!
//! Run with (sync):
//!   cargo test --release --package mem_db --test memdb_bench --features sync_code -- \
//!     --workers 8 --ops 2000000 --key-range 1000000 --preload 100000 --put-pct 70
//!
//! Run with (async):
//!   cargo test --release --package mem_db --test memdb_bench --features async_code -- \
//!     --workers 8 --ops 2000000 --key-range 1000000 --preload 100000 --put-pct 70
//!
//! Run with multiple tables (like RocksDB Column Families):
//!   cargo test --release --package mem_db --test memdb_bench --features sync_code -- \
//!     --workers 16 --ops 4000000 --tables 8 --put-pct 30
//!
//! Debug small workload:
//!   cargo test --package mem_db --test memdb_bench --features sync_code -- \
//!     --workers 2 --ops 1000 --key-range 100 --preload 10

use mem_db::{MemoryDB, TableSpec};

use rand::{rngs::StdRng, Rng, SeedableRng};
use std::sync::Arc;
use std::time::Instant;
use clap::Parser;

#[derive(Debug, Clone, Parser)]
#[clap(about = "MemDB concurrent performance benchmark")]
struct BenchArgs {
    /// Number of workers (reactors in async, threads in sync)
    #[clap(short = 'w', long, default_value = "8")]
    workers: usize,

    /// Total number of operations to perform
    #[clap(short = 'o', long, default_value = "2000000")]
    ops: u64,

    /// Key range (number of unique keys)
    #[clap(short = 'r', long = "key-range", default_value = "1000000")]
    key_range: u64,

    /// Number of keys to preload before benchmark
    #[clap(short = 'p', long, default_value = "100000")]
    preload: u64,

    /// PUT percentage (0-100, remainder is GET)
    #[clap(long = "put-pct", default_value = "70")]
    put_pct: u32,

    /// Key size in bytes
    #[clap(short = 'k', long = "key-size", default_value = "32")]
    key_size: usize,

    /// Value size in bytes
    #[clap(short = 'v', long = "value-size", default_value = "128")]
    value_size: usize,

    /// Number of independent tables (like RocksDB Column Families)
    #[clap(short = 't', long = "tables", default_value = "1")]
    num_tables: usize,

    /// BTree node size in bytes (e.g. 4096, 8192, 16384).
    /// Larger nodes hold more entries per page; inline value threshold = node_size/32.
    #[clap(long = "node-size", default_value = "4096")]
    node_size: u32,

    /// Partition key size in bytes (0 = UnshardedBtree, >=1 = ShardedBtree).
    #[clap(long = "partition-key-size", default_value = "2")]
    partition_key_size: usize,

    /// Enable MVCC snapshot isolation on the table.
    #[clap(long = "use-mvcc", default_value = "false")]
    use_mvcc: bool,

    /// Use inline GC (only meaningful when --use-mvcc is set).
    /// When false (default), uses deferred background GC.
    #[clap(long = "inline-gc", default_value = "false")]
    inline_gc: bool,

    /// Preload insertion stride / gap (snake pattern).
    /// Sequential insertion leaves B-tree nodes ~50% full (every split abandons the left half).
    /// With stride=256: pass 0 inserts keys 0, 256, 512, ...; pass 1 inserts 769, 513, 257, 1
    /// (reversed); pass 2 inserts 2, 258, 514, 770 ... filling both sides of every node.
    /// Set to 1 to disable (pure sequential).
    #[clap(long = "preload-stride", default_value = "256")]
    preload_stride: u64,
}

//================================================================================
// Sync BackgroundTasks Wrapper (mimics async iomgr::BackgroundTasks API)
//================================================================================

#[cfg(feature = "sync_frontend")]
mod sync_tasks {
    use std::thread::JoinHandle;

    pub struct BackgroundTasks {
        handles: Vec<JoinHandle<()>>,
    }

    #[allow(dead_code)]
    pub struct ReactorTarget(pub usize);

    impl ReactorTarget {
        #[allow(non_snake_case)]
        pub fn Reactor(id: usize) -> Self { Self(id) }
    }

    impl BackgroundTasks {
        pub fn new() -> Self { Self { handles: Vec::new() } }

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

cfg_if::cfg_if! {
    if #[cfg(feature = "sync_frontend")] {
        use sync_tasks::{BackgroundTasks, ReactorTarget};
    } else if #[cfg(feature = "async_frontend")] {
        use iomgr::{BackgroundTasks, ReactorTarget};
    }
}

#[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
async fn run_benchmark(args: BenchArgs) {
    println!("=== MEMDB CONCURRENT PERFORMANCE BENCHMARK ===");
    cfg_if::cfg_if! {
        if #[cfg(feature = "async_frontend")] {
            println!("Mode: Async (reactors)");
        } else if #[cfg(feature = "sync_frontend")] {
            println!("Mode: Sync (threads)");
        }
    }

    println!("Configuration:");
    println!("  Workers: {}", args.workers);
    println!("  Tables: {}", args.num_tables);
    println!("  Total operations: {}", args.ops);
    println!("  Key range: {}", args.key_range);
    println!("  Key size: {} bytes", args.key_size);
    println!("  Value size: {} bytes", args.value_size);
    println!("  Preload per table: {}", args.preload / args.num_tables as u64);
    println!("  Operation mix: {}% PUT, {}% GET", args.put_pct, 100 - args.put_pct);
    let btree_mode = if args.partition_key_size == 0 { "concurrent (no sharding)" } else { "sharded" };
    println!("  BTree mode: {} (partition_key_size={})", btree_mode, args.partition_key_size);
    println!("  Node size: {} bytes (inline value cap: {} bytes)", args.node_size, args.node_size / 32);
    let mvcc_mode = if !args.use_mvcc { "disabled" } else if args.inline_gc { "inline GC" } else { "deferred GC" };
    println!("  MVCC: {}", mvcc_mode);
    let stride_desc = if args.preload_stride <= 1 { "sequential (stride=1)".to_string() } else { format!("snake (stride={})", args.preload_stride) };
    println!("  Preload order: {}", stride_desc);
    println!();

    let db = MemoryDB::new(args.workers).expect("Failed to create MemoryDB");

    let mut tables = Vec::new();

    println!("Creating {} tables...", args.num_tables);
    for i in 0..args.num_tables {
        let table_name = format!("benchmark_{}", i);
        let mut spec = TableSpec::fixed_kv(args.key_size, args.value_size)
            .partition_key_size(args.partition_key_size)
            .node_size(args.node_size);
        if args.use_mvcc { spec = spec.mvcc(); if args.inline_gc { spec = spec.inline_gc(); } }
        let table = db.create_table(&table_name, spec).await.unwrap();
        tables.push(table);
    }

    // Phase 1: Parallel preload — divide the flat key space [0, preload) across workers.
    // Global key k maps to: table_idx = k / preload_per_table, key_id = k.
    // Within each worker's slice the snake pattern is applied (stride sub-passes, alternating direction).
    let preload_per_table = args.preload / args.num_tables as u64;
    println!("Phase 1: Preloading {} keys per table ({} total) using {} parallel workers...",
        preload_per_table, args.preload, args.workers);
    let preload_start = Instant::now();

    {
        cfg_if::cfg_if! {
            if #[cfg(feature = "sync_frontend")] {
                let mut bg_pre = BackgroundTasks::new();
            } else if #[cfg(feature = "async_frontend")] {
                let bg_pre = BackgroundTasks::new();
            }
        }

        let total_keys = preload_per_table * args.num_tables as u64;
        let chunk = total_keys.div_ceil(args.workers as u64);
        let stride = args.preload_stride.max(1);
        let ppt = preload_per_table;

        for worker_id in 0..args.workers {
            let g_start = (worker_id as u64 * chunk).min(total_keys);
            let g_end = ((worker_id as u64 + 1) * chunk).min(total_keys);
            if g_start >= g_end { continue; }

            let tables_w: Vec<_> = tables.iter().map(Arc::clone).collect();
            let key_size = args.key_size;
            let value_size = args.value_size;

            #[cfg(feature = "async_frontend")]
            bg_pre.spawn(ReactorTarget::Reactor(worker_id), async move {
                let count = g_end - g_start;
                let cols = count.div_ceil(stride);
                for i in 0..count {
                    let pass = i % stride;
                    let step = i / stride;
                    let snake_step = if pass % 2 == 0 { step } else { cols.saturating_sub(1).saturating_sub(step) };
                    let offset = (pass + snake_step * stride).min(count - 1);
                    let key_id = g_start + offset;
                    let table_idx = (key_id / ppt) as usize;
                    let key = generate_key(key_id, key_size);
                    let value = generate_value(key_id, value_size);
                    let _ = tables_w[table_idx].put(key, value).await;
                }
            });

            #[cfg(feature = "sync_frontend")]
            bg_pre.spawn(ReactorTarget::Reactor(worker_id), move || {
                let count = g_end - g_start;
                let cols = count.div_ceil(stride);
                for i in 0..count {
                    let pass = i % stride;
                    let step = i / stride;
                    let snake_step = if pass % 2 == 0 { step } else { cols.saturating_sub(1).saturating_sub(step) };
                    let offset = (pass + snake_step * stride).min(count - 1);
                    let key_id = g_start + offset;
                    let table_idx = (key_id / ppt) as usize;
                    let key = generate_key(key_id, key_size);
                    let value = generate_value(key_id, value_size);
                    let _ = tables_w[table_idx].put(key, value);
                }
            });
        }

        bg_pre.join_all().await;
    }

    let preload_elapsed = preload_start.elapsed();
    let preload_ops_per_sec = args.preload as f64 / preload_elapsed.as_secs_f64();
    println!("  Preload: {} ops in {:?} ({:.0} ops/sec)", args.preload, preload_elapsed, preload_ops_per_sec);
    println!();

    // Phase 2: Concurrent operations across all workers
    println!("Phase 2: Running {} concurrent operations across {} workers ({} tables)...",
        args.ops, args.workers, args.num_tables);

    cfg_if::cfg_if! {
        if #[cfg(feature = "sync_frontend")] {
            let mut bg_tasks = BackgroundTasks::new();
        } else if #[cfg(feature = "async_frontend")] {
            let bg_tasks = BackgroundTasks::new();
        }
    }

    let ops_per_worker = args.ops / args.workers as u64;

    let concurrent_start = Instant::now();

    for worker_id in 0..args.workers {
        let table_idx = worker_id % args.num_tables;
        let table = Arc::clone(&tables[table_idx]);

        let start_op = worker_id as u64 * ops_per_worker;
        let end_op = if worker_id == args.workers - 1 {
            args.ops
        } else {
            start_op + ops_per_worker
        };
        let key_range = args.key_range;
        let put_pct = args.put_pct;
        let key_size = args.key_size;
        let value_size = args.value_size;

        #[cfg(feature = "async_frontend")]
        bg_tasks.spawn(ReactorTarget::Reactor(worker_id), async move {
            let mut rng = StdRng::seed_from_u64(42 + worker_id as u64);

            for _ in start_op..end_op {
                let key_id = rng.gen_range(0..key_range);
                let op_choice = rng.gen_range(0..100);

                if op_choice < put_pct {
                    let key = generate_key(key_id, key_size);
                    let value = generate_value(key_id, value_size);
                    let _ = table.put(key, value).await;
                } else {
                    let key = generate_key(key_id, key_size);
                    let _ = table.get(key).await;
                }
            }
        });

        #[cfg(feature = "sync_frontend")]
        bg_tasks.spawn(ReactorTarget::Reactor(worker_id), move || {
            let mut rng = StdRng::seed_from_u64(42 + worker_id as u64);

            for _ in start_op..end_op {
                let key_id = rng.gen_range(0..key_range);
                let op_choice = rng.gen_range(0..100);

                if op_choice < put_pct {
                    let key = generate_key(key_id, key_size);
                    let value = generate_value(key_id, value_size);
                    let _ = table.put(key, value);
                } else {
                    let key = generate_key(key_id, key_size);
                    let _ = table.get(key);
                }
            }
        });
    }

    bg_tasks.join_all().await;
    let concurrent_elapsed = concurrent_start.elapsed();
    let concurrent_ops_per_sec = args.ops as f64 / concurrent_elapsed.as_secs_f64();

    println!("  Concurrent: {} ops in {:?} ({:.0} ops/sec)",
        args.ops, concurrent_elapsed, concurrent_ops_per_sec
    );
    println!();

    println!("=== PERFORMANCE SUMMARY ===");
    println!("Preload (sequential):   {:.0} ops/sec", preload_ops_per_sec);
    println!("Concurrent ({} workers): {:.0} ops/sec", args.workers, concurrent_ops_per_sec);
    println!("Speedup: {:.2}x", concurrent_ops_per_sec / preload_ops_per_sec);
}

fn generate_key(id: u64, size: usize) -> Vec<u8> {
    let mut key = vec![0u8; size];
    key[0..8].copy_from_slice(&id.to_le_bytes());
    for i in 8..size.min(key.len()) {
        key[i] = ((id.wrapping_mul(31).wrapping_add(i as u64)) % 256) as u8;
    }
    key
}

fn generate_value(id: u64, size: usize) -> Vec<u8> {
    let mut value = vec![0u8; size];
    value[0..8].copy_from_slice(&id.to_le_bytes());
    for i in 8..size.min(value.len()) {
        value[i] = ((id.wrapping_mul(37).wrapping_add(i as u64)) % 256) as u8;
    }
    value
}

fn main() {
    let args = BenchArgs::parse();

    cfg_if::cfg_if! {
        if #[cfg(feature = "async_frontend")] {
            tokio::runtime::Runtime::new().unwrap().block_on(run_benchmark(args));
        } else if #[cfg(feature = "sync_frontend")] {
            run_benchmark(args);
        }
    }
}
