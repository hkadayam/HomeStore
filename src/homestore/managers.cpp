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

#include "managers.h"
#include "meta/meta_blk_manager.hpp"
#include "device/device_manager.h"
#include "checkpoint/cp_mgr.h"

namespace homestore {

// Static member definitions
unique< MetaBlkManager > Managers::s_meta_mgr_;
unique< DeviceManager >  Managers::s_device_mgr_;
unique< CPManager >      Managers::s_cp_mgr_;

void Managers::init_meta_mgr(unique< MetaBlkManager > mgr) { s_meta_mgr_ = std::move(mgr); }
void Managers::init_device_mgr(unique< DeviceManager > mgr) { s_device_mgr_ = std::move(mgr); }
void Managers::init_cp_mgr(unique< CPManager > mgr) { s_cp_mgr_ = std::move(mgr); }

void Managers::reset() {
    s_meta_mgr_.reset();
    s_device_mgr_.reset();
    s_cp_mgr_.reset();
}

} // namespace homestore
