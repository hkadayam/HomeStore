#pragma once

#include <cstdint>

#include "common/defs.h"

namespace homedb {

// ── OpType ───────────────────────────────────────────────────────────────────────────────────────────────────────
// Per-entry operation discriminant.

enum class OpType : uint8_t {
    Put = 0,
    Remove = 1,
};

// ── TxnRecordHeader ──────────────────────────────────────────────────────────────────────────────────────────────
// Every journal record represents one transaction — a set of entries all made durable together and applied
// atomically at the target tables.  Layout on wire:
//
//   [ TxnRecordHeader | (TxnEntryHeader | key | value) × entry_count ]
//
// A single put/remove is a 1-entry transaction; multi-entry batches from a real Transaction API reuse the
// same shape.  Journal is byte-agnostic — the layout is owned entirely by Database (encoder) and its
// CommitSink implementation (decoder).

#pragma pack(1)
struct TxnRecordHeader {
    static constexpr uint8_t kVersion = 1;

    uint8_t version{kVersion};
    uint8_t _pad0{0};
    uint16_t entry_count{0};
    uint32_t reserved{0};
};
#pragma pack()
static_assert(sizeof(TxnRecordHeader) == 8, "TxnRecordHeader must remain a stable 8 bytes");

// ── TxnEntryHeader ───────────────────────────────────────────────────────────────────────────────────────────────
// Fixed 12-byte prefix for each entry inside a transaction record.  The key / value bytes follow
// concatenated; value_size == 0 when op_type == Remove.

#pragma pack(1)
struct TxnEntryHeader {
    uint8_t op_type{0}; // OpType
    uint8_t _pad0{0};
    uint16_t table_id{0};
    uint32_t key_size{0};
    uint32_t value_size{0};
};
#pragma pack()
static_assert(sizeof(TxnEntryHeader) == 12, "TxnEntryHeader must remain a stable 12 bytes");

} // namespace homedb
