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

use std::sync::atomic::{AtomicI64, Ordering};
use std::cell::UnsafeCell;

/// Checkpoint ID type
pub type CpId = i64;

/// Checkpoint status enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum CpStatus {
    Unknown = 0,

    // IO Phase
    IoReady = 1,
    Trigger = 2,

    // Flush Phase
    FlushReady = 3,
    Flushing = 4,
    FlushDone = 5,

    // Cleanup Phase
    Cleaning = 6,
    Completed = 7,
}

impl From<u8> for CpStatus {
    fn from(v: u8) -> Self {
        match v {
            1 => CpStatus::IoReady,
            2 => CpStatus::Trigger,
            3 => CpStatus::FlushReady,
            4 => CpStatus::Flushing,
            5 => CpStatus::FlushDone,
            6 => CpStatus::Cleaning,
            7 => CpStatus::Completed,
            _ => CpStatus::Unknown,
        }
    }
}

/// Checkpoint consumer enumeration
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum CpConsumer {
    HsClient = 0,
    IndexSvc = 1,
    BlkDataSvc = 2,
    ReplicationSvc = 3,
    Sentinel = 4,
}

impl CpConsumer {
    pub fn count() -> usize {
        Self::Sentinel as usize
    }
    
    pub fn index(self) -> usize {
        self as usize
    }
}

/// Checkpoint context - base class for consumer-specific contexts
pub trait CpContext: Send + Sync {
    /// Get the checkpoint ID
    fn id(&self) -> CpId;
    
    /// Complete the checkpoint flush
    fn complete(&self, status: bool);
}

/// Checkpoint structure
pub struct Cp {
    /// Checkpoint status (for debugging/logging only - not used in logic)
    /// SAFETY: Only written from CP's owning thread, may read stale values from other threads
    status: UnsafeCell<u8>,
    
    /// Enter count - number of threads in critical section
    enter_cnt: AtomicI64,
    
    /// Checkpoint ID
    cp_id: CpId,
    
    /// Reference to CP Manager for triggering flush
    cp_mgr: std::sync::Arc<crate::checkpoint::cp_mgr::CpManager>,
}

impl Cp {
    /// Create a new checkpoint
    pub fn new(cp_id: CpId, cp_mgr: std::sync::Arc<crate::checkpoint::cp_mgr::CpManager>) -> Self {
        Self {
            status: UnsafeCell::new(CpStatus::Unknown as u8),
            enter_cnt: AtomicI64::new(1),  // Start at 1 (CpManager's implicit guard)
            cp_id,
            cp_mgr,
        }
    }
    
    /// Get checkpoint ID
    pub fn id(&self) -> CpId {
        self.cp_id
    }
    
    /// Get current status (for debugging/logging only)
    /// May return stale values when read from non-owning thread
    pub fn status(&self) -> CpStatus {
        // SAFETY: Used only for logging - stale reads are acceptable
        unsafe { CpStatus::from(*self.status.get()) }
    }
    
    /// Set status (should only be called from CP's owning thread)
    pub fn set_status(&self, status: CpStatus) {
        // SAFETY: Called only from CP's owning thread in Glommio thread-per-core model
        unsafe { *self.status.get() = status as u8; }
    }
    
    /// Increment enter count
    pub fn increment_enter_cnt(&self) -> i64 {
        self.enter_cnt.fetch_add(1, Ordering::AcqRel) + 1
    }
    
    /// Decrement enter count and test if zero
    pub fn decrement_enter_cnt_testz(&self) -> bool {
        self.enter_cnt.fetch_sub(1, Ordering::AcqRel) == 1
    }
    
    /// Get enter count
    pub fn enter_cnt(&self) -> i64 {
        self.enter_cnt.load(Ordering::Acquire)
    }
    
    /// Convert to string for debugging
    pub fn to_string(&self) -> String {
        format!(
            "CP={}: status={:?}, enter_count={}",
            self.cp_id,
            self.status(),
            self.enter_cnt()
        )
    }
}

// Cp is no longer Clone - use Arc<Cp> for shared ownership

impl std::fmt::Debug for Cp {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Cp")
            .field("cp_id", &self.cp_id)
            .field("status", &self.status())
            .field("enter_cnt", &self.enter_cnt())
            .finish()
    }
}

unsafe impl Send for Cp {}
unsafe impl Sync for Cp {}

#[cfg(test)]
mod tests {
    use super::*;
    
    // Note: Cp tests require CpManager, which is tested in cp_mgr.rs
}
