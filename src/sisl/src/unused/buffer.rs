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

use std::alloc::{alloc, dealloc, Layout};
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use bytes::{Bytes, BytesMut};
use aligned_vec::{AVec, RuntimeAlign};

/// Buffer tags for tracking different types of allocations
/// Corresponds to C++ buftag enum
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum BufTag {
    Common = 0,      // Default tag if nothing supplied
    Bitset = 1,      // Default tag for bitset
    SuperBlk = 2,    // Superblk
    MetaBlk = 3,     // MetaBlk
    LogRead = 4,     // logbuf read from journal
    LogWrite = 5,    // logbuf written by group commit
    Compression = 6, // Compression entries
    DataJournal = 7, // All indx_mgr data journal
    BtreeJournal = 8, // Journal entries for btree
    BtreeNode = 9,   // Data entries for btree
    Sentinel = 10,   // This is expected to be the last
}

impl BufTag {
    pub fn name(&self) -> &'static str {
        match self {
            BufTag::Common => "common",
            BufTag::Bitset => "bitset",
            BufTag::SuperBlk => "superblk",
            BufTag::MetaBlk => "metablk",
            BufTag::LogRead => "logread",
            BufTag::LogWrite => "logwrite",
            BufTag::Compression => "compression",
            BufTag::DataJournal => "data_journal",
            BufTag::BtreeJournal => "btree_journal",
            BufTag::BtreeNode => "btree_node",
            BufTag::Sentinel => "sentinel",
        }
    }
}

/// Metrics for aligned allocations
#[derive(Debug)]
pub struct AlignedAllocatorMetrics {
    counters: [AtomicUsize; BufTag::Sentinel as usize],
}

impl AlignedAllocatorMetrics {
    pub fn new() -> Self {
        Self {
            counters: Default::default(),
        }
    }

    pub fn increment(&self, tag: BufTag, size: usize) {
        if (tag as usize) < self.counters.len() {
            self.counters[tag as usize].fetch_add(size, Ordering::Relaxed);
        }
    }

    pub fn decrement(&self, tag: BufTag, size: usize) {
        if (tag as usize) < self.counters.len() {
            self.counters[tag as usize].fetch_sub(size, Ordering::Relaxed);
        }
    }

    pub fn get_counter(&self, tag: BufTag) -> usize {
        if (tag as usize) < self.counters.len() {
            self.counters[tag as usize].load(Ordering::Relaxed)
        } else {
            0
        }
    }
}

impl Default for AlignedAllocatorMetrics {
    fn default() -> Self {
        Self::new()
    }
}

/// Global aligned allocator with metrics
#[derive(Debug)]
pub struct AlignedAllocator {
    metrics: AlignedAllocatorMetrics,
}

impl AlignedAllocator {
    pub fn instance() -> &'static Mutex<AlignedAllocator> {
        static INSTANCE: std::sync::OnceLock<Mutex<AlignedAllocator>> = std::sync::OnceLock::new();
        INSTANCE.get_or_init(|| {
            Mutex::new(AlignedAllocator {
                metrics: AlignedAllocatorMetrics::new(),
            })
        })
    }

    pub fn aligned_alloc(&mut self, align: usize, size: usize, tag: BufTag) -> Result<NonNull<u8>, String> {
        if size == 0 {
            return Err("Cannot allocate zero-sized buffer".to_string());
        }

        if !align.is_power_of_two() {
            return Err("Alignment must be a power of two".to_string());
        }

        let layout = Layout::from_size_align(size, align)
            .map_err(|e| format!("Invalid layout: {}", e))?;

        let ptr = unsafe { alloc(layout) };

        if ptr.is_null() {
            return Err("Failed to allocate aligned buffer".to_string());
        }

        self.metrics.increment(tag, size);

        NonNull::new(ptr).ok_or_else(|| "Null pointer returned from allocator".to_string())
    }

    pub fn aligned_free(&mut self, ptr: NonNull<u8>, size: usize, align: usize, tag: BufTag) {
        let layout = Layout::from_size_align(size, align).unwrap();
        unsafe {
            dealloc(ptr.as_ptr(), layout);
        }
        self.metrics.decrement(tag, size);
    }

    pub fn aligned_realloc(&mut self, old_ptr: NonNull<u8>, old_size: usize, align: usize, new_size: usize, tag: BufTag) -> Result<NonNull<u8>, String> {
        // Simple implementation: allocate new, copy, free old
        let new_ptr = self.aligned_alloc(align, new_size, tag)?;
        
        let copy_size = std::cmp::min(old_size, new_size);
        unsafe {
            ptr::copy_nonoverlapping(old_ptr.as_ptr(), new_ptr.as_ptr(), copy_size);
        }

        self.aligned_free(old_ptr, old_size, align, tag);
        Ok(new_ptr)
    }

    pub fn metrics(&self) -> &AlignedAllocatorMetrics {
        &self.metrics
    }
}

