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

#include <cstring>

#include <fmt/format.h>
#include <boost/uuid/uuid_io.hpp>
#include <folly/Executor.h>
#include <folly/hash/Hash.h>
#include <folly/io/async/EventBase.h>

#include <libnuraft/context.hxx>
#include <libnuraft/delayed_task.hxx>
#include <libnuraft/delayed_task_scheduler.hxx>
#include <libnuraft/logger.hxx>
#include <libnuraft/raft_params.hxx>
#include <libnuraft/raft_server.hxx>
#include <libnuraft/rpc_cli_factory.hxx>
#include <libnuraft/rpc_listener.hxx>

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

ReplicaSet::ReplicaSet(ReplicationManager& mgr, superblk< ReplicaSetSuperBlk >&& rd_sb, bool load_existing) :
        mgr_{mgr},
        group_id_{rd_sb->group_id},
        my_uuid_{mgr.get_my_repl_id()},
        raft_server_id_{to_server_id(my_uuid_)},
        rd_sb_{std::move(rd_sb)} {

    if (load_existing) {
        // Existing replica set — fields recovered from the persisted superblk.
        rset_name_ = rd_sb_->rset_name;
    } else {
        // Fresh replica set — initialize superblk with defaults and persist.
        rset_name_ = fmt::format("rset_{}", boost::uuids::to_string(group_id_).substr(0, 8));
        rd_sb_->set_rset_name(rset_name_);
        rd_sb_->destroy_pending = 0;
        rd_sb_->last_snapshot_lsn = 0;
        rd_sb_.write();
    }

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
// write — client entry point
//
// HS stamps its own fields on `hdr` (code = HS_DATA_INLINE, version, value_size) — app cannot influence those
// bytes. Each follower's HomeRaftLogStore may rewrite `code` to HS_DATA_INDIRECT in its on-disk shim form when
// the value exceeds the size threshold, but the in-memory bufs() we replicate stays INLINE so each replica
// makes its own storage-form decision.
// The chain is [ReplLogHeader-as-buf (zero-copy via take_ownership of the shared<ReplLogHeader>) ,
// value-as-buf (zero-copy take_ownership of caller's IoBufSpan bytes)]. log_entry's auto-allocated bufs_[0] (9B
// [term|val_type] header) sits in front of both. Term=0 at construction; the leader's propose path fills the
// real term in place via log_entry::set_term.
//
// Lifetime: caller MUST keep value bytes alive until this Task<> resolves. ReplLogHeader carries its own
// storage via the aliasing shared_ptr returned from alloc().
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
folly::coro::Task< ReplResult<> > ReplicaSet::write(shared< ReplLogHeader > hdr, sisl::IoBufSpan value, TraceId tid) {
    (void)tid;
    if (!hdr) {
        co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
    }

    // Stamp HS-owned fields.
    hdr->code = to_u8(JournalType::HS_DATA_INLINE);
    hdr->major_version = ReplLogHeader::kMajor;
    hdr->minor_version = ReplLogHeader::kMinor;
    hdr->value_size = to_u32(value.size());

    // Build the value chain — zero copy. hdr bytes flow through ReplLogHeader::to_nuraft_buffer (deleter
    // captures the shared_ptr keeping storage alive). value bytes flow through buffer::take_ownership; the
    // deleter holds the IoBufSpan struct by value — caller's lifetime contract keeps the underlying memory
    // alive until this Task<> resolves.
    auto hdr_buf = ReplLogHeader::to_nuraft_buffer(std::move(hdr));
    auto value_buf = nuraft::buffer::take_ownership(value.bytes(), value.size(),
                                                    [held = value](nuraft::byte*) noexcept { (void)held; });

    nuraft::log_entry_chain chain;
    chain.push_back(std::move(hdr_buf));
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

} // namespace homestore
