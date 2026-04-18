#pragma once

#include <map>
#include <mutex>
#include <gtest/gtest.h>

#include "btree_test_kvs.hpp"
#include "range_scheduler.hpp"

ENUM(ShadowMapDelta, uint8_t, Added, Removed, Updated);

template < typename K, typename V >
class ShadowMap {
private:
    std::map< K, V > map_;
    RangeScheduler range_scheduler_;
    uint32_t max_keys_;
    // Test-only shared map; short critical sections so a plain std::mutex is adequate even when accessed from inside
    // reactor coroutines.
    using mutex = std::mutex;
    mutex mutex_;
    //#define SHOWM(X) cout << #X " = " << (X) << endl
    //    void testPrint(std::map< uint32_t, std::string >& map_, int i) {
    //        SHOWM(m[i]);
    //        SHOWM(m.find(i)->first);
    //    }
public:
    ShadowMap(uint32_t num_keys) : range_scheduler_(num_keys), max_keys_{num_keys} {}

    void put_and_check(const K& key, const V& val, const V& old_val, bool expected_success) {
        std::lock_guard lock{mutex_};
        auto const [it, happened] = map_.insert(std::make_pair(key, val));
        ASSERT_EQ(happened, expected_success) << "Testcase issue, expected inserted slots to be in shadow map";
        if (!happened) {
            ASSERT_EQ(old_val, it->second) << "Put: Existing value doesn't return correct data for key: " << it->first;
        }
        range_scheduler_.put_key(key.key());
    }

    void force_put(const K& key, const V& val) {
        std::lock_guard lock{mutex_};
        map_.insert_or_assign(key, val);
        range_scheduler_.put_key(key.key());
    }

    void range_upsert(uint64_t start_k, uint32_t count, const V& val) {
        std::lock_guard lock{mutex_};
        for (uint32_t i{0}; i < count; ++i) {
            K key{start_k + i};
            V range_value{val};
            if constexpr (std::is_same_v< V, TestIntervalValue >) {
                range_value.shift(i);
            }
            map_.insert_or_assign(key, range_value);
        }
        range_scheduler_.put_keys(start_k, start_k + count - 1);
    }

    void range_update(const K& start_key, uint32_t count, const V& new_val) {
        std::lock_guard lock{mutex_};
        auto const start_it = map_.lower_bound(start_key);
        auto it = start_it;
        uint32_t c = 0;
        while ((it != map_.end()) && (++c <= count)) {
            it->second = new_val;
            ++it;
        }
        range_scheduler_.remove_keys_from_working(start_key.key(), start_key.key() + count - 1);
    }

    std::pair< K, K > pick_existing_range(const K& start_key, uint32_t max_count) const {
        std::lock_guard lock{mutex_};
        auto const start_it = map_.lower_bound(start_key);
        auto it = start_it;
        uint32_t count = 0;
        while ((it != map_.cend()) && (++count < max_count)) {
            ++it;
        }
        return std::pair(start_it->first, it->first);
    }

    uint32_t max_keys() const { return max_keys_; }

    bool exists(const K& key) const {
        std::lock_guard lock{mutex_};
        return map_.find(key) != map_.end();
    }

    bool exists_in_range(const K& key, uint64_t start_k, uint64_t end_k) const {
        std::lock_guard lock{mutex_};
        const auto itlower = map_.lower_bound(K{start_k});
        const auto itupper = map_.upper_bound(K{end_k});
        auto it = itlower;
        while (it != itupper) {
            if (it->first == key) {
                return true;
            }
            ++it;
        }
        return false;
    }

    uint64_t size() const {
        std::lock_guard lock{mutex_};
        return map_.size();
    }

    uint32_t num_elems_in_range(uint64_t start_k, uint64_t end_k) const {
        const auto itlower = map_.lower_bound(K{start_k});
        const auto itupper = map_.upper_bound(K{end_k});
        return std::distance(itlower, itupper);
    }

    void validate_data(const K& key, const V& btree_val) const {
        std::lock_guard lock{mutex_};
        const auto r = map_.find(key);
        ASSERT_NE(r, map_.end()) << "Key " << key.to_string() << " is not present in shadow map";
        ASSERT_EQ(btree_val, r->second) << "Found value in btree doesn't return correct data for key=" << r->first;
    }

    void remove_and_check(const K& key, const V& btree_val) {
        std::lock_guard lock{mutex_};
        const auto r = map_.find(key);
        ASSERT_NE(r, map_.end()) << "Key " << key.to_string() << " is not present in shadow map";
        ASSERT_EQ(btree_val, r->second) << "Found value in btree doesn't return correct data for key=" << r->first;
        map_.erase(key);
        range_scheduler_.remove_key(key.key());
    }

    void erase(const K& key) {
        std::lock_guard lock{mutex_};
        map_.erase(key);
        range_scheduler_.remove_key(key.key());
    }

    void range_erase(const K& start_key, uint32_t count) {
        std::lock_guard lock{mutex_};
        auto it = map_.lower_bound(start_key);
        uint32_t i{0};
        while ((it != map_.cend()) && (i++ < count)) {
            it = map_.erase(it);
        }
        range_scheduler_.remove_keys(start_key.key(), start_key.key() + count);
    }

