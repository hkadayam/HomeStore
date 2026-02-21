//! # HomeDB - Unified Database API
//!
//! Provides three database types via compile-time feature flags:
//! - `MemoryDB` (features = ["inmem", "sync"]) - Synchronous in-memory database
//! - `AsyncMemoryDB` (features = ["inmem"]) - Async in-memory database  
//! - `Database` (features = ["persistent"]) - Async persistent database
//!
//! ## Architecture
//!
//! ```text
//! HomeDB (this crate)
//!   ├─ MemoryDB (sync + inmem)
//!   ├─ AsyncMemoryDB (async + inmem)
//!   └─ Database (async + persistent)
//!      ↓
//! Homestore (storage engine)
//!   ├─ Btree (always async internally)
//!   └─ Device Layer (persistent mode only)
//! ```
//!
//! ## Sync Mode Implementation
//!
//! For `MemoryDB` (sync mode), we use a dedicated single-threaded Tokio runtime
//! embedded in the DB instance to bridge to the async homestore layer. This avoids
//! per-call runtime overhead while maintaining clean separation.

mod error;
mod key_value_spec;
mod table;
mod table_index;
mod iterator;

pub(crate) use mem_db_macros::reactor_method;

pub use error::{HomeDbError, Result};
pub use key_value_spec::{KeySpec, ValueSpec, TableSpec};
pub use table::Table;
pub use table_index::{TableIndex, IndexType};

use std::sync::Arc;
use dashmap::DashMap;

//=================================================================================
// Conditional Compilation Based on Features
//=================================================================================

// Sync In-Memory DB (sync + inmem features)
#[cfg(all(feature = "inmem", feature = "sync"))]
mod memory_db_sync;
#[cfg(all(feature = "inmem", feature = "sync"))]
pub use memory_db_sync::MemoryDB;

// Async In-Memory DB (inmem without sync)
#[cfg(all(feature = "inmem", not(feature = "sync")))]
mod memory_db_async;
#[cfg(all(feature = "inmem", not(feature = "sync")))]
pub use memory_db_async::AsyncMemoryDB;

// Async Persistent DB (persistent feature)
#[cfg(all(feature = "persistent", not(feature = "sync")))]
mod database;
#[cfg(all(feature = "persistent", not(feature = "sync")))]
pub use database::Database;

//=================================================================================
// Shared Types and Utilities
//=================================================================================

/// Internal table storage (shared across all DB types)
pub(crate) type TableMap = DashMap<String, Arc<Table>>;