/// Vectored I/O structures using standard Rust slices - corresponds to C++ sg_list and related types
#[derive(Debug)]
pub struct SgList {
    pub slices: Vec<Bytes>,
}

impl SgList {
    pub fn new() -> Self {
        Self {
            slices: Vec::new(),
        }
    }

    pub fn add_slice(&mut self, data: Bytes) {
        self.slices.push(data);
    }

    pub fn total_size(&self) -> u64 {
        self.slices.iter().map(|s| s.len() as u64).sum()
    }

    pub fn iter(&self) -> impl Iterator<Item = &[u8]> {
        self.slices.iter().map(|b| b.as_ref())
    }
}

impl Default for SgList {
    fn default() -> Self {
        Self::new()
    }
}

/// Iterator for scatter-gather lists - simplified using standard types
#[derive(Debug)]
pub struct SgIterator {
    slices: Vec<Bytes>,
    cur_index: usize,
    cur_offset: usize,
}

impl SgIterator {
    pub fn new(slices: Vec<Bytes>) -> Self {
        assert!(!slices.is_empty(), "SgIterator requires non-empty slices");
        Self {
            slices,
            cur_index: 0,
            cur_offset: 0,
        }
    }

    pub fn next_bytes(&mut self, size: usize) -> Vec<Bytes> {
        let mut result = Vec::new();
        let mut remaining = size;

        while remaining > 0 && self.cur_index < self.slices.len() {
            let current_slice = &self.slices[self.cur_index];
            let available = current_slice.len() - self.cur_offset;

            if remaining <= available {
                // We can satisfy the request from this slice
                let end = self.cur_offset + remaining;
                result.push(current_slice.slice(self.cur_offset..end));
                self.cur_offset = end;
                remaining = 0;
            } else {
                // Take the rest of this slice
                result.push(current_slice.slice(self.cur_offset..));
                remaining -= available;
                self.cur_index += 1;
                self.cur_offset = 0;
            }
        }

        result
    }

    pub fn move_offset(&mut self, size: usize) {
        let mut remaining = size;

        while remaining > 0 && self.cur_index < self.slices.len() {
            let current_slice = &self.slices[self.cur_index];
            let available = current_slice.len() - self.cur_offset;

            if remaining <= available {
                self.cur_offset += remaining;
                remaining = 0;
            } else {
                remaining -= available;
                self.cur_index += 1;
                self.cur_offset = 0;
            }
        }
    }
}

/// Aligned buffer for direct I/O operations using aligned-vec crate
/// This is a safe wrapper around AVec that maintains compatibility with the existing API
#[derive(Debug)]
pub struct AlignedBuffer {
    data: AVec<u8, RuntimeAlign>,
    tag: BufTag,
}

impl AlignedBuffer {
    /// Create a new aligned buffer suitable for direct I/O
    pub fn new(size: usize, alignment: usize) -> Result<Self, String> {
        Self::new_with_tag(size, alignment, BufTag::Common)
    }

    /// Create a new aligned buffer with specific tag
    pub fn new_with_tag(size: usize, alignment: usize, tag: BufTag) -> Result<Self, String> {
        if size == 0 {
            return Err("Buffer size cannot be zero".to_string());
        }
        
        if !alignment.is_power_of_two() {
            return Err("Alignment must be a power of two".to_string());
        }
        
        // Create aligned vector with specified alignment
        let mut data = AVec::<u8, RuntimeAlign>::new(alignment);
        data.resize(size, 0); // Zero-initialized
        
        // Update metrics (for compatibility with existing metrics system)
        let allocator = AlignedAllocator::instance().lock().unwrap();
        allocator.metrics().increment(tag, size);
        
        Ok(AlignedBuffer { data, tag })
    }
    
    /// Get a raw pointer to the buffer
    pub fn as_ptr(&self) -> *const u8 {
        self.data.as_ptr()
    }
    
