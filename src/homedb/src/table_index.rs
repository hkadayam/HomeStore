//! TableIndex implementation - wraps a Btree with schema validation
//! 
//! A TableIndex represents a physical B-tree index within a Table.
//! Each table can have multiple indices (primary, secondary, etc.)

use std::sync::Arc;
use homestore::index::btree::{
    btree::Btree,
    BtreeConfig,  // Re-exported from btree_types at btree module level
    btree_kvs::{BtreeKey, BtreeValue},
    underlying::mem::MemBtree,
};
use crate::{
    key_value_spec::TableSpec,
    error::{HomeDbError, Result},
};

/// Data storage for DbKey - either owned or borrowed
enum DbKeyData {
    Owned(Vec<u8>),                 // User input or copy=true deserialization
    Borrowed(*const u8, usize),     // Temp borrowed during search (copy=false)
}

// Safety: Borrowed variant is only used temporarily within btree operations
// and never crosses thread boundaries in practice (single-threaded during search)
unsafe impl Send for DbKeyData {}
unsafe impl Sync for DbKeyData {}

/// Generic key type for MemDB tables
/// 
/// DbKey is intelligent - it knows whether it represents a fixed or variable-sized key
/// based on the TableSpec it was created with.
/// 
/// Zero-copy design:
/// - User passes Vec<u8> → Owned variant (moved, not copied)
/// - Btree search → Borrowed variant (zero-copy temp references)
pub struct DbKey {
    data: DbKeyData,
    pub(crate) fixed_size: Option<usize>, // None = variable, Some(n) = fixed n bytes
}

impl DbKey {
    /// Create a new DbKey from owned data with schema information
    /// 
    /// The key carries size information from the KeySpec, allowing the B-tree
    /// to select the optimal node variant.
    /// 
    /// Data is moved (not copied) into the DbKey.
    pub fn new(data: Vec<u8>, key_spec: &crate::key_value_spec::KeySpec) -> Self {
        use crate::key_value_spec::KeyType;
        let fixed_size = match key_spec.key_type {
            KeyType::Fixed(size) => Some(size),
            KeyType::Variable(_) => None,
        };
        Self { 
            data: DbKeyData::Owned(data),
            fixed_size 
        }
    }
    
    /// Get bytes as slice (works for both Owned and Borrowed)
    pub fn as_bytes(&self) -> &[u8] {
        match &self.data {
            DbKeyData::Owned(vec) => vec.as_slice(),
            DbKeyData::Borrowed(ptr, len) => unsafe { 
                std::slice::from_raw_parts(*ptr, *len) 
            }
        }
    }
    
    /// Extract owned data (for returning to user)
    pub fn into_vec(self) -> Vec<u8> {
        match self.data {
            DbKeyData::Owned(vec) => vec,
            DbKeyData::Borrowed(ptr, len) => {
                // This should NEVER happen - Borrowed is only for temp btree comparisons
                debug_assert!(false, "into_vec() called on Borrowed DbKey - this is a bug!");
                unsafe {
                    std::slice::from_raw_parts(ptr, len).to_vec()
                }
            }
        }
    }
}

impl Clone for DbKey {
    fn clone(&self) -> Self {
        // Always clone as Owned to avoid dangling pointers from Borrowed variant
        Self {
            data: DbKeyData::Owned(self.as_bytes().to_vec()),
            fixed_size: self.fixed_size,
        }
    }
}

impl PartialEq for DbKey {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}

impl Eq for DbKey {}

impl PartialOrd for DbKey {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for DbKey {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.as_bytes().cmp(other.as_bytes())
    }
}

impl std::fmt::Debug for DbKey {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self.fixed_size {
            Some(size) => write!(f, "DbKey(fixed {} bytes)", size),
            None => write!(f, "DbKey(var {} bytes)", self.as_bytes().len()),
        }
    }
}

impl BtreeKey for DbKey {
    const FIXED_SERIALIZED_SIZE: Option<u32> = None; // Determined at runtime
    
    fn serialized_size(&self) -> u32 {
        let bytes = self.as_bytes();
        self.fixed_size.unwrap_or(bytes.len()) as u32
    }
    
    fn fixed_serialized_size(&self) -> Option<u32> {
        self.fixed_size.map(|s| s as u32)
    }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> std::io::Result<u32> {
        let bytes = self.as_bytes();
        let len = bytes.len();
        buf[..len].copy_from_slice(bytes);
        Ok(len as u32)
    }
    
