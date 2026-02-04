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

use std::{
    collections::{HashMap, HashSet},
    io,
    sync::{Arc, Mutex},
};

use arc_swap::ArcSwap;
use iomgr::IOBuffer;

use super::{
    chunk::Chunk, 
    chunk_pool::ChunkPool,
    chunk_selector::{ChunkSelector, ChunkSelectorType, RoundRobinChunkSelector, MostAvailableSpaceSelector, RandomChunkSelector, OnlyOneChunkSelector},
    device_metadata::*, 
    physical_dev::PhysicalDev
};
use crate::{
    blkalloc::BlkAllocator,
    common::{BlkAllocHints, BlkAllocStatus, BlkId, BlkIds, BlkNum},
};

/// Mutable state of VirtualDev - uses RCU (Read-Copy-Update) for lock-free reads
#[derive(Clone)]
struct VDevMutableState {
    /// Virtual device information
    vdev_info: VDevInfo,
    
    /// Physical devices this vdev uses
    pdevs: HashSet<u32>,
    
    /// All chunks indexed by chunk_id (for fast O(1) I/O operations)
    /// Hot path: write/read/free need fast lookup by chunk_id from BlkId
    all_chunks: HashMap<u32, Arc<Chunk>>,
    
    /// All chunks sorted by creation_order (for sequential access)
    /// Cold path: append log needs nth chunk in creation order
    /// Maintained: rebuilt whenever chunks are added/removed
    chunks_by_creation_order: Vec<Arc<Chunk>>,
    
    /// Total number of chunks (cached for convenience)
    total_chunk_num: u64,
    
    /// Next creation_order to assign (monotonically increasing, even across shrink/expand)
    /// During load: set to max(all chunk creation_orders) + 1
    /// During expand: used for new chunks and incremented
    /// Represents the order in which chunks were created
    next_creation_order: u32,
}

/// Virtual Device - Provides RAID-like striping across multiple physical devices
/// 
/// This is a simplified initial port. Full block allocation, chunk selection, and
/// advanced I/O features will be added incrementally.
pub struct VirtualDev {
    /// Name of the virtual device
    name: String,
    
    /// Mutex for serializing all chunk management operations (expand, remove_chunk, on_chunk_found, destroy)
    /// Does NOT protect reads - RCU provides lock-free reads
    /// This prevents lost updates when multiple threads modify mutable_state concurrently
    chunk_mgmt_mutex: Mutex<()>,
    
    /// Mutable state using RCU pattern (arc-swap)
    /// Reads: ~1-2ns (atomic load), Writes: clone + atomic swap
    mutable_state: ArcSwap<VDevMutableState>,

    /// Chunk selector - storage type depends on VDevSizeType
    /// Static: Direct access, zero overhead
    /// Dynamic: RwLock protected, minimal overhead
    chunk_selector: super::chunk_selector::ChunkSelector,
    
    // ===== Immutable cached fields from vdev_info (avoid atomic loads on hot paths) =====
    /// Virtual device ID (immutable)
    vdev_id: u32,
    
    /// Device type (immutable)
    hs_dev_type: HSDevType,
    
    /// Block size (immutable, accessed frequently)
    blk_size: u32,
    
    /// Multi-pdev choice strategy (immutable)
    multi_pdev_choice: MultiPDevOpts,
    
    /// VDev size type (immutable)
    size_type: VDevSizeType,
    
    /// Block allocator type (immutable)
    allocator_type: BlkAllocatorType,
    
    /// Chunk selector type (immutable)
    chunk_selector_type: ChunkSelectorType,

    /// Use slab allocator (immutable)
    use_slab_allocator: bool,
    
    /// Chunk size for incremental expansion (immutable)
    /// Used by expand() for dynamic VDevs
    incremental_chunk_size: u64,
    
    /// Optional chunk pool for efficient chunk reuse, None if chunk pooling is disabled
    chunk_pool: Option<Mutex<ChunkPool>>,
}

/// Virtual device parameters for creation
#[derive(Debug, Clone)]
pub struct VDevParameters {
    pub vdev_name: String, // Name of the vdev, automa
    pub vdev_size: u64, // Size of the vdev, can be 0 if so calculated from num_chunks and chunk_size
    pub num_chunks: u32, // Number of chunks to allocate, can be 0 if so calculated from vdev_size and chunk_size
    pub chunk_size: u64, // Size of each chunk, initially while created. Can be 0, if so expanded later
    pub incremental_chunk_size: u64, // Chunk size for incremental expansion (used by expand()). Only for dynamic VDevs
    pub size_type: VDevSizeType, // Static or dynamic VDev
    pub blk_size: u32, // Block size to use for IO and allocation for this Vdev
    pub dev_type: HSDevType, // Type of the physical device to use for this Vdev
    pub multi_pdev_opts: MultiPDevOpts, // Multi-pdev choice strategy for this Vdev
    pub num_mirrors: u32, // Number of mirrors to use for this Vdev, currently not supported
    pub alloc_type: BlkAllocatorType, // Type of the block allocator to alloc blks for this Vdev
    pub chunk_sel_type: ChunkSelectorType, // Type decides which chunk to use for allocation
    pub chunk_pool_limit: Option<usize>, // Chunk pool limit: None = no pooling, Some(n) = pool up to n chunks per size
                                         // Only for dynamic VDevs
}

/// Which chunk to shrink
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChunkToShrink {
    /// Shrink the last chunk (highest creation_order)
    Last,
    /// Shrink a specific chunk by ID
    Specific(u32),
}

/// Multi-physical device options
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MultiPDevOpts {
    AllPDevStriped,
    AllPDevMirrored,
    SingleFirstPDev,
    SingleRandomPDev,
}

/// VDev size type
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VDevSizeType {
    Static,
    Dynamic,
}

impl VDevSizeType {
    pub fn to_u8(self) -> u8 {
        match self {
            VDevSizeType::Static => 0,
            VDevSizeType::Dynamic => 1,
        }
    }
}

/// Block allocator type enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlkAllocatorType {
    None,
    Fixed,
    Varsize,
    Append,
}

impl BlkAllocatorType {
    pub fn from_u8(val: u8) -> Self {
        match val {
            0 => BlkAllocatorType::None,
            1 => BlkAllocatorType::Fixed,
            2 => BlkAllocatorType::Varsize,
            3 => BlkAllocatorType::Append,
            _ => BlkAllocatorType::None,
        }
    }

    pub fn to_u8(self) -> u8 {
        self as u8
    }
}

// Note: ChunkSelectorType moved to chunk_selector.rs
// Note: BlkId, BlkAllocStatus, and BlkAllocHints are now imported from crate::common

