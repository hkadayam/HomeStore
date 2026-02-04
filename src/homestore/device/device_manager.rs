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

use std::collections::HashMap;
use std::io;
use std::sync::{Arc, Mutex};

use super::chunk::Chunk;
use super::device_metadata::*;
use super::physical_dev::PhysicalDev;
use super::virtual_dev::{VirtualDev, VDevParameters};
use sisl::bitset::Bitset;

/// Mutable state of DeviceManager protected by a single mutex
struct DeviceManagerState {
    /// All virtual devices (indexed by vdev_id - the unique identifier)
    /// Note: vdev name is for display purposes only, vdev_id is the authoritative lookup key
    all_vdevs: HashMap<u32, Arc<VirtualDev>>,
    
    /// All chunks organized by chunk_id
    chunks: HashMap<u32, Arc<Chunk>>,
    
    /// Bitmap to track VDev slot IDs
    vdev_slot_bm: Bitset,
    
    /// All physical devices indexed by pdev_id
    all_pdevs: HashMap<u32, Arc<PhysicalDev>>,
    
    /// Physical devices organized by type
    pdevs_by_type: HashMap<HSDevType, Vec<Arc<PhysicalDev>>>,
    
    /// Current physical device ID counter
    cur_pdev_id: u32,
    
    /// First block header
    first_blk_hdr: FirstBlockHeader,
    
    /// Is this the first time boot?
    first_time_boot: bool,
    
    /// Are we booting in degraded mode?
    boot_in_degraded_mode: bool,
}

/// Device Manager - manages physical and virtual devices
/// 
/// All mutable state is consolidated in `state` mutex for thread-safe sharing
pub struct DeviceManager {
    /// Device information list (immutable after construction)
    dev_infos: Vec<DevInfo>,
    
    /// Open flags for HDD devices (immutable)
    hdd_open_flags: i32,
    
    /// Open flags for SSD devices (immutable)
    ssd_open_flags: i32,
    
    /// Mutable state protected by a single mutex
    state: Arc<Mutex<DeviceManagerState>>,
}

impl DeviceManager {
    /// Create a new DeviceManager and register it to the global Managers singleton
    /// 
    /// Returns a static reference to the registered manager for convenience
    pub fn new(devs: Vec<DevInfo>) -> io::Result<&'static Self> {
        let first_time_boot = true;
        let first_blk_hdr = FirstBlockHeader::new();
        
        // Check if any device has a valid superblock
        for _dev in &devs {
            // TODO: Read first block from device
            // For now, assume first time boot
        }
        
        let hdd_open_flags = determine_open_flags(IoFlag::BufferedIo);
        let ssd_open_flags = determine_open_flags(IoFlag::DirectIo);
        
        let mgr = Self {
            dev_infos: devs,
            hdd_open_flags,
            ssd_open_flags,
            state: Arc::new(Mutex::new(DeviceManagerState {
                all_vdevs: HashMap::new(),
                chunks: HashMap::new(),
                vdev_slot_bm: Bitset::new(HsSuperBlk::MAX_VDEVS_IN_SYSTEM as u64, 0),
                all_pdevs: HashMap::new(),
                pdevs_by_type: HashMap::new(),
                cur_pdev_id: 0,
                first_blk_hdr,
                first_time_boot,
                boot_in_degraded_mode: false,
            })),
        };
        
        // Register to global Managers singleton
        crate::common::managers::Managers::init_device_mgr(mgr);
        
