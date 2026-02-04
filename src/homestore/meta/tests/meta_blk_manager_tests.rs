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
use std::collections::HashMap;
use tempfile::TempDir;
use iomgr::IOBuffer;
use futures::stream::StreamExt;
use futures::pin_mut;

use crate::device::{DeviceManager, DevInfo, HSDevType};
use crate::meta::{MetaBlkManager, MetaBlk, MetaClient};
use crate::common::managers::{Managers, device_mgr, metablk_mgr};

// ═══════════════════════════════════════════════════════════════════════════
// Test Constants
// ═══════════════════════════════════════════════════════════════════════════

const PDEV_SIZE: u64 = 200 * 1024 * 1024; // 200 MB per physical device
const NUM_PDEVS: usize = 2;
const META_VDEV_SIZE: u64 = 160 * 1024 * 1024; // 160 MB (80% of total space)

// ═══════════════════════════════════════════════════════════════════════════
// Test Helper Functions
// ═══════════════════════════════════════════════════════════════════════════

/// Create temporary physical device files (call once per test)
fn create_devices(temp_dir: &TempDir) -> io::Result<Vec<DevInfo>> {
    let mut dev_infos = Vec::new();
    
    for i in 0..NUM_PDEVS {
        let file_path = temp_dir.path().join(format!("pdev_{}.data", i));
        let file_path_str = file_path.to_str().unwrap().to_string();
        
        // Create file with required size
        let file = std::fs::File::create(&file_path)?;
        file.set_len(PDEV_SIZE)?;
        
        dev_infos.push(DevInfo {
            dev_name: file_path_str,
            dev_size: PDEV_SIZE,
            dev_type: HSDevType::Fast,
        });
    }
    
    Ok(dev_infos)
}

/// Setup: Create and format fresh DeviceManager and MetaBlkManager
async fn setup_fresh(dev_infos: &[DevInfo]) -> io::Result<()> {
    // Create and format DeviceManager (self-registers to global singleton)
    let _device_manager = DeviceManager::new(dev_infos.to_vec())?;
    device_mgr().format_devices().await?;
    
    // Create MetaBlkManager (self-registers to global singleton)
    let _meta_mgr = MetaBlkManager::create(META_VDEV_SIZE).await?;
    
    Ok(())
}

/// Restart: Drop everything, restart IOManager, load DeviceManager and MetaBlkManager
async fn restart_and_load(dev_infos: &[DevInfo]) -> io::Result<()> {
    // Restart IOManager AND global Managers singleton
    iomgr::restart(8).await.map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;
    Managers::restart().await;
    
    // Load DeviceManager with SAME device files (self-registers to global singleton)
    let _device_manager = DeviceManager::new(dev_infos.to_vec())?;
    device_mgr().load_devices().await?;
    
    // Load MetaBlkManager (self-registers to global singleton)
    let _meta_mgr = MetaBlkManager::load().await?;
    
    Ok(())
}

// ═══════════════════════════════════════════════════════════════════════════
// TestClient - Test helper to track written and recovered MetaBlks
// ═══════════════════════════════════════════════════════════════════════════

struct ShadowMetaEntry {
    meta_blk: MetaBlk,
    data: Vec<u8>,
    recovery_count: usize,
}

/// TestClient - Wrapper around MetaClient with built-in validation
/// Embeds own_id (u64) in the first 8 bytes of each MetaBlk's data for tracking
struct TestClient {
    client: Option<MetaClient>,  // Current MetaClient instance
    next_id: u64,                // Auto-incrementing ID for each generated block
    entries: HashMap<u64, ShadowMetaEntry>, // (own_id -> tracked entry)
}

impl TestClient {
    fn new() -> Self {
        Self {
            client: None,
            next_id: 1,
            entries: HashMap::new(),
        }
    }
    
    /// Update the internal MetaClient (call after register_client or restart)
    fn update_client(&mut self, client: MetaClient) {
        self.client = Some(client);
    }
    
