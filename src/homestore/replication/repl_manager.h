#pragma once

// ReplicationManager — owns the per-node replication state. One instance per HomeStore; held by the core lib
// alongside CPManager / MetaBlkMgr. Drives the public Task<> proposer API by talking to per-group raft_servers
// (append_entries / add_srv / remove_srv) on the application's reactor pool. Requires LogStoreService to be
// running before start() is invoked.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>

#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <iomgr/iomgr_timer.hpp>

#include "sisl/fds/buffer.h"
#include "sisl/fds/enum.h"

#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/replication/repl_decls.h"
#include "homestore/superblk_handler.hpp"

namespace nuraft {
class raft_server;
class srv_config;
enum cmd_result_code : int;
} // namespace nuraft

namespace homestore {

namespace replication {
class FollyRpcClientFactory;
class FollyRpcListener;
} // namespace replication

class ReplicaSet;
class ReplicaSetListener;
class ReplApplication;
struct repl_dev_superblk;

VENUM(ReplImplType, uint8_t,
      server_side,    // Completely homestore controlled replication
      client_assisted // Client assisting in replication
);

// Leader election priority defaults and decay parameters used when seeding new raft groups.
constexpr int32_t raft_leader_priority = 100;
constexpr double raft_priority_decay_coefficient = 0.8;
constexpr uint32_t raft_priority_election_round_upper_limit = 5;

class ReplicationManager {
public:
    explicit ReplicationManager(shared< ReplApplication > repl_app);
    ~ReplicationManager();

    ReplicationManager(ReplicationManager const&) = delete;
    ReplicationManager& operator=(ReplicationManager const&) = delete;

    folly::coro::Task< void > start();
    folly::coro::Task< void > stop();

    // -------- Public Task<>-driven API (proposer-style operations) --------

    folly::coro::Task< ReplResult< shared< ReplicaSet > > > create_replica_set(GroupId group_id,
                                                                               std::set< ReplicaId > const& members);

    /// Schedule a replica set for destruction. Resolves once the destroy is accepted; actual resource reclaim
    /// happens lazily in the reaper.
    folly::coro::Task< ReplError > remove_replica_set(GroupId group_id);

    /// Replace one member of a group with another. Two-phase: (1) flip member_out to learner + add member_in;
    /// (2) remove member_out.
    folly::coro::Task< ReplResult<> > replace_member(GroupId group_id, ReplicaMemberInfo const& member_out,
                                                     ReplicaMemberInfo const& member_in, uint32_t commit_quorum = 0,
                                                     uint64_t trace_id = 0) const;

    folly::coro::Task< ReplResult<> > flip_learner_flag(GroupId group_id, ReplicaMemberInfo const& member, bool target,
                                                        uint32_t commit_quorum, bool wait_and_verify = true,
                                                        uint64_t trace_id = 0) const;

    // -------- Synchronous accessors --------

    ReplResult< shared< ReplicaSet > > get_replica_set(GroupId group_id) const;
    void iterate_replica_sets(std::function< void(cshared< ReplicaSet >&) > const& cb);

    ReplicaId get_my_repl_id() const { return my_uuid_; }
    ReplApplication& repl_app() { return *repl_app_; }

    /// Format `<host>:<port>` for a given peer via ReplApplication::lookup_peer. Empty string if unknown.
    std::string lookup_peer_addr(ReplicaId const& peer) const;

    /// Route an inbound wire frame's group_id to the owning raft_server. Returns nullptr if no replica
    /// set is registered for that group.
    nuraft::ptr< nuraft::raft_server > lookup_raft_server(nuraft::group_id_t const& gid) const;

    /// Same as lookup_raft_server but on a miss, attempts to construct a new ReplicaSet by asking the
    /// application via ReplApplication::create_replica_set_listener. If the application returns nullptr
    /// (rejecting the unknown group), this also returns nullptr. Called by the folly transport's
    /// InboundConnection on every inbound request — first contact from a leader that added us via add_srv
    /// arrives here.
    nuraft::ptr< nuraft::raft_server > lookup_or_create_raft_server(nuraft::group_id_t const& gid);

    /// Folly transport — used by per-group ReplicaSet's join_group to build the nuraft::context.
    shared< replication::FollyRpcClientFactory > rpc_client_factory() const { return rpc_client_factory_; }
    shared< replication::FollyRpcListener > rpc_listener() const { return rpc_listener_; }