    void range_erase(const K& start_key, const K& end_key) {
        std::lock_guard lock{mutex_};
        auto it = map_.lower_bound(start_key);
        auto const end_it = map_.upper_bound(end_key);
        while ((it != map_.cend()) && (it != end_it)) {
            it = map_.erase(it);
        }
        range_scheduler_.remove_keys(start_key.key(), end_key.key());
    }

    std::vector< std::pair< K, ShadowMapDelta > > diff(ShadowMap< K, V > const& other) {
        auto it1 = map_.begin();
        auto it2 = other.map_.begin();
        std::vector< std::pair< K, ShadowMapDelta > > ret_diff;

        while ((it1 != map_.end()) && (it2 != other.map_.end())) {
            auto const x = it1->first.compare(it2->first);
            if (x == 0) {
                if (it1->second != it2->second) {
                    ret_diff.emplace_back(it1->first, ShadowMapDelta::Updated);
                }
                ++it1;
                ++it2;
            } else if (x < 0) {
                // Has in current map, add it to addition
                ret_diff.emplace_back(it1->first, ShadowMapDelta::Added);
                ++it1;
            } else {
                ret_diff.emplace_back(it2->first, ShadowMapDelta::Removed);
                ++it2;
            }
        }

        while (it1 != map_.end()) {
            ret_diff.emplace_back(it1->first, ShadowMapDelta::Added);
            ++it1;
        }

        while (it2 != other.map_.end()) {
            ret_diff.emplace_back(it2->first, ShadowMapDelta::Removed);
            ++it2;
        }
        return ret_diff;
    }

    mutex& guard() { return mutex_; }
    std::map< K, V >& map() { return map_; }
    const std::map< K, V >& map_const() const { return map_; }

    void foreach (std::function< void(K, V) > func) const {
        std::lock_guard lock{mutex_};
        for (const auto& [key, value] : map_) {
            func(key, value);
        }
    }
    std::string to_string() const {
        std::string result;
        std::stringstream ss;
        const int key_width = 20;

        // Format the key-value pairs and insert them into the result string
        ss << std::left << std::setw(key_width) << "KEY"
           << " "
           << "VaLUE" << '\n';
        foreach ([&](const auto& key, const auto& value) {
            ss << std::left << std::setw(key_width) << key.to_string() << " " << value.to_string() << '\n';
        })
            ;
        result = ss.str();
        return result;
    }

    std::pair< uint32_t, uint32_t > pick_random_non_existing_keys(uint32_t max_keys) {
        do {
            std::lock_guard lock{mutex_};
            auto ret = range_scheduler_.pick_random_non_existing_keys(max_keys);
            if (ret.first != UINT32_MAX) {
                return ret;
            }
        } while (true);
    }

    // Returns {UINT32_MAX, UINT32_MAX} if no existing non-working keys are available.
    // Temporal retry: release lock between attempts so other threads can finish queries (freeing working keys).
    // Bail immediately if no keys exist at all (concurrent removes emptied the set).
    std::pair< uint32_t, uint32_t > pick_random_existing_keys(uint32_t max_keys) {
        for (;;) {
            std::lock_guard lock{mutex_};
            if (range_scheduler_.existing_count() == 0) {
                return {UINT32_MAX, UINT32_MAX}; // All keys removed — nothing to pick
            }
            auto ret = range_scheduler_.pick_random_existing_keys(max_keys);
            if (ret.first != UINT32_MAX) {
                return ret;
            }
            // All existing keys are in the working set (queries in-flight). Release lock, let other threads finish.
        }
    }

    std::pair< uint32_t, uint32_t > pick_random_non_working_keys(uint32_t max_keys) {
        do {
            std::lock_guard lock{mutex_};
            auto ret = range_scheduler_.pick_random_non_working_keys(max_keys);
            if (ret.first != UINT32_MAX) {
                return ret;
            }
        } while (true);
    }

    void remove_keys_from_working(uint32_t s, uint32_t e) {
        std::lock_guard lock{mutex_};
        range_scheduler_.remove_keys_from_working(s, e);
    }

    void remove_keys(uint32_t start_key, uint32_t end_key) {
        std::lock_guard lock{mutex_};
        range_scheduler_.remove_keys(start_key, end_key);
    }

    void save(const std::string& filename) {
        std::lock_guard lock{mutex_};
        std::ofstream file(filename);
        for (const auto& [key, value] : map_) {
            file << key.key() << " " << value << '\n';
        }
        file.close();
        LOGINFO("Saved shadow map to file: {}", filename);
    }

    void load(const std::string& filename) {
        std::lock_guard lock{mutex_};
        std::ifstream file(filename);
        if (file.is_open()) {
            map_.clear();
            uint64_t k;
            V value;
            while (file >> k >> value) {
                K key{k};
                map_.emplace(key, std::move(value));
                range_scheduler_.put_key(k);
            }
            file.close();
        }
    }
};