    /// Generate a MetaBlk with embedded own_id, write it, and track it
    async fn gen_meta_blk(&mut self, size: usize) -> io::Result<u64> {
        let client = self.client.as_ref().expect("Call update_client first");
        
        let own_id = self.next_id;
        self.next_id += 1;
        
        // Create data buffer with own_id embedded at the start (first 8 bytes)
        let mut data = vec![0u8; size.max(8)];
        data[0..8].copy_from_slice(&own_id.to_le_bytes());
        
        // Fill rest with pattern based on own_id (for uniqueness)
        for i in 8..data.len() {
            data[i] = (own_id as u8).wrapping_add(i as u8);
        }
        
        // Create and write MetaBlk
        let name = format!("test_meta_{}", own_id);
        let meta_blk = client.create_meta_blk(&name, Some(data.len())).await?;
        let mut buf = IOBuffer::new(data.len());
        buf.as_mut_slice().copy_from_slice(&data);
        client.write_meta_blk(meta_blk.clone(), &buf).await?;
        
        // Track it
        self.entries.insert(own_id, ShadowMetaEntry {
            meta_blk,
            data,
            recovery_count: 0,
        });
        
        println!("✓ Generated and wrote block own_id={}", own_id);
        Ok(own_id)
    }
    
    /// Remove a MetaBlk by own_id
    async fn remove_meta_blk(&mut self, own_id: u64) -> io::Result<()> {
        let client = self.client.as_ref().expect("Call update_client first");
        
        if let Some(entry) = self.entries.remove(&own_id) {
            client.remove_meta_blk(&entry.meta_blk).await?;
            println!("✓ Removed block own_id={}", own_id);
        }
        Ok(())
    }
    
    /// Validate recovery - stream through all recovered blocks, validate them,
    /// assert all were recovered exactly once, then reset recovery counts
    async fn validate_recovery(&mut self) -> io::Result<()> {
        let client = self.client.as_ref().expect("Call update_client first");
        
        let stream = client.recovered_blocks();
        pin_mut!(stream);
        
        while let Some(result) = stream.next().await {
            let (_meta_blk, data) = result?;
            
            // Extract own_id from first 8 bytes
            let data_slice = data.as_slice();
            if data_slice.len() < 8 {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "Data too small to contain own_id"));
            }
            
            let own_id = u64::from_le_bytes(data_slice[0..8].try_into().unwrap());
            
