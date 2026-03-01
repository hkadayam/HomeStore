use homestore::index::btree::btree_kvs::{BtreeKey, BtreeValue, Partitionable};
use super::key_value_spec::{KeySpec, ValueSpec};

/// Data storage for DbKey - either owned or borrowed
enum DbKeyData {
    Owned(Vec<u8>),             // User input or copy=true deserialization
    Borrowed(*const u8, usize), // Temp borrowed during search (copy=false)
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
    pub fn new(data: Vec<u8>, key_spec: &KeySpec) -> Self {
        use super::key_value_spec::KeyType;
        let fixed_size = match key_spec.key_type {
            KeyType::Fixed(size) => Some(size),
            KeyType::Variable(_) => None,
        };
        Self { data: DbKeyData::Owned(data), fixed_size }
    }

    /// Get bytes as slice (works for both Owned and Borrowed)
    pub fn as_bytes(&self) -> &[u8] {
        match &self.data {
            DbKeyData::Owned(vec) => vec.as_slice(),
            DbKeyData::Borrowed(ptr, len) => unsafe { std::slice::from_raw_parts(*ptr, *len) },
        }
    }

    /// Extract owned data (for returning to user)
    pub fn into_vec(self) -> Vec<u8> {
        match self.data {
            DbKeyData::Owned(vec) => vec,
            DbKeyData::Borrowed(ptr, len) => {
                // This should NEVER happen - Borrowed is only for temp btree comparisons
                debug_assert!(false, "into_vec() called on Borrowed DbKey - this is a bug!");
                unsafe { std::slice::from_raw_parts(ptr, len).to_vec() }
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
    fn eq(&self, other: &Self) -> bool { self.as_bytes() == other.as_bytes() }
}

impl Eq for DbKey {}

impl PartialOrd for DbKey {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> { Some(self.cmp(other)) }
}

impl Ord for DbKey {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering { self.as_bytes().cmp(other.as_bytes()) }
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

    fn fixed_serialized_size(&self) -> Option<u32> { self.fixed_size.map(|s| s as u32) }

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
        // Conservative max for variable keys. For interior nodes, btree uses this
        // to check if there's room for one more entry. Should be much smaller than node_size.
        // Typical use cases: fixed keys (8-128 bytes) or bounded variable keys (<1KB).
        512
    }
}

impl Partitionable for DbKey {
    fn with_partition_bytes<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        f(self.as_bytes())
    }
}

/// Data storage for DbValue - either owned or borrowed
enum DbValueData {
    Owned(Vec<u8>),             // User input or copy=true deserialization
    Borrowed(*const u8, usize), // Temp borrowed during search (copy=false)
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
    pub fn new(data: Vec<u8>, value_spec: &ValueSpec) -> Self {
        use super::key_value_spec::ValueSpec;
        let fixed_size = match value_spec {
            ValueSpec::Fixed(size) => Some(*size),
            ValueSpec::Variable(_) => None,
        };
        Self {
            data: DbValueData::Owned(data),
            fixed_size,
        }
    }

    /// Get bytes as slice (works for both Owned and Borrowed)
    pub fn as_bytes(&self) -> &[u8] {
        match &self.data {
            DbValueData::Owned(vec) => vec.as_slice(),
            DbValueData::Borrowed(ptr, len) => unsafe { std::slice::from_raw_parts(*ptr, *len) },
        }
    }

    /// Extract owned data (for returning to user)
    pub fn into_vec(self) -> Vec<u8> {
        match self.data {
            DbValueData::Owned(vec) => vec,
            DbValueData::Borrowed(ptr, len) => {
                // This should NEVER happen - Borrowed is only for temp btree comparisons
                debug_assert!(false, "into_vec() called on Borrowed DbValue - this is a bug!");
                unsafe { std::slice::from_raw_parts(ptr, len).to_vec() }
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
    fn eq(&self, other: &Self) -> bool { self.as_bytes() == other.as_bytes() }
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

    fn fixed_serialized_size(&self) -> Option<u32> { self.fixed_size.map(|s| s as u32) }

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
