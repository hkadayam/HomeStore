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

use std::sync::Arc;
use std::time::{Duration, Instant};
use bytes::Bytes;
use homestore::logstore::{LogStore, MemLogStore};

#[cfg(feature = "async_code")]
use tokio;

/// Benchmark single-threaded append performance
#[cfg(feature = "async_code")]
async fn bench_single_append(num_entries: usize) -> Duration {
    let store = MemLogStore::new(1, 0);
    let data = Bytes::from(vec![0u8; 64]);
    
    let start = Instant::now();
    for _ in 0..num_entries {
        let _ = store.append(data.clone()).await.unwrap();
    }
    start.elapsed()
}

/// Benchmark concurrent append with multiple writers
#[cfg(feature = "async_code")]
async fn bench_concurrent_append(num_writers: usize, entries_per_writer: usize) -> Duration {
    let store = Arc::new(MemLogStore::new(1, 0));
    let data = Bytes::from(vec![0u8; 64]);
    
    let start = Instant::now();
    
    let mut handles = vec![];
    for _ in 0..num_writers {
        let store_clone = Arc::clone(&store);
        let data_clone = data.clone();
        let handle = tokio::spawn(async move {
            for _ in 0..entries_per_writer {
                let _ = store_clone.append(data_clone.clone()).await.unwrap();
            }
        });
        handles.push(handle);
    }
    
    for handle in handles {
        handle.await.unwrap();
    }
    
    start.elapsed()
}

/// Benchmark read performance
#[cfg(feature = "async_code")]
async fn bench_read(num_entries: usize, num_reads: usize) -> Duration {
    let store = MemLogStore::new(1, 0);
    let data = Bytes::from(vec![0u8; 64]);
    
    // Pre-populate
    for _ in 0..num_entries {
        let _ = store.append(data.clone()).await.unwrap();
    }
    
    let start = Instant::now();
    for i in 0..num_reads {
        let seq = (i % num_entries) as u64;
        let _ = store.read(seq).await.unwrap();
    }
    start.elapsed()
}

#[cfg(feature = "async_code")]
#[tokio::main]
async fn main() {
    println!("=== MemLogStore Benchmarks ===\n");
    
    // Single-threaded append
    println!("Single-threaded append:");
    for &num_entries in &[10_000, 100_000, 1_000_000] {
        let elapsed = bench_single_append(num_entries).await;
        let ops_per_sec = num_entries as f64 / elapsed.as_secs_f64();
        let ns_per_op = elapsed.as_nanos() / num_entries as u128;
        println!("  {} entries: {:?} ({:.0} ops/sec, {} ns/op)", 
                 num_entries, elapsed, ops_per_sec, ns_per_op);
    }
    
    println!("\nConcurrent append (100K entries total):");
    for &num_writers in &[1, 2, 4, 8, 16, 32] {
        let entries_per_writer = 100_000 / num_writers;
        let elapsed = bench_concurrent_append(num_writers, entries_per_writer).await;
        let total_entries = num_writers * entries_per_writer;
        let ops_per_sec = total_entries as f64 / elapsed.as_secs_f64();
        let ns_per_op = elapsed.as_nanos() / total_entries as u128;
        println!("  {} writers: {:?} ({:.0} ops/sec, {} ns/op)", 
                 num_writers, elapsed, ops_per_sec, ns_per_op);
    }
    
    println!("\nRead performance:");
    for &num_entries in &[1_000, 10_000, 100_000] {
        let num_reads = 100_000;
        let elapsed = bench_read(num_entries, num_reads).await;
        let ops_per_sec = num_reads as f64 / elapsed.as_secs_f64();
        let ns_per_op = elapsed.as_nanos() / num_reads as u128;
        println!("  {} entries, {} reads: {:?} ({:.0} ops/sec, {} ns/op)", 
                 num_entries, num_reads, elapsed, ops_per_sec, ns_per_op);
    }
    
    println!("\nTruncate performance:");
    let store = MemLogStore::new(1, 0);
    let data = Bytes::from(vec![0u8; 64]);
    for _ in 0..100_000 {
        let _ = store.append(data.clone()).await.unwrap();
    }
    let start = Instant::now();
    let _ = store.truncate(49_999).await.unwrap();
    let elapsed = start.elapsed();
    println!("  Truncate 50K entries: {:?}", elapsed);
    
    println!("\nRollback performance:");
    let store = MemLogStore::new(1, 0);
    for _ in 0..100_000 {
        let _ = store.append(data.clone()).await.unwrap();
    }
    let start = Instant::now();
    let _ = store.rollback(49_999).await.unwrap();
    let elapsed = start.elapsed();
    println!("  Rollback 50K entries: {:?}", elapsed);
}

#[cfg(not(feature = "async_code"))]
fn main() {
    println!("Benchmarks require async_code feature");
}