            // Lookup in our map
            let entry = self.entries.get_mut(&own_id)
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, 
                    format!("own_id {} not found in shadow map", own_id)))?;
            
            // Validate the data matches what we wrote
            if data_slice != entry.data.as_slice() {
                return Err(io::Error::new(io::ErrorKind::InvalidData, 
                    format!("Data mismatch for own_id {}", own_id)));
            }
            
            // Increment recovery count
            entry.recovery_count += 1;
            println!("✓ Validated block own_id={}", own_id);
        }
        
        // Assert all blocks were recovered exactly once
        for (own_id, entry) in &self.entries {
            assert_eq!(entry.recovery_count, 1, 
                "Block own_id={} was recovered {} times (expected 1)", 
                own_id, entry.recovery_count);
        }
        
        // Reset recovery counts for next restart/load cycle
        for entry in self.entries.values_mut() {
            entry.recovery_count = 0;
        }
        
        Ok(())
    }
    
    /// Get total number of tracked blocks (remaining in shadow after removals)
    fn len(&self) -> usize {
        self.entries.len()
    }
    
    /// Clear all entries (used when client is deregistered)
    fn clear_all(&mut self) {
        self.entries.clear();
    }
    
    /// Update an existing MetaBlk with new size and data
    async fn update_meta_blk(&mut self, own_id: u64, new_size: usize) -> io::Result<()> {
        let client = self.client.as_ref().expect("Call update_client first");
        
        // Get the existing entry
        let entry = self.entries.get_mut(&own_id)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, 
                format!("own_id {} not found", own_id)))?;
        
        // Create new data with same own_id but new size
        let mut new_data = vec![0u8; new_size.max(8)];
        new_data[0..8].copy_from_slice(&own_id.to_le_bytes());
        
        // Fill rest with pattern based on own_id (for uniqueness)
        for i in 8..new_data.len() {
            new_data[i] = (own_id as u8).wrapping_add(i as u8);
        }
        
        // Write updated data
        let mut buf = IOBuffer::new(new_data.len());
        buf.as_mut_slice().copy_from_slice(&new_data);
        client.write_meta_blk(entry.meta_blk.clone(), &buf).await?;
        
        // Update shadow entry
        entry.data = new_data;
        
        println!("✓ Updated block own_id={} to new size {}", own_id, new_size);
        Ok(())
    }
    
    /// Read a specific MetaBlk and validate it matches shadow
    async fn read_and_validate_meta_blk(&self, own_id: u64) -> io::Result<()> {
        let client = self.client.as_ref().expect("Call update_client first");
        
        // Get the entry
        let entry = self.entries.get(&own_id)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, 
                format!("own_id {} not found", own_id)))?;
        
        // Read the block
        let read_data = client.read_meta_blk(&entry.meta_blk).await?;
        
        // Validate
        if read_data.as_slice() != entry.data.as_slice() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, 
                format!("Data mismatch for own_id {}", own_id)));
        }
        
        println!("✓ Read and validated block own_id={} (size={})", own_id, entry.data.len());
        Ok(())
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 1: Create MetaBlkManager → Drop → Restart → Load
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_create_restart_load() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    {
        println!("✓ Step 1: Created MetaBlkManager");
        setup_fresh(&dev_infos).await.unwrap();
    } // Drop happens here
    
    {
        println!("✓ Step 2: Restart and load MetaBlkManager");
        restart_and_load(&dev_infos).await.unwrap();
    }
    
    println!("✓ Test 1 PASSED: Create → Drop → Restart → Load");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 2: Register clients → Drop → Restart → Load → Validate
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_register_clients_restart() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    {
        println!("✓ Step 1: Create and register 3 clients");
        setup_fresh(&dev_infos).await.unwrap();
        
        // Register 3 clients
        let _client1 = metablk_mgr().register_client("test_client_1".to_string()).await.unwrap();
        let _client2 = metablk_mgr().register_client("test_client_2".to_string()).await.unwrap();
        let _client3 = metablk_mgr().register_client("test_client_3".to_string()).await.unwrap();
        
        println!("✓ Registered 3 clients");
        // Drop happens here
    }
    
    {
        println!("✓ Step 2: Restart and verify clients are recovered");
        restart_and_load(&dev_infos).await.unwrap();
        
        // Re-register clients - should recover from disk
        let client1 = metablk_mgr().register_client("test_client_1".to_string()).await.unwrap();
        let client2 = metablk_mgr().register_client("test_client_2".to_string()).await.unwrap();
        let client3 = metablk_mgr().register_client("test_client_3".to_string()).await.unwrap();

        // Verify client names
        println!("✓ Step 3: Re-registered 3 clients");
        assert_eq!(client1.client_name().await, "test_client_1");
        assert_eq!(client2.client_name().await, "test_client_2");
        assert_eq!(client3.client_name().await, "test_client_3");
    }
    
    println!("✓ Test 2 PASSED: Register clients → Drop → Restart → Validate");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 3: Write data → Restart → Validate callbacks
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_write_data_restart_validate() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    let mut test_client1 = TestClient::new();
    let mut test_client2 = TestClient::new();
    
    {
        println!("✓ Step 1: Create 2 clients");
        setup_fresh(&dev_infos).await.unwrap();
        let client1 = metablk_mgr().register_client("writer_client_1".to_string()).await.unwrap();
        let client2 = metablk_mgr().register_client("writer_client_2".to_string()).await.unwrap();
        
        test_client1.update_client(client1);
        test_client2.update_client(client2);
        
        println!("✓ Step 2: Wrote 2 blocks for each client");
        test_client1.gen_meta_blk(100).await.unwrap();
        test_client1.gen_meta_blk(200).await.unwrap();
        
        test_client2.gen_meta_blk(150).await.unwrap();
        test_client2.gen_meta_blk(250).await.unwrap();
        
        // Drop happens here
    }
    
    {
        println!("✓ Step 3: Restart and reload meta clients");
        restart_and_load(&dev_infos).await.unwrap();
        
        let client1 = metablk_mgr().register_client("writer_client_1".to_string()).await.unwrap();
        let client2 = metablk_mgr().register_client("writer_client_2".to_string()).await.unwrap();
        
        test_client1.update_client(client1);
        test_client2.update_client(client2);
        
        println!("✓ Step 4: Validate if all clients recovered their meta blocks");
        test_client1.validate_recovery().await.expect("Validation failed for client1");
        test_client2.validate_recovery().await.expect("Validation failed for client2");
        
        println!("✓ Validated: client1 recovered {} blocks with correct data", test_client1.len());
        println!("✓ Validated: client2 recovered {} blocks with correct data", test_client2.len());
    }
    
    println!("✓ Test 3 PASSED: Write data → Restart → Validate with streams");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 4: Remove metablks → Restart → Validate
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_remove_blocks_restart() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    let mut test_client = TestClient::new();
    
    {
        println!("✓ Step 1: Create client and write 5 blocks");
        setup_fresh(&dev_infos).await.unwrap();
        let client = metablk_mgr().register_client("removal_client".to_string()).await.unwrap();
        test_client.update_client(client);
        
        // Write 5 blocks
        let id0 = test_client.gen_meta_blk(100).await.unwrap();
        let _id1 = test_client.gen_meta_blk(120).await.unwrap();
        let id2 = test_client.gen_meta_blk(140).await.unwrap();
        let _id3 = test_client.gen_meta_blk(160).await.unwrap();
        let id4 = test_client.gen_meta_blk(180).await.unwrap();
        
        println!("✓ Step 2: Remove 3 blocks (middle id={}, end id={}, start id={})", id2, id4, id0);
        test_client.remove_meta_blk(id2).await.expect("Failed to remove middle block");
        test_client.remove_meta_blk(id4).await.expect("Failed to remove end block");
        test_client.remove_meta_blk(id0).await.expect("Failed to remove start block");
        
        // Should have blocks id1 and id3 remaining
        // Drop happens here
    }
    
    {
        println!("✓ Step 3: Restart and verify remaining blocks");
        restart_and_load(&dev_infos).await.unwrap();
        let client = metablk_mgr().register_client("removal_client".to_string()).await.unwrap();
        test_client.update_client(client);

        println!("✓ Step 4: Validate if all remainingclients recovered their meta blocks");
        test_client.validate_recovery().await.expect("Validation failed");
        println!("✓ Validated: client recovered {} blocks with correct data", test_client.len());
    }
    
    println!("✓ Test 4 PASSED: Remove blocks (middle/end/start) → Restart → Validate");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 5: Remove all metablks for a client → Restart → Validate
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_remove_all_blocks_restart() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    let mut test_victim = TestClient::new();
    let mut test_survivor = TestClient::new();
    
    {
        println!("✓ Step 1: Create 2 clients");
        setup_fresh(&dev_infos).await.unwrap();
        
        let client1 = metablk_mgr().register_client("victim_client".to_string()).await.unwrap();
        let client2 = metablk_mgr().register_client("survivor_client".to_string()).await.unwrap();
        
        test_victim.update_client(client1);
        test_survivor.update_client(client2);
        
        println!("✓ Step 2: Wrote 3 blocks for victim_client");
        let id0 = test_victim.gen_meta_blk(100).await.unwrap();
        let id1 = test_victim.gen_meta_blk(150).await.unwrap();
        let id2 = test_victim.gen_meta_blk(200).await.unwrap();
        
        println!("✓ Step 3: Wrote 2 blocks for survivor_client");
        test_survivor.gen_meta_blk(120).await.unwrap();
        test_survivor.gen_meta_blk(180).await.unwrap();
        
        println!("✓ Step 4: Remove all blocks from victim_client");
        for own_id in [id0, id1, id2] {
            test_victim.remove_meta_blk(own_id).await.expect("Failed to remove block");
        }
        println!("✓ Removed all blocks from victim_client");
        
        // Drop happens here
    }
    
    {
        println!("✓ Step 5: Restart and verify clients");
        restart_and_load(&dev_infos).await.unwrap();
        
        let client1 = metablk_mgr().register_client("victim_client".to_string()).await.unwrap();
        let client2 = metablk_mgr().register_client("survivor_client".to_string()).await.unwrap();
        
        test_victim.update_client(client1);
        test_survivor.update_client(client2);
        
        println!("✓ Step 6: Validate if victim_client has no blocks");
        test_victim.validate_recovery().await.expect("Validation failed for victim");
        assert_eq!(test_victim.len(), 0, "Expected no blocks for victim_client");
        
        println!("✓ Step 7: Validate if survivor_client has 2 blocks");
        test_survivor.validate_recovery().await.expect("Validation failed for survivor");
        assert_eq!(test_survivor.len(), 2, "Expected 2 blocks for survivor_client");
    }
    
    println!("✓ Test 5 PASSED: Remove all blocks for a client → Restart → Validate");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 6: Deregister client → Restart → Validate
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_deregister_client_restart() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    let mut test_keeper1 = TestClient::new();
    let mut test_doomed = TestClient::new();
    let mut test_keeper3 = TestClient::new();
    
    {
        println!("✓ Step 1: Create 3 clients");
        setup_fresh(&dev_infos).await.unwrap();
        
        let client1 = metablk_mgr().register_client("keeper_client_1".to_string()).await.unwrap();
        let client2 = metablk_mgr().register_client("doomed_client".to_string()).await.unwrap();
        let client3 = metablk_mgr().register_client("keeper_client_3".to_string()).await.unwrap();
        
        test_keeper1.update_client(client1);
        test_doomed.update_client(client2);
        test_keeper3.update_client(client3);
        
        println!("✓ Step 2: Write blocks for all 3 clients");
        test_keeper1.gen_meta_blk(100).await.unwrap();
        test_doomed.gen_meta_blk(150).await.unwrap();
        test_keeper3.gen_meta_blk(200).await.unwrap();
        
        println!("✓ Step 3: Deregister doomed_client");
        // Get the client back to deregister it
        let doomed_client = test_doomed.client.as_ref().unwrap();
        metablk_mgr().deregister_client(doomed_client).await.unwrap();
        
        // Clear all entries from test_doomed since it was deregistered
        test_doomed.clear_all();
        
        // Drop happens here
    }
    
    {
        println!("✓ Step 4: Restart and re-register all 3 clients");
        restart_and_load(&dev_infos).await.unwrap();
        
        // Client1 should be recovered
        let client1 = metablk_mgr().register_client("keeper_client_1".to_string()).await.unwrap();
        test_keeper1.update_client(client1);
        
        // Client2 should NOT be recovered (was deregistered). This will create a NEW client, not recover old one.
        let client2 = metablk_mgr().register_client("doomed_client".to_string()).await.unwrap();
        test_doomed.update_client(client2);
        
        // Client3 should be recovered
        let client3 = metablk_mgr().register_client("keeper_client_3".to_string()).await.unwrap();
        test_keeper3.update_client(client3);
        
        println!("✓ Step 5: Validate recovery - keeper clients should have blocks, doomed should have none");
        test_keeper1.validate_recovery().await.expect("Validation failed for keeper1");
        assert_eq!(test_keeper1.len(), 1, "Expected 1 block for keeper_client_1");
        
        test_doomed.validate_recovery().await.expect("Validation failed for doomed");
        assert_eq!(test_doomed.len(), 0, "Expected 0 blocks for doomed_client (was deregistered)");
        
        test_keeper3.validate_recovery().await.expect("Validation failed for keeper3");
        assert_eq!(test_keeper3.len(), 1, "Expected 1 block for keeper_client_3");
        
        println!("✓ Validated: keeper clients recovered blocks, doomed_client has none");
    }
    
    println!("✓ Test 6 PASSED: Deregister client → Restart → Validate");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Case 7: Overflow blocks - inline vs non-inline, size updates, read_meta_blk
