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

use uuid::Uuid;
use super::virtual_dev::VDevSizeType;

//
// ================ Super Block Layout ================
//
// Super blk format
//  ________________________________________________________________________________________________________
//  |        |<---------Vdev Area---------->|  <---------------------Chunk Area--------------->|           |
//  | First  | Vdev[1]| Vdev[2]| .. |Vdev[N]| Chunk Slot | Chunk[1] | Chunk[2]| .. |  Chunk[M] | Reserved  |
//  | Block  | Info   | Info   |    | Info  | Bitmap     | Info     | Info    |    |  Info     | Space     |
//  |________|________|________|___ |_______|____________|__________|_________|____|___________|___________|
//
//  where:
//    N = max number of vdevs we support for this class of device
//    M = max number of chunks we support for this class of device
//

//
// ================ Constants ================
//

/// Homestore super block configuration constants
pub struct HsSuperBlk;

impl HsSuperBlk {
    // Minimum chunk sizes
    pub const MIN_CHUNK_SIZE_DATA_DEVICE: u64 = 16 * 1024 * 1024; // 16MB
    pub const MIN_CHUNK_SIZE_FAST_DEVICE: u64 = 32 * 1024 * 1024; // 32MB

    // System limits
    pub const MAX_CHUNKS_IN_SYSTEM: u32 = 65536;
    pub const MAX_VDEVS_IN_SYSTEM: u32 = 1024;

    // Extra padding for future expansion
    pub const EXTRA_SB_SIZE_FOR_DATA_DEVICE: u64 = 8 * 1024 * 1024; // 8MB
    pub const EXTRA_SB_SIZE_FOR_FAST_DEVICE: u64 = 1 * 1024 * 1024; // 1MB

    /// First block offset in physical device
    pub const fn first_block_offset() -> u64 {
        0
    }

    /// First block size
    pub const fn first_block_size() -> u64 {
        FirstBlock::IO_FB_SIZE as u64
    }

    /// VDev slot bitmap size (tracks which VDev slots are active)
    pub const fn vdev_slot_bitmap_size() -> u64 {
        // Bitmap to track MAX_VDEVS_IN_SYSTEM slots
        // Each bit represents one VDev slot
        // Add 4KB headroom for bitmap serialized header
        let bytes = ((Self::MAX_VDEVS_IN_SYSTEM as u64 + 7) / 8) + 4096;
        ((bytes + 4095) / 4096) * 4096 // Round up to 4KB page boundary
    }

    /// VDev super block size (bitmap + all vdev_info structs)
    pub const fn vdev_super_block_size() -> u64 {
        Self::vdev_slot_bitmap_size() + (Self::MAX_VDEVS_IN_SYSTEM as u64) * (VDevInfo::SIZE as u64)
    }

    /// Chunk super block size (chunk bitmap + all chunk_info structs)
    pub fn chunk_super_block_size(dinfo: &DevInfo) -> u64 {
        Self::chunk_info_bitmap_size(dinfo) + (Self::max_chunks_in_pdev(dinfo) as u64) * 512
    }

    /// Chunk info bitmap size
    pub fn chunk_info_bitmap_size(dinfo: &DevInfo) -> u64 {
        // Chunk bitmap area has bitmap of max_chunks rounded off to 4k page
        // add 4KB headroom for bitmap serialized header
        let bytes = ((Self::max_chunks_in_pdev(dinfo) as u64 + 7) / 8) + 4096;
        ((bytes + 4095) / 4096) * 4096 // Round up to 4KB
    }

    /// Total super block size including padding
    pub fn total_size(dinfo: &DevInfo) -> u64 {
        Self::total_used_size(dinfo) + Self::future_padding_size(dinfo)
    }

    /// Total used super block size
    pub fn total_used_size(dinfo: &DevInfo) -> u64 {
        Self::first_block_size() + Self::vdev_super_block_size() + Self::chunk_super_block_size(dinfo)
    }

    /// VDev slot bitmap offset (immediately after first block)
    pub const fn vdev_slot_bitmap_offset() -> u64 {
        Self::first_block_offset() + Self::first_block_size()
    }

    /// VDev info array offset (after bitmap)
    pub const fn vdev_info_array_offset() -> u64 {
        Self::vdev_slot_bitmap_offset() + Self::vdev_slot_bitmap_size()
    }

    /// VDev super block offset (for backward compatibility - same as bitmap offset)
    pub const fn vdev_sb_offset() -> u64 {
        Self::vdev_slot_bitmap_offset()
    }

