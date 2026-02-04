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
use std::sync::{Arc, Mutex};
use std::io::{self, Error, ErrorKind};
use std::mem;
use std::ptr;

use crate::BlkId;
use crate::device::virtual_dev::VirtualDev;
use super::meta_sb::*;

const INIT_CRC32: u32 = 0xFFFFFFFF;

/// Meta Block Service - manages metadata blocks for all subsystems
pub struct MetaBlkService {
    sb_vdev: Option<Arc<VirtualDev>>,
    meta_mtx: Arc<Mutex<()>>,
    shutdown_mtx: Arc<Mutex<()>>,
    meta_blks: MetaBlkMap,
    ovf_blk_hdrs: OvfHdrMap,
    sub_info: ClientInfoMap,
    last_mblk_id: Box<BlkId>,
    ssb: Option<Box<MetaBlkSb>>,
    compress_buf: Vec<u8>,
    compress_buf_size: usize,
    inited: bool,
    meta_vdev_context: Option<MetaVdevContext>,
    dep_topo_graph: SubtypeGraph,
    self_recover: bool,
}

impl MetaBlkService {
    /// Create a new MetaBlkService
    pub fn new() -> Self {
        Self {
            sb_vdev: None,
            meta_mtx: Arc::new(Mutex::new(())),
            shutdown_mtx: Arc::new(Mutex::new(())),
            meta_blks: HashMap::new(),
            ovf_blk_hdrs: HashMap::new(),
            sub_info: HashMap::new(),
            last_mblk_id: Box::new(BlkId::default()),
            ssb: None,
            compress_buf: Vec::new(),
            compress_buf_size: 0,
            inited: false,
            meta_vdev_context: None,
            dep_topo_graph: HashMap::new(),
            self_recover: false,
        }
    }

    /// Start the meta block service
    pub async fn start(&mut self, need_format: bool) -> io::Result<()> {
        if need_format {
            self.format_ssb().await?;
        } else {
            self.load_ssb().await?;
            self.scan_meta_blks().await?;
        }
        Ok(())
    }

    /// Stop the meta block service
    pub async fn stop(&mut self) -> io::Result<()> {
        {
            let _shutdown_guard = self.shutdown_mtx.lock().unwrap();
            self.cache_clear();
            
            {
                let _meta_guard = self.meta_mtx.lock().unwrap();
                self.sub_info.clear();
            }
        }
        
        self.ssb = None;
        self.compress_buf.clear();
        self.sb_vdev = None;
        
        Ok(())
    }

