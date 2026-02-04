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

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::cell::UnsafeCell;
use futures::channel::oneshot;
use futures::future::{Shared, FutureExt};
use parking_lot::RwLock;
use async_trait::async_trait;

use super::cp::*;
use sisl::RcuPtr;
use crate::meta::ModuleMetaBlk;
use crate::common::managers;
// TODO: Re-enable when utils module is implemented
// use crate::common::utils::{now, get_elapsed_duration};
use iomgr;

/// CP Manager superblock structure
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct CpMgrSuperBlock {
    pub magic: u64,
    pub version: u32,
    pub last_flushed_cp: CpId,
}

impl CpMgrSuperBlock {
    pub const MAGIC: u64 = 0xc0c0c01a;
    pub const VERSION: u32 = 0x1;
    
    pub fn new() -> Self {
        Self {
            magic: Self::MAGIC,
            version: Self::VERSION,
            last_flushed_cp: -1,
        }
    }
}

impl Default for CpMgrSuperBlock {
    fn default() -> Self {
        Self::new()
    }
}

/// Checkpoint trigger reason
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CpTriggerReason {
    Unknown,
    Timer,
    IndexBufferFull,
    IndexFreeBlksExceeded,
    LogStoreFull,
    DataFreeBlksExceeded,
    UserDriven,
    Shutdown
}

/// CP callbacks trait - implemented by checkpoint consumers
#[async_trait]
pub trait CpCallbacks: Send + Sync {
    /// Called when a new CP session is about to be started and CPManager is asking its consumers 
    /// to switch over to new CP
    fn on_switchover_cp(&self, cur_cp: Option<&Cp>, new_cp: &Cp);
    
    /// Called to flush any dirty buffers accumulated on the given CP session
    async fn cp_flush(&self, cp: &Cp) -> Result<(), String>;
    
    /// Called to cleanup after all the CP consumers been flushed the CP session
    async fn cp_cleanup(&self, cp: &Cp);
    
    /// Get flush progress percentage of a given CP flush
    async fn cp_progress_percent(&self) -> u32;
    
    /// Attempt to repair slow CP. Consumers are expected to say increase their batch size etc to 
    /// try increase the CP flush speed if possible.
    async fn repair_slow_cp(&mut self) {}
}

/// Checkpoint Manager
pub struct CpManager {
    /// Current checkpoint (RCU-protected for lock-free reads)
    cur_cp: RcuPtr<Arc<Cp>>,
        
    /// Superblock data (using ModuleMetaBlk for automatic persistence)
    /// Wrapped in UnsafeCell for interior mutability since cp_flush takes &self
    /// Only accessed in cp_flush() which runs single-threaded via spawn_local()
    superblock: UnsafeCell<ModuleMetaBlk<CpMgrSuperBlock>>,
    
    /// Registered CP consumers
    consumers: RwLock<HashMap<CpConsumer, Arc<dyn CpCallbacks>>>,
    
    /// Trigger mutex state
    trigger_mtx: std::sync::Mutex<TriggerState>,
    
    /// CP timer interval in milliseconds
    cp_timer_interval_ms: u64,
    
    /// Last flush completion time in nanoseconds since epoch (for adaptive timer)
    last_flush_time_ns: AtomicU64,
    
    /// Timer shutdown signal
    timer_shutdown_tx: std::sync::Mutex<Option<futures::channel::oneshot::Sender<()>>>,
}

// SAFETY: CpManager is Sync because:
// - cur_cp: RcuPtr provides safe concurrent access
// - superblock: UnsafeCell is only accessed in cp_flush() which runs single-threaded via spawn_local()
// - consumers: RwLock provides safe concurrent access
// - trigger_mtx: std::sync::Mutex provides safe concurrent access
// - cp_timer_interval_ms: immutable after construction
// - last_flush_time_ns: AtomicU64 provides safe concurrent access
// - timer_shutdown_tx: std::sync::Mutex provides safe concurrent access
unsafe impl Sync for CpManager {}

/// Internal trigger state
struct TriggerState {
    in_flush_phase: bool,
    pending_trigger: bool,
    current_flush: Option<(
        Shared<oneshot::Receiver<Result<(), String>>>,
        oneshot::Sender<Result<(), String>>,
    )>,
    queued_flush: Option<(
        Shared<oneshot::Receiver<Result<(), String>>>,
        oneshot::Sender<Result<(), String>>,    
    )>,
}

