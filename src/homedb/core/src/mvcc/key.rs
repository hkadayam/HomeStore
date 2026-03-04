//! MVCC key and value wrappers for key-level snapshot isolation.
//!
//! # Key encoding
//!
//! Each user key is stored as `MvccKey<K> = (inner_key, !seq_id)`.
//! Storing the bitwise-NOT of the seq_id means:
//!   - higher seq_id → smaller `inv_seq` → sorts EARLIER (ascending btree order)
//!   - lower  seq_id → larger  `inv_seq` → sorts LATER
//!
//! Within the same user key, versions are ordered newest-first. Seeking to
//! `MvccKey { inner, inv_seq: !snapshot_ts }` finds the latest version with
//! `seq_id ≤ snapshot_ts` in one forward step.
//!
//! # Serialization layout
//!
//! ```text
//! [ inner key bytes (inner.serialized_size()) | inv_seq: u64 LE (8 bytes) ]
//! ```
//!
//! For variable-size inner keys the inv_seq is always the last 8 bytes, so
//! `deserialize_from` uses `buf.len() - 8` as the inner key boundary.

use homestore::index::btree::btree_kvs::{BtreeKey, BtreeValue, Partitionable};
use homestore::index::btree::detail::btree_req::BtreeKeyRange;
use crate::common::db_kv::{DbKey, DbValue};
use crate::common::key_value_spec::{KeySpec, ValueSpec};

// ============================================================================
// MvccKey
// ============================================================================

/// Composite btree key: `(inner_key, !seq_id)`.
///
/// Compares by inner key ascending, then by `inv_seq` ascending (= seq_id descending).
pub struct MvccKey<K> {
    pub inner: K,
    /// `!seq_id` — inverted so that the newest version sorts first.
    pub inv_seq: u64,
}

impl<K> MvccKey<K> {
    /// Create a live key for the given user key and commit sequence number.
    pub fn new(inner: K, seq_id: u64) -> Self {
        Self { inner, inv_seq: !seq_id }
    }

    /// The sequence id (commit timestamp) recovered from the stored inverted value.
    pub fn seq_id(&self) -> u64 { !self.inv_seq }
}

// --- Clone, PartialEq, Eq, PartialOrd, Ord, Debug ---------------------------

impl<K: Clone> Clone for MvccKey<K> {
    fn clone(&self) -> Self {
        Self { inner: self.inner.clone(), inv_seq: self.inv_seq }
    }
}

impl<K: PartialEq> PartialEq for MvccKey<K> {
    fn eq(&self, other: &Self) -> bool {
        self.inner == other.inner && self.inv_seq == other.inv_seq
    }
}

impl<K: Eq> Eq for MvccKey<K> {}

impl<K: Ord> PartialOrd for MvccKey<K> {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl<K: Ord> Ord for MvccKey<K> {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        // Inner key ascending, then inv_seq ascending (= seq_id descending).
        self.inner.cmp(&other.inner)
            .then_with(|| self.inv_seq.cmp(&other.inv_seq))
    }
}

impl<K: std::fmt::Debug> std::fmt::Debug for MvccKey<K> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "MvccKey({:?}, seq={})", self.inner, self.seq_id())
    }
}

// --- BtreeKey impl ----------------------------------------------------------

impl<K: BtreeKey> BtreeKey for MvccKey<K> {
    /// Always None: even if K is fixed-size, we keep this None to avoid const
    /// generic complexity; `fixed_serialized_size()` is used for runtime checks.
    const FIXED_SERIALIZED_SIZE: Option<u32> = None;

    fn serialized_size(&self) -> u32 {
        self.inner.serialized_size() + 8
    }

    fn fixed_serialized_size(&self) -> Option<u32> {
        self.inner.fixed_serialized_size().map(|n| n + 8)
    }

    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> std::io::Result<u32> {
        let inner_n = self.inner.serialize_to(buf, copy)? as usize;
        buf[inner_n..inner_n + 8].copy_from_slice(&self.inv_seq.to_le_bytes());
        Ok(inner_n as u32 + 8)
    }

    fn deserialize_from(buf: &[u8], copy: bool) -> std::io::Result<Self> {
        if buf.len() < 8 {
            // Buffer holds only partition-prefix bytes (no room for inv_seq).
            // ShardedBtree::clamp_range calls first_key() with just the partition
            // prefix to construct range-boundary keys.  Return the minimum key
            // for that prefix: inv_seq = 0 (= seq_id MAX = newest possible).
            let inner = K::deserialize_from(buf, copy)?;
            return Ok(Self { inner, inv_seq: 0 });
        }
        let inner_size = match K::FIXED_SERIALIZED_SIZE {
            Some(n) => n as usize,
            None => buf.len() - 8,
        };
        if inner_size + 8 > buf.len() {
            return Err(std::io::Error::new(
                std::io::ErrorKind::UnexpectedEof,
                "MvccKey inner key exceeds buffer",
            ));
        }
        let inner = K::deserialize_from(&buf[..inner_size], copy)?;
        let inv_seq = u64::from_le_bytes(
            buf[inner_size..inner_size + 8].try_into().unwrap(),
        );
        Ok(Self { inner, inv_seq })
    }

    fn get_max_size() -> u32 {
        K::get_max_size() + 8
    }
}

