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

use std::sync::Arc;
use std::io;
use iomgr::{iomgr, IOBuffer};

use crate::device::{
    DeviceManager, DevInfo, HSDevType, PhysicalDev, VirtualDev, VDevParameters,
    MultiPDevOpts, BlkAllocatorType, ChunkSelectorType,
};
use crate::device::virtual_dev::VDevSizeType;
use crate::common::{BlkAllocHints, BlkAllocStatus};

/// Test configuration
const PDEV_SIZE: u64 = 1024 * 1024 * 250; // 250MB per pdev (enough for overhead + 20MB chunks)
const CHUNKS_PER_PDEV: u32 = 4;
const CHUNK_SIZE: u64 = 1024 * 1024 * 20; // 20MB per chunk
const BLOCK_SIZE: u32 = 4096;

/// Helper to create an IOBuffer from data
fn create_buffer_with_data(data: &[u8]) -> IOBuffer {
    let mut buf = IOBuffer::new(data.len());
    buf.as_mut_slice().copy_from_slice(data);
    buf
}

/// Helper to create a temporary test file
fn create_temp_file(size: u64) -> io::Result<String> {
    use std::fs::File;
    use std::io::Write;
    
    let temp_dir = std::env::temp_dir();
    let file_path = temp_dir.join(format!("homestore_test_{}.dat", uuid::Uuid::new_v4()));
    
    let mut file = File::create(&file_path)?;
    file.set_len(size)?;
    file.flush()?;
    
    Ok(file_path.to_string_lossy().into_owned())
}

/// Test fixture for VirtualDev tests
struct VDevTestFixture {
    temp_files: Vec<String>,
    pdevs: Vec<Arc<PhysicalDev>>,
    vdev: VirtualDev,
    _iomgr_guard: (),
}

impl VDevTestFixture {
    /// Create a new test fixture with specified configuration
    async fn new(
        num_pdevs: usize,
        allocator_type: BlkAllocatorType,
        chunk_selector_type: ChunkSelectorType,
    ) -> io::Result<Self> {
        // Initialize iomanager if not already done
        // Note: In real tests, you'd use iomgr::start() with proper config
        
        // Create temporary files for physical devices
        let mut temp_files = Vec::new();
        let mut pdevs = Vec::new();
        
        for i in 0..num_pdevs {
            let file_path = create_temp_file(PDEV_SIZE)?;
            temp_files.push(file_path.clone());
            
            let dev_info = DevInfo {
                dev_name: file_path,
                dev_size: PDEV_SIZE,
                dev_type: HSDevType::Data,
            };
            
            // Create physical device (format mode)
            let pdev = PhysicalDev::create(dev_info, 0, i as u32).await?;
            pdevs.push(pdev);
        }
        
        // Create VirtualDev
        // Note: Only specify chunk_size, let VirtualDev calculate num_chunks based on available space
        let vdev_params = VDevParameters {
            vdev_name: "test_vdev".to_string(),
            vdev_size: PDEV_SIZE * num_pdevs as u64,
            num_chunks: 0, // Let VirtualDev calculate from chunk_size
            blk_size: BLOCK_SIZE,
            chunk_size: CHUNK_SIZE,
            dev_type: HSDevType::Data,
            multi_pdev_opts: MultiPDevOpts::AllPDevStriped,
            alloc_type: allocator_type,
            chunk_sel_type: chunk_selector_type,
            num_mirrors: 1,
        };
        
        // Chunk ID allocator
        let mut next_chunk_id = 0u32;
        let mut chunk_id_allocator = || {
            let id = next_chunk_id;
            next_chunk_id += 1;
            Some(id)
        };
        
        let vdev = VirtualDev::create(
            vdev_params,
            0, // vdev_id
            &pdevs,
            chunk_id_allocator,
        ).await?;
        
        Ok(Self {
            temp_files,
            pdevs,
            vdev,
            _iomgr_guard: (),
        })
    }
}

impl Drop for VDevTestFixture {
    fn drop(&mut self) {
        // Clean up temporary files
        for file in &self.temp_files {
            let _ = std::fs::remove_file(file);
        }
    }
}

