//! Table implementation - manages multiple indices
//!
//! A Table is a logical entity that can have multiple physical indices (TableIndex).
//! Each table has at least a primary index, and can have additional secondary indices.

use std::sync::Arc;
use dashmap::DashMap;
#[allow(unused_imports)] // IndexType used when sync_mode or async_mode is enabled
use crate::{table_index::{IndexType, TableIndex}, HomeDbError, Result, TableSpec};

/// A table in MemDB - manages multiple indices
///
/// Each table has:
/// - A primary index (created automatically)
/// - Optional secondary indices
///
/// Operations can be performed either:
/// - Directly on the table (convenience, uses primary index)
/// - On specific indices (for explicit control)
pub struct Table {
    name: String,
    spec: TableSpec,
    indices: DashMap<String, Arc<TableIndex>>,
}

impl Table {
    /// Create a new table with a primary index
    ///
    /// # Arguments
    /// * `name` - Name of the table
    /// * `spec` - Schema specification for the primary index
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(name: String, spec: TableSpec) -> Result<Self> {
        let table = Self {
            name: name.clone(),
            spec: spec.clone(),
            indices: DashMap::new(),
        };

        // Create primary index
        let primary = TableIndex::new(format!("{}_primary", name), IndexType::Primary, spec).await?;

        table.indices.insert("primary".to_string(), Arc::new(primary));

        Ok(table)
    }

    /// Get table name
    pub fn name(&self) -> &str { &self.name }

    /// Get table specification
    pub fn spec(&self) -> &TableSpec { &self.spec }

    /// Get the primary index
    pub fn primary_index(&self) -> Arc<TableIndex> {
        // Primary index always exists, safe to unwrap
        Arc::clone(self.indices.get("primary").unwrap().value())
    }

    /// Create a secondary index on this table
    ///
    /// # Arguments
    /// * `index_name` - Name for the secondary index (e.g., "email_idx")
    /// * `spec` - Schema specification for the secondary index
    ///
    /// # Returns
    /// Arc to the newly created TableIndex
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn create_index(&self, index_name: &str, spec: TableSpec) -> Result<Arc<TableIndex>> {
        // Check if index already exists
        if self.indices.contains_key(index_name) {
            return Err(HomeDbError::InvalidConfig(
                format!("Index '{}' already exists on table '{}'", index_name, self.name),
            ));
        }

        // Create secondary index
        let index = TableIndex::new(format!("{}_{}", self.name, index_name), IndexType::Secondary, spec).await?;

        let index_arc = Arc::new(index);
        self.indices.insert(index_name.to_string(), Arc::clone(&index_arc));

        Ok(index_arc)
    }

    /// Get a specific index by name
    ///
    /// # Arguments
    /// * `index_name` - Name of the index (e.g., "primary", "email_idx")
    pub fn get_index(&self, index_name: &str) -> Result<Arc<TableIndex>> {
        self.indices.get(index_name).map(|entry| Arc::clone(entry.value())).ok_or_else(|| {
            HomeDbError::InvalidConfig(format!("Index '{}' not found on table '{}'", index_name, self.name))
        })
    }

    /// List all index names
    pub fn list_indices(&self) -> Vec<String> { self.indices.iter().map(|entry| entry.key().clone()).collect() }

    /// Drop a secondary index
    ///
    /// Note: Cannot drop the primary index
    pub fn drop_index(&self, index_name: &str) -> Result<()> {
        if index_name == "primary" {
            return Err(HomeDbError::InvalidConfig("Cannot drop primary index".to_string()));
        }

        self.indices.remove(index_name).ok_or_else(|| {
            HomeDbError::InvalidConfig(format!("Index '{}' not found on table '{}'", index_name, self.name))
        })?;

        Ok(())
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Primary operations - operate on primary index
    // ═══════════════════════════════════════════════════════════════════════

    /// Put a single key-value pair (uses primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()> { self.primary_index().put(key, value).await }

    /// Get a single value by key (uses primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> { self.primary_index().get(key).await }

    /// Remove a single key (uses primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> { self.primary_index().remove(key).await }

    /// Put multiple key-value pairs (uses primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn put_range(&self, kvs: Vec<(Vec<u8>, Vec<u8>)>) -> Result<()> {
        let primary = self.primary_index();
        for (key, value) in kvs {
            primary.put(key, value).await?;
        }
        Ok(())
    }

    /// Query a range of keys (convenience method, delegates to primary index)
    ///
    /// For better performance, get the index handle and call methods directly:
    /// ```ignore
    /// let primary = table.primary_index();
    /// let iter = primary.get_range(start, end, batch_size).await?;
    /// ```
    ///
    /// # Arguments
    /// - `start_key`: Start of range (inclusive)
    /// - `end_key`: End of range (exclusive)
    /// - `batch_size`: Number of results to fetch per batch
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<homedb_common::RangeIterator> {
        self.primary_index().get_range(start_key, end_key, batch_size).await
    }

    /// Query a range in reverse order (convenience method, delegates to primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range_reverse(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<homedb_common::RangeIterator> {
        self.primary_index().get_range_reverse(start_key, end_key, batch_size).await
    }

    /// Get any key-value pair in the given range (convenience method, delegates to primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_any(&self, start_key: Vec<u8>, end_key: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        self.primary_index().get_any(start_key, end_key).await
    }

    /// Remove any key in the given range (convenience method, delegates to primary index)
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove_any(&self, start_key: Vec<u8>, end_key: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        self.primary_index().remove_any(start_key, end_key).await
    }
}
