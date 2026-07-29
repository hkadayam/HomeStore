/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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
//
// Multi-process replication test harness.  HomeStore's manager singletons are process-global, so a cluster is
// run as one OS process per replica.  Coordination is via a boost::interprocess shared-memory barrier; process
// 0 is the driver (spawns the peers and creates the raft group), the rest rendezvous into it.  The replication
// API surface used here is thin: a TestReplApplication (ReplApplication impl) + repl_service().create_replica_set().
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <boost/process/v1.hpp> // boost 1.91 defaults boost::process to v2; the child/group/cmd API used here is v1
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/nil_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <gtest/gtest.h>

#include "sisl/logging/logging.h"
#include "sisl/options/options.h"
#include "sisl/fds/enum.h"

#include "homestore/replication/repl_manager.h" // ReplApplication, ReplicationManager, repl_service()
#include "homestore/replication/replica_set.h"  // ReplicaSetListener, ReplicaSet, ReplicaSetOptions
#include "homestore/base/homestore_utils.h"       // hs_utils::gen_random_uuid
#include "homestore/test_common/hs_test_common.h"
#include "homestore/test_common/hs_test_store.h" // TestStore (interface only — no btree dependency here)

SISL_OPTION_GROUP(test_repl_common_setup,
                  (replicas, "", "replicas", "Total number of replicas in the group",
                   ::cxxopts::value< uint32_t >()->default_value("3"), "number"),
                  (spare_replicas, "", "spare_replicas", "Additional spare replicas not part of the group at bootstrap",
                   ::cxxopts::value< uint32_t >()->default_value("1"), "number"),
                  (base_port, "", "base_port", "Port number of the first replica",
                   ::cxxopts::value< uint16_t >()->default_value("4000"), "number"),
                  (replica_num, "", "replica_num",
                   "Internal replica num (set when spawning peer processes — don't override)",
                   ::cxxopts::value< uint16_t >()->default_value("0"), "number"),
                  (replica_dev_list, "", "replica_dev_list",
                   "Device list for all replicas (flattened, replicas * devs_per_replica)",
                   ::cxxopts::value< std::vector< std::string > >(), "path [...]"));

namespace bip = boost::interprocess;
namespace bproc = boost::process::v1;

namespace test_common {

// Cluster-wide phases the shared-memory barrier steps through.
ENUM(ReplTestPhase, uint32_t, REGISTER, MEMBER_START, TEST_RUN, VALIDATE, CLEANUP);

class HSReplTestHelper : public HSTestHelper {
protected:
    // One instance lives in shared memory; every replica process maps it and uses it as an N-way barrier.
    struct IPCData {
        bip::interprocess_mutex mtx_;
        bip::interprocess_condition cv_;
        bip::interprocess_mutex exec_mtx_; // serializes exclusive_replica() sections across processes

        ReplTestPhase phase_{ReplTestPhase::REGISTER};
        uint32_t registered_count_{0};
        uint32_t test_start_count_{0};
        uint32_t verify_start_count_{0};
        uint32_t cleanup_start_count_{0};
        uint64_t test_dataset_size_{0};

        void sync_for_member_start(uint32_t n = 0) { sync_for(registered_count_, ReplTestPhase::MEMBER_START, n); }
        void sync_for_test_start(uint32_t n = 0) { sync_for(test_start_count_, ReplTestPhase::TEST_RUN, n); }
        void sync_for_verify_start(uint32_t n = 0) { sync_for(verify_start_count_, ReplTestPhase::VALIDATE, n); }
        void sync_for_cleanup_start(uint32_t n = 0) { sync_for(cleanup_start_count_, ReplTestPhase::CLEANUP, n); }

    private:
        // Barrier: the last of `max_count` arrivals flips the phase and wakes the rest.
        void sync_for(uint32_t& count, ReplTestPhase new_phase, uint32_t max_count) {
            if (max_count == 0) {
                max_count = SISL_OPTIONS["replicas"].as< uint32_t >();
            }
            std::unique_lock< bip::interprocess_mutex > lg(mtx_);
            if (++count == max_count) {
                phase_ = new_phase;
                cv_.notify_all();
            } else {
                cv_.wait(lg, [this, new_phase]() { return phase_ == new_phase; });
            }
            count = 0;
        }
    };

public:
    // Application object handed to HomeStore — pure delegation back to the helper.
    class TestReplApplication : public ReplApplication {
    public:
        explicit TestReplApplication(HSReplTestHelper& h) : helper_{h} {}

        bool need_timeline_consistency() const override { return false; }

        shared< ReplicaSetListener > create_replica_set_listener(GroupId group_id, bool /*load_existing*/) override {
            return helper_.get_listener(group_id);
        }
        void destroy_replica_set_listener(GroupId /*group_id*/) override {}

