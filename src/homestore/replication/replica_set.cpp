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

#include "homestore/replication/replica_set.h"
#include "homestore/replication/home_raft_log_store.h"
#include "common/async.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <folly/Executor.h>
#include <folly/hash/Hash.h>
#include <folly/io/async/EventBase.h>

#include <libnuraft/cluster_config.hxx>
#include <libnuraft/context.hxx>
#include <libnuraft/delayed_task.hxx>
#include <libnuraft/delayed_task_scheduler.hxx>
#include <libnuraft/logger.hxx>
#include <libnuraft/raft_params.hxx>
#include <libnuraft/raft_server.hxx>
#include <libnuraft/rpc_cli_factory.hxx>
#include <libnuraft/rpc_listener.hxx>
#include <libnuraft/srv_config.hxx>
#include <libnuraft/srv_state.hxx>

#include <nlohmann/json.hpp>

#include "sisl/logging/logging.h"

#include "common/homestore_assert.h"
#include "common/hs_runtime_config.h"
#include "homestore/homestore.h"
#include "homestore/replication/repl_manager.h"
#include "replication/transport/folly_rpc_client_factory.h"
#include "replication/transport/folly_rpc_listener.h"
#include "iomanager/iomanager.h"

SISL_LOGGING_DECL(replication)

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// FollyEventBaseScheduler — minimal nuraft::delayed_task_scheduler adapter that schedules onto a given
// folly::EventBase (an iomgr reactor). cancel_impl is a no-op because folly::EventBase::runAfterDelay does
// not return a handle; nuraft's task->cancel() (called by the base class cancel()) marks the task cancelled,
// so when the timer fires the execute() body sees the cancelled flag and bails out.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace {

// ReplicaSetLogger — implements nuraft::logger by routing into sisl's `replication` log module with the
// group_id prefixed to every line so logs from multiple raft groups in the same process can be told apart.
// nuraft's `put_details` level encoding: 1=critical, 2=err, 3=warn, 4=info, 5=debug, anything else=trace.
class ReplicaSetLogger : public nuraft::logger {
public:
    explicit ReplicaSetLogger(GroupId const& group_id) : group_id_str_{boost::uuids::to_string(group_id)} {}

    void set_level(int /*level*/) override {}

    void put_details(int level, char const* source_file, char const* func_name, size_t line_number,
                     std::string const& log_line) override {
        auto prefix = fmt::format("[{}:{}:{}] [group={}]", source_file, line_number, func_name, group_id_str_);
        switch (level) {
        case 1:
            LOGCRITICALMOD(replication, "{} {}", prefix, log_line);
            break;
        case 2:
            LOGERRORMOD(replication, "{} {}", prefix, log_line);
            break;
        case 3:
            LOGWARNMOD(replication, "{} {}", prefix, log_line);
            break;
        case 4:
            LOGINFOMOD(replication, "{} {}", prefix, log_line);
            break;
        case 5:
            LOGDEBUGMOD(replication, "{} {}", prefix, log_line);
            break;
        default:
            LOGTRACEMOD(replication, "{} {}", prefix, log_line);
            break;
        }
    }

private:
    std::string group_id_str_;
};

class FollyEventBaseScheduler : public nuraft::delayed_task_scheduler {
public:
    explicit FollyEventBaseScheduler(folly::EventBase* eb) : eb_{eb} {}

    void schedule(nuraft::ptr< nuraft::delayed_task >& task, int32 milliseconds) override {
        eb_->runAfterDelay([task]() { task->execute(); }, milliseconds);
    }

private:
    void cancel_impl(nuraft::ptr< nuraft::delayed_task >& /*task*/) override {}

    folly::EventBase* eb_;
};

// ── cluster_config / srv_config JSON serde ─────────────────────────────────────────────────────────────────────

nlohmann::json serialize_server_config(std::list< nuraft::ptr< nuraft::srv_config > > const& server_list) {
    auto servers = nlohmann::json::array();
    for (auto const& server_conf : server_list) {
        if (!server_conf) {
            continue;
        }
        servers.push_back(nlohmann::json{{"id", server_conf->get_id()},
                                         {"dc_id", server_conf->get_dc_id()},
                                         {"endpoint", server_conf->get_endpoint()},
                                         {"aux", server_conf->get_aux()},
                                         {"learner", server_conf->is_learner()},
                                         {"priority", server_conf->get_priority()}});
    }
    return servers;
}

nlohmann::json serialize_cluster_config(nuraft::cluster_config const& config) {
    return nlohmann::json{{"log_idx", config.get_log_idx()},
                          {"prev_log_idx", config.get_prev_log_idx()},
                          {"eventual_consistency", config.is_async_replication()},
                          {"user_ctx", config.get_user_ctx()},
                          {"servers", serialize_server_config(config.get_servers())}};
}

nuraft::ptr< nuraft::srv_config > deserialize_server_config(nlohmann::json const& server) {
    return nuraft::cs_new< nuraft::srv_config >(to_i32(server["id"]), to_i32(server["dc_id"]), server["endpoint"],
                                                server["aux"], server["learner"], to_i32(server["priority"]));
}

nuraft::ptr< nuraft::cluster_config > deserialize_cluster_config(nlohmann::json const& cluster_config) {
    auto raft_config = nuraft::cs_new< nuraft::cluster_config >(
        cluster_config["log_idx"], cluster_config["prev_log_idx"], cluster_config["eventual_consistency"]);
    raft_config->set_user_ctx(cluster_config["user_ctx"]);
    for (auto const& server_json : cluster_config["servers"]) {
        raft_config->get_servers().push_back(deserialize_server_config(server_json));
    }
    return raft_config;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ReplicaSetMetrics — registers the per-group metrics group with MetricsFarm.  No metrics are defined yet;
// this is the scaffolding.  Adds go here when we start instrumenting counters/histograms; MetricsFarm handles
// gather/report.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ReplicaSetMetrics::ReplicaSetMetrics(char const* inst_name) : sisl::MetricsGroup("ReplicaSet", inst_name) {
    register_me_to_farm();
}

ReplicaSetMetrics::~ReplicaSetMetrics() {
    deregister_me_from_farm();
}

nuraft::ptr< nuraft::buffer > ReplicaSet::build_log_entry(JournalType type, sisl::Blob const& user_header,
                                                          uint32_t value_size) {
    auto const uh_size = to_u32(user_header.size());
    auto slab = nuraft::buffer::alloc(sizeof(ReplLogHeader) + uh_size);
    auto* h = new (slab->data_begin()) ReplLogHeader{};
    h->code = to_u8(type);
    h->major_version = ReplLogHeader::kMajor;
    h->minor_version = ReplLogHeader::kMinor;
    h->value_size = value_size;
    h->user_header_size_ = uh_size;
    h->commit_lsn_at_write = commit_upto_lsn_.load();
    if (uh_size > 0) {
        std::memcpy(slab->data_begin() + sizeof(ReplLogHeader), user_header.cbytes(), uh_size);
    }
    return slab;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ReplicaSet::ReplicaSet(ReplicationManager& mgr, MetaBlkWrapper sb_mblk, ReplicaSetOptions const& options) :
        // Minimal ctor — SB-independent members only.  start() reads sb_mblk_ and populates the rest
        // (sb_buffer_, group_id_, rset_name_, identify_str_, metrics_, commit_upto_lsn_).
        mgr_{mgr},
        my_uuid_{mgr.get_my_repl_id()},
        raft_server_id_{to_server_id(my_uuid_)},
        sb_mblk_{std::move(sb_mblk)},
        options_{options} {
    // Single 4-byte sentinel handed back on every pre-commit/commit — nuraft treats the buffer as opaque here.
    success_ptr_ = nuraft::buffer::alloc(sizeof(int));
    success_ptr_->put(0);
}

ReplicaSet::~ReplicaSet() = default;

///////////////////////////////////// Core Public API Section ////////////////////////////////////////////////

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// write — single client entry point.  Replicates the user's (header, value) through raft.  Resolves after
// the entry is committed across the quorum (the listener's on_commit has already fired), or with a
// ReplError if replication couldn't complete (not leader, alloc failure, no quorum, etc).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
Async< ReplResult<> > ReplicaSet::write(sisl::IoBuf const& user_header, sisl::IoBufView value, TraceId tid) {
    (void)tid;

    // Value buf — zero-copy via IoBufView::extract (IoBufShared) + buffer::take_ownership.  The captured
    // IoBufShared keeps the value bytes alive until folly drops the nuraft::buffer (post wire send / disk
    // write / blob_stream write).
    auto ba = value.extract();
    auto value_buf =
        nuraft::buffer::take_ownership(ba->bytes(), ba->size(), [held = ba](nuraft::byte*) noexcept { (void)held; });

    nuraft::log_entry_chain chain;
    chain.push_back(build_log_entry(JournalType::HS_DATA_INLINE,
                                    sisl::Blob{user_header.cbytes(), to_u32(user_header.size())},
                                    to_u32(value.size())));
    chain.push_back(std::move(value_buf));

    std::vector< nuraft::log_entry_chain > chains;
    chains.push_back(std::move(chain));

    auto result = co_await raft_server_->append_entries_chained(chains);
    if (!result.accepted || !result.committed) {
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(result.code));
    }
    co_return ReplResult<>{};
}

Async< void > ReplicaSet::free_indirect_blk(BlkId const& blkid, raft_lsn_t referenced_lsn) {
    // Consumer-visible entry point for releasing an indirect BlkId surfaced by on_commit's blob_refs.
    // The actual free is deferred until the raft log entry at `referenced_lsn` is truncated past the
    // retention horizon — see HomeRaftLogStore::deferred_free for the truncation/GC contract.  Inline
    // entries (empty blob_refs at commit time) never reach this call.
    if (!log_store_) {
        co_return;
    }
    co_await log_store_->deferred_free(blkid, referenced_lsn);
}