    /// Chunk super block offset
    pub const fn chunk_sb_offset() -> u64 {
        Self::vdev_sb_offset() + Self::vdev_super_block_size()
    }

    /// Future padding size for growth
    pub fn future_padding_size(dinfo: &DevInfo) -> u64 {
        match dinfo.dev_type {
            HSDevType::Fast => Self::EXTRA_SB_SIZE_FOR_FAST_DEVICE,
            _ => Self::EXTRA_SB_SIZE_FOR_DATA_DEVICE,
        }
    }

    /// Maximum chunks in a physical device
    pub fn max_chunks_in_pdev(dinfo: &DevInfo) -> u32 {
        let min_c_size = Self::min_chunk_size(dinfo.dev_type);
        (dinfo.dev_size / min_c_size) as u32
    }

    /// Minimum chunk size based on device type
    pub fn min_chunk_size(dtype: HSDevType) -> u64 {
        match dtype {
            HSDevType::Fast => Self::MIN_CHUNK_SIZE_FAST_DEVICE,
            _ => Self::MIN_CHUNK_SIZE_DATA_DEVICE,
        }
    }
}

//
// ================ Device Types ================
//

/// Device type enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum HSDevType {
    Data = 0,
    Fast = 1,
    ZNS = 2,
}

impl HSDevType {
    pub fn from_u8(val: u8) -> Self {
        match val {
            0 => HSDevType::Data,
            1 => HSDevType::Fast,
            2 => HSDevType::ZNS,
            _ => HSDevType::Data, // Default to Data
        }
    }

    pub fn to_u8(self) -> u8 {
        self as u8
    }
}

/// Device information structure
#[derive(Debug, Clone)]
pub struct DevInfo {
    pub dev_name: String,
    pub dev_size: u64,
    pub dev_type: HSDevType,
}

//
// ================ First Block Structures ================
//

/// Disk attributes (copy from iomgr::drive_attributes)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct DiskAttr {
    pub phys_page_size: u32,        // Physical page size of flash ssd/nvme
    pub align_size: u32,            // Size alignment supported by drives/kernel
    pub atomic_phys_page_size: u32, // Atomic page size of the drive
    pub num_streams: u32,           // Number of streams supported
}

impl DiskAttr {
    pub fn new() -> Self {
        Self {
            phys_page_size: 0,
            align_size: 0,
            atomic_phys_page_size: 0,
            num_streams: 0,
        }
    }

    pub fn is_valid(&self) -> bool {
        self.is_page_valid(self.phys_page_size)
            && self.is_page_valid(self.align_size)
            && self.is_page_valid(self.atomic_phys_page_size)
    }

    fn is_page_valid(&self, page_size: u32) -> bool {
        page_size != 0 && (page_size & (page_size - 1)) == 0 // Must be power of 2
    }
}

impl Default for DiskAttr {
    fn default() -> Self {
        Self::new()
    }
}

/// First block header structure (global homestore metadata)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct FirstBlockHeader {
    pub gen_number: u64,               // Generation count of this structure
    pub version: u32,                  // Version Id of this structure
    pub product_name: [u8; 64],        // Product name (64 bytes)
    pub num_pdevs: u32,                // Total number of pdevs homestore is being created on
    pub max_vdevs: u32,                // Max VDevs possible, cannot be changed post formatting
    pub max_system_chunks: u32,        // Max Chunks possible, cannot be changed post formatting
    pub system_uuid: [u8; 16],         // System UUID (Uuid as 16 bytes)
}

impl FirstBlockHeader {
    pub const PRODUCT_NAME: &'static str = "HomeStore4x";
    pub const PRODUCT_NAME_SIZE: usize = 64;
    pub const CURRENT_SUPERBLOCK_VERSION: u32 = 4;

    pub fn new() -> Self {
        let mut product_name = [0u8; 64];
        let name_bytes = Self::PRODUCT_NAME.as_bytes();
        let len = std::cmp::min(name_bytes.len(), 64);
        product_name[..len].copy_from_slice(&name_bytes[..len]);

        Self {
            gen_number: 0,
            version: Self::CURRENT_SUPERBLOCK_VERSION,
            product_name,
            num_pdevs: 0,
            max_vdevs: HsSuperBlk::MAX_VDEVS_IN_SYSTEM,
            max_system_chunks: HsSuperBlk::MAX_CHUNKS_IN_SYSTEM,
            system_uuid: [0u8; 16],
        }
    }

