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

use bytes::Bytes;
use crate::logstore::{LogStore, LogStoreError, MemLogStore};

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_basic_append_and_read() {
    let store = MemLogStore::new(1, 0);
    
    // Append some entries
    let data1 = Bytes::from("entry1");
    let data2 = Bytes::from("entry2");
    let data3 = Bytes::from("entry3");
    
    let seq1 = store.append(data1.clone()).await.unwrap();
    let seq2 = store.append(data2.clone()).await.unwrap();
    let seq3 = store.append(data3.clone()).await.unwrap();
    
    assert_eq!(seq1, 0);
    assert_eq!(seq2, 1);
    assert_eq!(seq3, 2);
    
    // Read entries
    let read1 = store.read(seq1).await.unwrap();
    let read2 = store.read(seq2).await.unwrap();
    let read3 = store.read(seq3).await.unwrap();
    
    assert_eq!(read1, data1);
    assert_eq!(read2, data2);
    assert_eq!(read3, data3);
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_read_not_found() {
    let store = MemLogStore::new(1, 0);
    
    // Try to read non-existent entry
    let result = store.read(0).await;
    assert!(matches!(result, Err(LogStoreError::NotFound(0))));
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_truncate() {
    let store = MemLogStore::new(1, 0);
    
    // Append entries
    for i in 0..10 {
        let data = Bytes::from(format!("entry{}", i));
        store.append(data).await.unwrap();
    }
    
    // Verify initial bounds
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 0);
    assert_eq!(next, 10);
    
    // Truncate first 5 entries (0-4 inclusive)
    store.truncate(4).await.unwrap();
    
    // Verify new bounds
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 5);
    assert_eq!(next, 10);
    
    // Try to read truncated entry
    let result = store.read(0).await;
    assert!(matches!(result, Err(LogStoreError::Truncated(0))));
    
    let result = store.read(4).await;
    assert!(matches!(result, Err(LogStoreError::Truncated(4))));
    
    // Read remaining entries
    for i in 5..10 {
        let data = store.read(i).await.unwrap();
        assert_eq!(data, Bytes::from(format!("entry{}", i)));
    }
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_rollback() {
    let store = MemLogStore::new(1, 0);
    
    // Append entries
    for i in 0..10 {
        let data = Bytes::from(format!("entry{}", i));
        store.append(data).await.unwrap();
    }
    
    // Verify initial bounds
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 0);
    assert_eq!(next, 10);
    
    // Rollback to seq 5 (keep 0-5, discard 6-9)
    store.rollback(5).await.unwrap();
    
    // Verify new bounds
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 0);
    assert_eq!(next, 6);
    
    // Read remaining entries
    for i in 0..6 {
        let data = store.read(i).await.unwrap();
        assert_eq!(data, Bytes::from(format!("entry{}", i)));
    }
    
    // Try to read rolled back entry
    let result = store.read(6).await;
    assert!(matches!(result, Err(LogStoreError::NotFound(6))));
    
    // Append new entry - should get seq 6
    let new_data = Bytes::from("new_entry");
    let seq = store.append(new_data.clone()).await.unwrap();
    assert_eq!(seq, 6);
    
    let read_data = store.read(seq).await.unwrap();
    assert_eq!(read_data, new_data);
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_rollback_invalid() {
    let store = MemLogStore::new(1, 0);
    
    // Append entries
    for i in 0..10 {
        let data = Bytes::from(format!("entry{}", i));
        store.append(data).await.unwrap();
    }
    
    // Try to rollback to invalid seq (>= next_seq - 1)
    let result = store.rollback(9).await;
    assert!(matches!(result, Err(LogStoreError::InvalidSeq(9))));
    
    let result = store.rollback(10).await;
    assert!(matches!(result, Err(LogStoreError::InvalidSeq(10))));
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_iterator() {
    let store = MemLogStore::new(1, 0);
    
    // Append entries
    for i in 0..10 {
        let data = Bytes::from(format!("entry{}", i));
        store.append(data).await.unwrap();
    }
    
    // Iterate from beginning
    let iter = store.iter(0).await;
    let entries: Vec<_> = iter.collect();
    assert_eq!(entries.len(), 10);
    
    for (i, (seq, data)) in entries.iter().enumerate() {
        assert_eq!(*seq, i as u64);
        assert_eq!(*data, Bytes::from(format!("entry{}", i)));
    }
    
    // Iterate from middle
    let iter = store.iter(5).await;
    let entries: Vec<_> = iter.collect();
    assert_eq!(entries.len(), 5);
    
    for (i, (seq, data)) in entries.iter().enumerate() {
        let expected_seq = (i + 5) as u64;
        assert_eq!(*seq, expected_seq);
        assert_eq!(*data, Bytes::from(format!("entry{}", expected_seq)));
    }
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_iterator_with_truncate() {
    let store = MemLogStore::new(1, 0);
    
    // Append entries
    for i in 0..10 {
        let data = Bytes::from(format!("entry{}", i));
        store.append(data).await.unwrap();
    }
    
    // Create iterator (snapshot)
    let iter = store.iter(0).await;
    
    // Truncate some entries
    store.truncate(4).await.unwrap();
    
    // Iterator should still have all 10 entries (snapshot)
    let entries: Vec<_> = iter.collect();
    assert_eq!(entries.len(), 10);
}

#[cfg(feature = "async_code")]
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn test_concurrent_append() {
    use std::sync::Arc;
    
    let store = Arc::new(MemLogStore::new(1, 0));
    let num_writers = 8;
    let entries_per_writer = 100;
    
    let mut handles = vec![];
    
    for writer_id in 0..num_writers {
        let store_clone = Arc::clone(&store);
        let handle = tokio::spawn(async move {
            let mut seqs = vec![];
            for i in 0..entries_per_writer {
                let data = Bytes::from(format!("writer{}_entry{}", writer_id, i));
                let seq = store_clone.append(data).await.unwrap();
                seqs.push(seq);
            }
            seqs
        });
        handles.push(handle);
    }
    
    // Wait for all writers
    let mut all_seqs = vec![];
    for handle in handles {
        let seqs = handle.await.unwrap();
        all_seqs.extend(seqs);
    }
    
    // Verify all sequences are unique and in valid range
    all_seqs.sort();
    assert_eq!(all_seqs.len(), num_writers * entries_per_writer);
    
    for (i, seq) in all_seqs.iter().enumerate() {
        assert_eq!(*seq, i as u64);
    }
    
    // Verify bounds
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 0);
    assert_eq!(next, (num_writers * entries_per_writer) as u64);
}

#[cfg(feature = "async_code")]
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn test_concurrent_read_write() {
    use std::sync::Arc;
    
    let store = Arc::new(MemLogStore::new(1, 0));
    
    // Pre-populate with some entries
    for i in 0..100 {
        let data = Bytes::from(format!("entry{}", i));
        store.append(data).await.unwrap();
    }
    
    let mut handles = vec![];
    
    // Spawn writers
    for writer_id in 0..4 {
        let store_clone = Arc::clone(&store);
        let handle = tokio::spawn(async move {
            for i in 0..50 {
                let data = Bytes::from(format!("new_writer{}_entry{}", writer_id, i));
                let _ = store_clone.append(data).await;
            }
        });
        handles.push(handle);
    }
    
    // Spawn readers
    for reader_id in 0..4 {
        let store_clone = Arc::clone(&store);
        let handle = tokio::spawn(async move {
            for _ in 0..100 {
                let seq = (reader_id * 10) as u64;
                if let Ok(data) = store_clone.read(seq).await {
                    assert!(data.len() > 0);
                }
            }
        });
        handles.push(handle);
    }
    
    // Wait for all tasks
    for handle in handles {
        handle.await.unwrap();
    }
    
    // Verify final state
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 0);
    assert_eq!(next, 100 + 4 * 50); // Initial + writers
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_is_append_mode() {
    let store = MemLogStore::new(1, 0);
    assert!(store.is_append_mode());
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_store_id() {
    let store = MemLogStore::new(42, 0);
    assert_eq!(store.store_id(), 42);
}

#[cfg(feature = "async_code")]
#[tokio::test]
async fn test_custom_start_seq() {
    let store = MemLogStore::new(1, 100);
    
    let data = Bytes::from("entry1");
    let seq = store.append(data).await.unwrap();
    
    assert_eq!(seq, 100);
    
    let (start, next) = store.seq_bounds();
    assert_eq!(start, 100);
    assert_eq!(next, 101);
}
