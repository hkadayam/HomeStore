#include "folly_rpc_client.h"
#include "folly_rpc_client_factory.h"
#include "wire_frame.h"

#include <cassert>
#include <cstring>

#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/SocketAddress.h>
#include <libnuraft/buffer.hxx>
#include <libnuraft/log_entry.hxx>
#include <libnuraft/log_val_type.hxx>
#include <libnuraft/req_msg.hxx>
#include <libnuraft/resp_msg.hxx>
#include <libnuraft/rpc_exception.hxx>

#include "iomanager/iomanager.h"

namespace homestore::replication {

namespace {

constexpr uint8_t kMarkerRequest = 0x00;

// Wire-layout descriptors. sizeof() gives the framed size; fields document field-by-field layout. Encode /
// decode loops still write/read fields with put_*/get_* helpers — these structs are not memcpy'd, just used
// for sizing and as a single source of truth for the layout.
#pragma pack(1)
struct WireReqHeader {
    uint8_t  marker;
    uint8_t  msg_type;
    int32_t  src;
    int32_t  dst;
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
    uint8_t  has_crc32;
    uint32_t crc32;
    uint64_t timestamp;
    uint32_t size;       // bytes that follow this header — includes the embedded [term | val_type] prefix
};
static_assert(sizeof(WireLogEntryHeader) == 17);

struct WireRespHeader {
    uint8_t  marker;
    uint8_t  msg_type;
    int32_t  src;
    int32_t  dst;
    uint64_t term;
    uint64_t next_idx;
    uint8_t  accepted;
    int32_t  result_code;
    int64_t  hint;
    uint32_t ctx_size;
};
static_assert(sizeof(WireRespHeader) == 43);
#pragma pack()

constexpr size_t kReqHeaderSize      = sizeof(WireReqHeader);
constexpr size_t kLogEntryHeaderSize = sizeof(WireLogEntryHeader);
constexpr size_t kRespHeaderSize     = sizeof(WireRespHeader);

// ── Little-endian field writers into a raw byte pointer ──────────────────────────────────────────
inline void put_u8(uint8_t*& p, uint8_t v)   { *p++ = v; }
inline void put_le_u32(uint8_t*& p, uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
inline void put_le_u64(uint8_t*& p, uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
inline void put_le_i32(uint8_t*& p, int32_t v)  { std::memcpy(p, &v, 4); p += 4; }
inline void put_le_i64(uint8_t*& p, int64_t v)  { std::memcpy(p, &v, 8); p += 8; }

// ── Build the IOBuf chain for an outbound req_msg payload ────────────────────────────────────────
//
// Returns: [head IOBuf with req + per-entry metadata]
//          -> [chain link per log_entry payload buffer, takeOwnership zero-copy]
unique< folly::IOBuf > encode_req_msg(nuraft::req_msg const& req) {
    auto& entries = const_cast< nuraft::req_msg& >(req).log_entries();
    size_t head_size = kReqHeaderSize + entries.size() * kLogEntryHeaderSize;
    auto head = folly::IOBuf::create(head_size);

    uint8_t* p = head->writableTail();
    put_u8     (p, kMarkerRequest);
    put_u8     (p, static_cast< uint8_t >(req.get_type()));
    put_le_i32 (p, req.get_src());
    put_le_i32 (p, req.get_dst());
    put_le_u64 (p, req.get_term());
    put_le_u64 (p, req.get_last_log_term());
    put_le_u64 (p, req.get_last_log_idx());
    put_le_u64 (p, req.get_commit_idx());
    put_le_u32 (p, to_u32(entries.size()));
    put_le_u64 (p, req.get_extra_flags());

    for (auto& le : entries) {
        // term + val_type are NOT written here — they live in the first 9 bytes of bufs_[0] and ride inside
        // the per-entry payload region instead. total_size() already includes those 9 bytes.
        put_u8     (p, le->has_crc32() ? 1 : 0);
        put_le_u32 (p, le->get_crc32());
        put_le_u64 (p, le->get_timestamp());
        put_le_u32 (p, to_u32(le->total_size()));
    }
    head->append(head_size);

    // Append one chain link per log_entry payload buffer — zero-copy via takeOwnership; the lambda holds a
    // ptr<buffer> alive until folly drops the IOBuf (after MSG_ZEROCOPY completion fires).
    for (auto& le : entries) {
        for (auto& part : le->bufs()) {
            if (!part || part->size() == 0) { continue; }
            head->appendToChain(folly::IOBuf::takeOwnership(
                part->data_begin(), part->size(),
                [held = part](void*, void*) noexcept {
                    (void)held;
                }));
        }
    }
    return head;
}

// ── Parse an inbound resp_msg payload from a contiguous nuraft::buffer ───────────────────────────
nuraft::ptr< nuraft::resp_msg > decode_resp_msg(nuraft::buffer& payload) {
    if (payload.size() < kRespHeaderSize) { return nullptr; }
    payload.pos(0);

    uint8_t marker = payload.get_byte();
    if (marker != WireFrame::kFlagResponse) { return nullptr; }
    auto type = static_cast< nuraft::msg_type >(payload.get_byte());
    int32_t  src         = payload.get_int();
    int32_t  dst         = payload.get_int();
    uint64_t term        = payload.get_ulong();
    uint64_t next_idx    = payload.get_ulong();
    uint8_t  accepted    = payload.get_byte();
    int32_t  result_code = payload.get_int();
    int64_t  hint        = static_cast< int64_t >(payload.get_ulong());
    uint32_t ctx_size    = static_cast< uint32_t >(payload.get_int());

    auto resp = nuraft::cs_new< nuraft::resp_msg >(term, type, src, dst, next_idx, accepted != 0);
    resp->set_result_code(static_cast< nuraft::cmd_result_code >(result_code));
    resp->set_next_batch_size_hint_in_bytes(hint);
    if (ctx_size > 0) {
        if (payload.size() - payload.pos() < ctx_size) { return nullptr; }
        auto ctx = nuraft::buffer::alloc(ctx_size);
        std::memcpy(ctx->data_begin(), payload.get_raw(ctx_size), ctx_size);
        resp->set_ctx(ctx);
    }
    return resp;
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                     FollyRpcClient
// ══════════════════════════════════════════════════════════════════════════════════════════════════

FollyRpcClient::FollyRpcClient(FollyRpcClientFactory* factory,
                               std::string peer_host,
                               uint16_t peer_port,
                               uint64_t client_id)
    : factory_(factory)
    , peer_host_(std::move(peer_host))
    , peer_port_(peer_port)
    , client_id_(client_id) {}

FollyRpcClient::~FollyRpcClient() = default;

void FollyRpcClient::send(nuraft::ptr< nuraft::req_msg >& req,
                          nuraft::group_id_t const& group_id,
                          nuraft::rpc_handler& when_done,
                          uint64_t send_timeout_ms) {
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

    auto do_send = [factory, host, port, group_id, msg_type, payload = std::move(payload),
                    handler_copy = std::move(handler_copy), timeout]() mutable {
        auto& sock = factory->get_or_open_outbound(host, port);
        sock.send(group_id, msg_type, std::move(payload), std::move(handler_copy), timeout);
    };

    auto& iom = iomanager::iomgr();
    if (iom.current_reactor_id() < iom.num_reactors()) {
        // Already on a reactor — dispatch inline.
        do_send();
    } else {
        // Cold path: hop to a sticky reactor chosen by hashing the peer endpoint.
        size_t rid = factory_->pick_reactor_for_cold_path(peer_host_, peer_port_);
        iom.reactor_for(rid)->runInEventBaseThread(std::move(do_send));
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
//                                  PeerOutboundSocket
// ══════════════════════════════════════════════════════════════════════════════════════════════════

PeerOutboundSocket::PeerOutboundSocket(folly::EventBase* eb,
                                       std::string host,
                                       uint16_t port,
                                       folly::Executor* slow_executor)
    : eb_(eb)
    , host_(std::move(host))
    , port_(port)
    , slow_executor_(slow_executor) {
    open_connection();
}

PeerOutboundSocket::~PeerOutboundSocket() {
    fail_all_pending(nuraft::cs_new< nuraft::rpc_exception >("socket destroyed",
                                                             nuraft::ptr< nuraft::req_msg >()));
    if (sock_) { sock_->closeNow(); }
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
    fail_all_pending(nuraft::cs_new< nuraft::rpc_exception >(
        std::string("connect failed: ") + ex.what(),
        nuraft::ptr< nuraft::req_msg >()));
}

void PeerOutboundSocket::send(nuraft::group_id_t const& group_id,
                              uint8_t msg_type,
                              unique< folly::IOBuf > payload,
                              nuraft::rpc_handler when_done,
                              std::chrono::milliseconds /*timeout*/) {
    uint64_t req_id = next_req_id_++;
    pending_.emplace(req_id, PendingEntry{std::move(when_done), msg_type});
    auto wire = WireFrame::build(group_id, req_id, /*is_response=*/false, std::move(payload));
    sock_->writeChain(this, std::move(wire), folly::WriteFlags::WRITE_MSG_ZEROCOPY);
}

void PeerOutboundSocket::writeErr(size_t /*bytes_written*/,
                                  folly::AsyncSocketException const& ex) noexcept {
    fail_all_pending(nuraft::cs_new< nuraft::rpc_exception >(
        std::string("write failed: ") + ex.what(),
        nuraft::ptr< nuraft::req_msg >()));
    if (sock_) { sock_->closeNow(); }
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
        if (rx_hdr_filled_ < WireFrame::kHeaderSize) { return; }
        // Header complete — parse it.
        folly::IOBuf hdr_buf(folly::IOBuf::WRAP_BUFFER, rx_hdr_buf_, WireFrame::kHeaderSize);
        folly::io::Cursor cur(&hdr_buf);
        if (!WireFrame::decode_header(cur, rx_hdr_)) {
            readErr(folly::AsyncSocketException(folly::AsyncSocketException::INVALID_STATE,
                                                "bad frame header"));
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
    if (rx_body_filled_ < rx_body_->size()) { return; }

    // Full frame received. We expect a response on this socket (outbound side); decode and fire handler.
    if (rx_hdr_.is_response()) {
        auto resp = decode_resp_msg(*rx_body_);
        auto it = pending_.find(rx_hdr_.req_id);
        if (it != pending_.end()) {
            auto entry = std::move(it->second);
            pending_.erase(it);
            nuraft::ptr< nuraft::rpc_exception > no_err;
            if (is_slow_rpc(entry.sent_msg_type) && slow_executor_) {
                // Hand the handler off so its possibly-blocking work (e.g. snapshot_resp processing that may
                // pull old log entries) does not stall the outbound reactor.
                slow_executor_->add([wd = std::move(entry.when_done), resp, no_err]() mutable {
                    wd(resp, no_err);
                });
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
    fail_all_pending(nuraft::cs_new< nuraft::rpc_exception >(
        "peer closed connection", nuraft::ptr< nuraft::req_msg >()));
    if (sock_) { sock_->closeNow(); }
}

void PeerOutboundSocket::readErr(folly::AsyncSocketException const& ex) noexcept {
    fail_all_pending(nuraft::cs_new< nuraft::rpc_exception >(
        std::string("read failed: ") + ex.what(),
        nuraft::ptr< nuraft::req_msg >()));
    if (sock_) { sock_->closeNow(); }
}

void PeerOutboundSocket::fail_all_pending(nuraft::ptr< nuraft::rpc_exception > ex) {
    auto drained = std::move(pending_);
    pending_.clear();
    nuraft::ptr< nuraft::resp_msg > null_resp;
    for (auto& kv : drained) {
        kv.second.when_done(null_resp, ex);
    }
}

} // namespace homestore::replication