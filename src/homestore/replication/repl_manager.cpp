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
#include "common/async.h"

#include <fmt/format.h>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/string_generator.hpp>
#include <folly/executors/thread_factory/NamedThreadFactory.h>

#include <libnuraft/error_code.hxx>

#include "sisl/logging/logging.h"

#include "common/homestore_assert.h"
#include "common/homestore_config.h"
#include "homestore/homestore.h"
#include "homestore/meta/meta_blk.h"
#include "homestore/meta/meta_blk_manager.h"
#include "homestore/meta/meta_client.h"

#include <nlohmann/json.hpp>

#include "homestore/replication/replica_set.h"
#include "homestore/replication/transport/folly_rpc_client_factory.h"
#include "homestore/replication/transport/folly_rpc_listener.h"

namespace homestore {

static constexpr std::string_view kReplicaSetMetaName = "ReplicaSet";
static constexpr std::string_view kReplicaRaftConfigMetaName = "ReplicaRaftConfig";

ReplicationManager::ReplicationManager(shared< ReplApplication > repl_app) : repl_app_{std::move(repl_app)} {
}

ReplicationManager::~ReplicationManager() = default;

Async< void > ReplicationManager::start() {
    my_uuid_ = repl_app_->get_my_repl_id();

    auto [bind_host, bind_port] = repl_app_->lookup_peer(my_uuid_, GroupId{});
    RM_LOG(INFO, NO_TRACE_ID, "starting; my_uuid={} bind={}:{}", boost::uuids::to_string(my_uuid_), bind_host,
           bind_port);

    cpu_executor_ =
        std::make_unique< folly::CPUThreadPoolExecutor >(2, std::make_shared< folly::NamedThreadFactory >("repl_cpu"));

    rpc_client_factory_ = std::make_shared< replication::FollyRpcClientFactory >(cpu_executor_.get());
    rpc_listener_ =
        std::make_shared< replication::FollyRpcListener >(iomanager::iomgr().reactor_for(0), bind_port, this);
    nuraft::ptr< nuraft::msg_handler > null_handler;
    rpc_listener_->listen(null_handler);

    rs_meta_client_ =
        std::make_shared< MetaClient >(co_await hs()->meta_blk_mgr().register_client(std::string{kReplicaSetMetaName}));
    rs_raft_cfg_meta_client_ = std::make_shared< MetaClient >(
        co_await hs()->meta_blk_mgr().register_client(std::string{kReplicaRaftConfigMetaName}));

    // First pass: raft configs.  raft_group_config_found parses group_id off the payload and stashes
    // (mblk, json) into pending_configs_ so the SB walk can look them up.
    co_await rs_raft_cfg_meta_client_->for_each_recovered_block(
        [this](MetaBlk const& blk, sisl::IoBufView data) -> Async< void > {
            co_await raft_group_config_found(blk, data);
        });

    // Second pass: SBs.  For each SB, load_replica_set pulls the paired config out of pending_configs_
    // (or destroys the SB if the config is missing) and drives rs->start().
    co_await rs_meta_client_->for_each_recovered_block(
        [this](MetaBlk const& blk, sisl::IoBufView data) -> Async< void > { co_await load_replica_set(blk, data); });

    // Orphans: configs still in pending_configs_ never matched an SB.  Destroy them.
    for (auto& [gid, cfg_pair] : pending_configs_) {
        RM_LOG(WARN, NO_TRACE_ID, "Raft config for group_id={} has no matching SB — destroying orphan",
               boost::uuids::to_string(gid));
        co_await cfg_pair.first.destroy();
    }
    pending_configs_.clear();

    RM_LOG(INFO, NO_TRACE_ID, "Replica-set replay completed");

    hs()->cp_mgr().register_consumer("Replication", std::make_shared< ReplCPHandler >());

    // Kick off the two recurring maintenance loops.  Both run as coroutines on any reactor — no separate
    // reaper thread — because every SB write inside is co_await-able.
    persist_commit_lsn_timer_.start(
        iomanager::ReactorTarget::any(),
        std::chrono::milliseconds{HS_DYNAMIC_CONFIG(consensus.flush_durable_commit_interval_ms)},
        iomanager::TimerKind::Recurring, [this]() -> Async< void > { co_await persist_commit_lsn(); });

    gc_timer_.start(iomanager::ReactorTarget::any(),
                    std::chrono::milliseconds{HS_DYNAMIC_CONFIG(consensus.gc_scan_interval_ms)},
                    iomanager::TimerKind::Recurring, [this]() -> Async< void > { co_await gc_replica_sets(); });

    co_return;
}

Async< void > ReplicationManager::stop() {
    // Stop timers first so no new maintenance fires against a listener that's about to shut down.  Each
    // stop() is idempotent and blocks until the currently-scheduled coroutine (if any) has drained.
    gc_timer_.stop();
    persist_commit_lsn_timer_.stop();

    if (rpc_listener_) {
        rpc_listener_->shutdown();
        rpc_listener_.reset();
    }
    rpc_client_factory_.reset();
    if (cpu_executor_) {
        cpu_executor_->stop();
        cpu_executor_.reset();
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

Async< ReplResult< shared< ReplicaSet > > >
ReplicationManager::create_replica_set(GroupId group_id, std::set< ReplicaId > const& members) {
    // Gate under rs_mtx_: if the group already exists, that's an idempotent return.  If a create is
    // in-flight (pending_creates_), a concurrent racer got here first — this call bails with
    // SERVER_ALREADY_EXISTS rather than duplicating the SB / raft_server setup and having to unwind
    // (which would fire on_destroy on a listener the app never really got to use).
    {
        std::unique_lock lk{rs_mtx_};
        if (auto it = replica_sets_.find(group_id); it != replica_sets_.end() && it->second) {
            co_return it->second;
        }
        if (pending_creates_.count(group_id)) {
            co_return folly::makeUnexpected(ReplError::SERVER_ALREADY_EXISTS);
        }
        pending_creates_.insert(group_id);
    }

    // Every error exit below must erase pending_creates_[group_id]; the success path erases it atomically
    // with the replica_sets_ insert under one lock.

    auto const gid_str = boost::uuids::to_string(group_id);
    auto listener = repl_app_->create_replica_set_listener(group_id, /*load_existing=*/false);
    if (!listener) {
        RM_LOG(INFO, NO_TRACE_ID, "Application rejected group_id={} on create", gid_str);
        std::unique_lock lk{rs_mtx_};
        pending_creates_.erase(group_id);
        co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
    }

    // SB — build initial ReplicaSetSuperBlk in a tmp buffer, persist, construct the ReplicaSet.  rs->start()
    // below reads the SB back into its own buffer.
    auto sb_mblk =
        co_await MetaBlkWrapper::create(rs_meta_client_, fmt::format("rs_{}", gid_str), sizeof(ReplicaSetSuperBlk));
    {
        auto tmp = sisl::make_io_buf_shared(to_u32(sizeof(ReplicaSetSuperBlk)));
        auto* sb = new (tmp->bytes()) ReplicaSetSuperBlk{};
        sb->group_id = group_id;
        sb->is_timeline_consistent = repl_app_->need_timeline_consistency() ? 1 : 0;
        sb->set_rset_name(fmt::format("rset_{}", gid_str.substr(0, 8)));
        co_await sb_mblk.write(tmp->cbytes(), tmp->size());
    }

    // Raft config MetaBlk.  Unlike on-demand, we PRE-POPULATE the cluster config with every `members`
    // entry — the state_mgr load_config path finds "config" already present and returns it as-is instead
    // of falling back to the single-self bootstrap.  Each server's aux holds the UUID string (matches
    // add_member()'s stamping) so downstream int32-srv_id → ReplicaId reverse lookups uniformly work.
    auto servers = nlohmann::json::array();
    auto const priority = HS_DYNAMIC_CONFIG(consensus.default_leader_priority);
    for (auto const& member_id : members) {
        servers.push_back(nlohmann::json{{"id", to_server_id(member_id)},
                                         {"dc_id", 0},
                                         {"endpoint", lookup_peer_addr(member_id)},
                                         {"aux", boost::uuids::to_string(member_id)},
                                         {"learner", false},
                                         {"priority", priority}});
    }
    nlohmann::json raft_cfg_json = {{"group_id", gid_str},
                                    {"config",
                                     {{"log_idx", 0},
                                      {"prev_log_idx", 0},
                                      {"eventual_consistency", false},
                                      {"user_ctx", std::string{}},
                                      {"servers", std::move(servers)}}}};

    auto raft_cfg_mblk =
        co_await MetaBlkWrapper::create(rs_raft_cfg_meta_client_, fmt::format("cfg_{}", gid_str), /*size=*/{});
    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb_mblk));
    rs->attach_listener(std::move(listener));
    if (!co_await rs->start(std::move(raft_cfg_mblk), std::move(raft_cfg_json))) {
        RM_LOG(ERROR, NO_TRACE_ID, "start failed for created group_id={}", gid_str);
        // Unwind everything rs->start() may have partially built (SB, cfg mblk, log store), then fire
        // on_destroy on the listener that the app allocated for this create.
        co_await rs->finish_destroy_local();
        std::unique_lock lk{rs_mtx_};
        pending_creates_.erase(group_id);
        co_return folly::makeUnexpected(ReplError::FAILED);
    }

    {
        std::unique_lock lk{rs_mtx_};
        replica_sets_.emplace(group_id, rs);
        pending_creates_.erase(group_id);
    }
    RM_LOG(INFO, NO_TRACE_ID, "Created ReplicaSet group_id={} members={}", gid_str, members.size());
    co_return rs;
}

Async< nuraft::ptr< nuraft::raft_server > >
ReplicationManager::create_replica_set_on_demand(nuraft::group_id_t const& gid) {
    GroupId group_id;
    std::memcpy(group_id.data, gid.data(), gid.size());
    auto const gid_str = boost::uuids::to_string(group_id);

    // Two peers can race to send us the first frame for a new group_id.  Gate under rs_mtx_: an existing
    // entry means we're done; a pending entry means the other peer's create is already running, and this
    // call bails to nullptr.  The RPC listener drops the frame; the peer retries; by then the winning
    // create has published to replica_sets_ and the retry hits it.
    {
        std::unique_lock lk{rs_mtx_};
        if (auto it = replica_sets_.find(group_id); it != replica_sets_.end() && it->second) {
            co_return it->second->raft_server();
        }
        if (pending_creates_.count(group_id)) {
            co_return nullptr;
        }
        pending_creates_.insert(group_id);
    }

    auto listener = repl_app_->create_replica_set_listener(group_id, /*load_existing=*/false);
    if (!listener) {
        RM_LOG(INFO, NO_TRACE_ID, "Application rejected unknown group_id={}", gid_str);
        std::unique_lock lk{rs_mtx_};
        pending_creates_.erase(group_id);
        co_return nullptr;
    }

    // Allocate a fresh MetaBlk under the shared "ReplicaSet" client for this group's SB.  Build the
    // initial ReplicaSetSuperBlk in a temporary buffer and persist it through sb_mblk.write().  The tmp
    // dies here; rs->start() will read the SB back into ReplicaSet's own buffer during setup.
    auto sb_mblk =
        co_await MetaBlkWrapper::create(rs_meta_client_, fmt::format("rs_{}", gid_str), sizeof(ReplicaSetSuperBlk));
    {
        auto tmp = sisl::make_io_buf_shared(to_u32(sizeof(ReplicaSetSuperBlk)));
        auto* sb = new (tmp->bytes()) ReplicaSetSuperBlk{};
        sb->group_id = group_id;
        sb->is_timeline_consistent = repl_app_->need_timeline_consistency() ? 1 : 0;
        sb->set_rset_name(fmt::format("rset_{}", gid_str.substr(0, 8)));
        co_await sb_mblk.write(tmp->cbytes(), tmp->size());
    }

    // Companion MetaBlk for the raft cluster/state config JSON.  Body starts with just the group_id key so
    // restart's raft_group_config_found can pair the config to its SB.  The state_mgr load_config bootstrap
    // path fills in the "config" and "state" keys on first save.
    auto raft_cfg_mblk =
        co_await MetaBlkWrapper::create(rs_raft_cfg_meta_client_, fmt::format("cfg_{}", gid_str), /*size=*/{});
    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb_mblk));
    rs->attach_listener(std::move(listener));
    nlohmann::json raft_cfg_json = {{"group_id", gid_str}};
    if (!co_await rs->start(std::move(raft_cfg_mblk), std::move(raft_cfg_json))) {
        RM_LOG(ERROR, NO_TRACE_ID, "start failed for newly-created group_id={}", gid_str);
        co_await rs->finish_destroy_local();
        std::unique_lock lk{rs_mtx_};
        pending_creates_.erase(group_id);
        co_return nullptr;
    }

    {
        std::unique_lock lk{rs_mtx_};
        replica_sets_.emplace(group_id, rs);
        pending_creates_.erase(group_id);
    }
    RM_LOG(INFO, NO_TRACE_ID, "Created ReplicaSet on demand for group_id={}", gid_str);
    co_return rs->raft_server();
}

