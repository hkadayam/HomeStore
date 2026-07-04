# NuRaft Async Cascade — Resume Notes

Snapshot to resume the multi-phase refactor that makes the whole nuraft stack coroutine-safe end-to-end.

---

## Goal

Convert nuraft (fork under `extern/NuRaft/`) so every method that may perform I/O returns a coroutine
(`AsyncTask<T>` in nuraft terms; `folly::coro::Task<T>` under the covers). Eliminate every path where a
reactor thread could block waiting for a coroutine. Preserve the ability to compile nuraft in a
non-coroutine "sync mode" via a compat layer so the fork stays usable by pre-coro consumers.

Downstream: our HomeStore `ReplicaSet` overrides state_machine / state_mgr / log_store interfaces and
plugs into the FollyRpcListener/FollyRpcClient transport. The current homestore-side design pins each
raft_server to one iomgr reactor event base; all its Tasks default to the same event base via folly's
executor stickiness.

---

## Design decisions locked in

1. **Compat layer** at `extern/NuRaft/include/libnuraft/async_compat.hxx`. Under `NURAFT_ASYNC_MODE`,
   `AsyncTask<T> = folly::coro::Task<T>` and `Baton` wraps `folly::coro::Baton`. Otherwise
   `AsyncTask<T> = T` (identity) and `Baton` wraps `EventAwaiter` (no folly needed for sync). Macros:
   `ASYNC_AWAIT(expr)`, `ASYNC_RETURN(x)`, `ASYNC_RETURN_VOID`.
2. **Type name** is `nuraft::AsyncTask<T>` (not `Task<T>` — reduces collisions with app code).
3. **Interface break in state_machine**: `create_snapshot(snapshot&, handler_type&)` becomes
   `AsyncTask<bool> create_snapshot(snapshot const&)`. Drops the async-result-handler arg.
4. **init_options rename**: `sync_executor_` → `slow_executor_` (opt-in isolation for CPU-heavy work like
   snapshot streaming; nobody uses it by default).
5. **init_options default flip**: `start_server_in_constructor_` default `true → false`. If left true,
   raft_server ctor co_invokes start_server on `main_executor_` (fire-and-forget) for legacy semantics.
6. **`rpc_handler` typedef** → `std::function<AsyncTask<void>(ptr<resp_msg>&, ptr<rpc_exception>&)>`.
7. **bg_append_thread retirement**: originally planned for N3; done in N2. `std::thread bg_append_thread_`
   is gone. `bg_append_baton_` (nuraft::Baton) replaces the EventAwaiter. append_entries_in_bg becomes
   a coroutine on `main_executor_`; its SemiFuture is stored in `append_bg_future_` for shutdown.
8. **`snapshot_io_mgr`** still uses `io_thread_` (std::thread) — remaining N3 work.
9. **global_mgr worker threads** kept sync; they blocking-wait AsyncTask via `folly::coro::blockingWait`
   guarded by `#ifdef NURAFT_ASYNC_MODE`. Legit since global_mgr worker threads are dedicated (not
   reactors). We don't use global_mgr in our HomeStore integration.
10. **`slow_executor_`** still constructed in `ReplicationManager` and injected into
    `raft_server::init_options`. Reserved for future CPU-hop use (snapshot_io_mgr streaming); nobody
    else uses it by default. Not required for correctness now that everything is Task.
11. **Mutex refactor** is a separate phase (N2.5) between N2 and N3 — see "Open concerns" below.

## Sync methods retained (no cascade)

Stayed sync because they don't do I/O:

- **state_machine**: `get_next_batch_size_hint_in_bytes`, `last_snapshot`, `last_commit_index`,
  `free_user_snp_ctx`, `chk_create_snapshot`, `allow_leadership_transfer`, `adjust_commit_index`.
- **state_mgr**: `load_log_store` (returns a stored ptr), `server_id`, `system_exit`.
- **log_store**: `next_slot`, `start_index`, `last_entry`, `end_of_append_batch`, `log_entries`,
  `log_entries_ext`, `pack`, `last_durable_index`, `quick_append` (µs in-memory enqueue).

## AsyncTask methods added / converted

Every method that reaches state_machine / state_mgr / log_store I/O, plus every ancestor in the call
graph that awaits one. Full list is in the header decl updates (see `raft_server.hxx` + interface headers).

---

## Files touched — snapshot

### Complete (async-mode compilable)

