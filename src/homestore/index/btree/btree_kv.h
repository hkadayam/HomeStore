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
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "homestore/blk.h"
#include "homestore/base/homestore_assert.hpp"
#include "sisl/fds/buffer.h"
#include "homestore/index/btree/detail/btree_internal.h"

namespace homestore {

template < typename K >
static K dummy_key;

template < typename V >
static V dummy_value;

ENUM(MultiMatchOption, uint16_t,
     DO_NOT_CARE, // Select anything that matches
     LEFT_MOST,   // Select the left most one
     RIGHT_MOST,  // Select the right most one
     MID          // Select the middle one
)

ENUM(BtreePutType, uint16_t,
     INSERT, // Insert only if it doesn't exist
     UPDATE, // Update only if it exists
     UPSERT  // Update if exists, insert otherwise
)

// The base class, btree library expects its key to be derived from
class BtreeKey {
public:
    BtreeKey() = default;

    // Deleting copy constructor forces the derived class to define its own copy constructor
    // BtreeKey(const BtreeKey& other) = delete;
    // BtreeKey(const sisl::Blob& b) = delete;
    BtreeKey(BtreeKey const& other) = default;
    virtual ~BtreeKey() = default;

    virtual int compare(BtreeKey const& other) const = 0;

    virtual sisl::Blob serialize() const = 0;
    virtual uint32_t serialized_size() const = 0;
    virtual void deserialize(sisl::Blob const& b, bool copy) = 0;

    virtual std::string to_string() const = 0;
    virtual bool is_interval_key() const { return false; }
};

// An extension of BtreeKey where each key is part of an interval range. Keys are not neccessarily only needs to be
// integers, but it needs to be able to get next or prev key from a given key in the key range
class BtreeIntervalKey : public BtreeKey {
public:
    virtual void shift(int n) = 0;
    virtual int distance(BtreeKey const& from) const = 0;
    bool is_interval_key() const override { return true; }

    virtual sisl::Blob serialize_prefix() const = 0;
    virtual sisl::Blob serialize_suffix() const = 0;

    virtual uint32_t serialized_prefix_size() const = 0;
    virtual uint32_t serialized_suffix_size() const = 0;
    virtual void deserialize(sisl::Blob const& prefix, sisl::Blob const& suffix, bool copy) = 0;
};

template < typename K >
class BtreeTraversalState;

template < typename K >
class BtreeKeyRange {
public:
    K start_key_;
    K end_key_;
    bool start_incl_{true};
    bool end_incl_{true};
    MultiMatchOption multi_selector_{MultiMatchOption::DO_NOT_CARE};

    friend class BtreeTraversalState< K >;

public:
    BtreeKeyRange() = default;

    BtreeKeyRange(const K& start_key, bool start_incl, const K& end_key, bool end_incl = true,
                  MultiMatchOption option = MultiMatchOption::DO_NOT_CARE) :
            start_key_{std::move(start_key)},
            end_key_{std::move(end_key)},
            start_incl_{start_incl},
            end_incl_{end_incl},
            multi_selector_{option} {}

    BtreeKeyRange(const K& start_key, const K& end_key) : BtreeKeyRange(start_key, true, end_key, true) {}

    BtreeKeyRange(const BtreeKeyRange& other) = default;
    BtreeKeyRange(BtreeKeyRange&& other) = default;
    BtreeKeyRange& operator=(const BtreeKeyRange< K >& other) = default;
    BtreeKeyRange& operator=(BtreeKeyRange< K >&& other) = default;

    void set_multi_option(MultiMatchOption o) { multi_selector_ = o; }
    const K& start_key() const { return start_key_; }
    const K& end_key() const { return end_key_; }
    bool is_start_inclusive() const { return start_incl_; }
    bool is_end_inclusive() const { return end_incl_; }
    MultiMatchOption multi_option() const { return multi_selector_; }

    void set_start_key(K&& key, bool incl) {
        start_key_ = std::move(key);
        start_incl_ = incl;
    }

    void set_end_key(K&& key, bool incl) {
        end_key_ = std::move(key);
        end_incl_ = incl;
    }

    std::string to_string() const {
        return fmt::format("{}{}-{}{}", is_start_inclusive() ? '[' : '(', start_key().to_string(),
                           end_key().to_string(), is_end_inclusive() ? ']' : ')');
    }
};

class BtreeValue {
public:
    BtreeValue() = default;
    virtual ~BtreeValue() = default;

    virtual sisl::Blob serialize() const = 0;
    virtual uint32_t serialized_size() const = 0;
    virtual void deserialize(sisl::Blob const& b, bool copy) = 0;
    virtual bool is_overflow_value() const { return false; }

    // Overflow-aware deserialize. Default asserts on overflow — only ValueOrOverflow<V> handles it.
    virtual void deserialize(sisl::Blob const& b, bool copy, bool is_overflow) {
        HS_DBG_ASSERT(!is_overflow, "overflow deserialize called on non-overflow-capable value");
        deserialize(b, copy);
    }

    // Serialize into an owned aligned buffer for overflow I/O. Default copies serialize() into a ByteArray.
    virtual sisl::ByteArray serialize_to_byte_array() const {
        auto blob = serialize();
        auto ba = sisl::make_byte_array(blob.size(), 512, sisl::Buftag::btree_node);
        std::memcpy(ba->bytes(), blob.cbytes(), blob.size());
        return ba;
    }

