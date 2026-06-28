#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/uuid/uuid_io.hpp>
#include <folly/coro/Task.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

#include <sisl/fds/buffer.h>
#include <sisl/fds/utils.h>
#include <sisl/metrics/metrics.h>

#include <libnuraft/buffer.hxx>
#include <libnuraft/state_machine.hxx>
#include <libnuraft/state_mgr.hxx>
#include <libnuraft/snapshot.hxx>
#include <libnuraft/callback.hxx>

#include "homestore/blk.h"
#include "homestore/blkdata_service.hpp"
#include "homestore/logstore/log_store.hpp"
#include "homestore/replication/repl_decls.h"
#include "homestore/superblk_handler.hpp"
#include "iomanager/coro_timer.h"

namespace nuraft {
template < typename T >
using ptr = std::shared_ptr< T >;

class buffer;
class cluster_config;
class log_entry;
class log_store;
class raft_server;
class snapshot;
class srv_state;
struct snapshot_obj;
} // namespace nuraft

namespace homestore {

class CP;
class RawBlkStream;
class ReplicaSet;
class ReplicaSetListener;
class ReplicationManager;

VENUM(JournalType, uint8_t,
      HS_DATA_INLINE = 0,           // App data — value bytes live inline in the record.
      HS_DATA_INDIRECT = 1,         // App data — value bytes live on blob_stream; record carries a list of BlkIds.
      HS_CTRL_DESTROY = 2,          // Control message to destroy the replica set.
      HS_CTRL_START_REPLACE = 3,    // Control message to start replacing a member.
      HS_CTRL_COMPLETE_REPLACE = 4, // Control message to complete replacing a member.
)

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ReplLogHeader — fixed prefix on every replicated log_entry's value buffer.
//
// Layout on the wire / disk / in memory:
//   | code (1B) | major_version (1B) | minor_version (1B) | _pad (1B) | value_size (4B) | user_header_size_ (4B)
//   | [user_header bytes ...]
//
// `code` / `major_version` / `minor_version` / `value_size` are filled by HS inside ReplicaSet::write — the app
// cannot set them.  The app writes its own header bytes starting at `user_header_bytes()` (immediately after the
// fixed 12-byte prefix).  user_header_size_ is stamped by alloc() and used by to_nuraft_buffer() to size the
// zero-copy take_ownership wrap.
//
// Allocated via ReplLogHeader::alloc(user_header_size) which returns shared<ReplLogHeader>; HS converts to a
// nuraft::buffer via to_nuraft_buffer(hdr) — the deleter captures the shared_ptr so the storage outlives the
// resulting nuraft::buffer.  Zero copy through the entire write path.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
#pragma pack(1)
struct ReplLogHeader {
    static constexpr uint8_t kMajor = 1;
    static constexpr uint8_t kMinor = 1;

    uint8_t code{0}; // journal_type_t value, set by HS in ReplicaSet::write
    uint16_t major_version{0};
    uint8_t minor_version{0};
    uint32_t value_size{0}; // bytes the app's IOBlob value carries — set by HS

private:
    uint32_t user_header_size_{0}; // stamped by alloc(); used by to_nuraft_buffer() for the take_ownership span

public:
    uint32_t user_header_size() const { return user_header_size_; }
    uint8_t* user_header_bytes() { return r_cast< uint8_t* >(this) + sizeof(ReplLogHeader); }
    uint8_t const* user_header_bytes() const { return r_cast< uint8_t const* >(this) + sizeof(ReplLogHeader); }
    size_t total_size() const { return sizeof(ReplLogHeader) + user_header_size_; }

    // Allocate a ReplLogHeader with `user_header_size` trailing bytes.  The full slab (sizeof(ReplLogHeader)
    // + user_header_size) is allocated once via sisl::make_io_buf_shared; placement-new constructs the prefix in
    // place and stamps user_header_size_.  Returned shared<ReplLogHeader> aliases the underlying IoBufShared
    // control block so the storage stays alive as long as either reference is held.
    static shared< ReplLogHeader > alloc(uint32_t user_header_size) {
        auto ba = sisl::make_io_buf_shared(sizeof(ReplLogHeader) + user_header_size);
        auto* hdr = new (ba->bytes()) ReplLogHeader{};
        hdr->user_header_size_ = user_header_size;
        return shared< ReplLogHeader >(ba, hdr); // aliasing ctor — ba's deleter frees the IoBufShared
    }

