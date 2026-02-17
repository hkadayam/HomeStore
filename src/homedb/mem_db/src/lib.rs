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
// Singleton Pattern (like init_iomgr)
// ═══════════════════════════════════════════════════════════════════════════

#[cfg(test)]
static MEM_HOMEDB: parking_lot::RwLock<Option<Box<MemoryDB>>> = parking_lot::RwLock::const_new(None);

#[cfg(test)]
static HOMEDB_INIT_COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

#[cfg(not(test))]
static mut MEM_HOMEDB: Option<MemoryDB> = None;

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
    #[cfg(test)]
    {
        use std::sync::atomic::Ordering;
        
        // Fast path: already initialized
        if HOMEDB_INIT_COUNT.load(Ordering::Acquire) > 0 {
            HOMEDB_INIT_COUNT.fetch_add(1, Ordering::SeqCst);
            return Ok(());
        }
        
        // Slow path: acquire write lock
        let mut guard = MEM_HOMEDB.write();
        
        // Atomically increment and check if we're first (or if we need to recreate after full shutdown)
        let prev = HOMEDB_INIT_COUNT.fetch_add(1, Ordering::SeqCst);
        
        if prev == 0 || guard.is_none() {
            // We're first OR MemoryDB was destroyed in previous shutdown - (re)create it
            *guard = Some(Box::new(MemoryDB::new(num_reactors)?));
        }
        // else: someone beat us, they already created it
        
        Ok(())
    }
    
    #[cfg(not(test))]
    unsafe {
        if MEM_HOMEDB.is_some() {
            return Err(MemDbError::InvalidConfig(
                "MemoryDB already initialized. Use mem_homedb() to access it.".to_string()
            ));
        }
        
        MEM_HOMEDB = Some(MemoryDB::new(num_reactors)?);
        Ok(())
    }
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
    #[cfg(test)]
    {
        use std::sync::atomic::Ordering;
        
        let count = HOMEDB_INIT_COUNT.load(Ordering::Acquire);
        assert!(count > 0, "MemoryDB not initialized. Call init_mem_homedb() first.");
        
        let guard = MEM_HOMEDB.read();
        let db_ref = guard.as_ref().expect("MemoryDB missing despite count > 0");
        
        // Safety: count > 0 means MemoryDB is alive
        // Tests are disciplined: they finish using it before shutdown
        unsafe {
            std::mem::transmute::<&MemoryDB, &'static MemoryDB>(db_ref.as_ref())
        }
    }
    
    #[cfg(not(test))]
    #[allow(static_mut_refs)]
    unsafe {
        MEM_HOMEDB.as_ref().expect("MemoryDB not initialized. Call init_mem_homedb() first.")
    }
}

/// Shutdown the MemoryDB singleton and all reactors
/// 
/// This should be called before application exit for clean shutdown.
/// Decrements the reference count and only performs actual shutdown when count reaches 0.
pub async fn shutdown_mem_homedb() {
    #[cfg(test)]
    {
        use std::sync::atomic::Ordering;
        
        // Acquire write lock first to serialize with init
        let mut guard = MEM_HOMEDB.write();
        
        let prev = HOMEDB_INIT_COUNT.fetch_sub(1, Ordering::SeqCst);
        
        if prev == 0 {
            // Undo the decrement
            HOMEDB_INIT_COUNT.fetch_add(1, Ordering::SeqCst);
            eprintln!("Warning: shutdown_mem_homedb() called more times than init");
            return;
        }
        
        if prev == 1 {
            // Last shutdown - destroy MemoryDB
            if let Some(_db) = guard.take() {
                drop(guard); // Release lock before async operation
            }
        }
        // else: count > 1, just decremented, keep MemoryDB alive
        
        // Always shutdown IOManager to match init (which always calls init_iomgr)
        let _ = iomgr::shutdown_iomgr().await;
    }
    
    #[cfg(not(test))]
    unsafe {
        if MEM_HOMEDB.is_some() {
            let _ = iomgr::shutdown_iomgr().await;
            MEM_HOMEDB = None;
        }
    }
}
