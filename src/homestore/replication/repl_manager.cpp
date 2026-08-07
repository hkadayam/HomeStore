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
#include <folly/base64.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>

#include <libnuraft/async.hxx>
#include <libnuraft/async_compat.hxx>
#include <libnuraft/error_code.hxx>
#include <libnuraft/raft_server.hxx>

#include "sisl/logging/logging.h"

#include "homestore/base/homestore_assert.h"
#include "homestore/base/hs_runtime_config.h"
#include "homestore/homestore.h"
#include "homestore/managers.h"
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

// The MetaBlk name field holds at most 31 chars, so a group's 36-char uuid string does not fit.  base64url of the
// uuid's 16 raw bytes is 22 chars and collision-free — recovery keys off the meta client and the group_id stored in
// the SB payload, so this name only needs to be unique per replica set.
static std::string gid_meta_tag(GroupId const& gid) {
    return folly::base64URLEncode(std::string_view{r_cast< char const* >(&*gid.begin()), gid.size()});
}

ReplicationManager::ReplicationManager(shared< ReplApplication > repl_app) : repl_app_{std::move(repl_app)} {
}

ReplicationManager::~ReplicationManager() = default;

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Static factories + phased bring-up.  Mirrors the manager lifecycle driven by HomeStore:
//   create() — first-time boot: infra + register CP consumer + go live (listen + timers).  No groups exist;
//              runtime create_replica_set/on_demand builds and launches each one individually.
//   load()   — recovery: infra + reconstruct replica sets (ReplicaSet::load, NO engine) + register CP consumer
//              (after the sets exist, so its initial on_switchover_cp seeds each set's per-CP checkpoint state).
//   replay() — recovery go-live: launch every reconstructed set's raft engine, then listen + timers.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< void > ReplicationManager::create(shared< ReplApplication > repl_app) {
    auto mgr = std::make_shared< ReplicationManager >(std::move(repl_app));
    co_await mgr->setup_infra();
    co_await mgr->start_engine();
    Managers::init_repl_mgr(mgr);
    RM_LOG(INFO, NO_TRACE_ID, "Replication service created (first-time boot)");
}

Async< void > ReplicationManager::load(shared< ReplApplication > repl_app) {
    auto mgr = std::make_shared< ReplicationManager >(std::move(repl_app));
    co_await mgr->setup_infra();
    co_await mgr->reconstruct_replica_sets();
    Managers::init_repl_mgr(mgr);
    RM_LOG(INFO, NO_TRACE_ID, "Replication service loaded ({} replica set(s)); awaiting replay()",
           mgr->replica_sets_.size());
}

Async< void > ReplicationManager::start_engine() {
    // Launch every reconstructed set's raft engine, then go live (open inbound traffic + maintenance timers).
    // First-time boot has no reconstructed sets, so the loop is a no-op and this just goes live; on recovery it
    // runs after LogStoreManager::replay() has restored each set's commit_upto_lsn (nuraft reads the state
    // machine's last-committed index at start_server).
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
        if (!co_await rs->start_engine()) {
            RM_LOG(ERROR, NO_TRACE_ID, "raft engine start failed for a reloaded group");
        }
    }

    // Go live: open the rpc listener to inbound traffic and start the gc maintenance loop (coroutines on any
    // reactor — every SB write inside is co_await-able, so no dedicated reaper thread).  Commit-watermark
    // durability needs no loop here: each ReplicaSet's commit_upto rides its raft log store's checkpt_lsn,
    // captured at every CP switchover.
    nuraft::ptr< nuraft::msg_handler > null_handler;
    rpc_listener_->listen(null_handler);
    gc_timer_.start(iomanager::ReactorTarget::any(),
                    std::chrono::milliseconds{HS_RUNTIME_CONFIG(consensus.replica_set_reaper_scan_interval_ms)},
                    iomanager::TimerKind::Recurring, [this]() -> Async< void > { co_await gc_replica_sets(); });

    RM_LOG(INFO, NO_TRACE_ID, "Replication service live ({} replica set(s))", sets.size());
    co_return;
}