        // Return reference from global singleton
        Ok(crate::common::managers::device_mgr())
    }
    
    /// Check if this is the first time boot
    pub fn is_first_time_boot(&self) -> bool {
        self.state.lock().unwrap().first_time_boot
    }
    
    /// Check if booting in degraded mode
    pub fn is_boot_in_degraded_mode(&self) -> bool {
        self.state.lock().unwrap().boot_in_degraded_mode
    }
    
    /// Format all devices
    pub async fn format_devices(&self) -> io::Result<()> {
        use iomgr::IOBuffer;
        
        {
            let mut state = self.state.lock().unwrap();
            state.first_blk_hdr.gen_number += 1;
            state.first_blk_hdr.version = FirstBlockHeader::CURRENT_SUPERBLOCK_VERSION;
            state.first_blk_hdr.num_pdevs = self.dev_infos.len() as u32;
            state.first_blk_hdr.max_vdevs = MAX_VDEVS_IN_SYSTEM;
            state.first_blk_hdr.max_system_chunks = MAX_CHUNKS_IN_SYSTEM;
            state.first_blk_hdr.set_system_uuid(uuid::Uuid::new_v4());
        }
        
        // Format each physical device
        let dev_infos = self.dev_infos.clone();
        for dinfo in &dev_infos {
            let oflags = self.device_open_flags(&dinfo.dev_name);
            
            let pdev_id = {
                let mut state = self.state.lock().unwrap();
                let id = state.cur_pdev_id;
                state.cur_pdev_id += 1;
                id
            };

            let pdev_arc = PhysicalDev::create(dinfo.clone(), oflags, pdev_id).await?;
            let pdev_id = pdev_arc.pdev_id();
            
            // Add to state
            let mut state = self.state.lock().unwrap();
            state.pdevs_by_type
                .entry(dinfo.dev_type)
                .or_insert_with(Vec::new)
                .push(Arc::clone(&pdev_arc));
            state.all_pdevs.insert(pdev_id, pdev_arc);
        }
        
        // Write initial VDev slot bitmap to ALL pdevs (all zeros = no VDevs allocated)
        // Only write if we have pdevs
        if !self.state.lock().unwrap().all_pdevs.is_empty() {
            self.write_vdev_slot_bitmap().await?;
        }
        
        Ok(())
    }
    
    /// Load existing devices
    pub async fn load_devices(&self) -> io::Result<()> {
        {
            let mut state = self.state.lock().unwrap();
            let num_pdevs = state.first_blk_hdr.num_pdevs;
            if num_pdevs != self.dev_infos.len() as u32 {
                eprintln!(
                    "WARN: Homestore formatted with {} devices, but restarted with {} devices",
                    num_pdevs,
                    self.dev_infos.len()
                );
                state.boot_in_degraded_mode = true;
            }
        }
        
        // Load all physical devices
        let dev_infos = self.dev_infos.clone();
        for dinfo in &dev_infos {
            let oflags = self.device_open_flags(&dinfo.dev_name);
            
            let pdev_id = {
                let mut state = self.state.lock().unwrap();
                let id = state.cur_pdev_id;
                state.cur_pdev_id += 1;
                id
            };

            let pdev_arc = PhysicalDev::load(dinfo.clone(), oflags, pdev_id).await?;
            let pdev_id = pdev_arc.pdev_id();
            
            // Add to state
            let mut state = self.state.lock().unwrap();
            state.pdevs_by_type
                .entry(dinfo.dev_type)
                .or_insert_with(Vec::new)
                .push(Arc::clone(&pdev_arc));
            state.all_pdevs.insert(pdev_id, pdev_arc);
        }
        
        // Load all virtual devices
        self.load_vdevs().await?;
       
        Ok(())
    }
    
    /// Create a virtual device
    /// Create a new virtual device
    /// 
    /// This method:
    /// 1. Allocates a vdev_id
    /// 2. Creates the VirtualDev (which creates chunks via PhysicalDev)
    /// 3. Persists the VDev slot bitmap
    pub async fn create_vdev(&self, params: VDevParameters) -> io::Result<Arc<VirtualDev>> {
        // Get pdevs by device type
        let pdevs = self.get_pdevs_by_dev_type(params.dev_type);
        if pdevs.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("No physical devices of type {:?} available", params.dev_type)));
        }
        
        // Allocate VDev ID
        let vdev_id = self.allocate_vdev_id().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::Other,
                format!("No VDev slots available (max: {})", HsSuperBlk::MAX_VDEVS_IN_SYSTEM))
        })?;
        
        // Create the virtual device (PhysicalDev allocates chunk IDs)
        let vdev = VirtualDev::create(params, vdev_id, &pdevs).await?;
        let vdev_arc = Arc::new(vdev);
        
        // Store in map (indexed by vdev_id - the unique identifier)
        self.state.lock().unwrap().all_vdevs.insert(vdev_id, Arc::clone(&vdev_arc));
        
        // Persist VDev slot bitmap to ALL pdevs (system-wide metadata)
        self.commit_vdev_bitmap().await?;
        
        println!("Created virtual device '{}' with id {}", vdev_arc.name(), vdev_id);
        Ok(vdev_arc)
    }

    /// Load all virtual devices from super blocks using bitmap-based scanning
    async fn load_vdevs(&self) -> io::Result<()> {
        use iomgr::IOBuffer;
        
        // Get all pdevs
        let all_pdevs = {
            let state = self.state.lock().unwrap();
            state.all_pdevs.values().cloned().collect::<Vec<_>>()
        };
        
        if all_pdevs.is_empty() {
            return Err(io::Error::new(io::ErrorKind::NotFound, "No physical devices loaded"));
        }
        
        // Load chunks from all pdevs and build vdev->chunks mapping
        println!("Loading chunks from {} physical devices", all_pdevs.len());
        let mut all_vdev_chunks: HashMap<u32, Vec<Arc<Chunk>>> = HashMap::new();
        for pdev in &all_pdevs {
            let pdev_chunks = pdev.load_chunks().await?;
            println!("Loaded {} vdevs worth of chunks from pdev {}", pdev_chunks.len(), pdev.pdev_id());
            
            // Merge this pdev's chunks into the global map
            for (vdev_id, chunks) in pdev_chunks {
                all_vdev_chunks.entry(vdev_id)
                    .or_insert_with(Vec::new)
                    .extend(chunks);
            }
        }
        
        let first_pdev = &all_pdevs[0];
        
        // Read VDev slot bitmap
        let bitmap_offset = HsSuperBlk::vdev_slot_bitmap_offset();
        let bitmap_buf = IOBuffer::new(HsSuperBlk::vdev_slot_bitmap_size() as usize);
        let (res, bitmap_buf) = first_pdev.read(bitmap_buf, bitmap_offset).await;
        res?;
        
        // Load bitmap
        let (bitmap, _) = Bitset::load(bitmap_buf).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("Failed to load VDev slot bitmap: {}", e))
        })?;
        
        // Find consecutive ranges of active VDev slots
        let active_ranges = Self::find_consecutive_ranges(&bitmap);
        
        // Populate in-memory bitmap from disk (after we're done using the reference)
        {
            let mut state = self.state.lock().unwrap();
            state.vdev_slot_bm = bitmap;
        }
        
        if active_ranges.is_empty() {
            println!("No active VDev slots found in bitmap");
            return Ok(());
        }
        
        println!("Found {} active VDev range(s) to load", active_ranges.len());
        
        // Track zombied VDevs (marked free but bitmap bit still set)
        let mut zombied_vdev_ids: Vec<u32> = Vec::new();
        
        // Track loaded VDev IDs for orphan chunk detection
        let mut loaded_vdev_ids: std::collections::HashSet<u32> = std::collections::HashSet::new();
        
        // Process each range with batched I/O
        for (range_start, range_end) in active_ranges {
            let num_slots = range_end - range_start + 1;
            let offset = VDevInfo::vdev_sb_offset(range_start);
            let size = (num_slots as usize) * VDevInfo::SIZE;
            
            // Read all VDevInfo structs in this range with a single I/O
            let batch_buf = IOBuffer::new(size);
            let (res, batch_buf) = first_pdev.read(batch_buf, offset).await;
            res?;
            
            // Deserialize and load each VDevInfo in the range
            for slot_offset in 0..num_slots {
                let vdev_id = range_start + slot_offset;
                let buf_offset = (slot_offset as usize) * VDevInfo::SIZE;
                
                let vdev_info = unsafe {
                    std::ptr::read(batch_buf.as_slice()[buf_offset..].as_ptr() as *const VDevInfo)
                };
                
                // Check if this is a zombied VDev (marked free but bitmap bit set)
                if !vdev_info.is_allocated() {
                    println!("Found zombied VDev id={} (marked free but bitmap bit set)", vdev_id);
                    zombied_vdev_ids.push(vdev_id);
                    continue;
                }
                
                println!("Loading VirtualDev id={} name={}", vdev_id, vdev_info.get_name());
                
                // Load VirtualDev from vdev_info
                let mut vdev = VirtualDev::load(vdev_info);
                
                // Notify VDev of all its chunks
                if let Some(chunks) = all_vdev_chunks.get(&vdev_id) {
                    for chunk in chunks {
                        vdev.on_chunk_found(Arc::clone(chunk))?;
                    }
                }
                
                // Update runtime stats (vdev_size, num_chunks) from actual chunks
                // ChunkInfo is the single source of truth!
                vdev.adjust_vdev_info();
                
                // Initialize chunk selector after all chunks are added
                vdev.create_chunk_selector();
                
                // Load block allocators for all chunks (recovery path)
                // TODO: Load from persisted metablks if available
                vdev.load_blk_allocator(None)?;
                
                // Store the vdev (indexed by vdev_id - the unique identifier)
                self.state.lock().unwrap().all_vdevs.insert(vdev_id, Arc::new(vdev));
                
                // Track loaded VDev IDs for orphan detection
                loaded_vdev_ids.insert(vdev_id);
            }
        }
        
        // Cleanup zombied VDevs if any were found
        if !zombied_vdev_ids.is_empty() {
            println!("Cleaning up {} zombied VDev(s)", zombied_vdev_ids.len());
            self.cleanup_zombied_vdevs(&zombied_vdev_ids).await?;
        }
        
        // Detect and cleanup orphaned chunks (chunks whose VDev wasn't loaded)
        // This handles crashes during VDev creation before bitmap was committed
        let mut orphaned_vdev_ids: Vec<u32> = Vec::new();
        for &vdev_id in all_vdev_chunks.keys() {
            if !loaded_vdev_ids.contains(&vdev_id) {
                orphaned_vdev_ids.push(vdev_id);
            }
        }
        
        if !orphaned_vdev_ids.is_empty() {
            println!("Found {} VDev(s) with orphaned chunks (crashed during creation)", orphaned_vdev_ids.len());
            for &vdev_id in &orphaned_vdev_ids {
                let chunk_count = all_vdev_chunks.get(&vdev_id).map(|v| v.len()).unwrap_or(0);
                println!("Cleaning up {} orphaned chunks for vdev_id={}", chunk_count, vdev_id);
                
                // Remove all chunks for this vdev_id from all pdevs
                // ChunkInfo is deleted, so these chunks disappear permanently
                for pdev in &all_pdevs {
                    pdev.remove_chunks_for_vdev(vdev_id).await?;
                }
            }
            println!("Successfully cleaned up orphaned chunks for {} VDev(s)", orphaned_vdev_ids.len());
        }
        
        println!("Loaded {} virtual device(s)", self.state.lock().unwrap().all_vdevs.len());
        Ok(())
    }
    
    /// Legacy method to load VDevs by scanning all slots (fallback)
    async fn load_vdevs_legacy(&self) -> io::Result<()> {
        // Get first pdev
        let first_pdev = {
            let state = self.state.lock().unwrap();
            state.all_pdevs.values().next().cloned()
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "No physical devices loaded"))?
        };
        
        for vdev_id in 0..HsSuperBlk::MAX_VDEVS_IN_SYSTEM {
            let vdev_info = Self::read_vdev_info(&first_pdev, vdev_id).await?;
            if !vdev_info.is_allocated() {
                continue;
            }
            
            println!("Loading VirtualDev id={} name={}", vdev_id, vdev_info.get_name());
            
            // Load VirtualDev from vdev_info
            let mut vdev = VirtualDev::load(vdev_info);
            
            // Collect all chunks for this vdev from all pdevs
            let all_pdevs = {
                let state = self.state.lock().unwrap();
                state.all_pdevs.values().cloned().collect::<Vec<_>>()
            };
            
            for pdev in &all_pdevs {
                let chunks = pdev.get_chunks_for_vdev(vdev_id).await;
                for chunk in chunks {
                    vdev.on_chunk_found(chunk)?;
                }
            }
            
            // Initialize chunk selector after all chunks are added
            vdev.create_chunk_selector();
            
            // Store the vdev (indexed by vdev_id - the unique identifier)
            self.state.lock().unwrap().all_vdevs.insert(vdev_id, Arc::new(vdev));
        }
        
        println!("Loaded {} virtual device(s)", self.state.lock().unwrap().all_vdevs.len());
        Ok(())
    }
    
    /// Find consecutive ranges of set bits in the bitmap efficiently
    /// Returns Vec<(start_slot, end_slot)> for batched I/O
    fn find_consecutive_ranges(bitmap: &Bitset) -> Vec<(u32, u32)> {
        use sisl::bitset::NPOS;
        
        let mut ranges = Vec::new();
        let max_slots = std::cmp::min(bitmap.size(), HsSuperBlk::MAX_VDEVS_IN_SYSTEM as u64);
        let mut current_bit = 0u64;
        
        loop {
            // Find next set bit (start of a range)
            current_bit = bitmap.get_next_set_bit(current_bit);
            if current_bit == NPOS || current_bit >= max_slots {
                break;
            }
            
            let range_start = current_bit as u32;
            
            // Find next reset bit (end of the range)
            let next_reset = bitmap.get_next_reset_bit(current_bit + 1);
            let range_end = if next_reset == NPOS || next_reset > max_slots {
                (max_slots - 1) as u32
            } else {
                (next_reset - 1) as u32
            };
            
            ranges.push((range_start, range_end));
            
            // Move to the bit after the current range
            current_bit = (range_end as u64) + 1;
            if current_bit >= max_slots {
                break;
            }
        }
        
        ranges
    }
    
    /// Read VDevInfo from a physical device's super block
    async fn read_vdev_info(pdev: &Arc<PhysicalDev>, vdev_id: u32) -> io::Result<VDevInfo> {
        use iomgr::IOBuffer;
        
        let offset = VDevInfo::vdev_sb_offset(vdev_id);
        let buf = IOBuffer::new(VDevInfo::SIZE);
        let (res, buf) = pdev.read(buf, offset).await;
        res?;
        
        // Deserialize VDevInfo from buffer
        let vdev_info = unsafe {
            std::ptr::read(buf.as_slice().as_ptr() as *const VDevInfo)
        };
        
        Ok(vdev_info)
    }
       
    /// Close all devices
    pub async fn close_devices(&self) {
        let all_pdevs = {
            let state = self.state.lock().unwrap();
            state.all_pdevs.values().cloned().collect::<Vec<_>>()
        };
        for pdev in &all_pdevs {
            pdev.close_device().await;
        }
    }
    
    /// Get physical device by ID
    pub fn get_pdev(&self, pdev_id: u32) -> Option<Arc<PhysicalDev>> {
        let state = self.state.lock().unwrap();
        state.all_pdevs.get(&pdev_id).cloned()
    }
    
    /// Get physical devices by type
    pub fn get_pdevs_by_dev_type(&self, dtype: HSDevType) -> Vec<Arc<PhysicalDev>> {
        self.state.lock().unwrap().pdevs_by_type.get(&dtype).cloned().unwrap_or_default()
    }
    
    /// Get total capacity
    pub fn total_capacity(&self) -> u64 {
        let state = self.state.lock().unwrap();
        state.all_pdevs
            .values()
            .map(|pdev| pdev.data_size())
            .sum()
    }
    
    /// Get total capacity for a specific device type
    pub fn total_capacity_by_type(&self, dtype: HSDevType) -> u64 {
        let state = self.state.lock().unwrap();
        state.pdevs_by_type
            .get(&dtype)
            .map(|pdevs| {
                pdevs
                    .iter()
                    .map(|pdev| pdev.data_size())
                    .sum()
            })
            .unwrap_or(0)
    }
    
    /// Get atomic page size for a device type
    pub fn atomic_page_size(&self, dtype: HSDevType) -> u32 {
        self.pdevs_by_type_internal(dtype)
            .first()
            .map(|pdev| pdev.atomic_page_size())
            .unwrap_or(512)
    }
    
    /// Get optimal page size for a device type
    pub fn optimal_page_size(&self, dtype: HSDevType) -> u32 {
        self.pdevs_by_type_internal(dtype)
            .first()
            .map(|pdev| pdev.optimal_page_size())
            .unwrap_or(4096)
    }
    
    /// Get align size for a device type
    pub fn align_size(&self, dtype: HSDevType) -> u32 {
        self.pdevs_by_type_internal(dtype)
            .first()
            .map(|pdev| pdev.align_size())
            .unwrap_or(512)
    }
    
    /// Get chunk by ID
    pub fn get_chunk(&self, chunk_id: u32) -> Option<Arc<Chunk>> {
        self.state.lock().unwrap().chunks.get(&chunk_id).cloned()
    }
    
    /// Get virtual device by ID (the authoritative lookup method)
    pub fn get_vdev(&self, vdev_id: u32) -> Option<Arc<VirtualDev>> {
        self.state.lock().unwrap().all_vdevs.get(&vdev_id).map(Arc::clone)
    }
    
    /// Get all physical devices
    pub fn get_all_pdevs(&self) -> Vec<Arc<PhysicalDev>> {
        self.state.lock().unwrap().all_pdevs.values().cloned().collect()
    }
    
    /// Allocate a VDev slot ID
    /// 
    /// Returns the allocated vdev_id, or None if no slots available
    pub fn allocate_vdev_id(&self) -> Option<u32> {
        let mut state = self.state.lock().unwrap();
        let id = state.vdev_slot_bm.get_next_reset_bit(0);
        if id == sisl::bitset::NPOS {
            None
        } else {
            state.vdev_slot_bm.set_bit(id);
            Some(id as u32)
        }
    }
    
    /// Free a VDev slot ID (in-memory only, call commit_vdev_bitmap to persist)
    pub fn free_vdev_id(&self, vdev_id: u32) {
        let mut state = self.state.lock().unwrap();
        state.vdev_slot_bm.reset_bit(vdev_id as u64);
    }
    
    /// Persist VDev slot bitmap to ALL physical devices
    pub async fn commit_vdev_bitmap(&self) -> io::Result<()> {
        self.write_vdev_slot_bitmap().await
    }
    
    /// Expand a virtual device with a new chunk for a stream
    /// 
    /// # Arguments
    /// * `vdev` - Virtual device to expand (must be Dynamic)
    /// * `stream_id` - Stream ID to assign to the new chunk (must be non-zero)
    /// * `chunk_size` - Size of the new chunk
    /// Expand a VDev with a new chunk
    /// 
    /// PhysicalDev allocates the chunk_id.  /// VirtualDev persists updated VDevInfo.
    /// Expand a virtual device by adding a new chunk
    ///
       
    /// Create physical device info
    /// Get device open flags
    fn device_open_flags(&self, _devname: &str) -> i32 {
        // TODO: Check if HDD or SSD
        self.ssd_open_flags
    }
    
    /// Internal helper to get pdevs by type
    fn pdevs_by_type_internal(&self, dtype: HSDevType) -> Vec<Arc<PhysicalDev>> {
        let state = self.state.lock().unwrap();
        state.pdevs_by_type
            .get(&dtype)
            .or_else(|| state.pdevs_by_type.get(&HSDevType::Data))
            .cloned()
            .unwrap_or_default()
    }
    
    /// Write VDev slot bitmap to physical device
    /// Write VDev slot bitmap to ALL physical devices (system-wide metadata)
    /// 
    /// The VDev slot bitmap is mirrored across all pdevs for redundancy.
    async fn write_vdev_slot_bitmap(&self) -> io::Result<()> {
        use iomgr::IOBuffer;
        use futures::future::join_all;
        
        let bitmap_offset = HsSuperBlk::vdev_slot_bitmap_offset();
        let bitmap_size = HsSuperBlk::vdev_slot_bitmap_size() as usize;
        
        // Get all pdevs and bitmap data
        let (all_pdevs, bitmap_data) = {
            let state = self.state.lock().unwrap();
            let pdevs = state.all_pdevs.values().cloned().collect::<Vec<_>>();
            let data = state.vdev_slot_bm.buffer().as_slice().to_vec();
            (pdevs, data)
        };
        
        if all_pdevs.is_empty() {
            return Err(io::Error::new(io::ErrorKind::NotFound, "No physical devices available"));
        }
        
        println!("Writing VDev slot bitmap to ALL {} physical devices", all_pdevs.len());
        
        // Write to ALL pdevs in parallel
        let writes: Vec<_> = all_pdevs.iter().map(|pdev| {
            let bitmap_data = bitmap_data.clone();
            async move {
                // Create IOBuffer and copy bitmap data
                let mut buf = IOBuffer::new(bitmap_size);
                let copy_len = std::cmp::min(bitmap_data.len(), bitmap_size);
                buf.as_mut_slice()[..copy_len].copy_from_slice(&bitmap_data[..copy_len]);
                
                // Write to disk (PhysicalDev::write takes &IOBuffer)
                pdev.write(&buf, bitmap_offset).await
            }
        }).collect();
        
        join_all(writes).await.into_iter().collect::<io::Result<Vec<()>>>()?;
        
        Ok(())
    }
    
    /// Cleanup zombied VDevs (interrupted destroys detected during recovery)
    /// 
    /// A zombied VDev has:
    /// - VDevInfo.slot_allocated == 0 (marked as free)
    /// - VDev slot bitmap bit == 1 (still allocated)
    /// - Potentially orphaned chunks on physical devices
    /// 
    /// PhysicalDev handles all chunk cleanup internally.
    async fn cleanup_zombied_vdevs(&self, zombied_vdev_ids: &[u32]) -> io::Result<()> {
        println!("Starting cleanup of {} zombied VDev(s)", zombied_vdev_ids.len());
        
        // Get all pdevs
        let all_pdevs = {
            let state = self.state.lock().unwrap();
            state.all_pdevs.values().cloned().collect::<Vec<_>>()
        };
        
        for &vdev_id in zombied_vdev_ids {
            println!("Cleaning up zombied VDev id={}", vdev_id);
            
            // For each pdev, remove all chunks belonging to this vdev_id
            // PhysicalDev handles: ChunkInfo cleanup, chunk bitmap updates, vdev_chunks map cleanup
            for pdev in &all_pdevs {
                pdev.remove_chunks_for_vdev(vdev_id).await?;
            }
            
            // Free the VDev slot (in memory)
            self.free_vdev_id(vdev_id);
            
            println!("Completed cleanup of zombied VDev id={}", vdev_id);
        }
        
        // Persist updated VDev slot bitmap to ALL pdevs (system metadata)
        if !zombied_vdev_ids.is_empty() {
            self.commit_vdev_bitmap().await?;
        }
        
        println!("Successfully cleaned up {} zombied VDev(s)", zombied_vdev_ids.len());
        Ok(())
    }

    /// Called by VirtualDev::destroy() to notify DeviceManager
    /// 
    /// Removes VDev from registry, frees vdev_id, and commits bitmap.
    /// This is the final stage of VDev destruction.
    /// 
    /// If this fails, VDev is already destroyed on disk - recovery will handle cleanup.
    pub async fn on_vdev_destroyed(&self, vdev_id: u32) -> io::Result<()> {
        println!("DeviceManager: Finalizing VDev {} destruction", vdev_id);
        
        // Remove from registry
        {
            let mut state = self.state.lock().unwrap();
            state.all_vdevs.remove(&vdev_id);
        }
        
        // Free VDev slot and persist bitmap to ALL pdevs (system metadata)
        self.free_vdev_id(vdev_id);
        self.commit_vdev_bitmap().await?;
        
        println!("DeviceManager: VDev {} removed from registry and bitmap committed", vdev_id);
        Ok(())
    }
}