Async< ReplResult<> > ReplicaSet::add_member(ReplicaMemberInfo const& member, bool learner, TraceId tid) {
    if (!raft_server_ || !raft_server_->is_leader()) {
        RS_LOG(WARN, tid, "add_member rejected — not leader, member={}", boost::uuids::to_string(member.id));
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }

    auto const srv_id = to_server_id(member.id);
    auto endpoint = mgr_.lookup_peer_addr(member.id);
    if (endpoint.empty()) {
        RS_LOG(ERROR, tid, "add_member: unknown peer endpoint for {}", boost::uuids::to_string(member.id));
        co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
    }

    // srv_config: dc_id=0 (multi-DC not modeled here); aux carries the peer UUID as a string so
    // get_leader_id / get_replication_status can reverse the int32 srv_id nuraft hands back into a
    // ReplicaId.  learner is caller-controlled so replace_member can stage the new node in without
    // quorum impact.  member.name stays in the CTRL header payload for observability but doesn't hit
    // srv_config.
    nuraft::srv_config srv_cfg(srv_id, /*dc_id=*/0, endpoint, /*aux=*/boost::uuids::to_string(member.id), learner,
                               member.priority);

    auto result = co_await raft_server_->add_srv(srv_cfg);
    if (!result || !result->get_accepted()) {
        auto const code = result ? result->get_result_code() : nuraft::cmd_result_code::CANCELLED;
        if (code == nuraft::cmd_result_code::SERVER_ALREADY_EXISTS) {
            RS_LOG(INFO, tid, "add_member: {} already in cluster — treating as ok", boost::uuids::to_string(member.id));
            co_return ReplResult<>{};
        }
        RS_LOG(ERROR, tid, "add_member failed member={} learner={} code={}", boost::uuids::to_string(member.id),
               learner, to_int(code));
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(code));
    }
    RS_LOG(INFO, tid, "add_member accepted for {} learner={}", boost::uuids::to_string(member.id), learner);
    co_return ReplResult<>{};
}

Async< ReplResult<> > ReplicaSet::remove_member(ReplicaMemberInfo const& member, TraceId tid) {
    if (!raft_server_ || !raft_server_->is_leader()) {
        RS_LOG(WARN, tid, "remove_member rejected — not leader, member={}", boost::uuids::to_string(member.id));
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }

    // Self-removal special case: if the leader is being asked to remove itself, yield leadership so a peer
    // takes over, then bounce the caller with NOT_LEADER to retry against the successor.  Otherwise the
    // remove_srv commit would race with our own shutdown.
    if (member.id == my_uuid_) {
        RS_LOG(INFO, tid, "remove_member: I am the target — yielding leadership");
        co_await raft_server_->yield_leadership(/*immediate_yield=*/false, /*successor=*/-1);
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }

    auto const srv_id = to_server_id(member.id);
    auto result = co_await raft_server_->remove_srv(srv_id);
    if (!result || !result->get_accepted()) {
        auto const code = result ? result->get_result_code() : nuraft::cmd_result_code::CANCELLED;
        if (code == nuraft::cmd_result_code::SERVER_NOT_FOUND) {
            RS_LOG(INFO, tid, "remove_member: {} not in cluster — treating as ok", boost::uuids::to_string(member.id));
            co_return ReplResult<>{};
        }
        RS_LOG(ERROR, tid, "remove_member failed member={} code={}", boost::uuids::to_string(member.id), to_int(code));
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(code));
    }
    RS_LOG(INFO, tid, "remove_member accepted for {}", boost::uuids::to_string(member.id));
    co_return ReplResult<>{};
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// replace_member — single-shot orchestrator.  Caller sees one call; internally: propose START_REPLACE ctrl
// log → add member_in as learner → wait for catch-up → promote learner to voter → remove member_out →
// propose COMPLETE_REPLACE ctrl log.  Both ctrl commits fire the listener on every replica so applications
// can persist the ongoing / completed swap.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
Async< ReplResult<> > ReplicaSet::replace_member(ReplicaMemberInfo const& member_out,
                                                 ReplicaMemberInfo const& member_in, TraceId tid) {
    if (!raft_server_ || !raft_server_->is_leader()) {
        RS_LOG(WARN, tid, "replace_member rejected — not leader, out={} in={}", boost::uuids::to_string(member_out.id),
               boost::uuids::to_string(member_in.id));
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }
    // Fresh attempt — generate a new task_id and drive the whole sequence from kProposeStart.
    auto const task_id = boost::uuids::to_string(boost::uuids::random_generator{}());
    RS_LOG(INFO, tid, "replace_member: out={} in={} task_id={}", boost::uuids::to_string(member_out.id),
           boost::uuids::to_string(member_in.id), task_id);
    co_return co_await do_replace_member(member_out, member_in, task_id, ReplaceStage::kProposeStart, tid);
}

Async< ReplResult< int64_t > > ReplicaSet::advance_truncate_upto(lsn_t lsn, TraceId tid) {
    if (!options_.allow_user_driven_truncate) {
        RS_LOG(WARN, tid, "advance_truncate_upto rejected — option disabled for this group");
        co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
    }
    if (!raft_server_ || !raft_server_->is_leader()) {
        RS_LOG(WARN, tid, "advance_truncate_upto rejected — not leader");
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }
    if (lsn <= app_truncate_upto_.load(std::memory_order_acquire)) {
        RS_LOG(WARN, tid, "advance_truncate_upto rejected — lsn={} <= current watermark={}", lsn,
               app_truncate_upto_.load(std::memory_order_acquire));
        co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
    }

    // Payload = raw int64_t bytes.  dispatch_commit's HS_CTRL_TRUNCATE case reads it out on every replica
    // and advances app_truncate_upto_ + SB.
    int64_t const target = lsn;
    sisl::Blob const payload{to_cu8ptr(&target), to_u32(sizeof(target))};
    std::vector< nuraft::ptr< nuraft::buffer > > logs;
    logs.push_back(build_log_entry(JournalType::HS_CTRL_TRUNCATE, payload, /*value_size=*/0));
    auto const result = co_await raft_server_->append_entries(logs);
    if (!result.accepted || !result.committed) {
        RS_LOG(ERROR, tid, "advance_truncate_upto failed accepted={} committed={} code={}", result.accepted,
               result.committed, to_int(result.code));
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(result.code));
    }
    co_return target;
}

Async< ReplResult<> > ReplicaSet::flip_learner_flag(ReplicaMemberInfo const& member, bool target, TraceId tid) {
    if (!raft_server_ || !raft_server_->is_leader()) {
        RS_LOG(WARN, tid, "flip_learner_flag rejected — not leader, member={}", boost::uuids::to_string(member.id));
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }
    auto const srv_id = to_server_id(member.id);
    auto result = co_await raft_server_->flip_learner_flag(srv_id, target);
    if (!result || !result->get_accepted()) {
        auto const code = result ? result->get_result_code() : nuraft::cmd_result_code::CANCELLED;
        RS_LOG(ERROR, tid, "flip_learner_flag failed member={} target={} code={}", boost::uuids::to_string(member.id),
               target, to_int(code));
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(code));
    }
    RS_LOG(INFO, tid, "flip_learner_flag accepted member={} target={}", boost::uuids::to_string(member.id), target);
    co_return ReplResult<>{};
}

Async< ReplResult<> > ReplicaSet::become_leader(TraceId tid) {
    if (!raft_server_) {
        RS_LOG(WARN, tid, "become_leader rejected — raft_server not initialized");
        co_return folly::makeUnexpected(ReplError::FAILED);
    }

    if (raft_server_->is_leader()) {
        RS_LOG(INFO, tid, "become_leader: already leader — no-op");
        co_return ReplResult<>{};
    }

    // request_leadership sends a transfer request to the current leader; returns true only if the request was
    // accepted for delivery.  Actual promotion is asynchronous and not guaranteed — callers watch is_leader().
    auto const accepted = co_await raft_server_->request_leadership();
    if (!accepted) {
        RS_LOG(ERROR, tid, "become_leader: request_leadership rejected");
        co_return folly::makeUnexpected(ReplError::FAILED);
    }
    RS_LOG(INFO, tid, "become_leader: request accepted");
    co_return ReplResult<>{};
}

Async< ReplResult<> > ReplicaSet::set_priority(ReplicaId const& member, int32_t priority, TraceId tid) {
    if (!raft_server_) {
        RS_LOG(WARN, tid, "set_priority rejected — raft_server not initialized");
        co_return folly::makeUnexpected(ReplError::FAILED);
    }
    auto const srv_id = to_server_id(member);

    // broadcast_when_leader_exists=false — only leader can commit; non-leader ignores and returns IGNORED
    // which we surface as NOT_LEADER so the caller retries against the leader.
    auto const result = co_await raft_server_->set_priority(srv_id, priority, /*broadcast_when_leader_exists=*/false);
    switch (result) {
    case nuraft::raft_server::PrioritySetResult::SET:
    case nuraft::raft_server::PrioritySetResult::BROADCAST:
        RS_LOG(INFO, tid, "set_priority member={} priority={} result={}", boost::uuids::to_string(member), priority,
               to_int(result));
        co_return ReplResult<>{};
    case nuraft::raft_server::PrioritySetResult::IGNORED:
        RS_LOG(WARN, tid, "set_priority member={} priority={} ignored — not leader", boost::uuids::to_string(member),
               priority);
        co_return folly::makeUnexpected(ReplError::NOT_LEADER);
    }
    co_return folly::makeUnexpected(ReplError::FAILED);
}