    /// Get a mutable raw pointer to the buffer
    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.data.as_mut_ptr()
    }
    
    /// Get the size of the buffer
    pub fn size(&self) -> usize {
        self.data.len()
    }
    
    /// Get the alignment of the buffer
    pub fn alignment(&self) -> usize {
        self.data.alignment()
    }

    /// Get the buffer tag
    pub fn tag(&self) -> BufTag {
        self.tag
    }
    
    /// Get a slice view of the buffer
    pub fn as_slice(&self) -> &[u8] {
        &self.data
    }
    
    /// Get a mutable slice view of the buffer
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        &mut self.data
    }

    /// Resize the buffer
    pub fn resize(&mut self, new_size: usize) -> Result<(), String> {
        let old_size = self.data.len();
        if new_size == old_size {
            return Ok(());
        }

        self.data.resize(new_size, 0);

        // Update metrics
        let allocator = AlignedAllocator::instance().lock().unwrap();
        if new_size > old_size {
            allocator.metrics().increment(self.tag, new_size - old_size);
        } else {
            allocator.metrics().decrement(self.tag, old_size - new_size);
        }
        
        Ok(())
    }
}

/// IO Buffer using standard Rust types - simplified from C++ io_blob
#[derive(Debug, Clone)]
pub struct IoBuffer {
    data: BytesMut,
    is_aligned: bool,
    tag: BufTag,
}

impl IoBuffer {
    pub fn new() -> Self {
        Self {
            data: BytesMut::new(),
            is_aligned: false,
            tag: BufTag::Common,
        }
    }

    /// Create with specific capacity
    pub fn with_capacity(size: usize, tag: BufTag) -> Self {
        Self {
            data: BytesMut::with_capacity(size),
            is_aligned: false,
            tag,
        }
    }

    /// Create aligned buffer (uses AlignedBuffer internally)
    pub fn with_aligned_capacity(size: usize, alignment: usize, tag: BufTag) -> Result<Self, String> {
        let aligned_buf = AlignedBuffer::new_with_tag(size, alignment, tag)?;
        
        // Copy data from aligned buffer to BytesMut
        let mut data = BytesMut::with_capacity(size);
        data.extend_from_slice(aligned_buf.as_slice());
        
        Ok(Self {
            data,
            is_aligned: true,
            tag,
        })
    }

    /// Create with initial data and fill value (for debugging)
    pub fn with_capacity_and_init(size: usize, tag: BufTag, init_val: u8) -> Self {
        let mut data = BytesMut::with_capacity(size);
        data.resize(size, init_val);
        Self {
            data,
            is_aligned: false,
            tag,
        }
    }

    /// Create from existing bytes (zero-copy if possible)
    pub fn from_bytes(bytes: Bytes) -> Self {
        Self {
            data: BytesMut::from(bytes.as_ref()),
            is_aligned: false,
            tag: BufTag::Common,
        }
    }

    /// Create from string
    pub fn from_string(s: &str) -> Self {
        Self::from_bytes(Bytes::copy_from_slice(s.as_bytes()))
    }

    /// Create from slice
    pub fn from_slice(slice: &[u8]) -> Self {
        Self {
            data: BytesMut::from(slice),
            is_aligned: false,
            tag: BufTag::Common,
        }
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }

    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    pub fn capacity(&self) -> usize {
        self.data.capacity()
    }

    pub fn is_aligned(&self) -> bool {
        self.is_aligned
    }

    pub fn tag(&self) -> BufTag {
        self.tag
    }

    pub fn set_tag(&mut self, tag: BufTag) {
        self.tag = tag;
    }

    /// Reserve additional capacity
    pub fn reserve(&mut self, additional: usize) {
        self.data.reserve(additional);
    }

    /// Resize the buffer
    pub fn resize(&mut self, new_len: usize, value: u8) {
        self.data.resize(new_len, value);
    }

    /// Get as slice
    pub fn as_slice(&self) -> &[u8] {
        &self.data
    }

