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

use crate::checkpoint::cp_mgr::CpManager;
use crate::device::device_manager::DeviceManager;
use crate::meta::meta_blk_manager::MetaBlkManager;

/// Container for all HomeStore managers
pub struct Managers {
    device_manager: Option<Box<DeviceManager>>,
    meta_manager: Option<Box<MetaBlkManager>>,
    cp_manager: Option<Box<CpManager>>,
}

static mut MANAGERS: Option<Managers> = None;

impl Managers {
    /// Initialize device manager (first in chain)
    /// 
    /// # Safety
    /// Must be called from single-threaded context during initialization
    pub fn init_device_mgr(dev_mgr: DeviceManager) -> &'static Self {
        unsafe {
            assert!(MANAGERS.is_none(), "Managers already initialized. Call Managers::restart() first.");
            MANAGERS = Some(Managers {
                device_manager: Some(Box::new(dev_mgr)),
                meta_manager: None,
                cp_manager: None,
            });
            MANAGERS.as_ref().unwrap()
        }
    }
    
    /// Get a static reference to the Managers singleton
    /// 
    /// # Panics
    /// Panics if Managers has not been initialized yet
    pub fn instance() -> &'static Self {
        unsafe {
            MANAGERS.as_ref().expect("Managers not initialized")
        }
    }
    
    /// Initialize meta block manager (chainable)
    /// 
    /// # Safety
    /// Must be called from single-threaded context during initialization
    pub fn init_metablk_mgr(&self, meta_mgr: MetaBlkManager) -> &'static Self {
        unsafe {
            let mgrs = MANAGERS.as_mut().unwrap();
            mgrs.meta_manager = Some(Box::new(meta_mgr));
            MANAGERS.as_ref().unwrap()
        }
    }
    
    /// Initialize checkpoint manager
    /// 
    /// # Safety
    /// Must be called from single-threaded context during initialization
    /// 
    /// # Returns
    /// Static reference to the initialized CpManager
    pub fn init_cp_mgr(&self, cp_mgr: CpManager) -> &'static CpManager {
        unsafe {
            let mgrs = MANAGERS.as_mut().unwrap();
            mgrs.cp_manager = Some(Box::new(cp_mgr));
            &**MANAGERS.as_ref().unwrap().cp_manager.as_ref().unwrap()
        }
    }
    
    /// Get checkpoint manager reference
    #[inline]
    pub fn cp_mgr() -> &'static CpManager {
        unsafe {
            &**MANAGERS.as_ref().expect("Managers not initialized").cp_manager.as_ref().expect("CpManager not initialized")
        }
    }
    
    /// Get device manager reference
    #[inline]
    pub fn device_mgr() -> &'static DeviceManager {
        unsafe {
            &**MANAGERS.as_ref().expect("Managers not initialized").device_manager.as_ref().expect("DeviceManager not initialized")
        }
    }
    
    /// Get meta block manager reference
    #[inline]
    pub fn metablk_mgr() -> &'static MetaBlkManager {
        unsafe {
            &**MANAGERS.as_ref().expect("Managers not initialized").meta_manager.as_ref().expect("MetaBlkManager not initialized")
        }
    }
    
    /// Restart/reset all managers - for tests only
    /// 
    /// # Safety
    /// Must be called from single-threaded context when no other code is accessing managers
    #[cfg(test)]
    pub async fn restart() {
        unsafe {
            if let Some(mgrs) = MANAGERS.take() {
                // Close DeviceManager if initialized
                if let Some(dev_mgr) = mgrs.device_manager {
                    let _ = dev_mgr.close_devices().await;
                }
                // MetaBlkManager and CpManager don't need explicit cleanup
            }
        }
    }
}

// Top-level convenience functions for cleaner syntax

/// Get checkpoint manager reference
#[inline]
pub fn cp_mgr() -> &'static CpManager {
    Managers::cp_mgr()
}

/// Get device manager reference
#[inline]
pub fn device_mgr() -> &'static DeviceManager {
    Managers::device_mgr()
}

/// Get meta block manager reference
#[inline]
pub fn metablk_mgr() -> &'static MetaBlkManager {
    Managers::metablk_mgr()
}