Async< ReplError > ReplicationManager::remove_replica_set(GroupId group_id) {
    // Consumer-visible remove: delegates to rs->destroy() (leader-side proposes HS_CTRL_DESTROY), which
    // resolves once the CTRL entry commits.  By that point every replica that saw the commit has run
    // start_destroy_local() locally (via dispatch_commit).  The reaper (gc_replica_sets) picks it up on
    // its next scan and calls finish_destroy_local() + erases from replica_sets_.
    auto rs_result = get_replica_set(group_id);
    if (!rs_result.hasValue() || !rs_result.value()) {
        co_return ReplError::SERVER_NOT_FOUND;
    }
    co_return co_await rs_result.value()->destroy();
}

Async< void > ReplicationManager::load_replica_set(MetaBlk const& blk, sisl::IoBufView data) {
    if (data.size() < sizeof(ReplicaSetSuperBlk)) {
        RM_LOG(ERROR, NO_TRACE_ID, "Recovered SB block too small ({}B) — destroying", data.size());
        co_await MetaBlkWrapper::load(rs_meta_client_, blk).destroy();
        co_return;
    }
    auto const* sb = r_cast< ReplicaSetSuperBlk const* >(data.cbytes());
    GroupId const group_id = sb->group_id;
    auto const gid_str = boost::uuids::to_string(group_id);

    auto sb_mblk = MetaBlkWrapper::load(rs_meta_client_, blk);

    // Pair the SB with its raft config.  No matching entry in pending_configs_ means this SB was persisted
    // but the companion config never was (crash between the two writes) — the group is unrecoverable, so
    // destroy the SB and move on.
    auto cfg_it = pending_configs_.find(group_id);
    if (cfg_it == pending_configs_.end()) {
        RM_LOG(WARN, NO_TRACE_ID, "SB for group_id={} has no matching raft config — destroying orphan SB", gid_str);
        co_await sb_mblk.destroy();
        co_return;
    }
    auto [raft_cfg_mblk, raft_cfg_json] = std::move(cfg_it->second);
    pending_configs_.erase(cfg_it);

    // Ask the app for a listener.  If it declines, we leave the on-disk state alone — the group might be
    // handled by a future run of the application.
    auto listener = repl_app_->create_replica_set_listener(group_id, /*load_existing=*/true);
    if (!listener) {
        RM_LOG(WARN, NO_TRACE_ID, "Application declined reloaded group_id={} — leaving SB + config on disk", gid_str);
        co_return;
    }

    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb_mblk));
    rs->attach_listener(std::move(listener));
    if (!co_await rs->start(std::move(raft_cfg_mblk), std::move(raft_cfg_json))) {
        RM_LOG(ERROR, NO_TRACE_ID, "start failed for reloaded group_id={} — leaving SB + config on disk", gid_str);
        co_return;
    }

    {
        std::unique_lock lk{rs_mtx_};
        replica_sets_.emplace(group_id, std::move(rs));
    }
    RM_LOG(INFO, NO_TRACE_ID, "Reloaded ReplicaSet group_id={}", gid_str);
}