    fn deserialize_from(buf: &[u8], copy: bool) -> std::io::Result<Self> {
        if copy {
            // Owned copy for user/persistent use
            Ok(DbKey {
                data: DbKeyData::Owned(buf.to_vec()),
                fixed_size: None,
            })
        } else {
            // Zero-copy borrowed reference for temp comparison
            Ok(DbKey {
                data: DbKeyData::Borrowed(buf.as_ptr(), buf.len()),
                fixed_size: None,
            })
        }
    }
    
    fn get_max_size() -> u32 {
        4096 // Reasonable default max key size
    }
}

/// Data storage for DbValue - either owned or borrowed
enum DbValueData {
    Owned(Vec<u8>),                 // User input or copy=true deserialization
    Borrowed(*const u8, usize),     // Temp borrowed during search (copy=false)
}

// Safety: Borrowed variant is only used temporarily within btree operations
// and never crosses thread boundaries in practice (single-threaded during search)
unsafe impl Send for DbValueData {}
unsafe impl Sync for DbValueData {}

/// Generic value type for MemDB tables
/// 
/// DbValue is intelligent - it knows whether it represents a fixed or variable-sized value
/// based on the TableSpec it was created with.
/// 
/// Zero-copy design:
/// - User passes Vec<u8> → Owned variant (moved, not copied)
/// - Btree search → Borrowed variant (zero-copy temp references)
pub struct DbValue {
    data: DbValueData,
    pub(crate) fixed_size: Option<usize>, // None = variable, Some(n) = fixed n bytes
}

impl DbValue {
    /// Create a new DbValue from owned data with schema information
    /// 
    /// The value carries size information from the ValueSpec, allowing the B-tree
    /// to select the optimal node variant.
    /// 
    /// Data is moved (not copied) into the DbValue.
    pub fn new(data: Vec<u8>, value_spec: &crate::key_value_spec::ValueSpec) -> Self {
        use crate::key_value_spec::ValueSpec;
        let fixed_size = match value_spec {
            ValueSpec::Fixed(size) => Some(*size),
            ValueSpec::Variable(_) => None,
        };
        Self { 
            data: DbValueData::Owned(data),
            fixed_size 
        }
    }
    
    /// Get bytes as slice (works for both Owned and Borrowed)
    pub fn as_bytes(&self) -> &[u8] {
        match &self.data {
            DbValueData::Owned(vec) => vec.as_slice(),
            DbValueData::Borrowed(ptr, len) => unsafe { 
                std::slice::from_raw_parts(*ptr, *len) 
            }
        }
    }
    
    /// Extract owned data (for returning to user)
    pub fn into_vec(self) -> Vec<u8> {
        match self.data {
            DbValueData::Owned(vec) => vec,
            DbValueData::Borrowed(ptr, len) => {
                // This should NEVER happen - Borrowed is only for temp btree comparisons
                debug_assert!(false, "into_vec() called on Borrowed DbValue - this is a bug!");
                unsafe {
                    std::slice::from_raw_parts(ptr, len).to_vec()
                }
            }
        }
    }
}

impl Clone for DbValue {
    fn clone(&self) -> Self {
        // Always clone as Owned to avoid dangling pointers from Borrowed variant
        Self {
            data: DbValueData::Owned(self.as_bytes().to_vec()),
            fixed_size: self.fixed_size,
        }
    }
}

impl PartialEq for DbValue {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}

impl Eq for DbValue {}

impl std::fmt::Debug for DbValue {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self.fixed_size {
            Some(size) => write!(f, "DbValue(fixed {} bytes)", size),
            None => write!(f, "DbValue(var {} bytes)", self.as_bytes().len()),
        }
    }
}

impl BtreeValue for DbValue {
    const FIXED_SERIALIZED_SIZE: Option<u32> = None; // Determined at runtime
    
    fn serialized_size(&self) -> u32 {
        let bytes = self.as_bytes();
        self.fixed_size.unwrap_or(bytes.len()) as u32
    }
    
    fn fixed_serialized_size(&self) -> Option<u32> {
        self.fixed_size.map(|s| s as u32)
    }
    
    fn serialize_to(&self, buf: &mut [u8], _copy: bool) -> std::io::Result<u32> {
        let bytes = self.as_bytes();
        let len = bytes.len();
        buf[..len].copy_from_slice(bytes);
        Ok(len as u32)
    }
    
    fn deserialize_from(buf: &[u8], copy: bool) -> std::io::Result<Self> {
        if copy {
            // Owned copy for user/persistent use
            Ok(DbValue {
                data: DbValueData::Owned(buf.to_vec()),
                fixed_size: None,
            })
        } else {
            // Zero-copy borrowed reference for temp comparison
            Ok(DbValue {
                data: DbValueData::Borrowed(buf.as_ptr(), buf.len()),
                fixed_size: None,
            })
        }
    }
}

