/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <folly/small_vector.h>

#include "common/defs.h"
#include "homestore/crc.h"
#include "sisl/fds/enum.h"
#include "sisl/fds/obj_life_counter.h"
#include "homestore/index/btree/btree_async.h"
#include "homestore/index/btree/detail/btree_internal.h"
#include "homestore/index/btree/btree_kv.h"

namespace homestore {
/// Node locking modes.
/// ReadInteriorWriteLeaf is the standard descend-for-mutation mode: interior nodes are read-locked,
/// leaf nodes are write-locked. This avoids holding exclusive locks high in the tree during descent.
ENUM(LockType, uint8_t, None, Read, Write, ReadInteriorWriteLeaf)

class NodeCore : public sisl::ObjLifeCounter< NodeCore > {
public:
    static constexpr uint8_t BTREE_NODE_VERSION = 1;
    static constexpr uint8_t BTREE_NODE_MAGIC = 0xab;

#pragma pack(1)
    struct PersistentHeader {
        uint8_t magic{BTREE_NODE_MAGIC};     // offset=0
        uint8_t version{BTREE_NODE_VERSION}; // offset=1
        uint16_t checksum{0};                // offset=2
        uint32_t nentries;                   // offset=4
        bnodeid_t node_id{empty_bnodeid};    // offset=8
        bnodeid_t next_node{empty_bnodeid};  // offset=16
        bnodeid_t edge_id{empty_bnodeid};    // offset=32: Edge entry information
        uint64_t node_gen{0};                // offset=24: Generation of this node, incremented on every update
        int64_t modified_cp_id{-1};          // offset=40: Checkpoint ID of the last modification of this node
        uint16_t level;                      // offset=48: Level of the node within the tree
        uint16_t node_size;                  // offset=50: Size of node, max 64K
        uint8_t node_type;                   // offset=52: Type of the node (simple vs varlen etc..)
        uint8_t leaf : 1;                    // offset=53: Is leaf or not, TODO: Cache this in wrapper to avoid bit ops
        uint8_t node_deleted : 1;            // offset=53: Is node deleted or not
        uint8_t reserved[2]{0, 0};           // offset=54-55: Reserved

        PersistentHeader() : nentries{0}, leaf{0}, node_deleted{0} {}
        std::string to_string() const {
            auto snext = (next_node == empty_bnodeid) ? "" : fmt::format(" next={}", next_node);
            auto sedge = (edge_id == empty_bnodeid) ? "" : fmt::format(" edge={}", edge_id);
            return fmt::format("magic={} version={} csum={} node_id={} nentries={} node_type={} is_leaf={} "
                               "deleted?={} gen={} modified_cp_id={}{}{} level={}",
                               magic, version, checksum, node_id, nentries, node_type, leaf, node_deleted,
                               node_gen, modified_cp_id, snext, sedge, level);
        }

        std::string to_compact_string() const {
            auto snext = (next_node == empty_bnodeid) ? "" : " next=" + std::to_string(next_node);
            auto sedge = (edge_id == empty_bnodeid) ? "" : fmt::format(" edge={}", edge_id);
            return fmt::format("id={}{}{} {} level={} nentries={}{} mod_cp={}", node_id, snext, sedge,
                               leaf ? "LEAF" : "INTERIOR", level, nentries, (node_deleted == 0x1) ? "  Deleted" : "",
                               modified_cp_id);
        }
    };
#pragma pack()

    /// Shared ownership of the persistent page buffer.
    ///
    /// The caller (storage backend) allocates the buffer and passes ownership in.
    /// Both backends use std::shared_ptr<uint8_t> but create it differently:
    ///   MemBtree  — std::shared_ptr<uint8_t>(new uint8_t[size], ...)
    ///   COWBtree  — sisl::AlignedSharedPtr<uint8_t>::make_sized(align, size)
    ///               (AlignedSharedPtr IS-A shared_ptr, so assignment works directly)
    ///
    /// Replacing the buffer (CoW on write) is a simple reassignment:
    ///   node->phys_node_buf_ = new_buf;
    /// The old buffer stays alive until all shared_ptr holders drop their copy.
    std::shared_ptr< uint8_t > phys_node_buf_;
    mutable BtreeSharedMutex lock_;

public:
    /// Construct a fresh node. The caller allocates and provides the buffer.
    NodeCore(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf, uint32_t node_size) :
            phys_node_buf_{std::move(buf)} {
        new (phys_node_buf_.get()) PersistentHeader{};
        set_node_id(id);
        set_leaf(is_leaf);
        set_node_size(node_size);
    }

