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
// Stream-based architecture (transitional)
pub mod stream_base;
pub mod blk_stream;
pub mod fixed_blk_stream;
pub mod append_only_stream;
pub mod blob_device_mgr;
pub mod new_blob_dev;  // New BlobDevice design

// Legacy modules (will be refactored)
pub mod blob_dev;  // Old BlobDevice (with BlobStream integration)
pub mod blob_mgr;
pub mod blob_stream;
pub mod blk_read_tracker;

// Re-exports for stream-based architecture (transitional)
pub use stream_base::{StreamBase, StreamId, StreamChunk};
pub use blk_stream::BlkStream;
pub use fixed_blk_stream::{FixedBlkStream, FixedBlkStreamConfig};
pub use append_only_stream::AppendOnlyStream;
pub use blob_device_mgr::{BlobDeviceManager, StreamInfo};
pub use new_blob_dev::{BlobDevice as BlobDeviceNew, StreamType, StreamMetadata};

// Re-exports for legacy architecture
pub use blk_read_tracker::*;
pub use blob_dev::BlobDevice as BlobDeviceOld;
pub use blob_mgr::BlobManager;
pub use blob_stream::{
    BlobStream, BlobStreamId, BlobStreamMetadata, BlobStreamConfig, 
    ChunkSegmentId, WriteUnit, WriteUnitStatus, SegmentId
};

