#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid_io.hpp>
#include "common/async.h"
#include <folly/coro/Baton.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

#include <sisl/fds/buffer.h>
#include <sisl/fds/utils.h>
#include <sisl/metrics/metrics.h>

#include <libnuraft/buffer.hxx>
#include <libnuraft/raft_params.hxx>
#include <libnuraft/state_machine.hxx>
#include <libnuraft/state_mgr.hxx>
#include <libnuraft/snapshot.hxx>
#include <libnuraft/callback.hxx>

#include "homestore/base/blk.h"
#include "homestore/logstore/log_stream.h" // logstore_id_t
#include "homestore/replication/repl_decls.h"
#include "homestore/meta/meta_blk.h"

#include <nlohmann/json.hpp>

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
} // namespace nuraft

namespace homestore {

#define NO_TRACE_ID "n/a"
#define RS_LOG(level, traceID, ...)                                                                                    \
    HS_DETAILED_LOG(level, replication, , "rs", identify_str(), "trace_id", traceID, ##__VA_ARGS__)

class HomeRaftLogStore;
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
      HS_CTRL_TRUNCATE = 5,         // Advance app-authorized compact ceiling. user_header carries int64_t lsn.
)

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ReplLogHeader — fixed prefix on every replicated log_entry's value buffer.  HS-internal: ReplicaSet::write
// allocates the slab, stamps the fields, and prefixes the user_header bytes.  The app never constructs one.
//
// Layout on the wire / disk / in memory:
//   | code (1B) | major_version (2B) | minor_version (1B) | value_size (4B) | user_header_size_ (4B)
//   | [user_header bytes ...]
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
#pragma pack(1)
struct ReplLogHeader {
    static constexpr uint8_t kMajor = 1;
    static constexpr uint8_t kMinor = 1;

    uint8_t code{0};
    uint16_t major_version{0};
    uint8_t minor_version{0};
    uint32_t value_size{0};
    uint32_t user_header_size_{0};
    // Snapshot of the proposer's commit_upto_lsn_ at the moment this entry was built — the per-entry commit
    // proof.  During log-store replay a durable entry stamped C proves every entry <= C was quorum-committed
    // pre-crash, so the replay callback dispatches exactly the proven prefix above the checkpt floor and
    // leaves unproven tail entries to nuraft's re-commit.  Zero-filled for the very first entries written
    // before any commit has advanced past 0.
    int64_t commit_lsn_at_write{0};

    uint32_t user_header_size() const { return user_header_size_; }
    uint8_t* user_header_bytes() { return r_cast< uint8_t* >(this) + sizeof(ReplLogHeader); }
    uint8_t const* user_header_bytes() const { return r_cast< uint8_t const* >(this) + sizeof(ReplLogHeader); }
    size_t total_size() const { return sizeof(ReplLogHeader) + user_header_size_; }
};
#pragma pack()
static_assert(sizeof(ReplLogHeader) == 20, "ReplLogHeader fixed prefix must be exactly 20 bytes on the wire/disk");

ENUM(ReplicaSetStage, uint8_t, INIT, ACTIVE, DESTROYING, DESTROYED, PERMANENT_DESTROYED);

// Raft role from this replica's perspective, delivered to the listener on every role transition so
// applications can gate work (e.g. reject writes when a follower, resume when leader, hold off on
// voting-participant assumptions when learner).
ENUM(ReplicaRole, uint8_t, LEADER, FOLLOWER, LEARNER);

// Per-group config chosen by whoever brings the group up: user-initiated create passes it as an arg to
// create_replica_set; peer-initiated on-demand AND restart/reload query listener->replica_set_options()
// right after listener construction.  Not persisted in the ReplicaSet SB — the listener (i.e., the app) is
// the single source of truth, responsible for its own persistence if it wants durability across restarts.
struct ReplicaSetOptions {
    uint32_t snapshot_distance{0};          // commits between Replication auto-snapshots (0 = manual only)
    uint32_t preserve_log_count{100000};    // log-entries floor past compact, threaded to LogStoreConfig
    bool allow_user_driven_truncate{false}; // when true, ReplicaSet::advance_truncate_upto is honored
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ReplSnapshot / ReplSnapshot::Builder
//
// Durable, readable snapshot handle applications derive from to attach their own state (path to persisted
// snapshot, DB pointer, file handle, whatever).  Every snapshot is uniquely keyed by `lsn` — the raft LSN at
// which state was captured.  read_next_chunk is always callable and streams the snapshot's contents in
// chunks; app defines the `obj_id` scheme (byte offset, chunk index, whatever).
//
// Builder is a SEPARATE class (not derived from ReplSnapshot).  It's the transient write-accumulator on the
// follower during baseline resync: build_snapshot returns one, ReplicaSet feeds chunks through
// write_next_chunk, and after the last chunk it calls finalize() to get the resulting shared<ReplSnapshot>
// that then flows into apply_snapshot and becomes what last_snapshot() returns afterwards.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
class ReplSnapshot {
public:
    class Builder;

    explicit ReplSnapshot(lsn_t lsn) : lsn_(lsn) {}
    virtual ~ReplSnapshot() = default;

