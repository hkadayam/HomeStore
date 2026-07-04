#include "nuraft_codec.h"

#include <cstring>

#include <folly/io/IOBuf.h>
#include <folly/small_vector.h>
#include <libnuraft/log_entry.hxx>
#include <libnuraft/log_val_type.hxx>
#include <libnuraft/cs_new.hxx>

namespace homestore::replication {

namespace {

// Wire-layout descriptors.  sizeof() gives the framed size; the structs document layout field-by-field.
// Encode / decode loops still write/read fields with put_*/get_* helpers — these structs are not memcpy'd,
// just used for sizing and as a single source of truth for the layout.
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

// Per-entry wire metadata.  term + val_type are NOT here — they're embedded in the first 9 bytes of the
// entry's bufs_[0] and ride inside the per-entry payload region itself.
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

// Little-endian field writers into a raw byte pointer.
inline void put_u8    (uint8_t*& p, uint8_t v)  { *p++ = v; }
inline void put_le_u32(uint8_t*& p, uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
inline void put_le_u64(uint8_t*& p, uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
inline void put_le_i32(uint8_t*& p, int32_t v)  { std::memcpy(p, &v, 4); p += 4; }
inline void put_le_i64(uint8_t*& p, int64_t v)  { std::memcpy(p, &v, 8); p += 8; }

} // namespace

// ──────────────────────────────────────────────────────────────────────────────────────────────────────────
// encode
// ──────────────────────────────────────────────────────────────────────────────────────────────────────────

unique< folly::IOBuf > encode_req_msg(nuraft::req_msg const& req) {
    auto& entries = const_cast< nuraft::req_msg& >(req).log_entries();
    size_t head_size = kReqHeaderSize + entries.size() * kLogEntryHeaderSize;
    auto head = folly::IOBuf::create(head_size);

    uint8_t* p = head->writableTail();
    put_u8    (p, kMarkerRequest);
    put_u8    (p, to_u8(req.get_type()));
    put_le_i32(p, req.get_src());
    put_le_i32(p, req.get_dst());
    put_le_u64(p, req.get_term());
    put_le_u64(p, req.get_last_log_term());
    put_le_u64(p, req.get_last_log_idx());
    put_le_u64(p, req.get_commit_idx());
    put_le_u32(p, to_u32(entries.size()));
    put_le_u64(p, req.get_extra_flags());

    for (auto& le : entries) {
        put_u8    (p, le->has_crc32() ? 1 : 0);
        put_le_u32(p, le->get_crc32());
        put_le_u64(p, le->get_timestamp());
        put_le_u32(p, to_u32(le->total_size()));
    }
    head->append(head_size);

    // One chain link per log_entry payload buffer — zero-copy via takeOwnership; the lambda holds a
    // ptr<buffer> alive until folly drops the IOBuf (after MSG_ZEROCOPY completion fires).
    for (auto& le : entries) {
        for (auto& part : le->bufs()) {
            if (!part || part->size() == 0) {
                continue;
            }
            head->appendToChain(folly::IOBuf::takeOwnership(part->data_begin(), part->size(),
                                                            [held = part](void*, void*) noexcept { (void)held; }));
        }
    }
    return head;
}

unique< folly::IOBuf > encode_resp_msg(nuraft::resp_msg const& resp) {
    auto ctx = resp.get_ctx();
    uint32_t ctx_size = ctx ? to_u32(ctx->size()) : 0;
    size_t total = kRespHeaderSize + ctx_size;
    auto out = folly::IOBuf::create(total);
    uint8_t* p = out->writableTail();
    put_u8    (p, kMarkerResponse);
    put_u8    (p, to_u8(resp.get_type()));
    put_le_i32(p, resp.get_src());
    put_le_i32(p, resp.get_dst());
    put_le_u64(p, resp.get_term());
    put_le_u64(p, resp.get_next_idx());
    put_u8    (p, resp.get_accepted() ? 1 : 0);
    put_le_i32(p, static_cast< int32_t >(resp.get_result_code()));   // enum→int direction
    put_le_i64(p, resp.get_next_batch_size_hint_in_bytes());
    put_le_u32(p, ctx_size);
    if (ctx_size > 0) {
        std::memcpy(p, ctx->data_begin(), ctx_size);
    }
    out->append(total);
    return out;
}

// ──────────────────────────────────────────────────────────────────────────────────────────────────────────
// decode
// ──────────────────────────────────────────────────────────────────────────────────────────────────────────

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
    // int→enum cast; defs.h has no helper for this direction.
    auto type     = static_cast< nuraft::msg_type >(payload.get_byte());
    int32_t  src     = payload.get_int();
    int32_t  dst     = payload.get_int();
    uint64_t term    = payload.get_ulong();
    uint64_t ll_term = payload.get_ulong();
    uint64_t ll_idx  = payload.get_ulong();
    uint64_t cm_idx  = payload.get_ulong();
    uint32_t n_le    = to_u32(payload.get_int());
    uint64_t extra   = payload.get_ulong();

    // After ReqHdr we have N * WireLogEntryHeader (17B each).  The per-entry payload region begins right
    // after — entries' bytes are tightly packed in that region in order.
    size_t const headers_end = kReqHeaderSize + static_cast< size_t >(n_le) * kLogEntryHeaderSize;
    if (payload.size() < headers_end) {
        return nullptr;
    }

    // First pass: read every WireLogEntryHeader and record each entry's metadata + payload offset inside
    // body.  We need all sizes before the second-pass take_ownership slices.
    struct EntryMeta {
        bool     has_crc;
        uint32_t crc;
        uint64_t ts;
        uint32_t size;
        size_t   payload_offset;
    };
    folly::small_vector< EntryMeta, 4 > metas;
    metas.reserve(n_le);
    size_t cursor = headers_end;
    for (uint32_t i = 0; i < n_le; ++i) {
        EntryMeta m;
        m.has_crc = (payload.get_byte() != 0);
        m.crc     = to_u32(payload.get_int());
        m.ts      = payload.get_ulong();
        m.size    = to_u32(payload.get_int());
        m.payload_offset = cursor;
        cursor += m.size;
        metas.push_back(m);
    }
    if (payload.size() < cursor) {
        return nullptr;
    }

    auto req = nuraft::cs_new< nuraft::req_msg >(term, type, src, dst, ll_term, ll_idx, cm_idx);
    req->set_extra_flags(extra);

    // Second pass: take_ownership wraps each entry's payload bytes — the deleter captures `body` so the
    // storage outlives every constructed log_entry.  from_serialized installs those bytes as bufs_[0],
    // whose first 9 bytes ARE the [term | val_type] header.
    for (auto const& m : metas) {
        auto le_buf = nuraft::buffer::take_ownership(payload.data_begin() + m.payload_offset, m.size,
                                                     [held = body](nuraft::byte*) noexcept { (void)held; });
        auto le = nuraft::log_entry::from_serialized(std::move(le_buf), m.ts, m.has_crc, m.crc);
        req->log_entries().push_back(le);
    }
    return req;
}

nuraft::ptr< nuraft::resp_msg > decode_resp_msg(nuraft::buffer& payload) {
    if (payload.size() < kRespHeaderSize) {
        return nullptr;
    }
    payload.pos(0);

    uint8_t marker = payload.get_byte();
    if (marker != kMarkerResponse) {
        return nullptr;
    }
    // int→enum cast; defs.h has no helper for this direction.
    auto type        = static_cast< nuraft::msg_type >(payload.get_byte());
    int32_t  src         = payload.get_int();
    int32_t  dst         = payload.get_int();
    uint64_t term        = payload.get_ulong();
    uint64_t next_idx    = payload.get_ulong();
    uint8_t  accepted    = payload.get_byte();
    int32_t  result_code = payload.get_int();
    int64_t  hint        = static_cast< int64_t >(payload.get_ulong());
    uint32_t ctx_size    = to_u32(payload.get_int());

    auto resp = nuraft::cs_new< nuraft::resp_msg >(term, type, src, dst, next_idx, accepted != 0);
    // int→enum cast; defs.h has no helper for this direction.
    resp->set_result_code(static_cast< nuraft::cmd_result_code >(result_code));
    resp->set_next_batch_size_hint_in_bytes(hint);
    if (ctx_size > 0) {
        if (payload.size() - payload.pos() < ctx_size) {
            return nullptr;
        }
        auto ctx = nuraft::buffer::alloc(ctx_size);
        std::memcpy(ctx->data_begin(), payload.get_raw(ctx_size), ctx_size);
        resp->set_ctx(ctx);
    }
    return resp;
}

} // namespace homestore::replication