bool ReplicaSet::is_leader() const {
    return raft_server_ && raft_server_->is_leader();
}

bool ReplicaSet::is_ready_for_traffic() const {
    return commit_upto_lsn_.load() >= traffic_ready_lsn_.load();
}

bool ReplicaSet::is_destroy_pending() const {
    return *stage_.access().get() == ReplicaSetStage::DESTROYED;
}

bool ReplicaSet::is_destroyed() const {
    return *stage_.access().get() == ReplicaSetStage::PERMANENT_DESTROYED;
}

nuraft::raft_server* ReplicaSet::raft_server() {
    return raft_server_.get();
}

ReplicaId ReplicaSet::get_leader_id() const {
    if (!raft_server_) {
        return boost::uuids::nil_uuid();
    }
    auto const leader_srv_id = raft_server_->get_leader();
    if (leader_srv_id < 0) {
        return boost::uuids::nil_uuid();
    }
    // Reverse int32 srv_id → ReplicaId via cluster_config.  Every srv_config we stamp (add_member's peer
    // add and load_config's bootstrap) carries the UUID string in `aux`; if the leader is us we get the
    // same answer from my_uuid_ directly, but going through the config keeps a single code path.
    auto config = raft_server_->get_config();
    for (auto const& srv : config->get_servers()) {
        if (srv->get_id() == leader_srv_id) {
            try {
                return boost::uuids::string_generator{}(srv->get_aux());
            } catch (...) { return boost::uuids::nil_uuid(); }
        }
    }
    return boost::uuids::nil_uuid();
}

Async< std::vector< PeerInfo > > ReplicaSet::get_replication_status() const {
    std::vector< PeerInfo > out;
    if (!raft_server_) {
        co_return out;
    }
    // Live per-peer view from the raft engine (last_log_idx, last_succ_resp_us) — only meaningful on a
    // leader; on a follower most fields are zero.  Cross-reference cluster_config for priority + learner
    // status and the UUID string tucked into aux.
    auto peer_infos = co_await raft_server_->get_peer_info_all();
    auto config = raft_server_->get_config();
    out.reserve(peer_infos.size());
    for (auto const& pi : peer_infos) {
        PeerInfo entry{};
        entry.replication_idx_ = pi.last_log_idx_;
        entry.last_succ_resp_us_ = pi.last_succ_resp_us_;
        for (auto const& srv : config->get_servers()) {
            if (srv->get_id() == pi.id_) {
                try {
                    entry.id_ = boost::uuids::string_generator{}(srv->get_aux());
                } catch (...) { entry.id_ = boost::uuids::nil_uuid(); }
                entry.priority_ = srv->get_priority();
                entry.can_vote = !srv->is_learner();
                break;
            }
        }
        out.push_back(std::move(entry));
    }
    co_return out;
}

Async< std::set< ReplicaId > > ReplicaSet::get_active_peers() const {
    std::set< ReplicaId > active;
    auto const my_lsn = commit_upto_lsn_.load();
    auto const laggy_threshold = HS_RUNTIME_CONFIG(consensus.laggy_threshold);
    auto peer_status = co_await get_replication_status();
    for (auto const& p : peer_status) {
        if (p.id_ == my_uuid_ || p.id_.is_nil()) {
            continue;
        }
        auto const peer_lsn = to_i64(p.replication_idx_);
        // "Active" = peer's replicated log index is within laggy_threshold of my commit watermark.
        if (peer_lsn + laggy_threshold >= my_lsn) {
            active.insert(p.id_);
        }
    }
    co_return active;
}

/////////////////////////////// ReplicationManager Interaction Section /////////////////////////////////////////