// ── Private bring-up helpers ─────────────────────────────────────────────────────────────────────────────────

Async< void > ReplicationManager::setup_infra() {
    my_uuid_ = repl_app_->get_my_repl_id();

    auto [bind_host, bind_port] = repl_app_->lookup_peer(my_uuid_, GroupId{});
    RM_LOG(INFO, NO_TRACE_ID, "setup; my_uuid={} bind={}:{}", boost::uuids::to_string(my_uuid_), bind_host, bind_port);

    cpu_executor_ =
        std::make_unique< folly::CPUThreadPoolExecutor >(2, std::make_shared< folly::NamedThreadFactory >("repl_cpu"));

    rpc_client_factory_ = std::make_shared< replication::FollyRpcClientFactory >(cpu_executor_.get());
    // Construct the listener but do NOT listen() yet — start_engine() opens inbound traffic once engines are up.
    rpc_listener_ =
        std::make_shared< replication::FollyRpcListener >(iomgr().reactor_for(0), bind_port, this);

    rs_meta_client_ =
        std::make_shared< MetaClient >(co_await meta_mgr().register_client(std::string{kReplicaSetMetaName}));
    rs_raft_cfg_meta_client_ =
        std::make_shared< MetaClient >(co_await meta_mgr().register_client(std::string{kReplicaRaftConfigMetaName}));
    co_return;
}

Async< void > ReplicationManager::reconstruct_replica_sets() {
    // First pass: raft configs.  raft_group_config_found parses group_id off the payload and stashes
    // (mblk, json) into pending_configs_ so the SB walk can look them up.
    co_await rs_raft_cfg_meta_client_->for_each_recovered_block(
        [this](MetaBlk const& blk, sisl::IoBufView data) -> Async< void > {
            co_await raft_group_config_found(blk, data);
        });

    // Second pass: SBs.  For each SB, load_replica_set pulls the paired config out of pending_configs_
    // (or destroys the SB if the config is missing) and drives rs->load() (engine start deferred to replay()).
    co_await rs_meta_client_->for_each_recovered_block(
        [this](MetaBlk const& blk, sisl::IoBufView data) -> Async< void > { co_await load_replica_set(blk, data); });

    // Orphans: configs still in pending_configs_ never matched an SB.  Destroy them.
    for (auto& [gid, cfg_pair] : pending_configs_) {
        RM_LOG(WARN, NO_TRACE_ID, "Raft config for group_id={} has no matching SB — destroying orphan",
               boost::uuids::to_string(gid));
        co_await cfg_pair.first.destroy();
    }
    pending_configs_.clear();
    RM_LOG(INFO, NO_TRACE_ID, "Replica-set reconstruction completed");
    co_return;
}