    // Zero-copy wrap of a shared<ReplLogHeader> as a nuraft::buffer.  The deleter captures `hdr` so the
    // underlying storage outlives the resulting nuraft::buffer.  Spans `total_size()` bytes (fixed prefix +
    // trailing user_header).
    static nuraft::ptr< nuraft::buffer > to_nuraft_buffer(shared< ReplLogHeader > hdr) {
        size_t total = hdr->total_size();
        return nuraft::buffer::take_ownership(r_cast< uint8_t* >(hdr.get()), total,
                                              [held = std::move(hdr)](nuraft::byte*) noexcept { (void)held; });
    }
};
#pragma pack()
static_assert(sizeof(ReplLogHeader) == 12, "ReplLogHeader fixed prefix must be exactly 12 bytes on the wire/disk");

ENUM(ReplicaSetStage, uint8_t, INIT, ACTIVE, DESTROYING, DESTROYED, PERMANENT_DESTROYED);

// Generic snapshot-context handle that hides the underlying nuraft::snapshot from the application. Serialize/
// deserialize lets the application persist and reload a checkpoint's worth of snapshot state.
class SnapshotContext {
public:
    SnapshotContext(int64_t lsn) : lsn_(lsn) {}
    explicit SnapshotContext(nuraft::snapshot& snp);
    explicit SnapshotContext(sisl::IoBufOwn const& snp_ctx);
    virtual ~SnapshotContext() = default;

    sisl::IoBufOwn serialize();
    void deserialize(sisl::IoBufOwn const& snp_ctx);
    nuraft::ptr< nuraft::snapshot > nuraft_snapshot() { return snapshot_; }
    int64_t get_lsn() const { return lsn_; }

private:
    nuraft::ptr< nuraft::snapshot > snapshot_;
    int64_t lsn_;
};

// `replace_member_ctx_superblk` is the persisted shape; the in-memory `replace_member_ctx` carries the full
// ReplicaMemberInfo (priority, name) while a replace is in progress.
struct ReplaceMemberSuperBlk {
    ReplicaId replica_out;
    ReplicaId replica_in;
};

struct replace_member_ctx {
    ReplicaMemberInfo replica_out;
    ReplicaMemberInfo replica_in;
};

#pragma pack(1)
struct ReplicaSetSuperBlk {
    static constexpr uint64_t REPL_SET_SB_MAGIC = 0xfeeddead4c4f4744;
    static constexpr uint32_t REPL_SET_SB_VERSION = 1;
    static constexpr uint32_t kRSetNameMaxLen = 64;

    uint64_t magic{REPL_SET_SB_MAGIC};
    uint32_t version{REPL_SET_SB_VERSION};
    GroupId group_id;
    uint8_t is_timeline_consistent;
    uint8_t destroy_pending;
    raft_lsn_t last_snapshot_lsn;

    // Per-group log_store ids set by HomeRaftLogStore::create. raft_log_store_id is always valid.
    // free_blks_journal_id is set only when the listener provided a non-null blob_stream() at
    // create time (i.e. large-value optimization is enabled for this group).
    logstore_id_t raft_log_store_id{UINT32_MAX};
    logstore_id_t free_blks_journal_id{UINT32_MAX};

    ReplaceMemberSuperBlk replace_ctx;
    char rset_name[kRSetNameMaxLen];

    uint64_t get_magic() const { return magic; }
    uint32_t get_version() const { return version; }
    void set_rset_name(std::string const& name) {
        std::memset(rset_name, 0, sizeof(rset_name));
        std::memcpy(rset_name, name.data(), std::min(name.size(), sizeof(rset_name) - 1));
    }
};
#pragma pack()

class ReplicaSetMetrics : public sisl::MetricsGroup {
public:
    explicit ReplicaSetMetrics(char const* inst_name);
    ~ReplicaSetMetrics() override;

    ReplicaSetMetrics(ReplicaSetMetrics const&) = delete;
    ReplicaSetMetrics(ReplicaSetMetrics&&) noexcept = delete;
    ReplicaSetMetrics& operator=(ReplicaSetMetrics const&) = delete;
    ReplicaSetMetrics& operator=(ReplicaSetMetrics&&) noexcept = delete;
};

// Application-implemented listener invoked by the ReplicaSet for committed log entries plus lifecycle events
// (snapshot, member-replace, restart). Lives across the lifetime of the ReplicaSet.
class ReplicaSetListener {
public:
    virtual ~ReplicaSetListener() = default;

    void set_replica_set(shared< ReplicaSet > rs) { repl_set_ = rs; }
    shared< ReplicaSet > replica_set() { return repl_set_.lock(); }

