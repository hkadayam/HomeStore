#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <folly/Executor.h>
#include <libnuraft/rpc_cli_factory.hxx>

#include "common/defs.h"
#include "iomanager/reactor_local.h"
#include "folly_rpc_client.h"

namespace homestore::replication {

// ------------------------------------------------------------------------------------------------------------
//                                      FollyRpcClientFactory
// ------------------------------------------------------------------------------------------------------------
//
// Implements nuraft::rpc_client_factory and also owns the per-reactor PeerOutboundSocket store. Singleton-style
// — one instance per node, held by ReplicationManager.
//
// Two responsibilities, kept on one class because they share the same lifetime and the same per-reactor map:
//
//   1) create_client(endpoint) — nuraft hands this an endpoint string ("host:port") and expects a fresh
//      rpc_client. We return a new FollyRpcClient pointing back at us. Each call gets its own client_id from
//      a monotonic counter so nuraft's peer abandonment logic can distinguish clients.
//
//   2) get_or_open_outbound(host, port) — called from FollyRpcClient::send() on the caller's reactor. Looks
//      up the per-reactor map for "host:port"; if present, returns the existing PeerOutboundSocket; if absent,
//      constructs one (which kicks off an async connect) and inserts it. The lookup, the construction, and the
//      insert all happen on the calling reactor's thread because outbound_.get() returns *that reactor's* slot.
//      No locks, no atomics.
//
// Reactor of send()
// -----------------
// FollyRpcClient::send() must run on a reactor before it can touch the ReactorLocal slot. The convention:
//   - If the caller is already on a reactor (the common case — raft_server's coro task runs on a reactor),
//     send() does its work inline.
//   - If the caller is not on a reactor (test threads, admin threads), send() dispatches the work to a
//     sticky reactor chosen by hashing the peer endpoint, so the same peer always lands on the same reactor
//     and the outbound socket is reused.
class FollyRpcClientFactory : public nuraft::rpc_client_factory {
public:
    explicit FollyRpcClientFactory(folly::Executor* slow_executor);
    ~FollyRpcClientFactory() override;

    folly::Executor* slow_executor() const { return slow_executor_; }

    // nuraft::rpc_client_factory API. `endpoint` is "host:port". Hands back a freshly-constructed
    // FollyRpcClient pointing at this factory.
    nuraft::ptr< nuraft::rpc_client > create_client(std::string const& endpoint) override;

    // Called from FollyRpcClient::send() once it has hopped to a reactor (or confirmed it is already on one).
    // Returns the PeerOutboundSocket owned by *the current reactor* for the given peer endpoint, opening it
    // lazily on first use. Must be called from a reactor thread.
    PeerOutboundSocket& get_or_open_outbound(std::string const& host, uint16_t port);

    // Pick a sticky reactor for cold-path (non-reactor) callers — hashes the peer endpoint so repeated calls
    // to the same peer always land on the same reactor and reuse its socket.
    size_t pick_reactor_for_cold_path(std::string const& host, uint16_t port) const;

private:
    struct PerReactorState {
        // "host:port" -> outbound socket owned by this reactor.
        std::unordered_map< std::string, unique< PeerOutboundSocket > > by_endpoint;
    };

    folly::Executor* slow_executor_; // non-owning; lifetime managed by ReplicationManager
    std::atomic< uint64_t > client_id_seq_{1};
    iomanager::ReactorLocal< PerReactorState > outbound_;
};

} // namespace homestore::replication
