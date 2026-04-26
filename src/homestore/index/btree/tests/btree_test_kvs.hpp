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

#include <string>
#include <random>
#include <map>
#include <memory>
#include <array>

#include "homestore/index/btree/btree_kv.h"
#include "homestore/index/btree/node_variant/simple_node.hpp"
#include "homestore/index/btree/node_variant/varlen_node.hpp"
// TODO: re-enable prefix_node when variant_node.hpp is ported.
// #include "homestore/index/btree/node_variant/prefix_node.hpp"

static constexpr uint32_t g_max_keysize{100}; // for  node size = 512 : free space : 442 => 100+100+6(record size) = 46%
static constexpr uint32_t g_max_valsize{100};
static std::random_device g_rd{};
static std::default_random_engine g_re{g_rd()};
static std::normal_distribution<> g_randkeysize_generator{32, 24};
// static std::uniform_int_distribution< uint32_t > g_randkeysize_generator{2, g_max_keysize};
static std::uniform_int_distribution< uint32_t > g_randval_generator{1, 30000};
static std::normal_distribution<> g_randvalsize_generator{32, 24};
// static std::uniform_int_distribution< uint32_t > g_randvalsize_generator{2, g_max_valsize};
static std::mutex g_map_lk;
static std::map< uint32_t, std::shared_ptr< std::string > > g_key_pool;

