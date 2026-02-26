//! Shared types and index backends for homedb.

mod btree_index;
mod db_kv;
mod error;
mod iterator;
mod key_value_spec;

mod unsharded_btree;
pub use unsharded_btree::UnshardedBtree;

mod sharded_btree;
pub use sharded_btree::ShardedBtree;


pub use btree_index::{BtreeIndex, IndexQueryHandle};
pub use db_kv::{DbKey, DbValue};
pub use error::{HomeDbError, Result};
pub use iterator::RangeIterator;
pub use key_value_spec::{KeySpec, KeyType, PrefixType, TableSpec, ValueSpec};
