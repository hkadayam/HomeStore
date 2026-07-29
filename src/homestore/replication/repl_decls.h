#pragma once
#include <iostream>
#include <string>

#include <folly/small_vector.h>
#include <folly/Expected.h> // Result = folly::Expected
#include <folly/Unit.h>     // folly::Unit
#include "common/async.h"   // Async<> (folly::coro::Task)

#include "sisl/logging/logging.h"
#include "homestore/base/homestore_decl.h"
#include "homestore/base/blk.h"
#include "homestore/base/homestore_assert.h"
#include "sisl/fds/buffer.h"

#include <libnuraft/ptr.hxx> // nuraft::ptr<> alias template used by the RaftXxxPtr typedefs below

// nuraft types are named below only through nuraft::ptr<> (shared_ptr) aliases — forward declarations suffice
// and keep this decls header from pulling the full nuraft surface.
namespace nuraft {
class buffer;
class cluster_config;
class log_entry;
class snapshot;
} // namespace nuraft

namespace homestore {
// clang-format off
VENUM(ReplError, int32_t,
      OK = 0,         // Everything OK
      CANCELLED = -1, // Request was cancelled
      TIMEOUT = -2,
      NOT_LEADER = -3,
      BAD_REQUEST = -4,
      SERVER_ALREADY_EXISTS = -5,
      CONFIG_CHANGING = -6,
      SERVER_IS_JOINING = -7,
      SERVER_NOT_FOUND = -8,
      CANNOT_REMOVE_LEADER = -9,
      SERVER_IS_LEAVING = -10,
      TERM_MISMATCH = -11,
      RETRY_REQUEST = -12,
      RESULT_NOT_EXIST_YET = -10000,
      NOT_IMPLEMENTED = -10001,
      NO_SPACE_LEFT = -20000,
      DRIVE_WRITE_ERROR = -20001,
      DATA_DUPLICATED = -20002,
      QUIENCE_STATE = -20003,
      QUORUM_NOT_MET = -20004,
      FAILED = -32768);
// clang-format on

template < typename V, typename E >
using Result = folly::Expected< V, E >;

template < class V = folly::Unit >
using ReplResult = Result< V, ReplError >;

template < class V, class E >
using AsyncResult = Async< Result< V, E > >;

template < class V = folly::Unit >
using AsyncReplResult = AsyncResult< V, ReplError >;

using ReplicaId = uuid_t;
using GroupId = uuid_t;

using store_lsn_t = int64_t; // 0-indexed; LogStore's native lsn space
using raft_lsn_t = int64_t;  // 1-indexed; nuraft's lsn space (= store_lsn + 1)
using RaftBufferPtr = nuraft::ptr< nuraft::buffer >;
using RaftClusterConfig = nuraft::ptr< nuraft::cluster_config >;
using RaftLogEntryPtr = nuraft::ptr< nuraft::log_entry >;
using RaftSnapshotPtr = nuraft::ptr< nuraft::snapshot >;

using TraceId = uint64_t;

struct PeerInfo {
    ReplicaId id_;                   // Peer ID.
    uint64_t replication_idx_ = 0;   // The last replication index that the peer has, from this server's point of view.
    uint64_t last_succ_resp_us_ = 0; // Elapsed time since the last successful response from this peer, 0 on leader
    uint32_t priority_ = 0;          // The priority for leader election
    bool can_vote = true; // Whether the peer can vote. If a peer is learner, this will be false. Hide the raft details.
};

struct ReplicaMemberInfo {
    static constexpr uint64_t max_name_len = 128;

    ReplicaId id;
    char name[max_name_len];
    int32_t priority{0};
};

/// Derive the int32 server id raft uses internally from a ReplicaId (16-byte uuid). Folds the uuid by xor'ing
/// 4-byte chunks; collisions are statistically negligible at typical cluster sizes.
inline int32_t to_server_id(ReplicaId const& uuid) {
    uint32_t r = 0;
    for (size_t i = 0; i < uuid.size(); i += 4) {
        uint32_t chunk = 0;
        std::memcpy(&chunk, &uuid.data[i], 4);
        r ^= chunk;
    }
    return static_cast< int32_t >(r);
}

} // namespace homestore
