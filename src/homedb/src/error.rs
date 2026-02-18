//! Error types for HomeDB

use std::fmt;

#[derive(Debug)]
pub enum HomeDbError {
    /// Table already exists
    TableExists(String),
    
    /// Table not found
    TableNotFound(String),
    
    /// Index already exists
    IndexExists(String),
    
    /// Index not found
    IndexNotFound(String),
    
    /// Key size mismatch (expected, actual)
    KeySizeMismatch(usize, usize),
    
    /// Value size mismatch (expected, actual)
    ValueSizeMismatch(usize, usize),
    
    /// Btree operation error
    BtreeError(String),

    /// Invalid configuration
    InvalidConfig(String),

    /// Invalid operation
    InvalidOperation(String),
    
    /// IOManager error
    IOManagerError(String),
}

impl fmt::Display for HomeDbError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            HomeDbError::TableExists(name) => write!(f, "Table '{}' already exists", name),
            HomeDbError::TableNotFound(name) => write!(f, "Table '{}' not found", name),
            HomeDbError::IndexExists(name) => write!(f, "Index '{}' already exists", name),
            HomeDbError::IndexNotFound(name) => write!(f, "Index '{}' not found", name),
            HomeDbError::KeySizeMismatch(expected, actual) => {
                write!(f, "Key size mismatch: expected {} bytes, got {} bytes", expected, actual)
            }
            HomeDbError::ValueSizeMismatch(expected, actual) => {
                write!(f, "Value size mismatch: expected {} bytes, got {} bytes", expected, actual)
            }
            HomeDbError::BtreeError(msg) => write!(f, "Btree error: {}", msg),
            HomeDbError::InvalidConfig(msg) => write!(f, "Invalid configuration: {}", msg),
            HomeDbError::InvalidOperation(msg) => write!(f, "Invalid operation: {}", msg),
            HomeDbError::IOManagerError(msg) => write!(f, "IOManager error: {}", msg),
        }
    }
}

impl std::error::Error for HomeDbError {}

pub type Result<T> = std::result::Result<T, HomeDbError>;