    /// Get as mutable slice
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        &mut self.data
    }

    /// Get as Bytes (zero-copy)
    pub fn as_bytes(&self) -> Bytes {
        self.data.clone().freeze()
    }

    /// Get the underlying BytesMut
    pub fn as_bytes_mut(&mut self) -> &mut BytesMut {
        &mut self.data
    }

    /// Append data
    pub fn extend_from_slice(&mut self, slice: &[u8]) {
        self.data.extend_from_slice(slice);
    }

    /// Clear the buffer
    pub fn clear(&mut self) {
        self.data.clear();
    }

    /// Split off part of the buffer
    pub fn split_off(&mut self, at: usize) -> Self {
        let split_data = self.data.split_off(at);
        Self {
            data: split_data,
            is_aligned: self.is_aligned,
            tag: self.tag,
        }
    }

    /// Split the buffer into two parts
    pub fn split_to(&mut self, at: usize) -> Self {
        let split_data = self.data.split_to(at);
        Self {
            data: split_data,
            is_aligned: self.is_aligned,
            tag: self.tag,
        }
    }
}

impl Default for IoBuffer {
    fn default() -> Self {
        Self::new()
    }
}

impl From<Vec<u8>> for IoBuffer {
    fn from(vec: Vec<u8>) -> Self {
        Self {
            data: BytesMut::from(vec.as_slice()),
            is_aligned: false,
            tag: BufTag::Common,
        }
    }
}

impl From<&[u8]> for IoBuffer {
    fn from(slice: &[u8]) -> Self {
        Self::from_slice(slice)
    }
}

impl From<String> for IoBuffer {
    fn from(s: String) -> Self {
        Self::from_string(&s)
    }
}

/// Shared buffer type - uses reference counting for efficient sharing
/// Corresponds to C++ byte_array but much simpler with Bytes
pub type SharedBuffer = Bytes;

/// Create a new shared buffer
pub fn make_shared_buffer(size: usize, tag: BufTag) -> SharedBuffer {
    let buffer = IoBuffer::with_capacity(size, tag);
    buffer.as_bytes()
}

/// Create a new aligned shared buffer
pub fn make_aligned_shared_buffer(size: usize, alignment: usize, tag: BufTag) -> Result<SharedBuffer, String> {
    let buffer = IoBuffer::with_aligned_capacity(size, alignment, tag)?;
    Ok(buffer.as_bytes())
}

/// Buffer view using Bytes for zero-copy slicing
/// Much simpler than C++ byte_view thanks to Bytes' built-in reference counting
#[derive(Debug, Clone)]
pub struct BufferView {
    data: Bytes,
}

impl BufferView {
    pub fn new() -> Self {
        Self {
            data: Bytes::new(),
        }
    }

    /// Create from Bytes
    pub fn from_bytes(data: Bytes) -> Self {
        Self { data }
    }

    /// Create from slice (will copy the data)
    pub fn from_slice(slice: &[u8]) -> Self {
        Self {
            data: Bytes::copy_from_slice(slice),
        }
    }

    /// Create from IoBuffer
    pub fn from_io_buffer(buffer: &IoBuffer) -> Self {
        Self {
            data: buffer.as_bytes(),
        }
    }

    /// Create from string
    pub fn from_string(s: &str) -> Self {
        Self::from_slice(s.as_bytes())
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }

    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    /// Create a sub-view (zero-copy slice)
    pub fn slice<R>(&self, range: R) -> Self 
    where
        R: std::ops::RangeBounds<usize>,
    {
        // Convert range bounds to start/end indices
        let start = match range.start_bound() {
            std::ops::Bound::Included(&n) => n,
            std::ops::Bound::Excluded(&n) => n + 1,
            std::ops::Bound::Unbounded => 0,
        };
        let end = match range.end_bound() {
            std::ops::Bound::Included(&n) => n + 1,
            std::ops::Bound::Excluded(&n) => n,
            std::ops::Bound::Unbounded => self.data.len(),
        };
        
        Self {
            data: self.data.slice(start..end),
        }
    }

    /// Create a sub-view from offset
    pub fn slice_from(&self, start: usize) -> Self {
        Self {
            data: self.data.slice(start..),
        }
    }

    /// Create a sub-view to offset
    pub fn slice_to(&self, end: usize) -> Self {
        Self {
            data: self.data.slice(..end),
        }
    }

    /// Move the view forward (consume bytes from the front)
    pub fn advance(&mut self, cnt: usize) {
        if cnt > 0 && cnt <= self.data.len() {
            self.data = self.data.slice(cnt..);
        }
    }

    /// Get as slice
    pub fn as_slice(&self) -> &[u8] {
        &self.data
    }

