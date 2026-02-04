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
use std::sync::Arc;
use std::cell::UnsafeCell;

use iomgr::{IOBuffer, AsyncMutex};
use crate::common::BlkId;
use crate::device::{
    VirtualDev, VDevParameters, MultiPDevOpts, HSDevType, 
    BlkAllocatorType, ChunkSelectorType,
};
use super::meta_client::{MetaClient, MetaClientInfo};
use super::meta_client::MAX_META_CLIENTS;
//
// ================ Constants ================
//
////////// Meta Block Manager Constants //////////
pub const META_SUPER_HEADER_MAGIC: u32 = 0xABCD9876; /// Magic number for meta super header
pub const META_SUPER_HEADER_SIZE: usize = 512; /// Size of MetaSuperHeader structure
pub const META_SUPER_HEADER_VERSION: u32 = 0x1; /// Version of the meta super header format

////////// Meta Block Constants //////////
pub const META_BLK_HEADER_MAGIC: u32 = 0xABCD9876; /// Magic number for meta block header
pub const META_BLK_HEADER_SIZE: usize = 64; /// Size of MetaBlk header

//
// ================ Structures ================
//

/// Meta block super header (first 512-byte block on meta vdev)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct MetaBlkSuperHeader {
    pub magic: u32,                  // Magic number to identify valid header (8 bytes)
    pub version: u32,                // Version of the metadata format (4 bytes)
    pub padding: [u8; 504],          // Padding to make exactly 512 bytes (8+4+500=512)
}

impl MetaBlkSuperHeader {
    pub const SIZE: usize = 512;
    
    pub fn new() -> Self {
        Self {
            magic: META_SUPER_HEADER_MAGIC,
            version: META_SUPER_HEADER_VERSION,
            padding: [0u8; 504],
        }
    }
    
    pub fn is_valid(&self) -> bool {
        self.magic == META_SUPER_HEADER_MAGIC && self.version == META_SUPER_HEADER_VERSION
    }
}

//
// ================ Meta Block Manager ================
//

// const _: () = assert!(std::mem::size_of::<MetaBlkSuperHeader>() == MetaBlkSuperHeader::SIZE);

/// Meta Block Manager - manages metadata blocks for all clients
pub struct MetaBlkManager {
    /// Meta vdev for storing metadata
    meta_vdev: Arc<VirtualDev>,
    
    /// Block ID for the client info area (64 blocks of 512 bytes each)
    client_info_bid: BlkId,
    
    /// Bitmap tracking which client slots are allocated (0 = free, 1 = allocated)
    /// 256 slots = 256 bytes (one byte per slot for simplicity)
    /// Protected by mgmt_mutex
    client_slots: UnsafeCell<Vec<u8>>,
    
    /// Recovered clients from load (pending registration)
    /// Protected by mgmt_mutex
    recovered_clients: UnsafeCell<HashMap<String, MetaClientInfo>>,
    
    /// AsyncMutex for thread-safe async operations
    mgmt_mutex: Arc<AsyncMutex<()>>,
}

// SAFETY: MetaBlkManager is Sync because:
// - client_slots and recovered_clients are UnsafeCell but protected by mgmt_mutex
// - All other fields (meta_vdev, client_info_bid, mgmt_mutex) are Sync
unsafe impl Sync for MetaBlkManager {}

impl MetaBlkManager {
    /// Create and format a new MetaBlkManager and register it to the global Managers singleton
    /// 
    /// Returns a static reference to the registered manager for convenience
    pub async fn create(vdev_size: u64) -> io::Result<&'static Self> {
        let vdev_params = VDevParameters {
            vdev_name: "meta_vdev".to_string(),
            vdev_size,
            num_mirrors: 0,
            blk_size: 512, // 512 byte blocks
            num_chunks: 1, 
            chunk_size: 0,  // Let VDev calculate
            dev_type: HSDevType::Fast,
            multi_pdev_opts: MultiPDevOpts::AllPDevStriped,
            alloc_type: BlkAllocatorType::Varsize,
            chunk_sel_type: ChunkSelectorType::OnlyOne,
        };
        
        // Create meta vdev and format it. We need formatting because we rely on zeros during recovery.
        let vdev = crate::common::managers::device_mgr().create_vdev(vdev_params).await?;
        
        // Initialize block allocators (must be called explicitly after create)
        vdev.init_blk_allocator(None)?;
        
        vdev.format().await?;
        
        // Calculate size needed for all client info slots and round up to 512 byte boundary
        let total_meta_size = MetaBlkSuperHeader::SIZE + (MAX_META_CLIENTS * MetaClientInfo::SIZE);
        let blk_size = vdev.block_size() as usize;
        let nblks = ((total_meta_size + blk_size - 1) / blk_size) as u16; // number of blocks needed
        