impl VirtualDev {
    /// Create a new VirtualDev with chunks allocated across physical devices
    /// Create a new VirtualDev
    /// 
    /// # Arguments
    /// * `vparam` - VDev parameters
    /// * `vdev_id` - Pre-allocated VDev ID (from DeviceManager)
    /// * `pdevs` - Physical devices to use
    pub async fn create(
        mut vparam: VDevParameters,
        vdev_id: u32,
        pdevs: &[Arc<PhysicalDev>],
    ) -> io::Result<Self> {
        // Validate we have physical devices
        if pdevs.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Unable to find any pdevs for given vdev type, can't create vdev",
            ));
        }

        // Validate blk_size is multiple of pdev align_size
        let align_size = pdevs[0].align_size();
        if vparam.blk_size % align_size != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("blk_size {} should be multiple of pdev align_size {}", vparam.blk_size, align_size),
            ));
        }

        // Pick physical devices based on multi_pdev_opts
        // This returns a new Vec containing only the selected pdevs
        let selected_pdevs = Self::pick_pdevs(pdevs, vparam.multi_pdev_opts)?;

        // Early return: Create empty VirtualDev (typically for dynamic expansion)
        if vparam.num_chunks == 0 {
            println!("New Virtual Dev={} with id={} type={:?} (no initial chunks)",
                vparam.vdev_name, vdev_id, vparam.size_type);
            
            let mut vdev_info = VDevInfo {
                vdev_id,
                vdev_size: 0,  // Will grow if dynamic
                num_mirrors: vparam.num_mirrors,
                blk_size: vparam.blk_size,
                num_primary_chunks: 0,
                chunk_size: vparam.chunk_size as u32,
                size_type: vparam.size_type.to_u8(),
                slot_allocated: 0x01,
                failed: 0x00,
                hs_dev_type: vparam.dev_type.to_u8(),
                multi_pdev_choice: vparam.multi_pdev_opts as u8,
                name: [0u8; 64],
                checksum: 0,
                alloc_type: vparam.alloc_type.to_u8(),
                chunk_sel_type: vparam.chunk_sel_type.to_u8(),
                use_slab_allocator: 0,
                padding: [0u8; 154],
                user_private: [0u8; 256],
            };
            vdev_info.set_name(&vparam.vdev_name);
            vdev_info.compute_checksum();

            return Ok(Self {
                name: vdev_info.get_name(),
                vdev_id: vdev_info.vdev_id,
                hs_dev_type: vparam.dev_type,
                blk_size: vdev_info.blk_size,
                multi_pdev_choice: vparam.multi_pdev_opts,
                size_type: vparam.size_type,
                allocator_type: BlkAllocatorType::from_u8(vdev_info.alloc_type),
                chunk_selector_type: ChunkSelectorType::from_u8(vdev_info.chunk_sel_type),
                use_slab_allocator: vdev_info.use_slab_allocator != 0,
                incremental_chunk_size: vparam.incremental_chunk_size,
                chunk_pool: vparam.chunk_pool_limit.map(|limit| Mutex::new(ChunkPool::new(limit))),
                chunk_mgmt_mutex: Mutex::new(()),
                mutable_state: ArcSwap::from_pointee(VDevMutableState {
                    vdev_info,
                    pdevs: HashSet::new(),
                    all_chunks: HashMap::new(),
                    chunks_by_creation_order: Vec::new(),
                    total_chunk_num: 0,
                    next_creation_order: 0,
                }),
                chunk_selector: super::chunk_selector::ChunkSelector::new(vparam.size_type),
            });
        }

        // Normal path: Create VirtualDev with initial chunks
        let input_vdev_size = vparam.vdev_size;
        Self::adjust_vdev_params(&mut vparam)?;

        println!("New Virtual Dev={} of size={} with id={} multi_pdev_opts={:?}. \
            Params: VDev_Size={} Num_pdevs={} Total_chunks={} Chunk_Size={}",
            vparam.vdev_name, input_vdev_size, vdev_id, vparam.multi_pdev_opts,
            vparam.vdev_size, selected_pdevs.len(), vparam.num_chunks, vparam.chunk_size);

        // Create VDevInfo (on-disk format)
        let mut vdev_info = VDevInfo {
            vdev_id,
            vdev_size: vparam.vdev_size,
            num_mirrors: vparam.num_mirrors,
            blk_size: vparam.blk_size,
            num_primary_chunks: vparam.num_chunks,
            chunk_size: vparam.chunk_size as u32, // Convert u64 to u32 for on-disk format
            size_type: vparam.size_type.to_u8(),
            slot_allocated: 0x01, // Mark as allocated
            failed: 0x00,
            hs_dev_type: vparam.dev_type.to_u8(),
            multi_pdev_choice: vparam.multi_pdev_opts as u8,
            name: [0u8; 64],
            checksum: 0,
            alloc_type: vparam.alloc_type.to_u8(),
            chunk_sel_type: vparam.chunk_sel_type.to_u8(),
            use_slab_allocator: 0, // TODO: Add to parameters
            padding: [0u8; 154],
            user_private: [0u8; 256],
        };
        vdev_info.set_name(&vparam.vdev_name);
        vdev_info.compute_checksum();

        // Create VirtualDev instance
        let mut vdev = Self {
            name: vdev_info.get_name(),
            vdev_id: vdev_info.vdev_id,
            hs_dev_type: vparam.dev_type,
            blk_size: vdev_info.blk_size,
            multi_pdev_choice: vparam.multi_pdev_opts,
            size_type: vparam.size_type,
            allocator_type: BlkAllocatorType::from_u8(vdev_info.alloc_type),
            chunk_selector_type: ChunkSelectorType::from_u8(vdev_info.chunk_sel_type),
            use_slab_allocator: vdev_info.use_slab_allocator != 0,
            incremental_chunk_size: vparam.incremental_chunk_size,
            chunk_pool: vparam.chunk_pool_limit.map(|limit| Mutex::new(ChunkPool::new(limit))),
            chunk_mgmt_mutex: Mutex::new(()),
            mutable_state: ArcSwap::from_pointee(VDevMutableState {
                vdev_info,
                pdevs: HashSet::new(),
                all_chunks: HashMap::new(),
                chunks_by_creation_order: Vec::new(),
                total_chunk_num: 0,
                next_creation_order: 0,  // Will be updated as chunks are added
            }),
            chunk_selector: super::chunk_selector::ChunkSelector::new(vparam.size_type),
        };

        // Calculate total size of all selected pdevs
        let total_type_size: u64 = selected_pdevs.iter().map(|p| p.data_size()).sum();
        println!("Total size of type {:?} is {}", vparam.dev_type, total_type_size);

        let mut total_created_chunks = 0u32;

        // Distribute chunks across pdevs based on their capacity
        for pdev_arc in selected_pdevs.iter() {
            if total_created_chunks >= vparam.num_chunks {
                break;
            }
            let pdev_data_size = pdev_arc.data_size();
            let total_chunk_num_in_pdev =
                (vparam.num_chunks as f64 * (pdev_data_size as f64 / total_type_size as f64)) as u32;

            if vparam.num_chunks < total_chunk_num_in_pdev {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Chunks in pdev {} is {}, larger than total chunks {}",
                        pdev_arc.get_devname(), total_chunk_num_in_pdev, vparam.num_chunks
                    ),
                ));
            }

            println!("{} chunks created on pdev {} for vdev {}, pdev data size is {}",
                total_chunk_num_in_pdev, pdev_arc.get_devname(), vparam.vdev_name, pdev_data_size);

            // Create chunks on physical device (PhysicalDev allocates chunk IDs using formula)
            // start_ordinal = 0 for initial chunks
            let chunks: Vec<Arc<Chunk>> = pdev_arc.create_chunks(
                vdev_id,
                total_chunk_num_in_pdev,
                vparam.chunk_size,
                /*start_ordinal=*/0
            ).await?;
            
            // Add chunks to VDev state
            for chunk in chunks.into_iter() {
                vdev.on_chunk_found(chunk)?;
            }

            total_created_chunks += total_chunk_num_in_pdev;
        }

        println!("{} chunks created for vdev {}, expected {}", total_created_chunks, vparam.vdev_name, 
               vparam.num_chunks);

        // Create chunk selector now that all chunks are added
        vdev.create_chunk_selector();

        // Construct block allocators for all chunks (fresh creation). Let the blk allocator allocate the buffer
        let all_chunks = vdev.mutable_state.load().all_chunks.clone();
        for (_chunk_id, chunk) in &all_chunks {
            vdev.construct_blk_allocator(chunk, None)?;
        }

        // Write vdev_info to super block on ALL pdevs (system-wide metadata)
        // NOTE: vdev_size and num_primary_chunks are "best effort" hints
        //       On recovery, these will be recomputed from ChunkInfo (source of truth)
        //       This makes VDev creation crash-safe: even if this write fails,
        //       orphan detection will clean up chunks that have no corresponding VDev.
        vdev.write_vdev_info().await?;

        println!("Virtual Dev={} of size={} successfully created", vparam.vdev_name, vparam.vdev_size);
        Ok(vdev)
    }

    /// Load an existing VirtualDev from disk (recovery)
    /// Chunks should be added via add_chunk() after calling this
    /// Call create_chunk_selector() after all chunks are added
    pub fn load(vdev_info: VDevInfo) -> Self {
        let size_type = if vdev_info.size_type == VDevSizeType::Dynamic.to_u8() {
            VDevSizeType::Dynamic
        } else {
            VDevSizeType::Static
        };
        
        let multi_pdev_choice = match vdev_info.multi_pdev_choice {
            0 => MultiPDevOpts::AllPDevStriped,
            1 => MultiPDevOpts::AllPDevMirrored,
            2 => MultiPDevOpts::SingleFirstPDev,
            3 => MultiPDevOpts::SingleRandomPDev,
            _ => MultiPDevOpts::SingleFirstPDev,
        };
        
        // For dynamic VDevs, use chunk_size as incremental size and enable pooling
        // For static VDevs, these fields are unused but still need to be initialized
        let incremental_chunk_size = vdev_info.chunk_size as u64;
        let enable_pooling = size_type == VDevSizeType::Dynamic;
        
        Self {
            name: vdev_info.get_name(),
            vdev_id: vdev_info.vdev_id,
            hs_dev_type: HSDevType::from_u8(vdev_info.hs_dev_type),
            blk_size: vdev_info.blk_size,
            multi_pdev_choice,
            size_type,
            allocator_type: BlkAllocatorType::from_u8(vdev_info.alloc_type),
            chunk_selector_type: ChunkSelectorType::from_u8(vdev_info.chunk_sel_type),
            use_slab_allocator: vdev_info.use_slab_allocator != 0,
            incremental_chunk_size,
            chunk_pool: None, // No pooling by default during recovery, call enable_chunk_pooling() to enable
            chunk_mgmt_mutex: Mutex::new(()),
            mutable_state: ArcSwap::from_pointee(VDevMutableState {
                vdev_info,
                pdevs: HashSet::new(),
                all_chunks: HashMap::new(),
                chunks_by_creation_order: Vec::new(),
                total_chunk_num: 0,
                next_creation_order: 0,  // Will be updated to max(chunk creation_orders) + 1 as chunks are loaded
            }),
            chunk_selector: super::chunk_selector::ChunkSelector::new(size_type),
        }
    }

    //
    // ================ Initialization Helper Internal Methods ================
    //

    /// Create a chunk selector based on the selector type
    /// Must be called after all chunks are added
    ///
    /// Optimization: Automatically uses OnlyOneChunkSelector for single-chunk vdevs,
    /// regardless of requested type. This is the most common case in homestore deployments
    /// and avoids any overhead from random/modulo/indexing operations.
    pub fn create_chunk_selector(&self) {
        let chunks: Vec<Arc<Chunk>> = self.mutable_state.load().all_chunks.values().cloned().collect();
        
        // Optimization: Use OnlyOneChunkSelector for single chunk (most common case)
        if chunks.len() == 1 {
            self.chunk_selector.update(Box::new(OnlyOneChunkSelector::new(chunks)));
            return;
        }
        
        let selector: Box<dyn super::chunk_selector::ChunkSelectorInner> = match self.chunk_selector_type {
            ChunkSelectorType::RoundRobin => {
                Box::new(RoundRobinChunkSelector::new(chunks))
            }
            ChunkSelectorType::Random => {
                Box::new(RandomChunkSelector::new(chunks))
            }
            ChunkSelectorType::MostAvailableSpace => {
                Box::new(MostAvailableSpaceSelector::new(chunks))
            }
            ChunkSelectorType::OnlyOne => {
                Box::new(OnlyOneChunkSelector::new(chunks))
            }
            ChunkSelectorType::Custom => {
                // For now, use MostAvailableSpace for Custom
                // TODO: Allow custom selector registration
                Box::new(MostAvailableSpaceSelector::new(chunks))
            }
        };
        
        self.chunk_selector.update(selector);
    }

    /// Enable chunk pooling with a specified limit
    /// 
    /// This should be called after load() for dynamic VDevs if chunk pooling is desired.
    /// When enabled, shrunk chunks are deactivated and kept in a pool for reuse by expand().
    /// 
    /// # Arguments
    /// * `pool_limit` - Maximum number of chunks to keep in pool per chunk size
    /// 
    /// # Note
    /// - Only applicable to Dynamic VDevs
    /// - If already enabled, this will replace the existing pool with a new limit
    pub fn enable_chunk_pooling(&mut self, pool_limit: usize) {
        self.chunk_pool = Some(Mutex::new(ChunkPool::new(pool_limit)));
        println!("VirtualDev '{}': Enabled chunk pooling with limit={}", self.name, pool_limit);
    }

    /// Expand VirtualDev by adding a new chunk
    /// 
    /// Returns the newly created chunk so caller can create metablks.
    /// Only works for Dynamic vdevs.
    /// 
    /// # Arguments
    /// * `stream_id` - Stream ID to assign to this chunk (must be non-zero)
    /// * `chunk_size` - Size of chunk to allocate
    /// * `pdevs` - Physical devices to allocate from
    /// * `allocate_chunk_id` - Function to allocate next chunk ID
    /// Expand VDev with a new chunk
    /// 
    /// PhysicalDev will allocate the chunk_id using the formula.
    /// The chunk will be assigned the next creation_order (monotonically increasing).
    /// 
    /// # Arguments
    /// * `chunk_size` - Size of chunk to allocate
    /// * `pdevs` - Physical devices to allocate from
    /// 
    /// # Returns
    /// The newly created chunk
    pub async fn expand(
        &self,
        chunk_size: u64,
    ) -> io::Result<Arc<Chunk>> {
        // Serialize all chunk management operations to prevent lost updates
        let _guard = self.chunk_mgmt_mutex.lock().unwrap();
        
        // RCU Step 1: Load current state (atomic pointer load - fast!)
        let old_state = self.mutable_state.load_full();
        
        // Check that this is a dynamic vdev (using cached field - no atomic load!)
        if self.size_type != VDevSizeType::Dynamic {
            return Err(io::Error::new(io::ErrorKind::Unsupported, "Cannot expand static vdev"));
        }
        
        // === OPTIMIZATION: Check chunk pool first (if enabled) ===
        if let Some(ref pool) = self.chunk_pool {
            if let Some(chunk) = pool.lock().unwrap().try_get_chunk(chunk_size) {
                println!("Reusing pooled chunk {} (size={}) with new creation_order={}", 
                chunk.chunk_id(), chunk_size, old_state.next_creation_order);
                
                // Reactivate chunk with new creation_order (updates in-place)
                chunk.physical_dev().reactivate_chunk(&chunk, old_state.next_creation_order).await?;
                
                // Add chunk to VDev state via on_chunk_found()
                // Upper layer must call init_blk_allocator() after expand()
                self.on_chunk_found(chunk.clone())?;
                
                println!("VirtualDev '{}': Reused pooled chunk_id={}", self.name, chunk.chunk_id());
                return Ok(chunk);
            }
        }
        
        // === No pooled chunk available, create new ===
        // Get pdevs from DeviceManager (cached field - no atomic load!)
        let device_mgr = crate::common::managers::device_mgr();
        let pdevs = device_mgr.get_pdevs_by_dev_type(self.hs_dev_type);
        if pdevs.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("No physical devices of type {:?} available", self.hs_dev_type)));
        }
        
        // Pick physical device using multi_pdev_opts strategy (cached field - no atomic load!)
        let selected_pdevs = Self::pick_pdevs(&pdevs, self.multi_pdev_choice)?;
        let pdev = selected_pdevs.get(0).ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotFound, "No physical devices available")
        })?;
        
        // RCU Step 2: Create chunk (async operation, no state held)
        // PhysicalDev allocates chunk_id using formula: chunk_id = (pdev_id + 1) * slot_number
        // Use next_creation_order from state (monotonically increasing - represents creation order)
        let num_chunks_to_create = 1u32;  // Currently expand creates 1 chunk at a time
        let chunks = pdev.create_chunks(
            self.vdev_id,  // Use cached field - no atomic load!
            num_chunks_to_create,
            chunk_size,
            old_state.next_creation_order,  // Use monotonically increasing creation_order
        ).await?;
        
        let chunk = chunks.into_iter().next().ok_or_else(|| {
            io::Error::new(io::ErrorKind::Other, "Failed to create chunk")
        })?;
        
        // Add chunk to VDev state via on_chunk_found() - single source of truth for state management
        // Upper layer must call init_blk_allocator() after expand()
        self.on_chunk_found(chunk.clone())?;
        
        println!("VirtualDev '{}': Expanded with new chunk_id={}, size={}", 
                 self.name, chunk.chunk_id(), chunk_size);
        
        Ok(chunk)
    }
    
    /// Shrink VirtualDev by removing a chunk
    /// 
    /// If chunk pooling is enabled, the chunk is deactivated and added to the pool for reuse.
    /// If pooling is disabled, the chunk is permanently removed from the physical device.
    /// 
    /// In both cases, the chunk is removed from VDev's all_chunks map.
    /// 
    /// # Arguments
    /// * `which` - Which chunk to shrink (Last or Specific(chunk_id))
    /// 
    /// # Returns
    /// Ok(chunk_id) - ID of the chunk that was shrunk, or error if:
    /// - Chunk not found
    /// - Not a dynamic VDev
    /// - No chunks available to shrink
    pub async fn shrink(&self, which: ChunkToShrink) -> io::Result<u32> {
        // Serialize all chunk management operations to prevent lost updates
        let _guard = self.chunk_mgmt_mutex.lock().unwrap();
        
        // Check that this is a dynamic vdev
        if self.size_type != VDevSizeType::Dynamic {
            return Err(io::Error::new(io::ErrorKind::Unsupported, "Cannot shrink static vdev"));
        }
        
        // Get the chunk to remove (INSIDE mutex - thread-safe!)
        let state = self.mutable_state.load();
        let chunk = match which {
            ChunkToShrink::Last => {
                // Get last chunk by creation_order (safest for sequential shrinking)
                state.chunks_by_creation_order.last()
                    .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "No chunks to shrink"))?
                    .clone()
            }
            ChunkToShrink::Specific(chunk_id) => {
                state.all_chunks.get(&chunk_id)
                    .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, 
                        format!("Chunk {} not found", chunk_id)))?
                    .clone()
            }
        };
        
        let chunk_id = chunk.chunk_id();
               
        // Remove from VDev state
        drop(state);
        let old_state = self.mutable_state.load_full();
        let mut new_state = (*old_state).clone();
        new_state.all_chunks.remove(&chunk_id);
        new_state.total_chunk_num -= 1;
        
        // Update vdev_size incrementally (keep size() accurate)
        new_state.vdev_info.vdev_size -= chunk.info().chunk_size as u64;
        new_state.vdev_info.num_primary_chunks -= 1;
        
        new_state.chunks_by_creation_order = new_state.all_chunks.values().cloned().collect();
        new_state.chunks_by_creation_order.sort_by_key(|c| c.creation_order());
        self.mutable_state.store(Arc::new(new_state));
        
        let pdev = chunk.physical_dev();
        
        // If pooling enabled: check if pool has room, then deactivate + add to pool
        // Otherwise: directly remove from pdev
        if let Some(ref pool) = self.chunk_pool {
            if pool.lock().unwrap().has_room(chunk.info().chunk_size) {
                pdev.deactivate_chunk(&chunk).await?;
                pool.lock().unwrap().return_chunk(chunk.clone());
                println!("Chunk {} deactivated and moved to pool", chunk_id);
            } else {
                pdev.remove_chunk(&chunk).await?;
                println!("Chunk {} removed (pool full)", chunk_id);
            }
        } else {
            pdev.remove_chunk(&chunk).await?;
            println!("Chunk {} removed", chunk_id);
        }
        
        Ok(chunk_id)
    }
    
    /// Get the Nth chunk (0-indexed), creating it if it doesn't exist
    /// 
    /// Returns (chunk, is_newly_created) where is_newly_created indicates if
    /// the chunk was just expanded. If true, upper layer should call init_blk_allocator().
    /// 
    /// # Arguments
    /// * `n` - Zero-based chunk index in creation order
    /// 
    /// # Returns
    /// Ok((Arc<Chunk>, bool)) - chunk and whether it was newly created
    pub async fn get_or_create_nth_chunk(&self, n: usize) -> io::Result<(Arc<Chunk>, bool)> {
        // Check if chunk already exists
        let state = self.mutable_state.load();
        
        if let Some(existing_chunk) = state.chunks_by_creation_order.get(n) {
            return Ok((existing_chunk.clone(), false));
        }
        
        let current_count = state.chunks_by_creation_order.len();
        
        if n != current_count {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Cannot create chunk at position {} - current count is {}. Chunks must be created sequentially.", n, current_count)
            ));
        }
        
        drop(state);
        let chunk = self.expand(self.incremental_chunk_size).await?;
        Ok((chunk, true))
    }
    
    /// Pick physical devices based on multi_pdev_opts
    /// Returns a new Vec containing the selected pdevs
    fn pick_pdevs(
        pdevs: &[Arc<PhysicalDev>],
        multi_pdev_opts: MultiPDevOpts,
    ) -> io::Result<Vec<Arc<PhysicalDev>>> {
        if pdevs.is_empty() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "No pdevs available"));
        }

        let total_pdevs = pdevs.len();

        // Determine which indices to select
        let indices: Vec<usize> = match multi_pdev_opts {
            MultiPDevOpts::AllPDevStriped => {
                // Use all pdevs for striping
                (0..pdevs.len()).collect()
            }
            MultiPDevOpts::AllPDevMirrored => {
                return Err(io::Error::new(io::ErrorKind::Unsupported, "AllPDevMirrored is not yet supported"));
            }
            MultiPDevOpts::SingleFirstPDev => {
                // Use only the first pdev
                vec![0]
            }
            MultiPDevOpts::SingleRandomPDev => {
                // Pick a random pdev
                use std::time::{SystemTime, UNIX_EPOCH};
                let seed = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos() as u64;
                let random_idx = (seed % pdevs.len() as u64) as usize;
                vec![random_idx]
            }
        };

        println!("Selected {} pdev(s) out of {} for multi_pdev_opts={:?}", indices.len(), total_pdevs, multi_pdev_opts);

        // Build the selected pdevs vector by cloning Arc pointers
        let selected: Vec<Arc<PhysicalDev>> = indices
            .iter()
            .map(|&idx| Arc::clone(&pdevs[idx]))
            .collect();

        Ok(selected)
    }

    /// Adjust vdev parameters based on size_type
    fn adjust_vdev_params(vparam: &mut VDevParameters) -> io::Result<()> {
        const MIN_CHUNK_SIZE: u64 = 16 * 1024 * 1024; // 16MB
        const MAX_CHUNKS_IN_SYSTEM: u32 = 65535;

        let max_num_chunks = std::cmp::min((vparam.vdev_size / MIN_CHUNK_SIZE) as u32, MAX_CHUNKS_IN_SYSTEM);

        let input_vdev_size = vparam.vdev_size;

        // Adjust parameters (assuming static size type)
        if vparam.vdev_size == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "Vdev size can't be 0"));
        }

        if vparam.num_chunks != 0 {
            // Calculate chunk_size from num_chunks
            let input_num_chunks = vparam.num_chunks;
            let min_num_chunks = (vparam.vdev_size - 1) / (Chunk::MAX_CHUNK_SIZE as u64) + 1;
            vparam.num_chunks = std::cmp::max(vparam.num_chunks, min_num_chunks as u32);
            vparam.num_chunks = std::cmp::min(vparam.num_chunks, max_num_chunks);

            if input_num_chunks != vparam.num_chunks {
                println!(
                    "{} Virtual device attempted with num_chunks={}, adjusted to {}",
                    vparam.vdev_name, input_num_chunks, vparam.num_chunks
                );
            }

            // Ensure chunk_size % blk_size == 0
            vparam.vdev_size = (vparam.vdev_size / (vparam.num_chunks as u64 * vparam.blk_size as u64))
                * (vparam.num_chunks as u64 * vparam.blk_size as u64);
            if input_vdev_size != vparam.vdev_size {
                println!(
                    "{} Virtual device size adjusted from {} to {}",
                    vparam.vdev_name, input_vdev_size, vparam.vdev_size
                );
            }
            vparam.chunk_size = vparam.vdev_size / vparam.num_chunks as u64;
        } else if vparam.chunk_size != 0 {
            // Calculate num_chunks from chunk_size
            let input_chunk_size = vparam.chunk_size;
            vparam.chunk_size = std::cmp::max(vparam.chunk_size, MIN_CHUNK_SIZE);
            vparam.chunk_size = ((vparam.chunk_size + vparam.blk_size as u64 - 1) / vparam.blk_size as u64)
                * vparam.blk_size as u64;
            if input_chunk_size != vparam.chunk_size {
                println!(
                    "{} Virtual device chunk_size adjusted from {} to {}",
                    vparam.vdev_name, input_chunk_size, vparam.chunk_size
                );
            }

            vparam.vdev_size = (vparam.vdev_size / vparam.chunk_size) * vparam.chunk_size;
            if input_vdev_size != vparam.vdev_size {
                println!(
                    "{} Virtual device size adjusted from {} to {}",
                    vparam.vdev_name, input_vdev_size, vparam.vdev_size
                );
            }

            vparam.num_chunks = (vparam.vdev_size / vparam.chunk_size) as u32;
        } else {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Both num_chunks and chunk_size can't be zero for vdev",
            ));
        }

        // Sanity checks
        const MIN_CHUNK_SIZE_CHECK: u64 = 16 * 1024 * 1024; // 16MB
        if vparam.vdev_size % vparam.chunk_size != 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "vdev_size should be multiple of chunk_size"));
        }
        if vparam.chunk_size % vparam.blk_size as u64 != 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "chunk_size should be multiple of blk_size"));
        }
        if vparam.chunk_size < MIN_CHUNK_SIZE_CHECK {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "chunk_size should be >= min_chunk_size"));
        }
        if vparam.num_chunks > max_num_chunks {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "num_chunks should be <= max_num_chunks"));
        }

        Ok(())
    }

    /// Construct a block allocator instance and attach it to the chunk
    fn construct_blk_allocator(&self, chunk: &Arc<Chunk>, buffer: Option<IOBuffer>) -> io::Result<()> {
        use iomgr::iomgr;

        use crate::blkalloc::{
            fixed_blk_allocator::FixedBlkAllocator,
            varsize_blk_allocator::{VarsizeBlkAllocConfig, VarsizeBlkAllocator},
            empty_blk_allocator::EmptyBlkAllocator,
            BlkAllocConfig,
        };

        let chunk_id = chunk.chunk_id();
        let chunk_size = chunk.info().chunk_size;
        let vblock_size = self.block_size();

        // Get physical device properties through chunk
        // TODO: We need to get PhysicalDev from chunk to access optimal_page_size and align_size
        // For now, use hardcoded values as placeholders
        let _ppage_sz = 4096u32; // Placeholder - should be chunk->physical_dev()->optimal_page_size()
        let align_sz = 512u32; // Placeholder - should be chunk->physical_dev()->align_size()

        let num_reactors = iomgr().num_reactors() as u32;       
        // Auto recovery is determined by whether buffer is provided
        let is_auto_recovery = buffer.is_some();

        let allocator: Box<dyn BlkAllocator> = match self.allocator_type {
            BlkAllocatorType::Fixed => {
                let name = format!("fixed_chunk_{}", chunk_id);
                let cfg = BlkAllocConfig::new(
                    vblock_size, align_sz, chunk_size, is_auto_recovery, // persistent
                    name, 1, // num_segments - Fixed allocator uses 1
                );

                let allocator = FixedBlkAllocator::new(&cfg, buffer, chunk_id as u16, num_reactors)
                    .map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;

                Box::new(allocator)
            }
            BlkAllocatorType::Varsize => {
                let name = format!("varsize_chunk_{}", chunk_id);

                // Determine if we should use slabs
                // TODO: Add is_data_drive_hdd() check similar to C++ line 71
                let use_slabs = self.use_slab_allocator;

                // Varsize-specific config
                let nsegments = 1u32; // Placeholder - may need to be configurable
                let max_cache_blks_per_slab = 256; // Placeholder
                let sweep_min_free_pct = 10u8; // Placeholder
                let max_slab_cache_entries = 1024; // Placeholder
                let enable_slab_allocation = use_slabs;

                let base_cfg =
                    BlkAllocConfig::new(vblock_size, align_sz, chunk_size, is_auto_recovery, name, nsegments);

                let cfg = VarsizeBlkAllocConfig::new(
                    base_cfg,
                    nsegments,
                    max_cache_blks_per_slab,
                    use_slabs,
                    sweep_min_free_pct,
                    max_slab_cache_entries,
                    enable_slab_allocation,
                );

                let allocator = VarsizeBlkAllocator::new(cfg, buffer, chunk_id as u16, num_reactors)
                    .map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;

                Box::new(allocator)
            }
            BlkAllocatorType::Append => {
                // TODO: Implement AppendBlkAllocator
                // For now, return error
                return Err(io::Error::new(io::ErrorKind::Unsupported, "AppendBlkAllocator not yet implemented"));
            }
            BlkAllocatorType::None => {
                // Create an EmptyBlkAllocator for log streams
                let allocator = EmptyBlkAllocator::new();
                Box::new(allocator)
            }
        };

        // Attach the allocator to the chunk
        chunk.set_block_allocator(allocator);

        Ok(())
    }
      
    //
    // ================ Chunk Management Helper Internal Methods ================
    //
    
    /// Add a chunk to this virtual device (without allocator)
    /// Note: In create() flow, allocators are automatically constructed after all chunks are added
    /// In load() flow, call load_blk_allocator() with recovery buffers after all chunks are added
    /// Called during recovery when a chunk belonging to this VDev is found
    /// 
    /// This method is called by DeviceManager during load_vdevs() for each
    /// chunk that belongs to this VDev. It updates the VDev's internal state.
    pub fn on_chunk_found(&self, chunk: Arc<Chunk>) -> io::Result<()> {
        // Serialize all chunk management operations to prevent lost updates
        let _guard = self.chunk_mgmt_mutex.lock().unwrap();
        
        let chunk_id = chunk.chunk_id();
        let is_allocated = chunk.info().is_allocated();
        
        // Inactive chunks go to pool, NOT to all_chunks
        if !is_allocated {
            println!("Found inactive chunk {} during recovery, adding to pool", chunk_id);
            
            if let Some(ref pool) = self.chunk_pool {
                pool.lock().unwrap().return_chunk(chunk);
            }
            
            // Do NOT add to all_chunks - inactive chunks stay out
            return Ok(());
        }
        
        // Active chunk - add to VDev state
        // Invariant: all_chunks contains ONLY active/allocated chunks
        let old_state = self.mutable_state.load_full();
        let mut new_state = (*old_state).clone();
        
        // Add chunk to HashMap (indexed by chunk_id for fast I/O lookup)
        new_state.pdevs.insert(chunk.info().vdev_id);
        new_state.all_chunks.insert(chunk_id, Arc::clone(&chunk));
        new_state.total_chunk_num += 1;
        
        // Update vdev_size incrementally (keep size() accurate)
        new_state.vdev_info.vdev_size += chunk.info().chunk_size as u64;
        new_state.vdev_info.num_primary_chunks += 1;
        
        // Update next_creation_order to be max(current, chunk_creation_order + 1)
        new_state.next_creation_order = new_state.next_creation_order.max(chunk.creation_order() + 1);
        
        // Rebuild sorted chunk list (by creation_order for sequential access)
        new_state.chunks_by_creation_order = new_state.all_chunks.values().cloned().collect();
        new_state.chunks_by_creation_order.sort_by_key(|c| c.creation_order());
        
        // SAFE: Protected by chunk_mgmt_mutex - no concurrent modifications possible
        self.mutable_state.store(Arc::new(new_state));

        Ok(())
    }

    /// Update runtime VDevInfo statistics from actual chunks (ChunkInfo as source of truth)
    /// 
    /// This is primarily used after recovery to recompute accurate vdev_size and num_primary_chunks
    /// from the actual chunks loaded, making ChunkInfo the single source of truth.
    /// 
    /// During normal operation, vdev_size is maintained incrementally by on_chunk_found() and shrink(),
    /// so calling this is not necessary. However, it can be used to verify/fix size inconsistencies.
    /// 
    /// Fields updated (in-memory only, not persisted):
    /// - vdev_size: Sum of all chunk sizes
    /// - num_primary_chunks: Count of all chunks
    pub fn adjust_vdev_info(&self) {
        let state = self.mutable_state.load();
        
        let actual_size: u64 = state.all_chunks.values()
            .map(|chunk| chunk.info().chunk_size as u64)
            .sum();
        
        let actual_count = state.all_chunks.len() as u32;
        
        // Update VDevInfo in mutable_state (in-memory only)
        // We need to do RCU update even though we're not persisting
        drop(state);
        
        let old_state = self.mutable_state.load_full();
        let mut new_state = (*old_state).clone();
        new_state.vdev_info.vdev_size = actual_size;
        new_state.vdev_info.num_primary_chunks = actual_count;
        self.mutable_state.store(Arc::new(new_state));
        
        println!("Updated runtime stats for VDev '{}': size={}, num_chunks={}", 
                 self.name, actual_size, actual_count);
    }

    /// Initialize block allocator(s)
    /// 
    /// Upper layer must call this explicitly after VDev creation or chunk expansion.
    /// 
    /// # Arguments
    /// * `chunk` - If Some(chunk), initialize only that chunk's allocator
    ///             If None, initialize allocators for all chunks
    /// 
    /// # Returns
    /// Ok(()) if successful
    pub fn init_blk_allocator(&self, chunk: Option<Arc<Chunk>>) -> io::Result<()> {
        match chunk {
            Some(chunk) => {
                // Initialize single chunk
                self.construct_blk_allocator(&chunk, None)?;
                Ok(())
            }
            None => {
                // Initialize all chunks
                let state = self.mutable_state.load();
                for chunk in state.all_chunks.values() {
                    self.construct_blk_allocator(chunk, None)?;
                }
                println!("Initialized block allocators for {} chunks", state.all_chunks.len());
                Ok(())
            }
        }
    }

    /// Get the buffer guard for a chunk's block allocator (for checkpoint persistence)
    /// 
    /// For bitmap-based allocators, returns a guard that provides zero-copy access to
    /// the underlying IOBuffer. Upper layer can call guard.buffer() to get &IOBuffer
    /// for metablk.write().
    /// 
    /// The guard ensures the buffer remains valid during checkpoint flush.
    /// 
    /// # Arguments
    /// * `chunk_id` - ID of chunk whose allocator buffer to get
    /// 
    /// # Returns
    /// Ok(Some(BitmapBufferGuard)) for bitmap allocators, Ok(None) for others
    pub async fn get_allocator_buffer(&self, chunk_id: u32) -> io::Result<Option<crate::blkalloc::bitmap_blk_allocator::BitmapBufferGuard<'_>>> {
        use crate::blkalloc::{
            bitmap_blk_allocator::BitmapBufferGuard,
            varsize_blk_allocator::VarsizeBlkAllocator,
        };
        
        let state = self.mutable_state.load();
        let chunk = state.all_chunks.get(&chunk_id)
            .ok_or_else(|| io::Error::new(
                io::ErrorKind::NotFound, 
                format!("Chunk {} not found", chunk_id)
            ))?
            .clone();
        
        drop(state);
        
        // Check allocator type and get buffer guard if bitmap-based
        match self.allocator_type {
            BlkAllocatorType::Fixed | BlkAllocatorType::Varsize => {
                let allocator_trait = chunk.blk_allocator_mut();
                let allocator = unsafe {
                    &mut *(allocator_trait as *mut dyn BlkAllocator as *mut VarsizeBlkAllocator)
                };
                
                // Return the guard directly - no cloning!
                let guard = allocator.acquire_buffer().await;
                Ok(Some(guard))
            }
            BlkAllocatorType::Append | BlkAllocatorType::None => {
                Ok(None)
            }
        }
    }

    /// Load block allocators from disk for recovery
    /// 
    /// # Arguments
    /// * `chunk_buffers` - Optional map of chunk_id -> IOBuffer with allocator state
    ///                     If None, constructs fresh allocators for all chunks
    pub fn load_blk_allocator(&self, chunk_buffers: Option<HashMap<u32, IOBuffer>>) -> io::Result<()> {
        // Load current chunks (RCU read - fast!)
        let chunks = self.mutable_state.load().all_chunks.clone();
        
        // Process ALL chunks - use buffer if available, otherwise construct fresh allocator
        let mut buffers = chunk_buffers;
        for chunk in chunks.values() {
            let chunk_id = chunk.chunk_id();
            let buffer = buffers.as_mut().and_then(|b| b.remove(&chunk_id));
            
            // Chunk has data if buffer is Some, otherwise construct fresh allocator
            self.construct_blk_allocator(chunk, buffer)?;
        }
    
        Ok(())
    }
    
    /// Write VDevInfo to all specified physical devices (mirrored for redundancy)
    /// Write VDevInfo to ALL physical devices (system-wide metadata)
    /// 
    /// VDevInfo is mirrored across all pdevs for redundancy and fast recovery.
    /// This is different from ChunkInfo which is pdev-specific.
    pub async fn write_vdev_info(&self) -> io::Result<()> {
        // Get all pdevs from DeviceManager
        let all_pdevs = crate::common::managers::device_mgr().get_all_pdevs();
        if all_pdevs.is_empty() {
            return Err(io::Error::new(io::ErrorKind::NotFound, "No physical devices available"));
        }
        
        // Get current VDevInfo from state
        let vdev_info = self.mutable_state.load().vdev_info;
        
        // VDevInfo is #[repr(C, packed)] so we can safely cast to bytes
        let vinfo_bytes = unsafe {
            std::slice::from_raw_parts(
                &vdev_info as *const VDevInfo as *const u8,
                std::mem::size_of::<VDevInfo>(),
            )
        };
        let mut vinfo_buf = IOBuffer::new(VDevInfo::SIZE);
        vinfo_buf.as_mut_slice()[..VDevInfo::SIZE].copy_from_slice(vinfo_bytes);

        let vdev_offset = VDevInfo::vdev_sb_offset(self.vdev_id);
        
        println!("Writing VDevInfo id={} to ALL {} physical devices", self.vdev_id, all_pdevs.len());
        
        // Write to ALL pdevs in parallel
        use futures::future::join_all;
        let writes: Vec<_> = all_pdevs.iter()
            .map(|pdev| pdev.write_super_block(&vinfo_buf, vdev_offset))
            .collect();
        
        join_all(writes).await.into_iter().collect::<io::Result<Vec<_>>>()?;
        
        Ok(())
    }

    /// Remove a chunk from this virtual device
    //
    // ================ I/O Operations ================
    //

    /// Write data to a block ID
    pub async fn write(&self, buf: &IOBuffer, bid: &BlkId) -> io::Result<()> {
        let (dev_offset, chunk) = self.to_dev_offset(bid)?;
        let pdev = chunk.physical_dev();
        
        println!("Writing to device: pdev_id={}, offset={}, size={}", 
            pdev.pdev_id(), dev_offset, buf.len());
        
        pdev.write(buf, dev_offset).await
    }

    /// Read data from a block ID
    pub async fn read(&self, buf: IOBuffer, bid: &BlkId) -> (io::Result<usize>, IOBuffer) {
        match self.to_dev_offset(bid) {
            Ok((dev_offset, chunk)) => {
                let pdev = chunk.physical_dev();
                pdev.read(buf, dev_offset).await
            }
            Err(e) => (Err(e), buf),
        }
    }

    /// Write multiple buffers (vectored I/O) to a block ID
    pub async fn writev(&self, buffers: Vec<IOBuffer>, bid: &BlkId) -> io::Result<()> {
        let (dev_offset, chunk) = self.to_dev_offset(bid)?;
        let pdev = chunk.physical_dev();
        
        let total_size: usize = buffers.iter().map(|b| b.len()).sum();
        println!("Writing vectored to device: pdev_id={}, offset={}, total_size={}", 
            pdev.pdev_id(), dev_offset, total_size);
        
        pdev.writev(buffers, dev_offset).await
    }

    /// Read multiple buffers (vectored I/O) from a block ID
    pub async fn readv(&self, buffers: Vec<IOBuffer>, bid: &BlkId) -> (io::Result<usize>, Vec<IOBuffer>) {
        match self.to_dev_offset(bid) {
            Ok((dev_offset, chunk)) => {
                let pdev = chunk.physical_dev();
                pdev.readv(buffers, dev_offset).await
            }
            Err(e) => (Err(e), buffers),
        }
    }

    /// Format the virtual device asynchronously (placeholder)
    /// Format the virtual device by writing zeros to all chunks
    /// 
    /// This efficiently zeros out all chunks in parallel using the underlying
    /// physical device's write_zero (which may use BLKZEROOUT ioctl for block devices).
    pub async fn format(&self) -> io::Result<()> {
        use futures::future::join_all;
        
        // Load chunks (RCU read - fast!)
        let all_chunks = self.mutable_state.load().all_chunks.clone();
        println!("Formatting VirtualDev {} with {} chunks", self.name, all_chunks.len());
        
        // Collect all write_zero futures
        let mut write_futures = Vec::new();
        
        for (&chunk_id, chunk) in &all_chunks {
            let pdev = chunk.physical_dev().clone();
            let chunk_size = chunk.info().chunk_size;
            let start_offset = chunk.start_offset();
            
            println!("Writing zeros for chunk: {}, size: {}, offset: {}", 
                chunk_id, chunk_size, start_offset);
            
            // Create future for this chunk's write_zero
            write_futures.push(async move {
                pdev.write_zero(chunk_size, start_offset).await
            });
        }
        
        // Wait for all write_zero operations to complete in parallel
        let results = join_all(write_futures).await;
        
        // Check if any failed
        for (idx, result) in results.iter().enumerate() {
            if let Err(e) = result {
                eprintln!("Error formatting chunk {}: {}", idx, e);
                return Err(io::Error::new(
                    io::ErrorKind::Other,
                    format!("Failed to format chunk {}: {}", idx, e)
                ));
            }
        }
        
        println!("VirtualDev {} formatted successfully", self.name);
        Ok(())
    }

    /// Fsync all physical devices used by this virtual device
    ///
    /// This flushes all pending writes to disk for all underlying physical devices.
    /// All pdevs are fsynced in parallel for efficiency.
    pub async fn fsync(&self) -> io::Result<()> {
        use futures::future::join_all;
        use std::collections::HashSet;
        
        // Load chunks (RCU read - fast!)
        let all_chunks = self.mutable_state.load().all_chunks.clone();
        
        // Collect unique physical devices from all chunks
        let mut unique_pdevs: HashSet<u32> = HashSet::new();
        let mut pdev_refs: Vec<Arc<PhysicalDev>> = Vec::new();
        
        for chunk in all_chunks.values() {
            let pdev = chunk.physical_dev();
            let pdev_id = pdev.pdev_id();
            
            if unique_pdevs.insert(pdev_id) {
                // First time seeing this pdev
                pdev_refs.push(pdev.clone());
            }
        }
        
        if pdev_refs.is_empty() {
            return Ok(());
        }
        
        println!("Fsyncing {} physical devices for VirtualDev {}", pdev_refs.len(), self.name);
        
        // Optimize for single pdev case
        if pdev_refs.len() == 1 {
            println!("Flushing pdev {}", pdev_refs[0].get_devname());
            return pdev_refs[0].fsync().await;
        }
        
        // Multiple pdevs: fsync all in parallel
        let mut fsync_futures = Vec::new();
        for pdev in &pdev_refs {
            let pdev_clone = pdev.clone();
            let devname = pdev.get_devname().to_string();
            println!("Flushing pdev {}", devname);
            
            fsync_futures.push(async move {
                pdev_clone.fsync().await
            });
        }
        
        // Wait for all fsync operations to complete
        let results = join_all(fsync_futures).await;
        for (idx, result) in results.iter().enumerate() {
            if let Err(e) = result {
                eprintln!("Error fsyncing pdev {}: {}", pdev_refs[idx].get_devname(), e);
                return Err(io::Error::new(
                    io::ErrorKind::Other,
                    format!("Failed to fsync pdev {}: {}", pdev_refs[idx].get_devname(), e)
                ));
            }
        }
        
        println!("VirtualDev {} fsynced successfully", self.name);
        Ok(())
    }

    /// Helper: Convert BlkId to device offset and get the chunk
    /// Returns (dev_offset, chunk_ref) or error if chunk not found
    fn to_dev_offset(&self, bid: &BlkId) -> io::Result<(u64, Arc<Chunk>)> {
        assert!(!bid.is_multi(), "write/read needs individual pieces of blkid - not MultiBlkid");
        
        let chunk_num = bid.chunk_num();
        let chunk = self.mutable_state.load().all_chunks
            .get(&(chunk_num as u32))
            .cloned()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, format!("Chunk {} not found", chunk_num)))?;
        
        let dev_offset = (bid.blk_num() as u64) * (self.block_size() as u64) + chunk.start_offset();
        Ok((dev_offset, chunk))
    }

    //
    // ================ Block Allocation Operations ================
    //

    /// Select a chunk for allocation with retry support
    ///
    /// On first attempt (last_failed_chunk_id = None), uses the chunk selector's strategy.
    /// On retry (last_failed_chunk_id = Some(id)), gets the next chunk after the failed one.
    /// This allows efficient retry across all chunks without external state tracking.
    ///
    /// Returns a cloned Arc<Chunk> (RCU pattern)
    fn select_chunk(&self, nblks: u32, hints: &BlkAllocHints, last_failed_chunk_id: Option<u32>) -> Option<Arc<Chunk>> {
        // If chunk_id_hint is provided, use that specific chunk (no retries)
        if let Some(chunk_id) = hints.chunk_id_hint {
            return self.mutable_state.load().all_chunks.get(&(chunk_id as u32)).cloned();
        }

        match last_failed_chunk_id {
            None => {
                // First attempt: use chunk selector's strategy
                self.chunk_selector.select_chunk(nblks, hints)
            }
            Some(failed_id) => {
                // Retry: get next chunk after the failed one
                self.chunk_selector.get_chunk_after(failed_id)
            }
        }
    }

    /// Allocate a single contiguous block (returns BlkId)
    pub async fn alloc_contiguous(&self, nblks: u32, hints: &BlkAllocHints) -> (BlkAllocStatus, Option<BlkId>) {
        // Ensure hints specify contiguous allocation
        let mut adjusted_hints = hints.clone();
        adjusted_hints.is_contiguous = true;
        
        let (status, blkids) = self.alloc(nblks, &adjusted_hints).await;
        if status == BlkAllocStatus::Success || (status == BlkAllocStatus::Partial && hints.partial_alloc_ok) {
            assert_eq!(blkids.len(), 1, "alloc_contiguous with is_contiguous=true must return exactly 1 BlkId, got {}", blkids.len());
            (status, blkids.first().copied())
        } else {
            (status, None)
        }
    }

    /// Allocate blocks (returns BlkIds)
    ///
    /// Attempts allocation with retries across chunks:
    /// 1. First attempt: uses chunk selector's strategy (round-robin, most-available, etc.)
    /// 2. On failure: retries with next chunks until all exhausted or success
    /// 3. Respects hints.can_look_for_other_chunk and hints.chunk_id_hint
    pub async fn alloc(&self, nblks: u32, hints: &BlkAllocHints) -> (BlkAllocStatus, BlkIds) {
        let mut last_failed_chunk_id: Option<u32> = None;
        let mut attempt = 0u64;
        let max_attempts = if hints.chunk_id_hint.is_some() { 
            1  // No retries for targeted allocation
        } else {
            self.mutable_state.load().total_chunk_num
        };
        
        loop {
            // Select chunk (first attempt or retry)
            let Some(chunk) = self.select_chunk(nblks, hints, last_failed_chunk_id) else {
                return (BlkAllocStatus::SpaceFull, BlkIds::new());
            };
            
            let chunk_id = chunk.chunk_id();
            let allocator = chunk.blk_allocator();
            
            // Allocate from chunk's allocator (async)
            let (status, blkids) = allocator.alloc(nblks as u16, hints).await;
            if status == BlkAllocStatus::Success 
                || (status == BlkAllocStatus::Partial && hints.partial_alloc_ok) {
                return (status, blkids);
            }
            
            // If we can't look for other chunks, stop
            if !hints.can_look_for_other_chunk || hints.chunk_id_hint.is_some() {
                return (status, blkids);
            }
            
            // Retry with next chunk
            attempt += 1;
            if attempt >= max_attempts {
                eprintln!("nblks={} failed to alloc after trying {} chunks", nblks, attempt);
                return (BlkAllocStatus::SpaceFull, BlkIds::new());
            }

            last_failed_chunk_id = Some(chunk_id);
        }
    }

    /// Free a block (async)
    pub async fn free(&self, bid: &BlkId) {
        let chunk_num = bid.chunk_num();
        if let Some(chunk) = self.mutable_state.load().all_chunks.get(&(chunk_num as u32)).cloned() {
            let allocator = chunk.blk_allocator();
            allocator.free(bid).await;
        } else {
            eprintln!("ERROR: Trying to free block in missing chunk {}", chunk_num);
        }
    }

    /// Free multiple blocks (async)
    /// 
    /// Takes a HashMap of chunk_id -> BlkIds to avoid walking the list multiple times.
    /// The caller groups BlkIds by chunk as they build the free list.
    /// This is critical for performance when freeing huge lists of blocks.
    pub async fn free_blks(&self, chunks_to_free: HashMap<u32, BlkIds>) {
        // Load chunks once (RCU read)
        let all_chunks = self.mutable_state.load();
        
        // Dispatch to each chunk's allocator - zero overhead!
        for (chunk_id, chunk_bids) in chunks_to_free {
            if let Some(chunk) = all_chunks.all_chunks.get(&chunk_id) {
                let allocator = chunk.blk_allocator();
                allocator.free_batch(&chunk_bids).await;
            } else {
                eprintln!("ERROR: Trying to free blocks in missing chunk {}", chunk_id);
            }
        }
    }

    /// Commit a block (for persistent allocators)
    pub fn commit_blk(&self, bid: &BlkId) -> BlkAllocStatus {
        let chunk_num = bid.chunk_num();
        let Some(chunk) = self.mutable_state.load().all_chunks.get(&(chunk_num as u32)).cloned() else {
            eprintln!("ERROR: fail to commit_blk: bid {:?}", bid);
            return BlkAllocStatus::InvalidDev;
        };
        
        let allocator = chunk.blk_allocator();
        allocator.schedule_commit(bid)
    }

    /// Destroy this virtual device (self-sufficient, handles everything)
    /// 
    /// Three-stage crash-safe destroy:
    /// - Stage 1: Mark VDevInfo.slot_allocated=0 and persist → crash-safe checkpoint
    /// - Stage 2: Free all chunks, clear resources
    /// - Stage 3: Notify DeviceManager (remove from registry, free vdev_id, commit bitmap)
    /// 
    /// If crash between stages, recovery detects as "zombied VDev" and cleans up
    pub async fn destroy(&self) -> io::Result<()> {
        // Serialize all chunk management operations to prevent lost updates
        let _guard = self.chunk_mgmt_mutex.lock().unwrap();
        
        println!("Destroying VirtualDev {} (id={})", self.name, self.vdev_id);
        
        // ========== STAGE 1: Mark VDevInfo as free (crash-safe checkpoint) ==========
        println!("Stage 1: Marking VDev {} as free", self.vdev_id);
        {
            // Mark VDevInfo as free in memory
            let old_state = self.mutable_state.load_full();
            let mut new_state = (*old_state).clone();
            new_state.vdev_info.set_free();
            new_state.vdev_info.compute_checksum();
            
            // SAFE: Protected by chunk_mgmt_mutex - no concurrent modifications possible
            self.mutable_state.store(Arc::new(new_state));
            
            // Persist to ALL pdevs (VDevInfo is system-wide metadata)
            self.write_vdev_info().await?;
        }
        println!("Stage 1 complete: VDevInfo.slot_allocated=0 persisted to ALL pdevs");
        
        // If crash happens here:
        // - VDevInfo.slot_allocated == 0
        // - VDev slot bitmap bit == 1
        // - Chunks may still exist
        // → Recovery will detect this as zombied VDev and call cleanup_zombied_vdevs()
        
        // ========== STAGE 2: Cleanup chunks and resources ==========
        println!("Stage 2: Cleaning up chunks and resources");
        
        // Collect chunk_ids and pdev_ids before cleanup
        let (chunk_ids, pdev_ids) = {
            let state = self.mutable_state.load();
            let mut chunk_ids = Vec::new();
            let mut pdev_ids: std::collections::HashSet<u32> = std::collections::HashSet::new();
            
            for chunk in state.all_chunks.values() {
                chunk_ids.push(chunk.info().chunk_id);
                pdev_ids.insert(chunk.physical_dev().pdev_id());
            }
            
            (chunk_ids, pdev_ids)
        };
        
        // Get PhysicalDev references from DeviceManager
        let device_mgr = crate::common::managers::device_mgr();
        let pdevs: Vec<Arc<PhysicalDev>> = pdev_ids.iter()
            .filter_map(|&id| device_mgr.get_pdev(id))
            .collect();
        
        println!("Destroying {} chunks across {} physical devices", chunk_ids.len(), pdevs.len());
        
        // Remove all chunks for this VDev from each pdev
        // PhysicalDev handles the details (ChunkInfo, chunk bitmap, vdev_chunks map)
        for pdev in &pdevs {
            pdev.remove_chunks_for_vdev(self.vdev_id).await?;
        }
        
        println!("VirtualDev {} cleanup complete, freed {} chunks", self.name, chunk_ids.len());
        
        // ========== STAGE 3: Notify DeviceManager ==========
        // Remove from registry, free vdev_id, commit bitmap
        // If this fails, VDev is already destroyed on disk - recovery will handle cleanup
        let device_mgr = crate::common::managers::device_mgr();
        if let Err(e) = device_mgr.on_vdev_destroyed(self.vdev_id).await {
            eprintln!("Warning: Failed to notify DeviceManager of VDev {} destruction: {}", 
                     self.vdev_id, e);
            eprintln!("VDev is destroyed on disk - recovery will cleanup on restart");
        }
        
        println!("VirtualDev {} (id={}) fully destroyed", self.name, self.vdev_id);
        Ok(())
    }

    /// Check if a block is allocated
    pub async fn is_blk_alloced(&self, blkid: &BlkId) -> bool {
        let chunk_num = blkid.chunk_num() as u32;
        
        if let Some(chunk) = self.mutable_state.load().all_chunks.get(&chunk_num).cloned() {
            let allocator = chunk.blk_allocator();
            // is_thread_safe=true since we might be checking from a different reactor
            allocator.is_blk_alloced(blkid, /*is_thread_safe=*/true).await
        } else {
            eprintln!("ERROR: Checking allocation for block in missing chunk {}", chunk_num);
            false
        }
    }

    //
    // ================ Getters and Setters ================
    //
    
    /// Get virtual device ID (cached, no atomic load)
    pub fn vdev_id(&self) -> u32 { self.vdev_id }
    
    /// Get virtual device name
    pub fn name(&self) -> &str { &self.name }
    
    /// Get block size (cached, no atomic load)
    pub fn block_size(&self) -> u32 { self.blk_size }
    
    /// Get device type (cached, no atomic load)
    pub fn hs_dev_type(&self) -> HSDevType { self.hs_dev_type }
    
    /// Get size type (cached, no atomic load)
    pub fn size_type(&self) -> VDevSizeType { self.size_type }
    
    /// Get virtual device size (mutable, requires atomic load)
    pub fn size(&self) -> u64 { self.mutable_state.load().vdev_info.vdev_size }
    
    /// Get used size (placeholder)
    pub fn used_size(&self) -> u64 {
        // TODO: Track actual used size
        0
    }
    
    /// Get alignment size
    pub fn align_size(&self) -> u32 {
        512 // Common alignment
    }
    
    /// Get available blocks (placeholder)
    pub fn available_blks(&self) -> u64 {
        // TODO: Calculate from allocator
        self.size() / self.block_size() as u64
    }
    
    /// Get number of chunks
    pub fn num_chunks(&self) -> u64 { self.mutable_state.load().vdev_info.num_primary_chunks as u64 }
    
    /// Get chunk size (immutable)
    pub fn chunk_size(&self) -> u64 { self.mutable_state.load().vdev_info.chunk_size as u64 }
    
    /// Get incremental chunk size (used for dynamic expansion)
    pub fn incremental_chunk_size(&self) -> u64 { self.incremental_chunk_size }
    
    /// Get all chunks (unordered - from HashMap)
    pub fn get_chunks(&self) -> Vec<Arc<Chunk>> { 
        self.mutable_state.load().all_chunks.values().cloned().collect() 
    }
    
    /// Get all chunks sorted by creation_order (cached - O(1) access)
    /// 
    /// This returns a pre-sorted list maintained internally, making it very fast.
    /// Chunks are sorted by creation_order, which represents the order they were created.
    pub fn get_chunks_by_creation_order(&self) -> Vec<Arc<Chunk>> {
        self.mutable_state.load().chunks_by_creation_order.clone()
    }
    
    /// Get the nth chunk in creation order (0-based index)
    /// 
    /// This returns the chunk at position N in the list sorted by creation_order.
    /// Creation_order represents the order chunks were created - used for logical ordering.
    /// 
    /// # Performance
    /// O(1) - uses cached sorted list (chunks_by_creation_order)
    /// 
    /// # Example
    /// If VDev has 3 chunks with creation_orders [5, 2, 8], then:
    /// - get_nth_chunk(0) returns chunk with creation_order 2
    /// - get_nth_chunk(1) returns chunk with creation_order 5
    /// - get_nth_chunk(2) returns chunk with creation_order 8
    pub fn get_nth_chunk(&self, n: usize) -> Option<Arc<Chunk>> {
        self.mutable_state.load()
            .chunks_by_creation_order
            .get(n)
            .cloned()
    }
    
    /// Get the actual number of chunks (from cached count)
    pub fn num_chunks_actual(&self) -> usize {
        self.mutable_state.load().all_chunks.len()
    }
    
    /// Get allocator data (bitmap) for a specific chunk
    /// 
    /// This serializes the allocator's state for persistence in metablk.
    /// 
    /// # Arguments
    /// * `chunk_id` - ID of the chunk
    /// 
    /// # Returns
    /// IOBuffer containing the serialized allocator bitmap
    pub fn get_chunk_allocator_data(&self, chunk_id: u32) -> io::Result<iomgr::IOBuffer> {
        use crate::blkalloc::BlkAllocator;
        
        let chunk = self.mutable_state.load().all_chunks
            .get(&chunk_id)
            .cloned()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, 
                format!("Chunk {} not found", chunk_id)))?;
        
        let allocator = chunk.blk_allocator();
        
        // Get the serialized allocator data
        // The allocator should provide a method to serialize its state
        allocator.serialize()
    }
}

