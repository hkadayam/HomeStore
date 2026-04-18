#pragma once

#include <array>
#include <map>
#include <memory>
#include <vector>

#include <sisl/cache/cache.h>
#include <homestore/blk.h>
#include "iomanager/drive_interface.hpp" // IOBuffer
#include <homestore/index/btree/btree_base.h>
#include <homestore/checkpoint/cp_mgr.h>

#include "sisl/fds/large_id_reserver.h"
#include "sisl/fds/concurrent_insert_vector.h"
#include "index/cow_btree/cow_btree_mgr.h"

namespace homestore {
class AppendBlkStream;
class AppendByteStream;
class RawBlkStream;
class BlobDev;

// ── OverflowEntry ────────────────────────────────────────────────────────────
// Value stored in the overflow cache — pairs the BlkId key with the raw data buffer.
struct OverflowEntry {
    BlkId blkid;
    sisl::ByteArray buf;
};

} // namespace homestore

namespace sisl {

template <>
struct CacheTraits< std::unique_ptr< homestore::NodeCore > > {
    static uint32_t size_of(std::unique_ptr< homestore::NodeCore > const& core) {
        return sizeof(homestore::NodeCore) + core->node_size();
    }
};

template <>
struct CacheTraits< homestore::OverflowEntry > {
    static uint32_t size_of(homestore::OverflowEntry const& entry) { return to_u32(entry.buf->size()); }
};

} // namespace sisl

namespace homestore {

// ── COWNodeHandle ─────────────────────────────────────────────────────────────
// NodeHandle implementation for COWBtree. Holds shared ownership of a NodeCore
// so nodes remain alive in the cache across multiple Node RAII wrappers.
// sizeof must fit in Node::kStorageBytes (= 3 * sizeof(void*)).
class COWNodeHandle final : public NodeHandle {
public:
    explicit COWNodeHandle(sisl::CacheHandle< unique< NodeCore > >&& h) noexcept : handle_{std::move(h)} {}

    NodeCore* get() override { return handle_.value().get(); }
    bool valid() const override { return bool(handle_); }
    void move_to(void* dest) noexcept override { new (dest) COWNodeHandle{std::move(handle_)}; }

private:
    sisl::CacheHandle< unique< NodeCore > > handle_;
};

// ── COWBtree ─────────────────────────────────────────────────────────────────
class COWBtree : public UnderlyingBtree {
public:
    // ── FlushNodeEntry ─────────────────────────────────────────────────────────
    // Holds a CacheHandle (pins the node in cache, preventing eviction while dirty) and a COW buffer snapshot for
    // flushing.  The buf is a shared_ptr copy from NodeCore::phys_node_buf_ taken at prepare_for_write time — if a
    // concurrent flush already holds the old buffer, a fresh aligned copy is made (COW).  Flush uses this buf, not
    // the live node's buffer, since the node may receive further mutations after the snapshot.
    struct FlushNodeEntry {
        sisl::CacheHandle< unique< NodeCore > > cache_handle; // pins node in cache until flush completes
        sisl::ByteArray flush_buf;                            // zero-copy wrap of COW snapshot

        FlushNodeEntry() = default;
        FlushNodeEntry(sisl::CacheHandle< unique< NodeCore > >&& h, std::shared_ptr< uint8_t > b) :
                cache_handle{std::move(h)},
                flush_buf{sisl::make_byte_array(std::move(b), cache_handle.value()->node_size())} {}
        FlushNodeEntry(FlushNodeEntry const&) = delete;
        FlushNodeEntry& operator=(FlushNodeEntry const&) = delete;
        FlushNodeEntry(FlushNodeEntry&&) noexcept = default;
        FlushNodeEntry& operator=(FlushNodeEntry&&) noexcept = default;

        NodeCore const& node() const { return *cache_handle.value(); }
        bnodeid_t node_id() const { return node().node_id(); }
        uint32_t node_size() const { return node().node_size(); }
        uint8_t* bytes() const { return flush_buf->bytes(); }
    };

public:
    using NodeCache = sisl::Cache< bnodeid_t, unique< NodeCore > >;
    using OverflowCache = sisl::Cache< BlkId, OverflowEntry >;