    pub fn get_product_name(&self) -> String {
        let null_pos = self.product_name.iter().position(|&c| c == 0).unwrap_or(64);
        String::from_utf8_lossy(&self.product_name[..null_pos]).to_string()
    }

    pub fn set_system_uuid(&mut self, uuid: Uuid) {
        self.system_uuid.copy_from_slice(uuid.as_bytes());
    }

    pub fn get_system_uuid(&self) -> Uuid {
        Uuid::from_bytes(self.system_uuid)
    }
}

impl Default for FirstBlockHeader {
    fn default() -> Self {
        Self::new()
    }
}

/// Physical device information header
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct PDevInfoHeader {
    pub data_offset: u64,          // Offset within pdev where data starts
    pub size: u64,                 // Total pdev size
    pub pdev_id: u32,              // Device ID for this store instance
    pub max_pdev_chunks: u32,      // Max chunks in this pdev possible
    pub dev_attr: DiskAttr,        // Attributes homestore expects from all devices
    pub mirror_super_block: u8,    // Have we mirrored the super block on head/tail (0x0 or 0x1)
    pub system_uuid: [u8; 16],     // Current system uuid stamp to protect from device exchange
}

impl PDevInfoHeader {
    pub fn new() -> Self {
        Self {
            data_offset: 0,
            size: 0,
            pdev_id: 0,
            max_pdev_chunks: 0,
            dev_attr: DiskAttr::new(),
            mirror_super_block: 0x00,
            system_uuid: [0u8; 16],
        }
    }

    pub fn set_system_uuid(&mut self, uuid: Uuid) {
        self.system_uuid.copy_from_slice(uuid.as_bytes());
    }

    pub fn get_system_uuid(&self) -> Uuid {
        Uuid::from_bytes(self.system_uuid)
    }
}

impl Default for PDevInfoHeader {
    fn default() -> Self {
        Self::new()
    }
}

/// First block structure (on-disk format)
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct FirstBlock {
    pub magic: u64,                   // Header magic expected to be at the top of block
    pub checksum: u32,                // Checksum of the entire first block (excluding this field)
    pub formatting_done: u32,         // Has formatting completed yet (bit 0), reserved (bits 1-31)
    pub hdr: FirstBlockHeader,        // Information about the entire system
    pub this_pdev_hdr: PDevInfoHeader, // Information about the current pdev
}

impl FirstBlock {
    pub const ATOMIC_FB_SIZE: usize = 512;  // Atomic size for first block
    pub const IO_FB_SIZE: usize = 4096;     // IO size we use (with padding)
    pub const HOMESTORE_MAGIC: u64 = 0xABBECDCD;

    pub fn new() -> Self {
        Self {
            magic: Self::HOMESTORE_MAGIC,
            checksum: 0,
            formatting_done: 0,
            hdr: FirstBlockHeader::new(),
            this_pdev_hdr: PDevInfoHeader::new(),
        }
    }

    pub fn is_valid(&self) -> bool {
        self.magic == Self::HOMESTORE_MAGIC
            && self.hdr.get_product_name() == FirstBlockHeader::PRODUCT_NAME
            && (self.formatting_done & 0x1) != 0
    }

    pub fn set_formatting_done(&mut self, done: bool) {
        if done {
            self.formatting_done |= 0x1;
        } else {
            self.formatting_done &= !0x1;
        }
    }

    pub fn is_formatting_done(&self) -> bool {
        (self.formatting_done & 0x1) != 0
    }
}

impl Default for FirstBlock {
    fn default() -> Self {
        Self::new()
    }
}

// Ensure FirstBlock fits within atomic size
const _: () = assert!(std::mem::size_of::<FirstBlock>() <= FirstBlock::ATOMIC_FB_SIZE);

//
// ================ VDev Structures ================
//