    /// Construct from an existing buffer (e.g. after reading from disk/memory).
    NodeCore(std::shared_ptr< uint8_t > buf, bnodeid_t id) : phys_node_buf_{std::move(buf)} {
        DEBUG_ASSERT_EQ(node_id(), id);
        DEBUG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC);
        DEBUG_ASSERT_EQ(version(), BTREE_NODE_VERSION);
    }

    virtual ~NodeCore() = default; // shared_ptr deleter frees phys_node_buf_ automatically

    // Identify if a node is a leaf node or not, from raw buffer, by just reading PersistentHeader
    static bool identify_leaf_node(uint8_t* buf) {
        return (r_cast< PersistentHeader* >(buf))->leaf;
    }
    static std::string to_string_buf(uint8_t* buf) {
        return (r_cast< PersistentHeader* >(buf))->to_compact_string();
    }

    static bool is_valid_node(sisl::Blob const& buf) {
        auto phdr = r_cast< PersistentHeader const* >(buf.cbytes());
        if ((phdr->magic != BTREE_NODE_MAGIC) || (phdr->version != BTREE_NODE_VERSION)) {
            return false;
        }
        if ((uint32_cast(phdr->node_size) + 1) != buf.size()) {
            return false;
        }
        if (phdr->node_id == empty_bnodeid) {
            return false;
        }

        auto const exp_checksum = crc16_t10dif(bt_init_crc_16, (buf.cbytes() + sizeof(PersistentHeader)),
                                               buf.size() - sizeof(PersistentHeader));
        if (phdr->checksum != exp_checksum) {
            return false;
        }

        return true;
    }

    /// @brief Finds the index of the entry with the specified key in the node.
    ///
    /// This method performs a binary search on the node to find the index of the entry with the specified key.
    /// If the key is not found in the node, the method returns the index of the first entry greater than the key.
    ///
    /// @param key The key to search for.
    /// @param outval [optional] A pointer to a BtreeValue object to store the value associated with the key.
    /// @param copy_val If outval is non-null, is the value deserialized from node needs to be copy of the btree
    /// internal buffer. Safest option is to set this true, it is ok to set it false, if find() is called and value is
    /// accessed and used before subsequent node modification.
    /// @return A pair of values representing the result of the search.
    ///         The first value is a boolean indicating whether the key was found in the node.
    ///         The second value is an integer representing the index of the entry with the specified key or the index
    ///         of the first entry greater than the key.
    std::pair< bool, uint32_t > find(BtreeKey const& key, BtreeValue* outval, bool copy_val) const {
        LOGMSG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC, "Magic mismatch on btree_node {}",
                         get_persistent_header_const()->to_string());

