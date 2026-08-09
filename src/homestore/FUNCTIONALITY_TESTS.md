# HomeStore Functionality Test Program

The complete functional test checklist for HomeStore as a library, derived from the public API surface
of every module and the enumerated cases of every existing test binary.  When every box is checked,
HomeStore is functionally tested with very high confidence.  The phases after this program are
**limits**, **resiliency**, **random-failure**, and **long-running**; tests that already seed one of
those are tagged, e.g. `[resiliency]`.

Sections needing product work before their tests can exist are marked **[needs product work]**.

Legend: `[x]` passing (run in this effort, seen green) · `[ ]` to do (no test, or currently red —
red items name their blocker).

**Current score: 56 checked, 74 pending — of the 74: 49 are pure test-writing (product ready,
including 17 crash windows whose flips are already in the code), 20 are blocked on product work
(membership §10: 11, snapshot §11: 9), 4 on `blob_opt` config enablement (§13),
1 downstream of §11 (§12 snapshot redirect).**  Every `[x]` is backed by a run of the named binary in this effort.  Latest full
sweep (2026-08-09): 17 binaries — every test binary in the tree, including replication — 311 tests,
all green, zero ASAN reports.

---

## 1. Device layer (physical devices, chunks, virtual devices)

Existing coverage (`test_dev_mgr` 9/9, `test_pdev` 14/14, `test_vdev` 21/21)
- [x] Format + first-time boot detection; load after format; vdev persistence across restart
      (FormatAndFirstTimeBoot, LoadAfterFormat, VdevPersistAcrossRestart)
- [x] Vdev create striped / multiple vdevs / destroy; vdev-id alloc+free; capacity queries
      (CreateVdevStriped, CreateMultipleVdevs, DestroyVdev, AllocateAndFreeVdevId, CapacityQueries)
- [x] Chunk lifecycle: single + batch create, remove+recreate, load after restart,
      deactivate/reactivate (pooling), dynamic creation, random op sequences
      (ChunkCreate*, ChunkRemoveAndRecreate, ChunkLoadAfterRestart, ChunkDeactivateReactivate,
      DynamicChunkCreation, RandomChunkOps)
- [x] Pdev IO: first-block read, superblock verify, single-block and large-buffer write/read,
      scatter-gather, fsync, interleaved concurrent writes
- [x] Vdev IO and allocation: alloc/write/read single+multi block, writev/readv, alloc-free-realloc,
      format zeroing, fsync
- [x] Vdev growth/shrink: expand, shrink last/specific, chunk-pool reuse, most-available-space
      selector (ExpandChunk, ShrinkChunk*, ChunkPoolReuse, MostAvailableSpaceSelector)
- [x] Concurrency: concurrent writes (single/multi pdev, multi chunk), concurrent expand
      `[resiliency]`
- [x] Global chunk-id uniqueness across striped pdevs; chunk-id pool rebuilt on recovery
      (StripedChunkIdsGloballyUnique, ChunkIdPoolRebuiltOnRecovery)

Missing functional coverage
- [ ] Degraded-mode boot: restart with fewer devices than formatted — detected, flagged, still boots
- [ ] Data vs Fast device types: placement honored; per-type capacity queries

Crash windows (all to do; the crash points exist in product code)
- [ ] After formatting, before the format commit — next boot cleanly re-formats
- [ ] After a chunk's info record, before its slot bit — the chunk vanishes harmlessly
- [ ] After a chunk is freed, before its bit clears — no resurrection, live or pooled  `[resiliency]`
- [ ] After a vdev's chunks are created, before the vdev record — dangling-chunk sweep reclaims all
- [ ] After the vdev record, before its slot bit — vdev never loads; chunks reclaimed
- [ ] After a vdev record is freed, before its chunks are removed — recovery finishes the removal
- [ ] After a vdev's chunks are removed, before its slot bit clears — stale-slot sweep frees the slot

## 2. Block allocator and sweep service

Existing coverage (`test_blkalloc` 9/9)
- [x] Compact allocator alloc/free; expanded allocator contiguous + scatter allocation across size
      distributions (uniform/rounded/slab-random/one-size), with and without slab caches
      (compact_alloc_free, expanded_contiguous_*, expanded_scatter_*, small_allocator_with_slab)

Missing functional coverage
- [ ] Persistent bitmaps survive restart; non-persistent rebuild from commits (exercised implicitly
      by upper layers, no direct test)
