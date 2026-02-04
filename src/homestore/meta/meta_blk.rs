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
use iomgr::IOBuffer;
use crate::common::BlkId;
use crate::device::VirtualDev;
use std::sync::Arc;

//
// ================ Constants ================
//

/// Magic number for meta block header
const META_BLK_HEADER_MAGIC: u32 = 0xABCD5678;

/// Size of MetaBlkHeader structure
const META_BLK_HEADER_SIZE: usize = 64;

//
// ================ Structures ================
//

/// Meta block header (stored at the beginning of each metadata block)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct MetaBlkHeader {
    pub magic: u32,              // Magic number to identify valid header
    pub data_size: u32,          // Size of actual data
    pub data_crc: u32,           // CRC32 for the data (not the header)
    pub next_bid: BlkId,         // Next block in chain (invalid if this is last)
    pub overflow_bid: BlkId,     // Overflow block for large data (when not inlined)
    pub name: [u8; 32],          // Name for this metablk
}

impl MetaBlkHeader {
    pub const SIZE: usize = META_BLK_HEADER_SIZE;
    
    pub fn new(name: &str) -> Self {
        let mut name_bytes = [0u8; 32];
        let bytes = name.as_bytes();
        let copy_len = bytes.len().min(31); // Leave room for null terminator
        name_bytes[..copy_len].copy_from_slice(&bytes[..copy_len]);
        
        Self {
            magic: META_BLK_HEADER_MAGIC,
            data_size: 0,
            data_crc: 0,
            next_bid: BlkId::default(),
            overflow_bid: BlkId::default(), // Default to inlined
            name: name_bytes,
        }
    }
    
    pub fn is_valid(&self) -> bool {
        self.magic == META_BLK_HEADER_MAGIC
    }
    
    /// Get the name from the header as a String
    pub fn get_name(&self) -> String {
        // Find the null terminator
        let end = self.name.iter().position(|&b| b == 0).unwrap_or(32);
        String::from_utf8_lossy(&self.name[..end]).to_string()
    }
}

impl Default for MetaBlkHeader {
    fn default() -> Self {
        Self::new("")
    }
}

/// Meta block - key structure that clients operate on
/// Header is stored directly in buffer (first MetaBlkHeader::SIZE bytes)
pub struct MetaBlk {
    pub blkid: BlkId,          // Block ID for this meta block
    pub prev_bid: BlkId,       // Previous block in chain (this is kept outside of persistent header)
    pub buffer: IOBuffer,      // Data buffer (header + data)
    pub(crate) is_fresh: bool, // True if not yet written to chain
}

impl Clone for MetaBlk {
    fn clone(&self) -> Self {
        let mut buffer = IOBuffer::new(self.buffer.len());
        buffer.as_mut_slice().copy_from_slice(self.buffer.as_slice());
        
        Self {
            blkid: self.blkid.clone(),
            prev_bid: self.prev_bid.clone(),
            buffer,
            is_fresh: self.is_fresh,
        }
    }
}

impl MetaBlk {
    /// Create a new MetaBlk with a name
    pub fn new(blkid: BlkId, estimated_size: usize, name: &str) -> Self {
        let mut buffer = IOBuffer::new(estimated_size);
        
        // Initialize header in buffer
        let mut header = MetaBlkHeader::new(name);
        header.data_size = 0;
        header.next_bid = BlkId::default();
        header.overflow_bid = BlkId::default(); // Default to inlined
        
        let header_bytes = unsafe {
            std::slice::from_raw_parts(
                &header as *const MetaBlkHeader as *const u8,
                MetaBlkHeader::SIZE,
            )
        };
        buffer.as_mut_slice()[..MetaBlkHeader::SIZE].copy_from_slice(header_bytes);
        
        Self {
            blkid,
            prev_bid: BlkId::default(),
            buffer,
            is_fresh: true,
        }
    }
    
    /// Write the data to the meta block, taking care of inline/overflow data. It updates the checksum in the header.
    pub async fn write_data(&mut self, data: &IOBuffer, meta_vdev: &Arc<VirtualDev>) -> io::Result<()> {
        let data_len = data.as_slice().len();
        let cur_overflow_bid = self.header().overflow_bid;
        let is_inlineable = data_len <= self.max_inline_data_size();

        if is_inlineable {
            // Inline: copy to buffer
            self.data_slice_mut()[..data_len].copy_from_slice(data.as_slice());
            self.header_mut().overflow_bid = BlkId::default();
        } else {
            // Non-inline: allocate and write overflow blocks
            let hints = crate::common::BlkAllocHints::default();
            let block_size = meta_vdev.block_size() as usize;
            let num_ovf_blks = ((data_len - 1) / block_size + 1) as u32;

            let (status, ovf_bid) = meta_vdev.alloc_contiguous(num_ovf_blks, &hints).await;
            if status != crate::common::BlkAllocStatus::Success {
                return Err(io::Error::new(io::ErrorKind::OutOfMemory, "Failed to allocate overflow blocks"));
            }

            // Write overflow data
            meta_vdev.write(data, &ovf_bid.unwrap()).await?;
            self.header_mut().overflow_bid = ovf_bid.unwrap();
        }
        self.header_mut().data_size = data_len as u32;
        self.header_mut().data_crc = crc::Crc::<u32>::new(&crc::CRC_32_ISCSI).checksum(data.as_slice());
        meta_vdev.write(&self.buffer, &self.blkid).await?;

        // Free the old overflow block if it existed.
        if cur_overflow_bid != BlkId::default() {
            meta_vdev.free(&cur_overflow_bid).await;
        }
        Ok(())
    }

