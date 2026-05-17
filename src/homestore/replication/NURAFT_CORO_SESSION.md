# NuRaft Coroutine-Readiness — Session Export

Session purpose: scope and begin executing a project to make NuRaft fully
coroutine-driven (folly::coro::Task<>) so HomeStore can run N raft groups on
M reactor threads via cooperative multitasking instead of dedicated threads.

This file is a hand-off so a new Claude session on a different machine can
continue the work. Read this end-to-end before doing anything.

## 1. Who and what

- **User:** Harihara Kadayam (harihara.kadayam@gmail.com).
- **Project:** HomeStore — eBay's storage engine, rebuilt on folly coroutines.
- **Repo:** /workspaces/HomeStore, branch `cpp/modern`.
- **Main branch for PRs:** `rust/main`.
- **Build:** Conan + CMake. **Always build Debug.**
- **GitHub:** hkadayam/HomeStore (HomeStore), hkadayam/NuRaft (fork).
- **Date as of session:** 2026-05-17.

## 2. Context — why this exists

HomeStore was rebuilt on folly::coro. The replication subdirectory
(`src/homestore/replication/`) is excluded from the build at
[src/CMakeLists.txt:78](src/CMakeLists.txt#L78) (commented out) because:
- The replication code mixes async + sync callbacks.
- NuRaft itself is not coro-aware.
- nuraft_mesg (the multi-group raft management layer) has been dropped from
  conanfile.py.

User's goal: re-enable replication, with NuRaft fork made coroutine-ready,
so that the two hot paths can use folly::coro::Task<>:

- **HomeStore → Raft (proposer):** `co_await raft.append_entries(logs)`
  returning on commit, plus a `_async` variant returning on accept only.
- **Raft → HomeStore (apply):** `co_await sm->commit_ext(idx)` and
  `co_await sm->pre_commit_ext(idx)` on the apply path.

The cooperative model: N raft groups share an M-thread executor (M ≪ N).
While group A's commit_loop coro is suspended on `co_await commit_ext`,
group B's commit_loop runs on the same OS thread. No thread-per-group.

## 3. User's working style — important behavioral notes

- **Pushes hard against rat-holing.** Direct, blunt feedback. Calls out when
  Claude is being verbose or going off-track. Re-read his messages before
  responding.
- **Hates half-baked options.** Rejected an early "L1/L2 wrap-vs-modify"
  framing. Wants the right design, not a hybrid shim. Don't propose
  `folly::coro::blockingWait` as if it's a real solution to coro-readiness
  — he caught this and rejected it loudly.
- **Always build Debug.** Rejected a Release build attempt during execution.
- **Prefers concrete over hand-wavy.** Wants code snippets, exact API names,
  exact thread context, exact LOC estimates per component.
- **Will interrupt to redirect scope.** If a question is in the rough area
  but framed wrong, he will interrupt and re-ask. Listen carefully.
- **Saves details to memory liberally.** Auto-memory at
  `/home/codespace/.claude/projects/-workspaces-HomeStore/memory/` —
  `MEMORY.md` index points to project notes.

## 4. The plan document

The comprehensive plan lives at:

  **`src/homestore/replication/NURAFT_CORO_PLAN.md`** (1158 lines)

It's committed and pushed to `origin/cpp/modern`. Read it as the source of
truth. This export is a session log; the plan is the authoritative design.

Plan structure (sections):

1. Goal
2. Non-goals (what stays sync)
3. Submodule setup (Step 3 — DONE, see §10 of this export)
4. Runtime model — one pool everywhere (iomanager)
5. NuRaft fork — required changes (ABI changes, commit_loop, durability_signal,
   append_entries Task<> entries, executor injection, scheduler shim)
6. Transport — folly::AsyncSocket on iomgr (FollyRpcClient,
   FollyRpcListener, FollyRaftConnection)
7. HomeStore-side changes (LogStore durability observer, log_store adapter,
   RaftStateMachine Task<> overrides, propose_to_raft coro, sync_executor)
8. Leader flow (end-to-end ASCII diagram with thread/coro context per step)
9. Follower flow (end-to-end, Phase 1 + Phase 2)
10. NuRaft API seam — rpc_client and rpc_listener (interfaces stay; impls swap)
11. Sync state_machine methods — sync_executor hop (commit_config,
    apply_snapshot, rollback_ext)
12. log_store adapter (method-by-method NuRaft → homestore LogStore mapping)
13. Locking discipline (single rule: no std::mutex held across co_await;
    + commit_conf lock-drop refactor)
14. Audits to do during execution (8 items)
15. LOC estimate (broken down by component, ~2370 total + tests)
16. Acceptance tests (11 scenarios)
17. Out-of-scope follow-ups

## 5. Key design decisions locked in

These were reached through extensive back-and-forth — don't relitigate without
strong reason.

### 5.1 Transport: folly::AsyncSocket on iomanager's existing pool

**Decision: use iomanager. Do NOT create a new pool.** iomanager already runs
an `IOThreadPoolExecutor pool_` with N reactor threads, each hosting a
`folly::EventBase` (with optional IoUringBackend). The transport pins each
accepted connection to a reactor (hash by `group_id % num_reactors`). All
folly::coro::Task<>s schedule on the same pool. No separate "rpc-thr."

API exposed by iomanager already (see [src/iomanager/iomanager.h](src/iomanager/iomanager.h)):

```cpp
class IOManager {
    folly::EventBase* reactor_for(size_t reactor_id) const;
    folly::EventBase* resolve_target(ReactorTarget target) const;
    template<typename F> void spawn_detached(ReactorTarget, F);
    template<typename T> folly::coro::Task<T> spawn_waitable(...);
};
```

You may need to expose a public accessor for `pool_` (the underlying executor)
if not already present. Trivial 5-line addition.

### 5.2 NuRaft API contract changes

**state_machine virtuals on the hot path → folly::coro::Task<>:**

- `commit_ext` and `pre_commit_ext` change signature to
  `folly::coro::Task<ptr<buffer>>`. ABI break — every state_machine impl
  recompiles.
- `rollback_ext`, `commit_config`, `rollback_config`, all snapshot APIs stay
  sync. They're called from commit_loop and hopped to `sync_executor` via a
  `co_run_sync` helper.

**raft_server proposer entries:**

```cpp
folly::coro::Task<append_result> append_entries(
    const std::vector<ptr<buffer>>& logs);          // await commit
folly::coro::Task<append_result> append_entries_async(
    const std::vector<ptr<buffer>>& logs);          // await accept
```

Wrap the existing `cmd_result`-returning internal call. The accept signal is
already synchronous (set via `cmd_result::accept()` inside
`append_entries_internal`), so `_async` is essentially `co_return
cr->get_accepted()`. The commit-waiting variant installs `when_ready` and
co_awaits a baton.

**handle_append_entries (follower) → Task<>:**

```cpp
folly::coro::Task<ptr<resp_msg>> handle_append_entries(req_msg& req);
```

`ea_follower_log_append_` (single-waiter EventAwaiter) replaced with a
**multi-waiter `durability_signal_`** primitive (sorted/indexed waiters keyed
by target_lsn; `advance(N)` wakes all with `target ≤ N`).

### 5.3 bg_commit_thread → commit_loop coroutine

```cpp
folly::coro::Task<void> raft_server::commit_loop() {
    while (!stopping_.load()) {
        co_await commit_baton_;
        co_await commit_in_bg_exec();   // Task<void>
    }
}
```

One coroutine per raft_server (per group). Scheduled on iomgr's executor via
`init_options.main_executor`. `commit_cv_`/`commit_cv_lock_` replaced with
`folly::coro::Baton commit_baton_`.

### 5.4 bg_append_thread STAYS as pthread (minimal scope)

Leader peer-fanout stays callback-based. `rpc_client::send(req, when_done,
timeout)` interface unchanged at NuRaft level. Our `FollyRpcClient` impl
just fires the callback when the response arrives on the EventBase thread.
(Coro-izing the fanout for `collectAll` is an out-of-scope follow-up.)

### 5.5 lock_ stays as std::recursive_mutex

Audit confirmed lock_ is recursive because of genuine reentrancy (e.g.,
`commit_conf` re-acquires it, `cb_func_.call(...)` user callbacks may
re-enter, `request_append_entries_for_all` from `notify_log_append_completion`).
Keep it. Just enforce: **never `co_await` while holding any std::mutex.**

One localized refactor needed: `commit_conf` currently holds lock_ across the
sync `commit_config` state_machine call. New code releases lock_ before
`co_await co_run_sync(sync_executor_, [&]{ sm->commit_config(...); })` and
re-acquires after, with invariant re-check. ~80–150 LOC.

### 5.6 Sync state_machine methods hopped via sync_executor

```cpp
template <typename Fn>
folly::coro::Task<std::invoke_result_t<Fn>>
co_run_sync(folly::Executor::KeepAlive<> sync_exec, Fn&& fn) {
    auto* main = co_await folly::coro::co_current_executor;
    co_await folly::coro::co_reschedule_on_current_executor.via(sync_exec);
    auto r = std::forward<Fn>(fn)();
    co_await folly::coro::co_reschedule_on_current_executor.via(main);
    co_return r;
}
```

`sync_executor` is a small `folly::CPUThreadPoolExecutor` (1–2 threads) owned
by `RaftReplService`, passed via `init_options.sync_executor`.

### 5.7 LogStore durability observer — homestore-side, ~10 LOC

Need a hook so the NuRaft adapter is notified when LogStore's `tail_lsn_`
advances:

```cpp
// In src/homestore/logstore/log_store.h:
using durability_cb = std::function<void(lsn_t new_tail_lsn)>;
void set_durability_observer(durability_cb cb);

void LogStore::on_write_completion(lsn_t lsn, const stream_key& key) override {
    // ... existing tail_lsn_ advance ...
    if (durability_observer_) durability_observer_(tail_lsn_.load());
}
```

The adapter subscribes; on each tail_lsn_ advance it:
- Updates cached `last_durable_index_` atomic.
- Calls `raft_server->notify_log_append_completion()`.
- This may post `commit_baton_` (advancing quick_commit_idx).

This runs **inline within `LogStream::flush()`'s coroutine** (an existing
iomgr coro driven by `flush_timer_`, see [src/homestore/logstore/log_stream.h](src/homestore/logstore/log_stream.h)).
No new thread.