static constexpr std::array< const char, 62 > alphanum{
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
    'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};

static std::string gen_random_string(size_t len, uint32_t preamble = std::numeric_limits< uint32_t >::max()) {
    std::string str;
    if (preamble != std::numeric_limits< uint32_t >::max()) {
        std::stringstream ss;
        ss << std::setw(8) << std::setfill('0') << std::hex << preamble;
        str += ss.str();
    }

    std::uniform_int_distribution< size_t > rand_char{0, alphanum.size() - 1};
    if (len < str.size()) {
        len = str.size();
    }
    for (size_t i{0}; i < len - str.size(); ++i) {
        str += alphanum[rand_char(g_re)];
    }
    return str;
}
template < typename T >
static bool willAdditionOverflow(T a, int b) {
    static_assert(std::is_integral< T >::value, "Template parameter must be an integral type.");

    if (b > 0) {
        return a > std::numeric_limits< T >::max() - b;
    } else if (b < 0) {
        return a < std::numeric_limits< T >::min() - b;
    }
    return false;
}

using namespace homestore;

class TestFixedKey : public BtreeKey {
private:
    uint64_t key_{0};

public:
    TestFixedKey() = default;
    TestFixedKey(uint64_t k) : key_{k} {}
    TestFixedKey(const TestFixedKey& other) : TestFixedKey(other.serialize(), true) {}
    TestFixedKey(const BtreeKey& other) : TestFixedKey(other.serialize(), true) {}
    TestFixedKey(const sisl::Blob& b, bool copy) : BtreeKey(), key_{*(r_cast< const uint64_t* >(b.cbytes()))} {}
    TestFixedKey& operator=(const TestFixedKey& other) = default;
    TestFixedKey& operator=(BtreeKey const& other) {
        key_ = s_cast< TestFixedKey const& >(other).key_;
        return *this;
    }

    virtual ~TestFixedKey() = default;

    int compare(const BtreeKey& o) const override {
        const TestFixedKey& other = s_cast< const TestFixedKey& >(o);
        if (key_ < other.key_) {
            return -1;
        } else if (key_ > other.key_) {
            return 1;
        } else {
            return 0;
        }
    }

    /*int compare_range(const BtreeKeyRange& range) const override {
        if (key_ == start_key(range)) {
            return range.is_start_inclusive() ? 0 : -1;
        } else if (key_ < start_key(range)) {
            return -1;
        } else if (key_ == end_key(range)) {
            return range.is_end_inclusive() ? 0 : 1;
        } else if (key_ > end_key(range)) {
            return 1;
        } else {
            return 0;
        }
    }*/

    sisl::Blob serialize() const override {
        return sisl::Blob{uintptr_cast(const_cast< uint64_t* >(&key_)), uint32_cast(sizeof(uint64_t))};
    }
    uint32_t serialized_size() const override { return get_fixed_size(); }
    static bool is_fixed_size() { return true; }
    static uint32_t get_fixed_size() { return (sizeof(uint64_t)); }
    std::string to_string() const { return fmt::format("{}", key_); }

    void deserialize(const sisl::Blob& b, bool copy) override { key_ = *(r_cast< const uint64_t* >(b.cbytes())); }

    static uint32_t get_max_size() { return get_fixed_size(); }
    friend std::ostream& operator<<(std::ostream& os, const TestFixedKey& k) {
        os << k.to_string();
        return os;
    }

    friend std::istream& operator>>(std::istream& is, TestFixedKey& k) {
        uint64_t key;
        is >> key;
        k = TestFixedKey{key};
        return is;
    }

    bool operator<(const TestFixedKey& o) const { return (compare(o) < 0); }
    bool operator==(const TestFixedKey& other) const { return (compare(other) == 0); }

    uint64_t key() const { return key_; }
    uint64_t start_key(const BtreeKeyRange< TestFixedKey >& range) const {
        const TestFixedKey& k = (const TestFixedKey&)(range.start_key());
        return k.key_;
    }
    uint64_t end_key(const BtreeKeyRange< TestFixedKey >& range) const {
        const TestFixedKey& k = (const TestFixedKey&)(range.end_key());
        return k.key_;
    }
};

class TestVarLenKey : public BtreeKey {
private:
    uint64_t key_{0};

    static uint64_t rand_key_size() {
        return (uint64_cast(std::abs(std::round(g_randkeysize_generator(g_re)))) % g_max_keysize) + 1;
    }

    static std::shared_ptr< std::string > idx_to_key(uint32_t idx) {
        std::unique_lock< std::mutex > lk(g_map_lk);
        auto it = g_key_pool.find(idx);
        if (it == g_key_pool.end()) {
            const auto& [it, happened] =
                g_key_pool.emplace(idx, std::make_shared< std::string >(gen_random_string(rand_key_size(), idx)));
            assert(happened);
            return it->second;
        } else {
            return it->second;
        }
    }

public:
    TestVarLenKey() = default;
    TestVarLenKey(uint64_t k) : BtreeKey(), key_{k} {}
    TestVarLenKey(const BtreeKey& other) : TestVarLenKey(other.serialize(), true) {}
    TestVarLenKey(const TestVarLenKey& other) = default;
    TestVarLenKey(TestVarLenKey&& other) = default;
    TestVarLenKey& operator=(const TestVarLenKey& other) = default;
    TestVarLenKey& operator=(TestVarLenKey&& other) = default;

    TestVarLenKey(const sisl::Blob& b, bool copy) : BtreeKey() { deserialize(b, copy); }
    virtual ~TestVarLenKey() = default;

    sisl::Blob serialize() const override {
        const auto& data = idx_to_key(key_);
        return sisl::Blob{(uint8_t*)(data->c_str()), (uint32_t)data->size()};
    }

    uint32_t serialized_size() const override { return idx_to_key(key_)->size(); }
    static bool is_fixed_size() { return false; }
    static uint32_t get_fixed_size() {
        assert(0);
        return 0;
    }

    void deserialize(const sisl::Blob& b, bool copy) {
        std::string data{r_cast< const char* >(b.cbytes()), b.size()};
        std::stringstream ss;
        ss << std::hex << data.substr(0, 8);
        ss >> key_;
        assert(data == *idx_to_key(key_));
    }

    // Add 8 bytes for preamble.
    static uint32_t get_max_size() { return g_max_keysize + 8; }

    int compare(const BtreeKey& o) const override {
        const TestVarLenKey& other = s_cast< const TestVarLenKey& >(o);
        if (key_ < other.key_) {
            return -1;
        } else if (key_ > other.key_) {
            return 1;
        } else {
            return 0;
        }
    }

    /*    int compare_range(const BtreeKeyRange& range) const override {
            if (key_ == start_key(range)) {
                return range.is_start_inclusive() ? 0 : -1;
            } else if (key_ < start_key(range)) {
                return -1;
            } else if (key_ == end_key(range)) {
                return range.is_end_inclusive() ? 0 : 1;
            } else if (key_ > end_key(range)) {
                return 1;
            } else {
                return 0;
            }
        } */

    std::string to_string() const { return fmt::format("{}-{}", key_, idx_to_key(key_)->substr(0, 8)); }

    friend std::ostream& operator<<(std::ostream& os, const TestVarLenKey& k) {
        os << k.to_string();
        return os;
    }

    friend std::istream& operator>>(std::istream& is, TestVarLenKey& k) {
        uint64_t key;
        is >> key;
        k = TestVarLenKey{key};
        return is;
    }

    bool operator<(const TestVarLenKey& o) const { return (compare(o) < 0); }
    bool operator==(const TestVarLenKey& other) const { return (compare(other) == 0); }

    uint64_t key() const { return key_; }
    uint64_t start_key(const BtreeKeyRange< TestVarLenKey >& range) const {
        const TestVarLenKey& k = (const TestVarLenKey&)(range.start_key());
        return k.key_;
    }
    uint64_t end_key(const BtreeKeyRange< TestVarLenKey >& range) const {
        const TestVarLenKey& k = (const TestVarLenKey&)(range.end_key());
        return k.key_;
    }
};

class TestIntervalKey : public BtreeIntervalKey {
private:
#pragma pack(1)
    uint32_t base_{0};
    uint32_t offset_{0};
#pragma pack()

public:
    TestIntervalKey() = default;
    TestIntervalKey(uint64_t k) {
        base_ = uint32_cast(k >> 32);
        offset_ = uint32_cast(k & 0xFFFFFFFF);
    }
    TestIntervalKey(uint32_t b, uint32_t o) : base_{b}, offset_{o} {
    }
    TestIntervalKey(const TestIntervalKey& other) = default;
    TestIntervalKey(const BtreeKey& other) : TestIntervalKey(other.serialize(), true) {
    }
    TestIntervalKey(const sisl::Blob& b, bool copy) : BtreeIntervalKey() {
        TestIntervalKey const* other = r_cast< TestIntervalKey const* >(b.cbytes());
        base_ = other->base_;
        offset_ = other->offset_;
    }

    TestIntervalKey& operator=(TestIntervalKey const& other) {
        base_ = other.base_;
        offset_ = other.offset_;
        return *this;
    };
    virtual ~TestIntervalKey() = default;

    /////////////////// Overriding methods of BtreeKey /////////////////
    int compare(BtreeKey const& o) const override {
        TestIntervalKey const& other = s_cast< TestIntervalKey const& >(o);
        if (base_ < other.base_) {
            return -1;
        } else if (base_ > other.base_) {
            return 1;
        } else if (offset_ < other.offset_) {
            return -1;
        } else if (offset_ > other.offset_) {
            return 1;
        } else {
            return 0;
        }
    }

    sisl::Blob serialize() const override {
        return sisl::Blob{uintptr_cast(const_cast< TestIntervalKey* >(this)), uint32_cast(sizeof(TestIntervalKey))};
    }

    uint32_t serialized_size() const override {
        return sizeof(TestIntervalKey);
    }

    void deserialize(sisl::Blob const& b, bool copy) override {
        assert(b.size() == sizeof(TestIntervalKey));
        TestIntervalKey const* other = r_cast< TestIntervalKey const* >(b.cbytes());
        base_ = other->base_;
        offset_ = other->offset_;
    }

    std::string to_string() const override {
        return fmt::format("{}", key());
    }

    static uint32_t get_max_size() {
        return sizeof(TestIntervalKey);
    }

    static bool is_fixed_size() {
        return true;
    }

    static uint32_t get_fixed_size() {
        return sizeof(TestIntervalKey);
    }

    /////////////////// Overriding methods of BtreeIntervalKey /////////////////
    void shift(int n) override {
        offset_ += n;
    }

    int distance(BtreeKey const& f) const override {
        TestIntervalKey const& from = s_cast< TestIntervalKey const& >(f);
        uint64_t this_val = (uint64_cast(base_) << 32) | offset_;
        uint64_t from_val = (uint64_cast(from.base_) << 32) | from.offset_;
        DEBUG_ASSERT_GE(this_val, from_val, "Invalid from key for distance");
        return static_cast< int >(this_val - from_val);
    }

    bool is_interval_key() const override {
        return true;
    }

    sisl::Blob serialize_prefix() const override {
        return sisl::Blob{uintptr_cast(const_cast< uint32_t* >(&base_)), uint32_cast(sizeof(uint32_t))};
    }

    sisl::Blob serialize_suffix() const override {
        return sisl::Blob{uintptr_cast(const_cast< uint32_t* >(&offset_)), uint32_cast(sizeof(uint32_t))};
    }

    uint32_t serialized_prefix_size() const override {
        return uint32_cast(sizeof(uint32_t));
    }

    uint32_t serialized_suffix_size() const override {
        return uint32_cast(sizeof(uint32_t));
    };

    void deserialize(sisl::Blob const& prefix, sisl::Blob const& suffix, bool) {
        DEBUG_ASSERT_EQ(prefix.size(), sizeof(uint32_t), "Invalid prefix size on deserialize");
        DEBUG_ASSERT_EQ(suffix.size(), sizeof(uint32_t), "Invalid suffix size on deserialize");
        uint32_t const* other_p = r_cast< uint32_t const* >(prefix.cbytes());
        base_ = *other_p;

        uint32_t const* other_s = r_cast< uint32_t const* >(suffix.cbytes());
        offset_ = *other_s;
    }

    /////////////////// Local methods for helping tests //////////////////
    bool operator<(const TestIntervalKey& o) const {
        return (compare(o) < 0);
    }
    bool operator==(const TestIntervalKey& other) const {
        return (compare(other) == 0);
    }

    uint64_t key() const {
        return (uint64_cast(base_) << 32) | offset_;
    }
    uint64_t start_key(const BtreeKeyRange< TestIntervalKey >& range) const {
        const TestIntervalKey& k = (const TestIntervalKey&)(range.start_key());
        return k.key();
    }
    uint64_t end_key(const BtreeKeyRange< TestIntervalKey >& range) const {
        const TestIntervalKey& k = (const TestIntervalKey&)(range.end_key());
        return k.key();
    }
    friend std::ostream& operator<<(std::ostream& os, const TestIntervalKey& k) {
        os << k.to_string();
        return os;
    }

    friend std::istream& operator>>(std::istream& is, TestIntervalKey& k) {
        uint32_t base_;
        uint32_t offset_;
        char dummy;
        is >> base_ >> dummy >> offset_;
        k = TestIntervalKey{base_, offset_};
        return is;
    }
};

class TestFixedValue : public BtreeValue {
private:
public:
    TestFixedValue(bnodeid_t val) { assert(0); }
    TestFixedValue(uint32_t val) : BtreeValue() { val_ = val; }
    TestFixedValue() : TestFixedValue((uint32_t)-1) {}
    TestFixedValue(const TestFixedValue& other) : BtreeValue() { val_ = other.val_; };
    TestFixedValue(const sisl::Blob& b, bool copy) : BtreeValue() { val_ = *(r_cast< uint32_t const* >(b.cbytes())); }
    virtual ~TestFixedValue() = default;

    static TestFixedValue generate_rand() { return TestFixedValue{g_randval_generator(g_re)}; }

    TestFixedValue& operator=(const TestFixedValue& other) {
        val_ = other.val_;
        return *this;
    }

    sisl::Blob serialize() const override {
        sisl::Blob b{r_cast< uint8_t const* >(&val_), uint32_cast(sizeof(val_))};
        return b;
    }

    uint32_t serialized_size() const override { return sizeof(val_); }
    static uint32_t get_fixed_size() { return sizeof(val_); }
    void deserialize(const sisl::Blob& b, bool copy) { val_ = *(r_cast< uint32_t const* >(b.cbytes())); }

    std::string to_string() const override { return fmt::format("{}", val_); }

    friend std::ostream& operator<<(std::ostream& os, const TestFixedValue& v) {
        os << v.to_string();
        return os;
    }

    friend std::istream& operator>>(std::istream& is, TestFixedValue& v) {
        uint32_t value;
        is >> value;
        v = TestFixedValue{value};
        return is;
    }

    // This is not mandatory overridden method for BtreeValue, but for testing comparision
    bool operator==(const TestFixedValue& other) const { return (val_ == other.val_); }

    uint32_t value() const { return val_; }

private:
    uint32_t val_;
};

class TestVarLenValue : public BtreeValue {
private:
    static uint32_t rand_val_size() {
        return (uint32_cast(std::abs(std::round(g_randvalsize_generator(g_re)))) % g_max_valsize) + 1;
    }

public:
    TestVarLenValue(bnodeid_t val) { assert(0); }
    TestVarLenValue(const std::string& val) : BtreeValue(), val_{val} {}
    TestVarLenValue() = default;
    TestVarLenValue(const TestVarLenValue& other) : BtreeValue() { val_ = other.val_; };
    TestVarLenValue(const sisl::Blob& b, bool copy) :
            BtreeValue(), val_{std::string((const char*)b.cbytes(), b.size())} {}
    virtual ~TestVarLenValue() = default;

    TestVarLenValue& operator=(const TestVarLenValue& other) {
        val_ = other.val_;
        return *this;
    }

    static TestVarLenValue generate_rand() { return TestVarLenValue{gen_random_string(rand_val_size())}; }

    sisl::Blob serialize() const override {
        sisl::Blob b{r_cast< const uint8_t* >(val_.c_str()), uint32_cast(val_.size())};
        return b;
    }

    uint32_t serialized_size() const override { return (uint32_t)val_.size(); }
    static uint32_t get_fixed_size() { return 0; }

    void deserialize(const sisl::Blob& b, bool copy) { val_ = std::string((const char*)b.cbytes(), b.size()); }

    std::string to_string() const override { return fmt::format("{}", val_); }

    friend std::ostream& operator<<(std::ostream& os, const TestVarLenValue& v) {
        os << v.to_string();
        return os;
    }

    friend std::istream& operator>>(std::istream& is, TestVarLenValue& v) {
        std::string value;
        is >> value;
        v = TestVarLenValue{value};
        return is;
    }

    // This is not mandatory overridden method for BtreeValue, but for testing comparision
    bool operator==(const TestVarLenValue& other) const { return (val_ == other.val_); }

    std::string value() const { return val_; }

private:
    std::string val_;
};

class TestIntervalValue : public BtreeIntervalValue {
private:
#pragma pack(1)
    uint32_t base_val_{0};
    uint16_t offset_{0};
#pragma pack()

public:
    TestIntervalValue(bnodeid_t val) {
        assert(0);
    }
    TestIntervalValue(uint32_t val, uint16_t o) : BtreeIntervalValue(), base_val_{val}, offset_{o} {
    }
    TestIntervalValue() = default;
    TestIntervalValue(const TestIntervalValue& other) :
            BtreeIntervalValue(), base_val_{other.base_val_}, offset_{other.offset_} {
    }
    TestIntervalValue(const sisl::Blob& b, bool copy) : BtreeIntervalValue() {
        this->deserialize(b, copy);
    }
    virtual ~TestIntervalValue() = default;

    static TestIntervalValue generate_rand() {
        return TestIntervalValue{g_randval_generator(g_re), s_cast< uint16_t >(0)};
    }

    ///////////////////////////// Overriding methods of BtreeValue //////////////////////////
    TestIntervalValue& operator=(const TestIntervalValue& other) = default;
    sisl::Blob serialize() const override {
        sisl::Blob b{r_cast< uint8_t const* >(this), sizeof(TestIntervalValue)};
        return b;
    }

    uint32_t serialized_size() const override {
        return sizeof(TestIntervalValue);
    }
    static uint32_t get_fixed_size() {
        return sizeof(TestIntervalValue);
    }
    void deserialize(const sisl::Blob& b, bool) {
        TestIntervalValue const* other = r_cast< TestIntervalValue const* >(b.cbytes());
        base_val_ = other->base_val_;
        offset_ = other->offset_;
    }

    std::string to_string() const override {
        return fmt::format("{}", value());
    }
    uint64_t value() const {
        return (uint64_cast(base_val_) << 16) | offset_;
    }

    friend std::ostream& operator<<(std::ostream& os, const TestIntervalValue& v) {
        os << v.to_string();
        return os;
    }

    friend std::istream& operator>>(std::istream& is, TestIntervalValue& v) {
        uint32_t base_val_;
        uint16_t offset_;
        char dummy;
        is >> base_val_ >> dummy >> offset_;
        v = TestIntervalValue{base_val_, offset_};
        return is;
    }

    ///////////////////////////// Overriding methods of BtreeIntervalValue //////////////////////////
    void shift(int n) override {
        offset_ += n;
    }

    sisl::Blob serialize_prefix() const override {
        return sisl::Blob{uintptr_cast(const_cast< uint32_t* >(&base_val_)), uint32_cast(sizeof(uint32_t))};
    }
    sisl::Blob serialize_suffix() const override {
        return sisl::Blob{uintptr_cast(const_cast< uint16_t* >(&offset_)), uint32_cast(sizeof(uint16_t))};
    }
    uint32_t serialized_prefix_size() const override {
        return uint32_cast(sizeof(uint32_t));
    }
    uint32_t serialized_suffix_size() const override {
        return uint32_cast(sizeof(uint16_t));
    }

    void deserialize(sisl::Blob const& prefix, sisl::Blob const& suffix, bool) override {
        DEBUG_ASSERT_EQ(prefix.size(), sizeof(uint32_t), "Invalid prefix size on deserialize");
        DEBUG_ASSERT_EQ(suffix.size(), sizeof(uint16_t), "Invalid suffix size on deserialize");
        base_val_ = *(r_cast< uint32_t const* >(prefix.cbytes()));
        offset_ = *(r_cast< uint16_t const* >(suffix.cbytes()));
    }

    bool operator==(TestIntervalValue const& other) const {
        return ((base_val_ == other.base_val_) && (offset_ == other.offset_));
    }
};