        auto [found, idx] = bsearch_node(key);
        if (idx == total_entries()) {
            if (!has_valid_edge() || is_leaf()) {
                DEBUG_ASSERT_EQ(found, false);
                return std::make_pair(found, idx);
            }
            if (outval) {
                *((NodeLink*)outval) = get_edge_value();
            }
        } else {
            if (outval) {
                get_nth_value(idx, outval, copy_val);
            }
        }
        return std::make_pair(found, idx);
    }

    template < typename K >
    bool match_range(BtreeKeyRange< K > const& range, uint32_t& start_idx, uint32_t& end_idx) const {
        LOGMSG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC, "Magic mismatch on btree_node {}",
                         get_persistent_header_const()->to_string());

        bool sfound, efound;
        // Get the start index of the search range.
        std::tie(sfound, start_idx) = this->bsearch_node(range.start_key());
        if (sfound && !range.is_start_inclusive()) {
            ++start_idx;
            sfound = false;
        }

        if (start_idx == this->total_entries()) {
            // We are already at the end of search, we should return this as the only entry
            end_idx = start_idx;
            return (!is_leaf() && this->has_valid_edge()); // No result found unless its a edge node
        }

        // Get the end index of the search range.
        std::tie(efound, end_idx) = this->bsearch_node(range.end_key());
        if (is_leaf() || ((end_idx == this->total_entries()) && !has_valid_edge())) {
            // Binary search will always return the index as the first key that is >= given key (end_key in this
            // case). Our goal here in leaf node is to find the last key that is less than in case of non_inclusive
            // search or less than or equal in case of inclusive search.
            if (!efound || !range.is_end_inclusive()) {
                // If we are already on the first key, then obviously nothing has been matched.
                if (end_idx == 0) {
                    return false;
                }
                --end_idx;
            }

            // If we point to same start and end without any match, it is hitting unavailable range
            if (start_idx > end_idx) {
                return false;
            }
        }

        return true;
    }

    virtual BtreeStatus insert(const BtreeKey& key, const BtreeValue& val) {
        const auto [found, idx] = find(key, nullptr, false);
        DEBUG_ASSERT(!is_leaf() || (!found), "Invalid node"); // We do not support duplicate keys yet
        insert(idx, key, val);
        DEBUG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC, "{}", get_persistent_header_const()->to_string());
        return BtreeStatus::success;
    }

    virtual bool remove_one(const BtreeKey& key, BtreeKey* outkey, BtreeValue* outval) {
        const auto [found, idx] = find(key, outval, true);
        if (found) {
            if (outkey) {
                read_nth_key(idx, *outkey, true);
            }
            remove(idx);
            LOGMSG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC, "{}", get_persistent_header_const()->to_string());
        }
        return found;
    }

    template < typename K >
    bool remove_any(const BtreeKeyRange< K >& range, BtreeKey* outkey, BtreeValue* outval) {
        const auto [found, idx] = get_any(range, outkey, outval, true, true);
        if (found) {
            remove(idx);
            LOGMSG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC, "{}", get_persistent_header_const()->to_string());
        }
        return found;
    }

    /* Update the key and value pair and after update if outkey and outval are non-nullptr, it fills them with
     * the key and value it just updated respectively */
    virtual bool update_one(const BtreeKey& key, const BtreeValue& val, BtreeValue* outval) {
        const auto [found, idx] = find(key, outval, true);
        if (found) {
            update(idx, val);
            LOGMSG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC, "{}", get_persistent_header_const()->to_string());
        }
        return found;
    }

    virtual void overwrite(const NodeCore& other_node) {
        DEBUG_ASSERT_EQ(node_size(), other_node.node_size(), "{}", get_persistent_header_const()->to_string());
        std::memcpy(phys_node_buf_.get(), other_node.phys_node_buf_.get(), other_node.node_size());
    }

    template < typename K >
    K get_nth_key(uint32_t idx, bool copy) const {
        K k;
        read_nth_key(idx, k, copy);
        return k;
    }

    template < typename K >
    K get_last_key() const {
        if (total_entries() == 0) {
            return K{};
        }
        return get_nth_key< K >(total_entries() - 1, true);
    }

    template < typename K >
    K get_first_key() const {
        if (total_entries() == 0) {
            return K{};
        }
        return get_nth_key< K >(0, true);
    }

    template < typename K >
    bool validate_key_order() const {
        for (auto i = 1u; i < total_entries(); ++i) {
            auto prevKey = get_nth_key< K >(i - 1, false);
            auto curKey = get_nth_key< K >(i, false);
            if (prevKey.compare(curKey) >= 0) {
                DEBUG_ASSERT(false, "Order check failed at entry={}", i);
                return false;
            }
        }
        return true;
    }

    virtual NodeLink get_edge_value() const {
        return NodeLink{edge_id()};
    }

    virtual void set_edge_value(const BtreeValue& v) {
        // v is always a NodeLink for edge slots (edges store child node ids, not user values).
        set_edge_id(s_cast< NodeLink const& >(v).id());
    }

    void invalidate_edge() {
        set_edge_id(empty_bnodeid);
    }

    uint32_t total_entries() const {
        return get_persistent_header_const()->nentries;
    }

    void set_level(uint16_t l) {
        get_persistent_header()->level = l;
    }
    uint16_t level() const {
        return get_persistent_header_const()->level;
    }

    // uint32_t total_entries() const { return (has_valid_edge() ? total_entries() + 1 : total_entries()); }

    template < typename K, typename V >
    using ToStringCallback = std::function< std::string(std::vector< std::pair< K, V > > const&) >;

    template < typename K, typename V >
    std::string to_custom_string(ToStringCallback< K, V > const& cb) const {
        std::string snext = (this->next_node() == empty_bnodeid) ? "" : fmt::format(" next_node={}", this->next_node());
        auto str = fmt::format("id={} level={} nEntries={} {}{} node_gen={} ", this->node_id(), this->level(),
                               this->total_entries(), (this->is_leaf() ? "LEAF" : "INTERIOR"), snext, this->node_gen());
        if (this->has_valid_edge()) {
            fmt::format_to(std::back_inserter(str), " edge={}", this->edge_id());
        }

        if (this->total_entries() == 0) {
            fmt::format_to(std::back_inserter(str), " [EMPTY] ");
            return str;
        } else if (this->is_leaf()) {
            std::vector< std::pair< K, V > > entries;
            for (uint32_t i{0}; i < this->total_entries(); ++i) {
                V v;
                get_nth_value(i, &v, false);
                entries.emplace_back(std::make_pair(get_nth_key< K >(i, false), v));
            }
            fmt::format_to(std::back_inserter(str), " Keys=[{}]", cb(entries));
            return str;
        } else {
            fmt::format_to(std::back_inserter(str), " Keys=[");
            for (uint32_t i{0}; i < this->total_entries(); ++i) {
                fmt::format_to(std::back_inserter(str), "{}{}", get_nth_key< K >(i, false).to_string(),
                               (i == this->total_entries() - 1) ? "" : ", ");
            }
            fmt::format_to(std::back_inserter(str), "]");
        }

        // Should not happen
        if (this->is_node_deleted()) {
            fmt::format_to(std::back_inserter(str), " **DELETED** ");
        }

        return str;
    }

