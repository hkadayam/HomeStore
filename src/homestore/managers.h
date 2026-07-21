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
#pragma once

#include <common/defs.h>
#include <stdexcept>

namespace homestore {

class MetaBlkManager;
class DeviceManager;
class CPManager;
class BlobDevManager;
class COWBtreeManager;
class ResourceMgr;
class LogStoreManager;
class ReplicationManager;

// ─────────────────────────────────────────────────────────────────────────────
// Managers
//
// Holds exactly one instance of each subsystem singleton.  Each subsystem's
// factory (create / load) calls the matching init_*() method once.  After
// that, callers use the free-function accessors below.
//
// Uses shared<> so that the deleter is type-erased at construction time.  This means reset() and the accessors only
// need forward declarations — a lower level test never has to #include upper level mgr headers
//
// Thread-safety: init_*() are called once at start-up before any concurrent access.  The accessors are read-only.
// ─────────────────────────────────────────────────────────────────────────────
class Managers {
public:
    static void init_meta_mgr(shared< MetaBlkManager > mgr) { s_meta_mgr_ = std::move(mgr); }
    static void init_device_mgr(shared< DeviceManager > mgr) { s_device_mgr_ = std::move(mgr); }
    static void init_cp_mgr(shared< CPManager > mgr) { s_cp_mgr_ = std::move(mgr); }
    static void init_blob_dev_mgr(shared< BlobDevManager > mgr) { s_blob_dev_mgr_ = std::move(mgr); }
    static void init_cow_btree_mgr(shared< COWBtreeManager > mgr) { s_cow_btree_mgr_ = std::move(mgr); }
    static void init_resource_mgr(shared< ResourceMgr > mgr) { s_resource_mgr_ = std::move(mgr); }
    static void init_log_store_mgr(shared< LogStoreManager > mgr) { s_log_store_mgr_ = std::move(mgr); }
    static void init_repl_mgr(shared< ReplicationManager > mgr) { s_repl_mgr_ = std::move(mgr); }

    static void reset_resource_mgr() { s_resource_mgr_.reset(); }

    static void reset() {
        s_meta_mgr_.reset();
        s_device_mgr_.reset();
        s_cp_mgr_.reset();
        s_blob_dev_mgr_.reset();
        s_cow_btree_mgr_.reset();
        s_resource_mgr_.reset();
        s_log_store_mgr_.reset();
        s_repl_mgr_.reset();
    }

private:
    friend MetaBlkManager& meta_mgr();
    friend DeviceManager& device_mgr();
    friend CPManager& cp_mgr();
    friend BlobDevManager& blob_dev_mgr();
    friend COWBtreeManager& cow_btree_mgr();
    friend ResourceMgr& resource_mgr();
    friend LogStoreManager& log_store_mgr();
    friend ReplicationManager& repl_mgr();

    inline static shared< MetaBlkManager > s_meta_mgr_;
    inline static shared< DeviceManager > s_device_mgr_;
    inline static shared< CPManager > s_cp_mgr_;
    inline static shared< BlobDevManager > s_blob_dev_mgr_;
    inline static shared< COWBtreeManager > s_cow_btree_mgr_;
    inline static shared< ResourceMgr > s_resource_mgr_;
    inline static shared< LogStoreManager > s_log_store_mgr_;
    inline static shared< ReplicationManager > s_repl_mgr_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Free-function accessors — call after the matching init_*() has returned.
// ─────────────────────────────────────────────────────────────────────────────
inline MetaBlkManager& meta_mgr() {
    if (!Managers::s_meta_mgr_) {
        throw std::logic_error{"meta_mgr() called before init_meta_mgr()"};
    }
    return *Managers::s_meta_mgr_;
}

inline DeviceManager& device_mgr() {
    if (!Managers::s_device_mgr_) {
        throw std::logic_error{"device_mgr() called before init_device_mgr()"};
    }
    return *Managers::s_device_mgr_;
}

inline CPManager& cp_mgr() {
    if (!Managers::s_cp_mgr_) {
        throw std::logic_error{"cp_mgr() called before init_cp_mgr()"};
    }
    return *Managers::s_cp_mgr_;
}

inline BlobDevManager& blob_dev_mgr() {
    if (!Managers::s_blob_dev_mgr_) {
        throw std::logic_error{"blob_dev_mgr() called before init_blob_dev_mgr()"};
    }
    return *Managers::s_blob_dev_mgr_;
}

inline COWBtreeManager& cow_btree_mgr() {
    if (!Managers::s_cow_btree_mgr_) {
        throw std::logic_error{"cow_btree_mgr() called before init_cow_btree_mgr()"};
    }
    return *Managers::s_cow_btree_mgr_;
}

inline ResourceMgr& resource_mgr() {
    if (!Managers::s_resource_mgr_) {
        throw std::logic_error{"resource_mgr() called before ResourceMgr::start()"};
    }
    return *Managers::s_resource_mgr_;
}

inline LogStoreManager& log_store_mgr() {
    if (!Managers::s_log_store_mgr_) {
        throw std::logic_error{"log_store_mgr() called before init_log_store_mgr()"};
    }
    return *Managers::s_log_store_mgr_;
}

// Replication is optional — a null s_repl_mgr_ means the application booted without a ReplApplication, so no
// replication service was created.  Callers that reach here on a non-replicated deployment are a bug.
inline ReplicationManager& repl_mgr() {
    if (!Managers::s_repl_mgr_) {
        throw std::logic_error{"repl_mgr() called before ReplicationManager::create()/load() "
                               "(replication disabled — no repl_app provided?)"};
    }
    return *Managers::s_repl_mgr_;
}

} // namespace homestore
