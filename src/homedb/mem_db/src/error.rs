//! Error types for MemDB

use std::fmt;

#[derive(Debug)]
pub enum MemDbError {
    /// Table already exists
    TableExists(String),
    
    /// Table not found
    TableNotFound(String),
    
    /// Key size mismatch (expected, actual)
    KeySizeMismatch(usize, usize),
    
    /// Value size mismatch (expected, actual)
    ValueSizeMismatch(usize, usize),
    
    /// Btree operation error
    BtreeError(String),
    
    /// Invalid configuration
    InvalidConfig(String),
}

impl fmt::Display for MemDbError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            MemDbError::TableExists(name) => write!(f, "Table '{}' already exists", name),
            MemDbError::TableNotFound(name) => write!(f, "Table '{}' not found", name),
            MemDbError::KeySizeMismatch(expected, actual) => {
                write!(f, "Key size mismatch: expected {} bytes, got {} bytes", expected, actual)
            }
            MemDbError::ValueSizeMismatch(expected, actual) => {
                write!(f, "Value size mismatch: expected {} bytes, got {} bytes", expected, actual)
            }
            MemDbError::BtreeError(msg) => write!(f, "Btree error: {}", msg),
            MemDbError::InvalidConfig(msg) => write!(f, "Invalid configuration: {}", msg),
        }
    }
}

impl std::error::Error for MemDbError {}

pub type Result<T> = std::result::Result<T, MemDbError>;