Async< void > ReplicationManager::raft_group_config_found(MetaBlk const& blk, sisl::IoBufView data) {
    nlohmann::json cfg_json;
    try {
        cfg_json = nlohmann::json::from_msgpack(data.cbytes(), data.cbytes() + data.size());
    } catch (std::exception const& e) {
        RM_LOG(ERROR, NO_TRACE_ID, "Corrupt raft config block — destroying: {}", e.what());
        co_await MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk).destroy();
        co_return;
    }
    if (!cfg_json.contains("group_id")) {
        RM_LOG(ERROR, NO_TRACE_ID, "Raft config block missing group_id — destroying");
        co_await MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk).destroy();
        co_return;
    }
    GroupId group_id;
    try {
        group_id = boost::uuids::string_generator{}(cfg_json["group_id"].get< std::string >());
    } catch (std::exception const& e) {
        RM_LOG(ERROR, NO_TRACE_ID, "Raft config block has malformed group_id ({}) — destroying", e.what());
        co_await MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk).destroy();
        co_return;
    }
    pending_configs_.emplace(group_id,
                             std::pair{MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk), std::move(cfg_json)});
    co_return;
}

Async< void > ReplicationManager::gc_replica_sets() {
    // Snapshot the reap candidates under the shared lock — never hold the lock across a co_await.
    auto const grace = std::chrono::seconds{HS_DYNAMIC_CONFIG(consensus.repl_dev_cleanup_interval_sec)};
    auto const now = Clock::now();
    std::vector< std::pair< GroupId, shared< ReplicaSet > > > reap_list;
    {
        std::shared_lock lk{rs_mtx_};
        for (auto const& [gid, rs] : replica_sets_) {
            if (!rs) {
                continue;
            }
            if (rs->is_destroy_pending() && (now - rs->destroyed_time() > grace)) {
                reap_list.emplace_back(gid, rs);
            }
        }
    }

    // Drive finish_destroy_local() outside the lock — this co_awaits SB destroys.  After it settles, take
    // the write lock briefly to erase from the registry, then notify the application so it can drop any
    // listener-scoped state (separate from the listener's own on_destroy that finish_destroy_local fired).
    // If the ReplicaSet has already been erased by a concurrent path, the erase is a no-op.
    for (auto& [gid, rs] : reap_list) {
        RM_LOG(INFO, NO_TRACE_ID, "reaper: finish_destroy_local group_id={}", boost::uuids::to_string(gid));
        co_await rs->finish_destroy_local();
        {
            std::unique_lock lk{rs_mtx_};
            replica_sets_.erase(gid);
        }
        repl_app_->destroy_replica_set_listener(gid);
    }
    co_return;
}

