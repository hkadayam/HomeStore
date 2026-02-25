//! # MemDB - In-Memory Key-Value Store
//!
//! A high-performance in-memory database built on Homestore's Btree implementation.
//! Supports multiple tables with multiple indices per table.
//!
//! ## Architecture
//!
//! ```text
//! Process
//!   └─ MemoryDB::new()
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
//! - Multiple tables, each with multiple indices
//! - Flexible key/value schemas (fixed/variable size, prefix compression)
//! - Iterator-based range queries
//! - Thread-safe table and index management
//! - Handle-based API (no string lookups in critical path)
//!
//! ## Feature flags
//! Specify exactly one top-level mode:
//! - `sync_code`           — sync API + ConcurrentBtree backend
//! - `async_code`          — async API + LockFreeBtree backend
//! - `sync_over_async_code` — sync API + LockFreeBtree backend

mod memory_db;
mod table;
mod table_index;

pub use homedb_common::{
    HomeDbError, KeySpec, KeyType, PrefixType, RangeIterator, Result, TableSpec, ValueSpec,
};
pub use mem_db_macros::reactor_method;
pub use table_index::{IndexType, TableIndex};
pub use table::Table;
pub use memory_db::MemoryDB;
