//! Database - Async persistent database

use std::sync::Arc;
use crate::{
    Table, TableSpec, TableMap,
    error::{HomeDbError, Result},
};

/// Async persistent database (compile with features = ["persistent"])
///
/// This provides an async API over Homestore's full persistent storage implementation,
/// including the device layer, metablks, and journaling.
pub struct Database {
    tables: TableMap,
    num_reactors: usize,
    // TODO: Add persistent storage configuration
    // config: DatabaseConfig,
    // device_manager: DeviceManager,
}

impl Database {
    /// Create a new persistent Database instance
    ///
    /// # Arguments
    /// * `num_reactors` - Number of reactor threads (typically number of CPU cores)
    ///
    /// TODO: Add configuration for:
    /// - Device paths
    /// - Journal configuration
    /// - Metablk configuration
    /// - Checkpoint settings
    pub fn new(num_reactors: usize) -> Result<Self> {
        #[cfg(feature = "persistent")]
        {
            // Initialize IOManager (required for persistent mode)
            iomgr::init_iomgr(num_reactors)
                .map_err(|e| HomeDbError::IOManagerError(format!("Failed to initialize IOManager: {}", e)))?;
            
            let actual_reactors = iomgr::iomgr().num_reactors;
            
            // TODO: Initialize device layer, metablks, journal
            
            Ok(Self {
                tables: TableMap::new(),
                num_reactors: actual_reactors,
            })
        }
        
        #[cfg(not(feature = "persistent"))]
        {
            Err(HomeDbError::InvalidConfig(
                "Database requires 'persistent' feature to be enabled".to_string()
            ))
        }
    }
    
    /// Get the number of reactor threads
    pub fn num_reactors(&self) -> usize {
        self.num_reactors
    }
    
    /// Shutdown the database, flush pending writes, and close devices
    pub async fn shutdown(self) {
        #[cfg(feature = "persistent")]
        {
            // TODO: Flush all pending writes
            // TODO: Close all devices
            let _ = iomgr::shutdown_iomgr().await;
        }
    }
    
    /// Create a new table with the given specification
    ///
    /// In persistent mode, this also allocates metablk space for the table metadata.
    pub async fn create_table(&self, name: &str, spec: TableSpec) -> Result<Arc<Table>> {
        // Check if table already exists
        if self.tables.contains_key(name) {
            return Err(HomeDbError::TableExists(name.to_string()));
        }
        
        // TODO: For persistent mode, we need to:
        // 1. Allocate metablk for table metadata
        // 2. Create COWBtree instead of MemBtree
        // 3. Persist table metadata
        
        // For now, use in-memory table (TODO: replace with persistent table)
        let table = Table::new(name.to_string(), spec).await?;
        let table_arc = Arc::new(table);
        
        // Insert into tables map
        self.tables.insert(name.to_string(), Arc::clone(&table_arc));
        
        Ok(table_arc)
    }
    
    /// Drop a table and free its persistent storage
    pub async fn drop_table(&self, name: &str) -> Result<()> {
        self.tables
            .remove(name)
            .ok_or_else(|| HomeDbError::TableNotFound(name.to_string()))?;
        
        // TODO: In persistent mode, also need to:
        // 1. Free metablk space
        // 2. Free all device blocks used by this table's btree
        
        Ok(())
    }
    
    /// Get a table handle by name
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
    
    /// Checkpoint all tables (flush to disk)
    pub async fn checkpoint(&self) -> Result<()> {
        // TODO: Implement checkpoint logic
        // For each table, trigger COWBtree checkpoint
        Ok(())
    }
    
    //==================================================================
    // Convenience methods (operate via table name lookup)
    //==================================================================
    
    /// Put a key-value pair into a table
    pub async fn put_one(&self, table_name: &str, key: &[u8], value: &[u8]) -> Result<()> {
        let table = self.get_table(table_name)?;
        table.put(key, value).await
    }
    
    /// Get a value by key from a table
    pub async fn get(&self, table_name: &str, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let table = self.get_table(table_name)?;
        table.get(key).await
    }
    
    /// Remove a key from a table
    pub async fn remove(&self, table_name: &str, key: &[u8]) -> Result<bool> {
        let table = self.get_table(table_name)?;
        table.remove(key).await
    }
}
