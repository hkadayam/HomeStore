//! AsyncMemoryDB - Async in-memory database

use std::sync::Arc;
use crate::{
    Table, TableSpec, TableMap,
    error::{HomeDbError, Result},
};

/// Async in-memory database (compile with features = ["inmem"])
///
/// This provides an async API over Homestore's in-memory btree implementation.
/// Requires an async runtime (tokio).
pub struct AsyncMemoryDB {
    tables: TableMap,
    num_reactors: usize,
}

impl AsyncMemoryDB {
    /// Create a new AsyncMemoryDB instance
    ///
    /// # Arguments
    /// * `num_reactors` - Number of reactor threads (typically number of CPU cores)
    pub fn new(num_reactors: usize) -> Result<Self> {
        // Initialize IOManager
        #[cfg(feature = "inmem")]
        {
            iomgr::init_iomgr(num_reactors)
                .map_err(|e| HomeDbError::IOManagerError(format!("Failed to initialize IOManager: {}", e)))?;
            
            let actual_reactors = iomgr::iomgr().num_reactors;
            
            Ok(Self {
                tables: TableMap::new(),
                num_reactors: actual_reactors,
            })
        }
        
        #[cfg(not(feature = "inmem"))]
        {
            Err(HomeDbError::InvalidConfig(
                "AsyncMemoryDB requires 'inmem' feature to be enabled".to_string()
            ))
        }
    }
    
    /// Get the number of reactor threads
    pub fn num_reactors(&self) -> usize {
        self.num_reactors
    }
    
    /// Shutdown the database and IOManager
    pub async fn shutdown(self) {
        #[cfg(feature = "inmem")]
        {
            let _ = iomgr::shutdown_iomgr().await;
        }
    }
    
    /// Create a new table with the given specification
    ///
    /// Returns an Arc to the newly created table for direct access.
    pub async fn create_table(&self, name: &str, spec: TableSpec) -> Result<Arc<Table>> {
        // Check if table already exists
        if self.tables.contains_key(name) {
            return Err(HomeDbError::TableExists(name.to_string()));
        }
        
        // Create table
        let table = Table::new(name.to_string(), spec).await?;
        let table_arc = Arc::new(table);
        
        // Insert into tables map
        self.tables.insert(name.to_string(), Arc::clone(&table_arc));
        
        Ok(table_arc)
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