### 5.8 log_store adapter — quick_append is the key

Homestore's `LogStore::quick_append(blob)` is **sync, lock-free, in-memory**.
That's what makes the hot-path "sync log_store" decision correct. The adapter:

| NuRaft method | Adapter impl | Note |
|---|---|---|
| `store_log_entry(entry, idx)` | `homestore_ls->quick_append(blob)` | µs CPU, safe on reactor |
| `entry_at(idx)`, `term_at(idx)` | `folly::coro::blockingWait(homestore_ls->read(idx))` | blocks calling thread — only on bg_append/sync_executor/RPC, never reactor on hot path |
| `last_durable_index()` | cached atomic, updated by observer | sync getter |
| `flush()`, `compact(idx)` | `blockingWait` on homestore coro | called from sync state_machine paths only (on sync_executor) |

### 5.9 NuRaft fork via git submodule

**Decision: submodule of GitHub fork.** Not vendor-copied, not Conan recipe.
The fork is `hkadayam/NuRaft` at GitHub (already existed before this session;
user pointed at it). Submodule path: `extern/NuRaft`. CMake glue under
`REPLICATION=ON` does `add_subdirectory(extern/NuRaft EXCLUDE_FROM_ALL)`.

Build options needed: `BUILD_TESTING=OFF`, `BUILD_EXAMPLES=OFF`,
`DISABLE_SSL=1` (TLS later). NuRaft has a nested submodule
`extern/NuRaft/asio` (chriskohlhoff/asio standalone) which it auto-uses via
its built-in `find_path(ASIO_INCLUDE_DIR ...)`. Picked up automatically.