- `extern/NuRaft/include/libnuraft/async_compat.hxx` — **new**
- `extern/NuRaft/include/libnuraft/state_mgr.hxx` — 4 methods AsyncTask, include compat
- `extern/NuRaft/include/libnuraft/state_machine.hxx` — 12 methods AsyncTask (including deprecated
  save_snapshot_data / read_snapshot_data), include compat
- `extern/NuRaft/include/libnuraft/log_store.hxx` — 7 methods AsyncTask, include compat
- `extern/NuRaft/include/libnuraft/rpc_cli.hxx` — rpc_handler typedef reshaped
- `extern/NuRaft/include/libnuraft/raft_server.hxx` — init_options rename + defaults;
  ~40 internal decls converted to AsyncTask; `commit_baton_` type `folly::coro::Baton` →
  `nuraft::Baton`; `bg_append_thread_`/`bg_append_ea_` replaced with `bg_append_baton_` +
  `append_bg_future_`
- `extern/NuRaft/src/handle_vote.cxx` — 4 fns cascaded
- `extern/NuRaft/src/handle_timeout.cxx` — 3 fns cascaded (`stop_election_timer` stays sync)
- `extern/NuRaft/src/handle_join_leave.cxx` — ~10 fns cascaded (`invite_srv_to_join_cluster`,
  `reset_srv_to_join`, `reset_srv_to_leave` stay sync)
- `extern/NuRaft/src/handle_snapshot_sync.cxx` — 5 fns cascaded; sync helpers untouched
- `extern/NuRaft/src/handle_client_request.cxx` — `handle_cli_req_prelock`, `request_append_entries_for_all`,
  `handle_cli_req` cascaded; sync callback helpers untouched
- `extern/NuRaft/src/handle_priority.cxx` — `set_priority`, `handle_priority_change_req` cascaded
- `extern/NuRaft/src/handle_custom_notification.cxx` — 4 fns cascaded
- `extern/NuRaft/src/handle_commit.cxx` — `commit`, `commit_loop`, `commit_in_bg_exec`, `commit_app_log`,
  `commit_conf`, `scan_sm_commit_and_notify`, `apply_config_log_entry`, `create_snapshot(options)`,
  `snapshot_and_compact`, `reconfigure` cascaded; `commit_baton_` uses ASYNC_AWAIT(baton.wait());
  create_snapshot's state_machine handler pattern replaced with direct `co_await` + inline callback
- `extern/NuRaft/src/handle_append_entries.cxx` — `append_entries_in_bg`, `append_entries_in_bg_exec`,
  `request_append_entries()`, `request_append_entries(peer)`, `create_append_entries_req`,
  `handle_append_entries`, `try_update_precommit_index`, `handle_append_entries_resp` cascaded;
  durability_signal await already existed and works with the macro
- `extern/NuRaft/src/global_mgr.cxx` — `#ifdef NURAFT_ASYNC_MODE` blockingWait bridge at append_worker
  (append_worker is a dedicated std::thread, so blockingWait is legit)
- `extern/NuRaft/src/raft_server.cxx` — ctor start-server scheduling, `start_server` body done

### In progress (raft_server.cxx bodies)

Signatures converted in the header. **These bodies still need cascade**:

- `update_rand_timeout` (called at raft_server.cxx:372)
- `update_params` (calls restart_election_timer)
- `process_req` (already Task<>; just rename to AsyncTask and update ASYNC_AWAIT sites)
- `reset_peer_info` (calls save_config, store_log_entry)
- `handle_peer_resp` (calls update_term, reset_peer_info, request_append_entries, etc.)
- `become_leader` (calls store_log_entry, request_append_entries, save_config maybe)
- `become_follower` (calls restart_election_timer maybe)
- `update_term` (calls save_state, become_follower)
- `handle_ext_msg` (calls update_term, various handle_*_req)
- `handle_ext_resp` (calls update_term, handle_*_resp)
- `store_log_entry` (calls log_store->append, log_store->write_at)
- Also `commit_app_log`'s trailing return_void addition (verify)
- Also `yield_leadership`, `check_leadership_transfer`, `check_leadership_validity`,
  `request_leadership`, `send_reconnect_request`, `handle_reconnect_req`, `handle_reconnect_resp` —
  audit whether they reach AsyncTask methods

### Not started (N2 remainder)

