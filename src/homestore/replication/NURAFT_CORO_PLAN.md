# NuRaft Coroutine-Readiness Plan

Status: scoping complete, ready to execute.
Owner: TBD.
Scope: a fork of [eBay/NuRaft](https://github.com/eBay/NuRaft) (master) + small adapter changes in `homestore/replication/`.

## 1. Goal

Make NuRaft's two hot paths coroutine-friendly so that homestore can drive them
via `folly::coro::Task` without blocking OS threads on application-level I/O.

The hot paths:

- **HomeStore → Raft (proposer):** `co_await raft_server->append_entries(logs)` /
  `append_entries_async(logs)`.
- **Raft → HomeStore (apply):** `co_await sm->commit_ext(idx)` /
  `co_await sm->pre_commit_ext(idx)`.

Target threading model: N raft groups share an M-thread folly executor (M ≪ N).
While group A's commit_loop is suspended on its `commit_ext` Task<>, group B's
commit_loop runs on the same OS thread.

## 2. Non-goals (what stays sync)

Everything else in NuRaft stays as it is today:

- `bg_append_thread_` stays a `std::thread`. Leader fanout stays callback-based.
- Follower's `handle_append_entries` stays sync, returns `ptr<resp_msg>` directly.
  `ea_follower_log_append_` stays as `EventAwaiter`.
- `log_store` virtuals stay sync.
- `state_mgr` virtuals stay sync.
- All snapshot APIs (`create_snapshot`, `read_logical_snp_obj`,
  `save_logical_snp_obj`, `apply_snapshot`, `last_snapshot`,
  `free_user_snp_ctx`) stay sync.
- `state_machine::rollback_ext`, `commit_config`, `rollback_config` stay sync.
- `rpc_client::send` stays callback. `rpc_listener` stays unchanged. Transport
  (asio_service or future grpc) untouched.
- `recursive_mutex lock_` stays as `std::recursive_mutex`. No flattening.
- `cli_lock_`, `config_lock_`, peer per-mutex, all atomics — unchanged.

## 3. Target architecture

```
                          folly::Executor (M threads, M ≪ N)
                          ─────────────────────────────────
         ┌──────────────────┐  ┌──────────────────┐
group A: │ commit_loop coro │  │ append_entries   │
         │  co_await        │  │  Task<> (caller  │
         │   commit_ext     │  │  is homestore)   │
         └──────────────────┘  └──────────────────┘
         ┌──────────────────┐
group B: │ commit_loop coro │   ... up to N groups
         └──────────────────┘

                   bg_append_thread_  (still a pthread, unchanged)
                   RPC threads        (still pthreads, unchanged)

                          sync_executor (folly::CPUThreadPool, ~1-2 threads)
                          ─────────────────────────────────
                          for occasional sync calls:
                            - state_machine::commit_config
                            - state_machine::apply_snapshot
                            - state_machine::rollback_ext
                            - any other sync state_machine call hit by commit_loop
```

When commit_loop hits an LSN that maps to a sync method, it hops the call to
`sync_executor` and `co_await`s the result. This releases the main executor
thread for other groups while the sync call runs (e.g., during
`cp_flush().wait()` inside `save_logical_snp_obj` / `apply_snapshot`).

## 4. Changes — ordered

### Step 1 — Plumbing: inject executor through `init_options`

NuRaft side:
- Add `folly::Executor::KeepAlive<> main_executor_` and
  `folly::Executor::KeepAlive<> sync_executor_` fields to `init_options` (or to
  `raft_server::init_options`, whichever is the actual constructor input).
- `raft_server` ctor stores them as members.
- `sync_executor_` defaults to a small CPU thread pool created internally if
  caller doesn't provide one.

HomeStore side:
- `RaftReplDev` (or wherever `raft_server` is constructed) passes its folly
  executor / iomgr executor through.

### Step 2 — Replace `bg_commit_thread_` with `commit_loop` coroutine

NuRaft side, `raft_server.{hxx,cxx}` + `handle_commit.cxx`:

- Delete `bg_commit_thread_` (`std::thread`), `commit_cv_`
  (`std::condition_variable`), `commit_cv_lock_` (`std::mutex`).
- Add `folly::coro::Baton commit_baton_`.
- Replace `commit_in_bg()` body with a coroutine:

  ```cpp
  folly::coro::Task<void> raft_server::commit_loop() {
      while (!stopping_.load()) {
          co_await commit_baton_;
          co_await commit_in_bg_exec();
      }
  }
  ```

- `commit()` (called from RPC threads to advance `quick_commit_index_`) becomes
  `commit_baton_.post()` instead of `commit_cv_.notify_one()`.
- Server start: schedule `commit_loop().scheduleOn(main_executor_).start()`.
- Server stop: set `stopping_`, post the baton once to unblock.
- `commit_in_bg_exec()` becomes `folly::coro::Task<void>`. Its inner loop
  (`for sm_idx in (sm_commit_index_+1 .. quick_commit_index_)`) becomes a
  sequential `co_await` over each LSN's `commit_ext`.
- Keep `commit_lock_` (the try_lock pattern) as `std::mutex` — never held across
  `co_await`.

### Step 3 — `commit_ext` and `pre_commit_ext` → `Task<>`

NuRaft side, `include/libnuraft/state_machine.hxx`:

```cpp
// was:
virtual ptr<buffer> commit_ext(const ext_op_params& params);
// becomes:
virtual folly::coro::Task<ptr<buffer>> commit_ext(const ext_op_params& params) = 0;

// same for pre_commit_ext
```

NuRaft call sites in `handle_commit.cxx`:

```cpp
ret_value = co_await state_machine_->commit_ext(
                state_machine::ext_op_params(sm_idx, buf));
```

HomeStore side, `repl_dev/raft_state_machine.{h,cpp}`:

- Change `RaftStateMachine::commit_ext` and `pre_commit_ext` signatures to
  return `folly::coro::Task<raft_buf_ptr_t>`.
- `m_rd.handle_commit(rreq)` becomes `co_await m_rd.handle_commit(rreq)`. This
  may require `handle_commit` itself to be `Task<>` — likely already needs to be
  since it does I/O.
- `m_rd.m_listener->on_pre_commit(...)` likewise — listener interface for
  `on_pre_commit` and `on_commit` becomes `Task<>`-returning.

### Step 4 — `raft_server::append_entries` becomes `Task<>`

NuRaft side, `raft_server.{hxx,cxx}`:

Replace the existing public:

```cpp
ptr<cmd_result<ptr<buffer>>> append_entries(const std::vector<ptr<buffer>>& logs);
```

with:

```cpp
folly::coro::Task<append_result> append_entries(
    const std::vector<ptr<buffer>>& logs);

folly::coro::Task<append_result> append_entries_async(
    const std::vector<ptr<buffer>>& logs);
```

Internal: keep the `cmd_result`-producing implementation as
`append_entries_internal(...)`. The two coroutine entries wrap it.

`append_entries_async` (await accept):

```cpp
auto cr = append_entries_internal(logs);
if (!cr) co_return rejected();   // bailout if internal returned null
co_return cr->get_accepted() ? accepted(cr) : rejected();
```

`append_entries` (await commit):

```cpp
auto cr = append_entries_internal(logs);
if (!cr || !cr->get_accepted()) co_return rejected();

folly::coro::Baton baton;
append_result captured;
cr->when_ready([&](auto& v, auto& e) {
    captured = make_result(v, e);
    baton.post();
});
// AUDIT: if when_ready does NOT auto-fire when has_result_ already true,
// check cr->has_result() here and synthesize the call ourselves.
co_await baton;
co_return captured;
```

`append_result` is a small struct with `accepted` flag, `committed` flag, value
buffer, error code. Define in `include/libnuraft/append_result.hxx`.

### Step 5 — Sync-executor hop helper

NuRaft side, new file `include/libnuraft/coro_helpers.hxx`:

```cpp
namespace nuraft {

// Run a synchronous callable on sync_executor, await the result on main executor.
template <typename Fn>
folly::coro::Task<std::invoke_result_t<Fn>>
co_run_sync(folly::Executor::KeepAlive<> sync_exec, Fn&& fn) {
    auto* main_exec = co_await folly::coro::co_current_executor;
    co_await folly::coro::co_reschedule_on_current_executor.via(sync_exec);
    auto result = std::forward<Fn>(fn)();
    co_await folly::coro::co_reschedule_on_current_executor.via(main_exec);
    co_return result;
}

}  // namespace nuraft
```

(Exact folly API may need adjustment — folly has `co_invoke_on`,
`co_via`, `co_reschedule_on_current_executor`. Pick whichever gives the
"hop, run sync, hop back" semantics cleanly. Verify in folly version pinned in
conanfile.py: `folly/2024.08.12.00`.)

### Step 6 — Wire sync hops in commit_loop for sync state_machine calls

NuRaft side, `handle_commit.cxx` and friends:

For `commit_config` (called from `commit_conf`):

```cpp
co_await co_run_sync(sync_executor_, [&]() {
    state_machine_->commit_config(idx_to_commit, new_conf);
});
```

For `apply_snapshot`:

```cpp
bool ok = co_await co_run_sync(sync_executor_, [&]() {
    return state_machine_->apply_snapshot(s);
});
```

For `rollback_ext`:

```cpp
co_await co_run_sync(sync_executor_, [&]() {
    state_machine_->rollback_ext(params);
});
```

The sites that take `lock_` before these calls keep their `recur_lock(lock_)`
exactly as today — the lock is held while `co_run_sync` is in flight. The sync
call runs on `sync_executor_` while the main executor thread is freed. **This is
safe because nothing on the main executor tries to acquire that group's `lock_`
during the suspension** — well-formed callers serialize through the same
`commit_loop` coroutine.

(See §6 "Locking discipline" for the proof obligation.)

### Step 7 — HomeStore-side wiring

`repl_dev/raft_repl_dev.cpp` and `repl_dev/raft_state_machine.cpp`:

- Pass executor through to `nuraft::raft_server::init_options`.
- Convert `propose_to_raft` to `Task<>`:

  ```cpp
  folly::coro::Task<ReplServiceError> RaftStateMachine::propose_to_raft(
      repl_req_ptr_t rreq) {
      // ...
      auto result = co_await m_rd.raft_server()->append_entries(*vec);
      // ...
  }
  ```

- Update `commit_ext` / `pre_commit_ext` overrides to be `Task<>`-returning, as
  described in Step 3.

## 5. Sync-executor hop pattern (canonical)

```cpp
// Inside commit_loop, processing an LSN that maps to a sync state_machine method:

folly::coro::Task<void> commit_one(ulong sm_idx, ptr<log_entry> le) {
    if (le->get_val_type() == log_val_type::app_log) {
        // hot path: real coroutine
        co_await state_machine_->commit_ext(ext_op_params(sm_idx, le->get_buf_ptr()));
    } else if (le->get_val_type() == log_val_type::conf) {
        // cold path: sync hop
        recur_lock(lock_);  // OK — no co_await while held
        ptr<cluster_config> new_conf = parse_conf(le);
        update_peer_list_locked(new_conf);
        co_await co_run_sync(sync_executor_, [&]() {
            state_machine_->commit_config(sm_idx, new_conf);
        });
        // lock_ released here when recur_lock guard goes out of scope
        // (BUT: see locking-discipline below — guard goes out of scope at end
        // of containing block, which contains co_await. So actual code must
        // structure the lock release before the co_await. See §6.)
    }
    co_return;
}
```

**Real shape with correct lock release:**

```cpp
folly::coro::Task<void> commit_one_conf(ulong sm_idx, ptr<log_entry> le) {
    ptr<cluster_config> new_conf;
    {
        recur_lock(lock_);
        new_conf = parse_conf(le);
        update_peer_list_locked(new_conf);
    } // lock_ released here, BEFORE co_await

    co_await co_run_sync(sync_executor_, [&]() {
        state_machine_->commit_config(sm_idx, new_conf);
    });

    // post-await: re-take lock if any final state update needs it
    {
        recur_lock(lock_);
        // re-validate term/role if needed; finalize state
    }
}
```

This is the lock-drop / re-validate pattern from the audit. It's the only place
in the minimal scope where it applies, because it's the only sync call site
where `lock_` was previously held across the call.

## 6. Locking discipline

**The single rule:** no `std::mutex` / `std::recursive_mutex` held across any
`co_await`. Atomics are fine. `folly::coro::Mutex` only if a critical section
genuinely must span a suspension (none in this scope).

Specifically:

- `commit_loop`'s body must not hold `lock_` across `co_await commit_ext(...)`.
  Verified: today's `commit_in_bg_exec` does NOT hold `lock_` across the
  `commit_ext` call. ✓
- `commit_one_conf` (above) must release `lock_` before
  `co_await co_run_sync(...)`. The pattern in §5 enforces this.
- `append_entries` (proposer) Task<> — `append_entries_internal` may take
  `lock_` to log-append, but releases it before returning. The Task<> awaits on
  the baton, not while holding any lock. ✓

`co_run_sync` does NOT need `lock_` released, as long as the caller doesn't
need any awaiting coroutine on the main executor to take that same `lock_`
during the suspension. For the commit path this is structurally true (single
commit_loop per group), but it's a proof obligation each caller must satisfy.

## 7. Audits to do during execution

These are the items I deferred during scoping; they should be done as
prerequisites or early in the work:

1. **`cmd_result::when_ready` install-after-set race.** Read
   `include/libnuraft/async.hxx`. If `when_ready` does not auto-fire when
   `has_result_` is already true, the `append_entries` Task<> wrapper must
   check `has_result_` after installing and synthesize the handler call. Patch
   `cmd_result` to be install-or-fire-atomic if needed.

2. **`log_store::store_log_entry` blocking?** Verify the existing log_store
   impl returns quickly (queues durably-async). If it blocks for disk I/O,
   `append_entries_internal` will block the executor thread when called from
   the proposer Task<>. Mitigation: hop the log_store append through
   `sync_executor` too. ~30 LOC if needed.

3. **`p->send_req` lock state in `request_append_entries`.** Doesn't matter for
   minimal scope (we're not coro-izing leader fanout) but worth confirming so
   the constraint is documented.

4. **`init_options` shape.** Confirm where to inject the two executors
   cleanly without breaking existing callers.

5. **folly executor switching idiom.** Confirm the right folly API for the
   "hop to executor X, run blocking call, hop back to original executor"
   pattern. Candidates: `folly::coro::co_invoke`, `co_reschedule_on_current_executor.via`,
   `co_via`. Pin the chosen one in `coro_helpers.hxx`.

## 8. LOC estimate

| Step | LOC |
|---|---|
| 1. Executor injection through init_options | ~30 |
| 2. `bg_commit_thread_` → `commit_loop` coroutine | ~150 |
| 3. `commit_ext` / `pre_commit_ext` → Task<> | ~50 |
| 4. `append_entries` / `append_entries_async` Task<> wrappers | ~80 |
| 5. `coro_helpers.hxx` (`co_run_sync`) | ~30 |
| 6. Sync hops at commit_config / apply_snapshot / rollback_ext sites + commit_conf lock-drop | ~80 |
| 7. HomeStore-side wiring (state_machine overrides, propose_to_raft) | ~50 |

**~470 LOC fork delta in NuRaft + adapter changes.** Higher than the earlier
~350 estimate because it includes the conscientious lock-drop pattern in
`commit_conf` and the homestore-side wiring.

## 9. Acceptance tests

1. **Cooperative multitasking, hot path:** 100 raft groups on a 4-thread
   executor. Drive concurrent proposals on all groups. Verify:
   - No group's commit is starved while another's is running.
   - Sample executor thread IDs while group A's `commit_ext` is suspended on
     I/O — group B's commit_loop must be running on one of those threads.

2. **Sync hop isolation:** Trigger an `apply_snapshot` on one group while
   nine other groups are committing on the same executor. Verify:
   - `cp_flush().wait()` runs on `sync_executor`, not on the main executor.
   - The other nine groups' commits are not blocked during the snapshot apply.

3. **Proposer await-commit:** Issue `co_await append_entries(logs)`. Verify:
   - Returns only after commit (not just accept).
   - No coroutine hangs if commit fails.

4. **Proposer await-accept:** Issue `co_await append_entries_async(logs)`.
   Verify:
   - Returns immediately after leader-side log append (synchronous in the
     leader case).
   - Subsequent `pre_commit` and `commit` Task<>s still fire on all replicas.

5. **Rejected-from-start:** Disable the leader; issue `append_entries` on a
   follower; verify the Task<> resolves with rejected, not hangs.

6. **`when_ready` install race:** Stress-test with very fast commit (in-memory
   state machine, single-node group). Verify no commits are missed by the
   wrapper.

## 10. Out-of-scope follow-ups

If perf measurements after this work shows pain points, the natural next
extensions (in increasing cost):

1. Coro-ize per-peer fanout in `request_append_entries` for `collectAll`.
2. Coro-ize follower's `handle_append_entries` (requires multi-waiter
   primitive for `ea_follower_log_append_` and rpc_listener interface change).
3. Coro-ize log_store / state_mgr virtuals end-to-end.
4. Replace `delayed_task_scheduler` with folly HHWheelTimer.

None of these are needed to satisfy the original requirements.