public:
    // Public method which needs to be implemented by variants
    virtual BtreeStatus insert(uint32_t ind, const BtreeKey& key, const BtreeValue& val) = 0;
    virtual void remove(uint32_t ind) {
        remove(ind, ind);
    }
    virtual void remove(uint32_t ind_s, uint32_t ind_e) = 0;
    virtual void remove_all() = 0;
    virtual BtreeStatus update(uint32_t ind, const BtreeValue& val) = 0;
    virtual BtreeStatus update(uint32_t ind, const BtreeKey& key) = 0;
    virtual BtreeStatus update(uint32_t ind, const BtreeKey& key, const BtreeValue& val) = 0;

    virtual uint32_t move_out_to_right_by_entries(NodeCore& other_node, uint32_t nentries) = 0;
    virtual uint32_t move_out_to_right_by_size(NodeCore& other_node, uint32_t size) = 0;

    /// @brief Appends entries copied from another SimpleNode into this node, up to a specified size limit.
    ///
    /// Copies entries starting from the `other_cursor` index in `other` node and appends them
    /// to the current node (`this`). Copying stops when either the source node runs out of entries
    /// starting from the cursor, or the occupied size of the current node reaches `upto_size`,
    /// or the current node runs out of available entry slots.
    ///
    /// @param o The source NodeCore (expected to be the same variant as this) to copy entries from.
    /// @param other_cursor [in, out] The starting index within `other` node to begin copying.
    ///                     This cursor is advanced by the number of entries successfully copied.
    /// @param upto_size The target maximum occupied size for the current node after appending.
    ///
    /// @return If any entries have been copied.
    /// @note Assumes appropriate node locks are held externally.
    virtual bool append_copy_in_upto_size(const NodeCore& other_node, uint32_t& other_cursor, uint32_t upto_size) = 0;