    /// Register a subsystem handler
    pub fn register_handler(
        &mut self,
        sub_type: MetaSubType,
        cb: MetaBlkFoundCb,
        comp_cb: MetaBlkRecoverCompCb,
        do_crc: bool,
        deps: Option<Vec<MetaSubType>>,
    ) -> io::Result<()> {
        let _guard = self.meta_mtx.lock().unwrap();
        
        if sub_type.len() >= MAX_SUBSYS_TYPE_LEN {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                format!("Subsystem type name too long: {}", sub_type.len()),
            ));
        }

        let reg_info = self.sub_info.entry(sub_type.clone()).or_insert_with(MetaSubRegInfo::new);
        reg_info.cb = Some(cb);
        reg_info.comp_cb = Some(comp_cb);
        reg_info.do_crc = do_crc;

        if let Some(dependencies) = deps {
            reg_info.has_deps = true;
            for dep in dependencies {
                self.sub_info.entry(dep.clone()).or_insert_with(MetaSubRegInfo::new).has_deps = true;
                self.dep_topo_graph.entry(dep).or_insert_with(Vec::new).push(sub_type.clone());
            }
        }

        Ok(())
    }

    /// Deregister a subsystem handler
    pub fn deregister_handler(&mut self, sub_type: &MetaSubType) {
        let _guard = self.meta_mtx.lock().unwrap();
        
        if self.sub_info.remove(sub_type).is_some() {
            self.dep_topo_graph.remove(sub_type);
            log::info!("[type={}] deregistered successfully", sub_type);
        } else {
            log::info!("[type={}] not found in registered list, no-op", sub_type);
        }
    }

    /// Add a subsystem superblock
    pub async fn add_sub_sb(
        &mut self,
        sub_type: MetaSubType,
        context_data: &[u8],
    ) -> io::Result<*mut MetaBlk> {
        let _guard = self.meta_mtx.lock().unwrap();
        
        if !self.inited {
            return Err(Error::new(
                ErrorKind::Other,
                "Accessing metablk store before init is not allowed",
            ));
        }

        if sub_type.len() >= MAX_SUBSYS_TYPE_LEN {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                format!("Subsystem type name too long: {}", sub_type.len()),
            ));
        }

        if !self.sub_info.contains_key(&sub_type) {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                format!("[type={}] not registered yet!", sub_type),
            ));
        }

        let meta_bid = self.alloc_meta_blk().await?;

        // Add meta_bid to in-memory reverse mapping
        if let Some(info) = self.sub_info.get_mut(&sub_type) {
            info.meta_bids.insert(meta_bid.to_integer());
        }

        let mblk = self.init_meta_blk(meta_bid, sub_type, context_data).await?;
        let ptr = mblk.as_ref() as *const MetaBlk as *mut MetaBlk;
        
        self.meta_blks.insert(meta_bid.to_integer(), mblk);
        
        Ok(ptr)
    }

    /// Update a subsystem superblock in-place
    pub async fn update_sub_sb(
        &mut self,
        context_data: &[u8],
        cookie: *mut MetaBlk,
    ) -> io::Result<()> {
        let _guard = self.meta_mtx.lock().unwrap();
        
        if !self.inited {
            return Err(Error::new(
                ErrorKind::Other,
                "Accessing metablk store before init is not allowed",
            ));
        }

        // Safety: caller guarantees cookie is valid
        let mblk = unsafe { &mut *cookie };
        
        let ovf_bid_to_free = mblk.hdr.h.ovf_bid;
        mblk.hdr.h.compressed = 0;
        mblk.hdr.h.ovf_bid = BlkId::default();
        mblk.hdr.h.gen_cnt += 1;

        // Write meta block to disk
        self.write_meta_blk_internal(mblk, context_data).await?;

        // Free old overflow blocks if any
        if ovf_bid_to_free.is_valid() {
            self.free_ovf_blk_chain(ovf_bid_to_free).await?;
        }

        Ok(())
    }

    /// Remove a subsystem superblock
    pub async fn remove_sub_sb(&mut self, cookie: *mut MetaBlk) -> io::Result<()> {
        let _guard = self.meta_mtx.lock().unwrap();
        
        if !self.inited {
            return Err(Error::new(
                ErrorKind::Other,
                "Accessing metablk store before init is not allowed",
            ));
        }

        // Safety: caller guarantees cookie is valid
        let rm_blk = unsafe { &*cookie };
        let rm_bid = rm_blk.hdr.h.bid;
        let type_name = rm_blk.hdr.h.get_type_name();

        if !self.meta_blks.contains_key(&rm_bid.to_integer()) {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                format!("[type={}], id: {:?} not found!", type_name, rm_bid),
            ));
        }

        let prev_bid = rm_blk.hdr.h.prev_bid;
        let next_bid = rm_blk.hdr.h.next_bid;

        log::info!(
            "[type={}], remove_sub_sb meta blk id: {:?}, prev_bid: {:?}, next_bid: {:?}",
            type_name, rm_bid, prev_bid, next_bid
        );

        // Update previous block's next pointer
        if let Some(ref ssb) = self.ssb {
            if prev_bid.to_integer() == ssb.bid.to_integer() {
                // Update SSB
                let ssb_mut = unsafe { &mut *(self.ssb.as_mut().unwrap().as_mut() as *mut MetaBlkSb) };
                ssb_mut.next_bid = next_bid;
                self.write_ssb().await?;
                
                if self.last_mblk_id.to_integer() == rm_bid.to_integer() {
                    *self.last_mblk_id = BlkId::default();
                }
            } else if let Some(prev_mblk) = self.meta_blks.get_mut(&prev_bid.to_integer()) {
                let prev_mblk_mut = unsafe { &mut *(prev_mblk.as_mut() as *mut MetaBlk) };
                prev_mblk_mut.hdr.h.next_bid = next_bid;
                self.write_meta_blk_to_disk(prev_mblk_mut).await?;
            }
        }

        // Update next block's prev pointer
        if next_bid.is_valid() {
            if let Some(next_mblk) = self.meta_blks.get_mut(&next_bid.to_integer()) {
                let next_mblk_mut = unsafe { &mut *(next_mblk.as_mut() as *mut MetaBlk) };
                next_mblk_mut.hdr.h.prev_bid = prev_bid;
                self.write_meta_blk_to_disk(next_mblk_mut).await?;
            }
        } else {
            // Removing last meta block
            *self.last_mblk_id = prev_bid;
        }

        // Remove from cache
        if let Some(mblk_box) = self.meta_blks.remove(&rm_bid.to_integer()) {
            self.free_meta_blk(&*mblk_box).await?;
        }

        // Remove from sub_info
        if let Some(info) = self.sub_info.get_mut(&type_name) {
            info.meta_bids.remove(&rm_bid.to_integer());
        }

        Ok(())
    }

    /// Read subsystem superblocks and trigger callbacks
    pub async fn read_sub_sb(&mut self, sub_type: &MetaSubType) -> io::Result<()> {
        let _guard = self.meta_mtx.lock().unwrap();
        
        if !self.inited {
            return Err(Error::new(
                ErrorKind::Other,
                "Accessing metablk store before init is not allowed",
            ));
        }

        let info = self.sub_info.get(sub_type).ok_or_else(|| {
            Error::new(
                ErrorKind::InvalidInput,
                format!("Unregistered client [type={}]", sub_type),
            )
        })?;

        let bids: Vec<u64> = info.meta_bids.iter().copied().collect();
        
        for bid in bids {
            if let Some(mblk) = self.meta_blks.get(&bid) {
                let buf = self.read_sub_sb_internal(&**mblk).await?;
                
                if let Some(ref cb) = info.cb {
                    cb(&**mblk, &buf, mblk.hdr.h.context_sz as usize);
                }
            }
        }

        if let Some(ref comp_cb) = info.comp_cb {
            comp_cb(true);
        }

        Ok(())
    }

    /// Recover metadata blocks
    pub async fn recover(&mut self, do_comp_cb: bool) -> io::Result<()> {
        let _guard = self.shutdown_mtx.lock().unwrap();
        
        // Topological sort for dependencies
        let mut ordered_subtypes = Vec::new();
        self.topological_sort(&mut ordered_subtypes)?;

        // Recover dependent subsystems in order
        for subtype in ordered_subtypes {
            self.recover_meta_sub_type(do_comp_cb, &subtype).await?;
        }

        // Recover independent subsystems
        let independent: Vec<String> = self.sub_info.iter()
            .filter(|(_, info)| !info.has_deps)
            .map(|(k, _)| k.clone())
            .collect();

        for subtype in independent {
            self.recover_meta_sub_type(do_comp_cb, &subtype).await?;
        }

        Ok(())
    }

    /// Get metadata block size
    pub fn meta_size(&self, cookie: *const MetaBlk) -> usize {
        let mblk = unsafe { &*cookie };
        let mut nblks = 1; // meta blk itself

        let mut obid = mblk.hdr.h.ovf_bid;
        while obid.is_valid() {
            if let Some(ovf_hdr) = self.ovf_blk_hdrs.get(&obid.to_integer()) {
                nblks += 1; // overflow header block
                
                let block_size = self.block_size();
                let data_bids = ovf_hdr.get_data_bids(block_size);
                for data_bid in data_bids {
                    nblks += data_bid.blk_count() as usize;
                }
                
                obid = ovf_hdr.h.next_bid;
            } else {
                break;
            }
        }

        nblks * self.block_size()
    }

    /// Get total size of meta vdev
    pub fn total_size(&self) -> u64 {
        self.sb_vdev.as_ref().map(|v| v.size()).unwrap_or(0)
    }

    /// Get used size of meta vdev
    pub fn used_size(&self) -> u64 {
        self.sb_vdev.as_ref().map(|v| v.used_size()).unwrap_or(0)
    }

    /// Get block size
    pub fn block_size(&self) -> usize {
        self.sb_vdev.as_ref().map(|v| v.block_size() as usize).unwrap_or(4096)
    }

    /// Get alignment size
    pub fn align_size(&self) -> usize {
        self.sb_vdev.as_ref().map(|v| v.align_size() as usize).unwrap_or(512)
    }

    /// Get available blocks
    pub fn available_blks(&self) -> u64 {
        self.sb_vdev.as_ref().map(|v| v.available_blks()).unwrap_or(0)
    }

    /// Check if aligned buffer is needed
    pub fn is_aligned_buf_needed(&self, size: usize) -> bool {
        size > self.meta_blk_context_sz()
    }

    /// Get meta block context size
    pub fn meta_blk_context_sz(&self) -> usize {
        self.block_size() - META_BLK_HDR_MAX_SZ as usize
    }

    /// Get max number of data blocks in overflow block
    pub fn ovf_blk_max_num_data_blk(&self) -> usize {
        (self.block_size() - MAX_BLK_OVF_HDR_MAX_SZ as usize) / mem::size_of::<BlkId>()
    }

    // Private helper methods

    fn cache_clear(&mut self) {
        let _guard = self.meta_mtx.lock().unwrap();
        self.meta_blks.clear();
        self.ovf_blk_hdrs.clear();
    }

    async fn format_ssb(&mut self) -> io::Result<()> {
        let _guard = self.meta_mtx.lock().unwrap();
        
        let bid = self.alloc_meta_blk().await?;
        
        self.meta_vdev_context = Some(MetaVdevContext {
            vdev_type: 1, // META_VDEV
            first_blkid: bid,
        });

        let mut ssb = Box::new(MetaBlkSb {
            magic: META_BLK_SB_MAGIC,
            version: META_BLK_SB_VERSION,
            next_bid: BlkId::default(),
            bid,
            migrated: 0,
            pad: [0; 7],
        });

        self.last_mblk_id.invalidate();
        self.ssb = Some(ssb);
        
        self.write_ssb().await?;
        self.inited = true;
        
        Ok(())
    }

    async fn load_ssb(&mut self) -> io::Result<()> {
        if let Some(ref ctx) = self.meta_vdev_context {
            let bid = ctx.first_blkid;
            
            // Commit the block
            if let Some(ref vdev) = self.sb_vdev {
                vdev.commit_blk(bid).await?;
            }

            let mut ssb_buf = vec![0u8; self.block_size()];
            self.read(bid, &mut ssb_buf).await?;

            let ssb = unsafe { &*(ssb_buf.as_ptr() as *const MetaBlkSb) };
            
            if ssb.magic != META_BLK_SB_MAGIC {
                return Err(Error::new(ErrorKind::InvalidData, "Invalid SSB magic"));
            }
            
            if ssb.version != META_BLK_SB_VERSION {
                return Err(Error::new(ErrorKind::InvalidData, "Invalid SSB version"));
            }

            self.ssb = Some(Box::new(MetaBlkSb {
                magic: ssb.magic,
                version: ssb.version,
                next_bid: ssb.next_bid,
                bid: ssb.bid,
                migrated: ssb.migrated,
                pad: ssb.pad,
            }));

            self.inited = true;
            log::info!("Successfully loaded meta ssb from disk: {}", self.ssb.as_ref().unwrap().to_string());
        }
        
        Ok(())
    }

    async fn write_ssb(&mut self) -> io::Result<()> {
        if let Some(ref ssb) = self.ssb {
            let ssb_ptr = ssb.as_ref() as *const MetaBlkSb as *const u8;
            let ssb_slice = unsafe { std::slice::from_raw_parts(ssb_ptr, self.block_size()) };
            
            if let Some(ref vdev) = self.sb_vdev {
                vdev.sync_write(ssb_slice, ssb.bid).await?;
            }
            
            log::info!("Successfully wrote ssb to disk: {}", ssb.to_string());
        }
        
        Ok(())
    }

    async fn scan_meta_blks(&mut self) -> io::Result<()> {
        self.cache_clear();
        
        let ssb_bid = self.ssb.as_ref().map(|s| s.bid).unwrap_or_default();
        let mut bid = self.ssb.as_ref().map(|s| s.next_bid).unwrap_or_default();
        let mut prev_meta_bid = ssb_bid;

        while bid.is_valid() {
            *self.last_mblk_id = bid;

            let mut mblk_buf = vec![0u8; self.block_size()];
            self.read(bid, &mut mblk_buf).await?;

            let mblk = unsafe { &*(mblk_buf.as_ptr() as *const MetaBlk) };
            
            // Verify magic
            if mblk.hdr.h.magic != META_BLK_MAGIC {
                return Err(Error::new(ErrorKind::InvalidData, "Invalid meta block magic"));
            }

            // Create owned copy
            let mblk_box = self.copy_meta_blk(mblk)?;
            let type_name = mblk_box.hdr.h.get_type_name();

            // Add to sub_info
            self.sub_info.entry(type_name.clone())
                .or_insert_with(MetaSubRegInfo::new)
                .meta_bids.insert(bid.to_integer());

            // Commit block
            if let Some(ref vdev) = self.sb_vdev {
                vdev.commit_blk(bid).await?;
            }

            // Handle overflow blocks
            let mut obid = mblk_box.hdr.h.ovf_bid;
            while obid.is_valid() {
                let mut ovf_buf = vec![0u8; self.block_size()];
                self.read(obid, &mut ovf_buf).await?;

                let ovf_hdr = unsafe { &*(ovf_buf.as_ptr() as *const MetaBlkOvfHdr) };
                
                if ovf_hdr.h.magic != META_BLK_OVF_MAGIC {
                    return Err(Error::new(ErrorKind::InvalidData, "Invalid overflow block magic"));
                }

                let ovf_box = self.copy_ovf_hdr(ovf_hdr)?;
                
                // Commit overflow block
                if let Some(ref vdev) = self.sb_vdev {
                    vdev.commit_blk(obid).await?;
                    
                    // Commit data blocks
                    let data_bids = ovf_box.get_data_bids(self.block_size());
                    for data_bid in data_bids {
                        vdev.commit_blk(*data_bid).await?;
                    }
                }

                obid = ovf_box.h.next_bid;
                self.ovf_blk_hdrs.insert(obid.to_integer(), ovf_box);
            }

            prev_meta_bid = bid;
            bid = mblk_box.hdr.h.next_bid;
            
            self.meta_blks.insert(mblk_box.hdr.h.bid.to_integer(), mblk_box);
        }

        Ok(())
    }

    async fn alloc_meta_blk(&mut self) -> io::Result<BlkId> {
        if let Some(ref vdev) = self.sb_vdev {
            vdev.alloc_contiguous_blks(1).await
        } else {
            Err(Error::new(ErrorKind::Other, "VirtualDev not initialized"))
        }
    }

    async fn init_meta_blk(
        &mut self,
        bid: BlkId,
        sub_type: MetaSubType,
        context_data: &[u8],
    ) -> io::Result<Box<MetaBlk>> {
        // Allocate meta block
        let block_size = self.block_size();
        let mut mblk_buf = vec![0u8; block_size];
        
        let mblk = unsafe { &mut *(mblk_buf.as_mut_ptr() as *mut MetaBlk) };
        
        mblk.hdr.h.magic = META_BLK_MAGIC;
        mblk.hdr.h.version = META_BLK_VERSION;
        mblk.hdr.h.bid = bid;
        mblk.hdr.h.gen_cnt = 0;
        mblk.hdr.h.compressed = 0;
        
        // Set type name
        let type_bytes = sub_type.as_bytes();
        let len = type_bytes.len().min(MAX_SUBSYS_TYPE_LEN);
        mblk.hdr.h.type_name[..len].copy_from_slice(&type_bytes[..len]);

        // Handle prev/next linkage
        if self.last_mblk_id.is_valid() {
            mblk.hdr.h.prev_bid = *self.last_mblk_id;
            
            if let Some(last_mblk) = self.meta_blks.get_mut(&self.last_mblk_id.to_integer()) {
                let last_mblk_mut = unsafe { &mut *(last_mblk.as_mut() as *mut MetaBlk) };
                last_mblk_mut.hdr.h.next_bid = bid;
            }
        } else {
            if let Some(ref ssb) = self.ssb {
                mblk.hdr.h.prev_bid = ssb.bid;
                let ssb_mut = unsafe { &mut *(self.ssb.as_mut().unwrap().as_mut() as *mut MetaBlkSb) };
                ssb_mut.next_bid = bid;
            }
        }

        mblk.hdr.h.next_bid = BlkId::default();

        // Write meta block
        self.write_meta_blk_internal(mblk, context_data).await?;

        // Update previous last or ssb
        if self.last_mblk_id.is_valid() {
            if let Some(last_mblk) = self.meta_blks.get(&self.last_mblk_id.to_integer()) {
                self.write_meta_blk_to_disk(&**last_mblk).await?;
            }
        } else {
            self.write_ssb().await?;
        }

        *self.last_mblk_id = bid;

        // Return owned copy
        self.copy_meta_blk(mblk)
    }

    async fn write_meta_blk_internal(&mut self, mblk: &mut MetaBlk, context_data: &[u8]) -> io::Result<()> {
        let sz = context_data.len();
        mblk.hdr.h.context_sz = sz as u64;

        if sz <= self.meta_blk_context_sz() {
            // Inline context data
            mblk.hdr.h.ovf_bid = BlkId::default();
            let context_ptr = unsafe {
                (mblk as *mut MetaBlk as *mut u8).add(mem::size_of::<MetaBlk>())
            };
            unsafe {
                ptr::copy_nonoverlapping(context_data.as_ptr(), context_ptr, sz);
            }
        } else {
            // Use overflow blocks
            let obid = self.write_meta_blk_ovf(context_data).await?;
            mblk.hdr.h.ovf_bid = obid;
        }

        // Calculate CRC if needed
        let type_name = mblk.hdr.h.get_type_name();
        if let Some(info) = self.sub_info.get(&type_name) {
            if info.do_crc {
                mblk.hdr.h.crc = crc32(context_data);
            }
        }

        self.write_meta_blk_to_disk(mblk).await
    }

    async fn write_meta_blk_to_disk(&self, mblk: &MetaBlk) -> io::Result<()> {
        let mblk_ptr = mblk as *const MetaBlk as *const u8;
        let mblk_slice = unsafe { std::slice::from_raw_parts(mblk_ptr, self.block_size()) };
        
        if let Some(ref vdev) = self.sb_vdev {
            vdev.sync_write(mblk_slice, mblk.hdr.h.bid).await
        } else {
            Err(Error::new(ErrorKind::Other, "VirtualDev not initialized"))
        }
    }

    async fn write_meta_blk_ovf(&mut self, context_data: &[u8]) -> io::Result<BlkId> {
        // Allocate data blocks
        let sz = context_data.len();
        let block_size = self.block_size();
        let num_data_blks = (sz + block_size - 1) / block_size;
        
        let mut context_data_blkids = Vec::new();
        for _ in 0..num_data_blks {
            let bid = self.alloc_meta_blk().await?;
            context_data_blkids.push(bid);
        }

        // Allocate first overflow header block
        let out_obid = self.alloc_meta_blk().await?;
        let mut next_bid = out_obid;
        let mut offset_in_ctx = 0;
        let mut data_blkid_indx = 0;

        while next_bid.is_valid() {
            let mut ovf_buf = vec![0u8; block_size];
            let ovf_hdr = unsafe { &mut *(ovf_buf.as_mut_ptr() as *mut MetaBlkOvfHdr) };
            
            let cur_bid = next_bid;
            ovf_hdr.h.magic = META_BLK_OVF_MAGIC;
            ovf_hdr.h.bid = cur_bid;

            let max_data_blks = self.ovf_blk_max_num_data_blk();
            if context_data_blkids.len() - data_blkid_indx <= max_data_blks {
                ovf_hdr.h.next_bid = BlkId::default();
            } else {
                ovf_hdr.h.next_bid = self.alloc_meta_blk().await?;
            }
            next_bid = ovf_hdr.h.next_bid;

            // Populate data bids
            let mut data_size = 0;
            let mut nbids = 0;
            let data_bid_ptr = unsafe {
                (ovf_hdr as *mut MetaBlkOvfHdr as *mut u8)
                    .add(mem::size_of::<MetaBlkOvfHdr>()) as *mut BlkId
            };

            while nbids < max_data_blks && data_blkid_indx < context_data_blkids.len() {
                unsafe {
                    *data_bid_ptr.add(nbids) = context_data_blkids[data_blkid_indx];
                }
                data_size += block_size;
                data_blkid_indx += 1;
                nbids += 1;
            }

            ovf_hdr.h.nbids = nbids as u32;
            ovf_hdr.h.context_sz = if data_blkid_indx < context_data_blkids.len() {
                data_size as u64
            } else {
                (sz - offset_in_ctx) as u64
            };

            // Write overflow header
            if let Some(ref vdev) = self.sb_vdev {
                vdev.sync_write(&ovf_buf, cur_bid).await?;
            }

            // Write data blocks
            for i in 0..nbids {
                let data_bid = unsafe { *data_bid_ptr.add(i) };
                let data_start = offset_in_ctx + i * block_size;
                let data_end = (data_start + block_size).min(sz);
                let mut data_buf = vec![0u8; block_size];
                data_buf[..data_end - data_start].copy_from_slice(&context_data[data_start..data_end]);
                
                if let Some(ref vdev) = self.sb_vdev {
                    vdev.sync_write(&data_buf, data_bid).await?;
                }
            }

            offset_in_ctx += ovf_hdr.h.context_sz as usize;
            
            // Store overflow header
            let ovf_box = self.copy_ovf_hdr(ovf_hdr)?;
            self.ovf_blk_hdrs.insert(cur_bid.to_integer(), ovf_box);
        }

        Ok(out_obid)
    }

    async fn free_meta_blk(&mut self, mblk: &MetaBlk) -> io::Result<()> {
        if let Some(ref vdev) = self.sb_vdev {
            vdev.free_blk(mblk.hdr.h.bid).await?;
        }

        if mblk.hdr.h.ovf_bid.is_valid() {
            self.free_ovf_blk_chain(mblk.hdr.h.ovf_bid).await?;
        }

        Ok(())
    }

    async fn free_ovf_blk_chain(&mut self, mut obid: BlkId) -> io::Result<()> {
        while obid.is_valid() {
            if let Some(ovf_hdr) = self.ovf_blk_hdrs.remove(&obid.to_integer()) {
                // Free data blocks
                let data_bids = ovf_hdr.get_data_bids(self.block_size());
                for data_bid in data_bids {
                    if let Some(ref vdev) = self.sb_vdev {
                        vdev.free_blk(*data_bid).await?;
                    }
                }

                // Free overflow block
                if let Some(ref vdev) = self.sb_vdev {
                    vdev.free_blk(obid).await?;
                }

                obid = ovf_hdr.h.next_bid;
            } else {
                break;
            }
        }

        Ok(())
    }

    async fn read(&self, bid: BlkId, dest: &mut [u8]) -> io::Result<()> {
        if let Some(ref vdev) = self.sb_vdev {
            let sz = dest.len();
            let aligned_sz = round_up(sz, self.align_size());
            
            if aligned_sz != sz {
                let mut temp_buf = vec![0u8; aligned_sz];
                vdev.sync_read(&mut temp_buf, bid).await?;
                dest.copy_from_slice(&temp_buf[..sz]);
            } else {
                vdev.sync_read(dest, bid).await?;
            }
            Ok(())
        } else {
            Err(Error::new(ErrorKind::Other, "VirtualDev not initialized"))
        }
    }

    async fn read_sub_sb_internal(&self, mblk: &MetaBlk) -> io::Result<Vec<u8>> {
        let context_sz = mblk.hdr.h.context_sz as usize;
        let mut buf = vec![0u8; context_sz];

        if context_sz <= self.meta_blk_context_sz() {
            // Data is inline
            let context_data = mblk.get_context_data(self.block_size());
            buf[..context_sz].copy_from_slice(&context_data[..context_sz]);
        } else {
            // Data is in overflow blocks
            let mut read_offset = 0;
            let mut obid = mblk.hdr.h.ovf_bid;

            while obid.is_valid() && read_offset < context_sz {
                if let Some(ovf_hdr) = self.ovf_blk_hdrs.get(&obid.to_integer()) {
                    let data_bids = ovf_hdr.get_data_bids(self.block_size());
                    
                    for data_bid in data_bids {
                        let read_sz = (context_sz - read_offset).min(self.block_size());
                        let mut temp_buf = vec![0u8; self.block_size()];
                        self.read(*data_bid, &mut temp_buf).await?;
                        buf[read_offset..read_offset + read_sz].copy_from_slice(&temp_buf[..read_sz]);
                        read_offset += read_sz;
                        
                        if read_offset >= context_sz {
                            break;
                        }
                    }

                    obid = ovf_hdr.h.next_bid;
                } else {
                    break;
                }
            }
        }

        Ok(buf)
    }

    async fn recover_meta_sub_type(&mut self, do_comp_cb: bool, sub_type: &MetaSubType) -> io::Result<()> {
        if let Some(info) = self.sub_info.get(sub_type) {
            let bids: Vec<u64> = info.meta_bids.iter().copied().collect();
            
            for bid in bids {
                if let Some(mblk) = self.meta_blks.get(&bid) {
                    self.recover_meta_block(&**mblk).await?;
                }
            }

            if do_comp_cb {
                if let Some(ref comp_cb) = info.comp_cb {
                    comp_cb(true);
                }
            }
        }

        Ok(())
    }

    async fn recover_meta_block(&self, mblk: &MetaBlk) -> io::Result<()> {
        let buf = self.read_sub_sb_internal(mblk).await?;
        let type_name = mblk.hdr.h.get_type_name();

        if let Some(info) = self.sub_info.get(&type_name) {
            // Verify CRC if needed
            if info.do_crc {
                let crc = crc32(&buf);
                if crc != mblk.hdr.h.crc {
                    return Err(Error::new(
                        ErrorKind::InvalidData,
                        format!("CRC mismatch: {} != {}", crc, mblk.hdr.h.crc),
                    ));
                }
            }

            // Trigger callback
            if let Some(ref cb) = info.cb {
                cb(mblk, &buf, buf.len());
            }
        }

        Ok(())
    }

    fn topological_sort(&self, result: &mut Vec<MetaSubType>) -> io::Result<()> {
        let mut in_degree: HashMap<MetaSubType, usize> = HashMap::new();
        let mut adj_list: HashMap<MetaSubType, Vec<MetaSubType>> = HashMap::new();

        // Build adjacency list and in-degree map
        for (node, deps) in &self.dep_topo_graph {
            adj_list.entry(node.clone()).or_insert_with(Vec::new);
            in_degree.entry(node.clone()).or_insert(0);
            
            for dep in deps {
                adj_list.entry(dep.clone()).or_insert_with(Vec::new).push(node.clone());
                *in_degree.entry(node.clone()).or_insert(0) += 1;
            }
        }

        // Kahn's algorithm
        let mut queue: Vec<MetaSubType> = in_degree.iter()
            .filter(|(_, &degree)| degree == 0)
            .map(|(node, _)| node.clone())
            .collect();

        while let Some(node) = queue.pop() {
            result.push(node.clone());

            if let Some(neighbors) = adj_list.get(&node) {
                for neighbor in neighbors {
                    if let Some(degree) = in_degree.get_mut(neighbor) {
                        *degree -= 1;
                        if *degree == 0 {
                            queue.push(neighbor.clone());
                        }
                    }
                }
            }
        }

        // Check for cycles
        if result.len() != in_degree.len() {
            return Err(Error::new(
                ErrorKind::Other,
                "Circular dependency detected in meta subsystem dependencies",
            ));
        }

        Ok(())
    }

    fn copy_meta_blk(&self, mblk: &MetaBlk) -> io::Result<Box<MetaBlk>> {
        let block_size = self.block_size();
        let mut buf = vec![0u8; block_size];
        let src_ptr = mblk as *const MetaBlk as *const u8;
        unsafe {
            ptr::copy_nonoverlapping(src_ptr, buf.as_mut_ptr(), block_size);
        }
        
        Ok(unsafe { Box::from_raw(buf.as_mut_ptr() as *mut MetaBlk) })
    }

    fn copy_ovf_hdr(&self, ovf_hdr: &MetaBlkOvfHdr) -> io::Result<Box<MetaBlkOvfHdr>> {
        let block_size = self.block_size();
        let mut buf = vec![0u8; block_size];
        let src_ptr = ovf_hdr as *const MetaBlkOvfHdr as *const u8;
        unsafe {
            ptr::copy_nonoverlapping(src_ptr, buf.as_mut_ptr(), block_size);
        }
        
        Ok(unsafe { Box::from_raw(buf.as_mut_ptr() as *mut MetaBlkOvfHdr) })
    }
}

// Helper functions

fn round_up(value: usize, multiple: usize) -> usize {
    ((value + multiple - 1) / multiple) * multiple
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = INIT_CRC32;
    for &byte in data {
        crc = (crc >> 8) ^ CRC32_TABLE[((crc ^ byte as u32) & 0xff) as usize];
    }
    !crc
}

// CRC32 IEEE table (simplified for example, should use a proper CRC library in production)
const CRC32_TABLE: [u32; 256] = {
    let mut table = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut crc = i as u32;
        let mut j = 0;
        while j < 8 {
            if crc & 1 != 0 {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
            j += 1;
        }
        table[i] = crc;
        i += 1;
    }
    table
};