    /// Read one chunk of the snapshot at cursor `obj_id`.  App returns the bytes as an IoBufView (owning
    /// a refcount to its backing storage) and sets is_last when done.  Returning an error aborts the
    /// sync — nuraft discards the transfer and retries from cursor 0 later.
    virtual AsyncReplResult< sisl::IoBufView > read_chunk(uint64_t& obj_id, bool& is_last) = 0;

    /// Raft LSN this snapshot is anchored at.  Unique per snapshot in the group's history.
    lsn_t lsn() const { return lsn_; }

protected:
    friend class ReplicaSet;
    lsn_t lsn_;
    /// Wrapped nuraft::snapshot — ReplicaSet fills this in after the listener returns the object.  App
    /// never touches it directly.
    RaftSnapshotPtr nuraft_snapshot_;
};

/// Transient write-side accumulator.  Contains — not is — a ReplSnapshot; finalize() produces the readable
/// snapshot.  ReplicaSet holds the Builder for the duration of the transfer, releases it after finalize
/// returns.  App derives to attach whatever build-side state it needs (target file, byte counter, running
/// hash, etc.).
class ReplSnapshot::Builder {
public:
    explicit Builder(lsn_t lsn) : lsn_(lsn) {}
    virtual ~Builder() = default;

    /// Accept one chunk at cursor `obj_id`.  App updates obj_id to whatever cursor value it wants the
    /// leader's next read to see.  No is_first / is_last plumbing — a Builder is freshly constructed via
    /// build_snapshot at the start of every transfer (so state is always empty on the first call), and
    /// finalize() signals the end.
    virtual Async< void > write_chunk(uint64_t& obj_id, sisl::IoBufView const& data) = 0;

    /// Called by ReplicaSet when the in-progress transfer is aborted (nuraft resets and restarts from
    /// obj_id=0, or the sync times out).  App must undo any partial state it accumulated via write_chunk
    /// calls — writes to live data need rollback, temp files need cleanup, external systems need revert.
    /// After abort() returns, the Builder is dropped.  Guaranteed NOT to be called after finalize().
    virtual void abort() = 0;

    /// Called by ReplicaSet after the last chunk lands.  App constructs and returns its derived
    /// ReplSnapshot representing the accumulated data.  The Builder is dropped by ReplicaSet after this
    /// returns; the returned snapshot flows into apply_snapshot and thereafter is what last_snapshot()
    /// should return.
    virtual shared< ReplSnapshot > finalize() = 0;

    lsn_t lsn() const { return lsn_; }

protected:
    lsn_t lsn_;
};

// UUID string (36 chars) + null terminator + slack.  Task ids are generated at replace_member() start and
// persisted in ReplaceMemberSuperBlk so crash-recovery resume can distinguish "this attempt" from a stale
// intent (e.g., a later leader ran a different replace).  Empty task_id[0] == '\0' means no replace pending.
static constexpr size_t kReplaceTaskIdLen = 40;

struct ReplaceMemberSuperBlk {
    ReplicaId replica_out;
    ReplicaId replica_in;
    char task_id[kReplaceTaskIdLen];
};

// On-wire payload for HS_CTRL_START_REPLACE / HS_CTRL_COMPLETE_REPLACE log entries.  Explicitly packed so the
// layout is stable across all replicas regardless of compiler alignment choices — the CTRL branch of
// dispatch_commit reads it out on every node to fire on_start_replace_member / on_complete_replace_member.
// task_id also lands here so followers persist it in their SB (STAGE1) and clear it later (STAGE2), keeping
// the resume invariant consistent across the whole group even if leadership changes mid-flight.  task_id is
// also handed to the listener as the correlation ID for the whole replace op — one identifier, no separate
// caller-side trace_id.
#pragma pack(1)
struct ReplaceMemberCtrlHeader {
    ReplicaId out_id;
    char out_name[ReplicaMemberInfo::max_name_len];
    int32_t out_priority;

    ReplicaId in_id;
    char in_name[ReplicaMemberInfo::max_name_len];
    int32_t in_priority;

    char task_id[kReplaceTaskIdLen];
};
#pragma pack()

#pragma pack(1)
struct ReplicaSetSuperBlk {
    static constexpr uint64_t REPL_SET_SB_MAGIC = 0xfeeddead4c4f4744;
    static constexpr uint32_t REPL_SET_SB_VERSION = 1;
    static constexpr uint32_t kRSetNameMaxLen = 64;

    // In-class member initialisers cover every field so `ReplicaSetSuperBlk{}` gives a fully-zeroed record
    // with the magic / version / sentinel ids populated.  Callers only override fields specific to their
    // create context (group_id, is_timeline_consistent, rset_name).
    uint64_t magic{REPL_SET_SB_MAGIC};
    uint32_t version{REPL_SET_SB_VERSION};
    GroupId group_id{};
    uint8_t is_timeline_consistent{0};
    uint8_t destroy_pending{0};
    raft_lsn_t last_snapshot_lsn{0};

    // NOTE: this SB carries NO commit watermark.  The applied-durable watermark lives in the raft LogStore's
    // checkpt_lsn (captured from ReplicaSet's commit_upto at every CP switchover via the commit-watermark
    // callback), and per-entry commit proof lives in ReplLogHeader.commit_lsn_at_write.  Together they cover
    // every recovery case, including "idle for hours then crashed" — the last CP's checkpt already equals the
    // commit watermark when nothing new was written.