#if 0
    virtual uint32_t copy_by_size(const NodeCore& other_node, uint32_t start_idx, uint32_t size) = 0;
    virtual uint32_t copy_by_entries(const NodeCore& other_node, uint32_t start_idx, uint32_t nentries) = 0;
    virtual uint32_t num_entries_by_size(uint32_t start_idx, uint32_t size) const = 0;
#endif

    virtual uint32_t available_size() const = 0;
    virtual bool has_room_for_put(BtreePutType put_type, uint32_t key_size, uint32_t value_size) const = 0;
    virtual uint32_t get_entries_size(uint32_t start_idx, uint32_t end_idx) const = 0;

    virtual int compare_nth_key(const BtreeKey& cmp_key, uint32_t ind) const = 0;
    virtual void read_nth_key(uint32_t ind, BtreeKey& out_key, bool copykey) const = 0;
    virtual uint32_t get_nth_key_size(uint32_t ind) const = 0;
    virtual void get_nth_value(uint32_t ind, BtreeValue* out_val, bool copy) const = 0;
    virtual uint32_t get_nth_value_size(uint32_t ind) const = 0;
    virtual bool is_nth_value_overflow(uint32_t /*ind*/) const {
        return false;
    }
    virtual uint32_t get_nth_obj_size(uint32_t ind) const {
        return get_nth_key_size(ind) + get_nth_value_size(ind);
    }

    virtual std::string to_string(bool print_friendly = false) const = 0;
    virtual std::string to_basic_string() const {
        return fmt::format("{}-{}", level(), node_id());
    }

    virtual std::string to_dot_keys() const = 0;

protected:
    std::pair< bool, uint32_t > bsearch_node(const BtreeKey& key) const {
        DEBUG_ASSERT_EQ(magic(), BTREE_NODE_MAGIC);
        auto [found, idx] = bsearch(-1, total_entries(), key);
        if (found) {
            DEBUG_ASSERT_LT(idx, total_entries());
        }

        return std::make_pair(found, idx);
    }

    std::pair< bool, uint32_t > bsearch(int start, int end, const BtreeKey& key) const {
        int mid = 0;
        bool found{false};
        uint32_t end_of_search_index{0};

        if ((end - start) <= 1) {
            return std::make_pair(found, end_of_search_index);
        }
        while ((end - start) > 1) {
            mid = start + (end - start) / 2;
            DEBUG_ASSERT(mid >= 0 && mid < int_cast(total_entries()), "Invalid mid={}", mid);
            int x = compare_nth_key(key, mid);
            if (x == 0) {
                found = true;
                end = mid;
                break;
            } else if (x > 0) {
                end = mid;
            } else {
                start = mid;
            }
        }

        return std::make_pair(found, end);
    }

