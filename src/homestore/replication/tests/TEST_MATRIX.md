# Replication Negative-Test Matrix (with flip / crash injection)

Single multi-process binary: `test_replica_set` (one OS process per replica, boost interprocess
barriers). Backend is the PERSISTENT COWBtree store. Fault injection uses the **Flip** framework
(`flip::FlipClient::instance().inject_{noreturn,delay,retval}_flip(...)`) and, for crashes, the
**CrashSimulator** (`base/crash_simulator.h`, gated on `iomgr_flip::test_flip(<name>)`), which
aborts the process at a named point so the harness can restart and validate recovery.

Legend — Status: ✅ implemented & passing · 🟡 implemented, flaky · ⛔ not implemented (TODO) · ▫ infra missing.

---

## A — Functional (no faults)

| ID | Test | Scenario | Flip / crash point | Expected | Status |
|----|------|----------|--------------------|----------|--------|
| A1 | `SingleWrite` | one key replicated | — | all 3 replicas commit+apply 1 | ✅ |
| A2 | `ReplicatedWrites` | N writes, validate | — | all commit, values match | ✅ |
| A3 | `HighQdepthWrites` | N in flight concurrently | — | all commit exactly once, ordered | ✅ |
| A4 | `SequentialBatches` | back-to-back batches | — | cumulative count exact | ✅ |
| A5 | large-value (indirect blk) write | value ≥ indirect threshold | — | blob stream path, commit, re-read | ⛔ |

## B — Recovery (clean restart, no fault)

| ID | Test | Scenario | Flip / crash point | Expected | Status |
|----|------|----------|--------------------|----------|--------|
| B1 | `WriteRestartValidate` | write, restart all, validate | — | recovered log replays, count exact | ✅ |
| B2 | `CheckpointRestartValidate` | write, CP, restart, validate | — | btree-durable set recovers; on_commit re-fired | 🟡 (0-commits flake under trace) |
| B3 | `WriteCheckpointWriteRestart` | write, CP, write more, restart | — | CP'd half from btree + post-CP tail replayed | ✅ |
| B4 | `WriteRestartWrite` | write, restart, write again | — | reformed cluster re-elects, accepts new LSNs | ✅ |
| B5 | `MultipleRestarts` | two reboots back-to-back | — | replay idempotent, no loss/dup | ✅ |

## C — Crash matrix (CrashSimulator flip at a named point, then restart + validate)

For every row: inject the crash flip on the target replica (leader/follower as noted), let the
process abort at that point, restart, then assert no data loss, no double-apply, chain/SB
consistent, group re-forms.

| ID | Crash point (flip name) | Injected where | Restart expectation | Status |
|----|-------------------------|----------------|---------------------|--------|
| C1 | after raft log append, before commit | leader | uncommitted tail dropped or re-committed; no dup | ⛔ |
| C2 | after commit (state-machine apply), before CP | any | recovered via log replay; count exact | ⛔ |
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

1. **CrashSimulator flip points** — add named crash flips at the C1–C11 sites (raft append,
   post-commit, post-CP, `write_sb`, metablk link, truncate, snapshot, membership).
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
