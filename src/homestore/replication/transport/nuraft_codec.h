#pragma once

#include <folly/io/IOBuf.h>
#include <libnuraft/buffer.hxx>
#include <libnuraft/ptr.hxx>
#include <libnuraft/req_msg.hxx>
#include <libnuraft/resp_msg.hxx>

#include "common/defs.h"

// Type alias for nuraft::ptr<nuraft::buffer> — RaftBufferPtr is defined in repl_decls.h but pulling that in
// from here drags the whole replication public surface.  Forward-define it here in the same form for the
// decode_req_msg signature.
namespace homestore {
using RaftBufferPtr = nuraft::ptr< nuraft::buffer >;
} // namespace homestore

namespace homestore::replication {

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────
//                                          NuraftCodec
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────
//
// Per-transport wire encoding of nuraft::req_msg and nuraft::resp_msg.  nuraft itself does not provide
// req_msg::serialize / req_msg::deserialize (its built-in asio transport hand-rolls the format inline); our
// folly transport owns its own encoding here.
//
// Wire layout for a request payload (the bytes that follow our 36B WireFrame header):
//
//   offset  size   field
//   ------  ----   -----
//   0       1      marker (0x00 = request)
//   1       1      msg_type
//   2       4      src
//   6       4      dst
//   10      8      term
//   18      8      last_log_term
//   26      8      last_log_idx
//   34      8      commit_idx
//   42      4      log_entries_count
//   46      8      extra_flags                                                — 54B fixed head
//   54      ...    repeated `log_entries_count` times:                        — 17B per-entry header
//                       1      le.has_crc32 (0/1)
//                       4      le.crc32
//                       8      le.timestamp_us
//                       4      le.payload_size
//                  then a tightly-packed region of per-entry payloads (term + val_type ride inside the first
//                  9 bytes of each entry's bufs_[0], so payload_size already covers them).
//
// Wire layout for a response payload:
//
//   offset  size   field
//   ------  ----   -----
//   0       1      marker (0x01 = response)
//   1       1      msg_type
//   2       4      src
//   6       4      dst
//   10      8      term
//   18      8      next_idx
//   26      1      accepted (0/1)
//   27      4      result_code
//   31      8      next_batch_size_hint_in_bytes
//   39      4      ctx_size
//   43      N      ctx bytes
//
// Encoding strategy
//   encode_req_msg builds an IOBuf CHAIN: head IOBuf with the fixed fields + per-entry metadata (single
//   contiguous allocation), then one chain link per log_entry's payload buffer constructed via
//   folly::IOBuf::takeOwnership over the log_entry's nuraft::buffer.  No payload bytes are copied at this
//   layer — the deleter captures the nuraft::ptr<buffer> to keep the buffer alive until folly drops the IOBuf
//   (which happens after the MSG_ZEROCOPY completion notification).
//
// Decoding strategy
//   decode_req_msg / decode_resp_msg take a fully-assembled contiguous nuraft::buffer (the receive path
//   pulls payload_len bytes off the wire into one nuraft::buffer first, paying the one unavoidable
//   kernel→user copy) and parse it into a fresh nuraft::ptr<req_msg> / resp_msg.  The per-entry payload bytes
//   are aliased via buffer::take_ownership with a deleter capturing the source body, so the log_entry
//   constructions are zero-copy on the receive side too.
//
// Failure: returns nullptr on malformed input (caller closes the socket).

// Build the IOBuf chain for a request payload.
unique< folly::IOBuf > encode_req_msg(nuraft::req_msg const& req);

// Build the IOBuf for a response payload.  resp_msg payloads are small enough today that this is a single
// IOBuf, not a chain.
unique< folly::IOBuf > encode_resp_msg(nuraft::resp_msg const& resp);

// Parse a request payload from a fully-assembled body buffer.  `body` is taken as a shared_ptr so the
// per-entry payload regions can be aliased via buffer::take_ownership with deleters capturing `body` — that
// keeps the source bytes alive for every constructed log_entry's lifetime.  Returns nullptr on malformed
// input.
nuraft::ptr< nuraft::req_msg > decode_req_msg(RaftBufferPtr const& body);

// Parse a response payload from a fully-assembled contiguous buffer.  Returns nullptr on malformed input.
nuraft::ptr< nuraft::resp_msg > decode_resp_msg(nuraft::buffer& payload);

// Markers used in the wire layout.
constexpr uint8_t kMarkerRequest  = 0x00;
constexpr uint8_t kMarkerResponse = 0x01;

} // namespace homestore::replication
