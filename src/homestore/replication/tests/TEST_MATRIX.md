# Replication Negative-Test Matrix (with flip / crash injection)

Single multi-process binary: `test_replica_set` (one OS process per replica, boost interprocess
barriers). Backend is the PERSISTENT COWBtree store. Fault injection uses the **Flip** framework
(armed via `HSTestHelper::set_flip`/`set_delay_flip`) and, for crashes, the **CrashSimulator**
(`base/crash_simulator.h`, `SISL_FLIP_ENABLED` builds only): a product crash point is one line —
`if (crash_if_flip_fired("<name>")) { co_return; }` — which freezes the device layer (every
PhysicalDev write fake-succeeds via the `is_crash_simulated()` gate, so the disk stays exactly as of
the crash instant, the dying shutdown's final CP included) and reboots the instance in-process
through ordinary recovery.  Tests arm the flip and block on `wait_for_crash_recovery()`.

Legend — Status: ✅ implemented & passing · 🟡 implemented, flaky · ⛔ not implemented (TODO) · ▫ infra missing.

---

## A — Functional (no faults)

| ID | Test | Scenario | Flip / crash point | Expected | Status |
|----|------|----------|--------------------|----------|--------|
| A2 | `ReplicatedWrites` | N writes, validate (covers the single-write path) | — | all commit, values match | ✅ |
| A3 | `HighQdepthWrites` | N in flight concurrently (qdepth 64) | — | all commit exactly once, ordered | ✅ |
| A4 | `SequentialBatches` | back-to-back batches | — | cumulative count exact | ✅ |
| A5 | large-value (indirect blk) write | value ≥ indirect threshold | — | blob stream path, commit, re-read | ⛔ (`blob_opt=off` everywhere — IndirectBlkHandler + free_blks_journal have zero coverage) |

## B — Recovery (clean restart, no fault)

Clean shutdown checkpoints every consumer (btree + logstore applied-watermark), so recovery loads
from the btree with an EMPTY replay window; replay re-delivery only happens on crash (category C).

| ID | Test | Scenario | Flip / crash point | Expected | Status |
|----|------|----------|--------------------|----------|--------|
| B1 | `WriteRestartAllMulti` | 6 rounds: write, quiesce-barrier, whole-cluster restart | — | every round recovers all prior keys; empty replay | ✅ |
| B2 | `FollowerRestarts` | followers down one at a time, leader keeps writing | — | downed follower catches up from log | ✅ |
| B3 | `LeaderQuickRestart` | leader blips; survivors hold a 30s window set in place via `update_raft_params` | — | pre-vote liveness blocks ALL candidacies (returned leader included) until the window lapses; post-blip winner (return or takeover) carries the round; non-leaders exit only on commit proof | ✅ (6/6 suite + 3/3 solo) |
| B4 | `LeaderSlowRestart` | leader sleeps > election window | — | survivors elect mid-gap and take writes; old leader returns follower + catches up | ✅ (6/6 suite) |
| B5 | `SlowReplicaRollingRestart` | `simulate_slow_replica_commit` delay flip on one replica; fast members restart one-by-one | delay flip, journal-type conditioned | slow replica converges while still slow | ✅ (6/6 suite) |
| B6 | `DestroyPendingRestartReap` | destroy commits, whole cluster restarts inside reaper grace | — | reloaded SB re-stages DESTROYED; reaper erases the group with no re-issued destroy | ✅ (includes the load() re-stage product fix) |
| B7 | `TrafficInflightFollowerRestart` | follower restarts mid-append-stream, no quiesce | — | leader commits on surviving quorum through the blip; returned follower converges | ✅ (leader-side uncommitted-tail + join-in-flight variants still ⛔) |
| B8 | `TwoFollowersSimultaneousRestart` | both followers down together — quorum lost | — | leader's proposals stall and resume exactly-once; leadership never moves | ✅ |
| B9 | `BootOrderRotation` | full-cluster restart, rotating who boots first/last | — | every boot order settles a leader past stale claims; rounds' writes land | ✅ (caught the log-store-id reuse corruption) |
| B10 | `ZeroWriteRestartChurn` | 4 back-to-back restarts with zero writes between | — | every boot sees an empty replay window; no watermark drift; group stays live | ✅ |

## C — Crash matrix (CrashSimulator flip at a named point, then restart + validate)

For every row: inject the crash flip on the target replica (leader/follower as noted), let the
process abort at that point, restart, then assert no data loss, no double-apply, chain/SB
consistent, group re-forms.

| ID | Crash point (flip name) | Injected where | Restart expectation | Status |
|----|-------------------------|----------------|---------------------|--------|
| C1 | after raft log append, before commit | leader | uncommitted tail dropped or re-committed; no dup | ⛔ |
| C2 | after commit (state-machine apply), before CP | follower | recovered via proof-gated log replay; state exact | ✅ `CrashAfterCommitFollower` (`crash_after_data_commit` flip) — verified from the recovery log: dying shutdown's CP never persisted (CP counter resumed at the same cp_id), log-store checkpt read back pre-crash (302), replay proved 303 and dropped 4 unproven tail entries to nuraft re-commit |
| C3 | after CP, before commit_lsn persist | any | btree durable; replay from stale commit_lsn is idempotent | ⛔ |
| C4 | during `write_sb` (RSSuperBlk persist) | any | SB either old or new, never torn; recovers | ⛔ |
| C5 | after metablk `write_data`, before chain link | any | orphan blk reconciled; no ghost in chain | ⛔ |
| C6 | during log truncation / compaction | any | log floor consistent; no gap | ⛔ |
| C7 | during snapshot create | leader | snapshot atomic; ret/redo clean | ⛔ |
| C8 | during snapshot install (receiver) | follower | partial snapshot discarded; refetch | ⛔ |
| C9 | during membership-change commit | any | config either old or new; symmetric across replicas | ⛔ |
| C10 | leader crash mid-replication (uncommitted in flight) | leader | re-elect, no committed loss, uncommitted may drop | ⛔ |
| C11 | follower crash mid-catch-up | follower | resumes catch-up (log back-fill or snapshot) | ⛔ |

## D — Election / leadership

| ID | Scenario | Flip / fault | Expected | Status |
|----|----------|--------------|----------|--------|
| D1 | leader crash → re-election | kill leader | new leader within election timeout; writes resume | ⛔ |
| D2 | post-restart re-election settles | restart all | a leader settles; teardown destroy succeeds | 🟡 (was connect-failed stall — FIXED, keep as regression) |
| D3 | dropped vote RPCs (split window) | flip drop `request_vote` | eventually elects; no two leaders same term | ⛔ |
| D4 | connect-failed during election (peer down window) | peer restart race | dead outbound socket dropped, reconnect, elect | ✅ (fixed: `PeerOutboundSocket::failed_` + factory drop) |
| D5 | pinned leadership (`leadership_expiry_ms=-1`) | — | creator stays leader whole test | ✅ (used by all tests) |

## E — Snapshot / baseline resync (requires non-stub listener snapshot hooks)

| ID | Scenario | Flip / fault | Expected | Status |
|----|----------|--------------|----------|--------|
| E1 | leader takes snapshot at LSN | — | snapshot recorded; log compactible | ▫ listener `take_snapshot` stubbed → BAD_REQUEST |
| E2 | lagging follower installs snapshot (baseline) | follower far behind | follower catches up via snapshot, not full log | ▫ `apply_snapshot` stubbed → false |
| E3 | crash during snapshot transfer | flip mid-transfer | discard partial, refetch | ⛔ (needs E infra) |
| E4 | log compaction after snapshot | — | old log freed; recovery uses snapshot + tail | ⛔ |

## F — Membership (requires non-stub membership hooks)

| ID | Scenario | Flip / fault | Expected | Status |
|----|----------|--------------|----------|--------|
| F1 | add member (spare joins) | — | joiner catches up (log/snapshot), commits | ▫ `on_membership_change` stubbed (empty) |
| F2 | remove member | — | removed cleanly; quorum recomputed | ▫ stubbed |
| F3 | replace member (start→complete) | — | out swapped for in; `task_id` carried; SB symmetric | ▫ `on_*_replace_member` stubbed |
| F4 | crash during membership commit | flip C9 | config atomic across replicas | ⛔ |
| F5 | membership change during active writes | concurrent load | no lost/mis-ordered commits across the change | ⛔ |

---

## Infra gaps to close before C/D/E/F land

1. **CrashSimulator flip points** — the infra is live (device-layer freeze, `crash_if_flip_fired`,
   in-process crash-restart, `wait_for_crash_recovery`); `crash_after_commit` (C2) is the first
   point.  Remaining named points to add: raft append (C1), CP mid-flush (C3), `write_sb` (C4),
   metablk link (C5), truncate (C6), membership commit (C9); snapshot points ride the E infra.
2. **Non-stub test listener** — implement `take_snapshot`/`build_snapshot`/`apply_snapshot` and the
   membership/replace hooks in `ReplTestListener` so E and F are observable.
3. **Per-group commit tracking in harness** — `wait_for_commits` currently polls a single
   `listener_`; multi-group recovery needs per-group resolution (see B2 investigation).
4. **Vote/RPC drop flips** — for D3 (drop `request_vote` / `append_entries` to force split windows).

## Known fixes recorded (regressions to keep)

- **D4 connect-failed stall**: failed `PeerOutboundSocket` was reused forever from the factory's
  per-reactor map → post-restart election stall. Fixed: terminal `failed_` flag + factory drops it
  and reopens on next send. Verified 5/8→7/8 on the alone/together stress loop.
- **Lower-layer (prereq for persistent backend)**: MetaBlk shared-handle refactor (no in-place
  chain divergence) and AppendByteStream destroy ordering (no re-append cycle) — meta 15/15,
  cow_btree 14/14.
- **Sync/async btree ODR-ABI collision**: `hs_mem_btree` (sync) removed from the `homestore`
  aggregate — one btree mode per binary, ever; async callers were entering the sync
  `upgrade_node_locks` with sret-shifted registers.
- **Destroyed-group resurrection**: only `join_cluster_request` may create a ReplicaSet on demand;
  any other message for an unknown group gets SERVER_NOT_FOUND. Group creation is invitation-based
  (self-only config → BecomeLeader-latched wait → `add_member` loop), matching upstream.
- **Joiner self-destroy**: eviction detection moved off commit_config's my-id-absent scan (misfired
  on catch-up replay of pre-add configs) onto nuraft's `RemovedFromCluster` event.