    /// Called once per committed log entry on the dedicated commit driver. `lsn` is monotonically increasing.
    /// `header` and `value` are the application bytes that were passed to write().
    ///
    /// `blob_ref` is set only when this entry was written via the large-value optimization (entry was ≥
    /// kLargeValueThreshold at write time and the listener's blob_stream() was non-null). When set, the value
    /// blob lives at this BlkId on the listener's RawBlkStream — applications can write only the BlkId into
    /// their index instead of copying the bytes. The full value bytes are also handed in via `value` so
    /// applications that don't want to track the BlkId can ignore blob_ref entirely.
    virtual void on_commit(int64_t lsn, sisl::Blob const& header, sisl::Blob const& value,
                           std::optional< BlkId > blob_ref) = 0;

    /// Periodic notification of the latest committed LSN; for the listener to checkpoint application state.
    virtual void notify_committed_lsn(int64_t lsn) = 0;

    /// Called when a log entry has been received and pre-committed. Returning false aborts the commit.
    virtual bool on_pre_commit(int64_t lsn, sisl::Blob const& header) = 0;

    /// Called when a previously pre-committed log entry has been rolled back.
    virtual void on_rollback(int64_t lsn, sisl::Blob const& header) = 0;

    /// Called when a previously pre-committed cluster-config entry has been rolled back.
    virtual void on_config_rollback(int64_t lsn) = 0;

    /// Called once on restart, after this ReplicaSet has been reconstructed from disk and before it joins the
    /// raft group. The listener should bring up any application-side state it needs (recovery, indexes, etc).
    virtual void on_restart() = 0;

    /// Called when write() failed to start replicating (not leader, alloc failure, no quorum, etc).
    virtual void on_error(ReplError error, sisl::Blob const& header, sisl::Blob const& key) = 0;

    /// Called when this replica set is being destroyed.
    virtual void on_destroy(GroupId const& group_id) = 0;

    virtual void on_start_replace_member(ReplicaMemberInfo const& member_out, ReplicaMemberInfo const& member_in,
                                         TraceId tid) = 0;
    virtual void on_complete_replace_member(ReplicaMemberInfo const& member_out, ReplicaMemberInfo const& member_in,
                                            TraceId tid) = 0;
    /// Called when nuraft requests a snapshot. The listener creates an application snapshot and returns it via
    /// the snapshot_context.
    virtual AsyncReplResult<> create_snapshot(shared< SnapshotContext > context) = 0;

    /// Called when nuraft completed a baseline resync and is applying the snapshot.
    virtual bool apply_snapshot(shared< SnapshotContext > context) = 0;

    /// Return the last application snapshot saved.
    virtual shared< SnapshotContext > last_snapshot() = 0;

    /// On the leader side during baseline resync, the leader uses this to fetch resync objects from the application.
    virtual int read_snapshot_obj(shared< SnapshotContext > context, shared< nuraft::snapshot_obj > snp_obj) = 0;

    /// On the follower side during baseline resync, the leader's resync objects are handed back to the application.
    virtual void write_snapshot_obj(shared< SnapshotContext > context, shared< nuraft::snapshot_obj > snp_obj) = 0;

    /// Free up the user-defined context inside snapshot_obj allocated during read_snapshot_obj.
    virtual void free_user_snp_ctx(void*& user_snp_ctx) = 0;

    /// Called after restart-time log replay is done and before joining the raft group.
    virtual void on_log_replay_done(GroupId const& group_id) {}