- [ ] Uncommitted allocations revert to free after restart — the crash contract of all blk data
- [ ] Allocation hints: chunk pinning (stream isolation depends on it)
- [ ] Sweep service directly: registration, background refill, blocking refill under pressure,
      shutdown drain  `[limits]`

## 3. Metablk service

Existing coverage (`test_meta_blk_mgr`) — verified 15/15 in this effort
- [x] Create/drop/reload; client registration across restart; write+restart validation; remove some /
      remove all + restart; deregister + restart (CreateDropReload, RegisterClientsRestart,
      WriteDataRestartValidate, Remove*Restart, DeregisterClientRestart)
- [x] Overflow blocks; in-place update; size transitions inline↔overflow (OverflowBlocks,
      UpdateInPlace, SizeTransitions)
- [x] Chain-integrity torture: rewrite non-tail via cached handle, rewrite head repeatedly + restart,
      remove chain then re-add then restart, multiple restarts (RewriteNonTail…, RewriteHead…,
      RemoveChainThenReAddThenRestart, MultipleRestarts)  `[long-running]`
- [x] Many clients in parallel; payload CRC integrity (ManyClients, CrcIntegrity)

Missing functional coverage
- [ ] Client-slot exhaustion (255) and block-name length limits behave predictably

Crash windows (all to do; generic crash points exist)
- [ ] Before a block's payload write — reads back previous contents
- [ ] After payload, before chain link — orphan reclaimed, never in any chain
- [ ] After unlink, before storage free — no ghost, space recovered
- [ ] All three with overflow payloads

## 4. Blob device and streams

Existing coverage (`test_raw_blk_stream` 20/20, `test_append_blk_stream` 15/15,
`test_append_byte_stream` 21/21)
- [x] Raw block stream: create, multiple streams, single/multi-block write+read, alloc/commit/
      invalidate lifecycle, expand on demand, bulk verify, CP flush, scatter-gather, invalidate
      waits for in-flight reads, write-flush-free-flush, fsync, destroy
- [x] Raw stream recovery: single block, multi-chunk, multiple streams, after free, double restart,
      block-size multiplier (incl. restart)
- [x] Append block stream: quick-append fast path, append+flush+read, multi-segment and multi-block
      appends, invalidate, CP switchover+flush, expand, bulk verify, concurrent appends across
      segments `[resiliency]`; recovery incl. after-invalidate, multiple streams, double restart
- [x] Append byte stream: contiguous appends, larger-than-block, chunk-spanning, partial tail
      carried across flush, reads across chunk boundaries, sequential + range cursors, truncate
      (keeping and releasing chunks), empty-flush no-op, concurrency on and off
      (ConcurrentAppendsSerialized, ConcurrentSafeOff); recovery: single/multi chunk, partial tail,
      after truncate, double restart, extended lifecycle  `[long-running]`
- [x] Created-but-never-flushed streams recover with their chunks attributed

Missing functional coverage
- [ ] Stream isolation under one shared vdev: streams never allocate from each other's chunks
- [ ] `reconcile_chunks` after unclean stops; multiple BlobDevs side by side

## 5. Log store and log stream

Existing coverage (`test_log_store` 18/18, `test_log_stream` 17/17, `test_log_store_mgr` 5/5 —
store create/drop/reload lifecycle across restart)
- [x] Store basics: append+readback, concurrent appends, out-of-order (non-append-mode) writes then
      truncate, holes filled by later writes, flush durability, out-of-range reads
- [x] Multiple stores on one stream — routing at replay, per-store floors (MultipleStoresOnOneStream,
      MultiStoreOneStream)
- [x] Truncation: global min across stores (incl. across restart, no-op on empty store), partial and
      truncate-all across restart, multi-chunk release
- [x] Rollback records: basic, persist across restart, rollback+append cycles with and without
      restart
- [x] Replay: order preserved, append-restart-append-restart cycles  `[long-running]`
- [x] Stream mechanics: group framing (single/multi record, multi group per flush), size- and
      timer-driven auto-flush, highly-concurrent append+timer flush `[resiliency]`, read-by-key,
      growth by expand
- [x] Torn-write handling: torn middle write DETECTED (refuses), legitimate torn tail at chain end
      truncated cleanly (RestartTornMiddleWriteDetected, RestartLegitimateTornTailAtChainEnd)

Missing functional coverage
- [ ] Orphan stores (no replay handler) dropped at recovery, storage reclaimed
- [ ] Store-id floor: dead stores' stream records never resurrect into recycled ids (product fix
      exists; needs its regression test)
- [ ] Auto-truncate timer stores; preserve-log-count

