//! Benchmark suite comparing MemDB with RocksDB PlainTable
//!
//! Run with: cargo bench --bench bench_memdb_vs_rocksdb
//!
//! This benchmark compares:
//! 1. Point lookup latency (p50, p99, p999)
//! 2. Range scan throughput
//! 3. Multi-threaded scaling
//! 4. Mixed workload performance (80% read, 20% write)
//! 5. Variable-length key/value performance

use criterion::{black_box, criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use mem_db::{init_mem_homedb, mem_homedb, TableSpec};
use rand::{Rng, SeedableRng};
use rand::rngs::StdRng;
use std::sync::Arc;
use std::time::Duration;

// Configuration constants
const NUM_KEYS: usize = 1_000_000;
const KEY_SIZE: usize = 8;
const VALUE_SIZE: usize = 64;
const NUM_THREADS: &[usize] = &[1, 2, 4, 8, 16];
const RANGE_SIZES: &[usize] = &[10, 100, 1000, 10000];

/// Setup MemDB with pre-populated data
async fn setup_memdb(num_keys: usize) -> Result<(), Box<dyn std::error::Error>> {
    init_mem_homedb(4)?;
    let db = mem_homedb();
    
    let spec = TableSpec::fixed_kv(KEY_SIZE, VALUE_SIZE);
    let table = db.create_table("bench_table", spec).await?;
    
    // Pre-populate with sequential keys
    for i in 0..num_keys {
        let key = i.to_be_bytes().to_vec();
        let value = vec![i as u8; VALUE_SIZE];
        table.put(key, value).await?;
    }
    
    Ok(())
}

/// Benchmark 1: Point Lookup Latency (Single-threaded Baseline)
/// Tests: How fast can we retrieve a single key?
/// Measures: p50, p99, p999 latencies
/// Expected: MemDB 1.2-1.5x faster (modest advantage)
fn bench_point_lookup(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    rt.block_on(async {
        setup_memdb(NUM_KEYS).await.unwrap();
    });
    
    let mut group = c.benchmark_group("point_lookup");
    group.throughput(Throughput::Elements(1));
    group.measurement_time(Duration::from_secs(10));
    
    group.bench_function("memdb_existing_key", |b| {
        let db = mem_homedb();
        let table = rt.block_on(async {
            db.get_table("bench_table").unwrap()
        });
        
        let mut rng = StdRng::seed_from_u64(42);
        
        b.iter(|| {
            let key_idx = rng.gen_range(0..NUM_KEYS);
            let key = key_idx.to_be_bytes().to_vec();
            
            rt.block_on(async {
                let result = table.get(black_box(key)).await.unwrap();
                assert!(result.is_some());
                black_box(result)
            });
        });
    });
    
    group.bench_function("memdb_non_existent_key", |b| {
        let db = mem_homedb();
        let table = rt.block_on(async {
            db.get_table("bench_table").unwrap()
        });
        
        b.iter(|| {
            let key = (NUM_KEYS + 1000).to_be_bytes().to_vec();
            
            rt.block_on(async {
                let result = table.get(black_box(key)).await.unwrap();
                assert!(result.is_none());
                black_box(result)
            });
        });
    });
    
    // TODO: Add RocksDB PlainTable comparison here
    // group.bench_function("rocksdb_existing_key", |b| { ... });
    
    group.finish();
}

/// Benchmark 2: Range Scan Throughput
/// Tests: How many keys can we scan per second?
/// Varies: Range sizes (10, 100, 1000, 10000 keys)
fn bench_range_scan(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    rt.block_on(async {
        setup_memdb(NUM_KEYS).await.unwrap();
    });
    
    let mut group = c.benchmark_group("range_scan");
    
    for range_size in RANGE_SIZES {
        group.throughput(Throughput::Elements(*range_size as u64));
        
        group.bench_with_input(
            BenchmarkId::new("memdb", range_size),
            range_size,
            |b, &size| {
                let db = mem_homedb();
                let table = rt.block_on(async {
                    db.get_table("bench_table").unwrap()
                });
                
                b.iter(|| {
                    rt.block_on(async {
                        let start_key = 0usize.to_be_bytes().to_vec();
                        let end_key = size.to_be_bytes().to_vec();
                        
                        let mut iter = table
                            .get_range(start_key, end_key, 100)
                            .await
                            .unwrap();
                        
                        let mut count = 0;
                        while let Some(_kv) = iter.next().await.unwrap() {
                            count += 1;
                        }
                        
                        black_box(count)
                    });
                });
            },
        );
        
        // TODO: Add RocksDB comparison
        // group.bench_with_input(BenchmarkId::new("rocksdb", range_size), ...);
    }
    
    group.finish();
}

/// Benchmark 3: Multi-threaded Scaling
/// Tests: How well does performance scale with more threads?
/// Varies: Thread counts (1, 2, 4, 8, 16)
fn bench_multi_threaded(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    rt.block_on(async {
        setup_memdb(NUM_KEYS).await.unwrap();
    });
    
    let mut group = c.benchmark_group("multi_threaded");
    
    for num_threads in NUM_THREADS {
        group.throughput(Throughput::Elements(1000));
        
        group.bench_with_input(
            BenchmarkId::new("memdb_read_only", num_threads),
            num_threads,
            |b, &threads| {
                let db = mem_homedb();
                let table = Arc::new(rt.block_on(async {
                    db.get_table("bench_table").unwrap()
                }));
                
                b.iter(|| {
                    let handles: Vec<_> = (0..threads)
                        .map(|thread_id| {
                            let table = Arc::clone(&table);
                            rt.spawn(async move {
                                let mut rng = StdRng::seed_from_u64(thread_id as u64);
                                
                                for _ in 0..(1000 / threads) {
                                    let key_idx = rng.gen_range(0..NUM_KEYS);
                                    let key = key_idx.to_be_bytes().to_vec();
                                    let _ = table.get(key).await;
                                }
                            })
                        })
                        .collect();
                    
                    rt.block_on(async {
                        for handle in handles {
                            handle.await.unwrap();
                        }
                    });
                });
            },
        );
    }
    
    group.finish();
}

/// Benchmark 4: Mixed Workload (80% reads, 20% writes)
/// Tests: Real-world workload performance
fn bench_mixed_workload(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    rt.block_on(async {
        setup_memdb(NUM_KEYS).await.unwrap();
    });
    
    let mut group = c.benchmark_group("mixed_workload");
    group.throughput(Throughput::Elements(100));
    
    group.bench_function("memdb_80read_20write", |b| {
        let db = mem_homedb();
        let table = rt.block_on(async {
            db.get_table("bench_table").unwrap()
        });
        
        let mut rng = StdRng::seed_from_u64(42);
        
        b.iter(|| {
            rt.block_on(async {
                for _ in 0..100 {
                    let is_write = rng.gen_bool(0.2);
                    let key_idx = rng.gen_range(0..NUM_KEYS);
                    let key = key_idx.to_be_bytes().to_vec();
                    
                    if is_write {
                        let value = vec![rng.gen::<u8>(); VALUE_SIZE];
                        table.put(key, value).await.unwrap();
                    } else {
                        let _ = table.get(key).await.unwrap();
                    }
                }
            });
        });
    });
    
    group.finish();
}

/// Benchmark 5: Variable-length Keys/Values
/// Tests: Performance advantage of VarObj node variant
fn bench_variable_length(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    // Setup table with variable-length keys/values
    rt.block_on(async {
        init_mem_homedb(4).ok(); // Ignore if already initialized
        let db = mem_homedb();
        
        use mem_db::{KeySpec, ValueSpec};
        let spec = TableSpec::new(
            KeySpec::variable(64),
            ValueSpec::variable(256),
        );
        
        let table = db.create_table("bench_varlen", spec).await.unwrap();
        
        // Populate with variable-length data
        let mut rng = StdRng::seed_from_u64(123);
        for i in 0..NUM_KEYS {
            let key_len = rng.gen_range(8..64);
            let val_len = rng.gen_range(64..256);
            
            let mut key = vec![0u8; key_len];
            key[0..8].copy_from_slice(&i.to_be_bytes());
            
            let value = vec![i as u8; val_len];
            table.put(key, value).await.unwrap();
        }
    });
    
    let mut group = c.benchmark_group("variable_length");
    group.throughput(Throughput::Elements(1));
    
    group.bench_function("memdb_varlen_get", |b| {
        let db = mem_homedb();
        let table = rt.block_on(async {
            db.get_table("bench_varlen").unwrap()
        });
        
        let mut rng = StdRng::seed_from_u64(456);
        
        b.iter(|| {
            let key_idx = rng.gen_range(0..NUM_KEYS);
            let key_len = rng.gen_range(8..64);
            let mut key = vec![0u8; key_len];
            key[0..8].copy_from_slice(&key_idx.to_be_bytes());
            
            rt.block_on(async {
                let result = table.get(black_box(key)).await.unwrap();
                black_box(result)
            });
        });
    });
    
    // TODO: Compare with RocksDB PlainTable variable-length performance
    
    group.finish();
}

criterion_group!(
    benches,
    bench_point_lookup,
    bench_range_scan,
    bench_multi_threaded,
    bench_mixed_workload,
    bench_variable_length
);

criterion_main!(benches);