// ============================================================================
// Format Tests
// ============================================================================

#[iomgr::iomanager_test]
async fn test_vdev_format_roundrobin_fixed() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Fixed,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    // Test format
    fixture.vdev.format().await.expect("Format failed");
    
    println!("✓ VDev format test passed (RoundRobin + Fixed)");
}

#[iomgr::iomanager_test]
async fn test_vdev_format_roundrobin_varsize() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    fixture.vdev.format().await.expect("Format failed");
    
    println!("✓ VDev format test passed (RoundRobin + Varsize)");
}

#[iomgr::iomanager_test]
async fn test_vdev_format_mostspace_fixed() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Fixed,
        ChunkSelectorType::Custom, // Maps to MostAvailableSpace
    ).await.expect("Failed to create test fixture");
    
    fixture.vdev.format().await.expect("Format failed");
    
    println!("✓ VDev format test passed (MostAvailableSpace + Fixed)");
}

#[iomgr::iomanager_test]
async fn test_vdev_format_mostspace_varsize() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::Custom,
    ).await.expect("Failed to create test fixture");
    
    fixture.vdev.format().await.expect("Format failed");
    
    println!("✓ VDev format test passed (MostAvailableSpace + Varsize)");
}

// ============================================================================
// Allocation, Write, Read Tests
// ============================================================================

#[iomgr::iomanager_test]
async fn test_vdev_alloc_write_read_roundrobin_fixed() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Fixed,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    // Allocate a block
    let hints = BlkAllocHints::default();
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(1, &hints).await;
    
    assert_eq!(status, BlkAllocStatus::Success, "Allocation failed");
    let blkid = blkid_opt.expect("No BlkId returned");
    
    println!("Allocated block: chunk={}, blk_num={}", blkid.chunk_num(), blkid.blk_num());
    
    // Write data
    let test_data = vec![0x42u8; BLOCK_SIZE as usize];
    let mut write_buf = IOBuffer::new(BLOCK_SIZE as usize);
    write_buf.as_mut_slice().copy_from_slice(&test_data);
    
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    println!("✓ Write successful");
    
    // Read data back
    let mut read_buf = IOBuffer::new(BLOCK_SIZE as usize);
    let (read_result, read_buf) = fixture.vdev.read(read_buf, &blkid).await;
    read_result.expect("Read failed");
    
    // Verify data
    assert_eq!(read_buf.as_slice(), &test_data[..], "Data mismatch!");
    println!("✓ Read successful and data matches");
    
    // Free the block
    fixture.vdev.free(&blkid).await;
    println!("✓ Free successful");
    
    println!("✓ Alloc/Write/Read test passed (RoundRobin + Fixed)");
}

#[iomgr::iomanager_test]
async fn test_vdev_alloc_write_read_roundrobin_varsize() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    // Allocate multiple contiguous blocks
    let hints = BlkAllocHints {
        is_contiguous: true,
        ..Default::default()
    };
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(4, &hints).await;
    
    assert_eq!(status, BlkAllocStatus::Success, "Allocation failed");
    let blkid = blkid_opt.expect("No BlkId returned");
    
    println!("Allocated 4 blocks: chunk={}, blk_num={}, count={}", 
        blkid.chunk_num(), blkid.blk_num(), blkid.blk_count());
    
    // Write data
    let test_data = vec![0x55u8; BLOCK_SIZE as usize * 4];
    let write_buf = create_buffer_with_data(&test_data);
    
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    
    // Read data back
    let mut read_buf = IOBuffer::new(BLOCK_SIZE as usize * 4);
    let (read_result, read_buf) = fixture.vdev.read(read_buf, &blkid).await;
    read_result.expect("Read failed");
    
    // Verify data
    assert_eq!(read_buf.as_slice(), &test_data[..], "Data mismatch!");
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Alloc/Write/Read test passed (RoundRobin + Varsize)");
}

