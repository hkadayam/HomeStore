/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/

#include "homestore/replication/repl_manager.h"

#include <boost/uuid/uuid_io.hpp>
#include <folly/executors/thread_factory/NamedThreadFactory.h>

#include <libnuraft/error_code.hxx>

#include "sisl/logging/logging.h"

#include "common/homestore_assert.h"
#include "common/homestore_config.h"
#include "homestore/homestore.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/meta/meta_client.h"

#include "homestore/replication/replica_set.h"
#include "homestore/replication/transport/folly_rpc_client_factory.h"
#include "homestore/replication/transport/folly_rpc_listener.h"

namespace homestore {

static constexpr std::string_view kReplDevMetaName = "repl_dev";
static constexpr std::string_view kReplDevRaftConfigMetaName = "repl_dev_raft_config";

ReplicationManager::ReplicationManager(shared< ReplApplication > repl_app) : repl_app_{std::move(repl_app)} {
}

ReplicationManager::~ReplicationManager() = default;

folly::coro::Task< void > ReplicationManager::start() {
    my_uuid_ = repl_app_->get_my_repl_id();

    auto [bind_host, bind_port] = repl_app_->lookup_peer(my_uuid_, GroupId{});
    LOGINFOMOD(replication, "ReplicationManager starting; my_uuid={} bind={}:{}", boost::uuids::to_string(my_uuid_),
               bind_host, bind_port);

    slow_executor_ = std::make_unique< folly::CPUThreadPoolExecutor >(
        2, std::make_shared< folly::NamedThreadFactory >("repl_slow"));

    rpc_client_factory_ = std::make_shared< replication::FollyRpcClientFactory >(slow_executor_.get());
    rpc_listener_ =
        std::make_shared< replication::FollyRpcListener >(iomanager::iomgr().reactor_for(0), bind_port, this);
    nuraft::ptr< nuraft::msg_handler > null_handler;
    rpc_listener_->listen(null_handler);

    rs_meta_client_ =
        std::make_shared< MetaClient >(co_await hs()->meta_blk_mgr().register_client(std::string{kReplDevMetaName}));
    rs_raft_cfg_meta_client_ = std::make_shared< MetaClient >(
        co_await hs()->meta_blk_mgr().register_client(std::string{kReplDevRaftConfigMetaName}));

    co_await rs_meta_client_->for_each_recovered_block(
        [this](MetaBlk const& blk, sisl::IoBufView data) -> folly::coro::Task< void > {
            (void)blk;
            load_replica_set(data);
            co_return;
        });

    co_await rs_raft_cfg_meta_client_->for_each_recovered_block(
        [this](MetaBlk const& blk, sisl::IoBufView data) -> folly::coro::Task< void > {
            (void)blk;
            (void)raft_group_config_found(data);
            co_return;
        });

    LOGINFOMOD(replication, "Replica-set replay completed; notifying application");
    repl_app_->on_replica_sets_init_completed();

    hs()->cp_mgr().register_consumer("Replication", std::make_shared< ReplCPHandler >());

    hs()->logstore_service().delete_unopened_logdevs();
    co_return;
}

folly::coro::Task< void > ReplicationManager::stop() {
    if (rpc_listener_) {
        rpc_listener_->shutdown();
        rpc_listener_.reset();
    }
    rpc_client_factory_.reset();
    if (slow_executor_) {
        slow_executor_->stop();
        slow_executor_.reset();
    }
    co_return;
}

ReplResult< shared< ReplicaSet > > ReplicationManager::get_replica_set(GroupId group_id) const {
    std::shared_lock lk{rs_mtx_};
    auto it = replica_sets_.find(group_id);
    if (it == replica_sets_.end()) {
        return folly::makeUnexpected(ReplError::SERVER_NOT_FOUND);
    }
    return it->second;
}

void ReplicationManager::iterate_replica_sets(std::function< void(cshared< ReplicaSet >&) > const& cb) {
    std::shared_lock lk{rs_mtx_};
    for (auto& [_, rs] : replica_sets_) {
        cshared< ReplicaSet > snapshot = rs;
        cb(snapshot);
    }
}

std::string ReplicationManager::lookup_peer_addr(ReplicaId const& peer) const {
    auto const p = repl_app_->lookup_peer(peer, GroupId{});
    if (p.first.empty()) {
        return {};
    }
    return fmt::format("{}:{}", p.first, p.second);
}

nuraft::ptr< nuraft::raft_server > ReplicationManager::lookup_raft_server(nuraft::group_id_t const& gid) const {
    GroupId group_id;
    std::memcpy(group_id.data, gid.data(), gid.size());
    auto rs = get_replica_set(group_id);
    if (!rs.hasValue() || !rs.value()) {
        return nullptr;
    }
    return rs.value()->raft_server();
}

nuraft::ptr< nuraft::raft_server >
ReplicationManager::lookup_or_create_raft_server(nuraft::group_id_t const& gid) {
    GroupId group_id;
    std::memcpy(group_id.data, gid.data(), gid.size());

    // Fast path: existing group under a shared lock.
    {
        std::shared_lock lk{rs_mtx_};
        if (auto it = replica_sets_.find(group_id); it != replica_sets_.end() && it->second) {
            return it->second->raft_server();
        }
    }

    // Slow path: ask the application whether this unknown group should be admitted; if so construct the
    // ReplicaSet, attach the listener it returned, and bring up the raft_server.
    std::unique_lock lk{rs_mtx_};
    if (auto it = replica_sets_.find(group_id); it != replica_sets_.end() && it->second) {
        // Lost the race; somebody else already created it.
        return it->second->raft_server();
    }

    auto listener = repl_app_->create_replica_set_listener(group_id);
    if (!listener) {
        LOGINFOMOD(replication, "Application rejected unknown group_id={}", boost::uuids::to_string(group_id));
        return nullptr;
    }

    superblk< ReplicaSetSuperBlk > sb{std::string{kReplDevMetaName}};
    sb.create();
    sb->group_id = group_id;
    sb->is_timeline_consistent = repl_app_->need_timeline_consistency() ? 1 : 0;
    sb->destroy_pending = 0;
    sb->last_snapshot_lsn = 0;

    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb), /*load_existing=*/false);
    rs->attach_listener(std::move(listener));
    if (!rs->join_group()) {
        LOGERRORMOD(replication, "join_group failed for newly-created group_id={}",
                    boost::uuids::to_string(group_id));
        return nullptr;
    }

    replica_sets_.emplace(group_id, rs);
    LOGINFOMOD(replication, "Created ReplicaSet on demand for group_id={}", boost::uuids::to_string(group_id));
    return rs->raft_server();
}