        // Get first chunk ID from vdev and create BlkId for super header area (starts at block 0)
        let chunks = vdev.get_chunks();
        if chunks.is_empty() {
            return Err(io::Error::new(io::ErrorKind::Other, "Meta vdev has no chunks"));
        }
        let client_info_bid = BlkId::new(0, nblks, chunks[0].chunk_id() as u16);
        vdev.commit_blk(&client_info_bid);         // Commit this block range so it's reserved
      
        let mgr = Self {
            meta_vdev: Arc::clone(&vdev),
            client_info_bid,
            client_slots: UnsafeCell::new(vec![0u8; MAX_META_CLIENTS]),  // All slots initially free
            recovered_clients: UnsafeCell::new(HashMap::new()),
            mgmt_mutex: Arc::new(AsyncMutex::new(())),
        };
        
        // Allocate a single buffer for the super header and all client info slots
        let mut buf = IOBuffer::new(MetaBlkSuperHeader::SIZE + (MetaClientInfo::SIZE * MAX_META_CLIENTS));

        // Serialize the super header into the buffer
        let super_header = MetaBlkSuperHeader::new();
        let super_header_bytes = unsafe {
            std::slice::from_raw_parts(
                &super_header as *const MetaBlkSuperHeader as *const u8,
                MetaBlkSuperHeader::SIZE)
        };
        buf.as_mut_slice()[..MetaBlkSuperHeader::SIZE].copy_from_slice(super_header_bytes);

        // Serialize each client info slot into the buffer
        for slot in 0..MAX_META_CLIENTS {
            let client_info = MetaClientInfo::new();
            // Convert struct to bytes and copy to buffer
            let bytes = unsafe {
                std::slice::from_raw_parts(
                    &client_info as *const MetaClientInfo as *const u8,
                    MetaClientInfo::SIZE)
            };
            buf.as_mut_slice()[(slot + 1) * MetaClientInfo::SIZE..(slot + 2) * MetaClientInfo::SIZE].copy_from_slice(bytes);
        }

        // Write the buffer to the meta vdev
        vdev.write(&buf, &client_info_bid).await?;
        println!("MetaBlkManager created with {} client slots in {} blocks ({} bytes per client)",
                 MAX_META_CLIENTS, nblks, MetaClientInfo::SIZE);
        
        // Register to global Managers singleton
        crate::common::managers::Managers::instance().init_metablk_mgr(mgr);
        