/// Virtual device information structure (on-disk format) 512 bytes
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct VDevInfo {
    pub vdev_size: u64,                  // 0: Size of the vdev (RUNTIME: recomputed from ChunkInfo on recovery)
    pub vdev_id: u32,                    // 8: Id for this vdev (unique per homestore instance)
    pub num_mirrors: u32,                // 12: Total number of mirrors
    pub blk_size: u32,                   // 16: IO block size for this vdev
    pub num_primary_chunks: u32,         // 20: Number of primary chunks (RUNTIME: recomputed from ChunkInfo on recovery)
    pub chunk_size: u32,                 // 24: Chunk size used in vdev (NOTE: u32, not u64!)
    pub size_type: u8,                   // 28: Whether its a static or dynamic type
    pub slot_allocated: u8,              // 29: Is this current slot allocated
    pub failed: u8,                      // 30: Set to true if disk is replaced
    pub hs_dev_type: u8,                 // 31: PDev dev type (as in fast or data)
    pub multi_pdev_choice: u8,           // 32: Choice when multiple pdevs present
    pub name: [u8; 64],                  // 33: Name of the vdev
    pub checksum: u16,                   // 97: Checksum of this entire Block
    pub alloc_type: u8,                  // 99: Allocator type of this vdev
    pub chunk_sel_type: u8,              // 100: Chunk Selector type of this vdev_id
    pub use_slab_allocator: u8,          // 101: Use slab allocator for this vdev
    pub padding: [u8; 154],              // 102: Padding (154 bytes to make total 256 bytes before user_private)
    pub user_private: [u8; 256],         // 256: User specific information
}

impl VDevInfo {
    pub const SIZE: usize = 512;
    pub const USER_PRIVATE_SIZE: usize = 256;
    pub const MAX_NAME_LEN: usize = 64;

    pub fn new() -> Self {
        Self {
            vdev_size: 0,
            vdev_id: 0,
            num_mirrors: 0,
            blk_size: 0,
            num_primary_chunks: 0,
            chunk_size: 0,
            size_type: VDevSizeType::Static.to_u8(),
            slot_allocated: 0x00,
            failed: 0x00,
            hs_dev_type: HSDevType::Data.to_u8(),
            multi_pdev_choice: 0,
            name: [0u8; 64],
            checksum: 0,
            alloc_type: 0,
            chunk_sel_type: 0,
            use_slab_allocator: 0,
            padding: [0u8; 154],
            user_private: [0u8; 256],
        }
    }

    pub fn set_name(&mut self, name: &str) {
        let bytes = name.as_bytes();
        let len = std::cmp::min(bytes.len(), Self::MAX_NAME_LEN - 1);
        self.name[..len].copy_from_slice(&bytes[..len]);
        self.name[len] = 0; // Null terminate
    }

    pub fn get_name(&self) -> String {
        let null_pos = self.name.iter().position(|&c| c == 0).unwrap_or(Self::MAX_NAME_LEN);
        String::from_utf8_lossy(&self.name[..null_pos]).to_string()
    }

    pub fn set_allocated(&mut self) {
        self.slot_allocated = 0x01;
    }

    pub fn set_free(&mut self) {
        self.slot_allocated = 0x00;
    }

    pub fn is_allocated(&self) -> bool {
        self.slot_allocated == 0x01
    }

    pub fn mark_failed(&mut self) {
        self.failed = 0x01;
    }

    pub fn is_failed(&self) -> bool {
        self.failed == 0x01
    }

    pub fn set_user_private(&mut self, data: &[u8]) {
        let len = std::cmp::min(data.len(), Self::USER_PRIVATE_SIZE);
        self.user_private[..len].copy_from_slice(&data[..len]);
    }

    pub fn compute_checksum(&mut self) {
        use crc::{Crc, Algorithm};
        
        // CRC-16 T10 DIF algorithm
        const CRC16_T10DIF_ALGO: Algorithm<u16> = Algorithm {
            width: 16,
            poly: 0x8bb7,
            init: 0x0000,
            refin: false,
            refout: false,
            xorout: 0x0000,
            check: 0x0000,
            residue: 0x0000,
        };
        const CRC16_T10DIF: Crc<u16> = Crc::<u16>::new(&CRC16_T10DIF_ALGO);
        
        self.checksum = 0;
        let bytes = unsafe {
            std::slice::from_raw_parts(
                self as *const Self as *const u8,
                std::mem::size_of::<VDevInfo>(),
            )
        };
        self.checksum = CRC16_T10DIF.checksum(bytes);
    }

    /// Get the offset in super block for this vdev_info
    pub fn vdev_sb_offset(vdev_id: u32) -> u64 {
        HsSuperBlk::vdev_info_array_offset() + (vdev_id as u64) * (Self::SIZE as u64)
    }
}

impl Default for VDevInfo {
    fn default() -> Self {
        Self::new()
    }
}

// Ensure VDevInfo is exactly 512 bytes
const _: () = assert!(std::mem::size_of::<VDevInfo>() == VDevInfo::SIZE);
