#include "folly_rpc_listener.h"
#include "nuraft_codec.h"
#include "wire_frame.h"

#include "common/async.h"
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <libnuraft/buffer.hxx>
#include <libnuraft/raft_server.hxx>
#include <libnuraft/req_msg.hxx>
#include <libnuraft/resp_msg.hxx>

#include "iomanager/iomanager.h"
#include "homestore/replication/repl_manager.h"

namespace homestore::replication {

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                       FollyRpcListener
// ══════════════════════════════════════════════════════════════════════════════════════════════════

FollyRpcListener::FollyRpcListener(folly::EventBase* accept_eb, uint16_t port, ReplicationManager* mgr) :
        accept_eb_(accept_eb), port_(port), mgr_(mgr), registry_(std::make_shared< ConnectionRegistry >()) {
}

FollyRpcListener::~FollyRpcListener() {
    shutdown();
}

void FollyRpcListener::listen(nuraft::ptr< nuraft::msg_handler >& /*unused*/) {
    server_ = folly::AsyncServerSocket::UniquePtr(new folly::AsyncServerSocket(accept_eb_));
    server_->bind(port_);
    server_->listen(/*backlog*/ 128);
    accept_cb_ = std::make_unique< AcceptCb >(mgr_, registry_);
    server_->addAcceptCallback(accept_cb_.get(), nullptr);
    server_->startAccepting();
}

void FollyRpcListener::stop() {
    if (server_) {
        server_->stopAccepting();
    }
}

void FollyRpcListener::shutdown() {
    if (server_) {
        // The AsyncServerSocket is bound to accept_eb_ and folly requires it to be stopped/destroyed on that
        // EventBase's thread. shutdown() may run on a different reactor (HomeStore shutdown dispatches to any
        // reactor), so hop to accept_eb_ and wait. Runs inline when already on that thread.
        accept_eb_->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
            server_->stopAccepting();
            server_.reset();
            accept_cb_.reset();
        });
    } else {
        accept_cb_.reset();
    }

    // Snapshot the registry under the mutex, then post closeNow() onto each connection's reactor.  Tasks
    // still holding shared_from_this() keep their connection alive until they finish; their post-shutdown
    // send_response calls silently no-op via the closed_ guard.
    std::unordered_map< InboundConnection*, shared< InboundConnection > > snapshot;
    if (registry_) {
        std::lock_guard lk{registry_->mtx};
        snapshot.swap(registry_->conns);
    }
    for (auto& kv : snapshot) {
        auto conn = kv.second; // shared_ptr captured by the lambda below keeps conn alive until close runs
        conn->event_base()->runInEventBaseThread([conn]() {
            conn->closed_ = true;
            if (conn->sock_) {
                conn->sock_->closeNow();
            }
        });
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                          AcceptCb
// ══════════════════════════════════════════════════════════════════════════════════════════════════

void FollyRpcListener::AcceptCb::connectionAccepted(folly::NetworkSocket fd, folly::SocketAddress const& /*client*/,
                                                    AcceptInfo /*info*/) noexcept {
    auto& iom = iomgr();
    size_t rid = iom.next_reactor();
    auto* recv_eb = iom.reactor_for(rid);
    auto* mgr = mgr_;
    auto registry = registry_; // captured shared so the registry outlives this lambda
    recv_eb->runInEventBaseThread([fd, recv_eb, mgr, registry]() {
        folly::AsyncSocket::UniquePtr sock(new folly::AsyncSocket(recv_eb, fd));
        sock->setZeroCopy(true);
        auto conn = std::make_shared< FollyRpcListener::InboundConnection >(recv_eb, std::move(sock), mgr, registry);
        {
            // Insert into the registry BEFORE start().  Without this, conn (the only shared_ptr) would die
            // when the lambda exits and the connection would be destroyed on the same tick it was created.
            std::lock_guard lk{registry->mtx};
            registry->conns.emplace(conn.get(), conn);
        }
        conn->start();
    });
}

void FollyRpcListener::AcceptCb::acceptError(folly::exception_wrapper /*ex*/) noexcept {
    // Logged elsewhere by folly; accept loop continues.
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                    InboundConnection
// ══════════════════════════════════════════════════════════════════════════════════════════════════

FollyRpcListener::InboundConnection::InboundConnection(folly::EventBase* eb, folly::AsyncSocket::UniquePtr sock,
                                                       ReplicationManager* mgr, shared< ConnectionRegistry > registry) :
        eb_(eb), sock_(std::move(sock)), mgr_(mgr), registry_(registry) {
}

FollyRpcListener::InboundConnection::~InboundConnection() {
    if (sock_) {
        sock_->closeNow();
    }
}

void FollyRpcListener::InboundConnection::start() {
    sock_->setReadCB(this);
}

void FollyRpcListener::InboundConnection::unregister_self() {
    if (auto reg = registry_.lock()) {
        std::lock_guard lk{reg->mtx};
        reg->conns.erase(this);
    }
}

void FollyRpcListener::InboundConnection::getReadBuffer(void** bufReturn, size_t* lenReturn) {
    if (rx_state_ == RxState::Header) {
        *bufReturn = rx_hdr_buf_ + rx_hdr_filled_;
        *lenReturn = WireFrame::kHeaderSize - rx_hdr_filled_;
    } else {
        *bufReturn = rx_body_->data_begin() + rx_body_filled_;
        *lenReturn = rx_body_->size() - rx_body_filled_;
    }
}

void FollyRpcListener::InboundConnection::readDataAvailable(size_t len) noexcept {
    if (rx_state_ == RxState::Header) {
        rx_hdr_filled_ += len;
        if (rx_hdr_filled_ < WireFrame::kHeaderSize) {
            return;
        }
        folly::IOBuf hdr_buf(folly::IOBuf::WRAP_BUFFER, rx_hdr_buf_, WireFrame::kHeaderSize);
        folly::io::Cursor cur(&hdr_buf);
        if (!WireFrame::decode_header(cur, rx_hdr_)) {
            readErr(folly::AsyncSocketException(folly::AsyncSocketException::INVALID_STATE, "bad frame header"));
            return;
        }
        if (rx_hdr_.payload_len == 0) {
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

    // Full frame received. We expect a request on this socket (inbound side); dispatch.
    if (!rx_hdr_.is_response()) {
        // Wire-format invariant — body[0] is the request marker (kMarkerRequest), body[1] is the nuraft
        // msg_type byte. Peeking it here lets routing decide reactor vs slow-executor without paying for full
        // req_msg decode up front.
        uint8_t const msg_type = rx_body_->size() >= 2 ? rx_body_->data_begin()[1] : 0;
        HS_LOG(TRACE, replication, "rpc rx request type={} req_id={} len={}", int(msg_type), rx_hdr_.req_id,
               rx_body_->size());
        if (is_cpu_intensive_rpc(msg_type) && mgr_->cpu_executor() != nullptr) {
            // Slow path: route to the CPU thread pool so the Task body (which may blocking-wait on log_store
            // reads via HomeRaftLogStore's sync API) does not stall any reactor.
            folly::coro::co_withExecutor(folly::Executor::getKeepAliveToken(mgr_->cpu_executor()),
                                         dispatch_request(rx_hdr_.group_id, rx_hdr_.req_id, rx_body_))
                .start();
        } else {
            // Hot path: startInlineUnsafe runs the Task body synchronously on this thread (we're already on
            // eb_) until process_req hits its first suspend point; no event-loop tick delay.
            folly::coro::co_withExecutor(folly::Executor::getKeepAliveToken(eb_),
                                         dispatch_request(rx_hdr_.group_id, rx_hdr_.req_id, rx_body_))
                .startInlineUnsafe();
        }
    }
    rx_body_.reset();
    rx_body_filled_ = 0;
    rx_hdr_filled_ = 0;
    rx_state_ = RxState::Header;
}

Async< void > FollyRpcListener::InboundConnection::dispatch_request(nuraft::group_id_t gid, uint64_t req_id,
                                                                    nuraft::ptr< nuraft::buffer > body) {
    // Keep `this` alive across the co_await — even if the connection's terminal-state handler runs and
    // unregisters us from the listener registry, the Task continues until process_req completes.
    auto self = shared_from_this();

    auto req = decode_req_msg(body);
    if (!req) {
        co_return;
    }
    // Hot path: sync lookup of an already-registered group.
    auto srv = mgr_->lookup_raft_server(gid);
    if (!srv) {
        // Cold path: first message for an unknown group_id — ask the application if it wants to host this
        // group, and (if so) construct the ReplicaSet.  Async because it does meta-block I/O.
        srv = co_await mgr_->create_replica_set_on_demand(gid);
    }
    if (!srv) {
        // This node doesn't host the group — synthesize a SERVER_NOT_FOUND response so the peer's pending
        // request resolves immediately instead of hanging until timeout.  Response msg_type is req_type+1
        // (nuraft pairs request/response at consecutive enum values).
        auto resp_type = static_cast< nuraft::msg_type >(static_cast< int >(req->get_type()) + 1);
        auto resp = nuraft::cs_new< nuraft::resp_msg >(/*term=*/0u, resp_type, /*src=*/0, req->get_src(),
                                                       /*next_idx=*/0u, /*accepted=*/false);
        resp->set_result_code(nuraft::cmd_result_code::SERVER_NOT_FOUND);
        send_response(gid, req_id, resp);
        co_return;
    }

    // Task stickiness keeps us on eb_ across the co_await — log_store flush posts the durability_signal_
    // baton on a log thread, but folly's await_transform re-enqueues the resume onto eb_ before continuing
    // process_req's body. After the await returns we are guaranteed to be on eb_ where the socket lives,
    // so send_response can write directly with no runInEventBaseThread hop.
    nuraft::raft_server::req_ext_params ext{};
    HS_LOG(TRACE, replication, "rpc process_req start type={} req_id={}", int(req->get_type()), req_id);
    auto resp = co_await nuraft::raft_server_handler::process_req(srv, *req, ext);
    HS_LOG(TRACE, replication, "rpc process_req done type={} req_id={} accepted={}", int(req->get_type()), req_id,
           resp ? int(resp->get_accepted()) : -1);
    send_response(gid, req_id, resp);
    co_return;
}

void FollyRpcListener::InboundConnection::send_response(nuraft::group_id_t const& gid, uint64_t req_id,
                                                        nuraft::ptr< nuraft::resp_msg > resp) {
    auto payload = resp ? encode_resp_msg(*resp) : folly::IOBuf::create(0);
    auto wire = WireFrame::build(gid, req_id, /*is_response=*/true, std::move(payload));
    // Hot path: caller is already on eb_ (dispatch_request started inline on eb_ and resumed on eb_ via
    // executor stickiness).  Slow / off-eb_ paths hop in via runInEventBaseThread.
    if (eb_->isInEventBaseThread()) {
        if (closed_ || !sock_) {
            return;
        }
        sock_->writeChain(this, std::move(wire), folly::WriteFlags::WRITE_MSG_ZEROCOPY);
    } else {
        auto self = shared_from_this();
        eb_->runInEventBaseThread([self, w = std::move(wire)]() mutable {
            if (self->closed_ || !self->sock_) {
                return;
            }
            self->sock_->writeChain(self.get(), std::move(w), folly::WriteFlags::WRITE_MSG_ZEROCOPY);
        });
    }
}

void FollyRpcListener::InboundConnection::readEOF() noexcept {
    closed_ = true;
    if (sock_) {
        sock_->closeNow();
    }
    unregister_self();
}

void FollyRpcListener::InboundConnection::readErr(folly::AsyncSocketException const& /*ex*/) noexcept {
    closed_ = true;
    if (sock_) {
        sock_->closeNow();
    }
    unregister_self();
}

void FollyRpcListener::InboundConnection::writeErr(size_t /*bytes*/,
                                                   folly::AsyncSocketException const& /*ex*/) noexcept {
    closed_ = true;
    if (sock_) {
        sock_->closeNow();
    }
    unregister_self();
}

} // namespace homestore::replication