- **COWBtree recovered-root loss**: `load_cow_btree` passed the caller's stale listed SB root to the
  Btree ctor → fresh empty root orphaned the recovered tree every boot (masked by full-log replay
  before the empty-replay-window design). Guarded by `GenericPutRestartLoop` in the cow local suite.
- **Shutdown-window lifecycle (LeaderQuickRestart SEGV)**: an inbound join dispatch that entered
  before `stop()` kept running against a manager mid-teardown — it read `rpc_client_factory_` after
  the reset and handed nuraft a null factory, detonating at first remote-peer construction inside
  `reconfigure` (`peer.hxx:47`).  Fixes: RCU-published `stopping` gate (two-phase `make()` only —
  never call the blocking `synchronize_rcu` on a reactor) for new entries, and
  `FollyRpcListener::shutdown_and_drain()` awaited FIRST in `stop()` — dispatch coroutines own their
  connection via shared_from_this(), so the drain waits for live connection objects to reach zero;
  the accept path checks `draining` in the same critical section as its registry insert.
- **Formation loses an invited member**: `add_member` "accepted" only means the join invitation was
  dispatched; an invitation dying in flight (target restarting mid-handshake) is never retried by
  the engine and the member silently never joins — the group forms undersized and writes commit on
  a 2/3 quorum while the orphan waits forever.  Fix: `create_replica_set` confirms each member via
  `ReplicaSet::has_member()` (srv present in raft config) and re-invites on timeout (idempotent —
  SERVER_ALREADY_EXISTS maps to success).
