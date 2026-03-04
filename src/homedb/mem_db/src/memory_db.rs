//! MemoryDB - In-memory database with multiple tables.
//!
//! Lifecycle:
//!   `MemoryDB::new(num_reactors)` initializes the IOManager (async/sync_over_async modes).
//!   Dropping the instance shuts it down automatically.

use std::sync::Arc;
use dashmap::DashMap;
use crate::{table::Table, Snapshot, HomeDbError, Result, TableSpec};
use crate::table_index::TableIndex;
use homedb_core::RangeIterator;

/// In-memory database managing multiple tables.
///
/// Create one instance per process; IOManager is initialized on construction and shut
/// down on drop (async/sync_over_async modes only — sync mode has no IOManager).
pub struct MemoryDB {
    tables: DashMap<String, Arc<Table>>,
    #[cfg(feature = "async_backend")]
    num_reactors: usize,
}

impl MemoryDB {
    /// Create a new MemoryDB. In async/sync_over_async mode, initializes the IOManager
    /// with `num_reactors` reactor threads (ignored in sync mode).
    pub fn new(num_reactors: usize) -> Result<Self> {
        #[cfg(feature = "async_backend")]
        {
            iomgr::init_iomgr(num_reactors)
                .map_err(|e| HomeDbError::InvalidConfig(format!("Failed to initialize IOManager: {}", e)))?;
            let actual_reactors = iomgr::iomgr().num_reactors;
            return Ok(Self { tables: DashMap::new(), num_reactors: actual_reactors });
        }

        #[cfg(feature = "sync_backend")]
        Ok(Self { tables: DashMap::new() })
    }

    /// Get the number of reactor threads (async/sync_over_async mode only).
    #[cfg(feature = "async_backend")]
    pub fn num_reactors(&self) -> usize { self.num_reactors }

