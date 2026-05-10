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

#include "sisl/logging/logging.h"
#include "homestore/index/btree/btree_kv.h"
#include "homestore/index/btree/detail/btree_node.h"

namespace homestore {
#pragma pack(1)
struct btree_obj_record {
    uint16_t obj_offset_ : 15;
    uint16_t is_overflow_ : 1;

    uint16_t obj_offset() const { return obj_offset_; }
    void set_obj_offset(uint16_t off) { obj_offset_ = off; }
    bool is_overflow() const { return is_overflow_ != 0; }
    void set_overflow(bool v) { is_overflow_ = v ? 1 : 0; }
};

struct var_node_header {
    uint16_t tail_arena_offset_; // Tail side of the arena where new keys are inserted
    uint16_t available_space_;

    uint16_t tail_offset() const { return tail_arena_offset_; }
    uint16_t available_space() const { return available_space_; }

    void set_tail_offset(uint16_t offset) { tail_arena_offset_ = offset; }
    void add_available_space(uint32_t sz) { available_space_ += sz; }
    void sub_available_space(uint32_t sz) { available_space_ -= sz; }
    void reclaim_tail(uint16_t obj_size) { tail_arena_offset_ -= obj_size; }

    void init(uint16_t data_size) {
        tail_arena_offset_ = data_size;
        available_space_ = data_size - sizeof(var_node_header);
    }
};
#pragma pack()

// Internal format of variable node:
// [Persistent Header][var node header][Record][Record].. ...  ... [key][value][key][value]
//
template < typename K, typename V >
class VarSizeNode : public NodeCore {
public:
    VarSizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf, uint32_t node_size) :
            NodeCore(std::move(buf), id, is_leaf, node_size) {
        get_var_node_header()->init(this->node_data_size());
    }

    VarSizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id) : NodeCore(std::move(buf), id) {}

    int compare_nth_key(const BtreeKey& cmp_key, uint32_t idx) const override {
        return get_nth_key< K >(idx, false).compare(cmp_key);
    }

    virtual ~VarSizeNode() = default;

    /* Insert the key and value in provided index
     * Assumption: Node lock is already taken */
    BtreeStatus insert(uint32_t idx, const BtreeKey& key, const BtreeValue& val) override {
        LOGTRACEMOD(btree, "{}:{}", key.to_string(), val.to_string());
        auto sz = insert(idx, key.serialize(), val.serialize());
        if (sz != 0 && val.is_overflow_value()) {
            get_nth_record_mutable(idx)->set_overflow(true);
        }
#ifndef NDEBUG
        validate_sanity();
#endif
        return (sz == 0) ? BtreeStatus::node_full : BtreeStatus::success;
    }

#ifndef NDEBUG
    void validate_sanity() {
        uint32_t i{0};
        // validate if keys are in ascending order
        K prevKey;
        while (i < this->total_entries()) {
            K key = NodeCore::get_nth_key< K >(i, false);
            uint64_t kp = *(uint64_t*)key.serialize().bytes();
            if (i > 0 && prevKey.compare(key) > 0) {
                DEBUG_ASSERT(false, "Found non sorted entry: {} -> {}", kp, to_string());
            }
            prevKey = key;
            ++i;
        }
    }
