# NuRaft Coroutine-Readiness Plan

Status: scoping complete, ready to execute.
Owner: TBD.
Scope: a fork of [eBay/NuRaft](https://github.com/eBay/NuRaft) added as a git
submodule under HomeStore, plus a folly::AsyncSocket-based transport plugged
into iomanager's existing reactor pool, plus adapter changes in
`homestore/replication/`.

## Table of contents

1. [Goal](#1-goal)
2. [Non-goals (what stays sync)](#2-non-goals-what-stays-sync)
3. [Submodule setup](#3-submodule-setup)
4. [Runtime model — one pool everywhere](#4-runtime-model--one-pool-everywhere)
5. [NuRaft fork — required changes](#5-nuraft-fork--required-changes)
6. [Transport — folly::AsyncSocket on iomgr](#6-transport--follyasyncsocket-on-iomgr)
7. [HomeStore-side changes](#7-homestore-side-changes)
8. [Leader flow (end-to-end)](#8-leader-flow-end-to-end)
9. [Follower flow (end-to-end)](#9-follower-flow-end-to-end)
10. [NuRaft API seam — rpc_client and rpc_listener](#10-nuraft-api-seam--rpc_client-and-rpc_listener)
11. [Sync state_machine methods — sync_executor hop](#11-sync-state_machine-methods--sync_executor-hop)
12. [log_store adapter](#12-log_store-adapter)
13. [Locking discipline](#13-locking-discipline)
14. [Audits to do during execution](#14-audits-to-do-during-execution)
15. [LOC estimate](#15-loc-estimate)
16. [Acceptance tests](#16-acceptance-tests)
17. [Out-of-scope follow-ups](#17-out-of-scope-follow-ups)

## 1. Goal

Make NuRaft's two hot paths fully coroutine-driven so HomeStore can run N raft
groups on M threads (M ≪ N) without parking OS threads on application I/O.

Hot paths:

- **HomeStore → Raft (proposer):** `co_await raft->append_entries(logs)` returns
  on commit; `co_await raft->append_entries_async(logs)` returns on accept.
- **Raft → HomeStore (apply):** `co_await sm->commit_ext(idx)` and
  `co_await sm->pre_commit_ext(idx)` — both run as coroutines on the iomanager
  reactor pool.
- **Follower receive path:** `handle_append_entries` returns `Task<ptr<resp_msg>>`
  so the rpc-receive thread is freed during durability waits.

Target threading model: **all coroutines and AsyncSocket I/O share iomanager's
existing reactor pool** (the `folly::IOThreadPoolExecutor pool_` with
`num_reactors` threads, each running a `folly::EventBase` with optional
io_uring backend). No new pool, no new threads on the hot path. A small sidecar
`sync_executor` (1–2 dedicated threads) hosts the rare sync state_machine calls
(`commit_config`, `apply_snapshot`, `rollback_ext`) so they cannot wedge a
reactor.

## 2. Non-goals (what stays sync)

- `bg_append_thread_` stays a `std::thread`. Leader peer-fanout stays
  callback-based using `rpc_client::send(req, when_done, timeout)`. (Coro-izing
  the leader fanout for `folly::coro::collectAll` is an out-of-scope follow-up.)
- Snapshot APIs stay sync: `create_snapshot`, `read_logical_snp_obj`,
  `save_logical_snp_obj`, `apply_snapshot`, `last_snapshot`, `free_user_snp_ctx`.
  Called from commit_loop or snapshot-install RPC; hopped to `sync_executor`.
- `state_machine::commit_config`, `rollback_config`, `rollback_ext` stay sync.
  Called from commit_loop; hopped to `sync_executor`.
- `log_store` virtuals stay sync. The adapter's `store_log_entry` calls
  homestore's `LogStore::quick_append` which is sync, lock-free, in-memory.
  Read-path methods bridge via `folly::coro::blockingWait` only on threads
  where parking is acceptable (never on a reactor running a hot-path coro).
  See §12.
- `state_mgr` virtuals stay sync.
- `rpc_client::send` interface stays callback-based. NuRaft is unchanged here;
  our `FollyRpcClient` implementation just fires the callback when the response
  arrives on the EventBase thread.
- `rpc_listener::listen(handler)` interface stays. Our listener calls
  `handler->handle_append_entries(req)` (now `Task<>`-returning) and awaits it
  inside its own coroutine.
- `recursive_mutex lock_` stays as `std::recursive_mutex`. No flattening.
- `cli_lock_`, `config_lock_`, peer per-mutex, all atomics — unchanged.
- `delayed_task_scheduler` continues to drive election/heartbeat timers; backed
  by an iomgr reactor's EventBase timer (small shim, see §5).

## 3. Submodule setup

```bash
# 1. Fork eBay/NuRaft on GitHub (e.g., to hkadayam/NuRaft-coro).

# 2. Add as submodule under HomeStore:
cd /workspaces/HomeStore
git submodule add https://github.com/<you>/NuRaft-coro.git extern/NuRaft
git submodule update --init --recursive
```

CMake glue (in HomeStore's top-level `CMakeLists.txt`, before the
`add_subdirectory(src)` for replication):

```cmake
# Suppress NuRaft tests/examples from polluting our build:
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(NURAFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NURAFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Make NuRaft pick up our conan-provided deps (boost::asio, openssl) rather
# than fetch its own:
# (the exact mechanism depends on NuRaft's CMakeLists.txt; may need a small
# patch in the fork to remove its FetchContent calls and use find_package
# instead. Verify during execution.)

add_subdirectory(extern/NuRaft EXCLUDE_FROM_ALL)
```

Replication CMakeLists.txt links against `nuraft` (the submodule's static lib
target). The current `add_subdirectory(homestore/replication/)` line at
[src/CMakeLists.txt:78](src/CMakeLists.txt#L78) is uncommented as part of this
work.

The fork's modifications (the entire §5 fork delta) live on a long-lived branch
in the fork repo — e.g., `coro-readiness`. Periodically rebase against
`upstream/master` to absorb eBay's bug fixes.

## 4. Runtime model — one pool everywhere

```
iomanager (existing, in src/iomanager/)
  ├─ folly::IOThreadPoolExecutor pool_          ← ONE pool, M threads
  ├─ EventBase 0  (= reactor 0, = exec-thr 0)   ← runs IoUringBackend
  ├─ EventBase 1  (= reactor 1)
  ├─ ...
  └─ EventBase M-1

This single pool hosts:
  - All folly::coro::Task<> (commit_loop, handle_ae_task, proposer Task<>s,
    iomgr's existing flush coros, etc.)
  - All AsyncSocket I/O (every accepted connection pinned to one reactor)
  - AsyncServerSocket accept loop
  - delayed_task_scheduler timer fires

Sidecar:
  sync_executor (folly::CPUThreadPoolExecutor, 1–2 threads)
    Hosts the rare sync state_machine calls (commit_config, apply_snapshot,
    rollback_ext) that cannot be made coroutine. Reached via co_run_sync helper.
```

Per-connection affinity: each accepted connection (and each outbound peer
client) is pinned to a reactor via `g_iomgr->reactor_for(group_id %
num_reactors)`. Result: a group's reads, its `handle_ae_task` coro, its
response writes, its commit_loop, and its `peer->send_req` calls all tend to
land on the same reactor. Cache-warm, no `runInEventBaseThread` hops on the
hot path.

Coroutine spawning helpers already exist on `IOManager`:
- `spawn_detached(target, factory)` — fire-and-forget Task<>.
- `spawn_waitable(target, task)` — Task<T>-returning awaitable.
- `spawn_and_block(target, task)` — sync wait (only outside reactors).

## 5. NuRaft fork — required changes

These are the changes living in the submodule fork (`extern/NuRaft`). The line
estimates assume mechanical signature propagation; actual diffs may be larger
where lock-drop refactors are needed (see §13).

### 5.1 ABI changes — state_machine virtuals → Task<>

`include/libnuraft/state_machine.hxx`:

```cpp
// Was:
virtual ptr<buffer> commit_ext(const ext_op_params& params);
virtual ptr<buffer> pre_commit_ext(const ext_op_params& params);

// Becomes:
virtual folly::coro::Task<ptr<buffer>> commit_ext(const ext_op_params& params) = 0;
virtual folly::coro::Task<ptr<buffer>> pre_commit_ext(const ext_op_params& params) = 0;
```

All call sites in `handle_commit.cxx` (commit path) and `handle_append_entries.cxx`
(follower path — `pre_commit_ext` is called there per the audit) become
`co_await sm->...`.

The other state_machine virtuals (`rollback_ext`, `commit_config`,
`rollback_config`, snapshot APIs) stay sync. No signature change. They are
hopped via `co_run_sync` (§11).

### 5.2 `bg_commit_thread_` → `commit_loop` coroutine

`include/libnuraft/raft_server.hxx`:

```cpp
// Remove:
//   std::thread bg_commit_thread_;
//   std::condition_variable commit_cv_;
//   std::mutex commit_cv_lock_;

// Add:
folly::coro::Baton commit_baton_;
folly::Executor::KeepAlive<> main_executor_;   // injected via init_options
folly::Executor::KeepAlive<> sync_executor_;   // injected via init_options
```

`raft_server.cxx`:

```cpp
folly::coro::Task<void> raft_server::commit_loop() {
    while (!stopping_.load(std::memory_order_relaxed)) {
        co_await commit_baton_;
        co_await commit_in_bg_exec();    // now Task<void>
    }
}

// At server start, instead of constructing bg_commit_thread_:
folly::coro::co_invoke([this]() -> folly::coro::Task<void> {
    return commit_loop();
}).scheduleOn(main_executor_).start();

// commit() (called when quick_commit_index_ advances) becomes:
void raft_server::commit(ulong target_idx) {
    if (target_idx > quick_commit_index_.load()) {
        quick_commit_index_.store(target_idx);
        commit_baton_.post();
    }
}
```

`commit_in_bg_exec` becomes `Task<void>`; its inner LSN loop becomes a
sequential `co_await commit_ext(idx)` per LSN.

### 5.3 `raft_server::handle_append_entries` → `Task<ptr<resp_msg>>`

`include/libnuraft/raft_server.hxx`:

```cpp
// Was:
ptr<resp_msg> handle_append_entries(req_msg& req);

// Becomes:
folly::coro::Task<ptr<resp_msg>> handle_append_entries(req_msg& req);
```

`handle_append_entries.cxx`:

- Replace `ea_follower_log_append_->wait_ms(...)` with
  `co_await durability_signal_.wait_until(req.get_last_log_idx() +
   req.log_entries().size())`. See §5.5 for the primitive.
- `pre_commit_ext` calls become `co_await sm->pre_commit_ext(idx, buf)`.
- `lock_` is taken/released as today (per the audit, `lock_` is NOT held at
  the durability-wait point or across the pre_commit calls — verify during
  execution).

### 5.4 `raft_server::append_entries` proposer entries → `Task<>`

`include/libnuraft/raft_server.hxx`:

```cpp
// Replace the existing public append_entries with two coroutine entries:
folly::coro::Task<append_result> append_entries(
    const std::vector<ptr<buffer>>& logs);

folly::coro::Task<append_result> append_entries_async(
    const std::vector<ptr<buffer>>& logs);
```

Internal `append_entries_internal(...)` keeps its existing sync behavior
(returns `cmd_result<ptr<buffer>>` with `accepted_` set synchronously).

`append_entries(logs)` body (await commit):

```cpp
folly::coro::Task<append_result> raft_server::append_entries(
    const std::vector<ptr<buffer>>& logs) {
    auto cr = append_entries_internal(logs);
    if (!cr || !cr->get_accepted()) co_return rejected_result();

    folly::coro::Baton baton;
    append_result captured;
    cr->when_ready([&](auto& v, auto& err) {
        captured = make_result(v, err);
        baton.post();
    });
    // AUDIT (see §14): if cr->has_result_ already true at install time,
    // synthesize the call ourselves; cmd_result currently only invokes the
    // handler from set_result, not from when_ready installation.

    co_await baton;
    co_return captured;
}
```

`append_entries_async(logs)` body (await accept):

```cpp
folly::coro::Task<append_result> raft_server::append_entries_async(
    const std::vector<ptr<buffer>>& logs) {
    auto cr = append_entries_internal(logs);
    if (!cr) co_return rejected_result();
    co_return cr->get_accepted() ? accepted_result(cr) : rejected_result();
}
```

`append_result` is a small struct in `include/libnuraft/append_result.hxx`:

```cpp
struct append_result {
    bool accepted;
    bool committed;        // false for the _async variant
    ptr<buffer> value;
    cmd_result_code code;
    ptr<std::exception> err;
};
```

### 5.5 Multi-waiter `durability_signal_` (replaces `ea_follower_log_append_`)

Today's `EventAwaiter` is single-waiter. With concurrent in-flight
`handle_ae_task` coroutines awaiting different target indices, we need:

```cpp
// New file: include/libnuraft/durability_signal.hxx
class durability_signal {
public:
    folly::coro::Task<void> wait_until(ulong target_lsn);
    void advance(ulong new_durable_lsn);   // wakes all waiters with target ≤ new

private:
    struct Waiter {
        ulong target;
        folly::coro::Baton baton;
    };
    std::mutex mtx_;
    std::vector<std::unique_ptr<Waiter>> waiters_;   // unsorted; small N typical
    std::atomic<ulong> last_durable_{0};
};
```

`wait_until` checks `last_durable_` first (fast path: already durable, no
suspension); otherwise registers a Waiter under `mtx_` and `co_await`s its
baton. `advance` updates `last_durable_`, walks waiters under `mtx_`, posts
each baton with `target ≤ new`, removes them.

For typical workloads `waiters_.size()` is small (bounded by RPC pool
concurrency × N groups). If contention on `mtx_` becomes a problem, switch
to a per-group signal indexed by `lock_`. ~80–100 LOC for the primitive +
tests.

### 5.6 Executor injection through `init_options`

`include/libnuraft/raft_server.hxx`:

```cpp
struct raft_server::init_options {
    // ... existing fields ...
    folly::Executor::KeepAlive<> main_executor;   // iomgr's pool
    folly::Executor::KeepAlive<> sync_executor;   // small CPU pool for sync hops
};
```

`raft_server` ctor stores them as members; `commit_loop` schedules itself on
`main_executor`; `co_run_sync` (§11) targets `sync_executor`.

### 5.7 `delayed_task_scheduler` — backed by iomgr EventBase

`delayed_task_scheduler` is used by NuRaft for election timeouts, leadership
transfer, status checks. Today it has its own thread or uses asio::steady_timer.

Replace with a thin shim that posts timer callbacks via
`folly::EventBase::scheduleAt` on a chosen iomgr reactor:

```cpp
class FollyTimerScheduler : public delayed_task_scheduler {
    folly::EventBase* eb_;          // pick an iomgr reactor at construction
    void schedule(ptr<delayed_task>& task, int32 ms) override {
        eb_->scheduleAt([task]() { (*task)(); },
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(ms));
    }
    // cancel_task: track tokens returned from scheduleAt; cancel via EventBase.
};
```

~80 LOC.

### 5.8 What stays as-is in NuRaft

- `bg_append_thread_` stays. `bg_append_ea_` (its EventAwaiter) stays.
  `request_append_entries` / `request_append_entries_for_all` unchanged.
  Leader fanout remains callback-based.
- `cmd_result`'s internal mutex/cv unchanged. Its `when_ready` callback fires
  outside its own lock — safe to call `coro::Baton::post` from inside.
- `recursive_mutex lock_` unchanged. The audit confirmed it's not held across
  `commit_ext` or pre-commit / durability-wait. The one site where it's held
  across a state_machine call (`commit_conf` → `commit_config`) needs a
  lock-drop refactor (§13).
- All atomics, per-peer mutex, `cli_lock_`, `config_lock_`, etc.

## 6. Transport — folly::AsyncSocket on iomgr

The transport implements NuRaft's `rpc_client` and `rpc_listener` interfaces
over folly::AsyncSocket. Lives outside NuRaft — under
`src/homestore/replication/transport/`.

### 6.1 Wire framing

Length-prefixed flatbuffers (homestore already has flatbuffers in deps; the
data channel `push_data_rpc.fbs` and `fetch_data_rpc.fbs` use this pattern).
Frame:

```
[ uint32 frame_len ][ uint64 seq ][ uint8 msg_type ][ payload (flatbuffer) ]
```

`payload` for AppendEntries / response is a flatbuffer wrapping NuRaft's
`req_msg` / `resp_msg` serialized form (NuRaft's `buffer::pack` produces a
length-prefixed byte stream we can embed).

### 6.2 `FollyRpcClient` (one per peer)

```cpp
class FollyRpcClient : public nuraft::rpc_client,
                       public folly::AsyncReader::ReadCallback {
    folly::EventBase* eb_;            // = iomgr reactor for this group
    std::shared_ptr<folly::AsyncSocket> sock_;
    std::atomic<uint64_t> next_seq_{0};
    std::mutex pending_lock_;
    std::unordered_map<uint64_t, nuraft::rpc_handler> pending_;

    // NuRaft API:
    void send(ptr<req_msg>& req, rpc_handler& cb, uint64_t to_ms) override {
        uint64_t seq = next_seq_++;
        auto buf = frame(req, seq);
        { std::lock_guard l(pending_lock_); pending_[seq] = cb; }
        // optional: schedule a timeout via eb_->scheduleAt
        eb_->runInEventBaseThread([this, b = std::move(buf)]() mutable {
            sock_->writeChain(&dummy_wcb_, std::move(b));
        });
    }
    bool is_abandoned() const override { return !sock_ || !sock_->good(); }
    uint64_t get_id() const override { return reinterpret_cast<uintptr_t>(this); }

    // Folly read demux:
    void readDataAvailable(size_t n) noexcept override {
        accumulate(n);
        while (auto f = try_decode_frame()) {
            nuraft::rpc_handler cb;
            {
                std::lock_guard l(pending_lock_);
                auto it = pending_.find(f->seq);
                if (it != pending_.end()) {
                    cb = std::move(it->second);
                    pending_.erase(it);
                }
            }
            if (cb) cb(decode_resp(f->payload), nullptr);
            // ★ This call runs handle_append_entries_resp inline on this
            //   EventBase thread (= reactor thread). It updates peer state,
            //   may post commit_baton_.
        }
    }
    void readEOF() noexcept override { fail_all_pending(); }
    void readErr(...) noexcept override { fail_all_pending(); }
};
```

### 6.3 `FollyRpcListener` + `FollyRaftConnection`

```cpp
class FollyRpcListener : public nuraft::rpc_listener,
                         public folly::AsyncServerSocket::AcceptCallback {
    folly::SocketAddress addr_;
    std::shared_ptr<folly::AsyncServerSocket> server_;
    nuraft::ptr<nuraft::raft_server> handler_;
    folly::Synchronized<std::unordered_set<std::shared_ptr<FollyRaftConnection>>>
        conns_;

    void listen(nuraft::ptr<nuraft::msg_handler>& h) override {
        handler_ = h;
        auto* accept_eb = g_iomgr->reactor_for(0);
        server_ = folly::AsyncServerSocket::newSocket(accept_eb);
        server_->bind(addr_);
        server_->addAcceptCallback(this, accept_eb);
        server_->listen(128);
        server_->startAccepting();
    }
    void stop() override { server_->stopAccepting(); }

    void connectionAccepted(folly::NetworkSocket fd, ...) noexcept override {
        size_t rid = next_reactor_index_.fetch_add(1) % g_iomgr->num_reactors();
        // (Optionally hash by source addr or by group_id once known.)
        auto* eb = g_iomgr->reactor_for(rid);
        auto sock = folly::AsyncSocket::UniquePtr(
            new folly::AsyncSocket(eb, fd));
        auto conn = std::make_shared<FollyRaftConnection>(
            std::move(sock), handler_, g_iomgr->executor());
        conn->start();
        conns_.wlock()->insert(conn);
    }
};

class FollyRaftConnection : public folly::AsyncReader::ReadCallback {
    std::shared_ptr<folly::AsyncSocket> sock_;
    nuraft::ptr<nuraft::raft_server> handler_;
    folly::Executor::KeepAlive<> exec_;     // = iomgr executor
    folly::EventBase* eb_;                   // = sock_->getEventBase()

    void start() { sock_->setReadCB(this); }

    void readDataAvailable(size_t n) noexcept override {
        accumulate(n);
        while (auto f = try_decode_frame()) {
            auto req = decode_req(f->payload);
            uint64_t seq = f->seq;

            folly::coro::co_invoke(
                [this, self = shared_from_this(), req, seq]()
                    -> folly::coro::Task<void> {
                    // ★ NuRaft handler — Task<>-returning ★
                    ptr<resp_msg> resp =
                        co_await handler_->handle_append_entries(*req);
                    co_await send_resp(seq, std::move(resp));
                })
              .scheduleOn(exec_).start();
        }
    }

    folly::coro::Task<void> send_resp(uint64_t seq, ptr<resp_msg> resp) {
        auto buf = frame(resp, seq);
        folly::coro::Baton baton;
        struct WCb : folly::AsyncWriter::WriteCallback {
            folly::coro::Baton* b;
            void writeSuccess() noexcept override { b->post(); }
            void writeErr(size_t,
                          const folly::AsyncSocketException&) noexcept override {
                b->post();
            }
        } cb{&baton};

        if (eb_->isInEventBaseThread()) {
            sock_->writeChain(&cb, std::move(buf));
        } else {
            eb_->runInEventBaseThread([&] {
                sock_->writeChain(&cb, std::move(buf));
            });
        }
        co_await baton;
    }

    void readEOF() noexcept override { tear_down(); }
    void readErr(const folly::AsyncSocketException&) noexcept override {
        tear_down();
    }
};
```

### 6.4 io_uring + zero-copy

iomanager's `EventBaseManager` is constructed with the `IoUringBackend`
factory when `uring_opts` is set ([iomanager.cpp:41-46](src/iomanager/iomanager.cpp#L41-L46)).
That gives io_uring multiplexing for free — no transport-side changes.

Zero-copy via `AsyncSocket::setZeroCopy(true)` is a per-socket toggle. Default
**off** for control-plane RPCs (heartbeats, AE with small entry batches —
zero-copy overhead exceeds savings below ~10KB writes). Turn it **on** for
snapshot transfers. Wire it in `FollyRaftConnection` based on message type or
expected payload size.

### 6.5 Reconnection

`FollyRpcClient`'s connect failure or read EOF triggers reconnect with
backoff. Pending requests fail with `rpc_exception("disconnected")` so
NuRaft's `handle_append_entries_resp` sees the failure and retries via the
peer's normal retransmission logic. Backoff: 100ms, 200ms, 500ms, 1s, 2s
capped. ~50 LOC.

## 7. HomeStore-side changes

### 7.1 LogStore — durability observer hook (~10 LOC)

In [src/homestore/logstore/log_store.h](src/homestore/logstore/log_store.h):

```cpp
class LogStore : public LogStreamClient {
public:
    using durability_cb = std::function<void(lsn_t new_tail_lsn)>;
    void set_durability_observer(durability_cb cb) {
        durability_observer_ = std::move(cb);
    }

    void on_write_completion(lsn_t lsn, const stream_key& key) override {
        // ... existing tail_lsn_ advance ...
        if (durability_observer_) {
            durability_observer_(tail_lsn_.load(std::memory_order_acquire));
        }
    }

private:
    durability_cb durability_observer_;
};
```

The adapter sets this at startup. The callback fires inline within
`LogStream::flush()`'s coroutine (on a reactor thread).

### 7.2 log_store adapter

A NuRaft `log_store` impl wrapping homestore's `LogStore`. See §12 for the
method-by-method mapping.

### 7.3 RaftStateMachine — Task<> overrides

[src/homestore/replication/repl_dev/raft_state_machine.{h,cpp}](src/homestore/replication/repl_dev/raft_state_machine.h):

```cpp
class RaftStateMachine : public nuraft::state_machine {
    folly::coro::Task<raft_buf_ptr_t> commit_ext(
        const nuraft::state_machine::ext_op_params& params) override {
        int64_t lsn = static_cast<int64_t>(params.log_idx);
        repl_req_ptr_t rreq = lsn_to_req(lsn);
        if (m_rd.need_skip_processing(lsn)) co_return m_success_ptr;

        if (rreq->is_proposer()) {
            rreq->add_state(repl_req_state_t::LOG_FLUSHED);
        }
        co_await m_rd.handle_commit(rreq);    // handle_commit is now Task<>
        co_return m_success_ptr;
    }

    folly::coro::Task<raft_buf_ptr_t> pre_commit_ext(
        const nuraft::state_machine::ext_op_params& params) override {
        int64_t lsn = static_cast<int64_t>(params.log_idx);
        repl_req_ptr_t rreq = lsn_to_req(lsn);
        co_await m_rd.m_listener->on_pre_commit(
            rreq->lsn(), rreq->header(), rreq->key(), rreq);
        co_return m_success_ptr;
    }

    // commit_config, rollback_ext, snapshot APIs stay sync (no signature
    // change). Hopped via co_run_sync from commit_loop in the NuRaft fork.
};
```

### 7.4 ReplicationListener — Task<> callbacks

[src/include/homestore/replication/repl_dev.h](src/include/homestore/replication/repl_dev.h):
the `ReplDevListener` interface methods that participate in the hot path
(`on_pre_commit`, `on_commit`) become `Task<>`-returning.

```cpp
class ReplDevListener {
public:
    virtual folly::coro::Task<void> on_pre_commit(int64_t lsn,
                                                  sisl::Blob const& header,
                                                  sisl::Blob const& key,
                                                  cintrusive<repl_req_ctx>& rreq) = 0;
    virtual folly::coro::Task<void> on_commit(int64_t lsn,
                                              sisl::Blob const& header,
                                              sisl::Blob const& key,
                                              MultiBlkId const& blkid,
                                              cintrusive<repl_req_ctx>& rreq) = 0;
    // ... other callbacks (rollback, snapshot) stay sync per non-goals ...
};
```

### 7.5 RaftReplDev — propose path

[src/homestore/replication/repl_dev/raft_repl_dev.cpp](src/homestore/replication/repl_dev/raft_repl_dev.cpp):
the proposer path becomes `Task<>`:

```cpp
folly::coro::Task<ReplServiceError> RaftStateMachine::propose_to_raft(
    repl_req_ptr_t rreq) {
    rreq->create_journal_entry(true /* raft_buf */, m_rd.server_id());
    auto* vec = sisl::VectorPool<raft_buf_ptr_t>::alloc();
    vec->push_back(rreq->raft_journal_buf());

    auto result = co_await m_rd.raft_server()->append_entries(*vec);
    sisl::VectorPool<raft_buf_ptr_t>::free(vec);

    if (!result.accepted) {
        co_return RaftReplService::to_repl_error(result.code);
    }
    co_return ReplServiceError::OK;
}
```

### 7.6 sync_executor

A small folly::CPUThreadPoolExecutor (1–2 threads) created at HomeStore
startup, owned by the RaftReplService singleton. Passed into NuRaft's
`init_options`.

### 7.7 IOManager getter

[src/iomanager/iomanager.h](src/iomanager/iomanager.h) — add (if not already
present) a public accessor for the executor:

```cpp
folly::Executor::KeepAlive<> executor() const {
    return folly::Executor::KeepAlive<>(pool_.get());
}
```

## 8. Leader flow (end-to-end)

```
─────────────────────────────────────────────────────────────────────────────
T+0  [reactor A]    homestore coro:
                      co_await raft.append_entries(logs)
                      └─ Task<> wrapper:
                         · append_entries_internal (sync, on reactor A):
                            - lock_ briefly: validate, quick_append entries
                              (µs, in-memory queue), register cmd_result
                            - cr->accepted_ = true
                            - notify bg_append_ea_                       ──╮
                            - release lock_; return cr
                         · install cr->when_ready(post proposer_baton)    │
                         · co_await proposer_baton  ◄── SUSPENDED         │
                         [reactor A free; serves other groups' coros]    │
                                                                          │
T+1  [bg_append      wakes  ◄────────────────────────────────────────────╯
      pthread]       for each peer:
                       FollyRpcClient::send(req, when_done_cb, ...):
                         · seq++, pending_[seq] = when_done_cb
                         · serialize req → IOBuf
                         · peer.eb_->runInEventBaseThread([&]{
                              sock_->writeChain(buf);
                           });
                         · return immediately
                     bg_append moves to next peer; sleeps when done

T+2  [reactor X]    iomgr's auto-flush timer fires:
   (LogStream's       co_await LogStream::flush()  ← suspends on disk I/O
    flush coro)       on completion → inline:
                        for each record: LogStore::on_write_completion
                          · tail_lsn_ advances
                          · adapter durability_observer fires inline:
                            - cached_last_durable_index = tail_lsn
                            - raft->notify_log_append_completion()
                              · advance leader's matched_idx for self
                              · check quorum:
                                · single-node: post commit_baton  ─────╮
                                · multi-node: usually not yet           │
                                                                         │
T+3  [reactor Z]    peer resp arrives on its socket:                     │
   (peer socket's     FollyRpcClient::readDataAvailable:                 │
    EventBase)        · decode resp, pending_[seq] → when_done_cb        │
                      · cb(resp, nullptr) →                              │
                        handle_append_entries_resp runs HERE:            │
                        - update peer matched_idx                        │
                        - check quorum → post commit_baton  ─────────────┤
                                                                         │
T+4  [reactor]      commit_loop wakes  ◄────────────────────────────────╯
   (commit_loop      for sm_idx in (sm_commit+1 .. quick_commit):
    coro for           co_await sm->commit_ext(sm_idx)
    this group)        [SUSPENDED on homestore commit I/O;
                        reactor freed for other coros]
                       [resumes via folly continuation when commit_ext
                        Task<> co_returns]
                       sm_commit_index_ advances
                     scan_sm_commit_and_notify:
                       cr->set_result → when_ready callback fires
                       → proposer_baton.post  ─────────────────────────╮
                                                                        │
T+5  [reactor ?]    homestore coro resumes  ◄──────────────────────────╯
                    co_return result
─────────────────────────────────────────────────────────────────────────────
```

## 9. Follower flow (end-to-end)

### Phase 1 — receiving entries, making them durable

```
─────────────────────────────────────────────────────────────────────────────
T+0  [reactor X]    AsyncSocket::ReadCallback fires (req bytes arrive):
   (this conn's       · accumulate, decode req frame
    EventBase)        · co_invoke([=]() -> Task<void> {
                          auto resp = co_await raft.handle_append_entries(req);
                          co_await send_resp(seq, resp);
                        }).scheduleOn(iomgr->executor()).start();
                      · ReadCallback returns; reactor continues serving
                                                                    ──────╮
T+1  [reactor ?]    handle_ae_task runs:  ◄─────────────────────────────╯
                    - recur_lock(lock_) briefly:
                        · validate term, prev_log_idx
                        · for each entry: log_store->store_log_entry
                            → LogStore::quick_append (µs)
                        · advance precommit_index_
                      release lock_
                    - for each entry:
                        co_await sm->pre_commit_ext(idx, buf)
                        [SUSPENDED on homestore on_pre_commit Task<>;
                         reactor free]
                    - co_await durability_signal_.wait_until(last_log_idx)
                      [SUSPENDED; reactor free for other coros AND for
                       other handle_ae_tasks on same group — they
                       serialize on lock_ during validation but their
                       durability waits run concurrently]

T+2  [reactor]      iomgr flush coro: LogStream::flush() returns
                      LogStore::on_write_completion →
                        tail_lsn_ advances →
                        durability_observer fires →
                        durability_signal_.advance(tail_lsn) wakes
                        ALL waiters with target ≤ tail_lsn  ──────────╮
                                                                       │
T+3  [reactor ?]    handle_ae_task resumes  ◄────────────────────────╯
                    - build resp_msg(success, next_idx=last_log_idx+1)
                    - co_await send_resp(seq, resp_msg):
                        · serialize → IOBuf
                        · if eb_->isInEventBaseThread():
                            sock_->writeChain(&wcb, buf) directly
                          else:
                            eb_->runInEventBaseThread([&]{
                                sock_->writeChain(&wcb, buf);
                            });
                        · co_await write_baton (posted by writeSuccess)
                    - Task complete
─────────────────────────────────────────────────────────────────────────────
```

### Phase 2 — later commit when leader_commit_idx advances

```
T+10 [reactor X]    new AppendEntries arrives (heartbeat or entries) with
                    leader_commit_idx > sm_commit_index_
                    · schedule handle_ae_task as in Phase 1
                                                                  ────────╮
T+11 [reactor ?]    handle_ae_task: same Phase 1 cycle, plus  ◄──────────╯
                    · under lock_: if leader_commit_idx > quick_commit_idx_:
                        quick_commit_idx_ = min(leader_commit_idx,
                                                last_log_idx)
                        commit_baton_.post()  ─────────────────────╮
                    · co_await durability if entries present        │
                    · resp(success)                                 │
                                                                    │
T+12 [reactor ?]    commit_loop wakes  ◄───────────────────────────╯
   (commit_loop     for sm_idx in (sm_commit+1 .. quick_commit):
    coro)              co_await sm->commit_ext(sm_idx)
                       [SUSPENDED on homestore commit I/O]
                       sm_commit_index_ advances
```

## 10. NuRaft API seam — rpc_client and rpc_listener

NuRaft sees only its abstract interfaces. Our folly::AsyncSocket implementation
plugs in unchanged from NuRaft's perspective.

```
NuRaft view                                Our implementation
───────────                                ──────────────────
class rpc_client {                         class FollyRpcClient :
  virtual void send(                         public nuraft::rpc_client,
    ptr<req_msg>& req,                       public folly::AsyncReader::ReadCallback
    rpc_handler& when_done,                {
    uint64_t timeout_ms);                    // see §6.2
  virtual uint64_t get_id() const;         };
  virtual bool is_abandoned() const;
};

class rpc_listener {                       class FollyRpcListener :
  virtual void listen(                       public nuraft::rpc_listener,
    ptr<msg_handler>& handler);              public folly::AsyncServerSocket
  virtual void stop();                                  ::AcceptCallback
  virtual void shutdown();                 {
};                                           // see §6.3
                                           };
```

`msg_handler` is a typedef for `nuraft::raft_server`. The listener calls
`handler->handle_append_entries(req)` which now returns
`Task<ptr<resp_msg>>`. Our `FollyRaftConnection` co_awaits it inside its own
coroutine.

The proposer side (homestore → raft) doesn't go through `rpc_client` —
it's an in-process call to `raft_server::append_entries(...)`. The
peer-fanout side (raft leader → raft followers) is what uses
`rpc_client::send`.

## 11. Sync state_machine methods — sync_executor hop

Sync methods called from `commit_loop`: `commit_config`, `rollback_ext`,
`apply_snapshot`. These cannot become `Task<>` (kept sync per non-goals)
but must not wedge a reactor thread.

Helper, in `extern/NuRaft/include/libnuraft/coro_helpers.hxx`:

```cpp
template <typename Fn>
folly::coro::Task<std::invoke_result_t<Fn>>
co_run_sync(folly::Executor::KeepAlive<> sync_exec, Fn&& fn) {
    auto* main_exec = co_await folly::coro::co_current_executor;
    co_await folly::coro::co_reschedule_on_current_executor.via(sync_exec);
    if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
        std::forward<Fn>(fn)();
        co_await folly::coro::co_reschedule_on_current_executor.via(main_exec);
    } else {
        auto result = std::forward<Fn>(fn)();
        co_await folly::coro::co_reschedule_on_current_executor.via(main_exec);
        co_return result;
    }
}
```

(Verify the exact folly API names against `folly/2024.08.12.00`. The right
helper may be `co_invoke + via` or `co_reschedule_on_current_executor`. Pin
the chosen one in `coro_helpers.hxx`.)

Usage in `handle_commit.cxx` for `commit_config`:

```cpp
folly::coro::Task<void> raft_server::commit_one_conf(
    ulong sm_idx, ptr<log_entry>& le) {
    ptr<cluster_config> new_conf;
    {
        recur_lock(lock_);
        new_conf = parse_conf(le);
        update_peer_list_locked(new_conf);
    } // lock_ released BEFORE co_await — see §13

    co_await co_run_sync(sync_executor_, [&]() {
        state_machine_->commit_config(sm_idx, new_conf);
    });

    // Re-validate post-await if any final state update is needed:
    {
        recur_lock(lock_);
        // role/term checks, finalize state
    }
}
```

Same pattern for `apply_snapshot` and `rollback_ext`.

## 12. log_store adapter

NuRaft's `log_store` interface is sync. Homestore's `LogStore` exposes a sync,
lock-free queue API for the hot path (`quick_append`) and coro APIs for
durability (`flush`, `flush_upto`) and reads (`read`). The adapter bridges:

| NuRaft `log_store` method | Adapter implementation | Threading note |
|---|---|---|
| `store_log_entry(entry, idx)` | `homestore_ls->quick_append(blob)` | sync, lock-free, µs CPU. Safe on reactor thread. |
| `next_slot()` / `start_index()` / `last_entry()` | read cached members updated via observer | sync, no I/O |
| `entry_at(idx)` / `term_at(idx)` | `folly::coro::blockingWait(homestore_ls->read(idx))` | **blocks calling thread**. Caller is bg_append (own pthread) / sync_executor / RPC threads on the snapshot path — never a reactor running a hot-path coro. |
| `last_durable_index()` | read cached atomic, updated by `durability_observer` | sync, no I/O |
| `flush()` (rare; from snapshot create) | `folly::coro::blockingWait(homestore_ls->flush())` | called from sync state_machine methods which are already on `sync_executor`. ✓ |
| `compact(idx)` | `folly::coro::blockingWait(homestore_ls->truncate(idx))` | same as flush |

**Where reactor threads call into the adapter:**

- Leader proposer Task<> on reactor → `append_entries_internal` →
  `log_store->store_log_entry` → `quick_append` (µs). ✓ No blocking.
- Follower `handle_ae_task` on reactor → `log_store->store_log_entry` →
  `quick_append` (µs). ✓ No blocking.
- `commit_loop` on reactor → `commit_in_bg_exec` may receive entry buffer from
  `commit_ret_elems_` (the pending-commit map) without re-reading log_store.
  **Verify during execution (audit §14)** — if a read is needed, hop via
  `co_run_sync(sync_executor_, ...)`.

**Where reactor threads do NOT call log_store reads:**
- Proposer Task<> only writes. ✓
- Follower handle_ae_task only writes. ✓
- bg_append_thread is its own pthread (acceptable to block on disk reads). ✓
- Snapshot paths run on sync_executor. ✓

The "sync but lock-free" nature of `quick_append` is what makes the hot-path
sync-by-default decision correct — there is no impedance mismatch on writes.
The impedance only appears on reads, and reads aren't on the hot reactor path.

## 13. Locking discipline

**The single rule:** no `std::mutex` / `std::recursive_mutex` held across any
`co_await`. Atomics are fine. `folly::coro::Mutex` only if a critical section
genuinely must span a suspension (not needed in this scope).

Specifically:

- `lock_` (recursive_mutex, per raft_server) protects log mutations and peer
  state. It is taken in brief CPU-only critical sections. Today's audit
  confirms it is **not** held across `commit_ext`, `pre_commit_ext`, or the
  follower durability wait. ✓
- `lock_` IS held across the (sync) `commit_config` call inside `commit_conf`
  today. The §11 lock-drop pattern releases `lock_` before the
  `co_run_sync` hop and re-acquires it after for any post-commit state work.
  ~80–150 LOC for the refactor including invariant re-check.
- `commit_loop`'s LSN loop must not hold `lock_` across `co_await
  sm->commit_ext`. Verified by the audit.
- `handle_ae_task` must not hold `lock_` across `co_await
  sm->pre_commit_ext` or `co_await durability_signal_.wait_until(...)`.
  Verified by the audit (lock_ released before the durability wait today;
  pre_commit must be moved out of any held-lock region during the conversion).
- Per-peer mutex (`p->get_lock()`) — taken briefly for next/matched index
  updates. Stays as `std::mutex`. Never spans a `co_await`.

`folly::coro::Mutex` is not introduced. If a future change requires holding
state across a suspension, that's where it would land — but every minimal-scope
caller can structure its critical sections to release before suspension.

## 14. Audits to do during execution

1. **`cmd_result::when_ready` install-after-set race.** Read
   [include/libnuraft/async.hxx](https://github.com/eBay/NuRaft/blob/master/include/libnuraft/async.hxx).
   If `when_ready` does not auto-fire when `has_result_` is already true, the
   `append_entries` Task<> wrapper must check `has_result_` after installing
   and synthesize the handler call. Patch `cmd_result` to be
   install-or-fire-atomic if needed. (Race is real; install ordering matters.)

2. **`cmd_result` rejection-without-result-set case.** Confirm what happens
   when the leader rejects up front (`accepted_=false`) and `set_result` is
   never called. The wrapper must bail out via the `!cr->get_accepted()`
   check before installing the handler — verify this is a sufficient guard.

3. **`p->send_req` lock state in `request_append_entries`.** Doesn't matter
   for minimal scope (we're not coro-izing leader fanout) but worth
   confirming so the constraint is documented for the eventual fanout
   coro-ization.

4. **`init_options` shape.** Confirm where the two executors fit cleanly.
   Existing callers must not break — provide defaults (e.g., a global
   default executor) for backwards compat in case any other consumers exist.

5. **folly executor switching idiom.** Pin the exact API in
   `coro_helpers.hxx`. Candidates: `folly::coro::co_invoke`,
   `co_reschedule_on_current_executor.via`, `co_via`. Verify against
   `folly/2024.08.12.00`.

6. **`commit_in_bg_exec` log_store read.** Confirm whether commit_loop reads
   from log_store on the reactor thread (i.e., whether the entry buffer is
   fetched from `commit_ret_elems_` map vs. via `log_store->entry_at(idx)`).
   If the latter, add `co_run_sync(sync_executor_, ...)` for that read.

7. **Multi-waiter `durability_signal_` mutex contention.** Profile under
   load. If `mtx_` becomes a bottleneck, partition by group_id.

8. **NuRaft submodule build wiring.** Verify the fork's CMakeLists.txt picks
   up conan-provided boost/openssl rather than fetching its own. Patch the
   fork if needed.

## 15. LOC estimate

| Block | Files | LOC |
|---|---|---|
| **NuRaft fork delta** (in `extern/NuRaft`) |   |   |
| `state_machine` virtuals → Task<> + call sites | state_machine.hxx, handle_commit.cxx, handle_append_entries.cxx | ~80 |
| `bg_commit_thread_` → `commit_loop` coroutine | raft_server.{hxx,cxx} | ~200 |
| `handle_append_entries` → Task<> | raft_server.hxx, handle_append_entries.cxx | ~150 |
| `append_entries` / `_async` Task<> wrappers | raft_server.{hxx,cxx} + append_result.hxx | ~120 |
| Multi-waiter `durability_signal_` primitive + tests | durability_signal.hxx, .cxx, tests | ~150 |
| `co_run_sync` helper | coro_helpers.hxx | ~40 |
| Sync hops at commit_config / apply_snapshot / rollback_ext + commit_conf lock-drop | handle_commit.cxx | ~150 |
| Executor injection through init_options | raft_server.{hxx,cxx} + init_options | ~50 |
| `delayed_task_scheduler` shim → folly EventBase timer | scheduler.{hxx,cxx} | ~80 |
| **Subtotal NuRaft fork** |   | **~1020** |
| **Transport** (under `src/homestore/replication/transport/`) |   |   |
| `FollyRpcClient` + framing + reconnection | folly_rpc_client.{h,cpp} | ~400 |
| `FollyRpcListener` + `FollyRaftConnection` | folly_rpc_listener.{h,cpp}, folly_raft_connection.{h,cpp} | ~400 |
| Wire framing + flatbuffers schema | raft_rpc.fbs, framing.h | ~100 |
| **Subtotal transport** |   | **~900** |
| **HomeStore-side adapters** |   |   |
| `LogStore::set_durability_observer` hook | log_store.h, log_store.cpp | ~10 |
| NuRaft `log_store` adapter | nuraft_log_store_adapter.{h,cpp} | ~200 |
| `RaftStateMachine` Task<> overrides | raft_state_machine.{h,cpp} | ~80 |
| `ReplDevListener` interface → Task<> + impl updates | repl_dev.h + listener impls | ~60 |
| `RaftReplDev::propose_to_raft` → Task<> | raft_repl_dev.cpp | ~30 |
| `sync_executor` creation + injection | raft_repl_service.{h,cpp} | ~30 |
| IOManager executor accessor (if not present) | iomanager.h | ~5 |
| **Subtotal homestore-side** |   | **~415** |
| **Build system** |   |   |
| Submodule wiring + CMake glue | CMakeLists.txt, .gitmodules | ~30 |
| Re-enable replication build | src/CMakeLists.txt | ~5 |
| **Subtotal build** |   | **~35** |
| **Total** |   | **~2370** |

This is the realistic number for a coro-clean implementation with the full
follower path included. Plus tests (acceptance + unit) — another ~1000 LOC.

If we cut Option B (full follower coro path) and stay with the leader-only
minimal scope, the NuRaft fork delta drops to ~300 LOC and the transport
becomes optional (could keep using NuRaft's built-in asio_service initially).
But per the discussion, the follower's flush wait is the load-bearing perf
concern; full follower coro is the right design.

## 16. Acceptance tests

1. **Cooperative multitasking, hot path:** 100 raft groups on a 4-thread
   iomgr. Drive concurrent proposals on all groups. Verify:
   - No group's commit is starved while another's commit_ext is suspended.
   - Sample reactor thread IDs while group A's `commit_ext` is suspended on
     I/O — group B's commit_loop must be running on one of those threads.

2. **Sync hop isolation:** Trigger an `apply_snapshot` on one group while
   nine other groups are committing on the same iomgr. Verify:
   - `cp_flush().wait()` runs on `sync_executor`, not on a reactor.
   - The other nine groups' commit_loops continue uninterrupted (sample
     wall-clock latency before/during/after the snapshot).

3. **Proposer await-commit:** `co_await raft.append_entries(logs)`. Verify:
   - Returns only after commit (not just accept).
   - Returns `rejected` cleanly if the proposer is no longer leader.
   - No coroutine hangs on commit failure.

4. **Proposer await-accept:** `co_await raft.append_entries_async(logs)`.
   Verify:
   - Returns immediately after leader-side log append (synchronous in leader
     case).
   - Subsequent `pre_commit` and `commit` Task<>s still fire on all replicas.

5. **Rejected-from-start:** Disable the leader; issue `append_entries` on a
   follower; verify the Task<> resolves with rejected, not hangs.

6. **`when_ready` install race:** Stress-test with single-node group (very
   fast commit). Verify no commits are missed by the proposer wrapper.

7. **Follower concurrent in-flight RPCs:** Drive a follower with multiple
   pipelined AppendEntries (different log ranges). Verify:
   - Multiple `handle_ae_task` coros run concurrently.
   - All eventually respond correctly with monotonic next_idx values.
   - lock_ contention on the validation phase doesn't cause wedges.

8. **Heartbeat during durability wait:** Send heartbeat to follower while
   prior AppendEntries is mid-durability-wait. Verify:
   - Heartbeat response goes back immediately reporting current durable
     next_idx, not the in-memory (yet-to-be-durable) one.
   - Earlier RPC's response goes back later with higher next_idx.
   - Leader handles out-of-order responses correctly.

9. **Multi-waiter `durability_signal_` correctness:** Many concurrent waiters
   for various target indices. Verify all waiters with `target ≤ advance(N)`
   are woken; none of those with `target > N` are.

10. **io_uring + zero-copy snapshot:** Configure iomgr with io_uring backend,
    enable zero-copy on snapshot send. Verify a large snapshot transfer
    succeeds and runs at the expected throughput.

11. **Reconnection:** Kill a peer mid-session; verify FollyRpcClient
    reconnects with backoff and pending requests fail with timeout, not hang.

## 17. Out-of-scope follow-ups

If perf measurements after this work show pain points, the natural next
extensions in increasing cost:

1. **Coro-ize leader peer fanout.** Replace bg_append_thread with a
   coroutine using `folly::coro::collectAll` over peer sends. Eliminates a
   pthread per group. ~200 LOC + `peer::send_req` getting a Task<> overload.

2. **Coro-ize all state_machine virtuals.** Move `commit_config`,
   `apply_snapshot`, etc. from sync to Task<>. Removes the sync_executor
   sidecar but expands the lock-drop refactor surface significantly.

3. **Coro-ize log_store / state_mgr virtuals end-to-end.** Eliminates the
   `blockingWait` reads on bg_append_thread and snapshot paths. Useful only
   if those paths' parking becomes measurable contention.

4. **Replace `delayed_task_scheduler` with folly HHWheelTimer directly.**
   Cleaner integration; minor change.

5. **Multi-group transport multiplexing.** Today each peer connection is
   per-group. If group count gets very high (thousands), multiplex many
   groups over one TCP connection per peer. ~500 LOC for the framing
   extension.

None of these are needed to satisfy the original requirements.
