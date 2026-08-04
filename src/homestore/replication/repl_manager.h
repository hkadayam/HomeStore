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

#include "common/async.h"
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "iomanager/coro_timer.h"

#include "sisl/fds/buffer.h"
#include "sisl/fds/enum.h"

#include <nlohmann/json.hpp>

#include "homestore/base/hs_runtime_config.h"
#include "homestore/checkpoint/cp_mgr.h"
#include "homestore/meta/meta_blk.h"
#include "homestore/replication/repl_decls.h"

#include <libnuraft/basic_types.hxx> // nuraft::group_id_t used in the public API signatures below

namespace nuraft {
class raft_server;
class srv_config;
enum cmd_result_code : int;
} // namespace nuraft

namespace homestore {

// ReplicationManager-scoped log wrapper — mirrors RS_LOG (which is per-ReplicaSet).  Adds a fixed "ReplMgr"
// tag so replication log lines from the manager are grepable, and threads a trace_id through when the caller
// has one; use NO_TRACE_ID for background/lifecycle work.
#define RM_LOG(level, traceID, ...)                                                                                    \
    HS_DETAILED_LOG(level, replication, , "rs", "ReplMgr", "trace_id", traceID, ##__VA_ARGS__)

namespace replication {
class FollyRpcClientFactory;
class FollyRpcListener;
} // namespace replication

class ReplicaSet;
class ReplicaSetListener;
class ReplApplication;
struct ReplicaSetOptions;
struct repl_dev_superblk;

// Priority policy for a member added to an existing group AFTER bootstrap.  Reads default_leader_priority
// and priority_decay_coefficient from the runtime consensus config so operators can tune the bias toward
// (or away from) new-member leadership without recompiling.  Clamped to a minimum of 1 so a peer is never
// completely locked out of leadership by aggressive decay.
inline int32_t new_member_priority() {
    auto const p = static_cast< int32_t >(HS_RUNTIME_CONFIG(consensus.default_leader_priority) *
                                          HS_RUNTIME_CONFIG(consensus.priority_decay_coefficient));
    return p > 0 ? p : 1;
}

class ReplicationManager {
public:
    explicit ReplicationManager(shared< ReplApplication > repl_app);
    ~ReplicationManager();

    ReplicationManager(ReplicationManager const&) = delete;
    ReplicationManager& operator=(ReplicationManager const&) = delete;

    // Phased bring-up, driven by HomeStore (self-register via Managers::init_repl_mgr):
    //   create()       — first-time boot: infra + register CP consumer + start_engine() (goes live, no sets).
    //   load()         — recovery: infra + reconstruct replica sets (no engines); start_engine() deferred.
    //   start_engine() — launch every reconstructed set's raft engine, then go live (open the listener + start
    //                    the maintenance timers).  Called by create() (first boot, empty set loop) and by
    //                    HomeStore::replay() (recovery, after LogStoreManager::replay()).
    static Async< void > create(shared< ReplApplication > repl_app);
    static Async< void > load(shared< ReplApplication > repl_app);
    Async< void > start_engine();
    Async< void > stop();

    // -------- Public Task<>-driven API (proposer-style operations) --------

    /// Consumer-initiated group create.  Bootstraps a fresh cluster config containing `members` (with UUIDs
    /// stamped into srv_config.aux), constructs a ReplicaSet, and brings it up.  Membership changes AFTER
    /// the group is created (add_member / remove_member / replace_member / flip_learner_flag) go directly
    /// on the ReplicaSet handle — no intermediate call through the manager.
    Async< ReplResult< shared< ReplicaSet > > >
    create_replica_set(GroupId group_id, std::set< ReplicaId > const& members, ReplicaSetOptions const& options);

    /// Construct a new ReplicaSet for an unknown group_id by asking the application via
    /// ReplApplication::create_replica_set_listener.  Peer-initiated path — called by the rpc listener when
    /// a message arrives for a group we do not know about.  Returns nullptr if the application rejects the
    /// group or if start fails.  Resolves after the new replica's engine is up.
    Async< nuraft::raft_server* > create_replica_set_on_demand(nuraft::group_id_t const& gid);

    /// Schedule a replica set for destruction.  Delegates to `rs->destroy()` (leader-initiated) and hands
    /// the final teardown off to the reaper, which calls `rs->finish_destroy_local()` after a grace period
    /// then erases the group from the registry.
    Async< ReplError > remove_replica_set(GroupId group_id);

    /// Fan out to every replica set: raft_server->schedule_snapshot_creation() + await completion.  The
    /// resulting snapshot advances raft's last_snapshot, which lets its subsequent internal compact() trim
    /// the log store's head_lsn.  Serial per set (snapshot creation is IO-heavy).  Groups whose raft engine
    /// hasn't started yet are skipped.
    Async< void > truncate();

    // -------- Synchronous accessors --------

    ReplResult< shared< ReplicaSet > > get_replica_set(GroupId group_id) const;
    void iterate_replica_sets(std::function< void(cshared< ReplicaSet >&) > const& cb);

    ReplicaId get_my_repl_id() const { return my_uuid_; }

    /// Format `<host>:<port>` for a given peer via ReplApplication::lookup_peer. Empty string if unknown.
    std::string lookup_peer_addr(ReplicaId const& peer) const;

    /// Route an inbound wire frame's group_id to the owning raft_server. Returns nullptr if no replica
    /// set is registered for that group.
    nuraft::raft_server* lookup_raft_server(nuraft::group_id_t const& gid) const;

    /// Folly transport — used by per-group ReplicaSet::start to build the consensus engine's context.
    shared< replication::FollyRpcClientFactory > rpc_client_factory() const { return rpc_client_factory_; }
    shared< replication::FollyRpcListener > rpc_listener() const { return rpc_listener_; }

    /// CPU-intensive executor — a thread pool reserved for RPC handlers that would otherwise starve the
    /// reactor's event loop (heavy signature verification, snapshot object hashing, etc.).  The rpc listener
    /// consults is_cpu_intensive_rpc(msg_type) to decide whether to reschedule off the reactor;
    /// Returned as a raw pointer so callers can wrap with folly::Executor::getKeepAliveToken when scheduling.
    folly::Executor* cpu_executor() const { return cpu_executor_.get(); }

    static ReplError to_repl_error(nuraft::cmd_result_code code);

private:
    /// Common infra for create()/load(): resolve my uuid + bind port, build executors, rpc client factory, and
    /// the (not-yet-listening) rpc listener, and register the two meta clients.  No CP consumer, no timers.
    Async< void > setup_infra();

    /// Recovery reconstruction: the two-pass raft-config + SB walk that rebuilds every ReplicaSet via
    /// ReplicaSet::load() (no raft engine), destroying orphaned configs.  Called only from load().
    Async< void > reconstruct_replica_sets();

    /// Reconstruct a ReplicaSet from its persisted SB during load().  Reads the group_id off the SB,
    /// pulls the matching raft config out of pending_configs_ (populated by raft_group_config_found which
    /// ran first), and drives rs->start().  If no matching config is in pending_configs_, the SB is a
    /// no-config orphan and gets destroyed.
    Async< void > load_replica_set(MetaBlk const& blk, sisl::IoBufView data);

    /// First-pass visitor for the raft-config walk.  Deserialises the JSON, extracts the group_id, and
    /// stashes (MetaBlkWrapper, json) in pending_configs_ keyed by group_id.  load_replica_set consumes
    /// entries out of this map on the second pass.
    Async< void > raft_group_config_found(MetaBlk const& blk, sisl::IoBufView data);

    /// Reaper body — walks replica_sets_ once, finish_destroy_local()s any set past its grace period, and
    /// erases them from the registry.  Invoked from gc_timer_ on a recurring cadence.  Async because
    /// finish_destroy_local co_awaits SB writes.
    Async< void > gc_replica_sets();

private:
    shared< ReplApplication > repl_app_;

    mutable std::shared_mutex rs_mtx_;
    std::map< GroupId, shared< ReplicaSet > > replica_sets_;
    // Group_ids currently in the "listener created, MetaBlks allocated / raft server coming up" window.
    // Guarded by rs_mtx_.  Both create paths insert here before any async work and remove either atomically
    // with the replica_sets_ insert (success) or on failure.  A second arrival that sees the group_id here
    // knows a create is racing and bails: create_replica_set returns SERVER_ALREADY_EXISTS; on-demand
    // returns nullptr and lets the peer's RPC retry hit the completed replica.
    std::set< GroupId > pending_creates_;
    ReplicaId my_uuid_;

    // CPU thread pool for RPC handlers flagged CPU-intensive.  Sized small (2 threads) — none are flagged
    // today; scale later when we actually identify hot spots.
    unique< folly::CPUThreadPoolExecutor > cpu_executor_;

    // Outbound RPC client factory holds per-reactor sockets to each peer.
    shared< replication::FollyRpcClientFactory > rpc_client_factory_;

    // Inbound listener owns the server socket on this node's bind port and demuxes frames by group_id
    // into raft_servers via lookup_raft_server().
    shared< replication::FollyRpcListener > rpc_listener_;

    shared< MetaClient > rs_meta_client_;
    shared< MetaClient > rs_raft_cfg_meta_client_;

    // Transient — populated by raft_group_config_found during the first-pass config walk in start(), then
    // consumed by load_replica_set during the second-pass SB walk.  Any entries still present after the
    // SB walk are orphan configs (no matching SB) and get destroyed.  Empty after start() returns.
    std::map< GroupId, std::pair< MetaBlkWrapper, nlohmann::json > > pending_configs_;

    iomanager::CoroTimer gc_timer_;
};

extern ReplicationManager& repl_service();

//
// ReplApplication — application-facing callbacks. The application implements this interface and hands an instance
// to the ReplicationManager during HomeStore startup.
//
class ReplApplication {
public:
    virtual ~ReplApplication() = default;

    /// Is the replica recovery needs timeline consistency. Currently only non-timeline-consistent is supported.
    virtual bool need_timeline_consistency() const = 0;

    /// Called on every ReplicaSet bring-up (both create paths and the restart reload path).  `load_existing`
    /// is true when the ReplicationManager is reloading a group off disk during start(), false for a fresh
    /// create (either user-initiated create_replica_set or peer-initiated on-demand).  Return nullptr to
    /// decline hosting this group.
    virtual shared< ReplicaSetListener > create_replica_set_listener(GroupId group_id, bool load_existing) = 0;

    /// Called by the reaper after a replica set has been physically destroyed and erased from the registry.
    /// Application can cleanup resources tied to the listener (separate from the listener's own on_destroy).
    virtual void destroy_replica_set_listener(GroupId group_id) = 0;

    /// Given a (peer uuid, group_id), return the peer's (host, port) for that group's RPC channel. The group_id is
    /// passed so applications that map different groups to different ports can do so; applications that use a
    /// single listening port per node can ignore the group_id and return a constant port. ReplicationManager calls
    /// this with `my_uuid` to discover its OWN listening port during start(), so the same callback is the single
    /// source of truth for both peer addresses and the local bind port.
    virtual std::pair< std::string, uint16_t > lookup_peer(ReplicaId uuid, GroupId group_id) const = 0;

    /// Return the current application/server repl uuid.
    virtual ReplicaId get_my_repl_id() const = 0;
};

} // namespace homestore