// ═══════════════════════════════════════════════════════════════════════════

#[iomgr::iomanager_test]
async fn test_metablk_overflow_and_size_updates() {
    let temp_dir = TempDir::new().unwrap();
    let dev_infos = create_devices(&temp_dir).unwrap();
    
    let mut test_client = TestClient::new();
    
    const SMALL_SIZE: usize = 100;      // Inlineable
    const LARGE_SIZE: usize = 512 * 1024;  // 512KB - overflow block
    const XLARGE_SIZE: usize = 1024 * 1024; // 1MB - larger overflow
    
    // Step 1: Create 3 blocks with different sizes
    let (blk1_id, blk2_id, blk3_id) = {
        println!("✓ Step 1: Create client and generate 3 metablks");
        setup_fresh(&dev_infos).await.unwrap();
        let client = metablk_mgr().register_client("overflow_test_client".to_string()).await.unwrap();
        test_client.update_client(client);
        
        // Blk1 = small (inlineable), Blk2 = 512KB, Blk3 = 512KB
        let blk1_id = test_client.gen_meta_blk(SMALL_SIZE).await.unwrap();
        let blk2_id = test_client.gen_meta_blk(LARGE_SIZE).await.unwrap();
        let blk3_id = test_client.gen_meta_blk(LARGE_SIZE).await.unwrap();
        
        println!("✓ Generated: blk1={} ({}B), blk2={} ({}KB), blk3={} ({}KB)", 
            blk1_id, SMALL_SIZE, blk2_id, LARGE_SIZE/1024, blk3_id, LARGE_SIZE/1024);
        
        (blk1_id, blk2_id, blk3_id)
    };
    
    // Step 2: Update blocks with different sizes
    {
        println!("✓ Step 2: Update blocks - Blk1: small->large, Blk2: large->small, Blk3: large->xlarge");
        // Blk1: small -> large (512KB)
        test_client.update_meta_blk(blk1_id, LARGE_SIZE).await.expect("Failed to update blk1");
        
        // Blk2: large -> small (100B)
        test_client.update_meta_blk(blk2_id, SMALL_SIZE).await.expect("Failed to update blk2");
        
        // Blk3: large -> xlarge (1MB)
        test_client.update_meta_blk(blk3_id, XLARGE_SIZE).await.expect("Failed to update blk3");
        
        println!("✓ Updated: blk1={} ({}KB), blk2={} ({}B), blk3={} ({}MB)", 
            blk1_id, LARGE_SIZE/1024, blk2_id, SMALL_SIZE, blk3_id, XLARGE_SIZE/(1024*1024));
        
        // Drop happens here
    }
    
    // Step 3: Restart and validate recovery
    {
        println!("✓ Step 3: Restart and validate recovery");
        restart_and_load(&dev_infos).await.unwrap();
        let client = metablk_mgr().register_client("overflow_test_client".to_string()).await.unwrap();
        test_client.update_client(client);
        
        println!("✓ Step 4: Validate all blocks recovered with correct sizes");
        test_client.validate_recovery().await.expect("Validation failed after restart");
        assert_eq!(test_client.len(), 3, "Expected 3 blocks after recovery");
    }
    
    // Step 4: Reverse the updates and validate with read_meta_blk (no restart)
    {
        println!("✓ Step 5: Reverse updates - Blk1: large->small, Blk2: small->large, Blk3: xlarge->small");
        // Blk1: large -> small
        test_client.update_meta_blk(blk1_id, SMALL_SIZE).await.expect("Failed to reverse update blk1");
        
        // Blk2: small -> large
        test_client.update_meta_blk(blk2_id, LARGE_SIZE).await.expect("Failed to reverse update blk2");
        
        // Blk3: xlarge -> small
        test_client.update_meta_blk(blk3_id, SMALL_SIZE).await.expect("Failed to reverse update blk3");
        
        println!("✓ Reversed: blk1={} ({}B), blk2={} ({}KB), blk3={} ({}B)", 
            blk1_id, SMALL_SIZE, blk2_id, LARGE_SIZE/1024, blk3_id, SMALL_SIZE);
        
        println!("✓ Step 6: Validate using read_meta_blk (without restart)");
        test_client.read_and_validate_meta_blk(blk1_id).await.expect("Failed to read blk1");
        test_client.read_and_validate_meta_blk(blk2_id).await.expect("Failed to read blk2");
        test_client.read_and_validate_meta_blk(blk3_id).await.expect("Failed to read blk3");
        
        println!("✓ All blocks validated via read_meta_blk");
    }
    
    println!("✓ Test 7 PASSED: Overflow blocks, size updates, and read_meta_blk validation");
}