    // Per-group log_store ids set by HomeRaftLogStore::create. raft_log_store_id is always valid.
    // free_blks_journal_id is set only when the listener provided a non-null blob_stream() at
    // create time (i.e. large-value optimization is enabled for this group).
    logstore_id_t raft_log_store_id{UINT32_MAX};
    logstore_id_t free_blks_journal_id{UINT32_MAX};

    ReplaceMemberSuperBlk replace_member_sb{};
    char rset_name[kRSetNameMaxLen]{};

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
    ~ReplicaSetMetrics();

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
    /// `blob_refs` is populated only when this entry was written via the large-value optimization (entry was
    /// ≥ kLargeValueThreshold at write time and the listener's blob_stream() was non-null).  When non-empty,
    /// the value blob lives at these BlkIds on the listener's RawBlkStream — applications can persist just
    /// the BlkIds into their index instead of copying the bytes.  The full value bytes are also handed in via
    /// `value` so applications that don't want to track BlkIds can ignore blob_refs entirely.  Multiple BlkIds
    /// only occur when the allocator handed back a discontiguous run for a single value.
    /// Awaited by the commit path — commit does not advance until the application has finished applying the
    /// entry, so "committed" means "durably applied by the app".
    virtual Async< void > on_commit(int64_t lsn, sisl::Blob const& header, sisl::Blob const& value,
                                    BlkIds const& blob_refs) = 0;

    /// Called when a log entry has been received and pre-committed. Returning false aborts the commit.
    virtual bool on_pre_commit(int64_t lsn, sisl::Blob const& header) = 0;

    /// Called when a previously pre-committed log entry has been rolled back.  Awaited, like on_commit, so the
    /// app's un-apply completes before the rollback path proceeds.
    virtual Async< void > on_rollback(int64_t lsn, sisl::Blob const& header) = 0;

    /// Called when a previously pre-committed cluster-config entry has been rolled back.
    virtual void on_config_rollback(int64_t lsn) = 0;

    /// Called on every raft role transition observed on this replica — LEADER after promotion, FOLLOWER after
    /// demotion / joining a running cluster, LEARNER while catching up prior to becoming a voter.  Fired from
    /// nuraft's cb_func dispatch; applications should treat the callback as best-effort ordering and idempotent
    /// (the same role may be delivered more than once, e.g. JoinedCluster then BecomeFollower).
    virtual void on_change_in_role(ReplicaRole new_role) = 0;

    /// Called when this replica set is being destroyed.
    virtual void on_destroy(GroupId const& group_id) = 0;

    /// Fired on every replica when HS_CTRL_START_REPLACE / HS_CTRL_COMPLETE_REPLACE respectively commit.
    /// `task_id` is a UUID-string that uniquely identifies this replace attempt across the group; it's the
    /// correlator to use for logging and any per-op state the app keeps.
    virtual void on_start_replace_member(ReplicaMemberInfo const& member_out, ReplicaMemberInfo const& member_in,
                                         std::string_view task_id) = 0;
    virtual void on_complete_replace_member(ReplicaMemberInfo const& member_out, ReplicaMemberInfo const& member_in,
                                            std::string_view task_id) = 0;

    /// Fires on every committed cluster_config that changes group membership (pure add or remove — replaces
    /// still use the start/complete_replace_member pair as well).  `added` and `removed` are disjoint UUID
    /// sets against the previously-committed membership.  ReplicaSet tracks the running committed member set
    /// internally so consumers don't have to.
    virtual void on_membership_change(std::set< ReplicaId > const& added, std::set< ReplicaId > const& removed) = 0;

    /// Leader: capture live state at `lsn` durably.  App must persist consumer state (typically by
    /// triggering a CP flush themselves) before returning success — the snapshot at lsn is only sound if
    /// consumer's own durability watermark has caught up.  The returned handle is thereafter readable via
    /// read_chunk for follower baseline resync.
    virtual AsyncReplResult< shared< ReplSnapshot > > take_snapshot(lsn_t lsn) = 0;

    /// Follower: allocate an empty Builder to accumulate chunks at `lsn`.  ReplicaSet feeds write_chunk
    /// calls into it, then either finalize()s (on complete transfer) or abort()s (on nuraft-triggered
    /// restart of the transfer).
    virtual AsyncReplResult< shared< ReplSnapshot::Builder > > build_snapshot(lsn_t lsn) = 0;

    /// Follower: mainline the finalized snapshot into live state.  App applies the snapshot to whatever
    /// data structures it manages.  Durability: ReplicaSet advances SB watermarks + forces a CP flush after
    /// this returns true, which persists anything under CP management (Index, BlkAllocator, VDev, MetaBlk).
    /// If the app persists state OUTSIDE CP management (raw journals, external systems), the app must
    /// ensure its own durability before returning true — a crash between this return and the CP flush
    /// completing would lose that outside-CP state.  From this point on `snap` is what last_snapshot()
    /// should return.
    virtual bool apply_snapshot(shared< ReplSnapshot > snap) = 0;

