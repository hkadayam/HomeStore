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

/// Sequence number type for log entries
pub type LogStoreSeqNum = u64;

/// LogStore identifier
pub type LogStoreId = u64;

/// LogStore error types
#[derive(Debug, Clone, thiserror::Error)]
pub enum LogStoreError {
    #[error("Seq {0} has been truncated")]
    Truncated(LogStoreSeqNum),

    #[error("Seq {0} not found")]
    NotFound(LogStoreSeqNum),

    #[error("Invalid seq {0}")]
    InvalidSeq(LogStoreSeqNum),

    #[error("Out of bounds: seq {0}")]
    OutOfBounds(LogStoreSeqNum),
}

/// Result type for LogStore operations
pub type Result<T> = std::result::Result<T, LogStoreError>;
