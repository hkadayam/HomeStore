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

use std::sync::Mutex;

// ═══════════════════════════════════════════════════════════════════════════
// Singleton Pattern (like init_iomgr)
// ═══════════════════════════════════════════════════════════════════════════

static MEM_HOMEDB: Mutex<Option<MemoryDB>> = Mutex::new(None);

/// Initialize the in-memory HomeDB singleton
/// 
/// This should be called once at application startup. Subsequent calls will
/// return an error. The singleton can be accessed via `mem_homedb()`.
/// 
/// # Arguments
/// * `num_reactors` - Number of reactor threads (typically number of CPU cores)
/// 
/// # Example
/// ```ignore
/// use mem_db::{init_mem_homedb, mem_homedb};
/// 
/// // Initialize once
/// init_mem_homedb(4)?;
/// 
/// // Access singleton
/// let db = mem_homedb();
/// let table = db.create_table("users", spec).await?;
/// ```
pub fn init_mem_homedb(num_reactors: usize) -> Result<()> {
    let mut db = MEM_HOMEDB.lock().unwrap();
    
    if db.is_some() {
        return Err(MemDbError::InvalidConfig(
            "MemoryDB already initialized. Use mem_homedb() to access it.".to_string()
        ));
    }
    
    *db = Some(MemoryDB::new(num_reactors)?);
    Ok(())
}

/// Get the singleton MemoryDB instance
/// 
/// # Panics
/// Panics if `init_mem_homedb()` has not been called yet.
/// 
/// # Example
/// ```ignore
/// let db = mem_homedb();
/// let users = db.get_table("users")?;
/// users.put(&key, &value).await?;
/// ```
pub fn mem_homedb() -> &'static MemoryDB {
    // Safety: We use a static reference with proper initialization check
    unsafe {
        let ptr = &MEM_HOMEDB as *const Mutex<Option<MemoryDB>>;
        let db_ref = &*ptr;
        let guard = db_ref.lock().unwrap();
        
        if guard.is_none() {
            panic!("MemoryDB not initialized. Call init_mem_homedb() first.");
        }
        
        // Convert Option<MemoryDB> to &MemoryDB with static lifetime
        // This is safe because the Mutex ensures the value never moves
        std::mem::transmute::<&MemoryDB, &'static MemoryDB>(
            guard.as_ref().unwrap()
        )
    }
}

/// Shutdown the MemoryDB singleton and all reactors
/// 
/// This should be called before application exit for clean shutdown.
pub async fn shutdown_mem_homedb() {
    let db = MEM_HOMEDB.lock().unwrap();
    
    if db.is_some() {
        drop(db); // Release lock before async operation
        let _ = iomgr::shutdown_iomgr().await;
        
        // Clear the singleton
        let mut db = MEM_HOMEDB.lock().unwrap();
        *db = None;
    }
}