#endif

    /* Update a value in a given index to the provided value. It will support change in size of the new value.
     * Assumption: Node lock is already taken, size check for the node to support new value is already done */
    BtreeStatus update(uint32_t idx, const BtreeValue& val) override {
        if (idx == this->total_entries()) {
            DEBUG_ASSERT_EQ(this->is_leaf(), false);
            this->set_edge_value(val);
            this->inc_gen();
            return BtreeStatus::success;
        }
        K key = NodeCore::get_nth_key< K >(idx, true);
        return update(idx, key, val);
    }

    BtreeStatus update(uint32_t idx, const BtreeKey& key) override {
        DEBUG_ASSERT_LT(idx, this->total_entries());
        V val;
        this->get_nth_value(idx, &val, true /* copy */);
        return update(idx, key, val);
    }

    BtreeStatus update(uint32_t idx, const BtreeKey& key, const BtreeValue& val) override {
        DEBUG_ASSERT_LE(idx, this->total_entries());
        LOGTRACEMOD(btree, "update {}:{}", key.to_string(), val.to_string());

        if (idx == this->total_entries()) {
            DEBUG_ASSERT_EQ(this->is_leaf(), false);
            this->set_edge_value(val);
            this->inc_gen();
            return BtreeStatus::success;
        }

        // Serialize first to get actual sizes — serialized_size() is an estimate.
        sisl::Blob kblob = key.serialize();
        sisl::Blob vblob = val.serialize();
        uint16_t new_obj_size = kblob.size() + vblob.size();
        uint16_t cur_obj_size = get_nth_obj_size(idx);

        if (cur_obj_size >= new_obj_size) {
            uint8_t* key_ptr = (uint8_t*)get_nth_obj(idx);
            uint8_t* val_ptr = key_ptr + kblob.size();
            if (key_ptr != kblob.cbytes()) {
                std::memcpy(key_ptr, kblob.cbytes(), kblob.size());
            }
            if (val_ptr != vblob.cbytes()) {
                std::memcpy(val_ptr, vblob.cbytes(), vblob.size());
            }
            auto* rec = get_nth_record_mutable(idx);
            set_nth_key_len(rec, kblob.size());
            set_nth_value_len(rec, vblob.size());
            rec->set_overflow(val.is_overflow_value());
            get_var_node_header()->add_available_space(cur_obj_size - new_obj_size);
            this->inc_gen();
        } else {
            if (available_size() < uint32_t(new_obj_size - cur_obj_size)) {
                return BtreeStatus::node_full;
            }
            remove(idx, idx);
            auto sz = insert(idx, kblob, vblob);
            DEBUG_ASSERT_GT(sz, 0u, "insert must succeed after space pre-check");
        }
        return BtreeStatus::success;
    }

    // ind_s and ind_e are inclusive
    void remove(uint32_t ind_s, uint32_t ind_e) override {
        uint32_t total_entries = this->total_entries();
        DEBUG_ASSERT_GE(total_entries, ind_s);
        DEBUG_ASSERT_GE(total_entries, ind_e);
        uint32_t rec_size = this->get_record_size();
        uint32_t no_of_elem = ind_e - ind_s + 1;
        if (ind_e == this->total_entries()) {
            DEBUG_ASSERT(!this->is_leaf() && this->has_valid_edge(), "Expected interior node with valid edge");

            V last_1_val;
            get_nth_value(ind_s - 1, &last_1_val, false);
            this->set_edge_value(last_1_val);

            for (uint32_t i = ind_s - 1; i < total_entries; i++) {
                get_var_node_header()->add_available_space(get_nth_key_size(i) + get_nth_value_size(i) + rec_size);
            }
            this->sub_entries(total_entries - ind_s + 1);
        } else {
            // claim available memory
            for (uint32_t i = ind_s; i <= ind_e; i++) {
                get_var_node_header()->add_available_space(get_nth_key_size(i) + get_nth_value_size(i) + rec_size);
            }
            auto* rec_ptr = to_u8ptr(get_nth_record_mutable(ind_s));
            memmove(rec_ptr, rec_ptr + rec_size * no_of_elem, (this->total_entries() - ind_e - 1) * rec_size);

            this->sub_entries(no_of_elem);
        }
        this->inc_gen();
    }

    void remove_all() override {
        this->sub_entries(this->total_entries());
        this->invalidate_edge();
        this->inc_gen();
        get_var_node_header()->init(this->node_data_size());
#ifndef NDEBUG
        validate_sanity();
#endif
    }

    /*V get(uint32_t idx, bool copy) const {
        // Need edge index
        if (idx == this->total_entries()) {
            DEBUG_ASSERT_EQ(this->is_leaf(), false);
            DEBUG_ASSERT(this->has_valid_edge(), "Expected valid edge");
            return this->get_edge_value();
        } else {
            return get_nth_value(idx, copy);
        }
    }*/

    uint32_t move_out_to_right_by_entries(NodeCore& o, uint32_t nentries) override {
        auto& other = static_cast< VarSizeNode& >(o);
        const auto this_gen = this->node_gen();
        const auto other_gen = other.node_gen();

        const auto this_nentries = this->total_entries();
        nentries = std::min(nentries, this_nentries);
        if (nentries == 0) {
            return 0; /* Nothing to move */
        }

        const uint32_t start_idx = this_nentries - 1;
        const uint32_t end_idx = this_nentries - nentries;
        uint32_t idx = start_idx;
        bool full_move{false};
        while (idx >= end_idx) {
            // Get the ith key and value blob and then remove the entry from here and insert to the other node
            sisl::Blob const kb{get_nth_obj(idx), get_nth_key_size(idx)};
            sisl::Blob const vb{kb.cbytes() + kb.size(), get_nth_value_size(idx)};

            auto sz = other.insert(0, kb, vb);
            if (!sz) {
                break;
            }
            if (idx == 0) {
                full_move = true;
                break;
            }
            --idx;
        }

        if (!this->is_leaf() && (other.total_entries() != 0)) {
            // Incase this node is an edge node, move the stick to the right hand side node
            other.set_edge_id(this->edge_id());
            this->invalidate_edge();
        }
        remove(full_move ? 0u : idx + 1, start_idx); // Remove all entries in bulk

        // Remove and insert would have set the gen multiple increments, just reset it to increment only by 1
        // TODO: This is bit ugly but needed in-order to avoid repeat the same code again, but see if we can produce
        // interface around it.
        this->set_gen(this_gen + 1);
        other.set_gen(other_gen + 1);

        return (start_idx - idx);
    }

    uint32_t move_out_to_right_by_size(NodeCore& o, uint32_t size_to_move) override {
        auto& other = static_cast< VarSizeNode& >(o);
        auto this_gen = this->node_gen();
        auto other_gen = other.node_gen();
        uint32_t nmoved{0};

        uint32_t idx = this->total_entries() - 1;
        while (idx > 0) {
            sisl::Blob const kb{get_nth_obj(idx), get_nth_key_size(idx)};
            sisl::Blob const vb{kb.cbytes() + kb.size(), get_nth_value_size(idx)};

            if ((kb.size() + vb.size() + this->get_record_size()) > size_to_move) {
                // We reached threshold of how much we could move
                break;
            }

            auto sz = other.insert(0, kb, vb);
            if (!sz) {
                break;
            }

            --idx;
            ++nmoved;
            size_to_move -= sz;
        }
        remove(idx + 1, this->total_entries() - 1);

        if (!this->is_leaf() && (other.total_entries() != 0)) {
            // Incase this node is an edge node, move the stick to the right hand side node
            other.set_edge_id(this->edge_id());
            this->invalidate_edge();
        }

        // Remove and insert would have set the gen multiple increments, just reset it to increment only by 1
        // TODO: This is bit ugly but needed in-order to avoid repeat the same code again, but see if we can produce
        // interface around it.
        this->set_gen(this_gen + 1);
        other.set_gen(other_gen + 1);

        return nmoved;
    }

    uint32_t get_entries_size(uint32_t start_idx, uint32_t end_idx) const override {
        if ((start_idx == 0) && (end_idx == this->total_entries())) {
            return (this->occupied_size() - sizeof(var_node_header));
        }

        uint32_t cum_size{0};
        for (uint32_t i = start_idx; i < end_idx; ++i) {
            cum_size += get_nth_key_size(i) + get_nth_value_size(i) + this->get_record_size();
        }
        return cum_size;
    }

    bool append_copy_in_upto_size(const NodeCore& o, uint32_t& other_cursor, uint32_t upto_size) override {
        if (occupied_size() >= upto_size) {
            return false;
        }
        if (o.total_entries() == 0) {
            return true;
        }
        auto const room = upto_size - occupied_size();
        auto const ncopied = copy_by_size(o, other_cursor, room);
        other_cursor += ncopied;
        return true;
    }

    uint32_t copy_by_size(const NodeCore& o, uint32_t start_idx, uint32_t copy_size) {
        auto& other = static_cast< const VarSizeNode& >(o);
        auto this_gen = this->node_gen();

        auto idx = start_idx;
        uint32_t n = 0;
        while (idx < other.total_entries()) {
            sisl::Blob const kb{(uint8_t*)other.get_nth_obj(idx), other.get_nth_key_size(idx)};
            sisl::Blob const vb{kb.cbytes() + kb.size(), other.get_nth_value_size(idx)};

            // We reached threshold of how much we could move
            if ((kb.size() + vb.size() + other.get_record_size()) > copy_size) {
                break;
            }

            auto sz = insert(this->total_entries(), kb, vb);
            if (sz == 0) {
                break;
            }
            ++n;
            ++idx;
            copy_size -= sz;
        }
        this->set_gen(this_gen + 1);

        // If we copied everything from start_idx till end and if its an edge node, need to copy the edge id as well.
        if (other.has_valid_edge() && ((start_idx + n) == other.total_entries())) {
            this->set_edge_id(other.edge_id());
        }
        return n;
    }