- **Election timers fired at most once** (`FollyEventBaseScheduler`): nuraft re-schedules the SAME
  cancelled task (restart_election_timer = cancel + schedule) and the scheduler owns un-cancelling;
  ours never called `task->reset()`, so any timer that fired once (or was ever cancel+re-armed, i.e.
  every follower's) was dead forever — no re-election retries, no takeover, cluster-wide.  Fix:
  synchronous `reset()` in schedule() (MUST be sync — deferring it onto the EventBase re-orders it
  after a shutdown's cancel and resurrects a timer bound to a freed raft_server: that exact UAF was
  caught by ASan) + per-task generation filter for superseded `runAfterDelay` armings.
- **Destroy leaked across clean restart**: `load()` never restored the stage from the SB's
  `destroy_pending`, so a reloaded pending destroy was invisible to the reaper (it matches on the
  in-memory stage) and the group leaked forever.  Fix: re-stage DESTROYED on load and re-arm
  `destroyed_time_` from boot (full grace window restarts).  Guarded by `DestroyPendingRestartReap`.
- **Log-store-id reuse resurrects dead records** (caught by `BootOrderRotation`): a destroyed
  store's records stay in the shared stream until truncation passes them, but `next_store_id_`
  reseeded only from surviving store SBs — a boot with zero SBs reset it to 0, a later group
  recycled the sid, and recovery (which routes records by sid alone) delivered the dead group's
  records into the live one: its CTRL_DESTROY destroyed a healthy group, its data records applied
  as foreign keys.  Fix: the recovery walk raises the id floor per record
  (`atomic_update_max(next_store_id_, sid + 1)`), so an id with records still in the stream can
  never be re-issued; truncation erasing the records replenishes the id space naturally.
- **Harness: leader identity is never a safe deferral signal** — a stale claim (claimed leader is
  down) and a legitimately returned leader are indistinguishable to an observer; both deadlocked the
  rounds when used as exits.  `write_on_leader` non-leaders exit only on commit-count proof
  (counts are cumulative across in-process restarts: the helper re-binds the same listener on
  recovery).
- **Diagnostic recipe for multi-process hangs** (ptrace is blocked for non-ancestors): remove the
  stale `/var/crash/*.crash` for the binary (apport dedups per executable), tgkill SIGABRT the
  suspect tid, `apport-unpack`, then gdb the CoreDump offline. NOTE: `SweepService` / `TwoQEvictor`
  worker threads inherit the creating reactor's `HSReactor*` thread name — don't chase them.