- `extern/NuRaft/src/handle_user_cmd.cxx` — Baton refactor (`folly::coro::Baton` → `nuraft::Baton`)
  + convert `folly::coro::Task<append_result>` return signatures to `AsyncTask<append_result>` +
  `co_await`/`co_return` sites to ASYNC_AWAIT/ASYNC_RETURN. About 5 Task fns.
- `extern/NuRaft/include/libnuraft/durability_signal.hxx` +
  `extern/NuRaft/src/durability_signal.cxx` — rename `folly::coro::Task<void>` → `AsyncTask<void>` +
  swap internal `folly::coro::Baton` for `nuraft::Baton`.
- `extern/NuRaft/src/asio_service.cxx` — the response-delivery wrapper at line ~818 already handles
  Task-returning `process_req`; just rename `folly::coro::Task<void>` → `AsyncTask<void>`. When
  `rpc_handler` becomes Task-returning (N4), the outbound response callback in asio needs a
  `co_invoke(...).scheduleOn(main_executor_).start()` wrapper.

---

## Phase order going forward

Numbering per current plan (may need renumbering — bg_append_thread retirement moved from N3 to N2):

1. **Finish N2** — remaining raft_server.cxx bodies, handle_user_cmd.cxx, durability_signal,
   asio_service. Compile-check async mode.
2. **N2.5 — Mutex refactor** (user's explicit ask — do BEFORE N3):
   - Current recursive mutex use in nuraft is fragile once coroutines can hop executors.
   - Path A (chosen): eliminate recursive locking. Entry-point methods take `lock_` once; internal
     methods take a `std::unique_lock<>&` guard param (or documented "caller must hold" via
     naming). Swap `std::recursive_mutex lock_` for `folly::coro::Mutex` (non-recursive) with
     `co_await mutex.co_scoped_lock()`.
   - Audit sites: `lock_`, `cli_lock_`, `commit_lock_`, `commit_ret_elems_lock_`, `sm_watchers_lock_`,
     `last_snapshot_lock_`, and per-peer `p->get_lock()`.
   - Also fix any callback that holds a mutex across a suspension point — those need to release
     before suspend and re-acquire after.
3. **N3 — Retire remaining threads**: `snapshot_io_mgr::io_thread_` becomes a coroutine on
   `slow_executor_` (since chunk memcpy/send is CPU-heavy). `async_io_loop` → `AsyncTask<void>`,
   awaits Baton. `bg_append_thread_` already retired in N2.
4. **N4 — asio adapter**: rpc_handler (typedef changed in N1) means asio's outbound
   response-delivery lambda needs `co_invoke(...).scheduleOn(main_executor_).start()` similar to
   its inbound-side pattern.
5. **H1–H5 — HomeStore side**:
   - `ReplicaSet` state_mgr overrides (save_config/save_state/load_config/read_state) drop
     `iomanager::blocking_wait`, use `co_await raft_config_sb_.write()` directly.
   - `ReplicaSet` state_machine overrides written from scratch matching the new AsyncTask
     signatures.
   - `HomeRaftLogStore` all `iomanager::blocking_wait` sites become `co_await`. Log_store overrides
     match the new AsyncTask signatures. Includes the tricky IndirectBlkHandler::write path
     (append returns Task<ulong>; inline entries co_return immediately without suspending — µs; only
     indirect entries suspend on blob_stream write).
   - Transport: `folly_rpc_listener` drops `is_slow_rpc` classification; everything goes to the
     reactor eb. `folly_rpc_client` outbound response callback is Task now.
   - `ReplicationManager`: keeps `slow_executor_` construction as an opt-in escape valve; injects into
     `raft_server::init_options.slow_executor_`. `start()` becomes AsyncTask; calls
     `co_await raft_server->start_server()`.
6. **T1–T3 — Nuraft mocks, examples, tests, bench**:
   - `examples/in_memory_state_mgr.hxx` — 4 methods `co_return` wrap.
   - `tests/unit/raft_functional_common.hxx` — state_mgr + state_machine test class, ~9 methods.
   - `tests/unit/fake_network.{cxx,hxx}` — rpc_handler invocation sites wrapped with co_invoke.
   - Example apps (`echo`, `calculator`) — mechanical state_machine `co_return` wraps.
   - `tests/bench/raft_bench.cxx` — same.
   - Test .cxx files: grep for calls to public API that changed (`raft_server->create_snapshot`,
     `->start_server`, `->set_priority`, etc.) and add `folly::coro::blockingWait(...)` — tests run
     on main thread, off-reactor, so blockingWait is safe there.
7. **Verify** — HomeStore REPLICATION=ON build green.

---

## Open concerns to revisit

1. **Mutex refactor** (N2.5) is user-flagged critical. Do not skip.
2. **`snapshot_and_compact` holds `snp_in_progress_` around a co_await** on state_machine::
   create_snapshot. If the coroutine resumes on a different thread, it's OK for atomic_bool but
   any std::mutex fields set nearby need auditing.
3. **`create_snapshot(options)` inline callback pattern** — replaced the handler_type dispatch with a
   direct call to `on_snapshot_completed(...)` after `co_await state_machine_->create_snapshot(...)`.
   Semantics equivalent; a downstream side effect to watch for is that `on_snapshot_completed`
   previously ran on whichever thread invoked the handler (could be async); now it runs on
   `main_executor_` right after the co_await. Behavior should be same or better.
4. **`recur_lock(lock_)` still in effect everywhere** — works only under the "same-thread-stickiness"
   assumption. N2.5 fixes this.
5. **AsyncTask default fall-through for void functions**: folly::coro::Task<void> allows implicit
   co_return at end; sync mode `void` allows implicit fall-through. Adding trailing
   `ASYNC_RETURN_VOID` is safe in both modes but slightly redundant. Not fixing.
6. **`store_log_entry` inline lambda in `create_snapshot(options)` was removed** — replaced with two
   nearly-identical await-and-return blocks. Refactor if it bothers you.
7. **`recur_lock(lock_)` inside `~raft_server`** — the destructor takes lock_ before triggering
   shutdown. In async-mode, we don't await inside the destructor (destructors can't be coroutines);
   the destructor uses `.get()` on the SemiFuture to wait for the coroutines to exit. Watch for
   deadlocks if any pending coroutine tries to acquire lock_ while destructor holds it.
