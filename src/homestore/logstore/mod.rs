/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

pub mod types;
pub mod mem_log_store;

#[cfg(test)]
mod tests;

pub use types::{LogStoreError, LogStoreId, LogStoreSeqNum, Result};
pub use mem_log_store::MemLogStore;

use bytes::Bytes;

#[cfg(feature = "async_code")]
use async_trait::async_trait;

/// Base trait for LogStore implementations
///
/// Provides an append-only log abstraction with sequence numbers,
/// truncation, rollback, and iteration support.
///
/// Note: Methods use `maybe_async_cfg` internally - they compile as
/// sync (feature="sync_code") or async (feature="async_code")
#[cfg_attr(feature = "async_code", async_trait)]
#[maybe_async_cfg::maybe(
    keep_self,
    sync(feature = "sync_code"),
    async(feature = "async_code")
)]
pub trait LogStore: Send + Sync {
    /// Returns true if this LogStore is in append-only mode
    fn is_append_mode(&self) -> bool;

    /// Append a log entry and return the allocated sequence number
    ///
    /// # Arguments
    /// * `data` - Log entry data
    ///
    /// # Returns
    /// * `Result<LogStoreSeqNum>` - Allocated sequence number for this entry
    async fn append(&self, data: Bytes) -> Result<LogStoreSeqNum>;

    /// Read a log entry at the specified sequence number
    ///
    /// # Arguments
    /// * `seq_num` - Sequence number to read
    ///
    /// # Returns
    /// * `Result<Bytes>` - Log entry data
    ///
    /// # Errors
    /// * `LogStoreError::Truncated` - If seq_num has been truncated
    /// * `LogStoreError::NotFound` - If seq_num was never written
    async fn read(&self, seq_num: LogStoreSeqNum) -> Result<Bytes>;

    /// Truncate log entries up to and including upto_seq
    ///
    /// # Arguments
    /// * `upto_seq` - Sequence number to truncate up to (inclusive)
    ///
    /// # Returns
    /// * `Result<()>` - Success or error
    async fn truncate(&self, upto_seq: LogStoreSeqNum) -> Result<()>;

    /// Rollback to the specified sequence number
    ///
    /// Clears all entries after to_seq. The next append will be at to_seq + 1.
    ///
    /// # Arguments
    /// * `to_seq` - Sequence number to rollback to
    ///
    /// # Returns
    /// * `Result<()>` - Success or error
    ///
    /// # Errors
    /// * `LogStoreError::InvalidSeq` - If to_seq is out of valid range
    async fn rollback(&self, to_seq: LogStoreSeqNum) -> Result<()>;

    /// Get the current sequence number bounds
    ///
    /// # Returns
    /// * `(start_seq, next_seq)` - First valid seq and next seq to be allocated
    fn seq_bounds(&self) -> (LogStoreSeqNum, LogStoreSeqNum);

    /// Get the store ID
    fn store_id(&self) -> LogStoreId;
}
