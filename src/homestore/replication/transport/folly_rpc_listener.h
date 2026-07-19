#pragma once

#include <atomic>
#include "common/async.h"
#include <cstdint>
#include <memory>

#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <libnuraft/rpc_listener.hxx>
#include <libnuraft/async.hxx>
#include <libnuraft/basic_types.hxx>

#include "common/defs.h"
#include "wire_frame.h"

namespace nuraft {
class raft_server;
class req_msg;
class resp_msg;
class buffer;
class rpc_exception;
} // namespace nuraft

namespace homestore::replication {

class ReplicationManager;
class FollyRpcListener;

// Holds every live InboundConnection.  Owned by FollyRpcListener (strong); accessed by each connection via
// std::weak_ptr so a connection that outlives its listener (e.g. an in-flight Task still running when
// shutdown() returned) can safely no-op its self-unregister.  Defined fully after FollyRpcListener so we
// can name its nested InboundConnection type.
struct ConnectionRegistry;

// ------------------------------------------------------------------------------------------------------------
//                                         FollyRpcListener
// ------------------------------------------------------------------------------------------------------------
//
// Implements nuraft::rpc_listener. One instance per node — owns a single folly::AsyncServerSocket bound to the
// configured port, with an accept callback that distributes inbound TCP connections across iomanager reactors
// round-robin. Each accepted fd is wrapped in an InboundConnection that lives on the chosen reactor for its
// whole lifetime.
//
// nuraft's rpc_listener API has only listen()/stop()/shutdown(). The `handler` argument to listen() is
// ignored on purpose: in our multi-group setup, the receive-side dispatch is not "one raft_server per
// listener" — every inbound WireFrame carries a group_id which we look up in ReplicationManager to find
// the right raft_server. So the listener holds a ReplicationManager* it consults at parse time, not a
// nuraft::msg_handler.
class FollyRpcListener : public nuraft::rpc_listener {
public:
    FollyRpcListener(folly::EventBase* accept_eb, uint16_t port, ReplicationManager* mgr);
    ~FollyRpcListener() override;

    // nuraft API. Called once at raft_server startup. Binds the server socket, installs the accept callback,
    // starts accepting. Returns immediately; the accept loop runs on accept_eb_.
    void listen(nuraft::ptr< nuraft::msg_handler >& /*unused*/) override;

    // Stops accepting new connections. Already-accepted InboundConnections continue to drain in flight work
    // and are torn down individually via their own readEOF/readErr paths.
    void stop() override;

    // Final teardown — called by raft_server during shutdown. Stops accepting, then walks the connection
    // registry and posts closeNow() to each connection's owning reactor.  Does NOT wait for in-flight
    // dispatch_request Tasks to finish — those Tasks captured shared_from_this() and run to completion
    // asynchronously; their post-shutdown writes silently no-op via the closed_ guard.
    void shutdown() override;

private:
    class AcceptCb;
    class InboundConnection;