    static folly::coro::Task< unique< COWBtree > > create(shared< BlobDev > blob_dev, MetaBlkWrapper&& mblk,
                                                          shared< NodeCache > node_cache,
                                                          shared< OverflowCache > overflow_cache);
    static folly::coro::Task< unique< COWBtree > > load(shared< BlobDev > blob_dev, MetaBlkWrapper&& mblk,
                                                        shared< NodeCache > node_cache,
                                                        shared< OverflowCache > overflow_cache);

    virtual ~COWBtree() = default;

    void bind_to(BtreeBase* base) override { base_btree_ = base; }
    COWBtreeSuperBlock const& super_blk() const;

private:
    COWBtree(shared< BlobDev > blob_dev, MetaBlkWrapper&& mblk, shared< NodeCache > node_cache,
             shared< OverflowCache > overflow_cache);
    COWBtreeSuperBlock& mutable_super_blk();

public:
    // ── UnderlyingBtree interface ─────────────────────────────
    Node create_node(bool is_leaf) override;
    BtreeResult< Node > read_node(bnodeid_t id, LockType lock_type) const override;
    void write_node(const Node& node) override;
    BtreeStatus prepare_for_write(const Node& node) override;
    void remove_node(const Node& node) override;
    void on_root_changed(const Node& root) override;
    uint64_t space_occupied() const override;

    OpGuard enter_op() override { return OpGuard::make(cp_mgr().cp_guard()); }

    // ── Overflow support ─────────────────────────────────────────────────────
    BtreeStatus write_overflow(const sisl::ByteArray& buf, BlkId& out_blkid) override;
    folly::coro::Task< BtreeStatus > read_overflow(const BlkId& blkid, sisl::ByteArray& out_buf) const override;
    BtreeStatus delete_overflow(const BlkId& blkid) override;

    // ── COWBtree-specific ─────────────────────────────────────────────────────
    bnodeid_t generate_node_id(bool is_overflow = false);
    void release_node_id(bnodeid_t id);
    void add_to_dirty_node_list(FlushNodeEntry entry, cp_id_t cp_id);
    void add_to_remove_node_list(bnodeid_t node_id);
    void add_to_dirty_overflow_list(sisl::CacheHandle< OverflowEntry >&& handle);
    void add_to_remove_overflow_list(const BlkId& blkid);
    void destroy();

    BlkId get_blkid_for_nodeid(bnodeid_t nodeid) const;
    uint32_t ordinal() const { return btree_ordinal_; }

    // ── CP hooks ─────────────────────────────────────────────────────────────
    folly::coro::Task< void > cp_flush(CP* cp);
    folly::coro::Task< bool > incr_cp_flush(CP* cp);
    folly::coro::Task< void > full_cp_flush(CP* cp);
    folly::coro::Task< void > recover();
    bool is_dirty(cp_id_t cp_id) const;

    static COWBtree* cast_to(BtreeBase& btree) { return r_cast< COWBtree* >(btree.underlying_btree()); }
    static COWBtree const* cast_to(BtreeBase const& btree) {
        return r_cast< COWBtree const* >(btree.underlying_btree());
    }
    static COWBtree* cast_to(Index* index) {
        return r_cast< COWBtree* >(s_cast< BtreeBase* >(index)->underlying_btree());
    }
    static COWBtree const* cast_to(Index const* index) {
        return r_cast< COWBtree const* >(s_cast< BtreeBase const* >(index)->underlying_btree());
    }

public:
    // ── Compact on-disk types ─────────────────────────────────────────────────
    using CompactNodeId = uint32_t;
    static constexpr CompactNodeId EmptyCompactNodeId = std::numeric_limits< CompactNodeId >::max();

#pragma pack(1)
    struct CompactBlkId {
        blk_num_t is_valid : 1;
        blk_num_t blk_num : 31;
        chunk_num_t chunk_num;

        CompactBlkId() : is_valid{false} {}
        CompactBlkId(BlkId const& b) : is_valid{true}, blk_num{b.blk_num()}, chunk_num{b.chunk_num()} {}
        CompactBlkId(BlkId const& b, uint16_t offset) :
                is_valid{true}, blk_num{b.blk_num() + offset}, chunk_num{b.chunk_num()} {}

        BlkId to_blkid() const { return is_valid ? BlkId{blk_num, 1u, chunk_num} : BlkId{}; }
        std::string to_string() const {
            return is_valid ? fmt::format("blknum={},chunk={}", blk_num, chunk_num) : "Invalid";
        }
        std::string to_compact_string() const { return is_valid ? fmt::format("{}:{}", blk_num, chunk_num) : "NA"; }
    };
#pragma pack()

