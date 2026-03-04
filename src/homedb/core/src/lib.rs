//! homedb_core — foundational types and index backends for HomeDB.

pub mod common;
pub mod index;
pub mod mvcc;
pub mod nontxn;

// Flat re-exports so callers can use `homedb_core::BtreeIndex` etc.
pub use common::{DbKey, DbValue, HomeDbError, KeySpec, KeyType, PrefixType, Result, TableSpec, ValueSpec};
pub use index::{BtreeIndex, IndexOps, IndexQueryHandle, RangeIterator, ShardedBtree, UnshardedBtree};
pub use mvcc::{GcEvent, GcQueue, MvccDeferredGcFilter, MvccGc, MvccInlineGcFilter, MvccKey, MvccOps, MvccQueryFilter, MvccValue, Snapshot, SnapshotOps, SnapshotRegistry, GLOBAL_SEQ};
pub use nontxn::NonTxnOps;
