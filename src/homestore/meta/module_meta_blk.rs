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
use std::marker::PhantomData;
use std::mem;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

use futures::{pin_mut, StreamExt};
use iomgr::IOBuffer;

use super::meta_blk::MetaBlk;
use super::meta_client::MetaClient;

/// Atomic counter for generating unique module names
static MODULE_COUNTER: AtomicU64 = AtomicU64::new(0);

/// ModuleMetaBlk - A wrapper around MetaClient + MetaBlk for a single metadata block per module
///
/// Similar to C++ homestore::superblk<T>, this provides:
/// - Automatic client registration and recovery
/// - Direct mutable access to T without unnecessary copying
/// - Simple write() interface for persistence
/// - Placement in IOBuffer for efficient I/O
///
/// # Type Requirements
/// T must be:
/// - `Default` for initial creation
/// - POD-like (`#[repr(C)]`, no drop logic, all fields are Copy)
/// - Or implement custom serialization via `ModuleMetaBlkSerialize` trait
///
/// # Concurrency Model
///
/// **Application-Level Responsibility:**
/// - `ModuleMetaBlk` itself is NOT thread-safe - methods require `&mut self`
/// - The caller must ensure exclusive access
/// - If you need concurrent access to the same `ModuleMetaBlk`:
///   - Use `RefCell<ModuleMetaBlk<T>>` if all access is from a single thread at a time
///   - Use `AsyncMutex<ModuleMetaBlk<T>>` if multiple threads access it concurrently
///
/// **Internal Protection (MetaClient layer):**
/// - The underlying `MetaClient` HAS internal async locking
/// - This ONLY protects MetaClient's internal data structures (block chains, allocation state)
/// - It does NOT protect your application's concurrent access to ModuleMetaBlk
///
/// # Example
/// ```rust,ignore
/// #[repr(C)]
/// #[derive(Default, Clone, Copy)]
/// struct MyConfig {
///     version: u32,
///     flags: u64,
///     name: [u8; 64],
/// }
///
/// // Create or load (uses sizeof(MyConfig))
/// let mut config_blk = ModuleMetaBlk::<MyConfig>::new("my_module", &mut meta_mgr, None).await?;
///
/// // Or with custom size (for variable-sized data)
/// let mut var_blk = ModuleMetaBlk::<MyConfig>::new("var_module", &mut meta_mgr, Some(8192)).await?;
///
/// // Direct mutable access
/// config_blk.get_mut().version = 2;
/// config_blk.get_mut().flags |= 0x1;
///
/// // Write to disk
/// config_blk.write().await?;
/// ```
pub struct ModuleMetaBlk<T> {
    /// MetaClient for this module
    client: MetaClient,
    
    /// The single MetaBlk (allocated upfront, written on first write())
    meta_blk: MetaBlk,
    
    /// IOBuffer holding the data (T is placed at the start)
    buffer: IOBuffer,
    
    /// Module name
    name: String,
    
    /// Track if this has been persisted to disk
    is_persisted: bool,
    
    /// Phantom data for T
    _phantom: PhantomData<T>,
}

impl<T: Default> ModuleMetaBlk<T> {
    /// Create a new ModuleMetaBlk or load existing one
    ///
    /// # Arguments
    /// - `name`: Module name (if empty, auto-generates unique name)
    /// - `size`: Optional buffer size (if None, uses mem::size_of::<T>())
    ///
    /// # Behavior
    /// - If module exists on disk, loads and deserializes T
    /// - If module doesn't exist, creates new T with Default::default()
    /// - MetaBlk is allocated upfront (but not written until write() is called)
    /// - Recovery order is implicitly determined by the order of register_client() calls
    /// - Gets MetaBlkManager from global Managers singleton
    pub async fn new(
        name: impl Into<String>,
        size: Option<usize>,
    ) -> io::Result<Self> {
        let name_str = name.into();
        let actual_name = if name_str.is_empty() {
            format!("meta_blk_{}", MODULE_COUNTER.fetch_add(1, Ordering::Relaxed))
        } else {
            name_str
        };
        
        // Determine buffer size (use provided or sizeof(T))
        let buffer_size = size.unwrap_or_else(|| mem::size_of::<T>());
        
        // Get MetaBlkManager from global singleton
        let meta_mgr = crate::common::managers::metablk_mgr();
        
        // Register client (will recover if exists)
        let client = meta_mgr.register_client(actual_name.clone()).await?;
        
        // Check if we have existing data (recovery case)
        let num_blocks = client.num_meta_blks().await;
        
        if num_blocks > 0 {
            // Recovery: load existing data
            Self::load_existing(client, actual_name, buffer_size).await
        } else {
            // First time: create with default
            Self::create_new(client, actual_name, buffer_size).await
        }
    }
    