Async< void > ReplicationManager::stop() {
    // Publish "stopping" FIRST: from here on, inbound joins and create/remove requests are refused.  A
    // join_cluster_request arriving mid-teardown would otherwise construct a raft server on top of a
    // HomeStore being freed underneath it — the sender gets SERVER_NOT_FOUND and its retry finds the
    // group after the next boot instead.  Two-phase make() only: exchange()'s synchronize_rcu would BLOCK
    // this reactor thread until every RCU reader quiesces — mid-shutdown a suspended reader may never
    // resume, wedging the reactor and the iomgr join behind it.  The superseded node stays parked until
    // the manager is destroyed; readers see the new state immediately.
    state_.make(RuntimeState{.stopping = true});

    // Close AND drain the inbound door before touching any engine.  The drain suspends until every in-flight
    // dispatch Task has finished — including one mid create_replica_set_on_demand that passed the stopping
    // gate before the store above: its ReplicaSet is registered by the time the drain releases, so the engine
    // sweep below shuts it down, and nothing this function frees afterwards (engines, client factory,
    // executor) can be observed half-torn-down by a dispatch.  Dispatches complete rather than wedge because
    // engines and storage are all still fully live here (storage teardown happens after replication stops).
    if (rpc_listener_) {
        co_await rpc_listener_->shutdown_and_drain();
        rpc_listener_.reset();
    }

    // Stop timers so no new maintenance fires against engines that are about to shut down.  Each stop()
    // is idempotent and blocks until the currently-scheduled coroutine (if any) has drained.
    co_await gc_timer_.stop();

    // Shut down every raft engine BEFORE freeing the RPC client factory / executor it references.  Each
    // engine's nuraft::context holds mgr_.rpc_client_factory() and the reactor executor; freeing those
    // first would leave the servers dangling, and nuraft's raft_server destructor asserts shutdown() ran.
    // Snapshot the registry under the shared lock, then release it before co_awaiting (never hold rs_mtx_
    // across a suspension point).
    std::vector< shared< ReplicaSet > > sets;
    {
        std::shared_lock lk{rs_mtx_};
        sets.reserve(replica_sets_.size());
        for (auto const& [_, rs] : replica_sets_) {
            sets.push_back(rs);
        }
    }
    for (auto const& rs : sets) {
        co_await rs->stop_engine();
    }
    if (rpc_client_factory_) {
        rpc_client_factory_->shutdown(); // tear down each reactor's outbound sockets on its own thread
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

nuraft::raft_server* ReplicationManager::lookup_raft_server(nuraft::group_id_t const& gid) const {
    GroupId group_id;
    std::memcpy(group_id.data, gid.data(), gid.size());
    auto rs = get_replica_set(group_id);
    if (!rs.hasValue() || !rs.value()) {
        return nullptr;
    }
    return rs.value()->raft_server();
}

Async< ReplResult< shared< ReplicaSet > > > ReplicationManager::create_replica_set(GroupId group_id,
                                                                                   std::set< ReplicaId > const& members,
                                                                                   ReplicaSetOptions const& options) {
    if (state_.get()->stopping) {
        co_return folly::makeUnexpected(ReplError::STOPPING);
    }
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
        co_await MetaBlkWrapper::create(rs_meta_client_, fmt::format("rs_{}", gid_meta_tag(group_id)),
                                        sizeof(ReplicaSetSuperBlk));
    {
        auto tmp = sisl::make_io_buf_shared(to_u32(sizeof(ReplicaSetSuperBlk)));
        auto* sb = new (tmp->bytes()) ReplicaSetSuperBlk{};
        sb->group_id = group_id;
        sb->is_timeline_consistent = repl_app_->need_timeline_consistency() ? 1 : 0;
        sb->set_rset_name(fmt::format("rset_{}", gid_str.substr(0, 8)));
        co_await sb_mblk.write(tmp->cbytes(), tmp->size());
    }

    // Raft config MetaBlk.  The initial cluster config contains ONLY this creator — it self-elects as the
    // leader of a 1-member group, and every other member is invited via add_member() below.  add_srv is what
    // makes nuraft send each peer a join_cluster_request, and that join is the only message the RPC listener
    // will create a group for — membership is established solely by explicit invitation, so a straggler
    // append/vote for a destroyed group can never rebuild it.  The server's aux holds the UUID string
    // (matches add_member()'s stamping) so downstream int32-srv_id → ReplicaId reverse lookups uniformly work.
    auto servers = nlohmann::json::array();
    auto const priority = HS_RUNTIME_CONFIG(consensus.default_leader_priority);
    servers.push_back(nlohmann::json{{"id", to_server_id(my_uuid_)},
                                     {"dc_id", 0},
                                     {"endpoint", lookup_peer_addr(my_uuid_)},
                                     {"aux", boost::uuids::to_string(my_uuid_)},
                                     {"learner", false},
                                     {"priority", priority}});
    nlohmann::json raft_cfg_json = {{"group_id", gid_str},
                                    {"config",
                                     {{"log_idx", 0},
                                      {"prev_log_idx", 0},
                                      {"eventual_consistency", false},
                                      {"user_ctx", std::string{}},
                                      {"servers", std::move(servers)}}}};

    auto raft_cfg_mblk =
        co_await MetaBlkWrapper::create(rs_raft_cfg_meta_client_, fmt::format("cfg_{}", gid_meta_tag(group_id)),
                                        /*size=*/{});

    // User-initiated create: options come from the caller arg (not the listener).
    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb_mblk), options);
    rs->attach_listener(std::move(listener));
    if (!co_await rs->load(std::move(raft_cfg_mblk), std::move(raft_cfg_json)) || !co_await rs->start_engine()) {
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

    // A 1-member group elects itself once its election timer fires; add_srv is leader-only, so wait for
    // the BecomeLeader event before inviting anyone.
    auto const elect_timeout = std::chrono::milliseconds{3ul * HS_RUNTIME_CONFIG(consensus.elect_to_high_ms)};
    if (!co_await rs->wait_to_be_leader(elect_timeout)) {
        RM_LOG(ERROR, NO_TRACE_ID, "created group_id={} did not self-elect within {}ms", gid_str,
               elect_timeout.count());
        co_return folly::makeUnexpected(ReplError::TIMEOUT);
    }

    // Invite every other member.  nuraft allows one config change at a time and holds off further changes
    // until the previous joiner has caught up, so retry while it reports CONFIG_CHANGING / SERVER_IS_JOINING;
    // any other error is fatal for the create.  Each add_srv sends the peer a join_cluster_request — the only
    // message its RPC listener will construct the group for.
    for (auto const& member_id : members) {
        if (member_id == my_uuid_) {
            continue;
        }
        ReplicaMemberInfo info{};
        info.id = member_id;
        info.priority = to_int(HS_RUNTIME_CONFIG(consensus.default_leader_priority));
        while (true) {
            auto const res = co_await rs->add_member(info);
            if (res.hasError()) {
                if ((res.error() != ReplError::CONFIG_CHANGING) && (res.error() != ReplError::SERVER_IS_JOINING)) {
                    RM_LOG(ERROR, NO_TRACE_ID, "create: add_member {} to group_id={} failed err={}",
                           boost::uuids::to_string(member_id), gid_str, to_int(res.error()));
                    co_return folly::makeUnexpected(res.error());
                }
                co_await iomgr().sleep(std::chrono::milliseconds{100});
                continue;
            }
            // "Accepted" only means the join invitation was dispatched — the config entry is appended after
            // the joiner confirms, and an invitation that dies in flight (peer restarting mid-handshake) is
            // never retried by the engine.  Confirm the member actually landed in the raft config; re-invite
            // if it didn't (idempotent: SERVER_ALREADY_EXISTS maps to success in add_member).
            constexpr uint32_t confirm_attempts = 30;
            bool in_config = false;
            for (uint32_t i = 0; !in_config && (i < confirm_attempts); ++i) {
                in_config = rs->has_member(member_id);
                if (!in_config) {
                    co_await iomgr().sleep(std::chrono::milliseconds{100});
                }
            }
            if (in_config) {
                break;
            }
            RM_LOG(WARN, NO_TRACE_ID, "create: member {} accepted but absent from raft config — re-inviting",
                   boost::uuids::to_string(member_id));
        }
    }
    co_return rs;
}

Async< nuraft::raft_server* >
ReplicationManager::create_replica_set_on_demand(nuraft::group_id_t const& gid) {
    if (state_.get()->stopping) {
        co_return nullptr; // dispatch answers SERVER_NOT_FOUND; the peer's join retry lands after reboot
    }
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

    // Peer-initiated on-demand: options come from the listener (freshly constructed above).  Same group_id
    // across replicas must yield the same options — divergent allow_user_driven_truncate across replicas
    // — that's the app's contract, we don't enforce.
    auto const options = listener->replica_set_options();

    // Allocate a fresh MetaBlk under the shared "ReplicaSet" client for this group's SB.  Build the
    // initial ReplicaSetSuperBlk in a temporary buffer and persist it through sb_mblk.write().  The tmp
    // dies here; rs->start() will read the SB back into ReplicaSet's own buffer during setup.  Per-group
    // options are NOT stamped into SB — the listener is the source of truth and gets queried again on
    // every restart.
    auto sb_mblk =
        co_await MetaBlkWrapper::create(rs_meta_client_, fmt::format("rs_{}", gid_meta_tag(group_id)),
                                        sizeof(ReplicaSetSuperBlk));
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
        co_await MetaBlkWrapper::create(rs_raft_cfg_meta_client_, fmt::format("cfg_{}", gid_meta_tag(group_id)),
                                        /*size=*/{});
    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb_mblk), options);
    rs->attach_listener(std::move(listener));
    nlohmann::json raft_cfg_json = {{"group_id", gid_str}};
    if (!co_await rs->load(std::move(raft_cfg_mblk), std::move(raft_cfg_json)) || !co_await rs->start_engine()) {
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
    if (state_.get()->stopping) {
        co_return ReplError::STOPPING;
    }
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

    // Restart/reload: options come from the listener (app is authoritative, may change across restarts).
    auto const options = listener->replica_set_options();

    auto rs = std::make_shared< ReplicaSet >(*this, std::move(sb_mblk), options);
    rs->attach_listener(std::move(listener));
    if (!co_await rs->load(std::move(raft_cfg_mblk), std::move(raft_cfg_json))) {
        RM_LOG(ERROR, NO_TRACE_ID, "load failed for reloaded group_id={} — leaving SB + config on disk", gid_str);
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
    bool corrupt = false;
    std::string corrupt_msg;
    try {
        cfg_json = nlohmann::json::from_msgpack(data.cbytes(), data.cbytes() + data.size());
    } catch (std::exception const& e) {
        corrupt = true;
        corrupt_msg = e.what();
    }
    if (corrupt) {
        RM_LOG(ERROR, NO_TRACE_ID, "Corrupt raft config block — destroying: {}", corrupt_msg);
        co_await MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk).destroy();
        co_return;
    }
    if (!cfg_json.contains("group_id")) {
        RM_LOG(ERROR, NO_TRACE_ID, "Raft config block missing group_id — destroying");
        co_await MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk).destroy();
        co_return;
    }
    GroupId group_id;
    bool malformed = false;
    std::string malformed_msg;
    try {
        group_id = boost::uuids::string_generator{}(cfg_json["group_id"].get< std::string >());
    } catch (std::exception const& e) {
        malformed = true;
        malformed_msg = e.what();
    }
    if (malformed) {
        RM_LOG(ERROR, NO_TRACE_ID, "Raft config block has malformed group_id ({}) — destroying", malformed_msg);
        co_await MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk).destroy();
        co_return;
    }
    pending_configs_.emplace(group_id,
                             std::pair{MetaBlkWrapper::load(rs_raft_cfg_meta_client_, blk), std::move(cfg_json)});
    co_return;
}

Async< void > ReplicationManager::gc_replica_sets() {
    // Snapshot the reap candidates under the shared lock — never hold the lock across a co_await.
    auto const grace = std::chrono::seconds{HS_RUNTIME_CONFIG(consensus.replica_set_reaper_grace_sec)};
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

Async< void > ReplicationManager::truncate() {
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
    // Serial: snapshot creation is IO-heavy per group.
    for (auto const& rs : sets) {
        auto* srv = rs->raft_server();
        if (!srv) {
            continue; // engine not yet started (mid-load, before start_engine)
        }
        auto cr = srv->schedule_snapshot_creation();
        if (!cr) {
            continue; // nuraft rejected (snapshot inflight or bad server state)
        }
        nuraft::Baton baton;
        cr->when_ready([&baton](uint64_t&, nuraft::ptr< std::exception >&) { baton.post(); });
        co_await baton.wait();
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

ReplicationManager& repl_service() {
    return repl_mgr();
}

} // namespace homestore