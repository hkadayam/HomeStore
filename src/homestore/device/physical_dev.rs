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

use iomgr::AsyncMutex;
use std::collections::{HashMap, HashSet};
use std::io;
use std::sync::Arc;
use std::time::Instant;

use super::chunk::{Chunk, ChunkInfo, ChunkInterval, ChunkIntervalSet};
use super::device_metadata::*;
use iomgr::{DriveInterface, IOBuffer, IoDevice};

// Import Bitset from sisl
use sisl::bitset::{Bitset, NPOS};

/// Cached opened devices - global cache
static CACHED_OPENED_DEVS: once_cell::sync::Lazy<AsyncMutex<HashMap<String, Arc<IoDevice>>>> =
    once_cell::sync::Lazy::new(|| AsyncMutex::new(HashMap::new()));

/// Open and cache a device
pub async fn open_and_cache_dev(devname: String, oflags: i32) -> io::Result<Arc<IoDevice>> {
    let mut cache = CACHED_OPENED_DEVS.lock().await;

    if let Some(iodev) = cache.get(&devname) {
        return Ok(Arc::clone(iodev));
    }

    let iodev = DriveInterface::open_dev(devname.clone(), oflags).await?;

    cache.insert(devname, Arc::clone(&iodev));
    Ok(iodev)
}

/// Close and uncache a device
pub async fn close_and_uncache_dev(devname: &str, _iodev: Arc<IoDevice>) {
    let mut cache = CACHED_OPENED_DEVS.lock().await;
    cache.remove(devname);
    // Device will be closed automatically when all Arc references are dropped
}

/// Physical Device
/// Internal mutable state protected by a single mutex to avoid taking
/// multiple locks for related chunk metadata structures.
struct ChunkProvisioner {
    chunk_data_area: ChunkIntervalSet,
    chunk_info_slots: Option<Bitset>,
    chunk_start: HashSet<u64>,
    chunks: HashMap<u32, Arc<Chunk>>,  // Indexed by chunk_id for efficient VDev construction
}

pub struct PhysicalDev {
    iodev: Arc<IoDevice>,
    drive_iface: Arc<DriveInterface>,
    devname: String,
    dev_type: HSDevType,
    dev_info: DevInfo,
    pdev_info: PDevInfoHeader,
    devsize: u64,
    super_blk_in_footer: bool,
    chunk_provisioner: AsyncMutex<ChunkProvisioner>,
}

impl PhysicalDev {
    /// Build a PDevInfoHeader for a given device (helper shared with DeviceManager).
    pub fn create_pdev_info(dinfo: &DevInfo, pdev_id: u32) -> io::Result<PDevInfoHeader> {
        // Calculate sizes/placeholders
        let data_offset = 8192; // TODO: compute based on superblock layout

        let dev_attr = DiskAttr {
            phys_page_size: 512,
            align_size: 512,
            atomic_phys_page_size: 512,
            num_streams: 0,
        };

        Ok(PDevInfoHeader {
            pdev_id,
            data_offset,
            size: dinfo.dev_size,
            max_pdev_chunks: 0,         // Will be calculated properly later
            dev_attr,
            mirror_super_block: 0x00,
            system_uuid: [0u8; 16],     // Will be set properly later
        })
    }

    /// Read first block from device
    pub async fn read_first_block(drive_iface: &Arc<DriveInterface>, devname: &str, oflags: i32) -> io::Result<FirstBlock> {
        let iodev = open_and_cache_dev(devname.to_string(), oflags).await?;

        let buf = IOBuffer::new(FirstBlock::IO_FB_SIZE);
        let (res, buf) = drive_iface.read(&iodev, buf, Self::first_block_offset()).await;
        res?;

        // Parse FirstBlock from buffer
        // TODO: Properly deserialize FirstBlock from buffer using serde or zerocopy
        // For now, use ptr::read to move the value
        let first_block = unsafe { std::ptr::read(buf.as_slice().as_ptr() as *const FirstBlock) };
        Ok(first_block)
    }

    /// Get device size
    pub async fn get_dev_size(devname: &str) -> io::Result<u64> {
        let iodev = open_and_cache_dev(devname.to_string(), libc::O_RDWR | libc::O_CREAT).await?;
        DriveInterface::get_size(&iodev).await
    }

    /// Create a new PhysicalDev for formatting (first time).
    pub async fn create(dinfo: DevInfo, oflags: i32, pdev_id: u32) -> io::Result<Arc<Self>> {
        // Create new pdev_info for this device
        let pinfo = Self::create_pdev_info(&dinfo, pdev_id)?;
        
        // Create the instance using common constructor
        let pdev = Arc::new(Self::construct(dinfo, oflags, pinfo).await?);
        
        // Format chunks for first time
        pdev.format_chunks().await?;
        
        Ok(pdev)
    }

