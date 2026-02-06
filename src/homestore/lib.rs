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
// Conditional compilation based on features
#[cfg(feature = "device")]
pub mod device;
#[cfg(feature = "checkpoint")]
pub mod checkpoint;
#[cfg(feature = "blkalloc")]
pub mod blkalloc;
#[cfg(any(feature = "device", feature = "btree-only"))]
pub mod common;
#[cfg(any(feature = "device", feature = "cow_btree"))]
pub mod meta;
#[cfg(feature = "blob")]
pub mod blob;
#[cfg(feature = "streams")]
pub mod streams;  // Stream-based Virtual Device implementations
pub mod index;    // B-tree index implementations (always included)

#[cfg(feature = "device")]
pub use device::*;
#[cfg(feature = "checkpoint")]
pub use checkpoint::*;
#[cfg(feature = "blkalloc")]
pub use blkalloc::*;
#[cfg(any(feature = "device", feature = "btree-only"))]
pub use common::*;
#[cfg(any(feature = "device", feature = "cow_btree"))]
pub use meta::*;
#[cfg(feature = "blob")]
pub use blob::*;
#[cfg(feature = "streams")]
pub use streams::*;
pub use index::*;

//================================================================================
// Tracing Initialization
//================================================================================

/// Initialize tracing/logging for Homestore
///
/// This sets up the tracing subscriber with sensible defaults:
/// - Timestamps on all log entries
/// - Module paths for filtering
/// - Environment-based filtering via RUST_LOG
///
/// # Example
/// ```rust,ignore
/// // In your main() or test setup
/// homestore::init_tracing();
///
/// // Then control via environment:
/// // RUST_LOG=homestore::index::btree=debug cargo run
/// // RUST_LOG=homestore::index::btree::detail::remove=trace cargo test
/// ```
pub fn init_tracing() {
    use tracing_subscriber::EnvFilter;
    
    let _ = tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::from_default_env()
                .add_directive("homestore=info".parse().unwrap())
        )
        .with_target(true)
        .with_thread_ids(false)
        .try_init();
}