#[iomgr::iomanager_test]
async fn test_vdev_alloc_write_read_mostspace_fixed() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Fixed,
        ChunkSelectorType::Custom,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    let hints = BlkAllocHints::default();
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(1, &hints).await;
    
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    let test_data = vec![0x99u8; BLOCK_SIZE as usize];
    let write_buf = create_buffer_with_data(&test_data);
    
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    
    let mut read_buf = IOBuffer::new(BLOCK_SIZE as usize);
    let (read_result, read_buf) = fixture.vdev.read(read_buf, &blkid).await;
    read_result.expect("Read failed");
    
    assert_eq!(read_buf.as_slice(), &test_data[..]);
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Alloc/Write/Read test passed (MostAvailableSpace + Fixed)");
}

#[iomgr::iomanager_test]
async fn test_vdev_alloc_write_read_mostspace_varsize() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::Custom,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    let hints = BlkAllocHints {
        is_contiguous: true,
        ..Default::default()
    };
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(2, &hints).await;
    
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    let test_data = vec![0xAAu8; BLOCK_SIZE as usize * 2];
    let write_buf = create_buffer_with_data(&test_data);
    
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    
    let mut read_buf = IOBuffer::new(BLOCK_SIZE as usize * 2);
    let (read_result, read_buf) = fixture.vdev.read(read_buf, &blkid).await;
    read_result.expect("Read failed");
    
    assert_eq!(read_buf.as_slice(), &test_data[..]);
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Alloc/Write/Read test passed (MostAvailableSpace + Varsize)");
}

// ============================================================================
// Vectored I/O Tests
// ============================================================================

#[iomgr::iomanager_test]
async fn test_vdev_writev_readv_roundrobin_fixed() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Fixed,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    // Allocate blocks
    let hints = BlkAllocHints::default();
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(4, &hints).await;
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    // Create multiple buffers for writev
    let buf1 = create_buffer_with_data(&vec![0x11u8; BLOCK_SIZE as usize]);
    let buf2 = create_buffer_with_data(&vec![0x22u8; BLOCK_SIZE as usize]);
    let buf3 = create_buffer_with_data(&vec![0x33u8; BLOCK_SIZE as usize]);
    let buf4 = create_buffer_with_data(&vec![0x44u8; BLOCK_SIZE as usize]);
    
    let write_bufs = vec![buf1, buf2, buf3, buf4];
    
    // Writev
    fixture.vdev.writev(write_bufs, &blkid).await.expect("Writev failed");
    println!("✓ Writev successful");
    
    // Readv
    let read_buf1 = IOBuffer::new(BLOCK_SIZE as usize);
    let read_buf2 = IOBuffer::new(BLOCK_SIZE as usize);
    let read_buf3 = IOBuffer::new(BLOCK_SIZE as usize);
    let read_buf4 = IOBuffer::new(BLOCK_SIZE as usize);
    
    let read_bufs = vec![read_buf1, read_buf2, read_buf3, read_buf4];
    
    let (read_result, returned_bufs) = fixture.vdev.readv(read_bufs, &blkid).await;
    read_result.expect("Readv failed");
    
    // Verify data
    assert_eq!(returned_bufs[0].as_slice(), &vec![0x11u8; BLOCK_SIZE as usize][..]);
    assert_eq!(returned_bufs[1].as_slice(), &vec![0x22u8; BLOCK_SIZE as usize][..]);
    assert_eq!(returned_bufs[2].as_slice(), &vec![0x33u8; BLOCK_SIZE as usize][..]);
    assert_eq!(returned_bufs[3].as_slice(), &vec![0x44u8; BLOCK_SIZE as usize][..]);
    
    println!("✓ Readv successful and data matches");
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Writev/Readv test passed (RoundRobin + Fixed)");
}