    pub async fn link(&mut self, next_bid: BlkId, meta_vdev: &Arc<VirtualDev>) -> io::Result<()> {
        self.header_mut().next_bid = next_bid;
        meta_vdev.write(&self.buffer, &self.blkid).await?;
        Ok(())
    }

    pub async fn update_next_bid(&mut self, next_bid: BlkId, meta_vdev: &Arc<VirtualDev>) -> io::Result<()> {
        self.header_mut().next_bid = next_bid;
        meta_vdev.write(&self.buffer, &self.blkid).await?;
        Ok(())
    }

    // Read the data from the meta block, taking care of inline/overflow data. In case of inline data, 
    // the data is cached with the metablk object. If overflow, it reads from device and returns the data.
    pub async fn read_data(&self, meta_vdev: &Arc<VirtualDev>) -> io::Result<IOBuffer> {
        let header = self.header();
        let is_inlineable = header.data_size <= self.max_inline_data_size() as u32;

        let mut data = IOBuffer::new(header.data_size as usize);
        if is_inlineable {
            data.as_mut_slice()[..header.data_size as usize].copy_from_slice(&self.data_slice());
            return Ok(data);
        } else {
            let (res, data) = meta_vdev.read(data, &header.overflow_bid).await;
            if res.is_err() {
                return Err(res.unwrap_err());
            }
            return Ok(data);
        }
    }

    pub async fn free(&mut self, meta_vdev: &Arc<VirtualDev>) -> io::Result<()> {
        if self.header().overflow_bid != BlkId::default() {
            meta_vdev.free(&self.header().overflow_bid).await;
        }
        meta_vdev.free(&self.blkid).await;
        Ok(())
    }

    /// Get header reference from buffer (read-only)
    pub fn header(&self) -> MetaBlkHeader {
        unsafe {
            std::ptr::read(self.buffer.as_slice().as_ptr() as *const MetaBlkHeader)
        }
    }
    
    /// Get mutable reference to header in buffer
    pub(crate) fn header_mut(&mut self) -> &mut MetaBlkHeader {
        unsafe {
            &mut *(self.buffer.as_mut_slice().as_mut_ptr() as *mut MetaBlkHeader)
        }
    }

    /// Get the data portion of the buffer (after header)
    pub fn data_slice(&self) -> &[u8] {
        &self.buffer.as_slice()[MetaBlkHeader::SIZE..]
    }
    
    /// Get mutable data portion of the buffer (after header)
    pub fn data_slice_mut(&mut self) -> &mut [u8] {
        &mut self.buffer.as_mut_slice()[MetaBlkHeader::SIZE..]
    }
    
    /// Get maximum data size that can fit in this block
    pub fn max_inline_data_size(&self) -> usize {
        self.buffer.as_slice().len() - MetaBlkHeader::SIZE
    }

    /// Calculate number of blocks needed for given data size
    pub fn data_size_to_nblks(&self, data_size: usize, block_size: usize) -> u32 {
        ((data_size + MetaBlkHeader::SIZE + block_size - 1) / block_size) as u32
    }
}

//
// ================ MetaBlkWrapper ================
//

/// Convenience wrapper around MetaBlk and MetaClient
/// 
/// This combines a MetaBlk with its MetaClient for easier metadata operations,
/// eliminating the need to pass both around separately.
pub struct MetaBlkWrapper {
    meta_blk: MetaBlk,
    meta_client: Arc<super::meta_client::MetaClient>,
}

impl MetaBlkWrapper {
    /// Create a new MetaBlkWrapper by allocating a new metablk
    pub async fn create(
        meta_client: Arc<super::meta_client::MetaClient>,
        name: &str,
        estimated_data_size: Option<usize>,
    ) -> io::Result<Self> {
        let meta_blk = meta_client.create_meta_blk(name, estimated_data_size).await?;
        Ok(Self { meta_blk, meta_client })
    }
    
    /// Load an existing MetaBlkWrapper from a MetaBlk
    pub fn load(meta_client: Arc<super::meta_client::MetaClient>, meta_blk: MetaBlk) -> Self {
        Self { meta_blk, meta_client }
    }
    
    /// Write data to the metablk
    pub async fn write(&self, data: &[u8]) -> io::Result<()> {
        self.meta_blk.write_data(data, &self.meta_client.meta_vdev).await
    }
    
    /// Read data from the metablk
    pub async fn read(&self) -> io::Result<iomgr::IOBuffer> {
        self.meta_blk.read_data(&self.meta_client.meta_vdev).await
    }
    
    /// Get reference to underlying MetaBlk
    pub fn meta_blk(&self) -> &MetaBlk {
        &self.meta_blk
    }
    
    /// Get reference to MetaClient
    pub fn meta_client(&self) -> &Arc<super::meta_client::MetaClient> {
        &self.meta_client
    }
}