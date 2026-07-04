#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <folly/Executor.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/HHWheelTimer.h>
#include <libnuraft/rpc_cli.hxx>
#include <libnuraft/async.hxx>
#include <libnuraft/basic_types.hxx>

#include "common/defs.h"
#include "wire_frame.h"

namespace nuraft {
class req_msg;
class resp_msg;
class rpc_exception;
} // namespace nuraft

namespace homestore::replication {

class PeerOutboundSocket;
class FollyRpcClientFactory;

// ------------------------------------------------------------------------------------------------------------
//                                         FollyRpcClient
// ------------------------------------------------------------------------------------------------------------
//
// Implements nuraft::rpc_client. One instance per (peer, raft_group) pair, created by FollyRpcClientFactory.
// It is intentionally stateless on the wire: it holds the peer endpoint and the client id, and on every send()
// it (a) figures out which iomanager reactor the caller is currently on, (b) borrows the PeerOutboundSocket
// owned by *that reactor* for *this peer*, opening it lazily on first use, and (c) hands the work to it.
//
// The wire-level multiplexing across raft groups is done via WireFrame.group_id, not by maintaining a separate
// FollyRpcClient socket per group. So N FollyRpcClient instances pointing at the same peer share the same
// underlying TCP connection (per local reactor) — no extra sockets per group.
class FollyRpcClient : public nuraft::rpc_client, public std::enable_shared_from_this< FollyRpcClient > {
public:
    FollyRpcClient(FollyRpcClientFactory* factory, std::string peer_host, uint16_t peer_port, uint64_t client_id);
    ~FollyRpcClient() override;

    // nuraft::rpc_client API. when_done fires on the outbound socket's owning reactor (= the same reactor
    // send() ran on, since the response returns over the same socket — Design X invariant). Either fires with
    // (resp, nullptr) when the matched response is parsed, or with (nullptr, rpc_exception) on
    // connect/write/timeout/abandon failure.
    void send(nuraft::ptr< nuraft::req_msg >& req, nuraft::group_id_t const& group_id, nuraft::rpc_handler& when_done,
              uint64_t send_timeout_ms = 0) override;

    uint64_t get_id() const override { return client_id_; }

    // Becomes true once the underlying outbound socket has hit a non-recoverable error.  nuraft's peer
    // logic consults this to drop the client and request a fresh one on the next round.
    bool is_abandoned() const override { return abandoned_.load(std::memory_order_acquire); }

