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

use std::io;
use std::sync::Arc;
use iomgr::{IOBuffer, AsyncMutex};
use crate::common::BlkId;
use crate::device::VirtualDev;
use std::collections::HashMap;
use super::meta_blk::{MetaBlk, MetaBlkHeader};
use super::meta_blk_manager::{META_SUPER_HEADER_SIZE};
use async_stream::stream;
use futures::stream::Stream;

//
// ================ Constants ================
//
pub const MAX_META_CLIENTS: usize = 255; /// Maximum number of metadata clients
pub const MAX_CLIENT_NAME_LEN: usize = 232; /// Maximum length of client name
const META_CLIENT_INFO_MAGIC: u32 = 0xCEEDBEED; /// Magic number for meta client info
pub const META_CLIENT_INFO_VERSION: u16 = 0x1; /// Version of the meta client info format
const META_CLIENT_INFO_SIZE: usize = 512; /// Size of MetaClientInfo structure

//
// ================ Types ================
//

// const _: () = assert!(std::mem::size_of::<MetaClientInfo>() == MetaClientInfo::SIZE);

//
// ================ Structures ================
//

/// Meta client information (one per registered client)
/// Exactly 128 bytes to fit 4 per 512-byte block, 256 clients in 64 blocks
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct MetaClientInfo {
    pub magic: u32,                     // Magic number to identify valid entry
    pub version: u16,                   // Version of the metadata format
    pub client_id: u8,                  // Client ID / slot number
    pub slot_allocated: u8,             // Is this slot allocated (1) or free (0)
    pub padding1: [u8; 4],              // Padding for word boundary
    pub first_blkid: BlkId,             // First block ID for this client's metadata
    pub crc: u32,                       // CRC32 for integrity check
    pub client_name: [u8; MAX_CLIENT_NAME_LEN], // Client name (null-terminated)
    pub padding2: [u8; 256],            // Padding to make total 512 bytes (4+2+1+1+4+8+4+232+256=512)
}

impl MetaClientInfo {
    pub const SIZE: usize = META_CLIENT_INFO_SIZE;
    
    pub fn new() -> Self {
        let mut info =         Self {
            magic: META_CLIENT_INFO_MAGIC,
            version: META_CLIENT_INFO_VERSION,
            client_id: 0,
            slot_allocated: 0,
            padding1: [0u8; 4],
            first_blkid: BlkId::default(),
            crc: 0,
            client_name: [0u8; MAX_CLIENT_NAME_LEN],
            padding2: [0u8; 256],
        };
        info.update_crc();
        info
    }
    
    pub fn is_allocated(&self) -> bool {
        self.slot_allocated == 1 && self.magic == META_CLIENT_INFO_MAGIC && self.validate_crc()
    }
    
    pub fn set_allocated(&mut self) {
        self.magic = META_CLIENT_INFO_MAGIC;
        self.slot_allocated = 1;
        self.update_crc();
    }
    
    pub fn set_free(&mut self) {
        self.slot_allocated = 0;
        self.update_crc();
    }
    
    /// Calculate and update CRC for this client info
    pub fn update_crc(&mut self) {
        // Temporarily zero out CRC field for calculation
        self.crc = 0;
        
        let bytes = unsafe {
            std::slice::from_raw_parts(
                self as *const MetaClientInfo as *const u8,
                Self::SIZE,
            )
        };
        
        // Calculate CRC32 over entire structure
        let digest = crc::Crc::<u32>::new(&crc::CRC_32_ISCSI).checksum(bytes);
        self.crc = digest;
    }
    
    /// Validate CRC for this client info
    pub fn validate_crc(&self) -> bool {
        let saved_crc = self.crc;
        
        // Create a copy and zero out CRC for validation
        let mut copy = *self;
        copy.crc = 0;
        
        let bytes = unsafe {
            std::slice::from_raw_parts(
                &copy as *const MetaClientInfo as *const u8,
                Self::SIZE,
            )
        };
        
        let computed_crc = crc::Crc::<u32>::new(&crc::CRC_32_ISCSI).checksum(bytes);
        computed_crc == saved_crc
    }
    
    pub fn set_client_name(&mut self, name: &str) {
        let bytes = name.as_bytes();
        let len = std::cmp::min(bytes.len(), MAX_CLIENT_NAME_LEN - 1);
        self.client_name[..len].copy_from_slice(&bytes[..len]);
        self.client_name[len] = 0; // Null terminate
        self.update_crc();
    }
    
    pub fn get_client_name(&self) -> String {
        let null_pos = self.client_name.iter().position(|&c| c == 0).unwrap_or(MAX_CLIENT_NAME_LEN);
        String::from_utf8_lossy(&self.client_name[..null_pos]).to_string()
    }
}