    /// Per-group RawBlkStream for the large-value optimization. Return nullptr to disable — HomeRaftLogStore
    /// will write all entries inline regardless of size, and no free_blks log_store gets created. App is
    /// responsible for picking the BlobDev, creating the stream on first boot, looking it up on restart,
    /// and persisting whatever it needs to round-trip the stream across restarts. Replication never sees
    /// the BlobDev directly. Cached by ReplicaSet at attach_listener time; called once per ReplicaSet life.
    virtual shared< RawBlkStream > blob_stream() { return nullptr; }

private:
    std::weak_ptr< ReplicaSet > repl_set_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ReplicaSet
//
// One instance per raft group on this node. Owns the raft_server, the application's listener, the persisted
// superblk, and the per-group metrics. Implements both `nuraft::state_machine` and `nuraft::state_mgr` so
// nuraft drives commit / rollback / snapshot AND load_config / save_config / save_state all against this one
// object — no separate state_machine class.
//
// write() is the single client entry point — a coroutine that allocates blkids, writes value bytes to the
// blob store, builds the journal entry, and proposes it to raft. Resolves on commit (or error).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
class ReplicaSet : public nuraft::state_machine,
                   public nuraft::state_mgr,
                   public std::enable_shared_from_this< ReplicaSet > {
public:
    ReplicaSet(ReplicationManager& mgr, superblk< ReplicaSetSuperBlk >&& rd_sb, bool load_existing);
    ~ReplicaSet() override;

    ReplicaSet(ReplicaSet const&) = delete;
    ReplicaSet& operator=(ReplicaSet const&) = delete;

    /// Bring the raft_server up and join the group. Called by ReplicationManager after construction.
    bool join_group();

    // ── Public client API ────────────────────────────────────────────────────────────────────────────────────

    /// Single client write entry point. App pre-fills its own header bytes inside the ReplLogHeader's trailing
    /// region (after the fixed 12-byte prefix); HS stamps code/major/minor/value_size, wraps both `hdr` and
    /// `value` zero-copy into a 2-part log_entry chain, and proposes through raft. Resolves when commit fires
    /// (the listener's on_commit() has already run before this returns successfully) or on error.
    folly::coro::Task< ReplResult<> > write(shared< ReplLogHeader > hdr, sisl::IoBufSpan const& value, TraceId tid = 0);

    // ── Membership / leadership ──────────────────────────────────────────────────────────────────────────────

    folly::coro::Task< ReplResult<> > become_leader();
    folly::coro::Task< ReplResult<> > start_replace_member(ReplicaMemberInfo const& member_out,
                                                           ReplicaMemberInfo const& member_in,
                                                           uint32_t commit_quorum = 0, TraceId tid = 0);
    folly::coro::Task< ReplResult<> > complete_replace_member(ReplicaMemberInfo const& member_out,
                                                              ReplicaMemberInfo const& member_in,
                                                              uint32_t commit_quorum = 0, TraceId tid = 0);
    folly::coro::Task< ReplResult<> > flip_learner_flag(ReplicaMemberInfo const& member, bool target,
                                                        uint32_t commit_quorum, bool wait_and_verify = true,
                                                        TraceId tid = 0);

    ReplError do_add_member(ReplicaMemberInfo const& member, TraceId tid = 0);
    ReplError do_remove_member(ReplicaMemberInfo const& member, TraceId tid = 0);
    ReplError do_flip_learner(ReplicaMemberInfo const& member, bool target, bool wait_and_verify, TraceId tid = 0);
    ReplError set_priority(ReplicaId const& member, int32_t priority, TraceId tid = 0);

    folly::SemiFuture< ReplError > destroy_group();

    // ── Synchronous accessors ────────────────────────────────────────────────────────────────────────────────

    bool is_leader() const;
    ReplicaId get_leader_id() const;
    std::vector< PeerInfo > get_replication_status() const;
    std::set< ReplicaId > get_active_peers() const;

    GroupId const& group_id() const { return group_id_; }
    std::string group_id_str() const { return boost::uuids::to_string(group_id_); }
    std::string const& rset_name() const { return rset_name_; }
    std::string const& identify_str() const { return identify_str_; }
    std::string my_replica_id_str() const { return boost::uuids::to_string(my_uuid_); }

    void set_custom_rset_name(std::string const& name);

    raft_lsn_t get_last_commit_lsn() const { return commit_upto_lsn_.load(); }
    void set_last_commit_lsn(raft_lsn_t lsn) { commit_upto_lsn_.store(lsn); }
    raft_lsn_t get_last_append_lsn();
    uint32_t get_blk_size() const;

    bool is_destroy_pending() const;
    bool is_destroyed() const;
    Clock::time_point destroyed_time() const { return destroyed_time_; }
    bool is_ready_for_traffic() const;

    nuraft::raft_server* raft_server();
    ReplicaSetMetrics& metrics() { return *metrics_; }

    void attach_listener(shared< ReplicaSetListener > listener);
    void detach_listener();
    shared< ReplicaSetListener > get_listener() { return listener_; }

    void purge();
    void stop();
    void clear_chunk_req(chunk_num_t chunk_id);

    shared< SnapshotContext > deserialize_snapshot_context(sisl::IoBufOwn& snp_ctx);

    // ── Hooks invoked by ReplicationManager ─────────────────────────────────────────────────────────────────

    void use_config(json_superblk raft_config_sb);
    void on_restart();
    void force_leave();
    void flush_durable_commit_lsn();
    void check_replace_member_status();
    void gc_repl_reqs();
    void on_compact(raft_lsn_t upto_lsn) { compact_lsn_.store(upto_lsn); }
    void on_log_found(logstore_seq_num_t lsn, log_buffer buf, void* ctx);
    void become_leader_cb();
    void become_follower_cb();
    void become_ready();
    bool need_skip_processing(raft_lsn_t lsn) const { return lsn <= rd_sb_->last_snapshot_lsn; }

    // ── CP integration ──────────────────────────────────────────────────────────────────────────────────────

    void cp_flush(CP* cp);
    void cp_cleanup(CP* cp);

    // ── nuraft::state_machine overrides ─────────────────────────────────────────────────────────────────────

    folly::coro::Task< RaftBufferPtr > commit_ext_chained(ulong log_idx,
                                                          std::vector< RaftBufferPtr > const& bufs) override;
    folly::coro::Task< RaftBufferPtr > commit_ext(ext_op_params const& params) override;
    RaftBufferPtr pre_commit_ext(ext_op_params const& params) override;
    void rollback_ext(ext_op_params const& params) override;
    void commit_config(ulong log_idx, nuraft::ptr< nuraft::cluster_config >& new_conf) override;
    void rollback_config(ulong log_idx, nuraft::ptr< nuraft::cluster_config >& conf) override;
    void create_snapshot(nuraft::snapshot& s, nuraft::async_result< bool >::handler_type& when_done) override;
    bool apply_snapshot(nuraft::snapshot& s) override;
    nuraft::ptr< nuraft::snapshot > last_snapshot() override;
    ulong last_commit_index() override;
    void save_logical_snp_obj(nuraft::snapshot& s, ulong& obj_id, nuraft::buffer& data, bool is_first_obj,
                              bool is_last_obj) override;
    int read_logical_snp_obj(nuraft::snapshot& s, void*& user_snp_ctx, ulong obj_id, RaftBufferPtr& data_out,
                             bool& is_last_obj) override;
    void free_user_snp_ctx(void*& user_snp_ctx) override;

    // ── nuraft::state_mgr overrides ─────────────────────────────────────────────────────────────────────────

    nuraft::ptr< nuraft::cluster_config > load_config() override;
    void save_config(nuraft::cluster_config const& config) override;
    void save_state(nuraft::srv_state const& state) override;
    nuraft::ptr< nuraft::srv_state > read_state() override;
    nuraft::ptr< nuraft::log_store > load_log_store() override;
    int32_t server_id() override;
    void system_exit(int exit_code) override;

    /// state_mgr also needs a state_machine — return ourselves.
    std::shared_ptr< nuraft::state_machine > get_state_machine() override {
        return std::static_pointer_cast< nuraft::state_machine >(shared_from_this());
    }

    uint32_t get_logstore_id() const;
    void permanent_destroy();
    void leave();

    nuraft::cb_func::ReturnCode raft_event(nuraft::cb_func::Type, nuraft::cb_func::Param*);

private:
    void handle_error(int64_t lsn, sisl::Blob const& header, sisl::Blob const& key, ReplError err);
    void commit_blk(int64_t lsn, std::vector< MultiBlkId > const& blkids);
    void reset_quorum_size(uint32_t commit_quorum, TraceId tid);
    void report_blk_metrics_if_needed(uint32_t total_blks, uint32_t actual_blks);
    void set_log_store_last_durable_lsn(store_lsn_t lsn);
    nuraft::cmd_result_code retry_when_config_changing(std::function< nuraft::cmd_result_code() > const& func,
                                                       TraceId tid = 0);
    bool wait_and_check(std::function< bool() > const& check_func, uint32_t timeout_ms, uint32_t interval_ms = 100);

    // ── Members ─────────────────────────────────────────────────────────────────────────────────────────────

    ReplicationManager& mgr_;
    GroupId group_id_;
    ReplicaId my_uuid_;
    int32_t raft_server_id_;
    std::string rset_name_;
    std::string identify_str_;

    shared< ReplicaSetListener > listener_;
    shared< nuraft::log_store > log_store_;
    shared< nuraft::raft_server > raft_server_;

    sisl::urcu_scoped_ptr< ReplicaSetStage > stage_;

    superblk< ReplicaSetSuperBlk > rd_sb_;
    ReplicaSetSuperBlk sb_in_mem_;
    json_superblk raft_config_sb_;

    std::mutex sb_mtx_;
    std::mutex config_mtx_;

    std::atomic< raft_lsn_t > commit_upto_lsn_{0};
    std::atomic< raft_lsn_t > compact_lsn_{0};
    raft_lsn_t last_flushed_commit_lsn_{0};

    iomanager::CoroTimer sb_flush_timer_;
    Clock::time_point destroyed_time_;
    folly::Promise< ReplError > destroy_promise_;

    unique< ReplicaSetMetrics > metrics_;
};

} // namespace homestore