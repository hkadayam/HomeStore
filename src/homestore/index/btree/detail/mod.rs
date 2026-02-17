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

//! Btree Detail Module
//!
//! This module contains detailed btree operations split into separate files
//! (mutate, query) similar to the C++ implementation structure.

// btree_req contains only types/requests - always available
pub mod btree_req;
pub use btree_req::*;

// Implementation modules - only when async-locks is enabled
#[cfg(feature = "async-locks")]
pub mod mutate;
#[cfg(feature = "async-locks")]
pub mod query;
#[cfg(feature = "async-locks")]
pub mod remove;

#[cfg(feature = "async-locks")]
pub use mutate::*;