#if 0
    uint32_t copy_by_entries(const NodeCore& o, uint32_t start_idx, uint32_t nentries) override {
        auto& other = static_cast< const VarSizeNode& >(o);
        auto this_gen = this->node_gen();

        nentries = std::min(nentries, other.total_entries() - start_idx);
        auto idx = start_idx;
        uint32_t n = 0;
        while (n < nentries) {
            sisl::Blob const kb{other.get_nth_obj(idx), other.get_nth_key_size(idx)};
            sisl::Blob const vb{kb.cbytes() + kb.size(), other.get_nth_value_size(idx)};

            auto sz = insert(this->total_entries(), kb, vb);
            if (sz == 0) { break; }
            ++n;
            ++idx;
        }
        this->set_gen(this_gen + 1);

        // If we copied everything from start_idx till end and if its an edge node, need to copy the edge id as well.
        if (other.has_valid_edge() && ((start_idx + n) == other.total_entries())) {
            this->set_edge_id(other.edge_id());
        }
        return n;
    }

        uint32_t num_entries_by_size(uint32_t start_idx, uint32_t size) const override {
        auto idx = start_idx;
        uint32_t cum_size{0};

        while (idx < this->total_entries()) {
            uint32_t const rec_size = this->get_record_size() + get_nth_key_size(idx) + get_nth_value_size(idx);
            cum_size += rec_size;
            if (cum_size > size) { break; }
            ++idx;
        }

        return idx - start_idx;
    }
