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

use std::sync::atomic::{AtomicU32, Ordering};

use iomgr::iomgr;
use sisl::reactor_local::ReactorLocal;

use crate::common::{BlkId, BlkNum, BlkTemp};

/// Bitmap word size in blocks (64 blocks per word)
const WORD_SIZE: BlkNum = 64;

/// Base trait for portion data - implemented by allocator-specific portion types
/// This trait is object-safe (dyn compatible) for use with trait objects
pub trait PortionBase: Send + Sync + 'static {
    fn reactor_id(&self) -> u32;
    fn start_blk(&self) -> BlkNum;
    fn end_blk(&self) -> BlkNum;
    fn contains(&self, blk: BlkNum) -> bool { blk >= self.start_blk() && blk < self.end_blk() }
    fn len(&self) -> BlkNum { self.end_blk() - self.start_blk() }
    
    /// Helper for downcasting - returns self as Any
    /// Implementors should return self
    fn as_any(&self) -> &dyn std::any::Any;
}

/// Factory trait for creating portions - separate from PortionBase to keep it object-safe
pub trait PortionFactory<T: PortionBase> {
    fn new(reactor_id: u32, start_blk: BlkNum, end_blk: BlkNum) -> T;
}

/// Empty/dummy portion implementation for segments without portions
pub struct EmptyPortion {
    reactor_id: u32,
    start_blk: BlkNum,
    end_blk: BlkNum,
}

impl EmptyPortion {
    pub fn new(reactor_id: u32, start_blk: BlkNum, end_blk: BlkNum) -> Self {
        Self { reactor_id, start_blk, end_blk }
    }
}

impl PortionBase for EmptyPortion {
    fn reactor_id(&self) -> u32 { self.reactor_id }
    fn start_blk(&self) -> BlkNum { self.start_blk }
    fn end_blk(&self) -> BlkNum { self.end_blk }
    fn as_any(&self) -> &dyn std::any::Any { self }
}

/// Segment manages a contiguous range of blocks with reactor-local portions
/// Uses trait objects to avoid templates and support flexible portion types
pub struct Segment {
    segment_id: u32,
    temperature: BlkTemp,
    start_blk: BlkNum,
    num_blks: BlkNum,
    ondisk_portions: ReactorLocal<Box<dyn PortionBase>>,
    inmem_portions: ReactorLocal<Box<dyn PortionBase>>,
}

impl Segment {
    pub fn new(
        segment_id: u32,
        temperature: Option<BlkTemp>,
        start_blk: BlkNum,
        num_blks: BlkNum,
        ondisk_portions: ReactorLocal<Box<dyn PortionBase>>,
        inmem_portions: ReactorLocal<Box<dyn PortionBase>>,
    ) -> Self {
        let temperature = temperature.unwrap_or(segment_id);
        Self { segment_id, temperature, start_blk, num_blks, ondisk_portions, inmem_portions }
    }

    pub fn temperature(&self) -> BlkTemp { self.temperature }
    pub fn segment_id(&self) -> u32 { self.segment_id }
    pub fn start_blk(&self) -> BlkNum { self.start_blk }
    pub fn num_blks(&self) -> BlkNum { self.num_blks }
    pub fn end_blk(&self) -> BlkNum { self.start_blk + self.num_blks }
    
    /// Get the current reactor's ondisk portion
    pub fn my_ondisk_portion(&self) -> &dyn PortionBase { &**self.ondisk_portions.get() }
    
    /// Get the current reactor's inmem portion
    pub fn my_inmem_portion(&self) -> &dyn PortionBase { &**self.inmem_portions.get() }
}

/// SegmentManager manages a collection of segments
pub struct SegmentManager {
    segments: Vec<Box<Segment>>,
    segments_count: u32,
    num_blks: BlkNum,
    fixed_size_segments: bool,
    rr_next_segment: AtomicU32, // Next segment index for round-robin temperature=0 selection
}

impl SegmentManager {
    /// Create a SegmentManager from pre-built segments
    pub fn from_segments(segments: Vec<Box<Segment>>, fixed_size_segments: bool) -> Self {
        let segments_count = segments.len() as u32;
        let num_blks = segments.last().map(|s| s.end_blk()).unwrap_or(0);
        Self { segments, segments_count, num_blks, fixed_size_segments, rr_next_segment: AtomicU32::new(0) }
    }

