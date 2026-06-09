#include "folly_rpc_listener.h"
#include "wire_frame.h"

#include <cstring>

#include <folly/coro/Invoke.h>
#include <folly/coro/Task.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <libnuraft/buffer.hxx>
#include <libnuraft/log_entry.hxx>
#include <libnuraft/log_val_type.hxx>
#include <libnuraft/raft_server.hxx>
#include <libnuraft/req_msg.hxx>
#include <libnuraft/resp_msg.hxx>

#include "iomanager/iomanager.h"
#include "homestore/replication/repl_manager.h"

namespace homestore::replication {

namespace {

constexpr uint8_t kMarkerRequest = 0x00;
constexpr uint8_t kMarkerResponse = 0x01;

// Wire-layout descriptors — same shape as in folly_rpc_client.cpp; sizeof() gives the framed size and the
// fields document layout field-by-field. Layout is locked via static_assert so encoder and decoder can't drift.
#pragma pack(1)
struct WireReqHeader {
    uint8_t marker;
    uint8_t msg_type;
    int32_t src;
    int32_t dst;
    uint64_t term;
    uint64_t last_log_term;
    uint64_t last_log_idx;
    uint64_t commit_idx;
    uint32_t n_entries;
    uint64_t extra_flags;
};
static_assert(sizeof(WireReqHeader) == 54);

// Per-entry wire metadata, after §11. term + val_type are NOT here — they're embedded in the first 9 bytes
// of the entry's bufs_[0], so they ride inside the per-entry payload region itself.
struct WireLogEntryHeader {
    uint8_t has_crc32;
    uint32_t crc32;
    uint64_t timestamp;
    uint32_t size; // bytes that follow this header — includes the embedded [term | val_type] prefix
};
static_assert(sizeof(WireLogEntryHeader) == 17);

struct WireRespHeader {
    uint8_t marker;
    uint8_t msg_type;
    int32_t src;
    int32_t dst;
    uint64_t term;
    uint64_t next_idx;
    uint8_t accepted;
    int32_t result_code;
    int64_t hint;
    uint32_t ctx_size;
};
static_assert(sizeof(WireRespHeader) == 43);
#pragma pack()

constexpr size_t kReqHeaderSize = sizeof(WireReqHeader);
constexpr size_t kLogEntryHeaderSize = sizeof(WireLogEntryHeader);
constexpr size_t kRespHeaderSize = sizeof(WireRespHeader);

inline void put_u8(uint8_t*& p, uint8_t v) {
    *p++ = v;
}
inline void put_le_u32(uint8_t*& p, uint32_t v) {
    std::memcpy(p, &v, 4);
    p += 4;
}
inline void put_le_u64(uint8_t*& p, uint64_t v) {
    std::memcpy(p, &v, 8);
    p += 8;
}
inline void put_le_i32(uint8_t*& p, int32_t v) {
    std::memcpy(p, &v, 4);
    p += 4;
}
inline void put_le_i64(uint8_t*& p, int64_t v) {
    std::memcpy(p, &v, 8);
    p += 8;
}

// ── Parse a request payload into a fresh nuraft::req_msg with zero-copy log_entries ─
//
// Wire layout (separated form): [WireReqHeader] [WireLogEntryHeader x N] [payload_0] [payload_1] ... [payload_{N-1}]
// where each payload_i is `WireLogEntryHeader[i].size` bytes containing the entry's [term | val_type | value]
// contiguous slab (the bytes that ARE bufs_[0] in the constructed log_entry).
//
// We take `body` as a shared_ptr so the per-entry payload regions can be wrapped via buffer::take_ownership
// — the deleter captures `body` to keep the underlying bytes alive for as long as any constructed log_entry
// is live. The log_entry's bufs_[0] points straight into `body`'s storage. Zero copy on RX.
nuraft::ptr< nuraft::req_msg > decode_req_msg(RaftBufferPtr const& body) {
    nuraft::buffer& payload = *body;
    if (payload.size() < kReqHeaderSize) {
        return nullptr;
    }
    payload.pos(0);

    uint8_t marker = payload.get_byte();
    if (marker != kMarkerRequest) {
        return nullptr;
    }
    auto type = static_cast< nuraft::msg_type >(payload.get_byte());
    int32_t src = payload.get_int();
    int32_t dst = payload.get_int();
    uint64_t term = payload.get_ulong();
    uint64_t ll_term = payload.get_ulong();
    uint64_t ll_idx = payload.get_ulong();
    uint64_t cm_idx = payload.get_ulong();
    uint32_t n_le = static_cast< uint32_t >(payload.get_int());
    uint64_t extra = payload.get_ulong();

    // After ReqHdr we have N * WireLogEntryHeader (17B each). The per-entry payload region begins right
    // after — each entry's bytes are tightly packed in that region in order.
    size_t const headers_end = kReqHeaderSize + static_cast< size_t >(n_le) * kLogEntryHeaderSize;
    if (payload.size() < headers_end) {
        return nullptr;
    }

    // First pass: read every WireLogEntryHeader and record each entry's metadata + its payload offset
    // inside body. We need all sizes before we can build the second-pass take_ownership slices.
    struct EntryMeta {
        bool has_crc;
        uint32_t crc;
        uint64_t ts;
        uint32_t size;
        size_t payload_offset;
    };
    folly::small_vector< EntryMeta, 4 > metas;
    metas.reserve(n_le);
    size_t cursor = headers_end;
    for (uint32_t i = 0; i < n_le; ++i) {
        EntryMeta m;
        m.has_crc = (payload.get_byte() != 0);
        m.crc = static_cast< uint32_t >(payload.get_int());
        m.ts = payload.get_ulong();
        m.size = static_cast< uint32_t >(payload.get_int());
        m.payload_offset = cursor;
        cursor += m.size;
        metas.push_back(m);
    }
    if (payload.size() < cursor) {
        return nullptr;
    }

    auto req = nuraft::cs_new< nuraft::req_msg >(term, type, src, dst, ll_term, ll_idx, cm_idx);
    req->set_extra_flags(extra);

    // Second pass: for each entry, take_ownership wraps the payload bytes [payload_offset, +size) — capturing
    // `body` in the deleter so the storage outlives every constructed log_entry. from_serialized installs
    // those bytes directly as bufs_[0], whose first 9 bytes ARE the [term | val_type] header.
    for (auto const& m : metas) {
        auto le_buf = nuraft::buffer::take_ownership(payload.data_begin() + m.payload_offset, m.size,
                                                     [held = body](nuraft::byte*) noexcept { (void)held; });
        auto le = nuraft::log_entry::from_serialized(std::move(le_buf), m.ts, m.has_crc, m.crc);
        req->log_entries().push_back(le);
    }
    return req;
}

// ── Build a contiguous IOBuf for an outbound resp_msg payload ─────────────────────────────────────
unique< folly::IOBuf > encode_resp_msg(nuraft::resp_msg const& resp) {
    auto ctx = resp.get_ctx();
    uint32_t ctx_size = ctx ? to_u32(ctx->size()) : 0;
    size_t total = kRespHeaderSize + ctx_size;
    auto out = folly::IOBuf::create(total);
    uint8_t* p = out->writableTail();
    put_u8(p, kMarkerResponse);
    put_u8(p, static_cast< uint8_t >(resp.get_type()));
    put_le_i32(p, resp.get_src());
    put_le_i32(p, resp.get_dst());
    put_le_u64(p, resp.get_term());
    put_le_u64(p, resp.get_next_idx());
    put_u8(p, resp.get_accepted() ? 1 : 0);
    put_le_i32(p, static_cast< int32_t >(resp.get_result_code()));
    put_le_i64(p, resp.get_next_batch_size_hint_in_bytes());
    put_le_u32(p, ctx_size);
    if (ctx_size > 0) {
        std::memcpy(p, ctx->data_begin(), ctx_size);
        p += ctx_size;
    }
    out->append(total);
    return out;
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                       FollyRpcListener
// ══════════════════════════════════════════════════════════════════════════════════════════════════

FollyRpcListener::FollyRpcListener(folly::EventBase* accept_eb, uint16_t port, ReplicationManager* mgr) :
        accept_eb_(accept_eb), port_(port), mgr_(mgr) {
}

FollyRpcListener::~FollyRpcListener() {
    shutdown();
}

void FollyRpcListener::listen(nuraft::ptr< nuraft::msg_handler >& /*unused*/) {
    server_ = folly::AsyncServerSocket::UniquePtr(new folly::AsyncServerSocket(accept_eb_));
    server_->bind(port_);
    server_->listen(/*backlog*/ 128);
    accept_cb_ = std::make_unique< AcceptCb >(mgr_);
    server_->addAcceptCallback(accept_cb_.get(), accept_eb_);
    server_->startAccepting();
}

void FollyRpcListener::stop() {
    if (server_) {
        server_->stopAccepting();
    }
}

void FollyRpcListener::shutdown() {
    if (server_) {
        server_->stopAccepting();
        server_.reset();
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                          AcceptCb
// ══════════════════════════════════════════════════════════════════════════════════════════════════

void FollyRpcListener::AcceptCb::connectionAccepted(folly::NetworkSocket fd, folly::SocketAddress const& /*client*/,
                                                    AcceptInfo /*info*/) noexcept {
    auto& iom = iomanager::iomgr();
    size_t rid = iom.next_reactor();
    auto* recv_eb = iom.reactor_for(rid);
    auto* mgr = mgr_;
    recv_eb->runInEventBaseThread([fd, recv_eb, mgr]() {
        folly::AsyncSocket::UniquePtr sock(new folly::AsyncSocket(recv_eb, fd));
        sock->setZeroCopy(true);
        auto conn = std::make_shared< FollyRpcListener::InboundConnection >(recv_eb, std::move(sock), mgr);
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
                                                       ReplicationManager* mgr) :
        eb_(eb), sock_(std::move(sock)), mgr_(mgr) {
}

FollyRpcListener::InboundConnection::~InboundConnection() {
    if (sock_) {
        sock_->closeNow();
    }
}

void FollyRpcListener::InboundConnection::start() {
    sock_->setReadCB(this);
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
        if (is_slow_rpc(msg_type) && mgr_->slow_executor() != nullptr) {
            // Slow path: route to the CPU thread pool so the Task body (which may blocking-wait on log_store
            // reads via HomeRaftLogStore's sync API) does not stall any reactor.
            std::move(dispatch_request(rx_hdr_.group_id, rx_hdr_.req_id, rx_body_))
                .scheduleOn(folly::Executor::getKeepAliveToken(mgr_->slow_executor()))
                .start();
        } else {
            // Hot path: startInlineUnsafe runs the Task body synchronously on this thread (we're already on
            // eb_) until process_req hits its first suspend point; no event-loop tick delay.
            std::move(dispatch_request(rx_hdr_.group_id, rx_hdr_.req_id, rx_body_))
                .scheduleOn(folly::Executor::getKeepAliveToken(eb_))
                .startInlineUnsafe();
        }
    }
    rx_body_.reset();
    rx_body_filled_ = 0;
    rx_hdr_filled_ = 0;
    rx_state_ = RxState::Header;
}

folly::coro::Task< void > FollyRpcListener::InboundConnection::dispatch_request(nuraft::group_id_t gid, uint64_t req_id,
                                                                                nuraft::ptr< nuraft::buffer > body) {
    auto req = decode_req_msg(body);
    if (!req) {
        co_return;
    }
    auto srv = mgr_->lookup_or_create_raft_server(gid);
    if (!srv) {
        // Unknown group_id — drop. (Future: send an error response.)
        co_return;
    }

    // Task stickiness keeps us on eb_ across the co_await — log_store flush posts the durability_signal_
    // baton on a log thread, but folly's await_transform re-enqueues the resume onto eb_ before continuing
    // process_req's body. After the await returns we are guaranteed to be on eb_ where the socket lives,
    // so send_response can write directly with no runInEventBaseThread hop.
    nuraft::raft_server::req_ext_params ext{};
    auto resp = co_await srv->process_req(*req, ext);
    send_response(gid, req_id, resp);
    co_return;
}

void FollyRpcListener::InboundConnection::send_response(nuraft::group_id_t const& gid, uint64_t req_id,
                                                        nuraft::ptr< nuraft::resp_msg > resp) {
    if (closed_.load(std::memory_order_acquire) || !sock_) {
        return;
    }
    auto payload = resp ? encode_resp_msg(*resp) : folly::IOBuf::create(0);
    auto wire = WireFrame::build(gid, req_id, /*is_response=*/true, std::move(payload));
    sock_->writeChain(this, std::move(wire), folly::WriteFlags::WRITE_MSG_ZEROCOPY);
}

void FollyRpcListener::InboundConnection::readEOF() noexcept {
    closed_.store(true, std::memory_order_release);
    if (sock_) {
        sock_->closeNow();
    }
}

void FollyRpcListener::InboundConnection::readErr(folly::AsyncSocketException const& /*ex*/) noexcept {
    closed_.store(true, std::memory_order_release);
    if (sock_) {
        sock_->closeNow();
    }
}

void FollyRpcListener::InboundConnection::writeErr(size_t /*bytes*/,
                                                   folly::AsyncSocketException const& /*ex*/) noexcept {
    closed_.store(true, std::memory_order_release);
    if (sock_) {
        sock_->closeNow();
    }
}

} // namespace homestore::replication