impl Default for MetaClientInfo {
    fn default() -> Self {
        Self::new()
    }
}

/// Mutable state protected by mutex
struct MetaClientState {
    info: MetaClientInfo,                  // Client info (on-disk metadata)
    meta_blks: HashMap<BlkId, MetaBlk>,    // All the MetaBlks in the chain indexed by block ID
    tail_blkid: BlkId,                     // Block ID of the tail meta block in the chain
}

/// Meta Client - represents a registered metadata client
pub struct MetaClient {
    state: Arc<AsyncMutex<MetaClientState>>,  // Mutable state protected by mutex
    pub meta_vdev: Arc<VirtualDev>,           // Reference to meta vdev for I/O operations
    info_bid: BlkId,                          // Block ID for the client info area (calculated from client_id)
}

impl MetaClient {
    /// Create a new MetaClient and persist client info to disk
    pub async fn create(
        client_name: String,
        client_id: u8,
        meta_vdev: Arc<VirtualDev>,
    ) -> io::Result<Self> {
        // Create MetaClientInfo
        let mut info = MetaClientInfo::new();
        info.set_allocated();
        info.client_id = client_id;
        info.set_client_name(&client_name);
        info.first_blkid = BlkId::default();  // No pre-allocation
        info.update_crc();  // Final CRC after all fields set
        
        let nsuper_hdr_blks = (META_SUPER_HEADER_SIZE - 1) / meta_vdev.block_size() as usize + 1;
        let info_nblks = (MetaClientInfo::SIZE - 1) / meta_vdev.block_size() as usize + 1;
        let info_bid = BlkId::new(((client_id as usize * info_nblks) + nsuper_hdr_blks) as u32, 
                                  info_nblks as u16, 
                                  meta_vdev.get_chunks()[0].chunk_id() as u16);
        
        let state = MetaClientState {
            info,
            meta_blks: HashMap::new(),
            tail_blkid: BlkId::default(),
        };
        
        let client = Self {
            state: Arc::new(AsyncMutex::new(state)),
            meta_vdev,
            info_bid,
        };
        
        // Write client info to disk immediately after creation
        {
            let state = client.state.lock().await;
            client.write_client_info(&state.info).await?;
        }  // Drop lock before returning
        
        Ok(client)
    }
    
    /// Get the client ID
    pub async fn client_id(&self) -> u8 {
        let state = self.state.lock().await;
        state.info.client_id
    }
    
    /// Create MetaClient from existing MetaClientInfo (for recovery)
    /// Loads the chain of MetaBlks from disk and reconstructs the client state
    /// Note: This only loads the MetaBlk metadata, not the actual data.
    /// Use recovered_blocks() to stream the data lazily.
    pub async fn load(
        info: MetaClientInfo,
        meta_vdev: Arc<VirtualDev>,
    ) -> io::Result<Self> {
        let client_id = info.client_id;
        let nsuper_hdr_blks = (META_SUPER_HEADER_SIZE - 1) / meta_vdev.block_size() as usize + 1;
        let info_nblks = (MetaClientInfo::SIZE - 1) / meta_vdev.block_size() as usize + 1;
        let info_bid = BlkId::new(((client_id as usize * info_nblks) + nsuper_hdr_blks) as u32, 
                                  info_nblks as u16, 
                                  meta_vdev.get_chunks()[0].chunk_id() as u16);
        
        let mut meta_blks = HashMap::new();
        let mut tail_blkid = BlkId::default();
        
        // Start traversing the chain from first_blkid
        let mut current_bid = info.first_blkid.clone();
        let mut prev_bid = BlkId::default();
        
        while current_bid != BlkId::default() {
            // Read the block header to get nblks
            let header_buf = IOBuffer::new(MetaBlkHeader::SIZE);
            let (res, header_buf) = meta_vdev.read(header_buf, &current_bid).await;
            if let Err(e) = res {
                println!("Failed to read meta block {:?} for client {}: {}", 
                         current_bid, info.get_client_name(), e);
                break;
            }
            
            // Parse header
            let header = unsafe {
                std::ptr::read(header_buf.as_slice().as_ptr() as *const MetaBlkHeader)
            };
            
            // Validate header magic
            if !header.is_valid() {
                println!("Invalid meta block header magic for block {:?}, client {}", 
                         current_bid, info.get_client_name());
                break;
            }
            
            // Read the full block (we need nblks from the current_bid)
            let block_size = meta_vdev.block_size() as usize;
            let nblks = current_bid.blk_count();
            let block_buf = IOBuffer::new(nblks as usize * block_size);
            let (res, block_buf) = meta_vdev.read(block_buf, &current_bid).await;
            if let Err(e) = res {
                println!("Failed to read full meta block {:?} for client {}: {}", 
                         current_bid, info.get_client_name(), e);
                break;
            }
            
            // Create MetaBlk from the buffer
            let meta_blk = MetaBlk {
                blkid: current_bid.clone(),
                prev_bid: prev_bid.clone(),
                buffer: block_buf,
                is_fresh: false,  // Loaded from disk, not fresh
            };
            
            // Get next_bid before moving the block
            let next_bid = meta_blk.header().next_bid.clone();
            
            // Update tail_blkid
            tail_blkid = current_bid.clone();
            
            // Add to HashMap
            meta_blks.insert(current_bid.clone(), meta_blk);
            
            // Move to next block
            prev_bid = current_bid;
            current_bid = next_bid;
        }
        
        println!("Loaded {} meta blocks for client {}", meta_blks.len(), info.get_client_name());
        
        let state = MetaClientState {
            info,
            meta_blks,
            tail_blkid,
        };
        
        Ok(Self {
            state: Arc::new(AsyncMutex::new(state)),
            meta_vdev,
            info_bid,
        })
    }
    