    /// Create a SegmentManager with customizable portion creators
    /// 
    /// SegmentManager handles:
    /// - Dividing total blocks into segments
    /// - Subdividing each segment into reactor-local portions
    /// - Creating ReactorLocal wrappers for portions
    /// 
    /// The creator callbacks only need to create the actual portion object:
    /// - ondisk_portion_creator: (reactor_id, portion_start, portion_end) -> Box<dyn PortionBase>
    /// - inmem_portion_creator: (reactor_id, portion_start, portion_end) -> Box<dyn PortionBase>
    /// 
    /// # Arguments
    /// * `num_blks` - Total number of blocks
    /// * `seg_count` - Number of segments to create
    /// * `num_reactors` - Number of reactors (portions per segment)
    /// * `ondisk_portion_creator` - Creates ondisk portion: (reactor_id, start_blk, end_blk) -> Box<dyn PortionBase>
    /// * `inmem_portion_creator` - Creates inmem portion: (reactor_id, start_blk, end_blk) -> Box<dyn PortionBase>
    pub fn new<F1, F2>(
        num_blks: BlkNum, 
        seg_count: u32, 
        num_reactors: usize,
        fixed_size_segments: bool,
        ondisk_portion_creator: F1,
        inmem_portion_creator: F2,
    ) -> Self 
    where
        F1: Fn(u32, BlkNum, BlkNum) -> Box<dyn PortionBase> + Send + Sync + Clone + 'static,
        F2: Fn(u32, BlkNum, BlkNum) -> Box<dyn PortionBase> + Send + Sync + Clone + 'static,
    {
        assert!(fixed_size_segments, "SegmentManager currently assumes fixed_size_segments=true");
        
        let seg_count = seg_count.max(1);
        let mut segments: Vec<Box<Segment>> = Vec::with_capacity(seg_count as usize);

        // Each segment should be aligned to word size (64 blocks)
        let seg_len = (num_blks / seg_count as BlkNum / WORD_SIZE) * WORD_SIZE;

        let mut seg_start: BlkNum = 0;
        for sid in 0..seg_count {
            let remaining = num_blks - seg_start;
            let this_seg_len = if sid == seg_count - 1 { remaining } else { std::cmp::min(seg_len, remaining) };

            // Calculate portion ranges for this segment
            let seg_start_blk = seg_start;
            let seg_num_blks = this_seg_len;
            let portion_len = (seg_num_blks / num_reactors as BlkNum / WORD_SIZE) * WORD_SIZE;
            
            // Create ondisk portions using ReactorLocal
            // SegmentManager handles the ReactorLocal and portion range calculation
            // Callback only creates the portion object
            let ondisk_creator_clone = ondisk_portion_creator.clone();
            let ondisk_portions = ReactorLocal::new(move || {
                let rid = iomgr().current_reactor_id();
                let portion_start = seg_start_blk + (rid as BlkNum * portion_len);
                let portion_end = if rid == num_reactors - 1 {
                    seg_start_blk + seg_num_blks
                } else {
                    portion_start + portion_len
                };
                // Call user's creator to create the portion
                ondisk_creator_clone(rid as u32, portion_start, portion_end)
            });
            
            // Create inmem portions using ReactorLocal
            let inmem_creator_clone = inmem_portion_creator.clone();
            let inmem_portions = ReactorLocal::new(move || {
                let rid = iomgr().current_reactor_id();
                let portion_start = seg_start_blk + (rid as BlkNum * portion_len);
                let portion_end = if rid == num_reactors - 1 {
                    seg_start_blk + seg_num_blks
                } else {
                    portion_start + portion_len
                };
                // Call user's creator to create the portion
                inmem_creator_clone(rid as u32, portion_start, portion_end)
            });

            let seg_obj = Segment::new(sid, None, seg_start, this_seg_len, ondisk_portions, inmem_portions);
            seg_start += this_seg_len;
            segments.push(Box::new(seg_obj));
        }

        let segments_count = segments.len() as u32;
        Self { segments, segments_count, num_blks, fixed_size_segments, rr_next_segment: AtomicU32::new(0) }
    }

    pub fn segments(&self) -> &[Box<Segment>] { &self.segments }
    pub fn segments_count(&self) -> u32 { self.segments_count }
    pub fn num_blks(&self) -> BlkNum { self.num_blks }

    /// Get the segment index for a given block ID
    #[inline]
    pub fn get_segment_index(&self, bid: &BlkId) -> u32 {
        let blk_num = bid.blk_num();
        if self.fixed_size_segments {
            let seg_idx = blk_num / (self.num_blks / self.segments_count as BlkNum);
            seg_idx.min((self.segments_count - 1) as BlkNum) as u32
        } else {
            for (idx, seg) in self.segments.iter().enumerate() {
                if blk_num < seg.end_blk() {
                    return idx as u32;
                }
            }
            self.segments_count - 1
        }
    }

    #[inline]
    pub fn get_segment(&self, idx: usize) -> Option<&Segment> {
        self.segments.get(idx).map(|b| b.as_ref())
    }

    /// Get the segment that contains the given block ID
    #[inline]
    pub fn blkid_to_segment(&self, bid: &BlkId) -> &Segment {
        let seg_idx = self.get_segment_index(bid) as usize;
        self.get_segment(seg_idx).expect("Invalid segment index")
    }

    /// Select a segment based on temperature hint
    /// 
    /// Temperature-based segment selection:
    /// - temp == 0: Round-robin across all segments
    /// - temp in 1..=segments.len(): Direct index (temp-1)
    /// - temp > segments.len(): Clamp to last segment
    #[inline]
    pub fn select_segment(&self, temp: BlkTemp) -> &Segment {
        let seg_len = self.segments_count as usize;
        if seg_len == 0 {
            panic!("No segments available");
        }
        
        let idx = if temp == 0 {
            // Round-robin for temperature 0
            let cur = self.rr_next_segment.fetch_add(1, Ordering::Relaxed) % seg_len as u32;
            cur as usize
        } else {
            // Direct temperature mapping: temp 1 -> segment 0, temp 2 -> segment 1, etc.
            let idx = (temp as usize).saturating_sub(1);
            if idx >= seg_len {
                seg_len - 1 // Clamp to last segment
            } else {
                idx
            }
        };
        
        self.get_segment(idx).expect("Invalid segment index")
    }
}
