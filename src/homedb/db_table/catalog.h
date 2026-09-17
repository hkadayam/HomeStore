#pragma once

#include <cstdint>
#include <string>

#include "common/defs.h"

#include "homedb/common/kv_spec.h"

namespace homedb {

// ── CatalogEntry ─────────────────────────────────────────────────────────────────────────────────────────────────
// Per-table metadata stashed inside each COWBtree's user_sb (via COWBtreeSuperBlock::user_sb_data()) at
// create time and read back on recovery.  Fixed-layout POD — the record is passed through as a raw
// `sisl::Blob` view; cow_btree_mgr memcpys it into its own metablk.  No serializer needed.
//
// Every entry duplicates `logstore_id` (all Tables in one HomeDB share the same Journal); recovery reads it
// from any surviving Table's SB to bootstrap the Journal before opening Tables.

#pragma pack(1)
struct CatalogEntry {
    static constexpr uint8_t kVersion = 1;
    static constexpr size_t kNameMax = 64;

    uint8_t version{kVersion};
    uint8_t _pad0[1]{};
    uint16_t table_id{0};
    uint32_t journal_id{0}; // backend-opaque; LocalJournal stores its LogStore id here
    char name[kNameMax]{};
    TableSpecOnDisk spec{};

    void set_name(std::string const& s);
    std::string get_name() const;
};
#pragma pack()

} // namespace homedb
