//! MemoryDB - In-memory database with multiple tables

use std::sync::Arc;
use dashmap::DashMap;
use crate::{
    table::Table,
    key_value_spec::TableSpec,
    iterator::RangeIterator,
    error::{MemDbError, Result},
};

/// In-memory database managing multiple tables
/// 
/// MemoryDB should be accessed via the singleton pattern:
/// - Initialize once with `init_mem_homedb(num_reactors)` (async mode only)
/// - Initialize once with `init_mem_homedb_sync()` (sync mode only)
/// - Access via `mem_homedb()`
/// 
/// In async mode, all operations run within reactor context.
/// In sync mode, all operations use standard Rust synchronous primitives.
pub struct MemoryDB {
    tables: DashMap<String, Arc<Table>>,
    #[cfg(feature = "async_mode")]
    num_reactors: usize,
}

impl MemoryDB {
    /// Create a new MemoryDB instance (private - only callable from lib.rs singleton)
    /// 
    /// This is intentionally private to enforce the singleton pattern.
    /// Use `init_mem_homedb()` (async) or `init_mem_homedb_sync()` (sync) to create the singleton instance.
    /// Create a new MemoryDB instance
    /// 
    /// In async mode, caller should ensure iomgr is initialized first via `init_mem_homedb()`
    /// or by calling `iomgr::init_iomgr()` directly (both are idempotent/refcounted).
    pub fn new() -> Result<Self> {
        #[cfg(feature = "async_mode")]
        {
            let actual_reactors = iomgr::iomgr().num_reactors;
            
            Ok(Self {
                tables: DashMap::new(),
                num_reactors: actual_reactors,
            })
        }
        
        #[cfg(feature = "sync_mode")]
        {
            Ok(Self {
                tables: DashMap::new(),
            })
        }
    }
    
    /// Get the number of reactor threads (async mode only)
    #[cfg(feature = "async_mode")]
    pub fn num_reactors(&self) -> usize {
        self.num_reactors
    }
    