    // Flat vector indexed by CompactNodeId.  entries_[nodeid].is_valid distinguishes live from empty slots.
    // Serialized format: BNodeIdMapHeader followed by raw entries bytes.

#pragma pack(1)
    struct BNodeIdMapHeader {
        static constexpr uint32_t MAGIC = 0xB0DE1D00;
        uint32_t magic{MAGIC};
        cp_id_t cp_id{-1};
        uint32_t live_count{0};
        uint32_t num_entries{0}; // total slots (including invalid)
    };
#pragma pack()

    struct BNodeIdMap {
        void update(CompactNodeId nodeid, CompactBlkId blkid) {
            std::unique_lock lg{mtx_};
            if (nodeid >= entries_.size()) {
                entries_.resize(nodeid + 1);
            }
            if (!entries_[nodeid].is_valid) {
                ++live_count_;
            }
            entries_[nodeid] = blkid;
        }

        void remove(CompactNodeId nodeid) {
            std::unique_lock lg{mtx_};
            if (nodeid < entries_.size() && entries_[nodeid].is_valid) {
                entries_[nodeid] = CompactBlkId{};
                --live_count_;
            }
        }

        BlkId lookup(CompactNodeId nodeid) const {
            std::shared_lock lg{mtx_};
            return (nodeid < entries_.size()) ? entries_[nodeid].to_blkid() : BlkId{};
        }

        size_t size() const {
            std::shared_lock lg{mtx_};
            return live_count_;
        }

        static size_t serialized_size(size_t num_entries) {
            return sizeof(BNodeIdMapHeader) + num_entries * sizeof(CompactBlkId);
        }

        // Serialize under shared lock: calls cb twice — first with the header blob, then with the raw entries blob.
        // The shared lock is held for both calls so the stream's memcpy completes while entries_ is stable.
        template < typename Cb >
        void serialize(cp_id_t cp_id, Cb&& cb) const {
            std::shared_lock lg{mtx_};
            BNodeIdMapHeader hdr{};
            hdr.cp_id = cp_id;
            hdr.live_count = to_u32(live_count_);
            hdr.num_entries = to_u32(entries_.size());
            cb(sisl::Blob{to_cu8ptr(&hdr), sizeof(hdr)});
            cb(sisl::Blob{to_cu8ptr(entries_.data()), to_u32(entries_.size() * sizeof(CompactBlkId))});
        }

        // Load from the full serialized stream content: BNodeIdMapHeader + raw entries.  No iteration needed.
        void load_from(sisl::Blob const& blob) {
            std::unique_lock lg{mtx_};
            HS_REL_ASSERT_GE(blob.size(), sizeof(BNodeIdMapHeader), "BNodeIdMap: blob too small for header");
            auto const& hdr = *r_cast< BNodeIdMapHeader const* >(blob.cbytes());
            HS_REL_ASSERT_EQ(hdr.magic, BNodeIdMapHeader::MAGIC, "BNodeIdMap: bad header magic");
            live_count_ = hdr.live_count;
            entries_.resize(hdr.num_entries);
            std::memcpy(entries_.data(), blob.cbytes() + sizeof(BNodeIdMapHeader),
                        hdr.num_entries * sizeof(CompactBlkId));
        }

        void clear() {
            std::unique_lock lg{mtx_};
            entries_.clear();
            live_count_ = 0;
            updates_since_last_flush_.store(0);
        }

        std::vector< CompactBlkId > entries_;
        size_t live_count_{0};
        mutable folly::SharedMutex mtx_;
        std::atomic< uint64_t > updates_since_last_flush_{0};
    };

    // ── Per-CP session ────────────────────────────────────────────────────────
    // Tracks dirty/deleted nodes for one CP epoch (double-buffered by cp_id % max_concurrent_cps).
    // Streams handle block allocation and persistence; CPSession is intentionally thin.
    struct CPSession {
        COWBtree& bt_;
        cp_id_t cp_id_{-1};
        sisl::ConcurrentInsertVector< FlushNodeEntry > modified_nodes_;
        sisl::ConcurrentInsertVector< CompactNodeId > deleted_nodes_;
        sisl::ConcurrentInsertVector< sisl::CacheHandle< OverflowEntry > > dirty_overflow_blks_;
        sisl::ConcurrentInsertVector< BlkId > deleted_overflow_blks_;
        std::atomic< bnodeid_t > new_root_id_{empty_bnodeid};