impl CpManager {
    /// Create a new CP Manager
    ///
    /// # Arguments
    /// - `cp_timer_interval_ms`: CP timer interval in milliseconds
    ///
    /// # Returns
    /// Static reference to the created CpManager (registered in global Managers singleton)
    pub async fn new(cp_timer_interval_ms: u64) -> std::io::Result<&'static Self> {
        // Create or load superblock using ModuleMetaBlk (gets MetaBlkManager from singleton)
        let superblock = ModuleMetaBlk::<CpMgrSuperBlock>::new("cp_manager", None).await?;
        
        // Create first CP
        let first_cp = Arc::new(Cp::new(superblock.last_flushed_cp + 1));
        first_cp.set_status(CpStatus::IoReady);
        
        // Create shutdown channel for timer
        let (shutdown_tx, shutdown_rx) = futures::channel::oneshot::channel();
        
        // Create CpManager with first CP already set
        let mgr = Self {
            cur_cp: RcuPtr::new(first_cp),
            superblock: UnsafeCell::new(superblock),
            consumers: RwLock::new(HashMap::new()),
            trigger_mtx: std::sync::Mutex::new(TriggerState {
                in_flush_phase: false,
                pending_trigger: false,
                current_flush: None,
                queued_flush: None,
            }),
            cp_timer_interval_ms,
            last_flush_time_ns: AtomicU64::new(now()),
            timer_shutdown_tx: std::sync::Mutex::new(Some(shutdown_tx)),
        };
        
        // Self-register to global Managers singleton
        let mgr_ref = managers::Managers::instance().init_cp_mgr(mgr);
        
        // Start CP timer loop
        mgr_ref.start_cp_timer(shutdown_rx);
        