    /// Load existing module from recovered data
    async fn load_existing(client: MetaClient, name: String, _buffer_size: usize) -> io::Result<Self> {
        // Get recovered blocks (take ownership temporarily)
        let (meta_blk, buffer) = {
            let stream = client.recovered_blocks();
            pin_mut!(stream);
            
            // Get the first (and only) block
            if let Some(result) = stream.next().await {
                let (meta_blk, data) = result?;
                
                // Ensure buffer is large enough for T
                let min_size = mem::size_of::<T>();
                if data.len() < min_size {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("Recovered data too small: {} bytes, expected at least {}", 
                            data.len(), min_size)));
                }
                
                // Use the recovered buffer directly (no copy needed!)
                println!("✓ Loaded module '{}' from disk ({} bytes)", name, data.len());
                (meta_blk, data)
            } else {
                return Err(io::Error::new(
                    io::ErrorKind::NotFound,
                    "Expected recovered block but stream was empty",
                ));
            }
        };
        
        Ok(Self {
            client,
            meta_blk,
            buffer,
            name,
            is_persisted: true,  // Loaded from disk
            _phantom: PhantomData,
        })
    }
    
    /// Create new module with default T
    async fn create_new(client: MetaClient, name: String, buffer_size: usize) -> io::Result<Self> {
        let mut buffer = IOBuffer::new(buffer_size);
        
        // Initialize T with Default
        let default_value = T::default();
        
        // SAFETY: T is POD-like, we're writing Default::default() to the buffer
        unsafe {
            ptr::write(
                buffer.as_mut_slice().as_mut_ptr() as *mut T,
                default_value,
            );
        }
        
        // Allocate MetaBlk upfront (not written to disk yet)
        let meta_blk = client.create_meta_blk(name, Some(buffer_size)).await?;
        
        Ok(Self {
            client,
            meta_blk,
            buffer,
            name,
            is_persisted: false,  // Not written yet
            _phantom: PhantomData,
        })
    }
    
    /// Get immutable reference to T
    ///
    /// Returns a direct reference to T within the buffer (zero-copy)
    pub fn get(&self) -> &T {
        // SAFETY: Buffer is guaranteed to hold a valid T
        unsafe { &*(self.buffer.as_slice().as_ptr() as *const T) }
    }
    
    /// Get mutable reference to T
    ///
    /// Returns a direct mutable reference to T within the buffer
    /// Modifications are in-place, call write() to persist
    pub fn get_mut(&mut self) -> &mut T {
        // SAFETY: Buffer is guaranteed to hold a valid T
        unsafe { &mut *(self.buffer.as_mut_slice().as_mut_ptr() as *mut T) }
    }
    
    /// Write current state to disk
    ///
    /// Persists the current T to the metadata block.
    /// MetaBlk is already allocated, so this just writes the data.
    pub async fn write(&mut self) -> io::Result<()> {
        // Write data (zero-copy - just pass reference)
        self.client.write_meta_blk(self.meta_blk.clone(), &self.buffer).await?;
        
        if !self.is_persisted {
            println!("✓ Created and wrote module '{}'", self.name);
            self.is_persisted = true;
        } else {
            println!("✓ Updated module '{}'", self.name);
        }
        
        Ok(())
    }
    
    /// Destroy this module's metadata
    ///
    /// Removes the MetaBlk from disk (if it was persisted).
    /// After this, the ModuleMetaBlk is invalid for further writes.
    pub async fn destroy(&mut self) -> io::Result<()> {
        if self.is_persisted {
            self.client.remove_meta_blk(&self.meta_blk).await?;
            self.is_persisted = false;
            println!("✓ Destroyed module '{}'", self.name);
        }
        Ok(())
    }
    
    /// Get module name
    pub fn name(&self) -> &str {
        &self.name
    }
    
    /// Get buffer size
    pub fn size(&self) -> usize {
        self.buffer.len()
    }
    
    /// Resize the buffer (useful if T is variable-sized)
    ///
    /// WARNING: This invalidates the current T, you must reinitialize it
    pub fn resize(&mut self, new_size: usize) -> &mut T {
        self.buffer = IOBuffer::new(new_size);
        
        // Reinitialize T with default
        let default_value = T::default();
        unsafe {
            ptr::write(
                self.buffer.as_mut_slice().as_mut_ptr() as *mut T,
                default_value,
            );
        }
        
        self.get_mut()
    }
}

// Implement Deref for ergonomic access (m.field instead of m.get().field)
impl<T: Default> std::ops::Deref for ModuleMetaBlk<T> {
    type Target = T;
    
    fn deref(&self) -> &Self::Target {
        self.get()
    }
}

// Implement DerefMut for ergonomic mutable access
impl<T: Default> std::ops::DerefMut for ModuleMetaBlk<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.get_mut()
    }
}

#[cfg(test)]
mod tests {
    #[repr(C)]
    #[derive(Default, Clone, Copy, Debug, PartialEq)]
    struct TestConfig {
        version: u32,
        flags: u64,
        counter: u64,
        name: [u8; 32],
    }
    
    // Note: Tests would require MetaBlkManager setup
    // These are placeholder test structures for documentation
}