    /// Shutdown the database and IOManager
    /// 
    /// This should be called before the application exits to cleanly shutdown
    /// all reactor threads (async mode) or perform cleanup (sync mode).
    /// 
    /// Note: Prefer using `shutdown_mem_homedb()` for singleton pattern.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn shutdown(self) {
        #[cfg(feature = "async_mode")]
        {
            let _ = iomgr::shutdown_iomgr().await;
        }
        // Sync mode: no-op, DashMap cleanup happens automatically
    }
    
    /// Create a new table with the given specification
    /// 
    /// Returns an Arc to the newly created table for direct access.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn create_table(&self, name: &str, spec: TableSpec) -> Result<Arc<Table>> {
        // Check if table already exists
        if self.tables.contains_key(name) {
            return Err(MemDbError::TableExists(name.to_string()));
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
            .ok_or_else(|| MemDbError::TableNotFound(name.to_string()))?;
        
        // Arc drop will clean up all btree nodes automatically
        Ok(())
    }
    
    /// Get a table handle by name
    /// 
    /// Returns an Arc to the table for direct operations.
    /// This should be called once and the handle reused for efficiency.
    pub fn get_table(&self, name: &str) -> Result<Arc<Table>> {
        self.tables
            .get(name)
            .map(|entry| Arc::clone(entry.value()))
            .ok_or_else(|| MemDbError::TableNotFound(name.to_string()))
    }
    
    /// List all table names
    pub fn list_tables(&self) -> Vec<String> {
        self.tables.iter().map(|entry| entry.key().clone()).collect()
    }
    
    /// Get table specification
    pub fn get_table_spec(&self, name: &str) -> Result<TableSpec> {
        let table = self.get_table(name)?;
        Ok(table.spec().clone())
    }
    
    //==========================================================================
    // Convenience operations - operate on table by name
    // 
    // NOTE: These perform a table lookup on every call. For better performance,
    // get the table handle once with get_table() and reuse it:
    //   let table = db.get_table("users")?;
    //   table.put(key, value).await?;
    //==========================================================================
    
    /// Put a single key-value pair (convenience method)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn put_one(&self, table_name: &str, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        let table = self.get_table(table_name)?;
        table.put(key, value).await
    }
    
    /// Get a single value by key (convenience method)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn get(&self, table_name: &str, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        let table = self.get_table(table_name)?;
        table.get(key).await
    }
    
    /// Remove a single key (convenience method)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn remove(&self, table_name: &str, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        let table = self.get_table(table_name)?;
        table.remove(key).await
    }
    
    //==========================================================================
    // Range operations (convenience methods - delegate to Table)
    //==========================================================================
    
    /// Put multiple key-value pairs in a range (convenience method)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn put_range(&self, table_name: &str, kvs: Vec<(Vec<u8>, Vec<u8>)>) -> Result<()> {
        let table = self.get_table(table_name)?;
        table.put_range(kvs).await
    }
    
    /// Query a range of keys (convenience method - returns iterator)
    /// 
    /// For better performance, get the table handle once and call `table.get_range()`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn get_range<'a>(
        &'a self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        let table = self.get_table(table_name)?;
        table.get_range(start_key, end_key, batch_size).await
    }
    
    /// Query a range in reverse order (convenience method)
    /// 
    /// For better performance, get the table handle once and call `table.get_range_reverse()`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn get_range_reverse<'a>(
        &'a self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        let table = self.get_table(table_name)?;
        table.get_range_reverse(start_key, end_key, batch_size).await
    }
    
    /// Get any key-value pair in the given range (convenience method)
    /// 
    /// For better performance, get the table handle once and call `table.get_any()`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn get_any(
        &self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        let table = self.get_table(table_name)?;
        table.get_any(start_key, end_key).await
    }
    
    /// Remove any key in the given range (convenience method)
    /// 
    /// For better performance, get the table handle once and call `table.remove_any()`.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub async fn remove_any(
        &self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        let table = self.get_table(table_name)?;
        table.remove_any(start_key, end_key).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    
    // Test implementation functions (unified with maybe-async-cfg)
    mod test_impls {
        use super::*;
        
        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
        pub(super) async fn test_create_table() {
        let db = MemoryDB::new().unwrap();
        
        let spec = TableSpec::fixed_kv(8, 16);
        let table = db.create_table("users", spec).await.unwrap();
        
        assert_eq!(table.name(), "users");
        let tables = db.list_tables();
        assert_eq!(tables.len(), 1);
        assert!(tables.contains(&"users".to_string()));
    }
    
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub(super) async fn test_put_get_via_table_handle() {
        let db = MemoryDB::new().unwrap();
        let spec = TableSpec::fixed_kv(8, 16);
        let table = db.create_table("test", spec).await.unwrap();
        
        let key = 42u64.to_le_bytes();
        let value = [1u8; 16];
        
        // Put using table handle
        table.put(key.to_vec(), value.to_vec()).await.unwrap();
        
        // Get using table handle
        let result = table.get(key.to_vec()).await.unwrap();
        assert_eq!(result, Some(value.to_vec()));
        
        // Get non-existent key
        let key2 = 99u64.to_le_bytes();
        let result = table.get(key2.to_vec()).await.unwrap();
        assert_eq!(result, None);
    }
    
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub(super) async fn test_put_get_convenience() {
        let db = MemoryDB::new().unwrap();
        let spec = TableSpec::fixed_kv(8, 16);
        db.create_table("test", spec).await.unwrap();
        
        let key = 42u64.to_le_bytes();
        let value = [1u8; 16];
        
        // Put using convenience method
        db.put_one("test", key.to_vec(), value.to_vec()).await.unwrap();
        
        // Get using convenience method
        let result = db.get("test", key.to_vec()).await.unwrap();
        assert_eq!(result, Some(value.to_vec()));
    }
    
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub(super) async fn test_remove() {
        let db = MemoryDB::new().unwrap();
        let spec = TableSpec::fixed_kv(8, 16);
        let table = db.create_table("test", spec).await.unwrap();
        
        let key = 42u64.to_le_bytes();
        let value = [1u8; 16];
        
        // Put
        table.put(key.to_vec(), value.to_vec()).await.unwrap();
        
        // Remove
        let removed = table.remove(key.to_vec()).await.unwrap();
        assert_eq!(removed, Some(value.to_vec()));
        
        // Verify removed
        let result = table.get(key.to_vec()).await.unwrap();
        assert_eq!(result, None);
    }
    
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub(super) async fn test_drop_table() {
        let db = MemoryDB::new().unwrap();
        let spec = TableSpec::fixed_kv(8, 16);
        let table = db.create_table("test", spec).await.unwrap();
        
        // Insert some data
        let key = 42u64.to_le_bytes();
        let value = [1u8; 16];
        table.put(key.to_vec(), value.to_vec()).await.unwrap();
        
        // Drop table
        db.drop_table("test").unwrap();
        
        // Verify table doesn't exist
        assert!(db.get("test", key.to_vec()).await.is_err());
        assert_eq!(db.list_tables().len(), 0);
    }
    
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_mode"), async(feature = "async_mode"))]
    pub(super) async fn test_secondary_index() {
        let db = MemoryDB::new().unwrap();
        
        // Create table with primary index
        let primary_spec = TableSpec::fixed_kv(8, 16);
        let table = db.create_table("users", primary_spec).await.unwrap();
        
        // Create secondary index for email lookups
        let email_spec = TableSpec::fixed_kv(32, 8); // email hash -> user id
        let email_idx = table.create_index("email_idx", email_spec).await.unwrap();
        
        // Verify indices exist
        let indices = table.list_indices();
        assert_eq!(indices.len(), 2);
        assert!(indices.contains(&"primary".to_string()));
        assert!(indices.contains(&"email_idx".to_string()));
        
        // Put data in primary index
        let user_id = 42u64.to_le_bytes();
        let user_data = [1u8; 16];
        table.put(user_id.to_vec(), user_data.to_vec()).await.unwrap();
        
        // Put data in secondary index
        let email_hash = [2u8; 32];
        email_idx.put(email_hash.to_vec(), user_id.to_vec()).await.unwrap();
        
        // Lookup via secondary index
        let found_id = email_idx.get(email_hash.to_vec()).await.unwrap();
        assert_eq!(found_id, Some(user_id.to_vec()));
        
        // Lookup primary data using ID from secondary
        let primary = table.primary_index();
        let user = primary.get(user_id.to_vec()).await.unwrap();
        assert_eq!(user, Some(user_data.to_vec()));
    }
    } // end test_impls module
    
    // Macro to generate test wrappers for both sync and async modes
    macro_rules! generate_tests {
        ($($test_fn:ident),* $(,)?) => {
            $(
                #[cfg(feature = "async_mode")]
                #[iomgr::iomanager_test]
                async fn $test_fn() {
                    test_impls::$test_fn().await;
                }
                
                #[cfg(feature = "sync_mode")]
                #[test]
                fn $test_fn() {
                    test_impls::$test_fn();
                }
            )*
        };
    }
    
    // Generate all test wrappers
    generate_tests!(
        test_create_table,
        test_put_get_via_table_handle,
        test_put_get_convenience,
        test_remove,
        test_drop_table,
        test_secondary_index,
    );
}