    folly::EventBase* accept_eb_;
    uint16_t port_;
    ReplicationManager* mgr_;
    folly::AsyncServerSocket::UniquePtr server_;
    unique< AcceptCb > accept_cb_;
    // Strong ref keeps every accepted InboundConnection alive past AcceptCb's lambda — the lambda inserts
    // here before exiting, fixing the original lifetime bug where the only shared_ptr died with the lambda.
    shared< ConnectionRegistry > registry_;
};

// ------------------------------------------------------------------------------------------------------------
//                                         InboundConnection
// ------------------------------------------------------------------------------------------------------------
//
// One per accepted socket. Pinned to whichever iomanager reactor the AcceptCb assigned it to; lives on that
// reactor's EventBase for its entire lifetime. All callbacks fire on that reactor's thread (folly contract),
// so all state is single-threaded by construction — no locks.
//
// Roles on this socket:
//   * Local node reads inbound *requests* on it (peer-initiated).
//   * Local node writes outbound *responses* back on it (Design X: response leaves on the same socket the
//     request arrived on).
// It does NOT carry outbound requests originating here — those go out on PeerOutboundSocket instances on the
// other set of sockets opened by this node.
//
// Per-frame dispatch flow
// -----------------------
// 1. RX state machine reads 36 bytes of header into rx_hdr_buf_, parses via WireFrame::decode_header().
// 2. Allocates a fresh nuraft::buffer of size payload_len and reads payload bytes directly into it (no
//    intermediate userspace copy on the parse boundary — the only RX copy is kernel->buffer).
// 3. group_id is looked up in mgr_ to find the raft_server for that group; if absent, the frame is dropped
//    and an error response is sent back stamped with the same req_id.
// 4. raft_server::process_req(req_msg) is invoked as a folly::coro::Task. The task runs initially on this
//    reactor; if it suspends on log-flush, the wait point uses co_withExecutor(this->eb_) so resumption
//    happens back on this same reactor — Design X requires this to keep the response write on this socket's
//    owning thread (no cross-reactor hop).
// 5. When the task produces a resp_msg, it's serialized into a nuraft::buffer, wrapped via
//    IOBuf::takeOwnership() (zero copy), chained behind a WireFrame header IOBuf with the same req_id +
//    is_response flag set, and writeChain'd with WriteFlags::WRITE_MSG_ZEROCOPY.
class FollyRpcListener::InboundConnection : public folly::AsyncReader::ReadCallback,
                                            public folly::AsyncWriter::WriteCallback,
                                            public std::enable_shared_from_this< FollyRpcListener::InboundConnection > {
public:
    InboundConnection(folly::EventBase* eb, folly::AsyncSocket::UniquePtr sock, ReplicationManager* mgr,
                      shared< ConnectionRegistry > registry);
    ~InboundConnection() override;

    // Called on eb_ after construction to enable SO_ZEROCOPY and start the read loop.
    void start();

    folly::EventBase* event_base() const { return eb_; }

    // folly::AsyncReader::ReadCallback
    void getReadBuffer(void** bufReturn, size_t* lenReturn) override;
    void readDataAvailable(size_t len) noexcept override;
    bool isBufferMovable() noexcept override { return false; }
    void readEOF() noexcept override;
    void readErr(folly::AsyncSocketException const& ex) noexcept override;

    // folly::AsyncWriter::WriteCallback. writeSuccess() is empty — folly drops the IOBuf chain for us once
    // the kernel signals completion (real send for non-zerocopy, MSG_ERRQUEUE drain for MSG_ZEROCOPY).
    // writeErr() tears the socket down; in-flight raft_server tasks producing responses will see writeErr
    // when they eventually try to send back.
    void writeSuccess() noexcept override {}
    void writeErr(size_t bytes_written, folly::AsyncSocketException const& ex) noexcept override;

private:
    // Once a full WireFrame request is in, look up the raft_server by group_id, dispatch its process_req
    // coro task, schedule the response write-back on this same socket when the task completes.
    Async< void > dispatch_request(nuraft::group_id_t gid, uint64_t req_id, nuraft::ptr< nuraft::buffer > body);

    // Build a response wire frame (header echoing req_id with is_response flag set, plus encoded resp_msg
    // payload) and writeChain it on this socket with MSG_ZEROCOPY.  Called from any thread — uses
    // eb_->isInEventBaseThread() to write directly when on the connection's reactor, or hops via
    // runInEventBaseThread(...) when off it (cpu_executor path, post-await resumption off-eb_, etc.).
    void send_response(nuraft::group_id_t const& gid, uint64_t req_id, nuraft::ptr< nuraft::resp_msg > resp);

    // Erase this connection from its registry under the registry's mutex.  No-op if the registry weak_ptr
    // has expired (listener has already destructed).
    void unregister_self();

    folly::EventBase* eb_;
    folly::AsyncSocket::UniquePtr sock_;
    ReplicationManager* mgr_;
    std::weak_ptr< ConnectionRegistry > registry_;

    // RX state machine — same shape as PeerOutboundSocket's but reading into the request buffer rather than
    // the response buffer.
    enum class RxState { Header, Body };
    RxState rx_state_{RxState::Header};
    WireFrame rx_hdr_{};
    uint8_t rx_hdr_buf_[WireFrame::kHeaderSize]{};
    size_t rx_hdr_filled_{0};
    nuraft::ptr< nuraft::buffer > rx_body_;
    size_t rx_body_filled_{0};

    // Set on terminal state (readEOF/readErr/writeErr/shutdown close).  Read by send_response inside an
    // eb_ thread.  All accesses are on eb_ — plain bool, no atomic needed.
    bool closed_{false};
};

// ------------------------------------------------------------------------------------------------------------
//                                         FollyRpcListener::AcceptCb
// ------------------------------------------------------------------------------------------------------------
//
// Folly invokes connectionAccepted() once per new inbound TCP connection. We round-robin the new fd across
// iomanager reactors, hop to the chosen reactor, wrap the fd in an AsyncSocket attached to that reactor, and
// hand it off to a fresh InboundConnection. All read/write activity for that socket then happens on that
// receiver reactor's thread for the connection's lifetime.
class FollyRpcListener::AcceptCb : public folly::AsyncServerSocket::AcceptCallback {
public:
    AcceptCb(ReplicationManager* mgr, shared< ConnectionRegistry > registry) :
            mgr_(mgr), registry_(std::move(registry)) {}

    void connectionAccepted(folly::NetworkSocket fd, folly::SocketAddress const& client_addr,
                            AcceptInfo info) noexcept override;
    void acceptError(folly::exception_wrapper ex) noexcept override;

private:
    ReplicationManager* mgr_;
    shared< ConnectionRegistry > registry_;
};

// Now that FollyRpcListener::InboundConnection is a complete type we can define the registry.
struct ConnectionRegistry {
    std::mutex mtx;
    std::unordered_map< FollyRpcListener::InboundConnection*, shared< FollyRpcListener::InboundConnection > > conns;
};

} // namespace homestore::replication