Async< bool > ReplicaSet::load(MetaBlkWrapper raft_cfg_mblk, nlohmann::json raft_cfg_json) {
    // Read the SB payload out of sb_mblk_ and populate the identity fields the ctor couldn't touch (it
    // can't co_await).  The returned IoBufView already owns/shares its backing buffer — inline or overflow
    // — so no allocation or copy on our side; we just hold the view.  From here on sb() is valid and
    // every downstream setup step can rely on group_id_ / identify_str_ / metrics_.
    {
        sb_buffer_ = co_await sb_mblk_.read();
        group_id_ = sb()->group_id;
        rset_name_ = sb()->rset_name;
        identify_str_ = rset_name_ + ":" + boost::uuids::to_string(group_id_);
        metrics_ = std::make_unique< ReplicaSetMetrics >(identify_str_.c_str());
        // Seed the running commit watermark from what was persisted on the last persist_commit_lsn.  Log-store
        // replay below advances it further via on_log_found from each replayed entry's commit_lsn_at_write.
        commit_upto_lsn_.store(sb()->commit_lsn);
        bool const fresh = (sb()->raft_log_store_id == UINT32_MAX);
        RS_LOG(INFO, NO_TRACE_ID, "Starting {} ReplicaSet replica_id={}, raft_server_id={}, commit_lsn_from_sb={}",
               (fresh ? "Fresh" : "Reloaded"), my_replica_id_str(), raft_server_id_, sb()->commit_lsn);
    }

    // Take ownership of the group's raft config (both the MetaBlk handle and the in-memory json body).  Held
    // here for the state_mgr overrides (load_config / save_config / read_state / save_state) to marshal
    // into/out of.  Guarded by config_mtx_ because save/load run on the state-machine driver while this
    // initial assignment is on the reactor.
    {
        auto lg = co_await config_mtx_.co_scoped_lock();
        raft_cfg_mblk_ = std::move(raft_cfg_mblk);
        raft_cfg_json_ = std::move(raft_cfg_json);

        // Seed committed_members_ from the persisted cluster config (aux carries each server's UUID).
        // Fresh on-demand groups arrive with just {"group_id": gid} and no "config" — leave the set empty;
        // the first commit_config after nuraft bootstraps the config will populate it and fire
        // on_membership_change against the empty baseline (all-added).
        if (raft_cfg_json_.contains("config") && raft_cfg_json_["config"].contains("servers")) {
            for (auto const& srv : raft_cfg_json_["config"]["servers"]) {
                if (srv.contains("aux")) {
                    try {
                        committed_members_.insert(boost::uuids::string_generator{}(srv["aux"].get< std::string >()));
                    } catch (std::exception const&) {
                        // Malformed aux — skip; won't survive as a peer identity anywhere else either.
                    }
                }
            }
        }
    }

    // Log store setup — must be ready before raft_server->start_server so nuraft's load_log_store override
    // can hand it out to the raft machinery.  Fresh vs existing is determined by the SB itself: sentinel
    // raft_log_store_id means fresh (create); a real id means recovery loaded an existing SB (load).  The
    // callback fires once per replayed entry during LogStoreManager::recover() — reads the ReplLogHeader
    // off the front and folds commit_lsn_at_write into commit_upto_lsn_ (single-threaded replay so plain
    // load-then-store is enough;)
    auto blob_stream = listener_ ? listener_->blob_stream() : nullptr;

    // Replay callback for LogStoreManager::replay().  Runs single-threaded per entry, in order, awaited by
    // the recover walker.  For every replayed entry:
    //   1. Fold ReplLogHeader.commit_lsn_at_write back into commit_upto_lsn_ (fine-grained watermark advance).
    //   2. For entries in (previous_commit, sb.commit_lsn]: re-drive dispatch_commit so the consumer's
    //      on_commit fires again for LSNs raft considered durably committed pre-crash.  Consumer must be
    //      idempotent — replay may re-fire on_commit for entries already applied durably in a prior boot.
    //   3. Entries > sb.commit_lsn were not durably committed pre-crash; nuraft's commit_ext will fire
    //      on_commit for them once the raft_server starts.
    auto on_log_found_cb = [this](lsn_t entry_lsn, sisl::IoBufView const& bv) -> Async< void > {
        if (bv.size() < sizeof(ReplLogHeader)) {
            co_return;
        }
        auto const* h = r_cast< ReplLogHeader const* >(bv.cbytes());
        if (h->commit_lsn_at_write > commit_upto_lsn_.load(std::memory_order_relaxed)) {
            commit_upto_lsn_.store(h->commit_lsn_at_write, std::memory_order_relaxed);
        }
        if (entry_lsn > sb()->commit_lsn) {
            co_return;
        }
        auto* payload = const_cast< uint8_t* >(bv.cbytes()) + sizeof(ReplLogHeader);
        sisl::Blob const user_header{payload, h->user_header_size_};
        sisl::Blob const value{payload + h->user_header_size_, h->value_size};
        co_await dispatch_commit(entry_lsn, s_cast< JournalType >(h->code), user_header, value);
    };
    // Compact ceiling callback — returns app_truncate_upto_ when the option is enabled, max() otherwise
    // (unclamped: HomeRaftLogStore forwards nuraft's ask as-is).
    HomeRaftLogStore::TruncateCeilingFn truncate_ceiling_cb = [this]() -> raft_lsn_t {
        return options_.allow_user_driven_truncate ? app_truncate_upto_.load(std::memory_order_acquire)
                                                  : std::numeric_limits< raft_lsn_t >::max();
    };
    if (sb()->raft_log_store_id == UINT32_MAX) {
        log_store_ = co_await HomeRaftLogStore::create(*sb(), std::move(blob_stream), std::move(on_log_found_cb),
                                                       std::move(truncate_ceiling_cb));
        co_await write_sb(); // persist newly-allocated log_store ids
    } else {
        log_store_ = co_await HomeRaftLogStore::load(*sb(), std::move(blob_stream), std::move(on_log_found_cb),
                                                     std::move(truncate_ceiling_cb));
    }

    // Seed app_truncate_upto_ to the log's current first_lsn.  Any compact that already occurred pre-restart
    // implies the app authorized compact at least up to log_store_->start_index() - 1, so this is a safe
    // lower bound that avoids re-clamping HomeRaftLogStore::compact to 0 until the app calls
    // advance_truncate_upto again.
    app_truncate_upto_.store(to_i64(log_store_->start_index()));

    // Log store is open and the replay handler is attached; the raft engine is brought up later by
    // start_engine(), after LogStoreManager::replay() has restored commit_upto_lsn to its pre-crash value.
    co_return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// start_engine — build and start the nuraft consensus engine.  Must run once the log store is in its final
// state: on recovery, after LogStoreManager::replay() restored the tail / commit index; on a fresh create,
// right after load() (nothing to replay).  Either way start_server then observes the correct tail.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
Async< bool > ReplicaSet::start_engine() {
    // Pin all this raft_server's coro work to one reactor — hash by group_id so each group sticks to one.
    auto& iom = iomanager::iomgr();
    size_t reactor_id = folly::hash::fnv64_buf(group_id_.data, sizeof(group_id_.data)) % iom.num_reactors();
    auto* eb = iom.reactor_for(reactor_id);

    // Carry the boost::uuids::uuid GroupId across to nuraft as raw 16 bytes (nuraft::group_id_t is a
    // std::array<uint8_t,16>). The wire frame's group_id field uses the same encoding.
    nuraft::group_id_t gid{};
    std::memcpy(gid.data(), group_id_.data, sizeof(gid));

    // Snapshot cadence: nuraft auto-fires snapshot_and_compact every N commits (0 disables — only
    // explicit schedule_snapshot_creation triggers).  Log-entries floor: nuraft's on_snapshot_completed
    // uses this to compute compact_upto = snap_lsn - reserved.  Both fed from ReplicaSetOptions.
    nuraft::raft_params params;
    params.with_snapshot_enabled(to_int(options_.snapshot_distance))
          .with_reserved_log_items(to_int(options_.preserve_log_count));

    // ReplicaSet inherits from both state_mgr and state_machine so both slots in the context are `this`.
    auto self = shared_from_this();
    auto* ctx = new nuraft::context(std::static_pointer_cast< nuraft::state_mgr >(self),
                                    std::static_pointer_cast< nuraft::state_machine >(self),
                                    std::static_pointer_cast< nuraft::rpc_listener >(mgr_.rpc_listener()),
                                    std::make_shared< ReplicaSetLogger >(group_id_),
                                    std::static_pointer_cast< nuraft::rpc_client_factory >(mgr_.rpc_client_factory()),
                                    std::make_shared< FollyEventBaseScheduler >(eb), params,
                                    nullptr, // custom_global_mgr
                                    gid);

    // `this` is safe raw: ctx above already stores two shared_ptr<ReplicaSet> aliases (as state_mgr and
    // state_machine), so nuraft transitively keeps this object alive for as long as raft_server_ is alive —
    // which is exactly the window in which raft_event can fire.  weak_ptr::lock() would cost two atomic ops
    // per event on the raft hot path for a race that can't actually happen.
    ctx->set_cb_func([this](nuraft::cb_func::Type t, nuraft::cb_func::Param* p) { return raft_event(t, p); });

    nuraft::raft_server::init_options opts;
    opts.main_executor_ = folly::Executor::getKeepAliveToken(eb);
    opts.start_server_in_constructor_ = false;

    raft_server_ = std::make_shared< nuraft::raft_server >(ctx, opts);
    if (!raft_server_) {
        co_return false;
    }
    co_await raft_server_->start_server(/*skip_initial_election_timeout=*/false);

    RS_LOG(INFO, NO_TRACE_ID, "Joined raft group on reactor={}", reactor_id);
    co_return true;
}

Async< ReplError > ReplicaSet::destroy() {
    stage_.update([](auto* s) { *s = ReplicaSetStage::DESTROYING; });

    // Single-buffer entry — no user_header, no value.  dispatch_commit's CTRL_DESTROY branch keys off the
    // code byte alone and runs start_destroy_local() on every replica.
    std::vector< nuraft::ptr< nuraft::buffer > > logs;
    logs.push_back(build_log_entry(JournalType::HS_CTRL_DESTROY, sisl::Blob{}, /*value_size=*/0));

    auto result = co_await raft_server_->append_entries(logs);
    if (!result.accepted || !result.committed) {
        // Propose failed or lost quorum before commit — roll the stage back so the caller can retry.
        stage_.update([](auto* s) { *s = ReplicaSetStage::ACTIVE; });
        RS_LOG(ERROR, NO_TRACE_ID, "destroy failed accepted={} committed={} code={}", result.accepted, result.committed,
               to_int(result.code));
        co_return ReplicationManager::to_repl_error(result.code);
    }
    // On success, start_destroy_local() has already run locally as part of dispatch_commit's CTRL_DESTROY branch.
    RS_LOG(INFO, NO_TRACE_ID, "destroy committed");
    co_return ReplError::OK;
}

Async< void > ReplicaSet::start_destroy_local() {
    // Idempotent — once destroy_pending is set, subsequent calls are no-ops.  Covers the case where a CTRL
    // op replays during recovery.
    if (is_destroy_pending()) {
        co_return;
    }
    stage_.update([](auto* s) { *s = ReplicaSetStage::DESTROYED; });
    destroyed_time_ = Clock::now();
    sb()->destroy_pending = 1;
    co_await write_sb();
    RS_LOG(INFO, NO_TRACE_ID, "ReplicaSet marked DESTROYED, awaiting reaper for finish_destroy_local");
}

Async< void > ReplicaSet::finish_destroy_local() {
    // Idempotent — the reaper may call twice (e.g. recovery replay after a crash between start_destroy_local and
    // this teardown), and we may also see a stray call if the caller doesn't guard against it.
    if (is_destroyed()) {
        co_return;
    }
    RS_LOG(INFO, NO_TRACE_ID, "finish_destroy_local starting");

    // Let the listener free its per-group state first — app owns anything it built off on_commit, and it may
    // need to consult members / raft_server before we tear them down.
    if (listener_) {
        listener_->on_destroy(group_id_);
    }

    // Order: JSON raft config → log store (which handles main log + IndirectBlkHandler internally) → mark
    // stage → rs_sb LAST.  Freeing rs_sb last leaves a discoverable stale SB for crash recovery to finish
    // teardown on next boot.
    co_await raft_cfg_mblk_.destroy();
    if (log_store_) {
        co_await log_store_->destroy();
    }
    stage_.update([](auto* s) { *s = ReplicaSetStage::PERMANENT_DESTROYED; });
    co_await sb_mblk_.destroy();

    RS_LOG(INFO, NO_TRACE_ID, "finish_destroy_local complete");
}

Async< void > ReplicaSet::persist_commit_lsn() {
    // Called by ReplicationManager's low-frequency periodic timer.  Skip if destroyed — the SB is (or will be)
    // torn down and writing would race.
    if (is_destroyed()) {
        co_return;
    }
    auto const lsn = commit_upto_lsn_.load();
    if (lsn == last_flushed_commit_lsn_) {
        // No new commits since last persist — save the SB write.
        co_return;
    }
    sb()->commit_lsn = lsn;
    co_await write_sb();
    last_flushed_commit_lsn_ = lsn;
    RS_LOG(TRACE, NO_TRACE_ID, "persist_commit_lsn — SB.commit_lsn advanced to {}", lsn);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// nuraft::state_mgr overrides
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< nuraft::ptr< nuraft::cluster_config > > ReplicaSet::load_config() {
    auto lg = co_await config_mtx_.co_scoped_lock();
    auto& js = raft_cfg_json_;
    if (!js.contains("config")) {
        // Bootstrap: single-member config with myself.  endpoint = physical address (from the manager's
        // peer table); aux = my UUID string, matching what add_member() stamps for peers — every downstream
        // path that reverse-maps int32 srv_id → ReplicaId reads srv_config.aux uniformly.
        auto cluster_conf = nuraft::cs_new< nuraft::cluster_config >();
        cluster_conf->get_servers().push_back(nuraft::cs_new< nuraft::srv_config >(
            raft_server_id_, 0, mgr_.lookup_peer_addr(my_uuid_),
            /*aux=*/my_replica_id_str(), false, HS_RUNTIME_CONFIG(consensus.default_leader_priority)));
        js["config"] = serialize_cluster_config(*cluster_conf);
    }
    co_return deserialize_cluster_config(js["config"]);
}

Async< void > ReplicaSet::save_config(nuraft::cluster_config const& config) {
    auto lg = co_await config_mtx_.co_scoped_lock();
    raft_cfg_json_["config"] = serialize_cluster_config(config);
    auto const pack = nlohmann::json::to_msgpack(raft_cfg_json_);
    co_await raft_cfg_mblk_.write(pack.data(), pack.size());
}

Async< void > ReplicaSet::save_state(nuraft::srv_state const& state) {
    auto lg = co_await config_mtx_.co_scoped_lock();
    raft_cfg_json_["state"] = nlohmann::json{{"term", state.get_term()},
                                             {"voted_for", state.get_voted_for()},
                                             {"election_timer_allowed", state.is_election_timer_allowed()},
                                             {"catching_up", state.is_catching_up()}};
    auto const pack = nlohmann::json::to_msgpack(raft_cfg_json_);
    co_await raft_cfg_mblk_.write(pack.data(), pack.size());
}

Async< nuraft::ptr< nuraft::srv_state > > ReplicaSet::read_state() {
    auto lg = co_await config_mtx_.co_scoped_lock();
    auto& js = raft_cfg_json_;
    auto state = nuraft::cs_new< nuraft::srv_state >();
    if (!js.contains("state") || js["state"].empty()) {
        js["state"] = nlohmann::json{{"term", state->get_term()},
                                     {"voted_for", state->get_voted_for()},
                                     {"election_timer_allowed", state->is_election_timer_allowed()},
                                     {"catching_up", state->is_catching_up()}};
    } else {
        try {
            state->set_term(to_u64(js["state"]["term"]));
            state->set_voted_for(to_int(js["state"]["voted_for"]));
            state->allow_election_timer(to_bool(js["state"]["election_timer_allowed"]));
            state->set_catching_up(to_bool(js["state"]["catching_up"]));
        } catch (std::out_of_range const&) { RS_LOG(WARN, NO_TRACE_ID, "Persisted state not in the expected format"); }
    }
    co_return state;
}

nuraft::ptr< nuraft::log_store > ReplicaSet::load_log_store() {
    return log_store_;
}

int32_t ReplicaSet::server_id() {
    return raft_server_id_;
}

void ReplicaSet::system_exit(int exit_code) {
    RS_LOG(INFO, NO_TRACE_ID, "System exit signal received exit_code={}", exit_code);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// nuraft::state_machine overrides — snapshot lifecycle
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Marshals ReplSnapshot / ReplSnapshot::Builder to/from the nuraft API surface:
//   • create_snapshot (leader)    → spawn coro → listener->take_snapshot → advance sb()->last_snapshot_lsn
//                                    → fire nuraft's when_done handler.
//   • save_logical_snp_obj (fwr)  → on is_first, abort any stale Builder then build a fresh one via
//                                    listener->build_snapshot.  Each call: zero-copy IoBufView over
//                                    nuraft's ptr<buffer>, co_await builder->write_chunk.  is_last_obj
//                                    ignored — finalize is state_machine::apply_snapshot's job.
//   • apply_snapshot (follower)   → finalize the cached Builder, hand the resulting ReplSnapshot to
//                                    listener->apply_snapshot, advance commit_upto_lsn_ / sb()->commit_lsn
//                                    / sb()->last_snapshot_lsn (max-guarded), write SB, trigger CP flush
//                                    (Snapshot reason).  Release_snapshot on both success and rejection.
//   • read_logical_snp_obj (ldr)  → assert s->last_log_idx <= commit_upto (corruption if not); box
//                                    shared<ReplSnapshot> from listener->last_snapshot() into user_snp_ctx
//                                    on first call; each call co_awaits snap->read_chunk.
//   • free_user_snp_ctx           → listener->release_snapshot(*boxed) then delete the box.  Fires from
//                                    nuraft on both success (peer acked last chunk) and every failure /
//                                    timeout path — always the end-of-transfer signal on the leader.
//   • last_snapshot               → return listener->last_snapshot()->nuraft_snapshot_ (or nullptr).

void ReplicaSet::create_snapshot(RaftSnapshotPtr const& s, nuraft::async_result< bool >::handler_type& when_done) {
    auto const lsn = s_cast< lsn_t >(s->get_last_log_idx());
    RS_LOG(INFO, NO_TRACE_ID, "create_snapshot lsn={}", lsn);

    iomgr().spawn_detached(
        iomanager::ReactorTarget::current(), [this, lsn, s, cb = when_done]() mutable -> Async< void > {
            auto exp = std::shared_ptr< std::exception >();
            if (!listener_) {
                if (cb) {
                    cb(/*ok=*/false, exp);
                }
                co_return;
            }

            auto res = co_await listener_->take_snapshot(lsn);
            if (res.hasError()) {
                RS_LOG(ERROR, NO_TRACE_ID, "take_snapshot lsn={} failed", lsn);
                if (cb) {
                    cb(/*ok=*/false, exp);
                }
                co_return;
            }

            auto new_snap = res.value();
            new_snap->nuraft_snapshot_ = s;

            // Advance the snapshot watermark with max-guard.  commit_lsn is owned by persist_commit_lsn's
            // periodic timer; not written here to avoid racing.
            sb()->last_snapshot_lsn = std::max(sb()->last_snapshot_lsn, lsn);
            co_await write_sb();

            if (cb) {
                cb(/*ok=*/true, exp);
            }
        });
}

Async< bool > ReplicaSet::apply_snapshot(RaftSnapshotPtr const& s) {
    auto const lsn = s_cast< lsn_t >(s->get_last_log_idx());
    RS_LOG(INFO, NO_TRACE_ID, "apply_snapshot lsn={}", lsn);

    if (!listener_ || !incoming_snapshot_builder_) {
        RS_LOG(ERROR, NO_TRACE_ID, "apply_snapshot lsn={} — no cached builder", lsn);
        co_return false;
    }

    auto snap = incoming_snapshot_builder_->finalize();
    incoming_snapshot_builder_.reset();
    snap->nuraft_snapshot_ = s;

    bool const applied = listener_->apply_snapshot(snap);
    if (!applied) {
        RS_LOG(ERROR, NO_TRACE_ID, "apply_snapshot lsn={} — listener rejected", lsn);
        // Repl has no further use for this snap regardless of apply outcome — signal release so the app
        // can drop the finalized object it just rejected (or keep it for other purposes).
        listener_->release_snapshot(snap);
        co_return false;
    }

    // Snapshot receipt is a durability event on the follower: commit and snapshot watermarks jump to
    // `lsn`.  Advance in-memory and SB with max-guards (concurrent commit_ext may have moved ahead),
    // persist SB, then trigger a CP flush so CP-managed consumer state is durable alongside our SB writes.
    {
        auto cur = commit_upto_lsn_.load(std::memory_order_acquire);
        while (cur < lsn && !commit_upto_lsn_.compare_exchange_weak(cur, lsn, std::memory_order_acq_rel)) {}
    }
    if (sb()->commit_lsn < lsn) {
        sb()->commit_lsn = lsn;
    }
    if (sb()->last_snapshot_lsn < lsn) {
        sb()->last_snapshot_lsn = lsn;
    }
    co_await write_sb();
    co_await hs()->cp_mgr().trigger_cp_flush(/*force=*/true, CPTriggerReason::Snapshot);

    // Signal release: Repl-side use of this snap is done.  App now owns the object's future — it may be
    // returned from last_snapshot() indefinitely or dropped.
    listener_->release_snapshot(snap);
    co_return true;
}

RaftSnapshotPtr ReplicaSet::last_snapshot() {
    if (!listener_) {
        return nullptr;
    }
    auto snap = listener_->last_snapshot();
    return snap ? snap->nuraft_snapshot_ : nullptr;
}

Async< void > ReplicaSet::save_logical_snp_obj(RaftSnapshotPtr const& s, ulong& obj_id,
                                               nuraft::ptr< nuraft::buffer > const& data, bool is_first_obj,
                                               bool is_last_obj) {
    auto const lsn = s_cast< lsn_t >(s->get_last_log_idx());
    if (!listener_) {
        co_return;
    }

    if (is_first_obj) {
        // nuraft aborts and restarts a snapshot transfer from is_first=true on any interruption.  If we have a stale
        // builder from a prior attempt, tell the app to undo whatever partial state it accumulated via write_chunk
        // BEFORE we drop the ref — otherwise partial writes to live data leak.
        if (incoming_snapshot_builder_) {
            incoming_snapshot_builder_->abort();
            incoming_snapshot_builder_.reset();
        }
        auto res = co_await listener_->build_snapshot(lsn);
        if (res.hasError()) {
            RS_LOG(ERROR, NO_TRACE_ID, "build_snapshot lsn={} failed", lsn);
            co_return;
        }
        incoming_snapshot_builder_ = res.value();
    }

    if (!incoming_snapshot_builder_) {
        RS_LOG(ERROR, NO_TRACE_ID, "save_logical_snp_obj lsn={} obj_id={} — no cached builder", lsn, obj_id);
        co_return;
    }

    // Zero-copy the chunk to the app.  The owner shared<uint8_t> holds nuraft's ptr<buffer> alive via a
    // custom deleter that captures `data`; the resulting IoBufView carries this refcount forward, so the
    // app can retain the view across its own async I/O and nuraft's buffer stays alive until the last
    // referencing view drops.
    sisl::IoBufView view{sisl::make_io_buf_shared(shared< uint8_t >{data->data_begin(), [data](uint8_t*) {}},
                                                   to_u32(data->size()))};
    uint64_t cursor = obj_id;
    co_await incoming_snapshot_builder_->write_chunk(cursor, view);
    obj_id = cursor;

    // is_last_obj is informational — finalize is deferred to state_machine::apply_snapshot which nuraft
    // calls right after the is_last_obj save (only if log compact succeeds; otherwise the transfer
    // aborts and next attempt restarts from is_first=true which triggers our abort path above).
    (void)is_last_obj;
    co_return;
}

Async< int > ReplicaSet::read_logical_snp_obj(RaftSnapshotPtr const& s, void*& user_snp_ctx, ulong obj_id,
                                              RaftBufferPtr& data_out, bool& is_last_obj) {
    if (!listener_) {
        co_return -1;
    }

    // Reading a snapshot at an LSN above our committed watermark is corruption: nuraft asked us to ship
    // state we never committed.  Assert rather than mask — silently returning -1 would delay the crash
    // and leave the bug uncovered.
    HS_REL_ASSERT_LE(s_cast< lsn_t >(s->get_last_log_idx()), commit_upto_lsn_.load(std::memory_order_acquire),
                     "read_logical_snp_obj: snapshot lsn > commit_upto");

    shared< ReplSnapshot > snap;
    if (user_snp_ctx == nullptr) {
        snap = listener_->last_snapshot();
        if (!snap) {
            co_return -1;
        }
        // Heap-box the shared_ptr so nuraft's void* slot holds a durable refcount across chunk calls;
        // free_user_snp_ctx unboxes, calls listener->release_snapshot, then deletes.
        user_snp_ctx = new shared< ReplSnapshot >(snap);
    } else {
        snap = *r_cast< shared< ReplSnapshot >* >(user_snp_ctx);
    }

    uint64_t cursor = obj_id;
    is_last_obj = false;
    auto res = co_await snap->read_chunk(cursor, is_last_obj);
    if (res.hasError()) {
        co_return -1;
    }

    // Zero-copy: nuraft::buffer::take_ownership wraps the IoBufView's memory with a custom deleter that
    // captures the view by value.  The view's IoBufShared refcount holds the app's backing storage alive
    // until nuraft drops the ptr<buffer>; at that point the deleter's captured `view` destructs and
    // releases the refcount.
    auto view = res.value();
    data_out = nuraft::buffer::take_ownership(view.bytes(), view.size(),
                                              [view](nuraft::byte*) mutable { (void)view; });
    co_return 0;
}

void ReplicaSet::free_user_snp_ctx(void*& user_snp_ctx) {
    if (user_snp_ctx) {
        // nuraft fires this on transfer completion (success + all failure paths — verified via
        // handle_snapshot_sync.cxx call sites).  Signal release to the app so its per-transfer state
        // can be dropped, then delete the box.
        auto* boxed = r_cast< shared< ReplSnapshot >* >(user_snp_ctx);
        if (listener_) {
            listener_->release_snapshot(*boxed);
        }
        delete boxed;
        user_snp_ctx = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// nuraft::state_machine overrides — pre-commit / commit / rollback
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

// Log-entry payload layout, matching what ReplicaSet::write() encodes:
//   [ReplLogHeader (12 fixed bytes)] [user_header bytes: user_header_size_] [value bytes: value_size]
// The caller inspects `header->code` first and only pulls out the value slice for JournalType variants that
// actually carry inline value bytes (INLINE).  For INDIRECT / control-plane entries, `value_size` on the wire
// carries a BlkId list or is zero — the value slice is not meaningful to hand as raw bytes to the listener.
struct EntryView {
    ReplLogHeader* header;
    uint8_t* payload_start; // first byte past the fixed ReplLogHeader prefix
};

EntryView view_coalesced_entry(nuraft::buffer& buf) {
    return {
        r_cast< ReplLogHeader* >(buf.data_begin()),
        buf.data_begin() + sizeof(ReplLogHeader),
    };
}

sisl::Blob view_user_header(EntryView const& ev) {
    return sisl::Blob(ev.payload_start, ev.header->user_header_size());
}

sisl::Blob view_inline_value(EntryView const& ev) {
    return sisl::Blob(ev.payload_start + ev.header->user_header_size(), ev.header->value_size);
}

// Extract `nbytes` bytes at `offset` from a chain of nuraft::buffers.  Returns {view, backing}:
//   - `view` points at the requested bytes (null on out-of-bounds or nbytes==0).
//   - `backing` is engaged only when the range straddled buffer boundaries and had to be coalesced into a
//     fresh allocation; the caller keeps it in scope for the lifetime of any use of `view`.
// Typical case (single-buffer range) is zero-copy: `view` points into a chain buffer, `backing` == nullopt.
std::pair< sisl::Blob, std::optional< sisl::IoBufOwn > > extract_from_chain(nuraft::log_entry_chain const& chain,
                                                                            size_t offset, size_t nbytes) {
    if (nbytes == 0) {
        return {sisl::Blob{}, std::nullopt};
    }
    size_t cursor = 0;
    for (size_t i = 0; i < chain.size(); ++i) {
        auto& buf = *chain[i];
        size_t const buf_start = cursor;
        cursor += buf.size();
        if (offset >= cursor) {
            continue;
        }
        size_t const in_buf_off = offset - buf_start;
        size_t const available_here = buf.size() - in_buf_off;
        if (available_here >= nbytes) {
            // Zero-copy: whole range lives in this one buffer.
            return {sisl::Blob(buf.data_begin() + in_buf_off, to_u32(nbytes)), std::nullopt};
        }

        // Straddles — coalesce across trailing buffers into a fresh backing.  alignment=0: not an I/O buffer,
        // no reason to pay the default 512-byte alignment.
        sisl::IoBufOwn owned{to_u32(nbytes), /*alignment=*/0};
        uint8_t* dst = owned.bytes();
        std::memcpy(dst, buf.data_begin() + in_buf_off, available_here);
        size_t copied = available_here;
        for (size_t j = i + 1; j < chain.size() && copied < nbytes; ++j) {
            auto& next = *chain[j];
            size_t const take = std::min(next.size(), nbytes - copied);
            std::memcpy(dst + copied, next.data_begin(), take);
            copied += take;
        }
        if (copied < nbytes) {
            // Requested range extends past chain end — caller-side layout bug.
            return {sisl::Blob{}, std::nullopt};
        }
        return {sisl::Blob(dst, to_u32(nbytes)), std::optional< sisl::IoBufOwn >{std::move(owned)}};
    }
    return {sisl::Blob{}, std::nullopt};
}

Async< RaftBufferPtr > ReplicaSet::pre_commit_ext(ext_op_params const& params) {
    auto const lsn = to_i64(params.log_idx);
    auto ev = view_coalesced_entry(*params.data);
    if (listener_) {
        bool const accepted = listener_->on_pre_commit(lsn, view_user_header(ev));
        if (!accepted) {
            RS_LOG(WARN, NO_TRACE_ID,
                   "Listener rejected pre-commit lsn={} — abort semantics not yet propagated to nuraft, entry "
                   "will proceed to commit_ext",
                   lsn);
        }
    }
    co_return success_ptr_;
}

Async< RaftBufferPtr > ReplicaSet::commit_ext_chained(ulong log_idx, nuraft::log_entry_chain const& bufs) {
    auto const lsn = to_i64(log_idx);
    if (should_skip_commit(lsn)) {
        RS_LOG(INFO, NO_TRACE_ID, "commit_ext_chained lsn={} covered by last snapshot, skipping", lsn);
        co_return success_ptr_;
    }

    // Walk the chain safely — no positional bufs[i] indexing.  Each piece (header, user_header, value) is
    // pulled out by byte offset; when a piece fits within one chain buffer we get a zero-copy view, when it
    // straddles a boundary extract_from_chain coalesces just that piece into an owned backing returned
    // alongside the view.  uh/val backings must stay in scope for the duration of dispatch_commit.
    auto [hdr_view, hdr_backing] = extract_from_chain(bufs, 0, sizeof(ReplLogHeader));
    if (hdr_view.bytes() == nullptr) {
        RS_LOG(ERROR, NO_TRACE_ID, "commit_ext_chained lsn={} chain shorter than ReplLogHeader", lsn);
        co_return success_ptr_;
    }
    auto const* h = r_cast< ReplLogHeader const* >(hdr_view.cbytes());
    auto const type = s_cast< JournalType >(h->code);
    auto const uh_size = h->user_header_size();
    auto const v_size = h->value_size;
    // hdr_view/hdr_backing can drop here — everything we needed off ReplLogHeader is copied out into locals.

    auto [uh_view, uh_backing] = extract_from_chain(bufs, sizeof(ReplLogHeader), uh_size);
    auto [val_view, val_backing] = extract_from_chain(bufs, sizeof(ReplLogHeader) + uh_size, v_size);
    if ((uh_size > 0 && uh_view.bytes() == nullptr) || (v_size > 0 && val_view.bytes() == nullptr)) {
        RS_LOG(ERROR, NO_TRACE_ID, "commit_ext_chained lsn={} declared sizes exceed chain length", lsn);
        co_return success_ptr_;
    }

    co_await dispatch_commit(lsn, type, uh_view, val_view);
    co_return success_ptr_;
}

Async< RaftBufferPtr > ReplicaSet::commit_ext(ext_op_params const& params) {
    auto const lsn = to_i64(params.log_idx);
    if (should_skip_commit(lsn)) {
        RS_LOG(INFO, NO_TRACE_ID, "commit_ext lsn={} covered by last snapshot, skipping", lsn);
        co_return success_ptr_;
    }
    auto ev = view_coalesced_entry(*params.data);
    co_await dispatch_commit(lsn, s_cast< JournalType >(ev.header->code), view_user_header(ev), view_inline_value(ev));
    co_return success_ptr_;
}

Async< void > ReplicaSet::rollback_ext(ext_op_params const& params) {
    auto const lsn = to_i64(params.log_idx);
    auto ev = view_coalesced_entry(*params.data);
    if (listener_) {
        listener_->on_rollback(lsn, view_user_header(ev));
    }
    co_return;
}

Async< void > ReplicaSet::commit_config(ulong log_idx, nuraft::ptr< nuraft::cluster_config >& new_conf) {
    auto const lsn = to_i64(log_idx);
    if (should_skip_commit(lsn)) {
        RS_LOG(INFO, NO_TRACE_ID, "commit_config lsn={} covered by last snapshot, skipping", lsn);
        co_return;
    }

#ifdef _PRERELEASE
    // Log the resulting membership at INFO so cluster-change audits are visible in test/debug runs; skipped in
    // release builds to keep the commit path allocation-free.
    std::vector< int32_t > server_ids;
    server_ids.reserve(new_conf->get_servers().size());
    for (auto const& srv : new_conf->get_servers()) {
        server_ids.push_back(srv->get_id());
    }
    RS_LOG(INFO, NO_TRACE_ID, "commit_config lsn={} servers=[{}] my_id={}", lsn, fmt::join(server_ids, ","),
           raft_server_id_);
#endif

    // Config entries advance the general commit watermark just like data entries — everything up to this LSN is
    // durable at this point.
    commit_upto_lsn_.store(lsn);

    // Self-remove path.  If I am no longer in the committed config, the leader removed me — tear down locally.
    // start_destroy_local() is idempotent so a stray call during a whole-group destroy is a no-op.  Same landing
    // point as CTRL_DESTROY, just a different trigger (leader-driven remove_srv vs group-wide destroy).
    bool in_config = false;
    std::set< ReplicaId > new_members;
    for (auto const& srv : new_conf->get_servers()) {
        if (srv->get_id() == raft_server_id_) {
            in_config = true;
        }
        try {
            new_members.insert(boost::uuids::string_generator{}(srv->get_aux()));
        } catch (std::exception const&) {
            // Malformed aux — skip; peer would fail identity checks elsewhere too.
        }
    }
    if (!in_config) {
        RS_LOG(INFO, NO_TRACE_ID,
               "commit_config lsn={} — my server_id={} absent from new config, start_destroy_local()", lsn,
               raft_server_id_);
        co_await start_destroy_local();
    }

    // Diff against the last-known-committed membership and fire the listener with the delta.  When the delta
    // is empty (initial commit_config after start() that just replays what's already on disk, or a config
    // rewrite with the same members), skip the callback entirely.
    std::set< ReplicaId > added;
    std::set< ReplicaId > removed;
    std::set_difference(new_members.begin(), new_members.end(), committed_members_.begin(), committed_members_.end(),
                        std::inserter(added, added.end()));
    std::set_difference(committed_members_.begin(), committed_members_.end(), new_members.begin(), new_members.end(),
                        std::inserter(removed, removed.end()));
    committed_members_ = std::move(new_members);
    if (listener_ && (!added.empty() || !removed.empty())) {
        listener_->on_membership_change(added, removed);
    }
    co_return;
}

Async< void > ReplicaSet::rollback_config(ulong log_idx, nuraft::ptr< nuraft::cluster_config >& conf) {
    auto const lsn = to_i64(log_idx);
    RS_LOG(DEBUG, NO_TRACE_ID, "rollback_config lsn={}", lsn);
    (void)conf; // listener notification is lsn-only; the config bytes are handled entirely inside nuraft
    if (listener_) {
        listener_->on_config_rollback(lsn);
    }
    co_return;
}

ulong ReplicaSet::last_commit_index() {
    // commit_upto_lsn_ is advanced by every commit_ext / commit_ext_chained / commit_config that lands.
    return to_ulong(commit_upto_lsn_.load());
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Private Vital methods to maintain the lifecycles
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// do_replace_member — shared step-driver.  Cascades from `start` through kProposeComplete, running each
// stage's action then falling into the next.  Each stage is either idempotent by itself (add/remove/flip)
// or safely re-proposable (CTRL log entries).  Called from both the fresh replace_member() entry point (with
// start=kProposeStart) and resume_pending_replace_member() (with start decided from cluster state).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< ReplResult<> > ReplicaSet::do_replace_member(ReplicaMemberInfo const& out, ReplicaMemberInfo const& in,
                                                    std::string const& task_id, ReplaceStage start, TraceId tid) {
    // Build the CTRL payload once — used by both kProposeStart and kProposeComplete.  The user_header IS this
    // packed struct; dispatch_commit's CTRL branches on every replica read it back and persist / clear the
    // SB and fire the listener.
    ReplaceMemberCtrlHeader payload{};
    payload.out_id = out.id;
    std::memcpy(payload.out_name, out.name, ReplicaMemberInfo::max_name_len);
    payload.out_priority = out.priority;
    payload.in_id = in.id;
    std::memcpy(payload.in_name, in.name, ReplicaMemberInfo::max_name_len);
    payload.in_priority = in.priority;
    std::memset(payload.task_id, 0, kReplaceTaskIdLen);
    std::memcpy(payload.task_id, task_id.data(), std::min(task_id.size(), kReplaceTaskIdLen - 1));
    sisl::Blob const payload_blob{to_cu8ptr(&payload), to_u32(sizeof(payload))};

    switch (start) {
    case ReplaceStage::kProposeStart: {
        std::vector< nuraft::ptr< nuraft::buffer > > logs;
        logs.push_back(build_log_entry(JournalType::HS_CTRL_START_REPLACE, payload_blob, /*value_size=*/0));
        auto result = co_await raft_server_->append_entries(logs);
        if (!result.accepted || !result.committed) {
            RS_LOG(ERROR, tid, "do_replace_member: START_REPLACE failed accepted={} committed={} code={}",
                   result.accepted, result.committed, to_int(result.code));
            co_return folly::makeUnexpected(ReplicationManager::to_repl_error(result.code));
        }
    }
        [[fallthrough]];
    case ReplaceStage::kAddLearner:
        if (auto r = co_await add_member(in, /*learner=*/true, tid); !r) {
            co_return r;
        }
        [[fallthrough]];
    case ReplaceStage::kWaitAndFlip:
        if (auto r = co_await wait_for_catchup(in, tid); !r) {
            co_return r;
        }
        if (auto r = co_await flip_learner_flag(in, /*target=*/false, tid); !r) {
            co_return r;
        }
        [[fallthrough]];
    case ReplaceStage::kRemoveOld:
        if (auto r = co_await remove_member(out, tid); !r) {
            co_return r;
        }
        [[fallthrough]];
    case ReplaceStage::kProposeComplete: {
        std::vector< nuraft::ptr< nuraft::buffer > > logs;
        logs.push_back(build_log_entry(JournalType::HS_CTRL_COMPLETE_REPLACE, payload_blob, /*value_size=*/0));
        auto result = co_await raft_server_->append_entries(logs);
        if (!result.accepted || !result.committed) {
            RS_LOG(ERROR, tid, "do_replace_member: COMPLETE_REPLACE failed accepted={} committed={} code={}",
                   result.accepted, result.committed, to_int(result.code));
            co_return folly::makeUnexpected(ReplicationManager::to_repl_error(result.code));
        }
        break;
    }
    }
    RS_LOG(INFO, tid, "do_replace_member: completed task_id={}", task_id);
    co_return ReplResult<>{};
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// resume_pending_replace_member — fired from raft_event(BecomeLeader) if the SB records an in-flight replace.
// Reads the intent from SB, waits for the traffic gate so get_config() is authoritative, inspects the current
// cluster config, picks the earliest ReplaceStage that still needs to run, and hands off to do_replace_member.
// Divergent state (persisted intent inconsistent with current config) clears the SB and bails without acting.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

Async< void > ReplicaSet::resume_pending_replace_member() {
    // Snapshot the persisted intent at the start.  If the SB gets cleared out from under us (a raced
    // CTRL_COMPLETE_REPLACE commit during log-replay finishes it before we grab leadership), we bail cleanly.
    ReplicaId const persisted_out = sb()->replace_member_sb.replica_out;
    ReplicaId const persisted_in = sb()->replace_member_sb.replica_in;
    std::string const task_id{sb()->replace_member_sb.task_id};
    if (task_id.empty()) {
        co_return;
    }

    // Gate on is_ready_for_traffic().  Before this returns true, get_config() may still contain uncommitted
    // config entries from a prior term — we'd be resuming off a phantom cluster view.  Poll rather than
    // block; if we lose leadership before the gate opens, hand off to whoever inherits leadership next.
    while (!is_ready_for_traffic()) {
        if (!is_leader()) {
            RS_LOG(INFO, NO_TRACE_ID, "resume_pending_replace_member: lost leadership before traffic gate, task_id={}",
                   task_id);
            co_return;
        }
        co_await folly::coro::sleep(std::chrono::milliseconds{100});
    }

    // From here on we can trust get_config() as the durable-committed view.
    auto const config = raft_server_->get_config();
    auto const in_srv_id = to_server_id(persisted_in);
    auto const out_srv_id = to_server_id(persisted_out);
    bool out_present = false;
    bool in_present_as_voter = false;
    bool in_present_as_learner = false;
    for (auto const& srv : config->get_servers()) {
        if (srv->get_id() == out_srv_id) {
            out_present = true;
        } else if (srv->get_id() == in_srv_id) {
            if (srv->is_learner()) {
                in_present_as_learner = true;
            } else {
                in_present_as_voter = true;
            }
        }
    }
    RS_LOG(INFO, NO_TRACE_ID,
           "resume_pending_replace_member: task_id={} out={} in={} out_present={} in_voter={} in_learner={}", task_id,
           boost::uuids::to_string(persisted_out), boost::uuids::to_string(persisted_in), out_present,
           in_present_as_voter, in_present_as_learner);

    // Divergent: out already gone AND in not a voter → someone else finished a different replace, or worse.
    // Clear the SB, don't touch cluster state.
    if (!out_present && !in_present_as_voter) {
        RS_LOG(WARN, NO_TRACE_ID,
               "resume_pending_replace_member: divergent task_id={} out_present={} in_learner={} — clearing SB",
               task_id, out_present, in_present_as_learner);
        std::memset(&sb()->replace_member_sb, 0, sizeof(sb()->replace_member_sb));
        co_await write_sb();
        co_return;
    }

    // Pick the earliest stage that still needs to run based on the config snapshot.  All SB has is UUIDs;
    // do_replace_member's steps that need name/priority (add_member) rely on lookup_peer_addr for the
    // endpoint and treat name/priority as best-effort empty — matches upstream semantics.
    ReplaceStage start = ReplaceStage::kProposeComplete;
    if (out_present && !in_present_as_voter && !in_present_as_learner) {
        start = ReplaceStage::kAddLearner;
    } else if (out_present && in_present_as_learner) {
        start = ReplaceStage::kWaitAndFlip;
    } else if (out_present && in_present_as_voter) {
        start = ReplaceStage::kRemoveOld;
    }
    // else: (!out_present && in_present_as_voter) → kProposeComplete, nothing to touch on cluster.

    ReplicaMemberInfo out_info{};
    out_info.id = persisted_out;
    ReplicaMemberInfo in_info{};
    in_info.id = persisted_in;

    auto r = co_await do_replace_member(out_info, in_info, task_id, start, /*tid=*/0);
    if (!r) {
        RS_LOG(ERROR, NO_TRACE_ID, "resume_pending_replace_member: do_replace_member failed err={}", to_int(r.error()));
    }
    co_return;
}

// nuraft dispatches every raft-side event through this callback (registered in start()).  We handle the
// role transitions inline — stamping the traffic gate and notifying the listener — and let nuraft handle
// everything else.  Runs synchronously inside nuraft's callback dispatch; keep the body cheap.
nuraft::cb_func::ReturnCode ReplicaSet::raft_event(nuraft::cb_func::Type type, nuraft::cb_func::Param* /*param*/) {
    switch (type) {
    case nuraft::cb_func::Type::BecomeLeader: {
        auto const gate = raft_server_ ? raft_server_->get_last_log_idx() : 0ul;
        traffic_ready_lsn_.store(to_i64(gate));
        RS_LOG(INFO, NO_TRACE_ID, "raft_event BecomeLeader — traffic_ready_lsn set to {}", gate);
        if (listener_) {
            listener_->on_change_in_role(ReplicaRole::LEADER);
        }
        // Restart-resume for any replace_member left mid-flight before the previous leader crashed / stepped
        // down.  SB carries the intent (task_id + out/in); we're the leader now so it's our job.  Fire and
        // forget onto the same reactor this ReplicaSet is pinned to — the coroutine internally waits for
        // is_ready_for_traffic() before touching cluster state so we never act on an uncommitted config view.
        if (sb()->replace_member_sb.task_id[0] != '\0') {
            iomanager::iomgr().spawn_detached(iomanager::ReactorTarget::current(),
                                              [this]() -> Async< void > { co_await resume_pending_replace_member(); });
        }
        break;
    }
    case nuraft::cb_func::Type::BecomeFollower:
    case nuraft::cb_func::Type::JoinedCluster: {
        traffic_ready_lsn_.store(0);
        RS_LOG(INFO, NO_TRACE_ID, "raft_event {} — traffic_ready_lsn reset",
               (type == nuraft::cb_func::Type::JoinedCluster) ? "JoinedCluster" : "BecomeFollower");
        if (listener_) {
            listener_->on_change_in_role(ReplicaRole::FOLLOWER);
        }
        break;
    }
    default:
        break;
    }
    return nuraft::cb_func::ReturnCode::Ok;
}

Async< void > ReplicaSet::dispatch_commit(int64_t lsn, JournalType type, sisl::Blob const& user_header,
                                          sisl::Blob const& value) {
    switch (type) {
    case JournalType::HS_DATA_INLINE:
    case JournalType::HS_DATA_INDIRECT: {
        // Both surface identically at this point.  HomeRaftLogStore's on_commit(lsn) is the authoritative
        // "was this entry stored indirectly?" signal — it promotes uncommitted BlkIds to committed and
        // returns them (empty for pure inline entries).
        BlkIds const bids = log_store_->on_commit(lsn);
        if (listener_) {
            listener_->on_commit(lsn, user_header, value, bids);
        }
        break;
    }
    case JournalType::HS_CTRL_DESTROY:
        // Every replica lands here when the leader's HS_CTRL_DESTROY commits.  start_destroy_local() marks
        // DESTROYED + persists destroy_pending — actual resource teardown deferred to the ReplicationManager's
        // reaper calling finish_destroy_local() after replica_set_cleanup_interval_sec.
        co_await start_destroy_local();
        break;
    case JournalType::HS_CTRL_START_REPLACE:
    case JournalType::HS_CTRL_COMPLETE_REPLACE: {
        // The user_header IS the packed ReplaceMemberCtrlHeader that replace_member stamped on the log.
        // Every replica lands here — persist / clear sb()->replace_member_sb symmetrically so any replica
        // that later becomes leader can resume from a well-defined SB state, and fire the listener hooks so
        // applications observe the swap start/finish uniformly across the group.
        if (user_header.size() != sizeof(ReplaceMemberCtrlHeader)) {
            RS_LOG(ERROR, NO_TRACE_ID, "commit lsn={} replace ctrl header size={} != expected {}", lsn,
                   user_header.size(), sizeof(ReplaceMemberCtrlHeader));
            break;
        }
        auto const* hdr = r_cast< ReplaceMemberCtrlHeader const* >(user_header.cbytes());
        ReplicaMemberInfo out{};
        out.id = hdr->out_id;
        std::memcpy(out.name, hdr->out_name, ReplicaMemberInfo::max_name_len);
        out.priority = hdr->out_priority;
        ReplicaMemberInfo in{};
        in.id = hdr->in_id;
        std::memcpy(in.name, hdr->in_name, ReplicaMemberInfo::max_name_len);
        in.priority = hdr->in_priority;
        // The header carries task_id as a fixed-length char array; the listener API takes a string_view so
        // apps never have to know the fixed length or reason about null-termination on the wire.  strnlen
        // trims to the actual UUID string (36 chars) even though the buffer is 40 bytes.
        std::string_view const task_id_view{hdr->task_id, ::strnlen(hdr->task_id, kReplaceTaskIdLen)};
        if (type == JournalType::HS_CTRL_START_REPLACE) {
            sb()->replace_member_sb.replica_out = hdr->out_id;
            sb()->replace_member_sb.replica_in = hdr->in_id;
            std::memcpy(sb()->replace_member_sb.task_id, hdr->task_id, kReplaceTaskIdLen);
            co_await write_sb();
            if (listener_) {
                listener_->on_start_replace_member(out, in, task_id_view);
            }
        } else {
            std::memset(&sb()->replace_member_sb, 0, sizeof(sb()->replace_member_sb));
            co_await write_sb();
            if (listener_) {
                listener_->on_complete_replace_member(out, in, task_id_view);
            }
        }
        break;
    }
    case JournalType::HS_CTRL_TRUNCATE: {
        // user_header is the packed int64_t target LSN stamped by advance_truncate_upto.  Every replica
        // lands here with a monotonic max-guard on the atomic.
        if (user_header.size() != sizeof(int64_t)) {
            RS_LOG(ERROR, NO_TRACE_ID, "commit lsn={} compact ctrl header size={} != expected {}", lsn,
                   user_header.size(), sizeof(int64_t));
            break;
        }
        auto const target = *r_cast< int64_t const* >(user_header.cbytes());
        auto cur = app_truncate_upto_.load(std::memory_order_acquire);
        while (cur < target && !app_truncate_upto_.compare_exchange_weak(cur, target, std::memory_order_acq_rel)) {}
        RS_LOG(INFO, NO_TRACE_ID, "HS_CTRL_TRUNCATE commit lsn={} target={}", lsn, target);
        break;
    }
    }
    commit_upto_lsn_.store(lsn);
    co_return;
}

// Catch-up thresholds — a learner is deemed caught up once its last replicated log index is within
// kMaxCatchupLag of the leader's tail.  The gap need not be zero; nuraft can promote learners that are only
// close, and any residual tail replicates alongside subsequent normal traffic once the promotion commits.
static constexpr ulong kMaxCatchupLag = 10;
static constexpr auto kCatchupPollInterval = std::chrono::milliseconds{500};
static constexpr auto kMaxCatchupWait = std::chrono::minutes{5};

Async< ReplResult<> > ReplicaSet::wait_for_catchup(ReplicaMemberInfo const& member, TraceId tid) {
    auto const srv_id = to_server_id(member.id);
    auto const deadline = Clock::now() + kMaxCatchupWait;
    while (true) {
        if (!raft_server_ || !raft_server_->is_leader()) {
            RS_LOG(WARN, tid, "wait_for_catchup: lost leadership while polling member={}",
                   boost::uuids::to_string(member.id));
            co_return folly::makeUnexpected(ReplError::NOT_LEADER);
        }
        auto const leader_tail = raft_server_->get_last_log_idx();
        auto const info = co_await raft_server_->get_peer_info(srv_id);
        auto const peer_tail = info.last_log_idx_;
        auto const lag = (leader_tail > peer_tail) ? (leader_tail - peer_tail) : 0;
        if (lag <= kMaxCatchupLag) {
            RS_LOG(INFO, tid, "wait_for_catchup: member={} caught up, lag={}, leader_tail={} peer_tail={}",
                   boost::uuids::to_string(member.id), lag, leader_tail, peer_tail);
            co_return ReplResult<>{};
        }
        if (Clock::now() >= deadline) {
            RS_LOG(ERROR, tid, "wait_for_catchup: member={} timed out, lag={}, leader_tail={} peer_tail={}",
                   boost::uuids::to_string(member.id), lag, leader_tail, peer_tail);
            co_return folly::makeUnexpected(ReplError::TIMEOUT);
        }
        co_await folly::coro::sleep(kCatchupPollInterval);
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Listener attach / detach — also hands the listener a back-reference to this ReplicaSet so its callbacks can
// reach state via replica_set() if they need to.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

void ReplicaSet::attach_listener(shared< ReplicaSetListener > listener) {
    listener_ = std::move(listener);
    if (listener_) {
        listener_->set_replica_set(shared_from_this());
    }
}

void ReplicaSet::detach_listener() {
    if (listener_) {
        listener_->set_replica_set(nullptr);
        listener_.reset();
    }
}
} // namespace homestore
