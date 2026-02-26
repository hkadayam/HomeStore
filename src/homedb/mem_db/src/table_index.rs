//! TableIndex implementation - wraps a BtreeIndex backend with schema validation.
//!
//! Backend selection (runtime `partition_key_size`):
//!   partition_key_size == 0  → UnshardedBtree  (single btree, no sharding)
//!   partition_key_size >= 1  → ShardedBtree    (hash-sharded by partition key prefix)

use std::sync::Arc;
use homestore::index::btree::{underlying::mem::MemBtree, BtreeConfig};
use crate::{HomeDbError, KeyType, Result, TableSpec, ValueSpec};
use homedb_common::{BtreeIndex, DbKey, DbValue};

/// Index type classification
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndexType {
    /// Primary index - stores the full row data
    Primary,
    /// Secondary index - stores pointer to primary key
    Secondary,
}

/// A physical B-tree index within a table.
///
/// Holds a `BtreeIndex` backend and provides schema-validated operations.
/// Each table can have multiple indices.
#[derive(Clone)]
pub struct TableIndex {
    name: String,
    index_type: IndexType,
    spec: TableSpec,
    btree: Arc<dyn BtreeIndex>,
    max_key_size: u32, // Cached from BtreeConfig for runtime validation
}

impl TableIndex {
    /// Create a new table index with the given specification.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn new(name: String, index_type: IndexType, spec: TableSpec) -> Result<Self> {
        let node_variant = Self::determine_node_variant(&spec);

        let mut config = BtreeConfig::new(spec.node_size, name.clone());
        config.leaf_node_variant = node_variant;
        config.int_node_variant = node_variant;

        if let crate::PrefixType::Prefixable(Some(prefix_size)) = spec.key_spec.prefix_type {
            config.expected_prefix_size = prefix_size as u16;
        }

        let value_size = match &spec.value_spec {
            ValueSpec::Fixed(fixed_size) => *fixed_size as u32,
            ValueSpec::Variable(max_size) => *max_size as u32,
        };
        config.suggest_inline_value_size(value_size);

        // Capture validation values from config before it is moved into the backend constructor.
        let node_size = config.node_size;
        let inline_value_size = config.inline_value_size;
        let max_key_size = config.max_key_size(); // fully computed by BtreeConfig::finalize()
        let partition_key_size = spec.partition_key_size;

        // Create the index backend. The .await is stripped by maybe_async_cfg in sync modes.
        // partition_key_size == 0 → single UnshardedBtree; >= 1 → ShardedBtree.
        let btree: Arc<dyn BtreeIndex> = if partition_key_size == 0 {
            Arc::new(
                homedb_common::UnshardedBtree::new(config, |c| Box::new(MemBtree::new(&c)))
                    .await
                    .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?,
            )
        } else {
            Arc::new(
                homedb_common::ShardedBtree::new(config, partition_key_size, |c| Box::new(MemBtree::new(&c)))
                    .await
                    .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?,
            )
        };

        // Validate TableSpec key constraints against btree capacity.
        let spec_max_key = match &spec.key_spec.key_type {
            KeyType::Fixed(fixed_size) => *fixed_size,
            KeyType::Variable(max_size) => *max_size,
        };
        if spec_max_key as u32 > max_key_size {
            return Err(HomeDbError::Config(format!(
                "Key size {} exceeds btree capacity {} (node_size={}, inline_value_size={})",
                spec_max_key, max_key_size, node_size, inline_value_size
            )));
        }

