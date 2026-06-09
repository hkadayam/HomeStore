#pragma once

#include <cstdint>

#include <folly/io/IOBuf.h>
#include <libnuraft/async.hxx>
#include <libnuraft/buffer.hxx>
#include <libnuraft/ptr.hxx>
#include <libnuraft/req_msg.hxx>
#include <libnuraft/resp_msg.hxx>

#include "common/defs.h"

namespace homestore::replication {

// ------------------------------------------------------------------------------------------------------------
//                                          NuraftCodec
// ------------------------------------------------------------------------------------------------------------
//
// Per-transport wire encoding of nuraft::req_msg and nuraft::resp_msg.  nuraft itself does not provide
// `req_msg::serialize` / `req_msg::deserialize` (the asio_service transport hand-rolls the format inline);
// our folly transport owns its own encoding here.
//
// Wire layout for a request payload (what sits AFTER our 36B WireFrame header):
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
//   46      8      extra_flags
//   54      ...    repeated `log_entries_count` times:
//                       8      le.term
//                       1      le.val_type
//                       1      le.has_crc32 (0/1)
//                       4      le.crc32
//                       8      le.timestamp_us
//                       4      le.payload_size
//                       N      payload_size bytes of payload
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
//   43      N      ctx bytes (resp_msg context, currently 0-length for our usage)
//
// Encoding strategy
// -----------------
// encode_req_msg() builds an IOBuf CHAIN:
//   * Head IOBuf: all the small fixed-size fields + per-log_entry metadata (single contiguous alloc).
//   * One chain link PER log_entry payload, constructed via folly::IOBuf::takeOwnership(...) over the
//     log_entry's nuraft::buffer.  No payload bytes are copied at this layer — the wire payload bytes are
//     the same bytes the nuraft::buffer holds, with a deleter capturing the nuraft::ptr<buffer> to keep
//     it alive until folly drops the IOBuf (which folly does after the MSG_ZEROCOPY completion notification).
//
// Decoding strategy
// -----------------
// decode_req_msg() / decode_resp_msg() take a fully-assembled contiguous nuraft::buffer (the receiver
// pulls payload_len bytes off the wire into a single nuraft::buffer first, paying the one unavoidable
// kernel→user copy) and parse it into a fresh nuraft::ptr<req_msg> (or resp_msg) with single-buffer
// log_entries.  No application-side copies on the decode path.
//
// Failure mode: returns nullptr on malformed input (caller closes the socket).
namespace nuraft_codec {

// Build the IOBuf chain for a request payload.  Walks req.log_entries() and zero-copy chains each
// log_entry's nuraft::buffer into the output chain via takeOwnership.  Returned chain is ready to be
// wrapped by WireFrame::build() and handed to writeChain.
unique< folly::IOBuf > encode_req_msg(nuraft::req_msg const& req);

// Build the IOBuf chain for a response payload.  resp_msg payloads are small (context bytes, ~0 today),
// so this currently produces a single IOBuf (no chain).  Future: if resp_msg grows zero-copy data, this
// can chain like encode_req_msg does.
unique< folly::IOBuf > encode_resp_msg(nuraft::resp_msg const& resp);

// Parse a request payload from a fully-assembled contiguous buffer.  Returns nullptr on malformed input.
// `payload` must contain exactly the bytes for this request (size matches WireFrame.payload_len).
nuraft::ptr< nuraft::req_msg > decode_req_msg(nuraft::buffer& payload);

// Parse a response payload from a fully-assembled contiguous buffer.  Returns nullptr on malformed input.
nuraft::ptr< nuraft::resp_msg > decode_resp_msg(nuraft::buffer& payload);

// Compute the fixed-size head portion of a request encoding (everything before the first log_entry
// payload IOBuf).  Used by encode_req_msg to size the head IOBuf in a single allocation.
size_t compute_req_head_size(nuraft::req_msg const& req);

// Markers used in the wire layout.
constexpr uint8_t kMarkerRequest = 0x00;
constexpr uint8_t kMarkerResponse = 0x01;

} // namespace nuraft_codec

} // namespace homestore::replication