    /// Return the current durable snapshot, or null if none exists.  Queried by nuraft at the start of
    /// every follower's baseline resync so the read cursor has something to open on.
    virtual shared< ReplSnapshot > last_snapshot() = 0;

    /// Repl signals: done with this snapshot from its side.  Fires when the transfer session ends (both
    /// success and failure paths on the leader, via free_user_snp_ctx) and after every apply_snapshot on
    /// the follower (whether the app accepted or rejected the snapshot).  App decides whether to drop its
    /// own refs or keep the object alive for other uses (DB time-travel, backup, etc.).
    virtual void release_snapshot(shared< ReplSnapshot > snap) = 0;

    /// Per-group options.  Queried by ReplicaSet immediately after listener construction on both the
    /// peer-initiated on-demand path and the restart/reload path — the returned options drive raft_params
    /// (snapshot_distance), LogStore (preserve_log_count), and truncate gating (allow_user_driven_truncate).
    /// Not consulted on the user-initiated create path (options passed as an arg to create_replica_set).
    /// Must be answerable as soon as the listener exists — no I/O or deferred init.  Same group_id across
    /// replicas must yield the same options; divergent options across replicas break replicated compact.
    /// App is responsible for its own persistence of options if it wants them stable across restarts.
    virtual ReplicaSetOptions replica_set_options() = 0;

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
    ReplicaSet(ReplicationManager& mgr, MetaBlkWrapper sb_mblk, ReplicaSetOptions const& options);
    ~ReplicaSet() override;

    ReplicaSet(ReplicaSet const&) = delete;
    ReplicaSet& operator=(ReplicaSet const&) = delete;

    /////////////////////////////////////// Public API Section ///////////////////////////////////////////////

    // Consumer-facing surface — anything a caller holding a shared<ReplicaSet> can invoke directly on that
    // handle.  Kept consensus-engine agnostic on purpose: no nuraft/raft type leaks, no HS-internal control
    // codes.  Every entry carries a brief usage note so this header is enough to consume from.

    /// Single write entry point.  The header may be any sisl::IoBuf flavor (Span, View, Own); the value is
    /// taken as an IoBufView and its ownership is threaded through zero-copy.  Large values with the
    /// large-value optimization enabled land on the listener's blob_stream and the commit callback surfaces
    /// their BlkIds; small values inline with the header.  Resolves after the entry has committed on the
    /// quorum (listener's on_commit has already fired) or with a ReplError if replication could not complete.
    Async< ReplResult<> > write(sisl::IoBuf const& user_header, sisl::IoBufView value, TraceId tid = 0);

    /// Release an indirect BlkId that the app received via the on_commit `blob_refs` list.  The BlkId is
    /// not freed immediately — it stays alive until the log entry at `referenced_lsn` is truncated past
    /// the retention horizon; only then does the log store trigger the underlying blob_stream free.  This
    /// deferred model is what lets followers safely replay the log after a crash even though the leader has
    /// already told the app "you can drop these bytes."  For inline entries (blob_refs was empty in
    /// on_commit) the app never calls this — there is nothing to free.
    Async< void > free_indirect_blk(BlkId const& blkid, raft_lsn_t referenced_lsn);

    /// Ask the replication engine to make this replica the leader.  Callable from any node — the engine
    /// routes a leadership-transfer request to the current leader on our behalf.  Idempotent — returns OK
    /// immediately if we are already the leader.  OK means the transfer request was accepted for delivery,
    /// NOT that promotion has happened; promotion is asynchronous and best-effort (may lose to a concurrent
    /// election).  Callers that need to gate work on being the leader should poll is_leader() or react to
    /// on_change_in_role(LEADER) on the listener rather than awaiting this call.  FAILED means the engine
    /// isn't running or the leader rejected the request outright.
    Async< ReplResult<> > become_leader(TraceId tid = 0);

    /// Leader-only.  Adds a peer to the replication group.  When `learner=true` the peer joins as a
    /// non-voting learner that replicates the log without counting toward the commit quorum — used by
    /// replace_member internally so the incoming node can catch up without breaking quorum.  Idempotent —
    /// already-a-member is treated as success.
    Async< ReplResult<> > add_member(ReplicaMemberInfo const& member, bool learner = false, TraceId tid = 0);

    /// Suspends until this replica has won a leader election at least once (raft_event's BecomeLeader posts
    /// the latch), or the timeout elapses.  Returns true when leadership was gained.  create_replica_set
    /// awaits this on the freshly-created single-member group before inviting the other members, since
    /// add_member is leader-only.
    Async< bool > wait_to_be_leader(std::chrono::milliseconds timeout);

    /// Leader-only.  Removes a peer from the replication group.  Idempotent — not-a-member is treated as
    /// success.  If asked to remove myself while I am leader, I yield leadership first and return
    /// NOT_LEADER so the caller retries against the successor (self-removal from the leader seat would
    /// race with our own shutdown otherwise).
    Async< ReplResult<> > remove_member(ReplicaMemberInfo const& member, TraceId tid = 0);