        // Return reference from global singleton
        Ok(crate::common::managers::metablk_mgr())
    }
    
    /// Load existing meta vdev and register it to the global Managers singleton
    /// 
    /// Returns a static reference to the registered manager for convenience
    pub async fn load() -> io::Result<&'static Self> {
        let vdev_name = "meta_vdev";
        
        // Load meta vdev through device manager
        let vdev = crate::common::managers::device_mgr().get_vdev(vdev_name)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound, format!("Meta vdev '{}' not found", vdev_name)))?;
        
        // Reconstruct client_info_bid (same calculation as in create)
        let total_meta_size = MetaBlkSuperHeader::SIZE + MAX_META_CLIENTS * MetaClientInfo::SIZE;
        let blk_size = vdev.block_size() as usize;
        let nblks = ((total_meta_size + blk_size - 1) / blk_size) as u16;
        
        let chunks = vdev.get_chunks();
        if chunks.is_empty() {
            return Err(io::Error::new(io::ErrorKind::Other, "Meta vdev has no chunks"));
        }
        let first_chunk_id = chunks[0].chunk_id();
        let client_info_bid = BlkId::new(0, nblks, first_chunk_id as u16);
        
        let mgr = Self {
            meta_vdev: Arc::clone(&vdev),
            client_info_bid,
            client_slots: UnsafeCell::new(vec![0u8; MAX_META_CLIENTS]),
            recovered_clients: UnsafeCell::new(HashMap::new()),
            mgmt_mutex: Arc::new(AsyncMutex::new(())),
        };
        
        // Read and validate all client info slots from disk
        mgr.load_client_info_from_disk().await?;
        
        let allocated_count = unsafe { (*mgr.client_slots.get()).iter().filter(|&&s| s == 1).count() };
        let recovered_count = unsafe { (*mgr.recovered_clients.get()).len() };
        println!("MetaBlkManager loaded with {} allocated client slots, {} recovered clients", 
                 allocated_count, recovered_count);
        
        // Register to global Managers singleton
        crate::common::managers::Managers::instance().init_metablk_mgr(mgr);
        
        // Return reference from global singleton
        Ok(crate::common::managers::metablk_mgr())
    }
    
    /// Register a client
    pub async fn register_client(
        &self,
        client_name: String,
    ) -> io::Result<MetaClient> {
        // Check recovery and reserve slot
        let (client_id, is_recovery, recovered_info) = {
            let _guard = self.mgmt_mutex.lock().await;
            
            // SAFETY: Protected by mgmt_mutex
            unsafe {
                let recovered_clients = &mut *self.recovered_clients.get();
                let client_slots = &mut *self.client_slots.get();
                
                if let Some(info) = recovered_clients.remove(&client_name) {
                    let client_id = info.client_id;
                    client_slots[client_id as usize] = 1;  // Mark slot as allocated
                    (client_id, true, Some(info))
                } else {
                    let slot = Self::reserve_slot_internal(client_slots)?;
                    (slot as u8, false, None)
                }
            }
        };  // _guard automatically dropped here
        
        // Create/load client (no lock held)
        let client = if is_recovery {
            // Load from disk
            MetaClient::load(
                recovered_info.unwrap(),
                Arc::clone(&self.meta_vdev),
            ).await?
        } else {
            // Create fresh
            MetaClient::create(
                client_name.clone(),
                client_id,
                Arc::clone(&self.meta_vdev),
            ).await?
        };
        
        println!("Registered client '{}' (id={}, recovery={})", client_name, client_id, is_recovery);
        Ok(client)
    }
    
    /// Deregister a client (free its slot in the client info area)
    pub async fn deregister_client(&self, client: &MetaClient) -> io::Result<()> {
        let _guard = self.mgmt_mutex.lock().await;
        
        let client_id = client.client_id().await;
        let client_name = client.client_name().await;
        
        // Free the slot
        // SAFETY: Protected by mgmt_mutex
        unsafe {
            let client_slots = &mut *self.client_slots.get();
            client_slots[client_id as usize] = 0;
        }
        
        // Mark client info as free on disk
        let nsuper_hdr_blks = (META_SUPER_HEADER_SIZE - 1) / self.meta_vdev.block_size() as usize + 1;
        let info_nblks = (MetaClientInfo::SIZE - 1) / self.meta_vdev.block_size() as usize + 1;
        let info_bid = BlkId::new(((client_id as usize * info_nblks) + nsuper_hdr_blks) as u32, 
                                  info_nblks as u16, 
                                  self.meta_vdev.get_chunks()[0].chunk_id() as u16);
        
        let mut info = MetaClientInfo::new();
        info.set_free();
        info.update_crc();
        
        let mut buf = IOBuffer::new(MetaClientInfo::SIZE);
        let info_bytes = unsafe {
            std::slice::from_raw_parts(
                &info as *const MetaClientInfo as *const u8,
                MetaClientInfo::SIZE)
        };
        buf.as_mut_slice().copy_from_slice(info_bytes);
        self.meta_vdev.write(&buf, &info_bid).await?;
        
        println!("Deregistered client '{}' (id={})", client_name, client_id);
        Ok(())
    }
    
    
    //
    // ================ Internal Helper Methods ================
    //
    
    /// Load and validate all client info from disk during recovery
    async fn load_client_info_from_disk(&self) -> io::Result<()> {
        // Read the entire client info area
        let total_size = MetaBlkSuperHeader::SIZE + (MAX_META_CLIENTS * MetaClientInfo::SIZE);
        let buf = IOBuffer::new(total_size);
        let (res, buf) = self.meta_vdev.read(buf, &self.client_info_bid).await;
        res?;
        
        // Validate super header
        let super_header = unsafe {
            std::ptr::read(buf.as_slice().as_ptr() as *const MetaBlkSuperHeader)
        };
        if !super_header.is_valid() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, 
                "Invalid meta block super header"));
        }
        
        // Scan all client info slots
        for slot in 0..MAX_META_CLIENTS {
            let offset = MetaBlkSuperHeader::SIZE + (slot * MetaClientInfo::SIZE);
            let mut info = unsafe {
                std::ptr::read(
                    buf.as_slice()[offset..].as_ptr() as *const MetaClientInfo
                )
            };
            
            if info.is_allocated() && info.validate_crc() {
                info.client_id = slot as u8;  // Ensure client_id is set correctly
                // SAFETY: Only called from load() during initialization, single-threaded
                unsafe {
                    let client_slots = &mut *self.client_slots.get();
                    let recovered_clients = &mut *self.recovered_clients.get();
                    
                    client_slots[slot] = 1;   // Mark slot as allocated
                    let client_name = info.get_client_name();
                    recovered_clients.insert(client_name.clone(), info);
                    println!("Recovered client '{}' (id={})", client_name, slot);
                }
            }
        }
        
        Ok(())
    }
    
    /// Find a free slot (internal helper for use with UnsafeCell)
    fn reserve_slot_internal(client_slots: &mut Vec<u8>) -> io::Result<usize> {
        for (slot, is_busy) in client_slots.iter().enumerate() {
            if *is_busy == 0 {
                client_slots[slot] = 1;
                return Ok(slot);
            }
        }
        Err(io::Error::new(io::ErrorKind::OutOfMemory,
            "No free client slots available"))
    }
}

