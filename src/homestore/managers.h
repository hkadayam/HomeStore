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

#include <memory>
#include <stdexcept>
#include <homestore/homestore_decl.hpp> // unique<>, shared<>

namespace homestore {

class MetaBlkManager;
class DeviceManager;
class CPManager;
class BlobDevManager;

// ─────────────────────────────────────────────────────────────────────────────
// Managers
//
// Holds exactly one instance of each subsystem singleton.  Each subsystem's
// factory (create / load) calls the matching init_*() method once.  After
// that, callers use the free-function accessors below.
//
// Thread-safety: init_*() are called once at start-up before any concurrent
// access.  The accessors themselves are read-only and need no locking.
// ─────────────────────────────────────────────────────────────────────────────
class Managers {
public:
    static void init_meta_mgr(unique< MetaBlkManager > mgr);
    static void init_device_mgr(unique< DeviceManager > mgr);
    static void init_cp_mgr(unique< CPManager > mgr);
    static void init_blob_dev_mgr(unique< BlobDevManager > mgr);

    static void reset(); // for unit-test tear-down

private:
    friend MetaBlkManager& meta_mgr();
    friend DeviceManager& device_mgr();
    friend CPManager& cp_mgr();
    friend BlobDevManager& blob_dev_mgr();

    static unique< MetaBlkManager > s_meta_mgr_;
    static unique< DeviceManager >  s_device_mgr_;
    static unique< CPManager >      s_cp_mgr_;
    static unique< BlobDevManager > s_blob_dev_mgr_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Free-function accessors — call after the matching init_*() has returned.
// ─────────────────────────────────────────────────────────────────────────────
inline MetaBlkManager& meta_mgr() {
    if (!Managers::s_meta_mgr_) { throw std::logic_error{"meta_mgr() called before init_meta_mgr()"}; }
    return *Managers::s_meta_mgr_;
}

inline DeviceManager& device_mgr() {
    if (!Managers::s_device_mgr_) { throw std::logic_error{"device_mgr() called before init_device_mgr()"}; }
    return *Managers::s_device_mgr_;
}

inline CPManager& cp_mgr() {
    if (!Managers::s_cp_mgr_) { throw std::logic_error{"cp_mgr() called before init_cp_mgr()"}; }
    return *Managers::s_cp_mgr_;
}

inline BlobDevManager& blob_dev_mgr() {
    if (!Managers::s_blob_dev_mgr_) { throw std::logic_error{"blob_dev_mgr() called before init_blob_dev_mgr()"}; }
    return *Managers::s_blob_dev_mgr_;
}

} // namespace homestore
