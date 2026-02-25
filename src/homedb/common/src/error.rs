//! Error types for HomeDB

use std::fmt;

#[derive(Debug)]
pub enum HomeDbError {
    /// Table already exists
    TableExists(String),

    /// Table not found
    TableNotFound(String),

    /// Key size mismatch (expected, actual)
    KeySizeMismatch(usize, usize),

    /// Key too large for btree node capacity (actual, max)
    KeyTooLarge { size: usize, max: usize },

    /// Value size mismatch (expected, actual)
    ValueSizeMismatch(usize, usize),

    /// Configuration error
    Config(String),

    /// Btree operation error
    BtreeError(String),

    /// Invalid configuration
    InvalidConfig(String),

    /// Invalid operation
    InvalidOperation(String),
}

impl fmt::Display for HomeDbError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            HomeDbError::TableExists(name) => write!(f, "Table '{}' already exists", name),
            HomeDbError::TableNotFound(name) => write!(f, "Table '{}' not found", name),
            HomeDbError::KeySizeMismatch(expected, actual) => {
                write!(f, "Key size mismatch: expected {} bytes, got {} bytes", expected, actual)
            }
            HomeDbError::KeyTooLarge { size, max } => {
                write!(f, "Key size {} bytes exceeds btree capacity {} bytes", size, max)
            }
            HomeDbError::ValueSizeMismatch(expected, actual) => {
                write!(f, "Value size mismatch: expected {} bytes, got {} bytes", expected, actual)
            }
            HomeDbError::Config(msg) => write!(f, "Configuration error: {}", msg),
            HomeDbError::BtreeError(msg) => write!(f, "Btree error: {}", msg),
            HomeDbError::InvalidConfig(msg) => write!(f, "Invalid configuration: {}", msg),
            HomeDbError::InvalidOperation(msg) => write!(f, "Invalid operation: {}", msg),
        }
    }
}

impl std::error::Error for HomeDbError {}

pub type Result<T> = std::result::Result<T, HomeDbError>;
