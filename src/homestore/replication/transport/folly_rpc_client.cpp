#include "folly_rpc_client.h"
#include "folly_rpc_client_factory.h"
#include "nuraft_codec.h"
#include "wire_frame.h"

#include <cassert>

#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/HHWheelTimer.h>
#include <folly/SocketAddress.h>
#include <libnuraft/buffer.hxx>
#include <libnuraft/req_msg.hxx>
#include <libnuraft/resp_msg.hxx>
#include <libnuraft/rpc_exception.hxx>

#include "iomanager/iomanager.h"
#include "homestore/base/homestore_assert.h"

namespace homestore::replication {

// Per-peer RPC transport tracing on the replication log module (mirrors RM_LOG / REPL_STORE_LOG), keyed by the peer
// endpoint so a peer's client + socket lines group together under `--log_mods replication:trace`.
#define RPC_CLI_LOG(level, ...)                                                                                        \
    HS_SUBMOD_LOG(level, replication, , "rpc_peer", fmt::format("{}:{}", peer_host_, peer_port_), ##__VA_ARGS__)
#define RPC_SOCK_LOG(level, ...)                                                                                       \
    HS_SUBMOD_LOG(level, replication, , "rpc_peer", fmt::format("{}:{}", host_, port_), ##__VA_ARGS__)

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                     FollyRpcClient
// ══════════════════════════════════════════════════════════════════════════════════════════════════

FollyRpcClient::FollyRpcClient(FollyRpcClientFactory* factory, std::string peer_host, uint16_t peer_port,
                               uint64_t client_id) :
        factory_(factory), peer_host_(std::move(peer_host)), peer_port_(peer_port), client_id_(client_id) {
}

FollyRpcClient::~FollyRpcClient() = default;

void FollyRpcClient::send(nuraft::ptr< nuraft::req_msg >& req, nuraft::group_id_t const& group_id,
                          nuraft::rpc_handler& when_done, uint64_t send_timeout_ms) {
    if (abandoned_.load(std::memory_order_acquire)) {
        nuraft::ptr< nuraft::resp_msg > null_resp;
        auto ex = nuraft::cs_new< nuraft::rpc_exception >("client abandoned", req);
        when_done(null_resp, ex);
        return;
    }

    auto payload = encode_req_msg(*req);
    uint8_t const msg_type = static_cast< uint8_t >(req->get_type());
    std::chrono::milliseconds timeout{send_timeout_ms};
    auto host = peer_host_;
    auto port = peer_port_;
    auto handler_copy = when_done;
    auto factory = factory_;

    // Captured into do_send so the socket can mark this client abandoned on a socket-level failure.  weak
    // so we don't keep the client alive past nuraft's lifetime.
    std::weak_ptr< FollyRpcClient > weak_self = shared_from_this();
    auto do_send = [factory, host, port, group_id, msg_type, payload = std::move(payload),
                    handler_copy = std::move(handler_copy), req, timeout, weak_self]() mutable {
        HS_SUBMOD_LOG(TRACE, replication, , "rpc_peer", fmt::format("{}:{}", host, port),
                      "do_send running on reactor={} msg_type={}", iomgr().current_reactor_id(), int(msg_type));
        auto& sock = factory->get_or_open_outbound(host, port);
        sock.register_client(weak_self);
        sock.send(group_id, msg_type, std::move(payload), std::move(handler_copy), std::move(req), timeout);
    };

    auto& iom = iomgr();
    if (iom.current_reactor_id() < iom.num_reactors()) {
        // Already on a reactor — dispatch inline.
        RPC_CLI_LOG(TRACE, "send msg_type={}: inline dispatch (cur_reactor={})", int(msg_type),
                    iom.current_reactor_id());
        do_send();
    } else {
        // Cold path: hop to a sticky reactor chosen by hashing the peer endpoint.
        size_t rid = factory_->pick_reactor_for_cold_path(peer_host_, peer_port_);
        RPC_CLI_LOG(TRACE, "send msg_type={}: cold-path hop to reactor={} (cur_reactor={} not a reactor)",
                    int(msg_type), rid, iom.current_reactor_id());
        iom.reactor_for(rid)->runInEventBaseThread(std::move(do_send));
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                  PeerOutboundSocket
// ══════════════════════════════════════════════════════════════════════════════════════════════════

// ── HHWheelTimer::Callback that fails one pending request on timeout ─────────────────────────────
struct PeerOutboundSocket::TimeoutCallback : public folly::HHWheelTimer::Callback {
    PeerOutboundSocket* sock;
    uint64_t req_id;
    TimeoutCallback(PeerOutboundSocket* s, uint64_t r) : sock(s), req_id(r) {}
    void timeoutExpired() noexcept override { sock->fail_pending_request(req_id); }
    void callbackCanceled() noexcept override {}
};

PeerOutboundSocket::PeerOutboundSocket(folly::EventBase* eb, std::string host, uint16_t port,
                                       folly::Executor* cpu_executor) :
        eb_(eb),
        host_(std::move(host)),
        port_(port),
        cpu_executor_(cpu_executor),
        timer_wheel_(folly::HHWheelTimer::newTimer(eb_)) {
    open_connection();
}

PeerOutboundSocket::~PeerOutboundSocket() {
    fail_socket("socket destroyed");
    if (sock_) {
        sock_->closeNow();
    }
}

void PeerOutboundSocket::open_connection() {
    sock_ = folly::AsyncSocket::UniquePtr(new folly::AsyncSocket(eb_));
    sock_->setZeroCopy(true);
    sock_->connect(this, folly::SocketAddress(host_, port_), /*timeout_ms*/ 5000);
    sock_->setReadCB(this);
}

void PeerOutboundSocket::connectSuccess() noexcept {
    connected_ = true;
    // No queued payloads under the current design — send() writes immediately if the socket has a connect in
    // flight, and folly buffers the bytes until connected.
}

void PeerOutboundSocket::connectErr(folly::AsyncSocketException const& ex) noexcept {
    connected_ = false;
    fail_socket(std::string("connect failed: ") + ex.what());
}

void PeerOutboundSocket::send(nuraft::group_id_t const& group_id, uint8_t msg_type, unique< folly::IOBuf > payload,
                              nuraft::rpc_handler when_done, nuraft::ptr< nuraft::req_msg > req,
                              std::chrono::milliseconds timeout) {
    uint64_t req_id = next_req_id_++;
    unique< TimeoutCallback > tcb;
    if (timeout.count() > 0) {
        tcb = std::make_unique< TimeoutCallback >(this, req_id);
        timer_wheel_->scheduleTimeout(tcb.get(), timeout);
    }
    pending_.emplace(req_id, PendingEntry{std::move(when_done), msg_type, std::move(tcb), std::move(req)});
    RPC_SOCK_LOG(TRACE, "tx request req_id={} msg_type={}", req_id, int(msg_type));
    auto wire = WireFrame::build(group_id, req_id, /*is_response=*/false, std::move(payload));
    sock_->writeChain(this, std::move(wire), folly::WriteFlags::WRITE_MSG_ZEROCOPY);
}

void PeerOutboundSocket::fail_pending_request(uint64_t req_id) {
    auto it = pending_.find(req_id);
    if (it == pending_.end()) {
        return;
    }
    auto entry = std::move(it->second);
    pending_.erase(it);
    nuraft::ptr< nuraft::resp_msg > null_resp;
    auto ex = nuraft::cs_new< nuraft::rpc_exception >("send timeout", entry.req);
    entry.when_done(null_resp, ex);
}

void PeerOutboundSocket::writeErr(size_t /*bytes_written*/, folly::AsyncSocketException const& ex) noexcept {
    fail_socket(std::string("write failed: ") + ex.what());
    if (sock_) {
        sock_->closeNow();
    }
}

void PeerOutboundSocket::getReadBuffer(void** bufReturn, size_t* lenReturn) {
    if (rx_state_ == RxState::Header) {
        *bufReturn = rx_hdr_buf_ + rx_hdr_filled_;
        *lenReturn = WireFrame::kHeaderSize - rx_hdr_filled_;
    } else {
        *bufReturn = rx_body_->data_begin() + rx_body_filled_;
        *lenReturn = rx_body_->size() - rx_body_filled_;
    }
}

void PeerOutboundSocket::readDataAvailable(size_t len) noexcept {
    if (rx_state_ == RxState::Header) {
        rx_hdr_filled_ += len;
        if (rx_hdr_filled_ < WireFrame::kHeaderSize) {
            return;
        }
        // Header complete — parse it.
        folly::IOBuf hdr_buf(folly::IOBuf::WRAP_BUFFER, rx_hdr_buf_, WireFrame::kHeaderSize);
        folly::io::Cursor cur(&hdr_buf);
        if (!WireFrame::decode_header(cur, rx_hdr_)) {
            readErr(folly::AsyncSocketException(folly::AsyncSocketException::INVALID_STATE, "bad frame header"));
            return;
        }
        if (rx_hdr_.payload_len == 0) {
            // Zero-length payload — process immediately.
            rx_state_ = RxState::Header;
            rx_hdr_filled_ = 0;
            return;
        }
        rx_body_ = nuraft::buffer::alloc(rx_hdr_.payload_len);
        rx_body_filled_ = 0;
        rx_state_ = RxState::Body;
        return;
    }
    // Body state
    rx_body_filled_ += len;
    if (rx_body_filled_ < rx_body_->size()) {
        return;
    }

    // Full frame received. We expect a response on this socket (outbound side); decode and fire handler.
    if (rx_hdr_.is_response()) {
        auto resp = decode_resp_msg(*rx_body_);
        auto it = pending_.find(rx_hdr_.req_id);
        RPC_SOCK_LOG(TRACE, "rx response req_id={} matched={}", rx_hdr_.req_id, (it != pending_.end()));
        if (it != pending_.end()) {
            auto entry = std::move(it->second);
            pending_.erase(it);
            if (entry.timeout) {
                entry.timeout->cancelTimeout();
            }
            nuraft::ptr< nuraft::rpc_exception > no_err;
            if (is_cpu_intensive_rpc(entry.sent_msg_type) && cpu_executor_) {
                // Hand the handler off so its possibly-blocking work (e.g. snapshot_resp processing that may
                // pull old log entries) does not stall the outbound reactor.
                cpu_executor_->add([wd = std::move(entry.when_done), resp, no_err]() mutable { wd(resp, no_err); });
            } else {
                entry.when_done(resp, no_err);
            }
        }
        // else: stale response, ignore
    }
    // Reset state machine for next frame.
    rx_body_.reset();
    rx_body_filled_ = 0;
    rx_hdr_filled_ = 0;
    rx_state_ = RxState::Header;
}

void PeerOutboundSocket::readEOF() noexcept {
    fail_socket("peer closed connection");
    if (sock_) {
        sock_->closeNow();
    }
}

void PeerOutboundSocket::readErr(folly::AsyncSocketException const& ex) noexcept {
    fail_socket(std::string("read failed: ") + ex.what());
    if (sock_) {
        sock_->closeNow();
    }
}

void PeerOutboundSocket::register_client(std::weak_ptr< FollyRpcClient > client) {
    // Dedup by raw pointer identity — same client registering twice (e.g. multiple sends) shouldn't fan out.
    auto* raw = client.lock().get();
    if (raw == nullptr) {
        return;
    }
    for (auto const& existing : clients_) {
        if (existing.lock().get() == raw) {
            return;
        }
    }
    clients_.push_back(std::move(client));
}

void PeerOutboundSocket::fail_socket(std::string const& reason) {
    // Terminal: this socket never reconnects on its own.  Mark it so the factory drops it from its per-reactor map on
    // the next get_or_open_outbound and opens a fresh socket (which reconnects) instead of handing back this dead one.
    failed_ = true;

    // Mark every still-live client routed through this socket as abandoned so nuraft drops them and
    // re-creates a fresh client on the next round.
    for (auto& w : clients_) {
        if (auto sp = w.lock()) {
            sp->set_abandoned();
        }
    }
    clients_.clear();

    // Drain pending request handlers with a per-request rpc_exception carrying that request (nuraft's error path
    // dereferences err->req()).  Cancel each entry's timer first so the timer wheel doesn't hold a stale callback
    // pointer past the entry's destruction.
    auto drained = std::move(pending_);
    pending_.clear();
    nuraft::ptr< nuraft::resp_msg > null_resp;
    for (auto& kv : drained) {
        if (kv.second.timeout) {
            kv.second.timeout->cancelTimeout();
        }
        auto ex = nuraft::cs_new< nuraft::rpc_exception >(reason, kv.second.req);
        kv.second.when_done(null_resp, ex);
    }
}

} // namespace homestore::replication