    /// Create a new table with the given specification.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn create_table(&self, name: &str, spec: TableSpec) -> Result<Arc<Table>> {
        if self.tables.contains_key(name) {
            return Err(HomeDbError::TableExists(name.to_string()));
        }
        let table = Table::new(name.to_string(), spec).await?;
        let table_arc = Arc::new(table);
        self.tables.insert(name.to_string(), Arc::clone(&table_arc));
        Ok(table_arc)
    }

    /// Drop a table (removes all data).
    pub fn drop_table(&self, name: &str) -> Result<()> {
        self.tables.remove(name).ok_or_else(|| HomeDbError::TableNotFound(name.to_string()))?;
        Ok(())
    }

    /// Get a table handle by name. Should be called once and the handle reused.
    pub fn get_table(&self, name: &str) -> Result<Arc<Table>> {
        self.tables
            .get(name)
            .map(|e| Arc::clone(e.value()))
            .ok_or_else(|| HomeDbError::TableNotFound(name.to_string()))
    }

    /// List all table names.
    pub fn list_tables(&self) -> Vec<String> { self.tables.iter().map(|e| e.key().clone()).collect() }

    /// Get table specification.
    pub fn get_table_spec(&self, name: &str) -> Result<TableSpec> {
        Ok(self.get_table(name)?.spec().clone())
    }

    //==========================================================================
    // Convenience operations - operate on table by name.
    //
    // NOTE: These perform a table lookup on every call. For better performance,
    // get the table handle once with get_table() and reuse it.
    //==========================================================================

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn put_one(&self, table_name: &str, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        self.get_table(table_name)?.put(key, value).await
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get(&self, table_name: &str, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.get_table(table_name)?.get(key).await
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove(&self, table_name: &str, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.get_table(table_name)?.remove(key).await
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn put_range(&self, table_name: &str, kvs: Vec<(Vec<u8>, Vec<u8>)>) -> Result<()> {
        self.get_table(table_name)?.put_range(kvs).await
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range<'a>(
        &'a self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        self.get_table(table_name)?.get_range(start_key, end_key, batch_size).await
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range_reverse<'a>(
        &'a self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<RangeIterator> {
        self.get_table(table_name)?.get_range_reverse(start_key, end_key, batch_size).await
    }

    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove_any(
        &self,
        table_name: &str,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        self.get_table(table_name)?.remove_any(start_key, end_key).await
    }

    // =========================================================================
    // Snapshot convenience method (MVCC tables only)
    // =========================================================================

    /// Create a `Snapshot` on the primary index of `table_name`.
    ///
    /// The returned `Snapshot` is self-contained: call `snap.get(key).await`,
    /// `snap.get_range(start, end, batch_size).await`, etc. directly on it.
    ///
    /// Returns `Err(InvalidOperation)` if the table was not created with
    /// `mvcc_enabled = true` in its `TableSpec`.
    pub fn get_snapshot(&self, table_name: &str) -> Result<Snapshot> {
        TableIndex::get_snapshot(self.get_table(table_name)?.primary_index())
    }
}

impl Drop for MemoryDB {
    fn drop(&mut self) {
        // Shut down the IOManager. drop() is always sync, but shutdown_iomgr() is async.
        // We bridge this by running the future on a dedicated thread with its own
        // single-thread Tokio runtime so we don't block or conflict with any caller runtime.
        #[cfg(feature = "async_backend")]
        {
            let _ = std::thread::spawn(|| {
                tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .map(|rt| rt.block_on(iomgr::shutdown_iomgr()))
            })
            .join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    mod test_impls {
        use super::*;

        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
        pub(super) async fn test_create_table() {
            let db = MemoryDB::new(2).unwrap();
            let spec = TableSpec::fixed_kv(8, 16);
            let table = db.create_table("users", spec).await.unwrap();
            assert_eq!(table.name(), "users");
            let tables = db.list_tables();
            assert_eq!(tables.len(), 1);
            assert!(tables.contains(&"users".to_string()));
        }

        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
        pub(super) async fn test_put_get_via_table_handle() {
            let db = MemoryDB::new(2).unwrap();
            let spec = TableSpec::fixed_kv(8, 16);
            let table = db.create_table("test", spec).await.unwrap();

            let key = 42u64.to_le_bytes();
            let value = [1u8; 16];

            table.put(key.to_vec(), value.to_vec()).await.unwrap();
            assert_eq!(table.get(key.to_vec()).await.unwrap(), Some(value.to_vec()));
            assert_eq!(table.get(99u64.to_le_bytes().to_vec()).await.unwrap(), None);
        }

        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
        pub(super) async fn test_put_get_convenience() {
            let db = MemoryDB::new(2).unwrap();
            let spec = TableSpec::fixed_kv(8, 16);
            db.create_table("test", spec).await.unwrap();

            let key = 42u64.to_le_bytes();
            let value = [1u8; 16];

            db.put_one("test", key.to_vec(), value.to_vec()).await.unwrap();
            assert_eq!(db.get("test", key.to_vec()).await.unwrap(), Some(value.to_vec()));
        }

        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
        pub(super) async fn test_remove() {
            let db = MemoryDB::new(2).unwrap();
            let spec = TableSpec::fixed_kv(8, 16);
            let table = db.create_table("test", spec).await.unwrap();

            let key = 42u64.to_le_bytes();
            let value = [1u8; 16];

            table.put(key.to_vec(), value.to_vec()).await.unwrap();
            assert_eq!(table.remove(key.to_vec()).await.unwrap(), Some(value.to_vec()));
            assert_eq!(table.get(key.to_vec()).await.unwrap(), None);
        }

        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
        pub(super) async fn test_drop_table() {
            let db = MemoryDB::new(2).unwrap();
            let spec = TableSpec::fixed_kv(8, 16);
            let table = db.create_table("test", spec).await.unwrap();

            table.put(42u64.to_le_bytes().to_vec(), [1u8; 16].to_vec()).await.unwrap();
            db.drop_table("test").unwrap();

            assert!(db.get("test", 42u64.to_le_bytes().to_vec()).await.is_err());
            assert_eq!(db.list_tables().len(), 0);
        }

        #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
        pub(super) async fn test_secondary_index() {
            let db = MemoryDB::new(2).unwrap();

            let table = db.create_table("users", TableSpec::fixed_kv(8, 16)).await.unwrap();
            let email_idx = table.create_index("email_idx", TableSpec::fixed_kv(32, 8)).await.unwrap();

            let indices = table.list_indices();
            assert_eq!(indices.len(), 2);
            assert!(indices.contains(&"primary".to_string()));
            assert!(indices.contains(&"email_idx".to_string()));

            let user_id = 42u64.to_le_bytes();
            let user_data = [1u8; 16];
            table.put(user_id.to_vec(), user_data.to_vec()).await.unwrap();

            let email_hash = [2u8; 32];
            email_idx.put(email_hash.to_vec(), user_id.to_vec()).await.unwrap();

            assert_eq!(email_idx.get(email_hash.to_vec()).await.unwrap(), Some(user_id.to_vec()));
            assert_eq!(
                table.primary_index().get(user_id.to_vec()).await.unwrap(),
                Some(user_data.to_vec())
            );
        }
    }

    macro_rules! generate_tests {
        ($($test_fn:ident),* $(,)?) => {
            $(
                cfg_if::cfg_if! {
                    if #[cfg(feature = "async_frontend")] {
                        #[iomgr::iomanager_test]
                        async fn $test_fn() {
                            test_impls::$test_fn().await;
                        }
                    } else if #[cfg(feature = "sync_frontend")] {
                        #[test]
                        fn $test_fn() {
                            test_impls::$test_fn();
                        }
                    }
                }
            )*
        };
    }

    generate_tests!(
        test_create_table,
        test_put_get_via_table_handle,
        test_put_get_convenience,
        test_remove,
        test_drop_table,
        test_secondary_index,
    );
}