        std::pair< std::string, uint16_t > lookup_peer(ReplicaId uuid, GroupId /*group_id*/) const override {
            auto const it = helper_.members_.find(uuid);
            RELEASE_ASSERT(it != helper_.members_.end(), "lookup_peer for a non-member replica");
            return {std::string{"127.0.0.1"}, uint16_t(SISL_OPTIONS["base_port"].as< uint16_t >() + it->second)};
        }

        ReplicaId get_my_repl_id() const override { return helper_.my_replica_id_; }

    private:
        HSReplTestHelper& helper_;
    };

    // The standard test listener: applies each committed (key,value) into a handed-in TestStore.  on_commit is
    // awaited by the engine, so commit_count() reflects entries committed AND durably applied.  Snapshot callbacks
    // are stubbed (baseline resync not exercised here).  Wire format: header = 8-byte key, value = 4-byte payload.
    class ReplTestListener : public ReplicaSetListener {
    public:
        explicit ReplTestListener(shared< TestStore > store, ReplicaSetOptions options = {}) :
                store_{std::move(store)}, options_{options} {}

        Async< void > on_commit(int64_t lsn, sisl::Blob const& header, sisl::Blob const& value,
                                BlkIds const& /*blob_refs*/) override {
            auto const key = *r_cast< uint64_t const* >(header.cbytes());
            auto const val = *r_cast< uint32_t const* >(value.cbytes());
            co_await store_->apply(key, val);
            // Bump only after the store apply completes: commit_count() then counts committed-AND-applied entries.
            last_committed_lsn_.store(lsn, std::memory_order_release);
            commit_count_.fetch_add(1, std::memory_order_acq_rel);
            co_return;
        }

        bool on_pre_commit(int64_t, sisl::Blob const&) override { return true; }
        Async< void > on_rollback(int64_t, sisl::Blob const&) override { co_return; }
        void on_config_rollback(int64_t) override {}
        void on_change_in_role(ReplicaRole role) override { role_ = role; }
        void on_destroy(GroupId const&) override {}
        void on_start_replace_member(ReplicaMemberInfo const&, ReplicaMemberInfo const&, std::string_view) override {}
        void on_complete_replace_member(ReplicaMemberInfo const&, ReplicaMemberInfo const&, std::string_view) override {}
        void on_membership_change(std::set< ReplicaId > const&, std::set< ReplicaId > const&) override {}

        AsyncReplResult< shared< ReplSnapshot > > take_snapshot(lsn_t) override {
            co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
        }
        AsyncReplResult< shared< ReplSnapshot::Builder > > build_snapshot(lsn_t) override {
            co_return folly::makeUnexpected(ReplError::BAD_REQUEST);
        }
        bool apply_snapshot(shared< ReplSnapshot >) override { return false; }
        shared< ReplSnapshot > last_snapshot() override { return nullptr; }
        void release_snapshot(shared< ReplSnapshot >) override {}
        ReplicaSetOptions replica_set_options() override { return options_; }

        uint64_t commit_count() const { return commit_count_.load(std::memory_order_acquire); }
        int64_t last_committed_lsn() const { return last_committed_lsn_.load(std::memory_order_acquire); }
        shared< TestStore > const& store() const { return store_; }

    private:
        shared< TestStore > store_;
        ReplicaSetOptions options_;
        ReplicaRole role_{ReplicaRole::FOLLOWER};
        std::atomic< uint64_t > commit_count_{0};
        std::atomic< int64_t > last_committed_lsn_{-1};
    };

    HSReplTestHelper(std::string name, std::vector< std::string > args, char** argv) :
            name_{std::move(name)}, args_{std::move(args)}, argv_{argv} {}

    // ── Cluster lifecycle ────────────────────────────────────────────────────────────────────────────────────────
    void setup(uint32_t num_replicas) {
        num_replicas_ = num_replicas;
        replica_num_ = SISL_OPTIONS["replica_num"].as< uint16_t >();

        sisl::logging::SetLogger(name_ + "_replica_" + std::to_string(replica_num_));
        sisl::logging::SetLogPattern("[%D %T%z] [%^%L%$] [%n] [%t] %v");

        assign_replica_ids(num_replicas);
        slice_devices(num_replicas);
        rendezvous_processes(num_replicas);

        LOGINFO("Starting HomeStore replica={}", replica_num_);
        HSTestHelper::BootParams bp;
        bp.repl_app = std::make_shared< TestReplApplication >(*this);
        start_homestore(name_ + std::to_string(replica_num_), std::move(bp), dev_list_);
    }