    /// Leader-only.  Flip `member`'s learner status.  target=false promotes a learner to a voter (counts
    /// toward quorum from then on); target=true demotes a voter back to a learner.  Idempotent — no-op if
    /// the flag is already at `target`.
    Async< ReplResult<> > flip_learner_flag(ReplicaMemberInfo const& member, bool target, TraceId tid = 0);

    /// Leader-only.  Single-shot member replacement — atomically (from the consumer's view) swaps
    /// `member_out` for `member_in` in the replication group.  Internally sequences add-as-learner →
    /// wait-for-catchup → promote-to-voter → remove-old, then fires on_start_replace_member and
    /// on_complete_replace_member on every replica's listener so applications can persist the swap.
    /// Consumer never sees the two phases — one call, one result.
    Async< ReplResult<> > replace_member(ReplicaMemberInfo const& member_out, ReplicaMemberInfo const& member_in,
                                         TraceId tid = 0);

    /// Adjust `member`'s election priority.  Callable from any node — the engine routes to the leader
    /// (committed change) or broadcasts (announced but not yet committed) if no leader is live.
    /// NOT_LEADER means we are not the leader and the engine could not broadcast either.
    Async< ReplResult<> > set_priority(ReplicaId const& member, int32_t priority, TraceId tid = 0);

    /// Apply `mutator` to a copy of the engine's current raft parameters and install the result via nuraft's
    /// update_params — e.g. widening the election window at runtime.  Local to this node: every member that
    /// should observe the change must call it on its own ReplicaSet.  An engine restart re-reads persisted
    /// settings, so a runtime update does not survive a reboot.
    Async< void > update_raft_params(std::function< void(nuraft::raft_params&) > const& mutator);

    /// Monotonically advance the app-authorized compact ceiling to `lsn`.  Writes a HS_CTRL_TRUNCATE journal
    /// entry that replicates across the group; on commit each replica updates its `app_truncate_upto_`
    /// atomic + SB, and subsequent HomeRaftLogStore::compact calls clamp their target to it.  Returns
    /// BAD_REQUEST if `options_.allow_user_driven_truncate` is false or if `lsn` is not strictly greater
    /// than the current watermark.  Does not itself trigger compaction — only advances the ceiling.
    Async< ReplResult< int64_t > > advance_truncate_upto(lsn_t lsn, TraceId tid = 0);

    /// Am I the leader of this replication group right now?  Best-effort snapshot — leadership can change
    /// between this call and the next line of consumer code.  Consumers that need to react to a transition
    /// should hook on_change_in_role on the listener instead.
    bool is_leader() const;

    /// UUID of the current leader as this replica sees it, or the nil UUID if unknown.
    ReplicaId get_leader_id() const;

    /// True when the group contains `member` — its add_member config entry has been appended, so the engine
    /// replicates to it durably (and keeps retrying while it is down).
    bool has_member(ReplicaId const& member) const;

    /// Per-peer replication status — one entry per known member of the group.  Awaits the engine's live
    /// per-peer info fetch and cross-references the group config for priority / learner flags.  Only
    /// meaningful when queried on the leader; on followers most `replication_idx_` fields are zero.
    Async< std::vector< PeerInfo > > get_replication_status() const;

    /// Peers whose committed-log lag against my commit_upto_lsn_ is within the "active" threshold — safe to
    /// count toward a dynamic quorum.  Excludes myself.  Only meaningful on the leader.
    Async< std::set< ReplicaId > > get_active_peers() const;

    /// After a leader change, returns true once the new leader has committed everything that was in its
    /// log at the moment of promotion — a consensus-correctness gate before it can serve new writes.
    /// Consumers should wait on this after seeing on_change_in_role(LEADER) before submitting the first
    /// write of the new term.
    bool is_ready_for_traffic() const;

    /// True once this replica has been marked destroyed but persistent teardown hasn't yet run — the window
    /// between the destroy log entry committing and the reaper firing finish_destroy_local().
    bool is_destroy_pending() const;

    /// True once persistent teardown (log store + superblks) has completed on this replica.
    bool is_destroyed() const;

    /// Wall-clock time at which this replica was marked destroyed.  Meaningful only after is_destroyed() /
    /// is_destroy_pending() returns true.
    Clock::time_point destroyed_time() const { return destroyed_time_; }

    /// Highest committed LSN observed on this replica.
    raft_lsn_t get_last_commit_lsn() const { return commit_upto_lsn_.load(); }

    /// Group identity + naming.  identify_str is the human-readable "rset_name:group_id" combo used in log
    /// lines; group_id_str / my_replica_id_str are UUID string forms.
    GroupId const& group_id() const { return group_id_; }
    std::string group_id_str() const { return boost::uuids::to_string(group_id_); }
    std::string const& rset_name() const { return rset_name_; }
    std::string const& identify_str() const { return identify_str_; }
    std::string my_replica_id_str() const { return boost::uuids::to_string(my_uuid_); }

    /////////////////////////////////////// ReplicationManager Interaction ///////////////////////////////////////

    /// Phase 1 — reconstruct this ReplicaSet without starting its consensus engine.  Reads the SB, seeds the
    /// commit/checkpoint watermarks, takes the raft config, and opens the log store with the replay handler
    /// attached (distinguishes create-vs-load internally from the SB itself; no separate flag).  Does NOT build
    /// the raft_server — call start_engine() for that.  Used by the manager's restart reconstruction and
    /// (immediately followed by start_engine()) by the runtime create paths.
    Async< bool > load(MetaBlkWrapper raft_cfg_mblk, nlohmann::json raft_cfg_json);