Async< void > ReplicationManager::persist_commit_lsn() {
    // Snapshot the current set of replica sets and fan out.  Under a shared lock we grab shared_ptrs so
    // sets that get destroyed mid-fan-out stay alive until we're done touching them; the co_await calls run
    // outside the lock.
    std::vector< shared< ReplicaSet > > sets;
    {
        std::shared_lock lk{rs_mtx_};
        sets.reserve(replica_sets_.size());
        for (auto const& [_, rs] : replica_sets_) {
            if (rs) {
                sets.push_back(rs);
            }
        }
    }
    for (auto const& rs : sets) {
        co_await rs->persist_commit_lsn();
    }
    co_return;
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

void ReplCPHandler::on_switchover_cp(CP* /*cur_cp*/, CP* /*new_cp*/) {
    // Fan out synchronously — each RS just captures an atomic.  Under shared_lock so a concurrent
    // create/remove can't invalidate iterators.
    repl_service().iterate_replica_sets([](cshared< ReplicaSet > const& rs) { rs->on_switchover_cp(); });
}

Async< bool > ReplCPHandler::cp_flush(CP* /*cp*/) {
    // Snapshot registry under shared_lock, then fan out awaits outside the lock so the SB write doesn't
    // hold the map lock.  Sequential co_await — CPs are low frequency (default 60s cadence) and per-RS
    // flush is a single MetaBlk write, so ordering by rs is fine.
    //
    // CORRECTNESS DEPENDENCY: this handler MUST run LAST among all CPConsumers, after every subsystem the
    // consumer's on_commit writes into (Index, BlkAlloc, VDev, etc.) has already flushed.  Persisting
    // checkpoint_lsn before those flushes complete would claim a durability watermark the data has not
    // reached — restart's on_log_found would then skip replay for LSNs the consumer state doesn't actually
    // hold.  See TODO(cp-ordering) in cp_mgr.cpp::cp_start_flush; today the order is whatever
    // std::unordered_map iteration returns, so the invariant is not yet enforced.
    std::vector< shared< ReplicaSet > > rsets;
    repl_service().iterate_replica_sets([&rsets](cshared< ReplicaSet > const& rs) { rsets.push_back(rs); });
    for (auto const& rs : rsets) {
        co_await rs->cp_flush();
    }
    co_return true;
}

void ReplCPHandler::cp_cleanup(CP* /*cp*/) {
}

int ReplCPHandler::cp_progress_percent() {
    return 100;
}

ReplicationManager& repl_service() {
    return hs()->repl_service();
}

} // namespace homestore