public:
    /// Returns a shared_ptr copy — caller holds shared ownership until done.
    /// CoW backends can replace phys_node_buf_ underneath; old buffer lives until all copies are dropped.
    std::shared_ptr< uint8_t > share_phys_node_buf() const {
        return phys_node_buf_;
    }

    PersistentHeader* get_persistent_header() {
        return r_cast< PersistentHeader* >(phys_node_buf_.get());
    }
    const PersistentHeader* get_persistent_header_const() const {
        return r_cast< const PersistentHeader* >(phys_node_buf_.get());
    }
    uint8_t* node_data_area() {
        return (phys_node_buf_.get() + sizeof(PersistentHeader));
    }
    const uint8_t* node_data_area_const() const {
        return (phys_node_buf_.get() + sizeof(PersistentHeader));
    }

    uint8_t magic() const {
        return get_persistent_header_const()->magic;
    }
    void set_magic() {
        get_persistent_header()->magic = BTREE_NODE_MAGIC;
    }

    uint8_t version() const {
        return get_persistent_header_const()->version;
    }
    uint16_t checksum() const {
        return get_persistent_header_const()->checksum;
    }
    void init_checksum() {
        get_persistent_header()->checksum = 0;
    }

    void set_node_id(bnodeid_t id) {
        get_persistent_header()->node_id = id;
    }
    bnodeid_t node_id() const {
        return get_persistent_header_const()->node_id;
    }
    int64_t get_modified_cp_id() const {
        return get_persistent_header_const()->modified_cp_id;
    }

    void set_checksum() {
        get_persistent_header()->checksum = crc16_t10dif(bt_init_crc_16, node_data_area_const(), node_data_size());
    }

    bool verify_node() const {
        auto exp_checksum = crc16_t10dif(bt_init_crc_16, node_data_area_const(), node_data_size());
        return ((magic() == BTREE_NODE_MAGIC) && (checksum() == exp_checksum));
    }

    bool is_leaf() const {
        return get_persistent_header_const()->leaf;
    }
    BtreeNodeType get_node_type() const {
        return s_cast< BtreeNodeType >(get_persistent_header_const()->node_type);
    }

    void set_total_entries(uint32_t n) {
        get_persistent_header()->nentries = n;
    }
    void add_entries(uint32_t addn = 1u) {
        get_persistent_header()->nentries += addn;
    }
    void sub_entries(uint32_t subn = 1u) {
        get_persistent_header()->nentries -= subn;
    }

    void set_leaf(bool leaf) {
        get_persistent_header()->leaf = leaf;
    }
    void set_node_type(BtreeNodeType t) {
        get_persistent_header()->node_type = uint32_cast(t);
    }
    void set_node_size(uint32_t size) {
        get_persistent_header()->node_size = s_cast< uint16_t >(size - 1);
    }
    uint64_t node_gen() const {
        return get_persistent_header_const()->node_gen;
    }
    uint32_t node_size() const {
        return s_cast< uint32_t >(get_persistent_header_const()->node_size) + 1;
    }
    uint32_t node_data_size() const {
        return node_size() - sizeof(PersistentHeader);
    }

    void inc_gen() {
        get_persistent_header()->node_gen++;
    }
    void set_gen(uint64_t g) {
        get_persistent_header()->node_gen = g;
    }

    void set_node_deleted() {
        get_persistent_header()->node_deleted = 0x1;
    }
    bool is_node_deleted() const {
        return (get_persistent_header_const()->node_deleted == 0x1);
    }

    NodeLink link_info() const {
        return NodeLink{node_id()};
    }

    virtual uint32_t occupied_size() const {
        return (node_data_size() - available_size());
    }
    bool is_merge_needed(const BtreeConfig& cfg) const {
        return (occupied_size() < cfg.suggested_min_size());
    }

    bnodeid_t next_node() const {
        return get_persistent_header_const()->next_node;
    }
    void set_next_node(bnodeid_t b) {
        get_persistent_header()->next_node = b;
    }

    bnodeid_t edge_id() const {
        return get_persistent_header_const()->edge_id;
    }
    void set_edge_id(bnodeid_t edge) {
        get_persistent_header()->edge_id = edge;
    }

    NodeLink edge_as_val() const {
        return NodeLink{edge_id()};
    }
    void set_edge(const NodeLink& id) {
        set_edge_id(id.id());
    }

    bool has_valid_edge() const {
        if (is_leaf()) {
            return false;
        }
        return (edge_id() != empty_bnodeid);
    }

    void set_modified_cp_id(int64_t id) {
        get_persistent_header()->modified_cp_id = id;
    }
};

///
/// NodeHandle — abstract lifetime token for a NodeCore.
/// Each storage backend (mem_btree, cow_btree, …) provides its own concrete
/// implementation.
///
/// move_to(dest): placement-moves *this into dest; after the call *this is
/// in a moved-from state and its destructor must be a no-op.  This lets
/// Node store the handle inline (SBO) with zero heap allocation.
///
/// Implementations must static_assert(sizeof(Impl) <= Node::kStorageBytes).
///
class NodeHandle {
public:
    virtual ~NodeHandle() = default;
    virtual NodeCore* get() = 0;
    virtual bool valid() const = 0;
    virtual void move_to(void* dest) noexcept = 0;
};