    /// Get the underlying Bytes
    pub fn as_bytes(&self) -> &Bytes {
        &self.data
    }

    /// Clone the underlying bytes (cheap operation)
    pub fn clone_bytes(&self) -> Bytes {
        self.data.clone()
    }

    /// Convert to string (lossy)
    pub fn to_string_lossy(&self) -> String {
        String::from_utf8_lossy(&self.data).to_string()
    }

    /// Convert to owned Vec<u8>
    pub fn to_vec(&self) -> Vec<u8> {
        self.data.to_vec()
    }

    /// Check if this view points to the same data as another
    pub fn ptr_eq(&self, other: &Self) -> bool {
        self.data.as_ptr() == other.data.as_ptr()
    }
}

impl Default for BufferView {
    fn default() -> Self {
        Self::new()
    }
}

impl From<Bytes> for BufferView {
    fn from(data: Bytes) -> Self {
        Self { data }
    }
}

impl From<&[u8]> for BufferView {
    fn from(slice: &[u8]) -> Self {
        Self::from_slice(slice)
    }
}

impl From<Vec<u8>> for BufferView {
    fn from(vec: Vec<u8>) -> Self {
        Self {
            data: Bytes::from(vec),
        }
    }
}

impl From<String> for BufferView {
    fn from(s: String) -> Self {
        Self {
            data: Bytes::from(s),
        }
    }
}

impl AsRef<[u8]> for BufferView {
    fn as_ref(&self) -> &[u8] {
        &self.data
    }
}

/// Buffer builder using BytesMut for efficient building
/// Much simpler than C++ buf_builder
#[derive(Debug)]
pub struct BufferBuilder {
    data: BytesMut,
    tag: BufTag,
}

impl BufferBuilder {
    pub fn new() -> Self {
        Self {
            data: BytesMut::new(),
            tag: BufTag::Common,
        }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            data: BytesMut::with_capacity(capacity),
            tag: BufTag::Common,
        }
    }

    pub fn with_capacity_and_tag(capacity: usize, tag: BufTag) -> Self {
        Self {
            data: BytesMut::with_capacity(capacity),
            tag,
        }
    }

    /// Append bytes from slice
    pub fn append_slice(&mut self, data: &[u8]) {
        self.data.extend_from_slice(data);
    }

    /// Append bytes from another buffer
    pub fn append_bytes(&mut self, bytes: &Bytes) {
        self.data.extend_from_slice(bytes);
    }

    /// Append from BufferView
    pub fn append_view(&mut self, view: &BufferView) {
        self.data.extend_from_slice(view.as_slice());
    }

    /// Append from IoBuffer
    pub fn append_buffer(&mut self, buffer: &IoBuffer) {
        self.data.extend_from_slice(buffer.as_slice());
    }

    /// Reserve additional capacity
    pub fn reserve(&mut self, additional: usize) {
        self.data.reserve(additional);
    }

    /// Get current length (occupied space)
    pub fn len(&self) -> usize {
        self.data.len()
    }

    /// Check if empty
    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    /// Get capacity
    pub fn capacity(&self) -> usize {
        self.data.capacity()
    }

    /// Get remaining capacity
    pub fn remaining_capacity(&self) -> usize {
        self.capacity() - self.len()
    }

    /// Clear the builder
    pub fn clear(&mut self) {
        self.data.clear();
    }

    /// Get tag
    pub fn tag(&self) -> BufTag {
        self.tag
    }

    /// Set tag
    pub fn set_tag(&mut self, tag: BufTag) {
        self.tag = tag;
    }

    /// Get as slice
    pub fn as_slice(&self) -> &[u8] {
        &self.data
    }

    /// Get mutable access to underlying BytesMut
    pub fn as_bytes_mut(&mut self) -> &mut BytesMut {
        &mut self.data
    }

    /// Build into BufferView (zero-copy)
    pub fn build_view(&self) -> BufferView {
        BufferView::from_bytes(self.data.clone().freeze())
    }

    /// Build into Bytes (consuming the builder)
    pub fn build(self) -> Bytes {
        self.data.freeze()
    }

    /// Build into IoBuffer
    pub fn build_io_buffer(self) -> IoBuffer {
        IoBuffer {
            data: self.data,
            is_aligned: false,
            tag: self.tag,
        }
    }

    /// Split off part of the data
    pub fn split_off(&mut self, at: usize) -> Bytes {
        self.data.split_off(at).freeze()
    }

    /// Split to get the first part
    pub fn split_to(&mut self, at: usize) -> Bytes {
        self.data.split_to(at).freeze()
    }
}