Crash windows (points exist)
- [ ] After truncation releases chunks, before the stream header updates
- [ ] After truncate-to-empty persists, before the seed refreshes — no resurrection  `[resiliency]`
- [ ] At a rollback-record persist
- [ ] Between subsystem flushes inside one checkpoint — watermark stays behind certified data

## 6. Checkpoint manager

Existing coverage (`test_cp_mgr` 8/8)
- [x] Initial CP creation, simulated IO + flush, back-to-back CPs, nested CP guards, cp-id advance
      on flush and survival across restart, IO running parallel to a flush, flushed-query
      (BackToBackCP, NestedCPGuard, CPIdSurvivesRestart, IOParallelToFlush, HasCPFlushed)

Missing functional coverage
- [ ] Watchdog: stalled checkpoint detected and reported  `[resiliency]`
- [ ] Consumer registration: rank ordering enforced, duplicate rank rejected

Crash windows (point exists)
- [ ] After every consumer flushed, before the CP superblock advances — checkpoint never-taken;
      next boot resumes the same cp id

## 7. Btree (COW persistent + in-memory production backend)

Existing coverage (`test_cow_btree_local` 22/22, `test_cow_btree_io` 32/32, `test_btree_node` 36/36,
`test_mem_btree` 32/32)
- [x] Basic durability: create+read, read-modify-write in one CP, remove subset, RMW across many
      incremental CPs, incremental→full→incremental transitions
- [x] Restart matrix: after incremental, after full, mixed flushes, between every CP, write-after-
      restart, remove-after-restart, many-nodes incremental and full  `[long-running]`
- [x] Destroy: destroy-then-recreate on the same BlobDev then restart (ordinal + stream reuse)
- [x] Concurrency: multi-threaded insert, concurrent CP+insert, concurrent RMW with CP, concurrent
      CP+destroy, multiple btrees concurrently, lock-upgrade under CP, prepare-retry under CP
      switchover  `[resiliency]`.  ConcurrentCpAndDestroy caught two real torn-superblock bugs
      (payload mutated concurrently with the metablk write; chain relink persisting a stale crc) —
      both fixed via the MetaBlk ownership model (Exclusive/Shared, buf/mutate_buf) and the chain
      crc restamp.
- [x] Crash-regression guard: generic put/restart loop (GenericPutRestartLoop)
- [x] Full op suite over IO backend: sequential/random insert, removes, range update/remove,
      concurrent all-ops (`test_cow_btree_io`)
- [x] Node-level ops: sequential/reverse/random insert, remove, range put/get, update, move
      (`test_btree_node`)
- [x] In-memory btree backend (production, sync mode): full op suite (`test_mem_btree`)

Missing functional coverage
- [ ] User-superblock payload round-trips create/restart
- [ ] Overflow (multi-block) node coverage as its own case
- [ ] Full-map flush threshold: automatic full-map trigger by delta volume (transitions are tested;
      the threshold trigger is not)

Crash windows
- [ ] Between a full-map flush and the superblock write — previous generation + journal replay
- [x] Torn incremental-journal tail ends replay cleanly; journal checksum verified

## 8. Resource manager and events  **[no test binary exists]**

- [ ] Capacity accounting: fast/data free-bytes vs real usage
- [ ] Cache sizing from system memory; btree cache respects it  `[limits]`
- [ ] Disk-full on write → log-space reclaim → writer retry succeeds  `[resiliency]`
- [ ] Log-stream space pressure event → truncation honors replication floors
- [ ] Event manager publish/subscribe/reset under concurrency

## 9. Replication — writes, restarts, crash recovery, elections
(three-process cluster, persistent btree state machine — all verified this effort)

Normal writes
- [x] Replicated writes with cross-replica validation
- [x] High queue-depth concurrent writes  `[limits]`
- [x] Sequential back-to-back batches