#[iomgr::iomanager_test]
async fn test_vdev_writev_readv_mostspace_varsize() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::Custom,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    let hints = BlkAllocHints {
        is_contiguous: true,
        ..Default::default()
    };
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(3, &hints).await;
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    // Writev with 3 buffers
    let buf1 = create_buffer_with_data(&vec![0xAAu8; BLOCK_SIZE as usize]);
    let buf2 = create_buffer_with_data(&vec![0xBBu8; BLOCK_SIZE as usize]);
    let buf3 = create_buffer_with_data(&vec![0xCCu8; BLOCK_SIZE as usize]);
    
    fixture.vdev.writev(vec![buf1, buf2, buf3], &blkid).await.expect("Writev failed");
    
    // Readv
    let read_bufs = vec![
        IOBuffer::new(BLOCK_SIZE as usize),
        IOBuffer::new(BLOCK_SIZE as usize),
        IOBuffer::new(BLOCK_SIZE as usize),
    ];
    
    let (read_result, returned_bufs) = fixture.vdev.readv(read_bufs, &blkid).await;
    read_result.expect("Readv failed");
    
    assert_eq!(returned_bufs[0].as_slice(), &vec![0xAAu8; BLOCK_SIZE as usize][..]);
    assert_eq!(returned_bufs[1].as_slice(), &vec![0xBBu8; BLOCK_SIZE as usize][..]);
    assert_eq!(returned_bufs[2].as_slice(), &vec![0xCCu8; BLOCK_SIZE as usize][..]);
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Writev/Readv test passed (MostAvailableSpace + Varsize)");
}

// ============================================================================
// Fsync Tests
// ============================================================================

#[iomgr::iomanager_test]
async fn test_vdev_fsync_roundrobin_fixed() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Fixed,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    // Allocate and write some data
    let hints = BlkAllocHints::default();
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(1, &hints).await;
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    let test_data = vec![0xFFu8; BLOCK_SIZE as usize];
    let write_buf = create_buffer_with_data(&test_data);
    
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    
    // Fsync
    fixture.vdev.fsync().await.expect("Fsync failed");
    println!("✓ Fsync successful");
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Fsync test passed (RoundRobin + Fixed)");
}

#[iomgr::iomanager_test]
async fn test_vdev_fsync_mostspace_varsize() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::Custom,
    ).await.expect("Failed to create test fixture");
    
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    let hints = BlkAllocHints {
        is_contiguous: true,
        ..Default::default()
    };
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(2, &hints).await;
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    let test_data = vec![0xDDu8; BLOCK_SIZE as usize * 2];
    let write_buf = create_buffer_with_data(&test_data);
    
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    
    // Fsync
    fixture.vdev.fsync().await.expect("Fsync failed");
    println!("✓ Fsync successful");
    
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Fsync test passed (MostAvailableSpace + Varsize)");
}

// ============================================================================
// Integration Test: All Operations
// ============================================================================

#[iomgr::iomanager_test]
async fn test_vdev_full_workflow() {
    let fixture = VDevTestFixture::new(
        2,
        BlkAllocatorType::Varsize,
        ChunkSelectorType::RoundRobin,
    ).await.expect("Failed to create test fixture");
    
    println!("Step 1: Format VDev");
    fixture.vdev.format().await.expect("Format failed");
    
    println!("Step 2: Attach block allocators");
    // Note: Block allocators are now automatically constructed during VirtualDev::create()
    
    println!("Step 3: Allocate blocks");
    let hints = BlkAllocHints {
        is_contiguous: true,
        ..Default::default()
    };
    let (status, blkid_opt) = fixture.vdev.alloc_contiguous(8, &hints).await;
    assert_eq!(status, BlkAllocStatus::Success);
    let blkid = blkid_opt.unwrap();
    
    println!("Step 4: Write data");
    let test_data = vec![0xEEu8; BLOCK_SIZE as usize * 8];
    let write_buf = create_buffer_with_data(&test_data);
    fixture.vdev.write(&write_buf, &blkid).await.expect("Write failed");
    
    println!("Step 5: Fsync");
    fixture.vdev.fsync().await.expect("Fsync failed");
    
    println!("Step 6: Read data back");
    let mut read_buf = IOBuffer::new(BLOCK_SIZE as usize * 8);
    let (read_result, read_buf) = fixture.vdev.read(read_buf, &blkid).await;
    read_result.expect("Read failed");
    
    println!("Step 7: Verify data");
    assert_eq!(read_buf.as_slice(), &test_data[..]);
    
    println!("Step 8: Free blocks");
    fixture.vdev.free(&blkid).await;
    
    println!("✓ Full workflow test passed!");
}

