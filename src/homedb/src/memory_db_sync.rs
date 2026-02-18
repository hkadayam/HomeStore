//! MemoryDB - Sync in-memory database

use std::sync::Arc;
use crate::{
    Table, TableSpec, TableMap,
    error::{HomeDbError, Result},
};

#[cfg(feature = "sync")]
use tokio::runtime::Runtime;

/// Sync in-memory database (compile with features = ["inmem", "sync"])
///
/// This provides a synchronous API over Homestore's in-memory btree implementation.
/// Internally uses a dedicated single-threaded Tokio runtime to bridge to the
/// async homestore layer.
pub struct MemoryDB {
    tables: TableMap,
    num_reactors: usize,
    /// Embedded runtime for bridging sync API to async homestore
    #[cfg(feature = "sync")]
    runtime: Runtime,
}

impl MemoryDB {
    /// Create a new sync MemoryDB instance
    ///
    /// # Arguments
    /// * `num_reactors` - Number of reactor threads (typically number of CPU cores)
    pub fn new(num_reactors: usize) -> Result<Self> {
        #[cfg(all(feature = "inmem", feature = "sync"))]
        {
            // Create a minimal single-threaded runtime for sync-to-async bridging
            let runtime = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .map_err(|e| HomeDbError::IOManagerError(format!("Failed to create runtime: {}", e)))?;
            
            // Initialize IOManager in the runtime
            runtime.block_on(async {
                iomgr::init_iomgr(num_reactors)
                    .map_err(|e| HomeDbError::IOManagerError(format!("Failed to initialize IOManager: {}", e)))
            })?;
            
            let actual_reactors = iomgr::iomgr().num_reactors;
            
            Ok(Self {
                tables: TableMap::new(),
                num_reactors: actual_reactors,
                runtime,
            })
        }
        
        #[cfg(not(all(feature = "inmem", feature = "sync")))]
        {
            Err(HomeDbError::InvalidConfig(
                "MemoryDB requires 'inmem' and 'sync' features to be enabled".to_string()
            ))
        }
    }
    
    /// Get the number of reactor threads
    pub fn num_reactors(&self) -> usize {
        self.num_reactors
    }
    
    /// Shutdown the database and IOManager
    pub fn shutdown(self) {
        #[cfg(feature = "sync")]
        {
            self.runtime.block_on(async {
                let _ = iomgr::shutdown_iomgr().await;
            });
        }
    }
    
    /// Create a new table with the given specification
    ///
    /// Returns an Arc to the newly created table for direct access.
    pub fn create_table(&self, name: &str, spec: TableSpec) -> Result<Arc<Table>> {
        #[cfg(feature = "sync")]
        {
            // Check if table already exists
            if self.tables.contains_key(name) {
                return Err(HomeDbError::TableExists(name.to_string()));
            }
            
            // Create table using embedded runtime
            let table = self.runtime.block_on(async {
                Table::new(name.to_string(), spec).await
            })?;
            
            let table_arc = Arc::new(table);
            
            // Insert into tables map
            self.tables.insert(name.to_string(), Arc::clone(&table_arc));
            
            Ok(table_arc)
        }
        
        #[cfg(not(feature = "sync"))]
        {
            Err(HomeDbError::InvalidConfig("MemoryDB requires sync feature".to_string()))
        }
    }
    
    /// Drop a table (removes all data)
    pub fn drop_table(&self, name: &str) -> Result<()> {
        self.tables
            .remove(name)
            .ok_or_else(|| HomeDbError::TableNotFound(name.to_string()))?;
        
        Ok(())
    }
    
    /// Get a table handle by name
    ///
    /// Returns an Arc to the table for direct operations.
    pub fn get_table(&self, name: &str) -> Result<Arc<Table>> {
        self.tables
            .get(name)
            .map(|entry| Arc::clone(entry.value()))
            .ok_or_else(|| HomeDbError::TableNotFound(name.to_string()))
    }
    
    /// List all table names
    pub fn list_tables(&self) -> Vec<String> {
        self.tables.iter().map(|entry| entry.key().clone()).collect()
    }
    
    /// Get number of tables
    pub fn num_tables(&self) -> usize {
        self.tables.len()
    }
    
    //==================================================================
    // Convenience methods (operate via table name lookup)
    //==================================================================
    
    /// Put a key-value pair into a table (synchronous)
    pub fn put_one(&self, table_name: &str, key: &[u8], value: &[u8]) -> Result<()> {
        #[cfg(feature = "sync")]
        {
            let table = self.get_table(table_name)?;
            self.runtime.block_on(async {
                table.put(key, value).await
            })
        }
        
        #[cfg(not(feature = "sync"))]
        {
            Err(HomeDbError::InvalidConfig("MemoryDB requires sync feature".to_string()))
        }
    }
    
    /// Get a value by key from a table (synchronous)
    pub fn get(&self, table_name: &str, key: &[u8]) -> Result<Option<Vec<u8>>> {
        #[cfg(feature = "sync")]
        {
            let table = self.get_table(table_name)?;
            self.runtime.block_on(async {
                table.get(key).await
            })
        }
        
        #[cfg(not(feature = "sync"))]
        {
            Err(HomeDbError::InvalidConfig("MemoryDB requires sync feature".to_string()))
        }
    }
    
    /// Remove a key from a table (synchronous)
    pub fn remove(&self, table_name: &str, key: &[u8]) -> Result<bool> {
        #[cfg(feature = "sync")]
        {
            let table = self.get_table(table_name)?;
            self.runtime.block_on(async {
                table.remove(key).await
            })
        }
        
        #[cfg(not(feature = "sync"))]
        {
            Err(HomeDbError::InvalidConfig("MemoryDB requires sync feature".to_string()))
        }
    }
}