    /// Load an existing PhysicalDev from disk (recovery).
    pub async fn load(dinfo: DevInfo, oflags: i32, _pdev_id: u32) -> io::Result<Arc<Self>> {
        let drive_iface = Arc::clone(iomgr::iomgr().drive_interface());
        
        // Read first block to get existing pdev_info
        let fb = Self::read_first_block(&drive_iface, &dinfo.dev_name, oflags).await?;
        
        // Validate first block
        if !fb.is_valid() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Invalid first block for device {}", dinfo.dev_name)
            ));
        }
        
        // Use the pdev_info from the first block
        let pdev = Arc::new(Self::construct(dinfo, oflags, fb.this_pdev_hdr).await?);
        
        // Load chunks from disk
        pdev.load_chunks().await?;
        
        Ok(pdev)
    }

    /// Common constructor that takes an existing pdev_info.
    async fn construct(dinfo: DevInfo, oflags: i32, pinfo: PDevInfoHeader) -> io::Result<Self> {
        let drive_iface = Arc::clone(iomgr::iomgr().drive_interface());
        let iodev = open_and_cache_dev(dinfo.dev_name.clone(), oflags).await?;

        let dev_size = DriveInterface::get_size(&iodev).await?;
        if dev_size == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, format!("Device {} size={} is too small", dinfo.dev_name, dev_size)));
        }
        let actual_dev_size = if dinfo.dev_size == 0 { dev_size } else { std::cmp::min(dev_size, dinfo.dev_size) };

        let devsize = Self::round_down(actual_dev_size, pinfo.dev_attr.phys_page_size as u64);
        if devsize != actual_dev_size {
            println!("device size={} is not the multiple of physical page adjusted size to {}", actual_dev_size, devsize);
        }
        println!("Device {} opened with dev_id={} size={}", dinfo.dev_name, iodev.dev_id(), devsize);

        let mut dev_info_copy = dinfo.clone();
        dev_info_copy.dev_size = actual_dev_size;

        Ok(Self {
            iodev,
            drive_iface,
            devname: dinfo.dev_name,
            dev_type: dinfo.dev_type,
            dev_info: dev_info_copy,
            pdev_info: pinfo.clone(),
            devsize,
            super_blk_in_footer: pinfo.mirror_super_block != 0,
            chunk_provisioner: AsyncMutex::new(ChunkProvisioner {
                chunk_data_area: ChunkIntervalSet::new(),
                chunk_info_slots: None,
                chunk_start: HashSet::new(),
                chunks: HashMap::new(),
            }),
        })
    }

    /// Write super block using an IOBuffer (aligned or heap backed depending on platform).
    pub async fn write_super_block(&self, buf: &IOBuffer, offset: u64) -> io::Result<()> {
        let n = self.drive_iface.write(&self.iodev, buf, offset).await?;
        if n != buf.len() {
            return Err(io::Error::new(io::ErrorKind::WriteZero, "Failed to write complete super block"));
        }
        if self.super_blk_in_footer {
            let t_offset = self.data_end_offset() + offset;
            let n2 = self.drive_iface.write(&self.iodev, buf, t_offset).await?;
            if n2 != buf.len() {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "Failed to write complete super block to footer",
                ));
            }
        }
        Ok(())
    }

    /// Read super block into an IOBuffer returning (Result, IOBuffer).
    pub async fn read_super_block(&self, buf: IOBuffer, offset: u64) -> (io::Result<usize>, IOBuffer) {
        self.drive_iface.read(&self.iodev, buf, offset).await
    }
    

    /// Close device
    pub async fn close_device(&self) {
        close_and_uncache_dev(&self.devname, Arc::clone(&self.iodev)).await;
    }

    /// Write data to device
    pub async fn write(&self, buf: &IOBuffer, offset: u64) -> io::Result<()> {
        let start_time = Instant::now();
        let _size = buf.len() as u64;
        self.drive_iface.write(&self.iodev, buf, offset).await?;
        let _elapsed = start_time.elapsed().as_micros() as u64; // placeholder metrics
        Ok(())
    }

    /// Write vectored data to device
    pub async fn writev(&self, buffers: Vec<IOBuffer>, offset: u64) -> io::Result<()> {
        let start_time = Instant::now();
        let _size: u64 = buffers.iter().map(|v| v.len() as u64).sum();
        self.drive_iface.writev(&self.iodev, buffers, offset).await?;
        let _elapsed = start_time.elapsed().as_micros() as u64; // placeholder metrics
        Ok(())
    }

    /// Read data from device
    pub async fn read(&self, buf: IOBuffer, offset: u64) -> (io::Result<usize>, IOBuffer) {
        let start_time = Instant::now();
        let (res, buf) = self.drive_iface.read(&self.iodev, buf, offset).await;
        let _elapsed = start_time.elapsed().as_micros() as u64; // metrics placeholder
        (res, buf)
    }

    /// Read vectored data from device
    pub async fn readv(&self, buffers: Vec<IOBuffer>, offset: u64) -> (io::Result<usize>, Vec<IOBuffer>) {
        let start_time = Instant::now();
        let (res, buffers) = self.drive_iface.readv(&self.iodev, buffers, offset).await;
        let _elapsed = start_time.elapsed().as_micros() as u64; // metrics placeholder
        (res, buffers)
    }

    /// Write zeros to device
    pub async fn write_zero(&self, size: u64, offset: u64) -> io::Result<()> {
        self.drive_iface.write_zero(&self.iodev, size, offset).await?;
        Ok(())
    }

    /// Fsync the device
    pub async fn fsync(&self) -> io::Result<()> {
        self.drive_iface.fsync(&self.iodev).await
    }

    // Helper methods
    fn round_down(value: u64, align: u64) -> u64 {
        (value / align) * align
    }

    fn first_block_offset() -> u64 { 0 }

    pub fn data_start_offset(&self) -> u64 {
        self.pdev_info.data_offset
    }

    pub fn data_end_offset(&self) -> u64 {
        if self.super_blk_in_footer {
            self.devsize - self.pdev_info.data_offset
        } else {
            self.devsize
        }
    }

    pub fn data_size(&self) -> u64 {
        self.data_end_offset() - self.data_start_offset()
    }

    pub fn optimal_page_size(&self) -> u32 {
        self.pdev_info.dev_attr.phys_page_size
    }

    pub fn align_size(&self) -> u32 {
        self.pdev_info.dev_attr.align_size
    }

    pub fn atomic_page_size(&self) -> u32 {
        self.pdev_info.dev_attr.atomic_phys_page_size
    }

    pub fn pdev_id(&self) -> u32 {
        self.pdev_info.pdev_id
    }

    pub fn get_devname(&self) -> &str {
        &self.devname
    }

    /// Format chunks - initialize chunk info bitmap
    pub async fn format_chunks(&self) -> io::Result<()> {
        let max_chunks = self.max_chunks_in_pdev();
        let bitset = Bitset::new(std::cmp::max(1, max_chunks) as u64, 0);
        let bitmap_mem = bitset.data();
        let mut sb_buf = IOBuffer::new(bitmap_mem.len());
        sb_buf.as_mut_slice().copy_from_slice(bitmap_mem);

        let bitmap_size = self.chunk_info_bitmap_size();
        if bitmap_mem.len() > bitmap_size {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Chunk info serialized bitmap mismatch with expected size",
            ));
        }

        self.write_super_block(&sb_buf, self.chunk_sb_offset()).await?;

        let mut prov = self.chunk_provisioner.lock().await;
        prov.chunk_info_slots = Some(bitset);

        Ok(())
    }

    /// Create a single chunk
    /// 
    /// Allocates a chunk slot and computes chunk_id using formula:
    ///   chunk_id = (pdev_id + 1) * slot_number
    pub async fn create_chunk(
        self: &Arc<Self>,
        vdev_id: u32,
        size: u64,
        ordinal: u32,
        user_private: &[u8],
    ) -> io::Result<Arc<Chunk>> {
        let mut prov = self.chunk_provisioner.lock().await;
        let pdev_id = self.pdev_info.pdev_id;
        
        // Find and allocate slot
        let cslot = {
            let slots = prov
                .chunk_info_slots
                .as_mut()
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Chunk info slots not initialized"))?;

            let slot = slots.get_next_reset_bit(0);
            if slot == NPOS {
                return Err(io::Error::new(
                    io::ErrorKind::OutOfMemory,
                    "System has no room for additional chunk",
                ));
            }
            slots.set_bit(slot as u64);
            slot
        };

        // Compute chunk_id using formula: chunk_id = (pdev_id + 1) * slot_number
        let chunk_id = ((pdev_id + 1) as u64 * cslot) as u32;

        // Populate chunk info
        let mut cinfo = ChunkInfo::new();
        Self::populate_chunk_info_locked(&self, &mut prov, &mut cinfo, vdev_id, size, chunk_id, ordinal, user_private)?;

        // Write chunk info to super block
        let raw = unsafe {
            std::slice::from_raw_parts(
                &cinfo as *const ChunkInfo as *const u8,
                std::mem::size_of::<ChunkInfo>(),
            )
        };
        let mut cinfo_buf = IOBuffer::new(raw.len());
        cinfo_buf.as_mut_slice().copy_from_slice(raw);
        self.write_super_block(&cinfo_buf, self.chunk_info_offset_nth(cslot as u32))
            .await?;

        // Create chunk with reference to this physical device
        let chunk = Arc::new(Chunk::new(cinfo, cslot as u32, Arc::clone(self)));
        
        // Add to chunk HashMap indexed by chunk_id
        prov.chunks.insert(chunk_id, Arc::clone(&chunk));

        // Update bitmap
        let bitmap_mem = {
            let slots = prov.chunk_info_slots.as_ref().unwrap();
            slots.data().to_vec()
        };
        let mut sb_buf2 = IOBuffer::new(bitmap_mem.len());
        sb_buf2.as_mut_slice().copy_from_slice(&bitmap_mem);
        self.write_super_block(&sb_buf2, self.chunk_sb_offset()).await?;

        println!("Created chunk {}", chunk.to_string());

        Ok(chunk)
    }

    /// Create multiple chunks in batch
    /// 
    /// Allocates chunk slots, computes chunk IDs using formula:
    ///   chunk_id = (pdev_id + 1) * slot_number
    /// 
    /// Each chunk is assigned an ordinal (starting from `start_ordinal`) 
    /// which allows VirtualDev to sort chunks for ordered access.
    /// 
    /// # Arguments
    /// * `vdev_id` - Virtual device ID these chunks belong to
    /// * `num_chunks` - Number of chunks to create
    /// * `size` - Size of each chunk
    /// * `start_ordinal` - Starting ordinal for the first chunk (increments by 1 for each subsequent chunk)
    pub async fn create_chunks(
        self: &Arc<Self>,
        vdev_id: u32,
        num_chunks: u32,
        size: u64,
        start_ordinal: u32,
    ) -> io::Result<Vec<Arc<Chunk>>> {
        let mut ret_chunks = Vec::new();
        let mut prov = self.chunk_provisioner.lock().await;
        let mut chunks_remaining = num_chunks as usize;
        let pdev_id = self.pdev_info.pdev_id;
        let mut cur_ordinal = start_ordinal;

        while chunks_remaining > 0 {
            // Find contiguous reset bits
            let b = {
                let slots = prov
                    .chunk_info_slots
                    .as_mut()
                    .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Chunk info slots not initialized"))?;

                let bits = slots.get_next_contiguous_n_reset_bits_range(
                    0,
                    None,
                    1,
                    chunks_remaining as u32,
                );
                if bits.nbits == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::OutOfMemory,
                        "System has no room for additional chunk",
                    ));
                }
                bits
            };

            // Allocate buffer for all chunk infos in this contiguous block
            let buf_size = ChunkInfo::SIZE * b.nbits as usize;
            let mut buf = IOBuffer::new(buf_size);
            let buf_slice = buf.as_mut_slice();

            // Create chunk infos and chunks for this contiguous block
            let mut temp_chunks = Vec::new();
            for i in 0..b.nbits {
                let cslot = b.start_bit + i as u64;
                let offset = (i as usize) * ChunkInfo::SIZE;
                
                // Compute chunk_id using formula: chunk_id = (pdev_id + 1) * slot_number
                let chunk_id = ((pdev_id + 1) as u64 * cslot) as u32;
                
                // Use current ordinal and increment for next chunk
                let ordinal = cur_ordinal;
                cur_ordinal += 1;
                
                // Create and populate chunk info (includes checksum computation)
                let mut cinfo = ChunkInfo::new();
                Self::populate_chunk_info_locked(
                    &self,
                    &mut prov,
                    &mut cinfo,
                    vdev_id,
                    size,
                    chunk_id,
                    ordinal,
                    &[],
                )?;

                // Write chunk info to buffer
                let cinfo_bytes = unsafe {
                    std::slice::from_raw_parts(
                        &cinfo as *const ChunkInfo as *const u8,
                        ChunkInfo::SIZE,
                    )
                };
                buf_slice[offset..offset + ChunkInfo::SIZE].copy_from_slice(cinfo_bytes);

                // Create chunk with reference to this physical device
                let chunk = Arc::new(Chunk::new(cinfo, cslot as u32, Arc::clone(self)));
                
                temp_chunks.push(chunk);
            }

            // Set bits in the bitset
            {
                let slots = prov.chunk_info_slots.as_mut().unwrap();
                slots.set_bits(b.start_bit, b.nbits as u64);
            }

            // Write all chunk infos to super block
            self.write_super_block(&buf, self.chunk_info_offset_nth(b.start_bit as u32))
                .await?;

            // Add chunks to result, HashMap, and log
            for chunk in temp_chunks {
                let chunk_id = chunk.info().chunk_id;
                println!("Created chunk {} (slot {}, vdev {})", chunk_id, chunk.slot_number(), vdev_id);
                
                prov.chunks.insert(chunk_id, Arc::clone(&chunk));
                ret_chunks.push(chunk);
            }

            chunks_remaining -= b.nbits as usize;
        }

        // Finally serialize the entire bitset and persist the chunk info bitmap itself
        let slots = prov
            .chunk_info_slots
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "Chunk info slots not initialized"))?;
        let bitmap_mem = slots.data();
        let mut sb_buf = IOBuffer::new(bitmap_mem.len());
        sb_buf.as_mut_slice().copy_from_slice(bitmap_mem);
        self.write_super_block(&sb_buf, self.chunk_sb_offset()).await?;

        Ok(ret_chunks)
    }

    /// Load chunks from disk
    /// Load all chunks from disk and populate internal chunk HashMap
    /// Returns a map of vdev_id -> [chunk_ids] for recovery
    pub async fn load_chunks(self: &Arc<Self>) -> io::Result<HashMap<u32, Vec<Arc<Chunk>>>> {
        let mut prov = self.chunk_provisioner.lock().await;

        // Read chunk info bitmap
        let bitmap_size = self.chunk_info_bitmap_size();
        let buf = IOBuffer::new(bitmap_size);
        let (res, buf) = self.read_super_block(buf, self.chunk_sb_offset()).await;
        res?;

        // Deserialize bitset directly from IOBuffer (zero-copy)
        let (bitset, _set_count) = Bitset::load(buf).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("Failed to deserialize bitset: {}", e))
        })?;

        // Build temporary map for recovery (not stored permanently)
        let mut chunks_by_vdev: HashMap<u32, Vec<Arc<Chunk>>> = HashMap::new();
        
        let mut prev_bit = 0u64;
        loop {
            let b = bitset.get_next_set_bit(prev_bit);
            if b == NPOS {
                break;
            }

            // Read chunk info
            let cinfo_buf = IOBuffer::new(ChunkInfo::SIZE);
            let (res, cinfo_buf) =
                self.read_super_block(cinfo_buf, self.chunk_info_offset_nth(b as u32)).await;
            res?;
            let cinfo = unsafe { std::ptr::read(cinfo_buf.as_slice().as_ptr() as *const ChunkInfo) };

            // Verify checksum
            let info_crc = cinfo.checksum;
            let mut cinfo_copy = cinfo;
            cinfo_copy.checksum = 0;
            cinfo_copy.compute_checksum();
            if cinfo_copy.checksum != info_crc {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Checksum mismatch for chunk info in slot {}", b),
                ));
            }

            let chunk = Arc::new(Chunk::new(cinfo, b as u32, Arc::clone(self)));
            let chunk_id = cinfo.chunk_id;
            let vdev_id = cinfo.vdev_id;

            // Add to chunk data area
            prov.chunk_data_area
                .insert(cinfo.chunk_start_offset..cinfo.chunk_start_offset + cinfo.chunk_size);
            
            // Add to chunk HashMap indexed by chunk_id
            prov.chunks.insert(chunk_id, Arc::clone(&chunk));
            
            // Add to temporary recovery map
            chunks_by_vdev.entry(vdev_id).or_insert_with(Vec::new).push(chunk);

            prev_bit = b + 1;
        }
        prov.chunk_info_slots = Some(bitset);
        
        // Return temporary map for recovery
        Ok(chunks_by_vdev)
    }

    /// Remove a chunk
    pub async fn remove_chunk(&self, chunk: &Arc<Chunk>) -> io::Result<()> {
        let mut prov = self.chunk_provisioner.lock().await;
        let mut cinfo = *chunk.info();
        let chunk_id = cinfo.chunk_id; // Copy field to avoid unaligned reference
        
        // Remove from chunk HashMap
        prov.chunks.remove(&chunk_id);
        
        Self::free_chunk_info_locked(&mut prov, &mut cinfo);

        // Write the freed chunk info
        let raw = unsafe {
            std::slice::from_raw_parts(
                &cinfo as *const ChunkInfo as *const u8,
                std::mem::size_of::<ChunkInfo>(),
            )
        };
        let mut freed_buf = IOBuffer::new(raw.len());
        freed_buf.as_mut_slice().copy_from_slice(raw);
        self.write_super_block(&freed_buf, self.chunk_info_offset_nth(chunk.slot_number()))
            .await?;

        // Reset the info slot
        if let Some(slots) = prov.chunk_info_slots.as_mut() {
            slots.reset_bit(chunk.slot_number() as u64);

            // Write updated bitmap
            let bitmap_mem = slots.data();
            let mut sb_buf3 = IOBuffer::new(bitmap_mem.len());
            sb_buf3.as_mut_slice().copy_from_slice(bitmap_mem);
            self.write_super_block(&sb_buf3, self.chunk_sb_offset()).await?;
        }

        println!("Removed chunk {}", chunk.to_string());

        Ok(())
    }

    /// Remove multiple chunks efficiently (batches bitmap write)
    pub async fn remove_chunks(&self, chunks: &[Arc<Chunk>]) -> io::Result<()> {
        if chunks.is_empty() {
            return Ok(());
        }
        
        println!("Removing {} chunks from pdev {}", chunks.len(), self.pdev_id());
        
        let mut prov = self.chunk_provisioner.lock().await;
        
        // Process each chunk
        for chunk in chunks {
            let mut cinfo = *chunk.info();
            let chunk_id = cinfo.chunk_id;
            
            // Remove from chunk HashMap
            prov.chunks.remove(&chunk_id);
            
            // Free chunk info in memory
            Self::free_chunk_info_locked(&mut prov, &mut cinfo);
            
            // Write the freed chunk info to disk
            let raw = unsafe {
                std::slice::from_raw_parts(
                    &cinfo as *const ChunkInfo as *const u8,
                    std::mem::size_of::<ChunkInfo>(),
                )
            };
            let mut freed_buf = IOBuffer::new(raw.len());
            freed_buf.as_mut_slice().copy_from_slice(raw);
            self.write_super_block(&freed_buf, self.chunk_info_offset_nth(chunk.slot_number()))
                .await?;
            
            // Reset the info slot in bitmap (in memory only)
            if let Some(slots) = prov.chunk_info_slots.as_mut() {
                slots.reset_bit(chunk.slot_number() as u64);
            }
            
            println!("Removed chunk {} (slot {})", chunk_id, chunk.slot_number());
        }
        
        // Write bitmap once at the end (batched!)
        if let Some(slots) = &prov.chunk_info_slots {
            let bitmap_mem = slots.data();
            let mut sb_buf = IOBuffer::new(bitmap_mem.len());
            sb_buf.as_mut_slice().copy_from_slice(bitmap_mem);
            self.write_super_block(&sb_buf, self.chunk_sb_offset()).await?;
            println!("Updated chunk bitmap for pdev {}", self.pdev_id());
        }
        
        println!("Successfully removed {} chunks from pdev {}", chunks.len(), self.pdev_id());
        Ok(())
    }

    /// Remove all chunks for a specific VDev
    /// 
    /// This is used during VDev destruction and cleanup of zombied VDevs.
    /// Filters all chunks by vdev_id and removes them.
    pub async fn remove_chunks_for_vdev(&self, vdev_id: u32) -> io::Result<()> {
        // Collect chunks for this vdev by filtering
        let chunks: Vec<Arc<Chunk>> = {
            let prov = self.chunk_provisioner.lock().await;
            prov.chunks.values()
                .filter(|chunk| chunk.info().vdev_id == vdev_id)
                .cloned()
                .collect()
        };
        
        if chunks.is_empty() {
            println!("No chunks found for vdev {} on pdev {}", vdev_id, self.pdev_id());
            return Ok(());
        }
        
        println!("Removing {} chunks for vdev {} from pdev {}", 
            chunks.len(), vdev_id, self.pdev_id());
        
        // Remove chunks (this handles bitmap persistence)
        self.remove_chunks(&chunks).await?;
        
        println!("Successfully removed all chunks for vdev {} from pdev {}", vdev_id, self.pdev_id());
        Ok(())
    }
    
    /// Deactivate a chunk (for pooling)
    /// 
    /// Marks the chunk as unallocated (chunk_allocated = 0) and persists to disk.
    /// The chunk remains in the VDev but is eligible for reuse.
    /// 
    /// # Arguments
    /// Deactivate a chunk (mark as unallocated for pooling)
    /// 
    /// Updates the ChunkInfo in-place without creating a new Arc<Chunk>.
    /// 
    /// # Arguments
    /// * `chunk` - The chunk to deactivate
    pub async fn deactivate_chunk(&self, chunk: &Arc<Chunk>) -> io::Result<()> {
        let chunk_id = chunk.chunk_id();
        
        // Update ChunkInfo - mark as unallocated
        let mut chunk_info = chunk.info().clone();
        chunk_info.chunk_allocated = 0x00;
        chunk_info.compute_checksum();
        
        // Persist updated ChunkInfo to disk
        let chunk_sb_offset = self.chunk_info_offset_nth(chunk.slot_number());
        let chunk_info_data = chunk_info.to_bytes();
        let mut buf = IOBuffer::new(ChunkInfo::SIZE);
        buf.as_mut_slice().copy_from_slice(&chunk_info_data);
        self.write(&buf, chunk_sb_offset).await?;
        
        // Update the chunk's info in-place (using unsafe cell)
        chunk.update_info(chunk_info);
        
        println!("Deactivated chunk {} for pooling", chunk_id);
        Ok(())
    }
    
    /// Reactivate a chunk with a new creation order (for reuse from pool)
    /// 
    /// Marks the chunk as allocated (chunk_allocated = 1), updates its creation_order
    /// to the provided value (to maintain append semantics), and persists to disk.
    /// Returns the updated chunk.
    /// 
    /// # Arguments
    /// * `chunk_id` - ID of chunk to reactivate
    /// * `new_creation_order` - New creation order for this chunk (should be next_creation_order)
    /// Reactivate a chunk (mark as allocated with new creation_order)
    /// 
    /// Updates the ChunkInfo in-place without creating a new Arc<Chunk>.
    /// 
    /// # Arguments
    /// * `chunk` - The chunk to reactivate (from pool)
    /// * `new_creation_order` - New creation order to assign
    pub async fn reactivate_chunk(&self, chunk: &Arc<Chunk>, new_creation_order: u32) -> io::Result<()> {
        let chunk_id = chunk.chunk_id();
        
        // Update ChunkInfo - mark as allocated and update creation_order
        let mut chunk_info = chunk.info().clone();
        chunk_info.chunk_allocated = 0x01;
        chunk_info.chunk_creation_order = new_creation_order;
        chunk_info.compute_checksum();
        
        // Persist updated ChunkInfo to disk
        let chunk_sb_offset = self.chunk_info_offset_nth(chunk.slot_number());
        let chunk_info_data = chunk_info.to_bytes();
        let mut buf = IOBuffer::new(ChunkInfo::SIZE);
        buf.as_mut_slice().copy_from_slice(&chunk_info_data);
        self.write(&buf, chunk_sb_offset).await?;
        
        // Update the chunk's info in-place (using unsafe cell)
        chunk.update_info(chunk_info);
        
        println!("Reactivated chunk {} with new creation_order={}", chunk_id, new_creation_order);
        Ok(())
    }

    /// Get all chunks (for recovery/scanning)
    pub async fn get_all_chunks(&self) -> Vec<Arc<Chunk>> {
        let prov = self.chunk_provisioner.lock().await;
        prov.chunks.values().cloned().collect()
    }

    /// Get a specific chunk by chunk_id
    pub async fn get_chunk(&self, chunk_id: u32) -> Option<Arc<Chunk>> {
        let prov = self.chunk_provisioner.lock().await;
        prov.chunks.get(&chunk_id).cloned()
    }

    /// Get all chunks as a HashMap indexed by chunk_id
    pub async fn get_all_chunks(&self) -> HashMap<u32, Arc<Chunk>> {
        let prov = self.chunk_provisioner.lock().await;
        prov.chunks.clone()
    }

    /// Get chunks for a specific vdev_id (for VDev construction)
    pub async fn get_chunks_for_vdev(&self, vdev_id: u32) -> Vec<Arc<Chunk>> {
        let prov = self.chunk_provisioner.lock().await;
        prov.chunks.values()
            .filter(|chunk| chunk.info().vdev_id == vdev_id)
            .cloned()
            .collect()
    }

    /// Get the total number of chunks
    pub async fn get_chunk_count(&self) -> usize {
        let prov = self.chunk_provisioner.lock().await;
        prov.chunks.len()
    }

    fn populate_chunk_info_locked(
        &self,
        prov: &mut ChunkProvisioner,
        cinfo: &mut ChunkInfo,
        vdev_id: u32,
        size: u64,
        chunk_id: u32,
        ordinal: u32,
        private_data: &[u8],
    ) -> io::Result<()> {
        // Find free area for chunk data using in-memory state
        let ival = Self::find_next_chunk_area_locked(self, &prov.chunk_data_area, size)?;
        prov.chunk_data_area.insert(ival.clone());

        cinfo.chunk_start_offset = ival.start;
        cinfo.chunk_size = size;
        cinfo.vdev_id = vdev_id;
        cinfo.chunk_id = chunk_id;
        cinfo.chunk_creation_order = ordinal;
        cinfo.set_allocated();
        cinfo.set_user_private(private_data);
        cinfo.compute_checksum();

        if !prov.chunk_start.insert(cinfo.chunk_start_offset) {
            let offset = cinfo.chunk_start_offset;
            let id = cinfo.chunk_id;
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!("Duplicate start offset {} for chunk {}", offset, id),
            ));
        }
        Ok(())
    }

    fn free_chunk_info_locked(prov: &mut ChunkProvisioner, cinfo: &mut ChunkInfo) {
        let ival = cinfo.chunk_start_offset..cinfo.chunk_start_offset + cinfo.chunk_size;
        prov.chunk_data_area.erase(ival);
        let offset = cinfo.chunk_start_offset;
        prov.chunk_start.remove(&offset);
        cinfo.set_free();
        cinfo.checksum = 0;
        cinfo.compute_checksum();
    }

    fn find_next_chunk_area_locked(
        &self,
        data_area: &ChunkIntervalSet,
        size: u64,
    ) -> io::Result<ChunkInterval> {
        let mut ins_start = self.data_start_offset();
        let mut ins_end = ins_start + size;
        for ival in data_area.iter() {
            if ins_end <= ival.start {
                break;
            }
            ins_start = ival.end;
            ins_end = ins_start + size;
        }
        if ins_end > self.data_end_offset() {
            return Err(io::Error::new(
                io::ErrorKind::OutOfMemory,
                "Physical dev has no room for additional chunk",
            ));
        }
        Ok(ins_start..ins_end)
    }

    pub fn chunk_info_offset_nth(&self, slot: u32) -> u64 {
        self.chunk_sb_offset() + self.chunk_info_bitmap_size() as u64 + (slot as u64 * ChunkInfo::SIZE as u64)
    }

    fn chunk_sb_offset(&self) -> u64 {
        // Placeholder - should come from hs_super_blk
        4096
    }

    fn chunk_info_bitmap_size(&self) -> usize {
        // Placeholder - should calculate based on max chunks
        4096
    }

    fn max_chunks_in_pdev(&self) -> u32 {
        // Placeholder - should calculate based on device size
        1024
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iomgr::IOBuffer;
    use std::fs;
    use tokio;

    fn create_test_file(path: &str, size: u64) -> io::Result<()> {
        let f = fs::File::create(path)?;
        f.set_len(size)?;
        Ok(())
    }
    fn cleanup_test_file(path: &str) {
        let _ = fs::remove_file(path);
    }
    fn create_test_dev_info(devname: &str, dev_size: u64) -> DevInfo {
        DevInfo {
            dev_name: devname.to_string(),
            dev_size,
            dev_type: HSDevType::Data,
        }
    }
    fn create_test_pdev_info() -> PDevInfoHeader {
        PDevInfoHeader {
            pdev_id: 1,
            data_offset: 8192,
            size: 1024 * 1024 * 1024,   // 1GB placeholder
            max_pdev_chunks: 64,
            dev_attr: DiskAttr {
                phys_page_size: 4096,
                align_size: 512,
                atomic_phys_page_size: 4096,
                num_streams: 0,
            },
            mirror_super_block: 0x00,
            system_uuid: [0u8; 16],
        }
    }
    fn get_direct_io_flags() -> i32 {
        #[cfg(target_os = "linux")]
        {
            libc::O_RDWR | libc::O_DIRECT
        }
        #[cfg(not(target_os = "linux"))]
        {
            libc::O_RDWR
        }
    }

    #[tokio::test]
    async fn test_direct_io_write_variations() {
        let test_file = "/tmp/test_pdev_write_direct.dat";
        let file_size = 10 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        let write_offset = pdev.data_start_offset();
        let page_size = pdev.optimal_page_size() as usize;
        let mut wbuf = IOBuffer::new(page_size);
        for b in wbuf.as_mut_slice() {
            *b = 0xAB;
        }
        assert!(pdev.write(&wbuf, write_offset).await.is_ok());
        let bufs: Vec<IOBuffer> = [0xCDu8, 0xEFu8, 0x12u8]
            .into_iter()
            .map(|pat| {
                let mut b = IOBuffer::new(page_size);
                for v in b.as_mut_slice() {
                    *v = pat;
                }
                b
            })
            .collect();
        assert!(pdev.writev(bufs, write_offset + page_size as u64).await.is_ok());
        assert!(pdev
            .write_zero((page_size * 2) as u64, write_offset + 4 * page_size as u64)
            .await
            .is_ok());
        pdev.close_device().await;
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_direct_io_read_variations() {
        let test_file = "/tmp/test_pdev_read_direct.dat";
        let file_size = 10 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        let page_size = pdev.optimal_page_size() as usize;
        let off = pdev.data_start_offset();
        let mut w1 = IOBuffer::new(page_size);
        for b in w1.as_mut_slice() {
            *b = 0x42;
        }
        pdev.write(&w1, off).await.unwrap();
        let mut w2 = IOBuffer::new(page_size);
        for b in w2.as_mut_slice() {
            *b = 0x43;
        }
        pdev.write(&w2, off + page_size as u64).await.unwrap();
        let (rres, rbuf) = pdev.read(IOBuffer::new(page_size), off).await;
        assert!(rres.is_ok());
        assert_eq!(rbuf.as_slice()[0], 0x42);
        let (vres, vbufs) = pdev.readv((0..2).map(|_| IOBuffer::new(page_size)).collect(), off).await;
        assert!(vres.is_ok());
        assert_eq!(vbufs[0].as_slice()[0], 0x42);
        assert_eq!(vbufs[1].as_slice()[0], 0x43);
        pdev.close_device().await;
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_buffered_io_with_fsync() {
        let test_file = "/tmp/test_pdev_buffered_sync.dat";
        let file_size = 10 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            libc::O_RDWR,
            1,
        )
        .await
        .unwrap();
        let ps = pdev.optimal_page_size() as usize;
        let off = pdev.data_start_offset();
        let mut w1 = IOBuffer::new(ps);
        for b in w1.as_mut_slice() {
            *b = 0x55;
        }
        assert!(pdev.write(&w1, off).await.is_ok());
        let mut w2 = IOBuffer::new(ps);
        for b in w2.as_mut_slice() {
            *b = 0x66;
        }
        assert!(pdev.write(&w2, off + ps as u64).await.is_ok());
        assert!(pdev.fsync().await.is_ok());
        let (rres, rbuf) = pdev.read(IOBuffer::new(ps), off).await;
        assert!(rres.is_ok());
        assert_eq!(rbuf.as_slice()[0], 0x55);
        pdev.close_device().await;
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_write_close_reopen_read() {
        let test_file = "/tmp/test_pdev_persistence.dat";
        let file_size = 10 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let flags = get_direct_io_flags();
        let info = create_test_dev_info(test_file, file_size);
        {
            let pdev = PhysicalDev::create(info.clone(), flags, 1).await.unwrap();
            let ps = pdev.optimal_page_size() as usize;
            let off = pdev.data_start_offset();
            let mut p1 = IOBuffer::new(ps);
            for b in p1.as_mut_slice() {
                *b = 0xAA;
            }
            pdev.write(&p1, off).await.unwrap();
            let mut p2 = IOBuffer::new(ps);
            for b in p2.as_mut_slice() {
                *b = 0xBB;
            }
            pdev.write(&p2, off + ps as u64).await.unwrap();
            let mut p3 = IOBuffer::new(ps);
            for b in p3.as_mut_slice() {
                *b = 0xCC;
            }
            pdev.write(&p3, off + 2 * ps as u64).await.unwrap();
            pdev.fsync().await.unwrap();
            pdev.close_device().await;
            tokio::time::sleep(tokio::time::Duration::from_millis(50)).await;
        }
        tokio::time::sleep(tokio::time::Duration::from_millis(50)).await;
        {
            let pdev = PhysicalDev::create(info.clone(), flags, 1).await.unwrap();
            let ps = pdev.optimal_page_size() as usize;
            let off = pdev.data_start_offset();
            let (_, b1) = pdev.read(IOBuffer::new(ps), off).await;
            assert_eq!(b1.as_slice()[0], 0xAA);
            let (_, b2) = pdev.read(IOBuffer::new(ps), off + ps as u64).await;
            assert_eq!(b2.as_slice()[0], 0xBB);
            let (_, b3) = pdev.read(IOBuffer::new(ps), off + 2 * ps as u64).await;
            assert_eq!(b3.as_slice()[0], 0xCC);
            let (vres, vbufs) = pdev.readv((0..3).map(|_| IOBuffer::new(ps)).collect(), off).await;
            assert!(vres.is_ok());
            assert_eq!(vbufs[0].as_slice()[0], 0xAA);
            assert_eq!(vbufs[1].as_slice()[0], 0xBB);
            assert_eq!(vbufs[2].as_slice()[0], 0xCC);
            pdev.close_device();
        }
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_all_io_operations_combined() {
        let test_file = "/tmp/test_pdev_combined.dat";
        let file_size = 20 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        let ps = pdev.optimal_page_size() as usize;
        let base = pdev.data_start_offset();
        let mut d1 = IOBuffer::new(ps);
        for b in d1.as_mut_slice() {
            *b = 0x11;
        }
        pdev.write(&d1, base).await.unwrap();
        let wv: Vec<IOBuffer> = [0x22u8, 0x33u8]
            .into_iter()
            .map(|pat| {
                let mut b = IOBuffer::new(ps);
                for v in b.as_mut_slice() {
                    *v = pat;
                }
                b
            })
            .collect();
        pdev.writev(wv, base + ps as u64).await.unwrap();
        pdev.write_zero(ps as u64, base + 3 * ps as u64).await.unwrap();
        let (_, rb) = pdev.read(IOBuffer::new(ps), base).await;
        assert_eq!(rb.as_slice()[0], 0x11);
        let (vres, vbufs) = pdev.readv((0..3).map(|_| IOBuffer::new(ps)).collect(), base + ps as u64).await;
        assert!(vres.is_ok());
        assert_eq!(vbufs[0].as_slice()[0], 0x22);
        assert_eq!(vbufs[1].as_slice()[0], 0x33);
        assert_eq!(vbufs[2].as_slice()[0], 0x00);
        pdev.fsync().await.unwrap();
        pdev.close_device().await;
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_alignment_requirements() {
        let test_file = "/tmp/test_pdev_alignment.dat";
        let file_size = 10 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        assert!(pdev.optimal_page_size() > 0);
        assert!(pdev.align_size() > 0);
        assert!(pdev.optimal_page_size() >= pdev.align_size());
        let ds = pdev.data_start_offset();
        let de = pdev.data_end_offset();
        assert!(ds < de);
        assert!(ds % pdev.optimal_page_size() as u64 == 0);
        pdev.close_device();
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_format_chunks() {
        let test_file = "/tmp/test_pdev_format_chunks.dat";
        let file_size = 50 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        assert!(pdev.format_chunks().await.is_ok());
        pdev.close_device();
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_create_chunk() {
        let test_file = "/tmp/test_pdev_create_chunk.dat";
        let file_size = 50 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        pdev.format_chunks().await.unwrap();
        let chunk = pdev
            .create_chunk(100, 1024 * 1024, 0, &[0xAA, 0xBB, 0xCC, 0xDD])
            .await
            .unwrap();
        assert_eq!(chunk.chunk_id(), 1);
        // TODO: Implement stream_id() and num_streams() methods
        // assert!(chunk.stream_id() < pdev.num_streams());
        pdev.close_device();
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_create_multiple_chunks() {
        let test_file = "/tmp/test_pdev_multiple_chunks.dat";
        let file_size = 100 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        pdev.format_chunks().await.unwrap();
        let mut chunks = Vec::new();
        for i in 0..5 {
            let c = pdev
                .create_chunk(200 + i, 1024 * 1024, i, &[i as u8; 4])
                .await
                .unwrap();
            chunks.push(c);
        }
        assert_eq!(chunks.len(), 5);
        let mut ids: Vec<u32> = chunks.iter().map(|c| c.chunk_id()).collect();
        ids.sort();
        ids.dedup();
        assert_eq!(ids.len(), 5);
        pdev.close_device();
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_load_chunks() {
        let test_file = "/tmp/test_pdev_load_chunks.dat";
        let file_size = 100 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        {
            let pdev = PhysicalDev::create(
                create_test_dev_info(test_file, file_size),
                get_direct_io_flags(),
                1,
            )
            .await
            .unwrap();
            pdev.format_chunks().await.unwrap();
            for i in 0..3 {
                pdev.create_chunk(600 + i, 2 * 1024 * 1024, i, &[0x11 * (i as u8 + 1); 4])
                    .await
                    .unwrap();
            }
            pdev.fsync().await.unwrap();
            pdev.close_device().await;
            tokio::time::sleep(tokio::time::Duration::from_millis(40)).await;
        }
        {
            let pdev = PhysicalDev::create(
                create_test_dev_info(test_file, file_size),
                get_direct_io_flags(),
                1,
            )
            .await
            .unwrap();
            pdev.load_chunks().await.unwrap();
            let loaded = pdev.get_all_chunks().await;
            assert_eq!(loaded.len(), 3);
            let mut ids: Vec<u32> = loaded.keys().copied().collect();
            ids.sort();
            assert_eq!(ids, vec![500, 501, 502]);
            pdev.close_device().await;
        }
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_remove_chunk() {
        let test_file = "/tmp/test_pdev_remove_chunk.dat";
        let file_size = 50 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        pdev.format_chunks().await.unwrap();
        let chunk = pdev
            .create_chunk(800, 1024 * 1024, 0, &[0xDE, 0xAD, 0xBE, 0xEF])
            .await
            .unwrap();
        assert!(pdev.remove_chunk(&chunk).await.is_ok());
        pdev.close_device().await;
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_chunk_lifecycle() {
        let test_file = "/tmp/test_pdev_chunk_lifecycle.dat";
        let file_size = 100 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        {
            let pdev = PhysicalDev::create(
                create_test_dev_info(test_file, file_size),
                get_direct_io_flags(),
                1,
            )
            .await
            .unwrap();
            pdev.format_chunks().await.unwrap();
            let mut chunks = Vec::new();
            for i in 0..5 {
                chunks.push(
                    pdev.create_chunk(2000 + i, 1024 * 1024, i, &[i as u8; 4])
                        .await
                        .unwrap(),
                );
            }
            pdev.remove_chunk(&chunks[2]).await.unwrap();
            pdev.remove_chunk(&chunks[4]).await.unwrap();
            pdev.fsync().await.unwrap();
            pdev.close_device().await;
            tokio::time::sleep(tokio::time::Duration::from_millis(40)).await;
        }
        {
            let pdev = PhysicalDev::create(
                create_test_dev_info(test_file, file_size),
                get_direct_io_flags(),
                1,
            )
            .await
            .unwrap();
            pdev.load_chunks().await.unwrap();
            let loaded = pdev.get_all_chunks().await;
            assert_eq!(loaded.len(), 3);
            let mut ids: Vec<u32> = loaded.keys().copied().collect();
            ids.sort();
            assert_eq!(ids, vec![1000, 1001, 1003]);
            pdev.close_device();
        }
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_chunk_space_allocation() {
        let test_file = "/tmp/test_pdev_chunk_space.dat";
        let file_size = 50 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        pdev.format_chunks().await.unwrap();
        let chunk_size = 5 * 1024 * 1024;
        assert!(pdev.create_chunk(1600, chunk_size, 0, &[0x55; 4]).await.is_ok());
        assert!(pdev.create_chunk(1601, chunk_size, 1, &[0x66; 4]).await.is_ok());
        pdev.close_device();
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_create_chunks_batch() {
        let test_file = "/tmp/test_pdev_create_chunks.dat";
        let file_size = 100 * 1024 * 1024;
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        pdev.format_chunks().await.unwrap();
        
        // Create multiple chunks in batch
        let num_chunks = 5;
        let chunk_size = 5 * 1024 * 1024;
        let vdev_id = 100;
        let start_ordinal = 0;
        
        let chunks = pdev.create_chunks(vdev_id, num_chunks, chunk_size, start_ordinal).await.unwrap();
        
        // Verify all chunks were created
        assert_eq!(chunks.len(), 5);
        for (i, chunk) in chunks.iter().enumerate() {
            assert_eq!(chunk.info().chunk_creation_order, start_ordinal + i as u32);
            assert_eq!(chunk.info().vdev_id, vdev_id);
            // TODO: Implement chunk_size() method
            // assert_eq!(chunk.chunk_size(), chunk_size);
        }
        
        pdev.close_device();
        cleanup_test_file(test_file);
    }

    #[tokio::test]
    async fn test_create_chunks_out_of_space() {
        let test_file = "/tmp/test_pdev_no_space.dat";
        let file_size = 20 * 1024 * 1024;  // Small device
        create_test_file(test_file, file_size).unwrap();
        let pdev = PhysicalDev::create(
            create_test_dev_info(test_file, file_size),
            get_direct_io_flags(),
            1,
        )
        .await
        .unwrap();
        pdev.format_chunks().await.unwrap();
        
        // Try to create more chunks than can fit
        let num_chunks = 100; // 100 chunks
        let chunk_size = 5 * 1024 * 1024;
        let vdev_id = 200;
        let start_ordinal = 0;
        
        let result = pdev.create_chunks(vdev_id, num_chunks, chunk_size, start_ordinal).await;
        assert!(result.is_err());
        
        pdev.close_device();
        cleanup_test_file(test_file);
    }
}