Verified standalone build: `cmake --build . --target static_lib -j4` produces
`libnuraft.a` cleanly with C++20.

## 6. Iterated design discussion — key turns

These are the moments the design pivoted. Useful for understanding the "why"
behind decisions.

### Turn 1 — User rejects L1/L2 wrap framing

Claude initially proposed "L1: keep NuRaft mostly sync, wrap at homestore
boundary" and "L2: deep coro inside NuRaft." User pushed back hard:

> "Your L1 is absolutely useless. blockingWait if I have to do, I might as well
> leave it as is. Don't fucking bring that option and waste my $s and tokens
> and time."

Decision: do it the right way, not a hybrid. The minimal scope still
involves real NuRaft changes (commit_ext → Task<>, commit_loop coro,
append_entries Task<>) — about ~300 LOC originally, expanded as full-follower
path was added.

### Turn 2 — User clarifies original ask was just append + commit/pre_commit

After Claude over-scoped to "modernize all of NuRaft," user pulled back:

> "My primary goal is what all needs to change to make NuRaft fundamentally
> coro ready and I am completely focussing from the perspective of homestore
> calling raft → append, raft calling homestore commit and pre-commit path
> that I wanted to be coro ready. Rest to be coro is nice to have."

This led to the minimal-scope re-articulation: only commit_ext, pre_commit_ext,
and the proposer entry are Task<>; bg_commit_thread becomes commit_loop;
everything else stays.