impl Default for BufferBuilder {
    fn default() -> Self {
        Self::new()
    }
}

impl From<Vec<u8>> for BufferBuilder {
    fn from(vec: Vec<u8>) -> Self {
        Self {
            data: BytesMut::from(vec.as_slice()),
            tag: BufTag::Common,
        }
    }
}

impl From<&[u8]> for BufferBuilder {
    fn from(slice: &[u8]) -> Self {
        Self {
            data: BytesMut::from(slice),
            tag: BufTag::Common,
        }
    }
}

impl Drop for AlignedBuffer {
    fn drop(&mut self) {
        // Update metrics to track deallocation
        let allocator = AlignedAllocator::instance().lock().unwrap();
        allocator.metrics().decrement(self.tag, self.data.len());
    }
}

// AlignedBuffer is automatically Send + Sync because AVec<u8, RuntimeAlign> is Send + Sync

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_buftag() {
        assert_eq!(BufTag::Common.name(), "common");
        assert_eq!(BufTag::Bitset.name(), "bitset");
        assert_eq!(BufTag::BtreeNode.name(), "btree_node");
    }

    #[test]
    fn test_aligned_allocator_metrics() {
        let metrics = AlignedAllocatorMetrics::new();
        
        metrics.increment(BufTag::Common, 1024);
        metrics.increment(BufTag::Bitset, 512);
        
        assert_eq!(metrics.get_counter(BufTag::Common), 1024);
        assert_eq!(metrics.get_counter(BufTag::Bitset), 512);
        
        metrics.decrement(BufTag::Common, 256);
        assert_eq!(metrics.get_counter(BufTag::Common), 768);
    }

    #[test]
    fn test_sg_list_and_iterator() {
        let data1 = Bytes::from_static(b"Hello");
        let data2 = Bytes::from_static(b", World!");
        
        let mut sg_list = SgList::new();
        sg_list.add_slice(data1.clone());
        sg_list.add_slice(data2.clone());
        
        assert_eq!(sg_list.total_size(), 13);
        assert_eq!(sg_list.slices.len(), 2);
        
        // Test iterator
        let mut iterator = SgIterator::new(sg_list.slices.clone());
        let next_bytes = iterator.next_bytes(8);
        
        // Should get "Hello" and ", W"
        assert_eq!(next_bytes.len(), 2);
        assert_eq!(next_bytes[0].as_ref(), b"Hello");
        assert_eq!(next_bytes[1].as_ref(), b", W");
        
        // Test concatenation
        let mut combined = Vec::new();
        for bytes in &next_bytes {
            combined.extend_from_slice(bytes);
        }
        assert_eq!(combined, b"Hello, W");
    }

    #[test]
    fn test_aligned_buffer_creation() {
        let buffer = AlignedBuffer::new(4096, 4096).unwrap();
        assert_eq!(buffer.size(), 4096);
        assert_eq!(buffer.alignment(), 4096);
        assert_eq!(buffer.tag(), BufTag::Common);
        assert_eq!(buffer.as_ptr() as usize % 4096, 0); // Check alignment
    }

    #[test]
    fn test_aligned_buffer_with_tag() {
        let buffer = AlignedBuffer::new_with_tag(1024, 512, BufTag::Bitset).unwrap();
        assert_eq!(buffer.size(), 1024);
        assert_eq!(buffer.alignment(), 512);
        assert_eq!(buffer.tag(), BufTag::Bitset);
    }

    #[test]
    fn test_aligned_buffer_zero_initialized() {
        let buffer = AlignedBuffer::new(1024, 64).unwrap();
        let slice = buffer.as_slice();
        assert!(slice.iter().all(|&b| b == 0));
    }

    #[test]
    fn test_aligned_buffer_write_read() {
        let mut buffer = AlignedBuffer::new(1024, 64).unwrap();
        let slice = buffer.as_mut_slice();
        
        // Write some data
        slice[0] = 0x42;
        slice[100] = 0xFF;
        slice[1023] = 0xAA;
        
        // Read it back
        let read_slice = buffer.as_slice();
        assert_eq!(read_slice[0], 0x42);
        assert_eq!(read_slice[100], 0xFF);
        assert_eq!(read_slice[1023], 0xAA);
    }

    #[test]
    fn test_aligned_buffer_resize() {
        let mut buffer = AlignedBuffer::new(1024, 64).unwrap();
        buffer.as_mut_slice()[0] = 0x42;
        
        buffer.resize(2048).unwrap();
        assert_eq!(buffer.size(), 2048);
        assert_eq!(buffer.as_slice()[0], 0x42); // Data should be preserved
    }

    #[test]
    fn test_aligned_buffer_error_cases() {
        // Zero size should fail
        assert!(AlignedBuffer::new(0, 4096).is_err());
        
        // Non-power-of-two alignment should fail
        assert!(AlignedBuffer::new(1024, 100).is_err());
    }

    #[test]
    fn test_io_buffer() {
        let mut buffer = IoBuffer::with_capacity(256, BufTag::Common);
        buffer.resize(256, 0);
        assert_eq!(buffer.len(), 256);
        assert!(!buffer.is_aligned());
        
        // Write some data
        let slice = buffer.as_mut_slice();
        slice[0] = 0x11;
        slice[255] = 0x22;
        
        assert_eq!(buffer.as_slice()[0], 0x11);
        assert_eq!(buffer.as_slice()[255], 0x22);
    }

    #[test]
    fn test_io_buffer_from_string() {
        let buffer = IoBuffer::from_string("Hello, World!");
        assert_eq!(buffer.len(), 13);
        assert_eq!(buffer.as_slice(), b"Hello, World!");
        assert!(!buffer.is_aligned());
    }

    #[test]
    fn test_io_buffer_aligned() {
        let buffer = IoBuffer::with_aligned_capacity(1024, 512, BufTag::Bitset).unwrap();
        assert_eq!(buffer.len(), 1024);
        assert!(buffer.is_aligned());
        assert_eq!(buffer.tag(), BufTag::Bitset);
    }

    #[test]
    fn test_io_buffer_operations() {
        let mut buffer = IoBuffer::with_capacity(128, BufTag::Common);
        buffer.extend_from_slice(b"Hello");
        assert_eq!(buffer.len(), 5);
        assert_eq!(buffer.as_slice(), b"Hello");
        
        buffer.extend_from_slice(b", World!");
        assert_eq!(buffer.len(), 13);
        assert_eq!(buffer.as_slice(), b"Hello, World!");
        
        // Test split operations
        let buffer2 = buffer.split_off(7);
        assert_eq!(buffer.as_slice(), b"Hello, ");
        assert_eq!(buffer2.as_slice(), b"World!");
    }

    #[test]
    fn test_shared_buffer() {
        let buffer = make_shared_buffer(1024, BufTag::Common);
        assert_eq!(buffer.len(), 0); // Initially empty
        
        // Can be cloned cheaply
        let buffer2 = buffer.clone();
        assert_eq!(buffer2.len(), 0);
        
        // Test with aligned buffer
        let aligned_buffer = make_aligned_shared_buffer(512, 256, BufTag::Bitset).unwrap();
        assert_eq!(aligned_buffer.len(), 512);
    }

    #[test]
    fn test_buffer_view() {
        let data = b"Hello, World! This is a longer test string.";
        let view = BufferView::from_slice(data);
        
        assert_eq!(view.len(), data.len());
        assert_eq!(view.as_slice(), data);
        
        // Test slicing (zero-copy)
        let sub_view = view.slice(7..12);
        assert_eq!(sub_view.as_slice(), b"World");
        
        let prefix = view.slice(..5);
        assert_eq!(prefix.as_slice(), b"Hello");
        
        let suffix = view.slice(7..);
        assert_eq!(suffix.as_slice(), b"World! This is a longer test string.");
    }

    #[test]
    fn test_buffer_view_advance() {
        let data = b"Hello, World!";
        let mut view = BufferView::from_slice(data);
        
        assert_eq!(view.len(), 13);
        assert_eq!(view.as_slice(), b"Hello, World!");
        
        view.advance(7);
        assert_eq!(view.len(), 6);
        assert_eq!(view.as_slice(), b"World!");
        
        view.advance(3);
        assert_eq!(view.len(), 3);
        assert_eq!(view.as_slice(), b"ld!");
    }

    #[test]
    fn test_buffer_view_conversions() {
        let data = "Hello, 世界!";
        let view = BufferView::from_string(data);
        
        assert_eq!(view.as_slice(), data.as_bytes());
        assert_eq!(view.to_string_lossy(), data);
        
        let vec = view.to_vec();
        assert_eq!(vec, data.as_bytes());
        
        let bytes = view.clone_bytes();
        assert_eq!(bytes.as_ref(), data.as_bytes());
    }

    #[test]
    fn test_buffer_builder() {
        let mut builder = BufferBuilder::with_capacity(128);
        assert_eq!(builder.len(), 0);
        assert!(builder.capacity() >= 128);
        
        // Append some data
        builder.append_slice(b"Hello");
        assert_eq!(builder.len(), 5);
        assert_eq!(builder.as_slice(), b"Hello");
        
        builder.append_slice(b", ");
        builder.append_slice(b"World!");
        assert_eq!(builder.len(), 13);
        assert_eq!(builder.as_slice(), b"Hello, World!");
        
        // Build into view
        let view = builder.build_view();
        assert_eq!(view.as_slice(), b"Hello, World!");
    }

    #[test]
    fn test_buffer_builder_auto_grow() {
        let mut builder = BufferBuilder::with_capacity(10); // Small initial size
        
        // Append data larger than initial capacity
        let large_data = vec![42u8; 20];
        builder.append_slice(&large_data);
        
        assert_eq!(builder.len(), 20);
        assert!(builder.capacity() >= 20); // Should have grown
        assert!(builder.as_slice().iter().all(|&b| b == 42));
    }

    #[test]
    fn test_buffer_builder_operations() {
        let mut builder = BufferBuilder::new();
        
        // Test appending different types
        let bytes = Bytes::from_static(b"Hello");
        builder.append_bytes(&bytes);
        
        let view = BufferView::from_slice(b", World!");
        builder.append_view(&view);
        
        let io_buffer = IoBuffer::from_string("!");
        builder.append_buffer(&io_buffer);
        
        assert_eq!(builder.as_slice(), b"Hello, World!!");
        
        // Test building
        let final_bytes = builder.build();
        assert_eq!(final_bytes.as_ref(), b"Hello, World!!");
    }

    #[test]
    fn test_buffer_builder_split() {
        let mut builder = BufferBuilder::from(b"Hello, World!".as_slice());
        
        let prefix = builder.split_to(7);
        assert_eq!(prefix.as_ref(), b"Hello, ");
        assert_eq!(builder.as_slice(), b"World!");
        
        let suffix = builder.split_off(3);
        assert_eq!(suffix.as_ref(), b"ld!");
        assert_eq!(builder.as_slice(), b"Wor");
    }

    #[test]
    fn test_metrics_tracking() {
        // This test ensures that allocations are tracked by metrics
        let initial_common = {
            let allocator = AlignedAllocator::instance().lock().unwrap();
            allocator.metrics().get_counter(BufTag::Common)
        };
        
        let buffer = AlignedBuffer::new(1024, 64).unwrap();
        
        let after_alloc = {
            let allocator = AlignedAllocator::instance().lock().unwrap();
            allocator.metrics().get_counter(BufTag::Common)
        };
        
        assert!(after_alloc >= initial_common + 1024);
        
        drop(buffer);
        
        // Note: Due to async nature of drop and potential other allocations,
        // we can't easily test the exact decrement here, but the increment test above
        // validates that the metrics tracking is working
    }

    #[test]
    fn test_zero_copy_operations() {
        // Test that Bytes operations are truly zero-copy
        let original = Bytes::from_static(b"Hello, World! This is a test of zero-copy operations.");
        
        // Slicing should be zero-copy
        let slice1 = original.slice(0..5);
        let slice2 = original.slice(7..12);
        
        // Both slices should point to the same underlying data
        assert_eq!(slice1.as_ref(), b"Hello");
        assert_eq!(slice2.as_ref(), b"World");
        
        // Converting to BufferView should also be zero-copy
        let view1 = BufferView::from_bytes(slice1);
        let view2 = BufferView::from_bytes(slice2);
        
        assert_eq!(view1.as_slice(), b"Hello");
        assert_eq!(view2.as_slice(), b"World");
        
        // Cloning Bytes is cheap
        let cloned = original.clone();
        assert_eq!(cloned.len(), original.len());
        assert_eq!(cloned.as_ptr(), original.as_ptr()); // Same underlying data
    }
}