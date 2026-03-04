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
//! - `sync_code`           — sync API + UnshardedBtree backend
//! - `async_code`          — async API + ShardedBtree backend
//! - `sync_over_async_code` — sync API + ShardedBtree backend (async execution, sync API)

mod memory_db;
mod table;
mod table_index;

pub use homedb_core::{
    HomeDbError, KeySpec, KeyType, PrefixType, RangeIterator, Result, TableSpec, ValueSpec,
    MvccDeferredGcFilter, MvccGc, MvccInlineGcFilter, MvccKey, MvccQueryFilter, MvccValue,
    Snapshot, SnapshotOps, SnapshotRegistry, GLOBAL_SEQ,
};
pub use mem_db_macros::reactor_method;
pub use table_index::{IndexType, TableIndex};
pub use table::Table;
pub use memory_db::MemoryDB;