/// IO flag enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoFlag {
    BufferedIo,
    ReadOnly,
    DirectIo,
}

/// Determine open flags based on IO flag
fn determine_open_flags(oflags: IoFlag) -> i32 {
    use libc::{O_CREAT, O_RDONLY, O_RDWR};
    
    match oflags {
        IoFlag::BufferedIo => O_RDWR | O_CREAT,
        IoFlag::ReadOnly => O_RDONLY,
        IoFlag::DirectIo => {
            #[cfg(target_os = "linux")]
            {
                use libc::O_DIRECT;
                O_RDWR | O_CREAT | O_DIRECT
            }
            #[cfg(not(target_os = "linux"))]
            {
                O_RDWR | O_CREAT
            }
        }
    }
}

// Constants for super block calculations
pub const MAX_VDEVS_IN_SYSTEM: u32 = 65536;
pub const MAX_CHUNKS_IN_SYSTEM: u32 = 16777216; // 16M chunks

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_first_block_header() {
        let hdr = FirstBlockHeader::new();
        
        // Copy packed fields to avoid unaligned references
        let version = hdr.version;
        let product_name = hdr.product_name;
        
        assert_eq!(version, FirstBlockHeader::CURRENT_SUPERBLOCK_VERSION);
        
        // Compare product_name byte array with string
        let product_name_str = std::str::from_utf8(&product_name)
            .unwrap()
            .trim_end_matches('\0');
        assert_eq!(product_name_str, "HomeStore4x");
    }
    
    #[test]
    fn test_determine_open_flags() {
        let buffered = determine_open_flags(IoFlag::BufferedIo);
        let readonly = determine_open_flags(IoFlag::ReadOnly);
        let direct = determine_open_flags(IoFlag::DirectIo);
        
        assert_ne!(buffered, 0);
        assert_ne!(readonly, 0);
        assert_ne!(direct, 0);
    }
}