#endif

    uint32_t available_size() const override {
        return get_var_node_header_const()->available_space();
    }

    void set_nth_key(uint32_t idx, const BtreeKey& key) {
        const auto kb = key.serialize();
        DEBUG_ASSERT_LT(idx, this->total_entries());
        DEBUG_ASSERT_EQ(kb.size(), get_nth_key_size(idx));
        memcpy(uintptr_cast(get_nth_obj(idx)), kb.cbytes(), kb.size());
    }

    bool has_room_for_put(BtreePutType put_type, uint32_t key_size, uint32_t value_size) const override {
        auto needed_size = key_size + value_size;
        if ((put_type == BtreePutType::UPSERT) || (put_type == BtreePutType::INSERT)) {
            needed_size += get_record_size();
        }
        return (available_size() >= needed_size);
    }

    virtual uint32_t get_record_size() const = 0;
    virtual void set_nth_key_len(btree_obj_record* rec, uint32_t key_len) = 0;
    virtual void set_nth_value_len(btree_obj_record* rec, uint32_t value_len) = 0;

    void read_nth_key(uint32_t idx, BtreeKey& out_key, bool copy) const override {
        DEBUG_ASSERT_LT(idx, this->total_entries());
        sisl::Blob b{const_cast< uint8_t* >(get_nth_obj(idx)), get_nth_key_size(idx)};
        out_key.deserialize(b, copy);
    }

    bool is_nth_value_overflow(uint32_t idx) const override {
        return get_nth_record(idx)->is_overflow();
    }

    void get_nth_value(uint32_t idx, BtreeValue* out_val, bool copy) const override {
        if (idx == this->total_entries()) {
            DEBUG_ASSERT_EQ(this->is_leaf(), false, "get_nth_value out-of-bound");
            DEBUG_ASSERT_EQ(this->has_valid_edge(), true, "get_nth_value out-of-bound");
            *static_cast< NodeLink* >(out_val) = this->get_edge_value();
        } else {
            sisl::Blob b{const_cast< uint8_t* >(get_nth_obj(idx)) + get_nth_key_size(idx), get_nth_value_size(idx)};
            auto const* rec = get_nth_record(idx);
            out_val->deserialize(b, copy, rec->is_overflow());
        }
    }

    std::string to_string(bool print_friendly = false) const override {
        auto str = fmt::format(
            "{}id={} level={} nEntries={} {} free_space={}{} ",
            (print_friendly ? "---------------------------------------------------------------------\n" : ""),
            this->node_id(), this->level(), this->total_entries(), (this->is_leaf() ? "LEAF" : "INTERIOR"),
            get_var_node_header_const()->available_space(),
            (this->next_node() == empty_bnodeid) ? "" : fmt::format(" next_node={}", this->next_node()));
        if (!this->is_leaf() && (this->has_valid_edge())) {
            fmt::format_to(std::back_inserter(str), "edge_id={}", this->edge_id());
        }
        for (uint32_t i{0}; i < this->total_entries(); ++i) {
            V val;
            get_nth_value(i, &val, false);
            fmt::format_to(std::back_inserter(str), "{}Entry{} [Key={} Val={}]", (print_friendly ? "\n\t" : " "), i + 1,
                           NodeCore::get_nth_key< K >(i, false).to_string(), val.to_string());
        }
        return str;
    }

    std::string to_dot_keys() const override {
        return "NOT Supported";
    }

    /*int compare_nth_key_range(const BtreeKeyRange& range, uint32_t idx) const {
        return get_nth_key(idx, false).compare_range(range);
    }*/