///
/// Node — RAII lock guard with inline (SBO) NodeHandle storage.
///
/// kStorageBytes is public so each NodeHandle implementation can
/// static_assert it fits, catching oversized handles at compile time.
///
/// Destructor sequence (lock_type_ != NONE):
///   1. ~Node() body  — NodeCore::unlock()             [releases mutex]
///   2. handle_->~NodeHandle() in body                 [releases backend reference]
/// When lock_type_ == NONE (node loaded but not yet locked) only step 2 fires.
///
class Node {
public:
    static constexpr size_t kStorageBytes = 3 * sizeof(void*);

    /// Resolve to the actual Read/Write lock type for a node of the given type.
    static LockType resolve_node_lock_type(LockType lt, bool is_leaf) {
        return (lt == LockType::ReadInteriorWriteLeaf) ? (is_leaf ? LockType::Write : LockType::Read) : lt;
    }

    static Node construct(NodeHandle& src, LockType lt) {
        Node n;
        src.move_to(n.storage_);
        n.handle_ = r_cast< NodeHandle* >(n.storage_);
        n.lock_type_ = resolve_node_lock_type(lt, n.handle_->get()->is_leaf());
        if (n.lock_type_ == LockType::Read) {
            n.handle_->get()->lock_.lock_shared();
        } else if (n.lock_type_ == LockType::Write) {
            n.handle_->get()->lock_.lock();
        }
        return n;
    }

    static BtreeTask< Node > async_construct(NodeHandle& src, LockType lt) {
        Node n;
        src.move_to(n.storage_);
        n.handle_ = r_cast< NodeHandle* >(n.storage_);
        n.lock_type_ = resolve_node_lock_type(lt, n.handle_->get()->is_leaf());
        if (n.lock_type_ == LockType::Read) {
            CO_AWAIT lock_shared_async(n.handle_->get()->lock_);
        } else if (n.lock_type_ == LockType::Write) {
            CO_AWAIT lock_async(n.handle_->get()->lock_);
        }
        CO_RETURN n;
    }

    Node(Node&& o) noexcept : lock_type_{o.lock_type_} {
        if (o.handle_) {
            o.handle_->move_to(storage_);
            handle_ = r_cast< NodeHandle* >(storage_);
            o.handle_ = nullptr;
            o.lock_type_ = LockType::None;
        }
    }
    Node& operator=(Node&& o) noexcept {
        if (this != &o) {
            this->~Node();
            new (this) Node(std::move(o));
        }
        return *this;
    }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    ~Node() {
        if (handle_) {
            if (lock_type_ == LockType::Read) {
                handle_->get()->lock_.unlock_shared();
            } else if (lock_type_ == LockType::Write) {
                handle_->get()->lock_.unlock();
            }
            handle_->~NodeHandle();
            handle_ = nullptr;
        }
    }

    // Acquire a lock on the node. Caller must have previously released or never locked.
    BtreeTask< void > acquire(LockType lt) {
        HS_DBG_ASSERT_EQ(lock_type_, LockType::None, "acquire called on already-locked node");
        lock_type_ = resolve_node_lock_type(lt, handle_->get()->is_leaf());
        if (lock_type_ == LockType::Read) {
            CO_AWAIT lock_shared_async(handle_->get()->lock_);
        } else if (lock_type_ == LockType::Write) {
            CO_AWAIT lock_async(handle_->get()->lock_);
        }
    }

    // Release the currently held lock without destroying the node handle.  After release(), lock_type() == None.
    void release() {
        if (!handle_) {
            return;
        }
        if (lock_type_ == LockType::Read) {
            handle_->get()->lock_.unlock_shared();
        } else if (lock_type_ == LockType::Write) {
            handle_->get()->lock_.unlock();
        }
        lock_type_ = LockType::None;
    }

    NodeCore* operator->() const { return handle_->get(); }
    NodeCore& operator*() const { return *handle_->get(); }
    bool valid() const { return handle_ && handle_->valid(); }
    LockType lock_type() const { return lock_type_; }

private:
    Node() = default;

    alignas(void*) std::byte storage_[kStorageBytes]{};
    NodeHandle* handle_{nullptr};
    LockType lock_type_{LockType::None};
};

using NodeList = folly::small_vector< Node, 3 >;

} // namespace homestore
