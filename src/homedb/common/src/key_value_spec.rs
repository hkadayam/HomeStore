//! Key and Value specifications for table schema

use crate::error::{HomeDbError, Result};

/// Key type specification
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyType {
    /// Fixed-size key (size in bytes)
    Fixed(usize),
    /// Variable-size key (max size in bytes)
    Variable(usize),
}

/// Key prefix compression specification
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PrefixType {
    /// Key supports prefix compression (prefix size in bytes)
    Prefixable(Option<usize>),
    /// Regular key without prefix compression
    Regular,
}

/// Complete key specification
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeySpec {
    /// Key type (fixed or variable)
    pub key_type: KeyType,
    /// Prefix compression type
    pub prefix_type: PrefixType,
}

impl KeySpec {
    /// Create a fixed-size key spec
    pub fn fixed(size: usize) -> Self {
        Self {
            key_type: KeyType::Fixed(size),
            prefix_type: PrefixType::Regular,
        }
    }

    /// Create a variable-size key spec
    pub fn variable(max_size: usize) -> Self {
        Self {
            key_type: KeyType::Variable(max_size),
            prefix_type: PrefixType::Regular,
        }
    }

    /// Enable prefix compression on this key spec
    pub fn prefixable(mut self, prefix_size: Option<usize>) -> Self {
        self.prefix_type = PrefixType::Prefixable(prefix_size);
        self
    }

    /// Get the maximum key size in bytes
    pub fn max_size(&self) -> usize {
        match self.key_type {
            KeyType::Fixed(size) => size,
            KeyType::Variable(max_size) => max_size,
        }
    }

    /// Check if this is a fixed-size key
    pub fn is_fixed(&self) -> bool {
        matches!(self.key_type, KeyType::Fixed(_))
    }

    /// Check if prefix compression is enabled
    pub fn is_prefixable(&self) -> bool {
        matches!(self.prefix_type, PrefixType::Prefixable(_))
    }

    /// Validate a key buffer against this spec
    pub fn validate_key(&self, key: &[u8]) -> Result<()> {
        match self.key_type {
            KeyType::Fixed(size) => {
                if key.len() != size {
                    return Err(HomeDbError::KeySizeMismatch(size, key.len()));
                }
            }
            KeyType::Variable(max_size) => {
                if key.len() > max_size {
                    return Err(HomeDbError::KeySizeMismatch(max_size, key.len()));
                }
            }
        }
        Ok(())
    }
}

/// Value type specification
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueSpec {
    /// Fixed-size value (size in bytes)
    Fixed(usize),
    /// Variable-size value (max size in bytes)
    Variable(usize),
}

impl ValueSpec {
    /// Create a fixed-size value spec
    pub fn fixed(size: usize) -> Self {
        ValueSpec::Fixed(size)
    }

    /// Create a variable-size value spec
    pub fn variable(max_size: usize) -> Self {
        ValueSpec::Variable(max_size)
    }

    /// Get the maximum value size in bytes
    pub fn max_size(&self) -> usize {
        match self {
            ValueSpec::Fixed(size) => *size,
            ValueSpec::Variable(max_size) => *max_size,
        }
    }

    /// Check if this is a fixed-size value
    pub fn is_fixed(&self) -> bool {
        matches!(self, ValueSpec::Fixed(_))
    }

    /// Validate a value buffer against this spec
    pub fn validate_value(&self, value: &[u8]) -> Result<()> {
        match self {
            ValueSpec::Fixed(size) => {
                if value.len() != *size {
                    return Err(HomeDbError::ValueSizeMismatch(*size, value.len()));
                }
            }
            ValueSpec::Variable(max_size) => {
                if value.len() > *max_size {
                    return Err(HomeDbError::ValueSizeMismatch(*max_size, value.len()));
                }
            }
        }
        Ok(())
    }
}

/// Complete table specification (key + value specs)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TableSpec {
    pub key_spec: KeySpec,
    pub value_spec: ValueSpec,
    /// Number of leading key bytes that identify a partition (routing prefix).
    /// `0` means no partitioning — a single UnshardedBtree is used.
    /// `>= 1` enables ShardedBtree: keys with the same leading bytes share a shard.
    /// Default is `2`.
    pub partition_key_size: usize,
    /// BTree node size in bytes. Larger nodes hold more entries per page, reducing tree
    /// height and cache misses at the cost of higher per-node I/O. Default is 4096.
    /// Inline value threshold is clamped to node_size/32 by BtreeConfig.
    pub node_size: u32,
}

impl TableSpec {
    /// Create a new table specification (partition_key_size defaults to 2).
    pub fn new(key_spec: KeySpec, value_spec: ValueSpec) -> Self {
        Self { key_spec, value_spec, partition_key_size: 2, node_size: 4096 }
    }

    /// Create a table spec with fixed-size keys and values (partition_key_size defaults to 2).
    pub fn fixed_kv(key_size: usize, value_size: usize) -> Self {
        Self {
            key_spec: KeySpec::fixed(key_size),
            value_spec: ValueSpec::fixed(value_size),
            partition_key_size: 2,
            node_size: 4096,
        }
    }

    /// Override the partition key size (builder-style).
    pub fn partition_key_size(mut self, size: usize) -> Self {
        self.partition_key_size = size;
        self
    }

    /// Override the BTree node size (builder-style).
    pub fn node_size(mut self, size: u32) -> Self {
        self.node_size = size;
        self
    }

    /// Validate key and value buffers
    pub fn validate(&self, key: &[u8], value: &[u8]) -> Result<()> {
        self.key_spec.validate_key(key)?;
        self.value_spec.validate_value(value)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_key_spec_fixed() {
        let spec = KeySpec::fixed(8);
        assert!(spec.is_fixed());
        assert_eq!(spec.max_size(), 8);
        assert!(!spec.is_prefixable());

        assert!(spec.validate_key(&[0u8; 8]).is_ok());
        assert!(spec.validate_key(&[0u8; 7]).is_err());
        assert!(spec.validate_key(&[0u8; 9]).is_err());
    }

    #[test]
    fn test_key_spec_variable() {
        let spec = KeySpec::variable(16);
        assert!(!spec.is_fixed());
        assert_eq!(spec.max_size(), 16);
        assert!(spec.validate_key(&[0u8; 1]).is_ok());
        assert!(spec.validate_key(&[0u8; 16]).is_ok());
        assert!(spec.validate_key(&[0u8; 17]).is_err());
    }

    #[test]
    fn test_key_spec_prefixable() {
        let spec = KeySpec::fixed(8).prefixable(Some(4));
        assert!(spec.is_fixed());
        assert!(spec.is_prefixable());
    }

    #[test]
    fn test_value_spec() {
        let spec = ValueSpec::fixed(16);
        assert!(spec.is_fixed());
        assert_eq!(spec.max_size(), 16);
        assert!(spec.validate_value(&[0u8; 16]).is_ok());
        assert!(spec.validate_value(&[0u8; 15]).is_err());
    }

    #[test]
    fn test_table_spec() {
        let spec = TableSpec::fixed_kv(8, 16);
        assert!(spec.validate(&[0u8; 8], &[0u8; 16]).is_ok());
        assert!(spec.validate(&[0u8; 7], &[0u8; 16]).is_err());
        assert!(spec.validate(&[0u8; 8], &[0u8; 15]).is_err());
    }
}