8. **`slow_executor_`** is currently unused by our code. It's plumbed for the snapshot streaming
   coroutine (N3). If N3 doesn't end up needing it, drop.

---

## How to resume

1. Read this doc + `NURAFT_CORO_PLAN.md` (background) + `NURAFT_CORO_SESSION.md` (prior session context).
2. `grep -rn "folly::coro::Task" extern/NuRaft/src/ extern/NuRaft/include/libnuraft/` to see what still
   needs renaming to `AsyncTask<>`. Anything in `raft_server.cxx` bodies is the immediate next work.
3. `grep -rn "co_await\|co_return" extern/NuRaft/src/` — anything in the raft_server.cxx methods
   listed under "in progress" above needs to become ASYNC_AWAIT/ASYNC_RETURN.
4. Build check: nuraft is expected to fail to compile until N2 is 100% done. Compile with
   `-DNURAFT_ASYNC_MODE`. Should compile cleanly (once N2 finished) in HomeStore's Debug build.

---

## Key files quick-links

- Compat layer: `extern/NuRaft/include/libnuraft/async_compat.hxx`
- Interface headers: `extern/NuRaft/include/libnuraft/{state_mgr,state_machine,log_store,rpc_cli,raft_server}.hxx`
- HomeStore side (unchanged until H1): `src/homestore/replication/{replica_set.{h,cpp},home_raft_log_store.{h,cpp},repl_manager.{h,cpp}}`
- HomeStore transport (unchanged until H4): `src/homestore/replication/transport/{folly_rpc_listener.{h,cpp},folly_rpc_client.{h,cpp},folly_rpc_client_factory.{h,cpp}}`
- Existing background reading: `src/homestore/replication/{NURAFT_CORO_PLAN.md,NURAFT_CORO_SESSION.md}`

## Things NOT to forget

- state_machine's `create_snapshot(snapshot&, async_result<bool>::handler_type&)` now
  `AsyncTask<bool> create_snapshot(snapshot const&)`. Application implementers MUST update.
- `init_options::sync_executor_` renamed to `slow_executor_`. Callers passing an executor must
  rename the field.
- `init_options::start_server_in_constructor_` defaults to false. Callers relying on legacy
  auto-start must either explicitly set true or explicitly `co_await raft_server->start_server(...)`.
- `bg_append_thread_` and `bg_append_ea_` MEMBERS are gone. Any external code touching them breaks.
- `folly::coro::Task<>` internal to nuraft is being renamed to `AsyncTask<>`. External callers using
  the old `raft_server::append_entries` return type should update or use the compat alias.