        explicit CPSession(COWBtree& bt) : bt_{bt} {}

        void finish() {
            modified_nodes_.clear();
            deleted_nodes_.clear();
            dirty_overflow_blks_.clear();
            deleted_overflow_blks_.clear();
            new_root_id_.store(empty_bnodeid);
        }
    };

    friend struct CPSession;

    template < typename OnNodeFlushed >
    friend folly::coro::Task< void > flush_dirty_nodes(COWBtree&, CP*, OnNodeFlushed&&);
    template < typename OnNodeDeleted >
    friend folly::coro::Task< void > flush_deleted_nodes(COWBtree&, CP*, OnNodeDeleted&&);
    friend folly::coro::Task< void > flush_overflow_nodes(COWBtree&, CP*);

private:
    BtreeBase* base_btree_{nullptr}; // set via bind_to() from Btree<K,V> constructor
    shared< BlobDev > blob_dev_;
    shared< NodeCache > node_cache_;
    shared< OverflowCache > overflow_cache_;
    BNodeIdMap bnodeid_map_;
    sisl::LargeIDReserver nodeid_generator_;
    MetaBlkWrapper
        mblk_; // owns the persisted COWBtreeSuperBlock + MetaClient ref; updated on root change, written at CP time

    uint32_t btree_ordinal_;
    uint64_t ordinal_shifted_;

    // Streams — looked up from blob_dev_ by stream ID during construction.
    shared< AppendBlkStream > node_stream_;
    shared< RawBlkStream > overflow_stream_;
    shared< AppendByteStream > incr_map_stream_;
    shared< AppendByteStream > full_map_streams_[2];
    std::atomic< uint8_t > active_full_map_{0};

    // ── Per-CP sessions ───────────────────────────────────────────────────────
    std::array< unique< CPSession >, CPManager::max_concurent_cps > cp_sessions_;

    iomgr::FiberManagerLib::mutex flush_mtx_;
    std::mutex id_mtx_;

private:
    CPSession* cp_session(cp_id_t cp_id);
    folly::coro::Task< void > recover_full_map(cp_id_t cur_cp_id);
    folly::coro::Task< uint64_t > recover_one_incr_cp(uint64_t offset, cp_id_t last_full_cp, cp_id_t cur_cp_id);
    folly::coro::Task< IOBuffer > read_from_incr_stream(uint64_t offset, size_t len);

    // ── Incremental map journal format (written to incr_map_stream_) ─────────
    //
    // Layout per CP flush:
    //   IncrMapHeader
    //   IncrMapNodeRecord × N    (one per contiguous blkid run; sum of n_nodes == num_updates)
    //   CompactNodeId × num_deletes
    //   IncrMapFooter
    //
    // Recovery: read header → walk records until actual_updates == num_updates →
    //           read num_deletes × CompactNodeId → validate footer.

#pragma pack(1)
    struct IncrMapHeader {
        static constexpr uint32_t HEADER_MAGIC = 0xBADC0FFE;
        uint32_t header_magic{HEADER_MAGIC};
        cp_id_t cp_id;
        uint32_t num_updates{0};
        uint32_t num_deletes{0};
        CompactNodeId new_root_nodeid{EmptyCompactNodeId};
    };

    // One contiguous run of nodes written to node_stream_.  base_blkid is the starting block; nodes[i] is at
    // base_blkid + i.  n_nodes is bounded by kMaxWriteUnitBlks.
    struct IncrMapNodeRecord {
        CompactBlkId base_blkid;
        uint16_t n_nodes{0};
        CompactNodeId nodes[]; // flexible array member

        uint32_t size() const { return size(n_nodes); }
        static uint32_t size(uint16_t n) { return sizeof(IncrMapNodeRecord) + n * sizeof(CompactNodeId); }
    };

    struct IncrMapFooter {
        static constexpr uint32_t FOOTER_MAGIC = 0xCAFEF00D;
        uint32_t footer_magic{FOOTER_MAGIC};
        uint32_t num_records{0}; // number of IncrMapNodeRecords written
        uint32_t checksum{0};    // CRC32 of everything from IncrMapHeader to before this footer
    };
#pragma pack()
};

} // namespace homestore
