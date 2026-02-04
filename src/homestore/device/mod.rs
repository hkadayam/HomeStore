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

pub mod device_metadata;
pub mod chunk;
pub mod chunk_pool;
pub mod chunk_selector;
pub mod physical_dev;
pub mod virtual_dev;
pub mod device_manager;

#[cfg(test)]
mod tests;

pub use device_metadata::*;
pub use chunk::*;
pub use chunk_pool::*;
pub use chunk_selector::*;
pub use physical_dev::*;
pub use virtual_dev::*;
pub use device_manager::*;