    /// Phase 2 — build and start the nuraft consensus engine.  On recovery it must run only after
    /// LogStoreManager::replay() has restored the log tail / commit index; on a fresh create it runs right after
    /// load() (nothing to replay).  Hands the engine the state_mgr / state_machine views on `this`, wires the
    /// role-change callback, and co_awaits start_server.
    Async< bool > start_engine();

    /// Clean shutdown of the consensus engine — co_awaits raft_server_->shutdown() (mandatory: nuraft's
    /// raft_server destructor asserts shutdown was completed) and drops the engine.  Must run while the RPC
    /// transport / executor the engine references are still alive, i.e. before ReplicationManager::stop() frees
    /// them.  Idempotent — a no-op if the engine was never started (raft_server_ is null).
    Async< void > stop_engine();

    /// Consumer-initiated wind-down — the "start" of a two-phase destroy.  Only the leader can call this
    /// successfully; proposes an HS_CTRL_DESTROY log entry through consensus and resolves once it commits
    /// on this node — by which point every replica that saw the commit has already run start_destroy_local()
    /// locally via dispatch_commit's CTRL_DESTROY branch and marked itself DESTROYED.  Actual resource
    /// teardown is deferred to the ReplicationManager's reaper calling finish_destroy_local() after a grace
    /// period.
    Async< ReplError > destroy();

    /// Reaper-driven "finish" of the destroy started by destroy() (or by a self-remove config commit).
    /// Called by ReplicationManager once the group has been marked destroyed for at least the configured
    /// minimum time.  Frees the log store, JSON group config, and the group SB (in that order — SB last so
    /// a crash mid-teardown leaves a discoverable stale SB that recovery can finish).  Idempotent — safe
    /// to invoke twice.
    Async< void > finish_destroy_local();

    // ── ReplicationManager accessor methods ───────────────────────────────────────────────────────────────────

    void attach_listener(shared< ReplicaSetListener > listener);
    void detach_listener();
    shared< ReplicaSetListener > get_listener() { return listener_; }
    nuraft::raft_server* raft_server();

protected:
    /////////////////////////////////////// nuraft Interface Overrides ///////////////////////////////////////

    // ── nuraft::state_machine overrides ─────────────────────────────────────────────────────────────────────
    // Protected: nuraft calls these through nuraft::state_machine* base pointers where access is checked
    // against the base's public signature.  ReplicaSet users must NOT invoke these directly.

    Async< RaftBufferPtr > commit_ext_chained(ulong log_idx, nuraft::log_entry_chain const& bufs) override;
    Async< RaftBufferPtr > commit_ext(ext_op_params const& params) override;
    Async< RaftBufferPtr > pre_commit_ext(ext_op_params const& params) override;
    Async< void > rollback_ext(ext_op_params const& params) override;
    Async< void > commit_config(ulong log_idx, nuraft::ptr< nuraft::cluster_config >& new_conf) override;
    Async< void > rollback_config(ulong log_idx, nuraft::ptr< nuraft::cluster_config >& conf) override;
    ulong last_commit_index() override;
    void create_snapshot(RaftSnapshotPtr const& s, nuraft::async_result< bool >::handler_type& when_done) override;
    Async< bool > apply_snapshot(RaftSnapshotPtr const& s) override;
    RaftSnapshotPtr last_snapshot() override;
    Async< void > save_logical_snp_obj(RaftSnapshotPtr const& s, ulong& obj_id, RaftBufferPtr const& data,
                                       bool is_first_obj, bool is_last_obj) override;
    Async< int > read_logical_snp_obj(RaftSnapshotPtr const& s, void*& user_snp_ctx, ulong obj_id,
                                      RaftBufferPtr& data_out, bool& is_last_obj) override;
    void free_user_snp_ctx(void*& user_snp_ctx) override;

    // ── nuraft::state_mgr overrides ─────────────────────────────────────────────────────────────────────────
    // Protected: same reason as above — nuraft dispatches via nuraft::state_mgr* base.

    Async< nuraft::ptr< nuraft::cluster_config > > load_config() override;
    Async< void > save_config(nuraft::cluster_config const& config) override;
    Async< void > save_state(nuraft::srv_state const& state) override;
    Async< nuraft::ptr< nuraft::srv_state > > read_state() override;
    nuraft::ptr< nuraft::log_store > load_log_store() override;
    int32_t server_id() override;
    void system_exit(int exit_code) override;

private:
    /// Typed accessor into the SB payload view.
    ReplicaSetSuperBlk* sb() { return r_cast< ReplicaSetSuperBlk* >(sb_buffer_.bytes()); }
    ReplicaSetSuperBlk const* sb() const { return r_cast< ReplicaSetSuperBlk const* >(sb_buffer_.cbytes()); }