        Ok(mgr_ref)
    }
    
    /// Register a CP consumer
    ///
    /// # Arguments
    /// - `consumer`: Consumer identifier
    /// - `callbacks`: Callbacks implementation
    pub fn register_consumer(
        &self,
        consumer: CpConsumer,
        callbacks: Arc<dyn CpCallbacks>,
    ) {
        // Notify consumer about current CP
        let guard = self.cur_cp.read();
        callbacks.on_switchover_cp(None, &**guard);  // Arc<Cp> -> Cp
        
        // Store callbacks (write lock for registration)
        self.consumers.write().insert(consumer, callbacks);
    }
    
    /// Start the CP timer loop (called from new())
    ///
    /// Timer triggers CP flush at regular intervals, adaptively adjusting to ensure
    /// minimum interval between flushes regardless of who triggered them.
    fn start_cp_timer(&'static self, shutdown_rx: futures::channel::oneshot::Receiver<()>) {
        let interval_ms = self.cp_timer_interval_ms;
        let interval_duration = std::time::Duration::from_millis(interval_ms);
        
        iomgr::spawn_local(async move {
            let mut shutdown_rx = shutdown_rx.fuse();
            
            loop {
                // Calculate how long to sleep based on last flush time
                let sleep_duration = {
                    let elapsed = get_elapsed_duration(self.last_flush_time_ns.load(Ordering::Relaxed));
                    if elapsed >= interval_duration {
                        std::time::Duration::from_millis(0) // Already past interval - trigger immediately (sleep 0)
                    } else {
                        interval_duration - elapsed // Sleep for remaining time until next interval
                    }
                };
                
                // Sleep for the calculated duration (or check shutdown immediately if 0)
                if sleep_duration > std::time::Duration::from_millis(0) {
                    futures::select! {
                        _ = iomgr::sleep(sleep_duration).fuse() => {}
                        _ = shutdown_rx => {
                            println!("CP timer shutting down");
                            return;
                        }
                    }
                }
                
                // Trigger flush (don't queue if already flushing - timer is periodic)
                if let Some(fut) = managers::cp_mgr().trigger_cp_flush(false, CpTriggerReason::Timer) {
                    let _ = fut.await;
                }
                // After flush completes, last_flush_time is updated in cp_flush() - Next loop iteration will
                // calculate new sleep duration
            }
        });
    }
    
    /// Shutdown the CP timer
    pub fn shutdown(&self) {
        if let Some(tx) = self.timer_shutdown_tx.lock().unwrap().take() {
            let _ = tx.send(());
        }
    }

    /// Get current CP with guard
    /// Returns a CPGuard that increments enter_cnt and triggers flush on drop
    pub fn cp_guard(&self) -> CPGuard {
        CPGuard::new()
    }

    /// Trigger a CP flush
    ///
    /// # Arguments
    /// - `queue_if_flushing`: If true, queue the request if flush is already in progress
    /// - `reason`: Reason for triggering the flush
    ///
    /// # Returns
    /// - `Some(future)`: Future that completes when flush is done (if queued or triggered)
    /// - `None`: If flush is already in progress and queue_if_flushing=false
    pub fn trigger_cp_flush(
        &self,
        queue_if_flushing: bool,
        reason: CpTriggerReason,
    ) -> Option<Shared<oneshot::Receiver<Result<(), String>>>> {
        let (owns_trigger, shared_rx) = {
            let mut trigger_state = self.trigger_mtx.lock().unwrap();
            
            // Check if CP flush is already in progress
            if trigger_state.in_flush_phase {
                if queue_if_flushing {
                    // Queue a NEW flush to run after the current one completes
                    trigger_state.pending_trigger = true;
                    
                    // Create or reuse shared future for the QUEUED flush (not current!)
                    if trigger_state.queued_flush.is_none() {
                        let (tx, rx) = oneshot::channel();
                        let shared_rx = rx.shared();
                        trigger_state.queued_flush = Some((shared_rx, tx));
                    }
                    
                    // Return the queued flush future - callers wait for NEXT flush, not current!
                    let future = trigger_state.queued_flush.as_ref()
                        .map(|(shared_rx, _)| shared_rx.clone());
                    
                    (false, future)
                } else {
                    (false, None)
                }
            } else {
                // Start new CP flush - we own the trigger!
                trigger_state.in_flush_phase = true;
                
                // Create runtime-agnostic shared future
                let (tx, rx) = oneshot::channel();
                let shared_rx = rx.shared();
                trigger_state.current_flush = Some((shared_rx.clone(), tx));
                
                (true, Some(shared_rx))
            }
            // trigger_state lock drops here automatically
        };
        
        if !owns_trigger {
            return shared_rx;
        }
        
        self.cp_switchover(reason);
        shared_rx
    }

    /// Prepare for CP switchover - creates new CP and notifies consumers
    /// The old CP guard is dropped at the end, potentially triggering flush
    fn cp_switchover(&self, reason: CpTriggerReason) {
        // Create guard for old CP (increments enter_cnt)
        let old_cpg = self.cp_guard();
        let old_cp = old_cpg.cp();

        old_cp.set_status(CpStatus::Trigger);
        println!("Time to flush CP {} - reason: {:?}", old_cp.id(), reason);
        
        // Create new CP
        let new_cp_id = old_cp.id() + 1;
        let new_cp = Arc::new(Cp::new(new_cp_id));
        new_cp.set_status(CpStatus::IoReady);
        
        // Switchover consumers to start preparing for this new CP session
        {
            let consumers = self.consumers.read();  // Read lock - not modifying HashMap
            for (_consumer, callbacks) in consumers.iter() {
                callbacks.on_switchover_cp(Some(old_cp), &*new_cp);
            }
        }
        
        old_cp.set_status(CpStatus::FlushReady);
        
        // Manually decrement (guaranteed not to hit 0 - we just created the guard). We incremented the enter count 
        // when the CP was created to make sure that cp flush isn't triggered until cp_switchover by this method is called.
        old_cp.decrement_enter_cnt();
        
        // Switch to new CP
        self.cur_cp.update(new_cp);
        println!("Switched to new CP {} (old CP {} ready for flush)", new_cp_id, old_cp.id());
        
        // old_cpg drops here → CPGuard::drop() checks enter_cnt and triggers flush if zero
    }
    
    /// Flush a checkpoint (called by CPGuard::drop when enter_cnt reaches 0)
    async fn cp_flush(&self, cp: Arc<Cp>) {
        println!("CPManager::cp_flush: Starting flush for CP {}", cp.id());
        cp.set_status(CpStatus::Flushing);
        
        // FLUSH PHASE 1: Flush all consumers
        {
            let consumers = self.consumers.read();
            for (_consumer, callbacks) in consumers.iter() {
                if let Err(e) = callbacks.cp_flush(&*cp).await {
                    eprintln!("CP {} flush error: {}", cp.id(), e);
                }
            }
        }
        cp.set_status(CpStatus::FlushDone);
        
        // FLUSH PHASE 2: Cleanup consumers
        {
            let consumers = self.consumers.read();
            for (_consumer, callbacks) in consumers.iter() {
                callbacks.cp_cleanup(&*cp).await;
            }
        }
        cp.set_status(CpStatus::Completed);
        println!("CPManager::cp_flush: Completed flush for CP {}", cp.id());
        
        // FLUSH PHASE 3: Update superblock
        // SAFETY: cp_flush() is called via spawn_local() guaranteeing single-threaded execution
        unsafe {
            let sb = &mut *self.superblock.get();
            sb.last_flushed_cp = cp.id();
            if let Err(e) = sb.write().await {
                eprintln!("Failed to write CP superblock: {}", e);
            }
        }
        
        // FLUSH PHASE 4: Complete the current flush future
        {
            let mut trigger_state = self.trigger_mtx.lock().unwrap();
            if let Some((_shared_rx, tx)) = trigger_state.current_flush.take() {
                let _ = tx.send(Ok(()));
            }
            trigger_state.in_flush_phase = false;
        }
        
        // Update last flush time for adaptive timer
        self.last_flush_time_ns.store(now(), Ordering::Relaxed);
        
        // FLUSH PHASE 5: Check if there's a QUEUED flush that needs to run, if so trigger the cp switchover for it
        let back_2_back_cp = {
            let mut trigger_state = self.trigger_mtx.lock().unwrap();
            if trigger_state.pending_trigger {
                trigger_state.pending_trigger = false;
                trigger_state.in_flush_phase = true;
                
                // Move queued_flush to current_flush
                trigger_state.current_flush = trigger_state.queued_flush.take();
                true
            } else {
                false
            }
        };
                
        if back_2_back_cp {
            println!("Starting QUEUED CP flush");
            self.cp_switchover(CpTriggerReason::Unknown);
        }
    }
}