impl MvccKey<DbKey> {
    /// Placeholder insert key — `inv_seq = 0` so `mutate_key` can stamp the real
    /// `commit_ts` (via `GLOBAL_SEQ.fetch_add`) inside the btree write lock.
    pub fn placeholder(key_bytes: Vec<u8>, key_spec: &KeySpec) -> Self {
        Self { inner: DbKey::new(key_bytes, key_spec), inv_seq: 0 }
    }

    /// Seek key for snapshot-isolated reads: finds the latest version with
    /// `seq_id ≤ ts` in one forward btree step.
    pub fn newest_since(key_bytes: Vec<u8>, key_spec: &KeySpec, ts: u64) -> Self {
        MvccKey::new(DbKey::new(key_bytes, key_spec), ts)
    }

    /// `BtreeKeyRange` covering all versions of the given user key bytes.
    pub fn all_versions_for(key_bytes: Vec<u8>, key_spec: &KeySpec) -> BtreeKeyRange<Self> {
        let key = DbKey::new(key_bytes, key_spec);
        BtreeKeyRange::new(
            MvccKey::new(key.clone(), u64::MAX), // seq_id=MAX → inv_seq=0 (newest)
            true,
            MvccKey::new(key, 0),         // seq_id=0   → inv_seq=MAX (oldest)
            true,
        )
    }

    /// Range-end bound at seq_id=0 (oldest possible version of this key).
    /// seq_id=0 → inv_seq = !0 = u64::MAX.
    pub fn oldest(key_bytes: Vec<u8>, key_spec: &KeySpec) -> Self {
        Self::new(DbKey::new(key_bytes, key_spec), 0)
    }
}

// --- Partitionable for MvccKey<K: Partitionable> ----------------------------

impl<K: Partitionable> Partitionable for MvccKey<K> {
    /// Routes by the INNER key's partition bytes — the seq_id suffix is ignored
    /// for shard routing so all versions of a key land in the same shard.
    fn with_partition_bytes<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        self.inner.with_partition_bytes(f)
    }
}

// ============================================================================
// MvccValue
// ============================================================================

/// Value wrapper that adds a tombstone flag for MVCC deletes.
///
/// # Serialization layout
///
/// ```text
/// [ flag: u8 (0 = live, 1 = tombstone) | value bytes (if live) ]
/// ```
pub struct MvccValue<V> {
    is_tombstone: bool,
    inner: Option<V>,
}

impl<V> MvccValue<V> {
    /// Wrap a live value.
    pub fn new(value: V) -> Self {
        Self { is_tombstone: false, inner: Some(value) }
    }

    /// Create a tombstone (marks the key as deleted at this seq_id).
    pub fn tombstone() -> Self {
        Self { is_tombstone: true, inner: None }
    }

    pub fn is_tombstone(&self) -> bool { self.is_tombstone }

    pub fn inner(&self) -> Option<&V> { self.inner.as_ref() }

    pub fn into_inner(self) -> Option<V> { self.inner }
}

impl<V: Clone> Clone for MvccValue<V> {
    fn clone(&self) -> Self {
        Self { is_tombstone: self.is_tombstone, inner: self.inner.clone() }
    }
}

impl<V: PartialEq> PartialEq for MvccValue<V> {
    fn eq(&self, other: &Self) -> bool {
        self.is_tombstone == other.is_tombstone && self.inner == other.inner
    }
}

impl<V: Eq> Eq for MvccValue<V> {}

impl<V: std::fmt::Debug> std::fmt::Debug for MvccValue<V> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_tombstone {
            write!(f, "MvccValue(tombstone)")
        } else {
            write!(f, "MvccValue({:?})", self.inner)
        }
    }
}

// --- BtreeValue impl --------------------------------------------------------

impl<V: BtreeValue> BtreeValue for MvccValue<V> {
    /// Always None: tombstones have 1 byte; live values have 1 + V.serialized_size().
    const FIXED_SERIALIZED_SIZE: Option<u32> = None;

    fn serialized_size(&self) -> u32 {
        1 + self.inner.as_ref().map_or(0, |v| v.serialized_size())
    }

    fn fixed_serialized_size(&self) -> Option<u32> { None }

    fn serialize_to(&self, buf: &mut [u8], copy: bool) -> std::io::Result<u32> {
        buf[0] = if self.is_tombstone { 1 } else { 0 };
        if let Some(ref inner) = self.inner {
            let n = inner.serialize_to(&mut buf[1..], copy)?;
            Ok(1 + n)
        } else {
            Ok(1)
        }
    }

    fn deserialize_from(buf: &[u8], copy: bool) -> std::io::Result<Self> {
        if buf.is_empty() {
            return Err(std::io::Error::new(
                std::io::ErrorKind::UnexpectedEof,
                "MvccValue empty buffer",
            ));
        }
        let is_tombstone = buf[0] != 0;
        if is_tombstone {
            Ok(Self { is_tombstone: true, inner: None })
        } else {
            let inner = V::deserialize_from(&buf[1..], copy)?;
            Ok(Self { is_tombstone: false, inner: Some(inner) })
        }
    }
}

// ============================================================================
// Concrete factory methods for MvccKey<DbKey> and MvccValue<DbValue>
//
// These hide the DbKey / DbValue construction from callers so only (key_bytes,
// key_spec) or (value_bytes, value_spec) need to be passed in.
// ============================================================================

impl MvccValue<DbValue> {
    /// Wrap a live user value — convenience constructor that hides `DbValue::new`.
    pub fn live(value_bytes: Vec<u8>, value_spec: &ValueSpec) -> Self {
        Self::new(DbValue::new(value_bytes, value_spec))
    }
}
