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
#include "common/async.h"

#include <cstring>

#include <fmt/format.h>
#include <boost/uuid/uuid_io.hpp>
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
#include "common/homestore_config.h"
#include "homestore/homestore.h"
#include "homestore/logstore_service.hpp"
#include "homestore/replication/repl_manager.h"
#include "replication/transport/folly_rpc_client_factory.h"
#include "replication/transport/folly_rpc_listener.h"
#include "iomanager/iomanager.h"

SISL_LOGGING_DECL(replication)

namespace homestore {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

ReplicaSet::ReplicaSet(ReplicationManager& mgr, ModuleMetaBlk< ReplicaSetSuperBlk >&& rs_sb, bool load_existing) :
        mgr_{mgr},
        group_id_{rs_sb->group_id},
        my_uuid_{mgr.get_my_repl_id()},
        raft_server_id_{to_server_id(my_uuid_)},
        rs_sb_{std::move(rs_sb)} {

    // The sb is durable and fully populated by the caller (create_replica_set_for_group for fresh-create,
    // or recovery for load-existing).  Ctor just reads.
    rset_name_ = rs_sb_->rset_name;
    identify_str_ = rset_name_ + ":" + group_id_str();
    metrics_ = std::make_unique< ReplicaSetMetrics >(identify_str_.c_str());

    LOGINFOMOD(replication, "Started {} ReplicaSet group_id={} replica_id={} raft_server_id={}",
               (load_existing ? "Existing" : "New"), group_id_str(), my_replica_id_str(), raft_server_id_);
}

ReplicaSet::~ReplicaSet() = default;

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
// join_group — construct the per-group nuraft::raft_server and register it with the folly transport
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

bool ReplicaSet::join_group() {
    // Pin all this raft_server's coro work to one reactor — hash by group_id so each group sticks to one.
    auto& iom = iomanager::iomgr();
    size_t reactor_id = folly::hash::fnv64_buf(group_id_.data, sizeof(group_id_.data)) % iom.num_reactors();
    auto* eb = iom.reactor_for(reactor_id);

    // Carry the boost::uuids::uuid GroupId across to nuraft as raw 16 bytes (nuraft::group_id_t is a
    // std::array<uint8_t,16>). The wire frame's group_id field uses the same encoding.
    nuraft::group_id_t gid{};
    std::memcpy(gid.data(), group_id_.data, sizeof(gid));

    nuraft::raft_params params;

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
    // Capture via weak_ptr — raft callbacks can fire during teardown and must not keep ReplicaSet alive.
    ctx->set_cb_func([wp = std::weak_ptr< ReplicaSet >(self)](nuraft::cb_func::Type t, nuraft::cb_func::Param* p) {
        if (auto sp = wp.lock(); sp) {
            return sp->raft_event(t, p);
        }
        return nuraft::cb_func::Ok;
    });

    nuraft::raft_server::init_options opts;
    opts.main_executor_ = folly::Executor::getKeepAliveToken(eb);

    raft_server_ = std::make_shared< nuraft::raft_server >(ctx, opts);

    LOGINFOMOD(replication, "Joined raft group_id={} on reactor={}", group_id_str(), reactor_id);
    return raft_server_ != nullptr;
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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// write — single client entry point.  Replicates the user's (header, value) through raft.  Resolves after
// the entry is committed across the quorum (the listener's on_commit has already fired), or with a
// ReplError if replication couldn't complete (not leader, alloc failure, no quorum, etc).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
Async< ReplResult<> > ReplicaSet::write(sisl::IoBuf const& user_header, sisl::IoBufView value, TraceId tid) {
    (void)tid;

    auto const uh_size = user_header.size();
    auto const v_size = value.size();

    // Allocate the header slab — fixed ReplLogHeader prefix + trailing user_header bytes.
    auto hdr_slab = nuraft::buffer::alloc(sizeof(ReplLogHeader) + uh_size);
    auto* h = new (hdr_slab->data_begin()) ReplLogHeader{};
    h->code = to_u8(JournalType::HS_DATA_INLINE);
    h->major_version = ReplLogHeader::kMajor;
    h->minor_version = ReplLogHeader::kMinor;
    h->value_size = to_u32(v_size);
    h->user_header_size_ = to_u32(uh_size);
    if (uh_size > 0) {
        std::memcpy(hdr_slab->data_begin() + sizeof(ReplLogHeader), user_header.cbytes(), uh_size);
    }

    // Value buf — zero-copy via IoBufView::extract (IoBufShared) + buffer::take_ownership.  The captured
    // IoBufShared keeps the value bytes alive until folly drops the nuraft::buffer (post wire send / disk
    // write / blob_stream write).
    auto ba = value.extract();
    auto value_buf =
        nuraft::buffer::take_ownership(ba->bytes(), ba->size(), [held = ba](nuraft::byte*) noexcept { (void)held; });

    nuraft::log_entry_chain chain;
    chain.push_back(std::move(hdr_slab));
    chain.push_back(std::move(value_buf));

    std::vector< nuraft::log_entry_chain > chains;
    chains.push_back(std::move(chain));

    auto result = co_await raft_server_->append_entries_chained(chains);

    if (!result.accepted) {
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(result.code));
    }
    if (!result.committed) {
        co_return folly::makeUnexpected(ReplicationManager::to_repl_error(result.code));
    }
    co_return ReplResult<>{};
}

void ReplicaSet::use_config(JsonMetaBlk raft_config_sb) {
    std::unique_lock lg{config_mtx_};
    raft_config_sb_ = std::move(raft_config_sb);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// nuraft::state_mgr overrides
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

nuraft::ptr< nuraft::cluster_config > ReplicaSet::load_config() {
    std::unique_lock lg{config_mtx_};
    auto& js = *raft_config_sb_;
    if (!js.contains("config")) {
        auto cluster_conf = nuraft::cs_new< nuraft::cluster_config >();
        cluster_conf->get_servers().push_back(nuraft::cs_new< nuraft::srv_config >(
            raft_server_id_, 0, my_replica_id_str(), "", false, raft_leader_priority));
        js["config"] = serialize_cluster_config(*cluster_conf);
    }
    return deserialize_cluster_config(js["config"]);
}

void ReplicaSet::save_config(nuraft::cluster_config const& config) {
    std::unique_lock lg{config_mtx_};
    (*raft_config_sb_)["config"] = serialize_cluster_config(config);
    iomanager::blocking_wait(raft_config_sb_.write());
}

void ReplicaSet::save_state(nuraft::srv_state const& state) {
    std::unique_lock lg{config_mtx_};
    (*raft_config_sb_)["state"] = nlohmann::json{{"term", state.get_term()},
                                                 {"voted_for", state.get_voted_for()},
                                                 {"election_timer_allowed", state.is_election_timer_allowed()},
                                                 {"catching_up", state.is_catching_up()}};
    iomanager::blocking_wait(raft_config_sb_.write());
}

nuraft::ptr< nuraft::srv_state > ReplicaSet::read_state() {
    std::unique_lock lg{config_mtx_};
    auto& js = *raft_config_sb_;
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
        } catch (std::out_of_range const&) {
            LOGWARNMOD(replication, "Persisted state not in the expected format [group_id={}]", group_id_str());
        }
    }
    return state;
}

nuraft::ptr< nuraft::log_store > ReplicaSet::load_log_store() {
    return log_store_;
}

int32_t ReplicaSet::server_id() {
    return raft_server_id_;
}

void ReplicaSet::system_exit(int exit_code) {
    LOGINFOMOD(replication, "System exit signal received [group_id={} exit_code={}]", group_id_str(), exit_code);
}

} // namespace homestore