impl VDevParameters {
    pub fn new() -> Self {
        Self {
            vdev_name: String::new(),
            vdev_size: 0,
            num_chunks: 0,
            chunk_size: 0,
            blk_size: 4096,
            dev_type: HSDevType::Data,
            num_mirrors: 1,
            alloc_type: BlkAllocatorType::Fixed,
            chunk_sel_type: ChunkSelectorType::RoundRobin,
            multi_pdev_opts: MultiPDevOpts::SingleFirstPDev,
            size_type: VDevSizeType::Static,
        }
    }
}

impl Default for VDevParameters {
    fn default() -> Self { Self::new() }
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    #[allow(unaligned_references)]
    fn test_vdev_info() {
        let info = VDevInfo::new();
        // Copy values to avoid unaligned references from packed struct
        let vdev_id = info.vdev_id;
        let blk_size = info.blk_size;
        assert_eq!(vdev_id, 0);
        assert_eq!(blk_size, 0); // Default is 0 in device_metadata
    }
    
    #[test]
    fn test_blk_id() {
        // BlkId::new takes (blk_num, nblks, chunk_num) in common/blk.rs
        let blkid = BlkId::new(100, 10, 1);
        assert_eq!(blkid.chunk_num(), 1);
        assert_eq!(blkid.blk_count(), 10);
        assert_eq!(blkid.blk_num(), 100);
        assert!(!blkid.is_multi());
    }
    
    #[test]
    fn test_virtual_dev_load() {
        let info = VDevInfo::new();
        let vdev = VirtualDev::load(info);
        assert_eq!(vdev.num_chunks(), 0);
        assert_eq!(vdev.block_size(), 4096);
    }

    #[test]
    fn test_vdev_parameters() {
        let params = VDevParameters {
            vdev_name: "test_vdev".to_string(),
            vdev_size: 1024 * 1024 * 1024, // 1GB
            num_chunks: 64,
            chunk_size: 0, // Will be calculated
            blk_size: 4096,
            dev_type: HSDevType::Data,
            num_mirrors: 1,
            alloc_type: BlkAllocatorType::Fixed,
            chunk_sel_type: ChunkSelectorType::RoundRobin,
            multi_pdev_opts: MultiPDevOpts::SingleFirstPDev,
        };
        assert_eq!(params.vdev_name, "test_vdev");
        assert_eq!(params.num_chunks, 64);
    }
}