protected:
    uint32_t insert(uint32_t idx, const sisl::Blob& key_blob, const sisl::Blob& val_blob) {
        DEBUG_ASSERT_LE(idx, this->total_entries());
        LOGTRACEMOD(btree, "{}:{}:{}:{}", idx, get_var_node_header()->tail_offset(), get_arena_free_space(),
                    get_var_node_header()->available_space());
        uint16_t obj_size = key_blob.size() + val_blob.size();
        uint16_t to_insert_size = obj_size + this->get_record_size();
        if (to_insert_size > get_var_node_header()->available_space()) {
            return 0;
        }

        // If we don't have enough space in the tail arena area, we need to compact and get the space.
        if (to_insert_size > get_arena_free_space()) {
            compact();
            // Expect after compaction to have available space to insert
            DEBUG_ASSERT_LE(to_insert_size, get_arena_free_space(), "We should have space available after compaction");
        }

        // Create a room for a new record
        auto* rec = get_nth_record_mutable(idx);
        memmove(to_u8ptr(rec) + this->get_record_size(), rec, (this->total_entries() - idx) * this->get_record_size());

        // Move up the tail area
        auto* hdr = get_var_node_header();
        DEBUG_ASSERT_GT(hdr->tail_offset(), obj_size);
        hdr->reclaim_tail(obj_size);
        hdr->sub_available_space(obj_size + this->get_record_size());

        // Create a new record — clear overflow bit since this is a fresh insert (not an overflow value).
        set_nth_key_len(rec, key_blob.size());
        set_nth_value_len(rec, val_blob.size());
        set_record_data_offset(rec, hdr->tail_offset());
        rec->set_overflow(false);

        // Copy the contents of key and value in the offset
        uint8_t* raw_data_ptr = offset_to_ptr_mutable(hdr->tail_offset());
        memcpy(raw_data_ptr, key_blob.cbytes(), key_blob.size());
        raw_data_ptr += key_blob.size();
        memcpy(raw_data_ptr, val_blob.cbytes(), val_blob.size());

        // Increment the entries and generation number
        this->add_entries(1);
        this->inc_gen();

#ifndef NDEBUG
        this->validate_sanity();
#endif

        return to_insert_size;
    }

    /*
     * This method compacts and provides contiguous tail arena space
     * so that available space == tail arena space
     * */
    void compact() {
#ifndef NDEBUG
        this->validate_sanity();
#endif
        // temp ds to sort records in stack space
        struct Record {
            uint16_t obj_offset_;
            uint16_t orig_record_idx;
        };

        uint32_t no_of_entries = this->total_entries();
        if (no_of_entries == 0) {
            // this happens when  there is only entry and in update, we first remove and than insert
            get_var_node_header()->set_tail_offset(this->node_data_size());
            LOGTRACEMOD(btree, "Full available size reclaimed");
            return;
        }
        std::vector< Record > rec;
        rec.reserve(no_of_entries);

        uint32_t idx = 0;
        while (idx < no_of_entries) {
            auto* rec_ptr = get_nth_record_mutable(idx);
            rec[idx].obj_offset_ = rec_ptr->obj_offset();
            rec[idx].orig_record_idx = idx;
            idx++;
        }

        // use comparator to sort based on obj_offset_ in desc order
        std::sort(rec.begin(), rec.begin() + no_of_entries,
                  [](Record const& a, Record const& b) -> bool { return b.obj_offset_ < a.obj_offset_; });

        uint16_t last_offset = this->node_data_size();

        idx = 0;
        uint16_t sparse_space = 0;
        // loop records
        while (idx < no_of_entries) {
            uint16_t total_key_value_len =
                get_nth_key_size(rec[idx].orig_record_idx) + get_nth_value_size(rec[idx].orig_record_idx);
            sparse_space = last_offset - (rec[idx].obj_offset_ + total_key_value_len);
            if (sparse_space > 0) {
                // do compaction
                uint8_t* old_key_ptr = (uint8_t*)get_nth_obj(rec[idx].orig_record_idx);
                uint8_t* raw_data_ptr = old_key_ptr + sparse_space;
                memmove(raw_data_ptr, old_key_ptr, total_key_value_len);

                // update original record
                auto* orig_rec = get_nth_record_mutable(rec[idx].orig_record_idx);
                orig_rec->set_obj_offset(orig_rec->obj_offset() + sparse_space);

                last_offset = orig_rec->obj_offset();

            } else {
                DEBUG_ASSERT_EQ(sparse_space, 0);
                last_offset = rec[idx].obj_offset_;
            }
            idx++;
        }
        get_var_node_header()->set_tail_offset(last_offset);
#ifndef NDEBUG
        this->validate_sanity();
#endif
        LOGTRACEMOD(btree, "Sparse space reclaimed:{}", sparse_space);
    }

    btree_obj_record const* get_nth_record(uint32_t idx) const {
        return r_cast< btree_obj_record const* >(this->node_data_area_const() + sizeof(var_node_header) +
                                                 (idx * this->get_record_size()));
    }
    btree_obj_record* get_nth_record_mutable(uint32_t idx) {
        return r_cast< btree_obj_record* >(this->node_data_area() + sizeof(var_node_header) +
                                           (idx * this->get_record_size()));
    }

    const uint8_t* get_nth_obj(uint32_t idx) const {
        return offset_to_ptr(get_nth_record(idx)->obj_offset());
    }
    uint8_t* get_nth_obj_mutable(uint32_t idx) {
        return offset_to_ptr_mutable(get_nth_record_mutable(idx)->obj_offset());
    }

    void set_record_data_offset(btree_obj_record* rec, uint16_t offset) {
        rec->set_obj_offset(offset);
    }

    uint8_t* offset_to_ptr_mutable(uint16_t offset) {
        return this->node_data_area() + offset;
    }

    const uint8_t* offset_to_ptr(uint16_t offset) const {
        return this->node_data_area_const() + offset;
    }

    ///////////// Other Private Methods //////////////////
    inline var_node_header* get_var_node_header() {
        return r_cast< var_node_header* >(this->node_data_area());
    }

    inline const var_node_header* get_var_node_header_const() const {
        return r_cast< const var_node_header* >(this->node_data_area_const());
    }

    uint16_t get_arena_free_space() const {
        return get_var_node_header_const()->tail_offset() - sizeof(var_node_header) -
            (this->total_entries() * this->get_record_size());
    }
};