/// RAII guard for checkpoint access
/// Increments enter_cnt on creation, decrements and potentially triggers flush on drop
pub struct CPGuard {
    rcu_guard: sisl::rcu_ptr::RcuGuard<'static, Arc<Cp>>,
}

impl CPGuard {
    fn new() -> Self {
        // Access via global singleton - no borrowing of CpManager!
        // Since managers::cp_mgr() returns &'static CpManager, the guard is already 'static
        let rcu_guard = managers::cp_mgr().cur_cp.read();
        
        // Increment enter_cnt on the CP
        rcu_guard.increment_enter_cnt();
        
        Self { rcu_guard }
    }
    
    /// Get reference to the CP
    pub fn cp(&self) -> &Cp {
        &**self.rcu_guard
    }
}

impl Drop for CPGuard {
    fn drop(&mut self) {
        // Decrement enter count and check if we should trigger flush
        let should_flush = self.rcu_guard.decrement_enter_cnt_testz();
        
        if should_flush {
            println!("CPGuard: CP {} ready for flush (enter_cnt reached 0)", self.rcu_guard.id());
            
            // Spawn async flush task in background using global manager
            let cp_clone: Arc<Cp> = Arc::clone(&*self.rcu_guard);
            
            // Use spawn_local since this doesn't need to be Send (stays on same reactor). We can't directly 
            // call cp_flush() since it's async and needs an await which cannot be done on a Drop.
            iomgr::spawn_local(async move {
                managers::cp_mgr().cp_flush(cp_clone).await;
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[iomgr::iomanager_test]
    async fn test_cp_manager_creation() {
        use crate::device::{DeviceManager, DevInfo, HSDevType};
        use crate::meta::MetaBlkManager;
        use crate::common::managers::{Managers, cp_mgr};
        
        // Create temp directory for test
        let temp_dir = tempfile::tempdir().unwrap();
        let dev_path = temp_dir.path().join("test_cp_mgr.dev");
        
        // Create a test device file (50MB - need enough for meta vdev)
        let file = std::fs::File::create(&dev_path).unwrap();
        file.set_len(50 * 1024 * 1024).unwrap();
        
        // Create DeviceManager (self-registers to singleton, returns &'static ref)
        let dev_info = DevInfo {
            dev_name: dev_path.to_str().unwrap().to_string(),
            dev_size: 50 * 1024 * 1024,
            dev_type: HSDevType::Fast,
        };
        let dev_mgr = DeviceManager::new(vec![dev_info]).unwrap();
        dev_mgr.format_devices().await.unwrap();
        
        // Create MetaBlkManager (self-registers to singleton, returns &'static ref)
        let _meta_mgr = MetaBlkManager::create(40 * 1024 * 1024).await.unwrap();
        
        // Create CpManager (self-registers to singleton, returns &'static ref)
        let cp_mgr_ref = CpManager::new(1000).await.unwrap();
        
        // Verify initial CP using returned reference
        {
            let guard = cp_mgr_ref.cp_guard();
            assert_eq!(guard.cp().id(), 0);
            assert_eq!(guard.cp().status(), CpStatus::IoReady);
        }
        
        // Also verify we can access via global function
        {
            let guard2 = cp_mgr().cp_guard();
            assert_eq!(guard2.cp().id(), 0);
        }
        
        // Cleanup
        Managers::restart().await;
    }
}