/// Index type classification
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IndexType {
    /// Primary index - stores the full row data
    Primary,
    /// Secondary index - stores pointer to primary key
    Secondary,
}

/// A physical B-tree index within a table
/// 
/// TableIndex wraps a B-tree and provides schema-validated operations.
/// Each table can have multiple indices (primary index, secondary indices).
#[derive(Clone)]
pub struct TableIndex {
    name: String,
    index_type: IndexType,
    spec: TableSpec,
    btree: Arc<Btree<DbKey, DbValue>>,
}

impl TableIndex {
    /// Create a new table index with the given specification
    /// 
    /// # Arguments
    /// * `name` - Name of the index (e.g., "primary", "email_idx")
    /// * `index_type` - Type of index (Primary or Secondary)
    /// * `spec` - Schema specification for keys and values
    pub async fn new(name: String, index_type: IndexType, spec: TableSpec) -> Result<Self> {
        // Determine node variant based on KeySpec and ValueSpec
        let node_variant = Self::determine_node_variant(&spec);
        
        // Create btree config
        let mut config = BtreeConfig::new(4096, name.clone());
        config.leaf_node_variant = node_variant;
        config.int_node_variant = node_variant;
        
        // Set expected_prefix_size if using prefix compression with fixed size
        if let crate::key_value_spec::PrefixType::Prefixable(Some(prefix_size)) = spec.key_spec.prefix_type {
            config.expected_prefix_size = prefix_size as u16;
        }
        
        // Create underlying storage
        let storage = Box::new(MemBtree::new(config.node_size));
        
        // Create btree
        let btree = Btree::<DbKey, DbValue>::new(config, storage, None)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        
        Ok(Self {
            name,
            index_type,
            spec,
            btree: Arc::new(btree),
        })
    }
    
    /// Determine which btree node variant to use based on table spec
    /// 
    /// Now that DbKey/DbValue support runtime size determination via serialized_size(),
    /// we can select the optimal node variant:
    /// - Variant 0 (SimpleNode): Fixed key + Fixed value (fastest)
    /// - Variant 1 (VarKeyNode): Variable key + Fixed value
    /// - Variant 2 (VarValueNode): Fixed key + Variable value  
    /// - Variant 3 (VarObjNode): Variable key + Variable value
    /// - Variant 4 (PrefixCompressNode): Fixed key + Fixed value with prefix compression
    fn determine_node_variant(spec: &TableSpec) -> u8 {
        use crate::key_value_spec::{KeyType, ValueSpec, PrefixType};
        
        // IMPORTANT: Prefixable keys are ALWAYS treated as variable-sized,
        // regardless of their underlying KeyType, because prefix compression
        // results in variable-length storage.
        
        // First, check if key is prefixable -> use PrefixCompressNode (variant 4)
        if matches!(spec.key_spec.prefix_type, PrefixType::Prefixable(_)) {
            return 4; // PrefixCompressNode
        }
        
        // For non-prefixable keys: Note that DbKey/DbValue have runtime-determined sizes
        // (FIXED_SERIALIZED_SIZE = None). SimpleNode requires compile-time constant sizes,
        // so we use VarObjNode which handles runtime-determined fixed sizes efficiently.
        match (&spec.key_spec.key_type, &spec.value_spec) {
            // Fixed key + Fixed value -> VarObjNode (handles runtime fixed sizes)
            (KeyType::Fixed(_), ValueSpec::Fixed(_)) => 3, // VarObjNode
            
            // Variable key + Fixed value
            (KeyType::Variable(_), ValueSpec::Fixed(_)) => 1, // VarKeyNode
            
            // Fixed key + Variable value
            (KeyType::Fixed(_), ValueSpec::Variable(_)) => 2, // VarValueNode
            
            // Variable key + Variable value
            (KeyType::Variable(_), ValueSpec::Variable(_)) => 3, // VarObjNode
        }
    }
    
    /// Get index name
    pub fn name(&self) -> &str {
        &self.name
    }
    
    /// Get index type
    pub fn index_type(&self) -> IndexType {
        self.index_type
    }
    
    /// Get table specification
    pub fn spec(&self) -> &TableSpec {
        &self.spec
    }
    
    /// Put a single key-value pair
    #[crate::reactor_method]
    pub async fn put(&self, key: Vec<u8>, value: Vec<u8>) -> Result<()> {
        // Validate key and value against spec
        self.spec.validate(&key, &value)?;
        
        // Create DbKey/DbValue with spec info (moved, not copied)
        let db_key = DbKey::new(key, &self.spec.key_spec);
        let db_value = DbValue::new(value, &self.spec.value_spec);
        
        self.btree
            .put_one(&db_key, &db_value, None)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        
        Ok(())
    }
    
