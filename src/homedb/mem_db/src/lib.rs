//! # MemDB - In-Memory Key-Value Store
//!
//! A high-performance in-memory database built on Homestore's Btree implementation.
//! Supports multiple tables with multiple indices per table.
//!
//! ## Architecture
//! 
//! ```text
//! Process
//!   └─ init_mem_homedb() → Singleton MemoryDB
//!       ├─ init_iomgr() → Starts reactors
//!       └─ Tables: DashMap<String, Arc<Table>>
//!           │
//!           ├─ Table "users"
//!           │   ├─ TableIndex "primary" (B-tree)
//!           │   └─ TableIndex "email_idx" (B-tree)
//!           │
//!           └─ Table "orders"
//!               └─ TableIndex "primary" (B-tree)
//! ```
//!
//! ## Features
//! - Singleton pattern for database initialization
//! - Multiple tables, each with multiple indices
//! - Flexible key/value schemas (fixed/variable size, prefix compression)
//! - Iterator-based range queries
//! - Thread-safe table and index management
//! - Handle-based API (no string lookups in critical path)
//!
//! ## Example
//! ```ignore
//! use mem_db::{init_mem_homedb, mem_homedb, TableSpec};
//!
//! // Initialize singleton (once per process)
//! init_mem_homedb(4)?; // 4 reactor threads
//! 
//! let db = mem_homedb();
//!
//! // Create a table with fixed 8-byte keys and values
//! let spec = TableSpec::fixed_kv(8, 8);
//! let users_table = db.create_table("users", spec).await?;
//!
//! // Operate directly on table handle (no string lookup!)
//! users_table.put(&key, &value).await?;
//! let value = users_table.get(&key).await?;
//!
//! // Or create secondary indices
//! let email_idx = users_table.create_index("email_idx", email_spec).await?;
//! email_idx.put(&email, &user_id).await?;
//!
//! // Convenience methods also available (but do string lookup)
//! db.put_one("users", &key, &value).await?;
//! ```

mod key_value_spec;
mod table_index;
mod table;
mod memory_db;
mod iterator;
mod error;

pub use mem_db_macros::reactor_method;

pub use key_value_spec::{KeySpec, ValueSpec, TableSpec};
pub use table_index::{TableIndex, IndexType};
pub use table::Table;
pub use memory_db::MemoryDB;
pub use iterator::RangeIterator;
pub use error::{MemDbError, Result};


// ═══════════════════════════════════════════════════════════════════════════
// IOManager Convenience Wrappers
// ═══════════════════════════════════════════════════════════════════════════

/// Initialize IOManager for async mode (convenience wrapper)
/// 
/// In async mode, this initializes the IOManager with the specified number of reactors.
/// In sync mode, this is a no-op.
/// 
/// This is idempotent and refcounted - safe to call multiple times.
/// 
/// # Arguments
/// * `num_reactors` - Number of reactor threads (typically number of CPU cores)
/// 
/// # Example
/// ```ignore
/// use mem_db::{init_mem_homedb, MemoryDB};
/// 
/// // Initialize IOManager once (async mode only)
/// init_mem_homedb(4)?;
/// 
/// // Create MemoryDB instances as needed
/// let db = MemoryDB::new()?;
/// let table = db.create_table("users", spec).await?;
/// ```
pub fn init_mem_homedb(num_reactors: usize) -> Result<()> {
    #[cfg(feature = "async_mode")]
    {
        iomgr::init_iomgr(num_reactors)
            .map_err(|e| MemDbError::InvalidConfig(format!("Failed to initialize IOManager: {}", e)))?;
    }
    
    #[cfg(feature = "sync_mode")]
    {
        let _ = num_reactors; // Unused in sync mode
    }
    
    Ok(())
}

/// Shutdown IOManager (convenience wrapper)
/// 
/// In async mode, shuts down the IOManager and all reactors.
/// In sync mode, this is a no-op.
/// 
/// This is refcounted - actual shutdown only happens when refcount reaches 0.
#[cfg(feature = "sync_mode")]
pub fn shutdown_mem_homedb() {
    // No-op in sync mode
}

#[cfg(feature = "async_mode")]
pub async fn shutdown_mem_homedb() {
    let _ = iomgr::shutdown_iomgr().await;
}
