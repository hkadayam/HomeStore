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

use std::collections::{HashMap, HashSet};
use crate::BlkId;

// Constants
pub const META_BLK_HDR_MAX_SZ: u32 = 512;
pub const META_BLK_MAGIC: u32 = 0xCEEDBEED;
pub const META_BLK_OVF_MAGIC: u32 = 0xDEADBEEF;
pub const META_BLK_SB_MAGIC: u32 = 0xABCDCEED;
pub const META_BLK_SB_VERSION: u32 = 0x1;
pub const META_BLK_VERSION: u32 = 0x1;
pub const MAX_SUBSYS_TYPE_LEN: usize = 64;
pub const CONTEXT_DATA_OFFSET_ALIGNMENT: usize = 64;
pub const MAX_BLK_OVF_HDR_MAX_SZ: u32 = 512;

pub type Crc32T = u32;
pub type MetaSubType = String;

// Callbacks
pub type MetaBlkFoundCb = Box<dyn Fn(&MetaBlk, &[u8], usize) -> () + Send + Sync>;
pub type MetaBlkRecoverCompCb = Box<dyn Fn(bool) -> () + Send + Sync>;

/// Meta subsystem registration information
pub struct MetaSubRegInfo {
    pub do_crc: bool,
    pub meta_bids: HashSet<u64>,
    pub cb: Option<MetaBlkFoundCb>,
    pub comp_cb: Option<MetaBlkRecoverCompCb>,
    pub has_deps: bool,
}

impl MetaSubRegInfo {
    pub fn new() -> Self {
        Self {
            do_crc: true,
            meta_bids: HashSet::new(),
            cb: None,
            comp_cb: None,
            has_deps: false,
        }
    }
}

/// Meta block super super block (first block in the chain)
#[repr(C, packed)]
pub struct MetaBlkSb {
    pub magic: u32,
    pub version: u32,
    pub next_bid: BlkId,
    pub bid: BlkId,
    pub migrated: u8,
    pub pad: [u8; 7],
}

impl MetaBlkSb {
    pub fn to_string(&self) -> String {
        format!(
            "magic: {:#x}, version: {}, next_bid: {:?}, self_bid: {:?}",
            self.magic, self.version, self.next_bid, self.bid
        )
    }
}

/// Meta block header structure
#[repr(C, packed)]
pub struct MetaBlkHdrS {
    pub magic: u32,
    pub version: u32,
    pub gen_cnt: u32,
    pub crc: Crc32T,
    pub next_bid: BlkId,
    pub prev_bid: BlkId,
    pub ovf_bid: BlkId,
    pub bid: BlkId,
    pub context_sz: u64,
    pub compressed_sz: u64,
    pub src_context_sz: u64,
    pub type_name: [u8; MAX_SUBSYS_TYPE_LEN],
    pub compressed: u8,
    pub pad: [u8; 7],
}

impl MetaBlkHdrS {
    pub fn get_type_name(&self) -> String {
        let len = self.type_name.iter().position(|&c| c == 0).unwrap_or(MAX_SUBSYS_TYPE_LEN);
        String::from_utf8_lossy(&self.type_name[..len]).to_string()
    }
}

/// Meta block header with padding
#[repr(C, packed)]
pub struct MetaBlkHdr {
    pub h: MetaBlkHdrS,
    pub padding: [u8; META_BLK_HDR_MAX_SZ as usize - std::mem::size_of::<MetaBlkHdrS>()],
}

/// Meta block structure
#[repr(C, packed)]
pub struct MetaBlk {
    pub hdr: MetaBlkHdr,
    // Context data follows immediately after this structure
}

impl MetaBlk {
    pub fn get_context_data(&self, block_size: usize) -> &[u8] {
        let base_ptr = self as *const MetaBlk as *const u8;
        let context_offset = std::mem::size_of::<MetaBlk>();
        let context_ptr = unsafe { base_ptr.add(context_offset) };
        let context_sz = (block_size - context_offset).min(self.hdr.h.context_sz as usize);
        unsafe { std::slice::from_raw_parts(context_ptr, context_sz) }
    }

    pub fn to_string(&self) -> String {
        // Copy packed fields to aligned locals to avoid E0793 (unaligned reference).
        let type_name = self.hdr.h.get_type_name();
        let magic = self.hdr.h.magic;
        let version = self.hdr.h.version;
        let gen_cnt = self.hdr.h.gen_cnt;
        let crc = self.hdr.h.crc;
        let next_bid = self.hdr.h.next_bid;
        let prev_bid = self.hdr.h.prev_bid;
        let ovf_bid = self.hdr.h.ovf_bid;
        let self_bid = self.hdr.h.bid;
        let context_sz = self.hdr.h.context_sz;
        let compressed_sz = self.hdr.h.compressed_sz;
        let src_context_sz = self.hdr.h.src_context_sz;
        let compressed = self.hdr.h.compressed;
        format!("magic: {:#x}, type: {}, version: {}, gen_cnt: {}, crc: {:#x}, next_bid: {:?}, prev_bid: {:?}, \
            ovf_bid: {:?}, context_sz: {}, compressed_sz: {}, src_context_sz: {}, compressed: {}, \
            self_bid: {:?}", magic, type_name, version, gen_cnt, crc, next_bid, prev_bid, ovf_bid, context_sz,
            compressed_sz, src_context_sz, compressed, self_bid)
    }
}

/// Overflow block header structure
#[repr(C, packed)]
pub struct MetaBlkOvfHdrS {
    pub magic: u32,
    pub nbids: u32,
    pub next_bid: BlkId,
    pub bid: BlkId,
    pub context_sz: u64,
}

/// Overflow block header with padding
#[repr(C, packed)]
pub struct MetaBlkOvfHdr {
    pub h: MetaBlkOvfHdrS,
    pub padding: [u8; MAX_BLK_OVF_HDR_MAX_SZ as usize - std::mem::size_of::<MetaBlkOvfHdrS>()],
    // Data BlkIds follow immediately after this structure
}

impl MetaBlkOvfHdr {
    pub fn get_data_bids(&self, block_size: usize) -> &[BlkId] {
        let base_ptr = self as *const MetaBlkOvfHdr as *const u8;
        let data_bid_offset = std::mem::size_of::<MetaBlkOvfHdr>();
        let data_bid_ptr = unsafe { base_ptr.add(data_bid_offset) as *const BlkId };
        unsafe { std::slice::from_raw_parts(data_bid_ptr, self.h.nbids as usize) }
    }

    pub fn to_string(&self) -> String {
        // Avoid taking references to packed fields directly (can cause unaligned reference UB)
        // Copy each field into a local properly aligned temporary before formatting.
        let magic = self.h.magic;
        let next_bid = self.h.next_bid;
        let self_bid = self.h.bid;
        let nbids = self.h.nbids;
        let context_sz = self.h.context_sz;
        format!(
            "magic: {:#x}, next_bid: {:?}, self_bid: {:?}, nbids: {}, context_sz: {}",
            magic, next_bid, self_bid, nbids, context_sz
        )
    }
}

/// Meta vdev context
#[repr(C, packed)]
pub struct MetaVdevContext {
    pub vdev_type: u8,
    pub first_blkid: BlkId,
}

// Type aliases for collections
pub type MetaBlkMap = HashMap<u64, Box<MetaBlk>>;
pub type OvfHdrMap = HashMap<u64, Box<MetaBlkOvfHdr>>;
pub type ClientInfoMap = HashMap<MetaSubType, MetaSubRegInfo>;
pub type SubtypeGraph = HashMap<MetaSubType, Vec<MetaSubType>>;