    /// Get a single value by key
    #[crate::reactor_method]
    pub async fn get(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        
        // Create DbKey with spec info (moved)
        let db_key = DbKey::new(key, &self.spec.key_spec);
        
        let result = self.btree
            .get(&db_key)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        
        Ok(result.map(|v| v.into_vec()))
    }
    
    /// Remove a single key
    #[crate::reactor_method]
    pub async fn remove(&self, key: Vec<u8>) -> Result<Option<Vec<u8>>> {
        self.spec.key_spec.validate_key(&key)?;
        
        // Create DbKey with spec info (moved)
        let db_key = DbKey::new(key, &self.spec.key_spec);
        
        let result = self.btree
            .remove_one(&db_key)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        
        Ok(result.map(|v| v.into_vec()))
    }
    
    /// Get the btree for internal use (for range queries, etc.)
    pub(crate) fn btree(&self) -> &Arc<Btree<DbKey, DbValue>> {
        &self.btree
    }
    
    /// Query a range of key-value pairs
    /// 
    /// Returns an iterator that fetches results in batches for efficient memory usage.
    /// 
    /// # Arguments
    /// - `start_key`: Start of range (inclusive)
    /// - `end_key`: End of range (exclusive)
    /// - `batch_size`: Number of results to fetch per batch
    #[crate::reactor_method]
    pub async fn get_range(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<crate::iterator::RangeIterator> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;
        
        // Validate keys
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;

        // Store keys before moving them
        let start_key_copy = start_key.clone();
        let end_key_copy = end_key.clone();

        // Create range query with spec info (moved)
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let range = BtreeKeyRange::new(start, true, end, false);

        let btree = Arc::clone(&self.btree);
        let handle = btree
            .query(range, batch_size, None)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;

        Ok(crate::iterator::RangeIterator::new(btree, handle, start_key_copy, end_key_copy, batch_size, false, self.spec.key_spec.clone()))
    }
    
    /// Query a range in reverse order
    /// 
    /// # Arguments
    /// - `start_key`: Start of range (inclusive, logically higher)
    /// - `end_key`: End of range (exclusive, logically lower)
    /// - `batch_size`: Number of results to fetch per batch
    #[crate::reactor_method]
    pub async fn get_range_reverse(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
        batch_size: u32,
    ) -> Result<crate::iterator::RangeIterator> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;
        
        // Validate keys
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;

        // Store keys before moving them
        let start_key_copy = start_key.clone();
        let end_key_copy = end_key.clone();

        // Create reverse range query with spec info (moved)
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let range = BtreeKeyRange::new(start, true, end, false);

        let btree = Arc::clone(&self.btree);
        let handle = btree
            .query_traversal(range, batch_size, None, true) // reverse=true
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;

        Ok(crate::iterator::RangeIterator::new(btree, handle, start_key_copy, end_key_copy, batch_size, true, self.spec.key_spec.clone()))
    }
    
    /// Get any key-value pair in the given range
    /// 
    /// Useful for existence checks or sampling.
    /// 
    /// # Arguments
    /// - `start_key`: Start of range (inclusive)
    /// - `end_key`: End of range (exclusive)
    #[crate::reactor_method]
    pub async fn get_any(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        // Validate keys
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        
        // Create keys with spec info (moved)
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        
        let result = self.btree
            .get_any(&start, &end)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        
        Ok(result.map(|(k, v)| (k.into_vec(), v.into_vec())))
    }
    
    /// Remove any key in the given range
    /// 
    /// Removes and returns one arbitrary key-value pair from the range.
    /// 
    /// # Arguments
    /// - `start_key`: Start of range (inclusive)
    /// - `end_key`: End of range (exclusive)
    #[crate::reactor_method]
    pub async fn remove_any(
        &self,
        start_key: Vec<u8>,
        end_key: Vec<u8>,
    ) -> Result<Option<(Vec<u8>, Vec<u8>)>> {
        use homestore::index::btree::detail::btree_req::BtreeKeyRange;
        
        // Validate keys
        self.spec.key_spec.validate_key(&start_key)?;
        self.spec.key_spec.validate_key(&end_key)?;
        
        // Create keys with spec info (moved)
        let start = DbKey::new(start_key, &self.spec.key_spec);
        let end = DbKey::new(end_key, &self.spec.key_spec);
        let range = BtreeKeyRange::new(start, true, end, false);
        
        let result = self.btree
            .remove_any(range)
            .await
            .map_err(|e| HomeDbError::BtreeError(format!("{:?}", e)))?;
        
        Ok(result.map(|(k, v)| (k.into_vec(), v.into_vec())))
    }
}