template < typename K, typename V >
class VarKeySizeNode : public VarSizeNode< K, V > {
public:
    VarKeySizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf, uint32_t node_size) :
            VarSizeNode< K, V >(std::move(buf), id, is_leaf, node_size) {
        this->set_node_type(BtreeNodeType::VAR_KEY);
    }

    VarKeySizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id) : VarSizeNode< K, V >(std::move(buf), id) {
        DEBUG_ASSERT_EQ(this->get_node_type(), BtreeNodeType::VAR_KEY);
    }

    virtual ~VarKeySizeNode() = default;

    uint32_t get_nth_key_size(uint32_t idx) const override {
        return static_cast< var_key_record const* >(this->get_nth_record(idx))->key_len();
    }
    uint32_t get_nth_value_size(uint32_t idx) const override { return dummy_value< V >.serialized_size(); }
    uint32_t get_record_size() const override { return sizeof(var_key_record); }

    void set_nth_key_len(btree_obj_record* rec, uint32_t key_len) override {
        static_cast< var_key_record* >(rec)->set_key_len(key_len);
    }
    void set_nth_value_len(btree_obj_record* rec, uint32_t value_len) override {
        DEBUG_ASSERT_EQ(value_len, dummy_value< V >.serialized_size());
    }

private:
#pragma pack(1)
    struct var_key_record : public btree_obj_record {
        uint16_t key_len_ : 15;
        uint16_t reserved : 1;

        uint16_t key_len() const { return key_len_; }
        void set_key_len(uint32_t len) { key_len_ = len; }
    };