### Turn 3 — User correctly identifies that pre_commit changes follower model

Per the audit, `pre_commit_ext` is called **only from
`handle_append_entries`** (follower's RPC thread). Making it Task<> forces:
- Option A: `blockingWait` on rpc-thr (parks the RPC thread).
- Option B: coro-ize handle_append_entries entirely (rpc_listener interface
  change, multi-waiter durability primitive, etc.).
- Option C: keep pre_commit_ext sync (loses the requirement).

User initially chose Option A (sized rpc-thr pool). Then realized the
**follower's durability wait is the real perf concern**:

> "I still didn't get which thread blocks on flush of logs to be completed.
> That is the crucial perf point. That cannot be blocking the thread, but
> has to be coro."

This led to committing to Option B — full follower coro path. Adds
`multi-waiter durability_signal_` primitive (~80–100 LOC) and
rpc_listener-side Task<> dispatch.

### Turn 4 — Concurrent in-flight handle_ae_tasks; ordering

User asked: with handle_append_entries as Task<>, multiple RPCs for same
group may run concurrently. Can the second response "beat" the first?

Analysis: no, because
- Validation+quick_append serialize through lock_.
- Durability waits cannot invert (monotonic — durable @ 8 implies durable @ 7).
- Each handler waits for its own target before responding.
- Raft response is content-keyed (max-monotonic `matched_idx`), not
  arrival-keyed.

Worst case: rare race where Task B grabs lock_ before Task A → spurious
rejection → leader retransmits. Normal Raft territory.

### Turn 5 — folly::AsyncSocket transport on iomanager

User wanted folly::AsyncSocket (not gRPC, not raw boost::asio). Asked whether
to make a new EventBase pool. Answer after reading iomanager.cpp: **NO**.
iomanager already runs an IOThreadPoolExecutor with N reactor threads, each
with its own EventBase, optionally io_uring backend. Use that pool.

Confirmed: `g_iomgr->reactor_for(rid)` returns the EventBase to attach
AsyncSocket to. `g_iomgr` exposes `pool_` (may need a public getter).

### Turn 6 — io_uring + zero-copy

Confirmed folly::AsyncSocket supports both:
- `setZeroCopy(true)` for `MSG_ZEROCOPY` — worth it for large writes
  (snapshot transfer). Default OFF for small AE messages.
- `IoUringBackend` for EventBase — readiness multiplexing via io_uring,
  works transparently with AsyncSocket. iomanager already conditional-enables
  this via `uring_opts`.

## 7. Final architecture summary

```
                  iomanager (existing) — folly::IOThreadPoolExecutor
                  ┌────────────────────────────────────────────────┐
                  │  Reactor 0  EventBase + IoUringBackend          │
                  │  Reactor 1  EventBase + IoUringBackend          │
                  │  ...                                            │
                  │  Reactor M-1 EventBase + IoUringBackend         │
                  └────────────────────────────────────────────────┘
                       │              │             │
                       │ hosts:       │ hosts:      │ hosts:
                       │              │             │
            AsyncSocket I/O   commit_loop coros   handle_ae_task coros
            (per peer, per    (one per group,     (one per inbound RPC
             accepted conn)    pinned via         per group, sched on
                               iomgr executor)    iomgr executor)
                       │              │             │
                       │              ▼             ▼
                       │       ┌─────────────────────┐
                       │       │  state_machine      │
                       │       │   .commit_ext        │
                       │       │   .pre_commit_ext    │
                       │       │   (Task<>)          │
                       │       └─────────────────────┘
                       │
                       └─ FollyRpcClient (outbound to peers)
                       │  FollyRpcListener + FollyRaftConnection (inbound)
                       │  Wire: length-prefixed flatbuffers
                       │
                       │  bg_append_thread (separate pthread)
                       │     drives leader peer fanout via rpc_client::send

  sync_executor (folly::CPUThreadPoolExecutor, 1-2 threads)
    Hosts rare sync state_machine calls:
      commit_config, apply_snapshot, rollback_ext, snapshot read/save
    Reached via co_run_sync helper from commit_loop.

  iomgr's existing flush mechanism:
    LogStream::flush_timer_ (CoroTimer) → co_await LogStream::flush()
    → LogStore::on_write_completion → durability_observer fires inline
    → raft->notify_log_append_completion → may post commit_baton_
```

## 8. LOC estimate (recap from plan §15)

| Block | LOC |
|---|---|
| NuRaft fork delta (state_machine Task<>, commit_loop, durability_signal, append_entries Task<>, scheduler shim, executor injection, sync hops, commit_conf refactor) | ~1020 |
| Transport (FollyRpcClient, FollyRpcListener, FollyRaftConnection, framing, reconnection) | ~900 |
| HomeStore-side (durability observer, log_store adapter, RaftStateMachine Task<>, listener Task<>, propose_to_raft, sync_executor, iomgr getter) | ~415 |
| Build system (submodule, CMake glue) | ~35 |
| **Total** | **~2370** |

Plus ~1000 LOC of tests.

## 9. Audits to do during execution (recap from plan §14)

These are deferred items — verify before/during the relevant change:

1. **`cmd_result::when_ready` install-after-set race.** Check async.hxx —
   does when_ready auto-fire if has_result_ already true? If not, the Task<>
   wrapper must check post-install and synthesize the call.
2. **Rejection-without-set_result case.** Verify wrapper's
   `!cr->get_accepted()` guard is sufficient.
3. **p->send_req lock state in `request_append_entries`.** Doesn't matter
   for minimal scope; relevant if leader fanout is later coro-ized.
4. **init_options shape.** Confirm clean injection point for two executors.
5. **folly executor switching idiom.** Pin the right folly API for "hop to
   sync_executor, run, hop back" in `coro_helpers.hxx`. Candidates:
   `folly::coro::co_invoke`, `co_reschedule_on_current_executor.via`,
   `co_via`. Verify against folly/2024.08.12.00.
6. **`commit_in_bg_exec` log_store read.** Confirm commit_loop receives the
   entry buffer from `commit_ret_elems_` map (no log_store re-read needed)
   or needs `co_run_sync(sync_executor, ...)` for the read.
7. **Multi-waiter `durability_signal_` contention.** Profile under load.
8. **NuRaft submodule build wiring.** Already verified standalone (§10);
   verify in homestore's REPLICATION=ON build when the work gets there.

## 10. Execution progress

### Step 3 — Submodule setup: **DONE**

Commit `c5cb6b94` on `cpp/modern` (pushed to origin):

1. `.gitmodules`:
   ```
   [submodule "extern/NuRaft"]
       path = extern/NuRaft
       url = https://github.com/hkadayam/NuRaft.git
   ```

2. Submodule entry `extern/NuRaft` at fork's master HEAD.
   Nested submodule `extern/NuRaft/asio` (chriskohlhoff/asio) auto-pulled via
   `git submodule update --init --recursive`.

3. Root [CMakeLists.txt](CMakeLists.txt) — added conditional block before
   `add_subdirectory(src)`:

   ```cmake
   if (DEFINED REPLICATION AND ${REPLICATION} STREQUAL "ON")
       set(_BUILD_TESTING_SAVED ${BUILD_TESTING})
       set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
       set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
       set(DISABLE_SSL 1 CACHE STRING "" FORCE)
       add_subdirectory(extern/NuRaft EXCLUDE_FROM_ALL)
       set(BUILD_TESTING ${_BUILD_TESTING_SAVED} CACHE BOOL "" FORCE)
   endif()
   ```

4. **Verified standalone:** `cmake --build . --target static_lib -j4`
   produces `libnuraft.a` with C++20.

5. **NOT yet verified in homestore's REPLICATION=ON build.** User wanted
   Debug build, but only Release toolchain was generated; needed `conan
   install . --build=missing -s build_type=Debug` first. User interrupted
   the conan call before it ran — likely wanted Claude to stop and let them
   do environment setup themselves.

### Next steps (Step 4 and beyond, per plan)

After Step 3, the plan calls for:

**Step 4 — Plumbing: inject executor through init_options.**
Add `folly::Executor::KeepAlive<> main_executor` and `sync_executor` fields
to `init_options` in `extern/NuRaft/include/libnuraft/raft_server.hxx`.
HomeStore-side passes `g_iomgr->executor()` (after exposing a getter).

**Step 5 — Replace bg_commit_thread with commit_loop coroutine.**
In `extern/NuRaft/src/raft_server.cxx`, replace
`bg_commit_thread_`/`commit_cv_` with `commit_baton_` (coro::Baton) and a
`commit_loop` Task<>. Schedule on `main_executor` at server start.
`commit_in_bg_exec()` becomes `Task<void>`.

**Step 6 — commit_ext / pre_commit_ext → Task<>.**
In `include/libnuraft/state_machine.hxx`. ABI break; ripples to
homestore-side `RaftStateMachine` impl.

**Step 7 — append_entries / _async Task<> wrappers.**
In `raft_server.{hxx,cxx}`. New `append_result` struct. Wraps existing
internal logic with `cmd_result::when_ready` → `coro::Baton`.

**Step 8 — coro_helpers.hxx + sync_executor hops.**
New helper `co_run_sync(sync_exec, fn)`. Apply at `commit_config`,
`apply_snapshot`, `rollback_ext` call sites in `handle_commit.cxx`. Plus
`commit_conf` lock-drop refactor.

**Step 9 — HomeStore-side wiring.**
LogStore durability observer hook. NuRaft log_store adapter. RaftStateMachine
Task<> overrides. propose_to_raft as Task<>. sync_executor in RaftReplService.
IOManager executor getter.

**Step 10 — Transport implementation.**
FollyRpcClient, FollyRpcListener, FollyRaftConnection. Framing. Reconnection.

**Step 11 — Re-enable replication build.**
Uncomment `add_subdirectory(homestore/replication/)` at
[src/CMakeLists.txt:78](src/CMakeLists.txt#L78).

**Step 12 — Acceptance tests.**
11 scenarios in plan §16.

## 11. Memory notes (auto-memory system)

Auto-memory files at
`/home/codespace/.claude/projects/-workspaces-HomeStore/memory/`:

- `MEMORY.md` — index, two entries:
  - `project_nuraft_coro_scoping.md` — long detailed scoping notes
  - `project_nuraft_coro_plan_location.md` — pointer telling future Claudes
    "the plan is in the repo; read it directly"

When porting to a new machine, copy this directory across, or just let the
new Claude session re-create memory as needed (the plan in the repo is the
source of truth; memory is supplementary).

## 12. Conversation cadence notes for the next Claude

- User prefers tight responses. No long preambles. Get to the answer.
- When user asks a question, **answer it**, don't reframe.
- When user pushes back, **don't defend the framing**, just absorb and
  reframe per their input.
- When proposing a design, **flag trade-offs up front** instead of
  burying them. User caught Claude doing this multiple times and called
  it out.
- When user says "just do X," **do X**, don't ask three clarifying
  questions first.
- **Always Debug build.** Don't run conan/cmake with `-s build_type=Release`
  unless asked.
- Conditional builds: replication-related code goes under `REPLICATION=ON`.
- Commit messages: brief subject line, body explains why not what,
  Co-Authored-By trailer with `Claude Opus 4.7 (1M context)`.

## 13. Open / pending items at end of session

1. **Verify homestore configures with REPLICATION=ON in Debug.** Conan
   install for Debug toolchain needed first. User interrupted this
   verification — they may complete it themselves or instruct otherwise.

2. **Step 4+ of the plan not started.** Next concrete action is to start
   modifying `extern/NuRaft` source: add executor fields to `init_options`,
   then replace bg_commit_thread with commit_loop, etc.

3. **Decisions on fork branch.** User chose to track `master` on the fork.
   The work happens directly on master — no separate `coro-readiness`
   branch was created. If the fork's master gets messy, may want a
   long-lived branch later.

4. **iomanager executor accessor.** Need to verify if `IOManager::pool_`
   has a public getter (probably not). Trivial 5-line addition:
   `folly::Executor::KeepAlive<> executor() { return pool_.get(); }`.

5. **Conanfile.py wiring for replication.** Currently
   [conanfile.py](conanfile.py) doesn't list nuraft/nuraft_mesg as deps
   (already cleaned). But the old [src/CMakeLists.txt:40-67](src/CMakeLists.txt#L40-L67)
   has `find_package(NuraftMesg)` and `nuraft_mesg::proto, nuraft::nuraft`
   wired under REPLICATION=ON — these need to be replaced with the
   submodule's `static_lib` target (alias `NuRaft::static_lib`) when
   replication code is re-enabled.

## 14. Key files to read first in new session

In order of importance:

1. **`src/homestore/replication/NURAFT_CORO_PLAN.md`** — the authoritative
   plan. Read sections 4, 5, 8, 9, 10 first.
2. **`/home/codespace/.claude/projects/-workspaces-HomeStore/memory/`** —
   if memory was synced.
3. **`extern/NuRaft/include/libnuraft/raft_server.hxx`** — where most of
   the fork delta lands.
4. **`extern/NuRaft/src/handle_commit.cxx`** — where commit_loop replaces
   bg_commit_thread.
5. **`extern/NuRaft/src/handle_append_entries.cxx`** — where the follower
   Task<> conversion lands.
6. **`src/iomanager/iomanager.h`** — the pool the transport plugs into.
7. **`src/homestore/logstore/log_store.h`** — `quick_append` API + where
   the durability observer hook goes.
8. **`src/homestore/replication/repl_dev/raft_state_machine.{h,cpp}`** —
   homestore-side state_machine impl (currently sync, needs Task<>).

## 15. Final commit on `cpp/modern`

As of session end:

```
c5cb6b94 extern/: add NuRaft fork as submodule (Step 3 of coro plan)
9dd70a0e replication/: rewrite NuRaft coro plan for full follower path + folly transport
e41ba4ae Safety checkin - 5      (user's safety checkin)
078ccfe7 replication/: add NuRaft coroutine-readiness plan
... earlier commits ...
```

Everything pushed to `origin/cpp/modern`. Pull and `git submodule update
--init --recursive` to bring up the submodule on the new machine.

---

End of session export. Pick up at Step 4 (executor injection through
init_options) when ready.