    void teardown() {
        LOGINFO("Stopping HomeStore replica={}", replica_num_);
        shutdown_homestore(dev_list_.empty() /* cleanup only generated devices */);
    }

    void restart(uint32_t shutdown_delay_secs = 5u) { restart_homestore(shutdown_delay_secs); }
    void restart_one_by_one() {
        exclusive_replica([this]() { restart_homestore(5u); });
    }

    // ── Group bring-up: create the standard listener over `store`, barrier, then replica 0 creates the raft group;
    // peers rendezvous via get_listener(). The store is handed in so a test picks its backend (mem vs index). ──────
    void register_replica_set(shared< TestStore > store) {
        store_ = std::move(store);
        listener_ = std::make_shared< ReplTestListener >(store_);

        if (replica_num_ != 0) {
            pending_listeners_.push_back(listener_);
        }

        ipc_data_->sync_for_member_start();

        if (replica_num_ != 0) {
            return;
        }

        // Leader path: the first --replicas members join at bootstrap; spares are added later by the test.
        std::set< ReplicaId > members;
        for (auto const& [id, idx] : members_) {
            if (idx < SISL_OPTIONS["replicas"].as< uint32_t >()) {
                members.insert(id);
            }
        }

        GroupId const group_id = hs_utils::gen_random_uuid();
        {
            std::unique_lock lg(groups_mtx_);
            repl_groups_.emplace(group_id, listener_);
        }

        auto result = iomgr().spawn_and_block(
            iomanager::ReactorTarget::any(), [group_id, members]() -> Async< ReplResult< shared< ReplicaSet > > > {
                co_return co_await repl_service().create_replica_set(group_id, members, ReplicaSetOptions{});
            }());
        ASSERT_TRUE(result.hasValue()) << "create_replica_set failed for group_id=" << boost::uuids::to_string(group_id)
                                       << " err=" << result.error();

        // Leadership is established asynchronously once the raft server starts; the creator bootstraps the group and
        // (leadership pinned for the test) wins the initial election.  get_leader_id() is nil for the first few
        // election ticks, so wait for the creator to win rather than asserting it synchronously.
        auto const rs = result.value();
        for (uint32_t waited_ms = 0; rs->get_leader_id() != my_replica_id_; waited_ms += 100) {
            RELEASE_ASSERT(waited_ms < 30000u, "creator did not become the initial leader within 30s");
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    // The standard listener/store/ReplicaSet this replica set up (valid after register_replica_set()).  On a
    // follower, repl_set() becomes non-null once the leader's first RPC binds the listener to a ReplicaSet.
    shared< ReplTestListener > listener() const { return listener_; }
    shared< TestStore > store() const { return store_; }
    shared< ReplicaSet > repl_set() const { return listener_ ? listener_->replica_set() : nullptr; }

    // Called (via TestReplApplication) both when replica 0 creates the group and when a peer/reload brings one up.
    shared< ReplicaSetListener > get_listener(GroupId group_id) {
        std::unique_lock lg(groups_mtx_);
        if (auto it = repl_groups_.find(group_id); it != repl_groups_.end() && it->second) {
            return it->second;
        }

        RELEASE_ASSERT(!pending_listeners_.empty(), "get_listener for group_id with no pending listener registered");
        auto listener = std::move(pending_listeners_.front());
        pending_listeners_.erase(pending_listeners_.begin());
        repl_groups_.emplace(group_id, listener);
        LOGINFO("Bound listener to group_id={} on replica={}", boost::uuids::to_string(group_id), replica_num_);
        return listener;
    }

    void unregister_listener(GroupId group_id) {
        std::unique_lock lg(groups_mtx_);
        repl_groups_.erase(group_id);
    }

    void add_pending_listener(shared< ReplicaSetListener > listener) {
        std::unique_lock lg(groups_mtx_);
        pending_listeners_.push_back(std::move(listener));
    }

    size_t num_groups() const {
        std::unique_lock lg(groups_mtx_);
        return repl_groups_.size();
    }

    // ── Identity / topology accessors ────────────────────────────────────────────────────────────────────────────
    uint16_t replica_num() const { return replica_num_; }
    ReplicaId my_replica_id() const { return my_replica_id_; }
    ReplicaId replica_id(uint16_t member_idx) const {
        for (auto const& [id, idx] : members_) {
            if (idx == member_idx) {
                return id;
            }
        }
        return boost::uuids::nil_uuid();
    }

    uint16_t member_idx(ReplicaId id) const {
        auto const it = members_.find(id);
        return (it != members_.end()) ? it->second : uint16_t(members_.size());
    }

    // ── Cross-process barriers ───────────────────────────────────────────────────────────────────────────────────
    void sync_for_test_start(uint32_t n = 0) { ipc_data_->sync_for_test_start(n); }
    void sync_for_verify_start(uint32_t n = 0) { ipc_data_->sync_for_verify_start(n); }
    void sync_for_cleanup_start(uint32_t n = 0) { ipc_data_->sync_for_cleanup_start(n); }
    void sync_dataset_size(uint64_t sz) { ipc_data_->test_dataset_size_ = sz; }
    uint64_t dataset_size() const { return ipc_data_->test_dataset_size_; }

    // Serialize a section so only one replica process runs it at a time (e.g. rolling restart).
    void exclusive_replica(std::function< void() > const& f) {
        std::unique_lock< bip::interprocess_mutex > lg(ipc_data_->exec_mtx_);
        f();
    }

private:
    void assign_replica_ids(uint32_t num_replicas) {
        boost::uuids::string_generator gen;
        for (uint32_t i{0}; i < num_replicas; ++i) {
            auto const id = gen(fmt::format("{:04}", i) + std::string{"0123456789abcdef0123456789ab"});
            if (i == replica_num_) {
                my_replica_id_ = id;
            }
            members_.emplace(id, i);
        }
    }

    // Carve this replica's slice out of the flattened --replica_dev_list (if given); otherwise dev_list_ stays
    // empty and HSTestHelper generates per-replica files.
    void slice_devices(uint32_t num_replicas) {
        if (!SISL_OPTIONS.count("replica_dev_list")) {
            return;
        }
        auto const all = SISL_OPTIONS["replica_dev_list"].as< std::vector< std::string > >();
        RELEASE_ASSERT(all.size() % num_replicas == 0, "replica_dev_list size must be a multiple of #replicas");
        auto const per = all.size() / num_replicas;
        for (uint32_t j{0}; j < per; ++j) {
            dev_list_.emplace_back(all[replica_num_ * per + j], HSDevType::Data);
        }
    }

    void rendezvous_processes(uint32_t num_replicas) {
        static constexpr char kShmem[] = "raft_repl_test_shmem";
        if (replica_num_ == 0) {
            bip::shared_memory_object::remove(kShmem);
            for (uint32_t i{0}; i < num_replicas; ++i) {
                check_and_kill(SISL_OPTIONS["base_port"].as< uint16_t >() + i);
            }
            shm_ = std::make_unique< bip::shared_memory_object >(bip::create_only, kShmem, bip::read_write);
            shm_->truncate(sizeof(IPCData));
            region_ = std::make_unique< bip::mapped_region >(*shm_, bip::read_write);
            ipc_data_ = new (region_->get_address()) IPCData;

            for (uint32_t i{1}; i < num_replicas; ++i) {
                std::string cmd;
                fmt::format_to(std::back_inserter(cmd), "{} --replica_num {}", args_[0], i);
                for (size_t j{1}; j < args_.size(); ++j) {
                    fmt::format_to(std::back_inserter(cmd), " {}", args_[j]);
                }
                LOGINFO("Spawning replica={} instance: {}", i, cmd);
                bproc::child c(bproc::cmd = cmd, proc_grp_);
                c.detach();
            }
        } else {
            shm_ = std::make_unique< bip::shared_memory_object >(bip::open_only, kShmem, bip::read_write);
            region_ = std::make_unique< bip::mapped_region >(*shm_, bip::read_write);
            ipc_data_ = static_cast< IPCData* >(region_->get_address());
        }
    }

    // Free a port left bound by a previous crashed run.
    static void check_and_kill(int port) {
        std::string cmd = "lsof -t -i:" + std::to_string(port);
        if (::system(cmd.c_str()) == 0) {
            cmd += " | xargs kill -9";
            if (::system(cmd.c_str()) == 0) {
                LOGINFO("Killed stale process on port {}", port);
            }
        }
    }

private:
    std::string name_;
    std::vector< std::string > args_;
    char** argv_;
    uint16_t replica_num_{0};
    uint32_t num_replicas_{0};

    std::vector< DevInfo > dev_list_; // this replica's slice of --replica_dev_list (empty => generated files)

    bproc::group proc_grp_;
    std::unique_ptr< bip::shared_memory_object > shm_;
    std::unique_ptr< bip::mapped_region > region_;
    IPCData* ipc_data_{nullptr};

    mutable std::mutex groups_mtx_;
    std::map< GroupId, shared< ReplicaSetListener > > repl_groups_;
    std::vector< shared< ReplicaSetListener > > pending_listeners_;
    std::map< ReplicaId, uint16_t > members_;
    ReplicaId my_replica_id_;

    // The standard listener/store this replica set up via register_replica_set() (single group per test for now).
    shared< ReplTestListener > listener_;
    shared< TestStore > store_;
};

} // namespace test_common