Clean restarts
- [x] Whole-cluster restart ×6 rounds, empty replay each  `[long-running]`
- [x] Followers restart one at a time under live writes
- [x] Leader quick restart (blip inside survivors' election window)
- [x] Leader slow restart (survivors take over; old leader returns, catches up)
- [x] Slow replica converging through rolling restarts  `[resiliency]`
- [x] Destroy interrupted by whole-cluster restart — reaper finishes, no leak
- [x] Follower restart mid-append-stream, no quiescing  `[resiliency]`
- [x] Both followers down at once — writes stall, resume exactly-once  `[resiliency]`
- [x] Full-cluster restart with rotating boot order  `[random-failure]`
- [x] Restart churn with zero writes between  `[long-running]`
- [ ] Member restarts while a join invitation is mid-handshake (needs join-dispatch delay flip)
- [ ] Leader restarts holding accepted-but-unreplicated entries — no duplicates

Crash recovery
- [x] Crash after apply, before checkpoint — proof-gated replay, unproven tail to consensus
      `[resiliency]`
- [x] Crash after log-durable, before apply — re-commit via consensus, no double-apply
- [x] Crash at the destroy-pending superblock write — replayed destroy re-executes, no leak
- [x] Crash at the term/vote persist during an election — vote safety holds

Elections
- [x] Elections settle from every boot order; stale claims never absorb writes
- [x] Dead outbound connections dropped and rebuilt during elections
- [ ] Dropped vote messages (split windows) — eventually elects, never two leaders per term
- [ ] Leader killed outright — re-election inside the window, writes resume
- [ ] Rollback of divergent entries after leader change — data + config, with/without crash

## 10. Replication — membership and leadership operations  **[needs product work: membership hooks
      are stubs]**

- [ ] Add a member: spare joins, catches up, config commits everywhere, notification fires
- [ ] Add as learner (no quorum impact), promote via learner flip
- [ ] Remove a member: quorum recomputes; removed node self-evicts, destroys local state
- [ ] Replace end-to-end: learner staging, gated catch-up, promote, complete — task id through both
      hooks, superblocks symmetric
- [ ] Replace interrupted by restart: resumes from persisted progress
- [ ] Membership change under active writes  `[resiliency]`
- [ ] Crash mid-membership-commit — every member heals to one consistent config
- [ ] Election priority changes propagate and influence elections
- [ ] Leadership transfer under writes
- [ ] Role-change notifications on every transition
- [ ] Status/active-peer introspection reflects true membership and lag

## 11. Replication — snapshot and baseline resync  **[needs product work: snapshot hooks are stubs]**

- [ ] Leader snapshots at an LSN via the builder; recorded and releasable
- [ ] Log compaction after snapshot; recovery = snapshot + tail
- [ ] Deeply-lagging follower resyncs via chunked transfer, not log replay
- [ ] Transfer interrupted — partial discarded, refetched  `[resiliency]`
- [ ] New member joins past a compacted log — baseline then tail
- [ ] Snapshot + restart: commits at/below snapshot LSN skipped on replay
- [ ] Crash during snapshot create (leader) — atomic, clean retry
- [ ] Crash during install (receiver) — partial discarded, refetched
- [ ] Resync completes under active writes

## 12. Replication — log compaction and space reclaim

- [ ] Replicated truncate ceiling commits everywhere, clamps local compaction
- [ ] Pressure-driven truncation respects replication floors
- [ ] Follower lagging past compaction redirected to the snapshot path (with §11)

## 13. Replication — large log entries (`blob_opt` indirect payloads)  **[needs config enablement —
      disabled in every config; zero coverage]**

Large raft log entry payloads stored indirectly by HomeRaftLogStore: the payload goes to separate
blocks through a blob stream (`IndirectBlkHandler`), the log record carries the references, and the
deferred-free journal returns the blocks when the log truncates past the referencing LSN.

- [ ] Indirect append: a large entry's payload lands on separate blocks via the blob stream, the log
      record carries the references, read back across restart
- [ ] Truncation reclaim: referenced blocks enter the deferred-free journal and return to the
      allocator when the log truncates past the referencing LSN
- [ ] Mixed inline/indirect workload with restart
- [ ] Crash between the payload-block writes and the log record naming them — blocks revert to free

## 14. Whole-store lifecycle

- [x] Format → use → clean shutdown → recovery, exercised continuously by every suite
- [x] Recovery correctness at every layer: checkpoint resume, replay floors, map generations,
      stream attribution, replica-set reconstruction, destroy resumption
- [ ] Degraded boot at store level (missing device) with all managers up
- [ ] Format options honored: chunk sizes, initial counts, per-type placement
- [ ] Settings changes surviving restart

---

## Completion claim

When sections 1-14 are fully checked — including the product work sections 10 and 11 require and the
`blob_opt` config enablement section 13 requires — HomeStore is functionally tested with very high
confidence:
every public operation of every module, every clean-restart path, and every crash window in its
durable-write orderings has a test asserting exact behavior.  That is the full functionality program.

Until then no such claim can be made — membership, snapshot/resync, indirect log payloads, and the
resource manager are core functionality currently untested (and in part unimplemented at the
listener boundary).

Next phases build on this: **limits**, **resiliency**, **random-failure**, **long-running** — the
tagged tests above are their seeds.