void ReplicationManager::add_replica_set(GroupId group_id, shared< ReplicaSet > rs) {
    std::unique_lock lk{rs_mtx_};
    replica_sets_.emplace(group_id, std::move(rs));
}

folly::coro::Task< ReplResult< shared< ReplicaSet > > >
ReplicationManager::create_replica_set(GroupId group_id, std::set< ReplicaId > const& members) {
    (void)group_id;
    (void)members;
    co_return folly::makeUnexpected(ReplError::NOT_IMPLEMENTED);
}

folly::coro::Task< ReplError > ReplicationManager::remove_replica_set(GroupId group_id) {
    (void)group_id;
    co_return ReplError::NOT_IMPLEMENTED;
}

folly::coro::Task< ReplResult<> > ReplicationManager::replace_member(GroupId group_id,
                                                                     ReplicaMemberInfo const& member_out,
                                                                     ReplicaMemberInfo const& member_in,
                                                                     uint32_t commit_quorum, uint64_t trace_id) const {
    (void)group_id;
    (void)member_out;
    (void)member_in;
    (void)commit_quorum;
    (void)trace_id;
    co_return folly::makeUnexpected(ReplError::NOT_IMPLEMENTED);
}

folly::coro::Task< ReplResult<> > ReplicationManager::flip_learner_flag(GroupId group_id,
                                                                        ReplicaMemberInfo const& member, bool target,
                                                                        uint32_t commit_quorum, bool wait_and_verify,
                                                                        uint64_t trace_id) const {
    (void)group_id;
    (void)member;
    (void)target;
    (void)commit_quorum;
    (void)wait_and_verify;
    (void)trace_id;
    co_return folly::makeUnexpected(ReplError::NOT_IMPLEMENTED);
}

shared< ReplicaSet > ReplicationManager::create_state_mgr(int32_t /*srv_id*/, GroupId const& /*group_id*/) {
    return nullptr;
}

void ReplicationManager::load_replica_set(sisl::IoBufView const& /*buf*/, void* /*meta_cookie*/) {
}

ReplicaSet* ReplicationManager::raft_group_config_found(sisl::IoBufView const& /*buf*/, void* /*meta_cookie*/) {
    return nullptr;
}

void ReplicationManager::start_reaper_thread() {
}
void ReplicationManager::stop_reaper_thread() {
}
void ReplicationManager::gc_replica_sets() {
}
void ReplicationManager::gc_repl_reqs() {
}
void ReplicationManager::flush_durable_commit_lsn() {
}
void ReplicationManager::check_replace_member_status() {
}

ReplError ReplicationManager::to_repl_error(nuraft::cmd_result_code code) {
    switch (code) {
    case nuraft::OK:
        return ReplError::OK;
    case nuraft::CANCELLED:
        return ReplError::CANCELLED;
    case nuraft::TIMEOUT:
        return ReplError::TIMEOUT;
    case nuraft::NOT_LEADER:
        return ReplError::NOT_LEADER;
    case nuraft::BAD_REQUEST:
        return ReplError::BAD_REQUEST;
    case nuraft::SERVER_ALREADY_EXISTS:
        return ReplError::SERVER_ALREADY_EXISTS;
    case nuraft::CONFIG_CHANGING:
        return ReplError::CONFIG_CHANGING;
    case nuraft::SERVER_IS_JOINING:
        return ReplError::SERVER_IS_JOINING;
    case nuraft::SERVER_NOT_FOUND:
        return ReplError::SERVER_NOT_FOUND;
    case nuraft::CANNOT_REMOVE_LEADER:
        return ReplError::CANNOT_REMOVE_LEADER;
    case nuraft::SERVER_IS_LEAVING:
        return ReplError::SERVER_IS_LEAVING;
    case nuraft::TERM_MISMATCH:
        return ReplError::TERM_MISMATCH;
    case nuraft::RESULT_NOT_EXIST_YET:
        return ReplError::RESULT_NOT_EXIST_YET;
    default:
        return ReplError::FAILED;
    }
}

int32_t ReplicationManager::compute_raft_follower_priority() {
    auto p = static_cast< int32_t >(raft_leader_priority * raft_priority_decay_coefficient);
    return p > 0 ? p : 1;
}

void ReplicationManagerCPHandler::on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) {
}

folly::coro::Task< bool > ReplicationManagerCPHandler::cp_flush(CP* /*cp*/) {
    co_return true;
}

void ReplicationManagerCPHandler::cp_cleanup(CP* /*cp*/) {
}

int ReplicationManagerCPHandler::cp_progress_percent() {
    return 100;
}

ReplicationManager& repl_service() {
    return hs()->repl_service();
}

} // namespace homestore