    /// Slow-path executor — a CPU thread pool reserved for RPCs that may block on log_store reads (snapshot
    /// install, sync_log, membership changes) and for internal callbacks that can stall (snapshot completion's
    /// config-chain walk). Hot RPCs (vote, append_entries, etc.) stay on iomgr reactors. Returned as a raw
    /// pointer so callers can wrap it with folly::Executor::getKeepAliveToken when scheduling.
    folly::Executor* slow_executor() const { return slow_executor_.get(); }

    static ReplError to_repl_error(nuraft::cmd_result_code code);
    int32_t compute_raft_follower_priority();

private:
    /// Reconstruct a ReplicaSet from its persisted repl_dev superblk during start().
    void load_replica_set(sisl::ByteView const& buf, void* meta_cookie);

    /// Match a raft-group-config superblk to its already-loaded ReplicaSet and attach it.
    ReplicaSet* raft_group_config_found(sisl::ByteView const& buf, void* meta_cookie);

    /// Construct the per-group raft state_mgr instance.
    shared< ReplicaSet > create_state_mgr(int32_t srv_id, GroupId const& group_id);

    void add_replica_set(GroupId group_id, shared< ReplicaSet > rs);

    void start_reaper_thread();
    void stop_reaper_thread();
    void gc_replica_sets();
    void gc_repl_reqs();
    void flush_durable_commit_lsn();
    void check_replace_member_status();

private:
    shared< ReplApplication > repl_app_;

    mutable std::shared_mutex rs_mtx_;
    std::map< GroupId, shared< ReplicaSet > > replica_sets_;
    ReplicaId my_uuid_;

    // CPU thread pool for slow-path RPCs and slow internal callbacks. Sized small (2 threads by default) —
    // these paths are infrequent and serialised against each other is fine.
    unique< folly::CPUThreadPoolExecutor > slow_executor_;

    // Outbound RPC client factory holds per-reactor sockets to each peer.
    shared< replication::FollyRpcClientFactory > rpc_client_factory_;

    // Inbound listener owns the server socket on this node's bind port and demuxes frames by group_id
    // into raft_servers via lookup_raft_server().
    shared< replication::FollyRpcListener > rpc_listener_;

    shared< MetaClient > rs_meta_client_;
    shared< MetaClient > rs_raft_cfg_meta_client_;

    iomgr::timer_handle_t gc_timer_hdl_;
    iomgr::timer_handle_t flush_durable_commit_timer_hdl_;
    iomgr::timer_handle_t replace_member_sync_check_timer_hdl_;
};

extern ReplicationManager& repl_service();

//
// ReplApplication — application-facing callbacks. The application implements this interface and hands an instance
// to the ReplicationManager during HomeStore startup.
//
class ReplApplication {
public:
    virtual ~ReplApplication() = default;

    /// Required implementation type of replication for this application.
    virtual ReplImplType get_impl_type() const = 0;

    /// Is the replica recovery needs timeline consistency. Currently only non-timeline-consistent is supported.
    virtual bool need_timeline_consistency() const = 0;

    /// Called when a repl dev is found upon restart. Application returns the per-group Listener.
    virtual shared< ReplicaSetListener > create_replica_set_listener(GroupId group_id) = 0;

    /// Called when the repl dev is destroyed. Application can cleanup resources tied to the listener.
    virtual void destroy_replica_set_listener(GroupId group_id) = 0;

    /// Called after all the repl devs are found upon restart; application can hook secondary recovery here.
    virtual void on_replica_sets_init_completed() = 0;

    /// Given a (peer uuid, group_id), return the peer's (host, port) for that group's RPC channel. The group_id is
    /// passed so applications that map different groups to different ports can do so; applications that use a
    /// single listening port per node can ignore the group_id and return a constant port. ReplicationManager calls
    /// this with `my_uuid` to discover its OWN listening port during start(), so the same callback is the single
    /// source of truth for both peer addresses and the local bind port.
    virtual std::pair< std::string, uint16_t > lookup_peer(ReplicaId uuid, GroupId group_id) const = 0;

    /// Return the current application/server repl uuid.
    virtual ReplicaId get_my_repl_id() const = 0;
};

// Hooks ReplicationManager into the HomeStore checkpoint lifecycle so per-group repl_dev superblks get
// flushed during a CP.
class ReplCPHandler : public CPCallbacks {
public:
    ReplCPHandler() = default;
    ~ReplCPHandler() override = default;

    void on_switchover_cp(CP* cur_cp, CP* new_cp) override;
    folly::coro::Task< bool > cp_flush(CP* cp) override;
    void cp_cleanup(CP* cp) override;
    int cp_progress_percent() override;
};

} // namespace homestore