    // Marked by PeerOutboundSocket when its socket fails — flips abandoned_ so subsequent send() calls
    // short-circuit with rpc_exception("client abandoned") and nuraft drops us.
    void set_abandoned() { abandoned_.store(true, std::memory_order_release); }

private:
    FollyRpcClientFactory* factory_; // non-owning; the factory outlives every client it created
    std::string peer_host_;
    uint16_t peer_port_;
    uint64_t client_id_;
    std::atomic< bool > abandoned_{false};
};

// ------------------------------------------------------------------------------------------------------------
//                                         PeerOutboundSocket
// ------------------------------------------------------------------------------------------------------------
//
// One instance per (peer, *local* reactor). Lives in per-reactor storage looked up by peer endpoint. Owns the
// outgoing TCP connection to that peer (the local reactor opened it) plus the per-socket pending-request
// table. Every method, every callback, every map access happens on the owning reactor's EventBase thread —
// folly's AsyncSocket contract guarantees that, so the table needs no synchronization.
//
// Roles on this socket:
//   * Local node writes outbound *requests* on it.
//   * Local node reads inbound *responses* on it (matched back via pending_).
// It does NOT carry inbound requests from the peer — those land on the *other* socket the peer opened to us,
// which is handled by InboundConnection on whichever reactor accepted it.
//
// Zero copy on TX
// ---------------
// (1) AsyncSocket is created with setZeroCopy(true) which enables SO_ZEROCOPY at the syscall layer and tells
//     folly to drain MSG_ERRQUEUE completion notifications transparently.
// (2) A size gate (set_zerocopy_min_size, default ~8KB) suppresses zero-copy for tiny payloads — for those the
//     kernel copies anyway and the notification overhead is a net loss.
// (3) On send(): caller wraps the nuraft::buffer (the serialized req_msg) via IOBuf::takeOwnership(...) with
//     a deleter holding nuraft::ptr<buffer>. We chain the 36-byte WireFrame header IOBuf in front via
//     WireFrame::build(...). The chain is handed to writeChain() with WriteFlags::WRITE_MSG_ZEROCOPY. No
//     payload bytes are copied application-side, and for payloads ≥ gate the kernel doesn't copy either.
//
// Response routing
// ----------------
// pending_ maps req_id -> rpc_handler closure. send() allocates a fresh req_id from a local counter, inserts
// the entry, builds the wire frame stamped with that req_id, and writes. When the peer responds, the response
// frame arrives on this socket (Design X), readDataAvailable() parses it, removes the pending_ entry on that
// req_id, and invokes the closure with the deserialized resp_msg.
class PeerOutboundSocket : public folly::AsyncSocket::ConnectCallback,
                           public folly::AsyncReader::ReadCallback,
                           public folly::AsyncWriter::WriteCallback {
public:
    PeerOutboundSocket(folly::EventBase* eb, std::string host, uint16_t port, folly::Executor* slow_executor);
    ~PeerOutboundSocket() override;

    // Called from FollyRpcClient::send() on this reactor. Allocates a req_id, inserts
    // pending_[req_id]={handler, msg_type}, wraps the nuraft buffer zero-copy, chains the wire header,
    // writeChain() with MSG_ZEROCOPY. If the socket isn't connected yet, the work is queued and flushed once
    // the connect callback fires. The msg_type is retained so the response-side path can route hot vs slow.
    void send(nuraft::group_id_t const& group_id, uint8_t msg_type,
              unique< folly::IOBuf > payload, // takeOwnership-wrapped nuraft::buffer; no copies
              nuraft::rpc_handler when_done, std::chrono::milliseconds timeout);

    // Register a FollyRpcClient that has routed through this socket so that it can be marked abandoned on
    // a socket-level failure.  Deduplicated by raw pointer identity; expired weaks are tolerated.
    void register_client(std::weak_ptr< FollyRpcClient > client);

    folly::EventBase* event_base() const { return eb_; }

    // folly::AsyncSocket::ConnectCallback
    void connectSuccess() noexcept override;
    void connectErr(folly::AsyncSocketException const& ex) noexcept override;

    // folly::AsyncReader::ReadCallback
    void getReadBuffer(void** bufReturn, size_t* lenReturn) override;
    void readDataAvailable(size_t len) noexcept override;
    bool isBufferMovable() noexcept override { return false; }
    void readEOF() noexcept override;
    void readErr(folly::AsyncSocketException const& ex) noexcept override;

    // folly::AsyncWriter::WriteCallback. writeSuccess() does nothing — folly drops the IOBuf chain for us and
    // we don't need a per-write hook on the happy path. writeErr() fails the just-sent request's handler with
    // an rpc_exception (looked up by the req_id stamped on the in-flight frame) and tears the socket down.
    void writeSuccess() noexcept override {}
    void writeErr(size_t bytes_written, folly::AsyncSocketException const& ex) noexcept override;

private:
    void open_connection();
    // Mark every registered FollyRpcClient as abandoned, then fail every pending request.  All error
    // callbacks (connectErr/writeErr/readErr/readEOF/dtor) funnel through here.
    void fail_socket(nuraft::ptr< nuraft::rpc_exception > ex);
    // Invoked by a per-request TimeoutCallback when its timer fires.  Looks up the pending entry, removes
    // it, and invokes the handler with rpc_exception("send timeout").  No-op if the entry has already been
    // resolved or drained.
    void fail_pending_request(uint64_t req_id);

    // HHWheelTimer::Callback subclass implemented in the cpp — forward-declared here so PendingEntry can
    // hold a unique<TimeoutCallback>.
    struct TimeoutCallback;

    folly::EventBase* eb_;
    std::string host_;
    uint16_t port_;
    folly::Executor* slow_executor_; // non-owning; for routing slow-RPC response callbacks off this reactor
    folly::AsyncSocket::UniquePtr sock_;

    // Per-EventBase timer wheel that drives every per-request timeout on this socket.  Constructed in the
    // ctor via folly::HHWheelTimer::newTimer(eb_).
    folly::HHWheelTimer::UniquePtr timer_wheel_;

    // Pending requests sent from this socket, awaiting their response. Single-threaded — accessed only on eb_.
    // sent_msg_type is recorded at send time so the response-arrival path can route slow-RPC handlers onto the
    // slow executor without re-decoding the wire payload.  `timeout` is non-null only when send_timeout_ms>0;
    // its dtor is invoked when the entry is erased — caller must cancelTimeout() first to avoid a stale
    // pointer inside the timer wheel.
    struct PendingEntry {
        nuraft::rpc_handler when_done;
        uint8_t sent_msg_type;
        unique< TimeoutCallback > timeout;
    };
    std::unordered_map< uint64_t /*req_id*/, PendingEntry > pending_;
    uint64_t next_req_id_{1};

    // Every FollyRpcClient that has sent through this socket, in weak form so they're not kept alive past
    // their natural lifetime.  On socket-level failure, lock() each and call set_abandoned().  Accessed only
    // on eb_ — no synchronization.
    std::vector< std::weak_ptr< FollyRpcClient > > clients_;

    // Inbound parser state machine. Two phases per frame: read 36-byte header, then read payload_len bytes
    // into a freshly allocated nuraft::buffer so the deserialize step does not need to copy again.
    enum class RxState { Header, Body };
    RxState rx_state_{RxState::Header};
    WireFrame rx_hdr_{};
    uint8_t rx_hdr_buf_[WireFrame::kHeaderSize]{};
    size_t rx_hdr_filled_{0};
    nuraft::ptr< nuraft::buffer > rx_body_;
    size_t rx_body_filled_{0};

    bool connected_{false};
};

} // namespace homestore::replication