    pub async fn client_name(&self) -> String {
        let state = self.state.lock().await;
        state.info.get_client_name()
    }
    
    /// Get the number of meta blocks in this client's chain
    pub async fn num_meta_blks(&self) -> usize {
        let state = self.state.lock().await;
        state.meta_blks.len()
    }
    
    /// Stream recovered blocks one at a time (lazy loading)
    /// This reads data on-demand, only one IOBuffer in memory at a time.
    /// Use this after load() to iterate through all recovered blocks.
    pub fn recovered_blocks(&self) -> impl Stream<Item = io::Result<(MetaBlk, IOBuffer)>> + '_ {
        let state = Arc::clone(&self.state);
        let meta_vdev = Arc::clone(&self.meta_vdev);
        
        stream! {
            // Get the list of block IDs (just IDs, no data yet)
            let blk_ids: Vec<BlkId> = {
                let guard = state.lock().await;
                guard.meta_blks.keys().cloned().collect()
            };
            
            // Now iterate and read data one at a time (lazy)
            for blk_id in blk_ids {
                let meta_blk = {
                    let guard = state.lock().await;
                    guard.meta_blks.get(&blk_id).cloned()
                };
                
                if let Some(meta_blk) = meta_blk {
                    // Read data for THIS block only (on-demand)
                    match meta_blk.read_data(&meta_vdev).await {
                        Ok(data) => yield Ok((meta_blk, data)),
                        Err(e) => {
                            eprintln!("Failed to read data for block {:?}: {}", blk_id, e);
                            yield Err(e);
                        }
                    }
                    // IOBuffer 'data' is dropped after yield, freeing memory!
                }
            }
        }
    }
    
    /// Create a named meta block for this client (allocates, but doesn't write yet)
    /// Returns the MetaBlk ready for writing.
    pub async fn create_meta_blk(&self, name: &str, estimated_data_size: Option<usize>) -> io::Result<MetaBlk> {
        let client_name = self.client_name().await;
        
        // Calculate number of blocks to be inlined and alloc a block for it.
        let block_size = self.meta_vdev.block_size() as usize;
        let nblks = ((estimated_data_size.unwrap_or(0) + MetaBlkHeader::SIZE + block_size - 1) / block_size) as u32;
        let hints = crate::common::BlkAllocHints::default();
        let (status, blkid) = self.meta_vdev.alloc_contiguous(nblks, &hints).await;
        
        if status != crate::common::BlkAllocStatus::Success {
            return Err(io::Error::new(io::ErrorKind::OutOfMemory,
                format!("Failed to allocate block for client {}", client_name)));
        }
                
        println!("Allocated fresh meta block {:?} for client {} (name: {})", 
                 blkid.unwrap(), client_name, name);

        // Create fresh MetaBlk (is_fresh=true by default in new())
        let meta_blk = MetaBlk::new(blkid.unwrap(), nblks as usize * block_size, name);

        Ok(meta_blk)
    }
    
    /// Get a meta block by name
    pub async fn get_meta_blk(&self, name: &str) -> Option<MetaBlk> {
        let state = self.state.lock().await;
        for meta_blk in state.meta_blks.values() {
            let header = meta_blk.header();
            let blk_name = std::str::from_utf8(&header.name)
                .ok()?
                .trim_end_matches('\0');
            if blk_name == name {
                return Some(meta_blk.clone());
            }
        }
        None
    }
    
    /// Write data to a MetaBlk (dispatches to write_new or write_existing)
    /// Handles both fresh blocks (first write, needs chaining) and existing blocks (update)
    pub async fn write_meta_blk(&self, mut meta_blk: MetaBlk, data: &IOBuffer) -> io::Result<()> {
        // First we write the data. At this point, the meta block is not yet chained into the client's list.
        meta_blk.write_data(data, &self.meta_vdev).await?;

        let mut state = self.state.lock().await;

        // If the block is already in the client's list, then we are writing them in place (not a new meta block).
        if state.meta_blks.contains_key(&meta_blk.blkid) {
            assert!(state.info.first_blkid != BlkId::default());
            // Update the existing entry
            state.meta_blks.insert(meta_blk.blkid.clone(), meta_blk);
            return Ok(());
        }

        // Look up for the tail meta block in the client's list. If not found, this is the first block in the chain.
        let blkid = meta_blk.blkid.clone();
        if state.tail_blkid == BlkId::default() {
            // First block in chain - update client's first_blkid and persist the client info
            state.info.first_blkid = blkid.clone();
            let info_copy = state.info;
            self.write_client_info(&info_copy).await?;
            state.meta_blks.insert(blkid.clone(), meta_blk);
            state.tail_blkid = blkid;
        } else {
            // Get tail block and update pointers
            let tail_bid = state.tail_blkid.clone();
            if let Some(tail_blk) = state.meta_blks.get_mut(&tail_bid) {
                meta_blk.prev_bid = tail_blk.blkid.clone();
                tail_blk.update_next_bid(blkid.clone(), &self.meta_vdev).await?;
            }
            state.meta_blks.insert(blkid.clone(), meta_blk);
            state.tail_blkid = blkid;
        }
        Ok(())
    }
    
    /// Read data from a meta block
    pub async fn read_meta_blk(&self, meta_blk: &MetaBlk) -> io::Result<IOBuffer> {
        // Validate that the block exists in our map
        let state = self.state.lock().await;
        if !state.meta_blks.contains_key(&meta_blk.blkid) {
            return Err(io::Error::new(io::ErrorKind::NotFound, "MetaBlk not found in this client"));
        }
        drop(state); // Release lock before async I/O
        
        // Read the data from the meta block
        meta_blk.read_data(&self.meta_vdev).await
    }
    
    pub async fn remove_meta_blk(&self, meta_blk: &MetaBlk) -> io::Result<()> {
        let mut state = self.state.lock().await;
        
        // Validate that the block exists in our map
        if !state.meta_blks.contains_key(&meta_blk.blkid) {
            return Err(io::Error::new(io::ErrorKind::NotFound, "MetaBlk not found in this client"));
        }
        
        // Remove the block from the map
        let mut meta_blk = state.meta_blks.remove(&meta_blk.blkid).unwrap();

        // Get the previous and next block IDs
        let prev_blkid = meta_blk.prev_bid.clone();
        let next_blkid = meta_blk.header().next_bid.clone();

        if prev_blkid != BlkId::default() {
            // This is not the first block in the chain, update the previous block's next pointer.
            if let Some(prev_blk) = state.meta_blks.get_mut(&prev_blkid) {
                prev_blk.update_next_bid(next_blkid.clone(), &self.meta_vdev).await?;
            }
            
            if next_blkid == BlkId::default() {
                // This was the tail block
                state.tail_blkid = prev_blkid;
            } else {
                // Update the next block's prev pointer
                if let Some(next_blk) = state.meta_blks.get_mut(&next_blkid) {
                    next_blk.prev_bid = prev_blkid;
                }
            }
        } else {
            // This is the first block in the chain, update the client info
            if next_blkid == BlkId::default() {
                // This was the only block
                state.info.first_blkid = BlkId::default();
                state.tail_blkid = BlkId::default();
            } else {
                // Update first_blkid to the next block
                state.info.first_blkid = next_blkid.clone();
                if let Some(next_blk) = state.meta_blks.get_mut(&next_blkid) {
                    next_blk.prev_bid = BlkId::default();
                }
            }
            self.write_client_info(&state.info).await?;
        }

        // Free the block and its overflow data
        meta_blk.free(&self.meta_vdev).await?;
        Ok(())
    }

    async fn write_client_info(&self, client_info: &MetaClientInfo) -> io::Result<()> {
        // Create a copy and update its CRC
        let mut info = *client_info;
        info.update_crc();
        
        let mut buf = IOBuffer::new(MetaClientInfo::SIZE);
        let info_bytes = unsafe {
            std::slice::from_raw_parts(
                &info as *const MetaClientInfo as *const u8,
                MetaClientInfo::SIZE)
        };
        buf.as_mut_slice()[..MetaClientInfo::SIZE].copy_from_slice(info_bytes);
        self.meta_vdev.write(&buf, &self.info_bid).await?;
        Ok(())
    }
}