        Ok(Self { name, index_type, spec, btree, max_key_size })
    }

    /// Determine which btree node variant to use based on table spec.
    fn determine_node_variant(spec: &TableSpec) -> u8 {
        use crate::{KeyType, PrefixType, ValueSpec};

        // Prefixable keys are always treated as variable-sized regardless of underlying KeyType,
        // because prefix compression results in variable-length storage.
        if matches!(spec.key_spec.prefix_type, PrefixType::Prefixable(_)) {
            return 4; // PrefixCompressNode
        }

        // DbKey/DbValue have runtime-determined sizes (FIXED_SERIALIZED_SIZE = None).
        // SimpleNode requires compile-time constant sizes; use VarObjNode for runtime fixed sizes.
        match (&spec.key_spec.key_type, &spec.value_spec) {
            (KeyType::Fixed(_), ValueSpec::Fixed(_)) => 3,       // VarObjNode
            (KeyType::Variable(_), ValueSpec::Fixed(_)) => 1,    // VarKeyNode
            (KeyType::Fixed(_), ValueSpec::Variable(_)) => 2,    // VarValueNode
            (KeyType::Variable(_), ValueSpec::Variable(_)) => 3, // VarObjNode
        }
    }

    pub fn name(&self) -> &str { &self.name }

    pub fn index_type(&self) -> IndexType { self.index_type }

    pub fn spec(&self) -> &TableSpec { &self.spec }

    /// Put a single key-value pair.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        if key.len() as u32 > self.max_key_size {
            return Err(HomeDbError::KeyTooLarge { size: key.len(), max: self.max_key_size as usize });
        }
        self.spec.validate(&key, &value)?;
        let db_key = DbKey::new(key, &self.spec.key_spec);
        let db_value = DbValue::new(value, &self.spec.value_spec);
        self.btree.put(&db_key, &db_value).await.map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))
    }

    /// Get a single value by key.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        let db_key = DbKey::new(key, &self.spec.key_spec);
        let result = self.btree.get(&db_key).await.map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        Ok(result.map(|v| v.into_vec()))
    }

    /// Remove a single key.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        let db_key = DbKey::new(key, &self.spec.key_spec);
        let result = self.btree.remove(&db_key).await.map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        Ok(result.map(|v| v.into_vec()))
    }

    /// Query a range of key-value pairs, returning a batch iterator.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<homedb_common::RangeIterator> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;

        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        let start_key_copy = start_key.clone();
        let end_key_copy = end_key.clone();
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let handle = self.btree
            .query(BtreeKeyRange::new(start, true, end, false), batch_size, None, false)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;

        Ok(homedb_common::RangeIterator::new(
            Arc::clone(&self.btree), handle, start_key_copy, end_key_copy,
            batch_size, false, self.spec.key_spec.clone(),
        ))
    }

    /// Query a range in reverse order, returning a batch iterator.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_range_reverse(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<homedb_common::RangeIterator> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;

        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        let start_key_copy = start_key.clone();
        let end_key_copy = end_key.clone();
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let handle = self.btree
            .query(BtreeKeyRange::new(start, true, end, false), batch_size, None, true)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;

        Ok(homedb_common::RangeIterator::new(
            Arc::clone(&self.btree), handle, start_key_copy, end_key_copy,
            batch_size, true, self.spec.key_spec.clone(),
        ))
    }

    /// Get any key-value pair in the given range (useful for existence checks).
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn get_any(&self, start_key: Vec<u8>, end_key: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;

        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let handle = self.btree
            .query(BtreeKeyRange::new(start, true, end, false), 1, None, false)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        Ok(handle.results().first().map(|(k, v)| (k.clone().into_vec(), v.clone().into_vec())))
    }

    /// Remove any key in the given range. Returns the removed key-value pair if found.
    #[maybe_async_cfg::maybe(keep_self, sync(feature = "sync_frontend"), async(feature = "async_frontend"))]
    pub async fn remove_any(&self, start_key: Vec<u8>, end_key: Vec<u8>) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;

        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let handle = self.btree
            .query(BtreeKeyRange::new(start, true, end, false), 1, None, false)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        let maybe_key = handle.results().first().map(|(k, _)| k.clone());
        drop(handle);

        if let Some(key) = maybe_key {
            let removed = self.btree.remove(&key).await
                .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
            Ok(removed.map(|v| (key.into_vec(), v.into_vec())))
        } else {
            Ok(None)
        }
    }
}
