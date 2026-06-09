#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include <folly/io/IOBuf.h>
#include <folly/io/Cursor.h>
#include <libnuraft/basic_types.hxx>
#include <libnuraft/msg_type.hxx>

#include "common/defs.h"

namespace homestore::replication {

// Binary frame layout for the folly transport. Every TCP connection between two nodes carries traffic for
// multiple raft groups; group_id is the demux key on the receive end, and req_id is what a sender uses to match
// an incoming response frame back to the rpc_handler it registered at send time.
//
//   offset  size   field
//   ------  ----   -----
//   0       4      magic   ('HSRR' = 0x48535252, little-endian)
//   4       1      version (current = 1)
//   5       1      flags   (bit 0 = is_response; bits 1-7 reserved, must be 0)
//   6       2      reserved (must be 0; future alignment / extension)
//   8       16     group_id (raw uuid bytes)
//   24      8      req_id   (little-endian, set by sender, echoed verbatim on response)
//   32      4      payload_len (little-endian; bytes that follow the header)
//   36      <var>  payload  (nuraft serialized req_msg or resp_msg blob)
//
// req_id allocation: each outbound socket has its own monotonically increasing 64-bit counter starting at 1.
// Since req_ids are only ever looked up on the same outbound socket that allocated them (Design X — response
// returns over the same socket the request was sent on), collisions across sockets are irrelevant.
struct WireFrame {
    static constexpr uint32_t kMagic = 0x48535252u; // 'HSRR'
    static constexpr uint8_t kVersion = 1;
    static constexpr size_t kHeaderSize = 36;

    static constexpr uint8_t kFlagResponse = 0x01;

    uint8_t version{kVersion};
    uint8_t flags{0};
    nuraft::group_id_t group_id{};
    uint64_t req_id{0};
    uint32_t payload_len{0};

    bool is_response() const { return (flags & kFlagResponse) != 0; }
    void set_response() { flags |= kFlagResponse; }

    // Read a header from `cur`. cur must hold at least kHeaderSize bytes; returns false on bad magic or
    // unsupported version (caller closes the socket in that case).
    static bool decode_header(folly::io::Cursor& cur, WireFrame& out) {
        if (cur.totalLength() < kHeaderSize) {
            return false;
        }
        uint32_t const magic = cur.readLE< uint32_t >();
        if (magic != kMagic) {
            return false;
        }
        out.version = cur.read< uint8_t >();
        if (out.version != kVersion) {
            return false;
        }
        out.flags = cur.read< uint8_t >();
        cur.skip(2); // reserved
        cur.pull(out.group_id.data(), out.group_id.size());
        out.req_id = cur.readLE< uint64_t >();
        out.payload_len = cur.readLE< uint32_t >();
        return true;
    }

    // Encode `*this` into the writable tail of `buf` and advance the tail by kHeaderSize.
    void encode_header(folly::IOBuf& buf) const {
        uint8_t* p = buf.writableTail();
        std::memcpy(p, &kMagic, 4);
        p += 4;
        *p++ = version;
        *p++ = flags;
        *p++ = 0;
        *p++ = 0;
        std::memcpy(p, group_id.data(), group_id.size());
        p += group_id.size();
        std::memcpy(p, &req_id, 8);
        p += 8;
        std::memcpy(p, &payload_len, 4);
        buf.append(kHeaderSize);
    }

    // Build a chain: 36-byte header IOBuf followed by the caller-provided payload IOBuf appended as a chain
    // link. No payload bytes are copied; folly::AsyncSocket::writeChain() lowers the chain into a single
    // writev() syscall (which combined with MSG_ZEROCOPY makes the TX path zero copies application-side).
    //
    // Lifetime: caller wraps producer-owned bytes (e.g. a nuraft::buffer) via IOBuf::takeOwnership(...) with
    // a deleter holding the producer ptr; the IOBuf and its deleter survive until folly's WriteCallback fires
    // (which for MSG_ZEROCOPY means after the kernel-side completion notification).
    static unique< folly::IOBuf > build(nuraft::group_id_t const& gid, uint64_t req_id, bool is_response,
                                                 unique< folly::IOBuf > payload) {
        uint32_t const payload_size = payload ? to_u32(payload->computeChainDataLength()) : 0;
        auto out = folly::IOBuf::create(kHeaderSize);
        WireFrame hdr{};
        hdr.group_id = gid;
        hdr.req_id = req_id;
        if (is_response) {
            hdr.set_response();
        }
        hdr.payload_len = payload_size;
        hdr.encode_header(*out);
        if (payload && payload_size != 0) {
            out->appendToChain(std::move(payload));
        }
        return out;
    }
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// RPC routing classification — used by both the inbound (FollyRpcListener) and outbound (PeerOutboundSocket)
// dispatch paths. Slow RPCs may pull historical log entries (snapshot install / sync_log / membership) or
// trigger heavy bookkeeping, and are routed onto a dedicated CPU thread pool where blocking reads from the
// log_store are acceptable. Everything else (vote/pre_vote/append_entries/client_request/ping/etc.) stays on
// a reactor thread.
//
// Statically materialized as a 32-entry lookup table keyed by raw msg_type value — is_slow_rpc() compiles into
// one bounds check + one byte load, no branch on the type itself.
namespace detail {
inline constexpr auto build_slow_rpc_table() {
    std::array< bool, 32 > tbl{};
    tbl[nuraft::add_server_request]        = true;
    tbl[nuraft::add_server_response]       = true;
    tbl[nuraft::remove_server_request]     = true;
    tbl[nuraft::remove_server_response]    = true;
    tbl[nuraft::sync_log_request]          = true;
    tbl[nuraft::sync_log_response]         = true;
    tbl[nuraft::join_cluster_request]      = true;
    tbl[nuraft::join_cluster_response]     = true;
    tbl[nuraft::leave_cluster_request]     = true;
    tbl[nuraft::leave_cluster_response]    = true;
    tbl[nuraft::install_snapshot_request]  = true;
    tbl[nuraft::install_snapshot_response] = true;
    return tbl;
}
inline constexpr auto kSlowRpcTable = build_slow_rpc_table();
} // namespace detail

inline bool is_slow_rpc(uint8_t msg_type) {
    return msg_type < detail::kSlowRpcTable.size() && detail::kSlowRpcTable[msg_type];
}

} // namespace homestore::replication