    /// Persist the current sb_buffer_ contents through sb_mblk_.  Called after any sb()-> mutation to make
    /// the change durable.
    Async< void > write_sb() {
        RS_LOG(DEBUG, NO_TRACE_ID, "write_sb: sb_blk_num={} raft_log_store_id={} free_blks_journal_id={}",
               sb_mblk_.meta_blk().blkid().blk_num(), sb()->raft_log_store_id, sb()->free_blks_journal_id);
        co_await sb_mblk_.write(sb_buffer_.cbytes(), sb_buffer_.size());
    }

    /// Local half of the two-phase destroy — marks this replica as DESTROYED and persists destroy_pending
    /// in the SB.  Idempotent — subsequent calls no-op.  Invoked from dispatch_commit's CTRL_DESTROY branch
    /// on every replica when the CTRL_DESTROY log commits (whole-group destroy), and from commit_config's
    /// self-remove branch when a member-remove evicts us specifically.  Real teardown deferred to
    /// finish_destroy_local().
    Async< void > start_destroy_local();

    /// Poll `member`'s replicated log index until it is within kMaxCatchupLag of this leader's last log index.
    /// Called by replace_member after adding the incoming node as a learner and before promoting it to voter,
    /// so the new voter's absence never blocks quorum during the catch-up window.  Returns TIMEOUT if the
    /// learner does not catch up within kMaxCatchupWait, or NOT_LEADER if this node lost leadership while polling.
    Async< ReplResult<> > wait_for_catchup(ReplicaMemberInfo const& member, TraceId tid);

    // Shared post-parse commit dispatch — called from both commit_ext (coalesced entry) and commit_ext_chained
    // (multi-buffer entry).  Takes already-extracted header/value blobs so the parsing style stays with the
    // caller and the actual work (BlkId promotion + listener notification + LSN advance + CTRL branch) lives
    // in one place.
    Async< void > dispatch_commit(int64_t lsn, JournalType type, sisl::Blob const& user_header,
                                  sisl::Blob const& value);

    /// Registered as nuraft's cb_func via ctx->set_cb_func in start().  Fires on every raft-side event
    /// nuraft dispatches — leader/follower transitions, joined-cluster, etc.  Only caller is the lambda in
    /// start(); kept private since no external code needs to invoke it.
    nuraft::cb_func::ReturnCode raft_event(nuraft::cb_func::Type, nuraft::cb_func::Param*);

    /// Restart-resume helper.  If the SB records an in-flight replace_member (leader crashed between
    /// START_REPLACE and COMPLETE_REPLACE), pick up where we left off — inspect current cluster config,
    /// pick the right ReplaceStage, and hand off to do_replace_member.  Invoked internally from
    /// raft_event(BecomeLeader) whenever the SB has a non-empty task_id.
    Async< void > resume_pending_replace_member();

    // Stage boundaries inside do_replace_member.  Both entry points (fresh replace_member, resume path) hand
    // in the earliest stage that still needs to run; the body cascades from there to kProposeComplete.
    enum class ReplaceStage : uint8_t {
        kProposeStart = 0,   // stamp+propose CTRL_START_REPLACE; on-commit persists SB via dispatch_commit
        kAddLearner = 1,     // add_member(in, learner=true)
        kWaitAndFlip = 2,    // wait_for_catchup + flip_learner_flag(false)
        kRemoveOld = 3,      // remove_member(out)
        kProposeComplete = 4 // stamp+propose CTRL_COMPLETE_REPLACE; on-commit clears SB via dispatch_commit
    };

    /// Shared step-driver used by fresh replace_member (starts at kProposeStart) and
    /// resume_pending_replace_member (starts at whatever stage matches the current cluster state).  Each
    /// stage runs, then falls through to the next.  Every step is either idempotent by itself
    /// (add_member / remove_member / flip_learner_flag) or is safely re-proposable (the CTRL log entries).
    Async< ReplResult<> > do_replace_member(ReplicaMemberInfo const& out, ReplicaMemberInfo const& in,
                                            std::string const& task_id, ReplaceStage start, TraceId tid);

    /// Alloc a single nuraft::buffer holding [ReplLogHeader (20B fixed prefix) | user_header bytes].
    /// Stamps code, versions, declared value_size, and the current commit_upto_lsn_ on the header.
    /// Callers pass the payload (may be empty) and value_size (0 for CTRL entries); the header value_size
    /// field lets replay's extract_from_chain partition the chain correctly.
    nuraft::ptr< nuraft::buffer > build_log_entry(JournalType type, sisl::Blob const& user_header, uint32_t value_size);

    /// Should we skip committing at this LSN?  True for entries already absorbed by the last baseline
    /// snapshot — nuraft may still replay them into commit_ext during startup, but the listener has
    /// already committed everything up to that snapshot LSN so re-firing on_commit for them would be
    /// double-processing.  Also the natural extension point when we add other "skip" conditions later
    /// (e.g. destroying state).
    bool should_skip_commit(raft_lsn_t lsn) const { return lsn <= sb()->last_snapshot_lsn; }

private:
    /////////////////////////////////////// Members ///////////////////////////////////////////////////////////

    ReplicationManager& mgr_;
    GroupId group_id_;
    ReplicaId my_uuid_;
    int32_t raft_server_id_;
    std::string rset_name_;
    std::string identify_str_;
    // Per-group options.  Set once at ctor time from whichever creation path is active
    // (user-initiated create passes arg; on-demand / restart query listener->replica_set_options()).
    // Not persisted in SB — listener is the source of truth across restarts.
    ReplicaSetOptions options_;