    virtual std::string to_string() const { return ""; }
};

// ── ValueOrOverflow<V> ─────────────────────────────────────────────────────────
// Discriminated union: either an inline value V or an overflow BlkId reference.  Inherits BtreeValue so it can be
// passed through the existing node virtual interface (insert/update/get_nth_value all take BtreeValue&).
//
// Write: node calls serialize() — dispatches to V::serialize() or BlkId bytes.
// Read:  node calls deserialize(blob, copy, is_overflow) — dispatches based on the overflow bit.
template < typename V >
class ValueOrOverflow : public BtreeValue {
    std::variant< V, BlkId > data_;

public:
    ValueOrOverflow() = default;
    ValueOrOverflow(V val) : data_{std::move(val)} {}
    ValueOrOverflow(BlkId bid) : data_{std::move(bid)} {}

    bool is_overflow() const { return std::holds_alternative< BlkId >(data_); }
    V const& inline_value() const { return std::get< V >(data_); }
    V& inline_value() { return std::get< V >(data_); }
    BlkId const& blkid() const { return std::get< BlkId >(data_); }

    static ValueOrOverflow make_inline(V val) { return ValueOrOverflow{std::move(val)}; }
    static ValueOrOverflow make_overflow(BlkId bid) { return ValueOrOverflow{std::move(bid)}; }

    sisl::Blob serialize() const override {
        if (is_overflow()) {
            return sisl::Blob{to_cu8ptr(&std::get< BlkId >(data_)), sizeof(BlkId)};
        }
        return inline_value().serialize();
    }

    uint32_t serialized_size() const override {
        return is_overflow() ? sizeof(BlkId) : inline_value().serialized_size();
    }

    void deserialize(sisl::Blob const& b, bool copy) override { std::get< V >(data_).deserialize(b, copy); }

    void deserialize(sisl::Blob const& b, bool copy, bool is_overflow) override {
        if (is_overflow) {
            BlkId bid;
            std::memcpy(&bid, b.cbytes(), sizeof(BlkId));
            data_ = std::move(bid);
        } else {
            data_ = V{};
            std::get< V >(data_).deserialize(b, copy);
        }
    }

    bool is_overflow_value() const override { return is_overflow(); }
    std::string to_string() const override { return is_overflow() ? "overflow" : inline_value().to_string(); }
};

class BtreeIntervalValue : public BtreeValue {
public:
    virtual void shift(int n) = 0;

    virtual sisl::Blob serialize_prefix() const = 0;
    virtual sisl::Blob serialize_suffix() const = 0;

    virtual uint32_t serialized_prefix_size() const = 0;
    virtual uint32_t serialized_suffix_size() const = 0;
    virtual void deserialize(sisl::Blob const& prefix, sisl::Blob const& suffix, bool copy) = 0;
};

// This class holds the current state of the search. This is where intermediate search state are stored
// and it is mutated by the do_put and do_query methods. Expect the current_sub_range and cursor to keep
// getting updated on calls.
template < typename K >
class BtreeTraversalState {
protected:
    const BtreeKeyRange< K > input_range_;
    BtreeKeyRange< K > working_range_;

public:
    BtreeTraversalState(BtreeKeyRange< K >&& inp_range) :
            input_range_{std::move(inp_range)}, working_range_{input_range_} {}
    BtreeTraversalState(const BtreeTraversalState& other) = default;
    BtreeTraversalState(BtreeTraversalState&& other) = default;

    const BtreeKeyRange< K >& input_range() const { return input_range_; }
    const BtreeKeyRange< K >& working_range() const { return working_range_; }

    // Trim the end key of the working range to a child boundary before descending.
    void trim_working_range(K&& end_key, bool end_incl) { working_range_.set_end_key(std::move(end_key), end_incl); }

    // Shift working range start to current end_key (exclusive) and reset end to full input_range end.
    // Used after each leaf so the next sibling sees the right remaining range.
    void shift_working_range() {
        working_range_.set_start_key(std::move(working_range_.end_key_), false);
        working_range_.end_key_ = input_range_.end_key();
        working_range_.end_incl_ = input_range_.is_end_inclusive();
    }

    // Shift working range start to a specific key (e.g. last_failed_key from multi_put).
    void shift_working_range(K&& start_key, bool start_incl) {
        working_range_.set_start_key(std::move(start_key), start_incl);
        working_range_.end_key_ = input_range_.end_key();
        working_range_.end_incl_ = input_range_.is_end_inclusive();
    }

    const K& first_key() const { return working_range_.start_key(); }

    uint32_t first_key_size() const {
        if (is_start_inclusive() || K::is_fixed_size()) {
            return working_range_.start_key().serialized_size();
        } else {
            return K::get_max_size();
        }
    }

private:
    bool is_start_inclusive() const { return input_range_.is_start_inclusive(); }
    bool is_end_inclusive() const { return input_range_.is_end_inclusive(); }
};

class NodeLink : public BtreeValue {
    bnodeid_t id_{empty_bnodeid};

public:
    NodeLink() = default;
    explicit NodeLink(bnodeid_t id) : id_{id} {}

    bnodeid_t id() const { return id_; }
    void set_id(bnodeid_t id) { id_ = id; }
    bool is_valid() const { return id_ != empty_bnodeid; }

    sisl::Blob serialize() const override {
        sisl::Blob b;
        b.set_size(sizeof(bnodeid_t));
        b.set_bytes(r_cast< const uint8_t* >(&id_));
        return b;
    }
    uint32_t serialized_size() const override { return sizeof(bnodeid_t); }
    static uint32_t get_fixed_size() { return sizeof(bnodeid_t); }
    void deserialize(const sisl::Blob& b, bool copy) override {
        DEBUG_ASSERT_EQ(b.size(), sizeof(bnodeid_t), "NodeLink deserialize received invalid blob");
        id_ = *r_cast< bnodeid_t const* >(b.cbytes());
    }
    std::string to_string() const override { return fmt::format("{}", id_); }
};

} // namespace homestore
