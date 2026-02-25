//! Shared types and index backends for homedb.

mod btree_index;
mod db_kv;
mod error;
mod iterator;
mod key_value_spec;

cfg_if::cfg_if! {
    if #[cfg(feature = "sync_backend")] {
        mod concurrent_btree;
        pub use concurrent_btree::ConcurrentBtree;
    } else if #[cfg(feature = "async_backend")] {
        mod lockfree_btree;
        pub use lockfree_btree::LockFreeBtree;
    }
}

pub use btree_index::{BtreeIndex, IndexQueryHandle};
pub use db_kv::{DbKey, DbValue};
pub use error::{HomeDbError, Result};
pub use iterator::RangeIterator;
pub use key_value_spec::{KeySpec, KeyType, PrefixType, TableSpec, ValueSpec};