    shared< ReplicaSetListener > listener_;
    shared< HomeRaftLogStore > log_store_;
    shared< nuraft::raft_server > raft_server_;

    // Pre-allocated once and returned unchanged from every pre-commit/commit hook — nuraft demands a non-null
    // ptr<buffer> return, but no callers on the raft side interpret its bytes.  Sharing one instance keeps the
    // commit path allocation-free.
    nuraft::ptr< nuraft::buffer > success_ptr_;

    sisl::Rcu::scoped_ptr< ReplicaSetStage > stage_;

    // SB layer.  sb_mblk_ pairs the on-disk MetaBlk with the shared MetaClient owned by the manager;
    // sb_buffer_ is the view returned by sb_mblk_.read() — it already holds shared ownership of the
    // underlying buffer (MetaBlk's cached inline block, or a freshly-read overflow buffer), so there's
    // nothing to allocate or copy on our side.  sb() casts sb_buffer_.bytes() to the typed struct for
    // in-place mutation; write_sb() persists the current view contents through sb_mblk_.
    MetaBlkWrapper sb_mblk_;
    sisl::IoBufView sb_buffer_;

    MetaBlkWrapper raft_cfg_mblk_;
    nlohmann::json raft_cfg_json_;

    folly::coro::Mutex config_mtx_;

    // Running record of the last-committed raft cluster membership (peer UUIDs).  Seeded in start() from
    // raft_cfg_json_ before raft_server startup; refreshed by every commit_config to diff against the new
    // membership and fire listener_->on_membership_change with the added/removed sets.  Read/written only
    // from the state-machine driver (commit_config runs there and start() runs before raft_server exists),
    // so no explicit synchronisation.
    std::set< ReplicaId > committed_members_;

    // Seeded in load() from log_store_->last_checkpt_lsn() (the applied watermark of the last completed CP).
    // Advanced during log-store replay to each proven-and-dispatched lsn, then by every commit_ext /
    // commit_config during steady-state operation.  Sampled by the LogStore commit-watermark callback at
    // every CP switchover, which is what round-trips it to disk.  Invariant: checkpt <= this <= tail.
    std::atomic< raft_lsn_t > commit_upto_lsn_{0};

    // Replay-only state (single-threaded boot walk, no locks).  Entries arrive from on_log_found in lsn
    // order but each entry's commit proof arrives with LATER entries' commit_lsn_at_write stamps, so
    // entries queue here until proven, then dispatch in order.  Entries never proven (uncommitted tail at
    // crash) are dropped by start_engine() — nuraft re-commits any that actually reached quorum.
    struct PendingReplayEntry {
        raft_lsn_t lsn;
        sisl::IoBufView bv; // owns a refcount on the log buffer backing
    };
    std::deque< PendingReplayEntry > replay_pending_;
    raft_lsn_t replay_proven_upto_{0};

    // App-authorized compact ceiling.  Advanced monotonically by dispatch_commit's HS_CTRL_TRUNCATE branch
    // on every replica; HomeRaftLogStore::compact clamps its target to this value.  Not persisted — on
    // start() seeded to log_store_->start_index() (the log's first_lsn), which is a safe lower bound
    // implied by whatever compact has already occurred pre-restart.
    std::atomic< raft_lsn_t > app_truncate_upto_{0};

    // Traffic gate for a freshly elected leader.  raft_event stamps this to raft_server_->get_last_log_idx()
    // on the BecomeLeader transition (i.e. the tail LSN at the moment of promotion — may include entries from
    // prior terms that still need to commit under our term).  is_ready_for_traffic() returns true once
    // commit_upto_lsn_ has caught up to this gate; write() consumers poll it before submitting.  Reset to 0
    // when we drop back to follower / joined cluster so new writes are held again until the next promotion
    // gate is reached.
    std::atomic< raft_lsn_t > traffic_ready_lsn_{0};

    // Latched "won a leader election at least once" signal — posted (idempotently) by raft_event's
    // BecomeLeader branch, awaited by wait_to_be_leader().  Never reset: once elected, later role changes
    // don't un-signal it.
    folly::coro::Baton elected_as_leader_;

    Clock::time_point destroyed_time_;

    // metrics_ is constructed inside open() once identify_str_ is populated from the SB — the payload-dependent
    // parts of setup live there, not in the ctor.  unique<> so the member can start nullptr in the ctor and
    // move to a real instance in open() without any placeholder registration/deregistration with MetricsFarm.
    unique< ReplicaSetMetrics > metrics_;

    // Snapshot receive-side state.  save_logical_snp_obj allocates the Builder on is_first and feeds
    // write_next_chunk on every call; state_machine::apply_snapshot(s) later finalizes it and hands the
    // result to listener->apply_snapshot.  A single follower can only be receiving one snapshot at a time,
    // so a scalar member suffices.  Accessed only on the raft state-machine driver — no lock.
    shared< ReplSnapshot::Builder > incoming_snapshot_builder_;
};

} // namespace homestore