#pragma pack()
};

/***************** Template Specialization for variable value records ******************/
template < typename K, typename V >
class VarValueSizeNode : public VarSizeNode< K, V > {
public:
    VarValueSizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf, uint32_t node_size) :
            VarSizeNode< K, V >(std::move(buf), id, is_leaf, node_size) {
        this->set_node_type(BtreeNodeType::VAR_VALUE);
    }

    VarValueSizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id) : VarSizeNode< K, V >(std::move(buf), id) {
        DEBUG_ASSERT_EQ(this->get_node_type(), BtreeNodeType::VAR_VALUE);
    }

    virtual ~VarValueSizeNode() = default;

    uint32_t get_nth_key_size(uint32_t idx) const override { return dummy_key< K >.serialized_size(); }
    uint32_t get_nth_value_size(uint32_t idx) const override {
        return static_cast< var_value_record const* >(this->get_nth_record(idx))->value_len();
    }
    uint32_t get_record_size() const override { return sizeof(var_value_record); }

    void set_nth_key_len(btree_obj_record* rec, uint32_t key_len) override {
        DEBUG_ASSERT_EQ(key_len, dummy_key< K >.serialized_size());
    }
    void set_nth_value_len(btree_obj_record* rec, uint32_t value_len) override {
        static_cast< var_value_record* >(rec)->set_value_len(value_len);
    }

private:
#pragma pack(1)
    struct var_value_record : public btree_obj_record {
        uint16_t value_len_ : 15;
        uint16_t reserved : 1;

        uint16_t value_len() const { return value_len_; }
        void set_value_len(uint32_t len) { value_len_ = len; }
    };
#pragma pack()
};

/***************** Template Specialization for variable object records ******************/
template < typename K, typename V >
class VarObjSizeNode : public VarSizeNode< K, V > {
public:
    VarObjSizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id, bool is_leaf, uint32_t node_size) :
            VarSizeNode< K, V >(std::move(buf), id, is_leaf, node_size) {
        this->set_node_type(BtreeNodeType::VAR_OBJECT);
    }

    VarObjSizeNode(std::shared_ptr< uint8_t > buf, bnodeid_t id) : VarSizeNode< K, V >(std::move(buf), id) {
        DEBUG_ASSERT_EQ(this->get_node_type(), BtreeNodeType::VAR_OBJECT);
    }

    virtual ~VarObjSizeNode() = default;

    uint32_t get_nth_key_size(uint32_t idx) const override {
        return static_cast< var_obj_record const* >(this->get_nth_record(idx))->key_len();
    }
    uint32_t get_nth_value_size(uint32_t idx) const override {
        return static_cast< var_obj_record const* >(this->get_nth_record(idx))->value_len();
    }
    uint32_t get_record_size() const override { return sizeof(var_obj_record); }

    void set_nth_key_len(btree_obj_record* rec, uint32_t key_len) override {
        static_cast< var_obj_record* >(rec)->set_key_len(key_len);
    }
    void set_nth_value_len(btree_obj_record* rec, uint32_t value_len) override {
        static_cast< var_obj_record* >(rec)->set_value_len(value_len);
    }

private:
#pragma pack(1)
    struct var_obj_record : public btree_obj_record {
        uint16_t key_len_ : 15;
        uint16_t reserved : 1;

        uint16_t value_len_ : 15;
        uint16_t reserved2 : 1;

        uint16_t key_len() const { return key_len_; }
        void set_key_len(uint32_t len) { key_len_ = len; }
        uint16_t value_len() const { return value_len_; }
        void set_value_len(uint32_t len) { value_len_ = len; }
    };
#pragma pack()
};
} // namespace homestore
