# HomeStore Functionality Test Program

The complete functional test checklist for HomeStore as a library, derived from the public API surface
of every module and the enumerated cases of every existing test binary.  When every box is checked,
HomeStore is functionally tested with very high confidence.  The phases after this program are
**limits**, **resiliency**, **random-failure**, and **long-running**; tests that already seed one of
those are tagged, e.g. `[resiliency]`.

Sections needing product work before their tests can exist are marked **[needs product work]**.

* Legend: `[x]` passing (run in this effort, seen green) · `[ ]` to do (no test, or currently red —
red items name their blocker). Every `[x]` is backed by a run of the named binary in this effort.  

* Total actual test cases — <b><span style="color:#2ea043">347</span></b> tests, <b><span style="color:#1f6feb">17</span></b> binaries

* Total Sections of test case (checklist count) : <b><span style="color:#bf8700">130</span></b>
  
* Current score: <b><span style="color:#2ea043">95</span></b> checked, <b><span style="color:#cf222e">35</span></b> pending — of the <b><span style="color:#cf222e">35</span></b>:
  * <b><span style="color:#bf8700">10</span></b> are pure test-writing (product ready, <b><span style="color:#bf8700">0</span></b> crash windows remaining — every wired crash flip is now exercised),
  * <b><span style="color:#cf222e">20</span></b> are blocked on product work (membership §10: <b><span style="color:#cf222e">11</span></b>, snapshot §11: <b><span style="color:#cf222e">9</span></b>),
  * <b><span style="color:#cf222e">4</span></b> on `blob_opt` config enablement (§13),
  * <b><span style="color:#cf222e">1</span></b> downstream of §11 (§12 snapshot redirect).

* §9 (replication — writes, restarts, crash recovery, elections) is <b><span style="color:#2ea043">COMPLETE</span></b>: <b><span style="color:#2ea043">25</span></b> of <b><span style="color:#bf8700">25</span></b>.

---

## 1. Device layer (physical devices, chunks, virtual devices)

Existing coverage (`test_dev_mgr` 18/18, `test_pdev` 14/14, `test_vdev` 21/21)
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
- [x] Degraded-mode boot: format with 3 devs, reload with 2 → is_boot_in_degraded_mode() and
      surviving pdev set (DegradedModeBoot)
- [x] Data vs Fast device types: mixed pool, per-type capacity queries and per-type vdev placement
      (DataVsFastDeviceTypes)
- [x] Crash after format, before format commit — next boot is first-time again
      (CrashBeforeCommitFormatting)
- [x] Crash after chunk info record, before slot bit — load_chunks iterates SET bits only, chunk
      vanishes harmlessly (CrashAfterChunkInfoWriteBeforeSlotBit)
- [x] Crash after chunk freed, before bit clears — vdev-destroy path exercised; recovery's
      stale-slot cleanup removes the chunk (CrashAfterChunkInfoFreeBeforeBitClears).  Note flagged:
      PhysicalDev::load_chunks materializes ChunkInfos at set bits without an is_allocated() check;
      the vdev-destroy path is safe because the vdev-level stale-slot sweep catches it, but a
      chunk-level shrink following this crash would surface a resurrection — product follow-up.
      `[resiliency]`
- [x] Crash after vdev chunks created, before vdev record — dangling-chunk sweep reclaims
      (CrashAfterVdevChunksCreate)
- [x] Crash after vdev record, before slot bit — slot never set, vdev not loaded, chunks reclaimed
      (CrashAfterVdevInfoWriteBeforeSlotBit)
- [x] Crash after vdev record freed, before chunks removed — stale-slot cleanup completes the
      chunk removal on next boot (CrashAfterVdevInfoFreeBeforeChunksRemoved)
- [x] Crash after vdev chunks removed, before slot bit clears — stale-slot cleanup frees the slot
      (CrashAfterVdevChunksRemoveBeforeSlotBit)

## 2. Block allocator and sweep service

Existing coverage (`test_blkalloc` 12/12)
- [x] Compact allocator alloc/free; expanded allocator contiguous + scatter allocation across size
      distributions (uniform/rounded/slab-random/one-size), with and without slab caches
      (compact_alloc_free, expanded_contiguous_*, expanded_scatter_*, small_allocator_with_slab)
- [x] Persistent bitmap survives restart AND uncommitted allocations revert to free (the crash
      contract of all blk data): alloc+commit A (durable), alloc B without commit, `acquire_buffer()`
      to capture serialized ondisk state, destroy, reconstruct from that buffer, `recovery_completed()`;
      assert `is_blk_alloced_on_disk(A)==true`, `is_blk_alloced_on_disk(B)==false`
      (PersistentBitmapSurvivesUncommittedReverts)
- [x] Non-persistent rebuild from commits: fresh non-persistent allocator, alloc+commit blocks,
      destroy, recreate empty, replay `commit(bid)` for each block (as upper-layer load does), verify
      inmem bitmap restored (NonPersistentReplayedCommitsRebuild)
- [x] Allocation hints: chunk pinning — covered by §4's StreamIsolationSharedVdev, which asserts
      alloc-with-`chunk_id_hint` lands only in the calling stream's chunks (VirtualDev routes via
      select_chunk_for_alloc at virtual_dev.cpp:619, alloc_blks with hint uses max_attempts=1)
- [x] Sweep service directly: registration (ExpandedAlloc auto-registers on ctor), blocking refill
      under pressure (heavy allocs succeed past initial drain via request_refill_blocking), shutdown
      drain (allocator dtor drains in_flight_ before releasing sweep_handle_)
      (SweepServiceRegisterRefillAndDrain).  Product fix alongside: SlabBlkAllocator dtor now sets
      `alive_=false` and spin-waits `in_flight_` on the sweep handle before releasing the shared_ptr.
      Queued refill tasks pin the handle alive via their own shared_ptr, so relying on
      `~AllocatorHandle` alone let late-firing tasks pass their alive check and touch freed
      `inmem_bm_` on the sweep worker thread (real ASAN heap-use-after-free reproduced by the test
      before the fix)  `[limits]`

## 3. Metablk service

Existing coverage (`test_meta_blk_mgr`) — verified 22/22 in this effort
- [x] Create/drop/reload; client registration across restart; write+restart validation; remove some /
      remove all + restart; deregister + restart (CreateDropReload, RegisterClientsRestart,
      WriteDataRestartValidate, Remove*Restart, DeregisterClientRestart)
- [x] Overflow blocks; in-place update; size transitions inline↔overflow (OverflowBlocks,
      UpdateInPlace, SizeTransitions)
- [x] Chain-integrity torture: rewrite non-tail via cached handle, rewrite head repeatedly + restart,
      remove chain then re-add then restart, multiple restarts (RewriteNonTail…, RewriteHead…,
      RemoveChainThenReAddThenRestart, MultipleRestarts)  `[long-running]`
- [x] Many clients in parallel; payload CRC integrity (ManyClients, CrcIntegrity)
- [x] Client-slot exhaustion (255) throws, deregister-then-register refills; client-name > 231 chars
      silently truncates (ClientSlotExhaustionAndNameLimits).  MetaBlk name > 31 chars is a
      HS_DBG_ASSERT (debug-abort) — documented, not exercised (no death-test pattern in repo)
- [x] Crash before a block's payload write — recovery reads back prior contents
      (CrashBeforeSbWriteInline, CrashBeforeSbWriteOverflow)
- [x] Crash after payload, before chain link — orphan invisible to chain walk, storage reverted via
      blkalloc's uncommitted-allocations-revert contract
      (CrashBeforeSbLinkedInline, CrashBeforeSbLinkedOverflow)
- [x] Crash after unlink, before storage free — ghost invisible to chain walk, storage reverted via
      the same uncommitted-revert path when the crash-removed block had not been re-committed via a
      prior MetaClient::load chain walk (CrashDuringSbRemoveInline, CrashDuringSbRemoveOverflow).
      Note: a crash-remove following a full close+reload cycle would leave the alloc bit committed
      and orphaned — reclaim then requires an explicit sweep that does not exist today; flagged for
      product follow-up, not exercised here

## 4. Blob device and streams

Existing coverage (`test_raw_blk_stream` 23/23, `test_append_blk_stream` 15/15,
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
- [x] Stream isolation under one shared vdev: chunk-id sets between two streams are disjoint and
      every alloc lands only in the calling stream's chunks (StreamIsolationSharedVdev).  Enforced
      by RawBlkStream::alloc_blk's per-chunk `chunk_id_hint` walk (raw_blk_stream.cpp:75-84).
- [x] reconcile_chunks after unclean stops — chunk created on vdev but its per-chunk MetaBlk never
      written (simulated by calling vdev.expand directly, bypassing StreamBase::init_chunk_mblk);
      BlobDev::load's reconcile_chunks sweeps the orphan on recovery (ReconcileChunksAfterUncleanStop).
      Multiple BlobDevs side by side: two BlobDevs each backed by their own vdev, independent streams
      and writes, both recover with data intact (MultipleBlobDevsSideBySide).

## 5. Log store and log stream

Existing coverage (`test_log_store` 18/18, `test_log_stream` 17/17, `test_log_store_mgr` 11/11)
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
- [x] Orphan stores dropped at recovery, storage reclaimed via LogStoreManager::truncate after drop
      (OrphanStoreStorageReclaimed).  Existing DropUnopenedStores also covers the "dropped" half.
- [x] Store-id floor: destroy a store without truncating its records, restart, verify a fresh
      create_log_store returns sid > destroyed_sid — replay walks the stream and raises next_store_id_
      past every seen sid, preventing recycled-id records from resurrecting into a new store
      (StoreIdFloorProtectsDeadRecords)
- [x] Auto-truncate timer opts stores into the manager's periodic compact loop; preserve_log_count
      leaves N records past the compact point (AutoTruncateWithPreserveLogCount)
- [x] Crash before LogStream truncate-commit — flip `crash_before_logstream_truncate_commit` fires
      between AppendByteStream chunk release and the LogStream sb refresh (init_crc bump).  On
      recovery, stale groups in the kept anchor chunk do NOT resurrect as new records
      (CrashBeforeLogstreamTruncateCommit).  Renamed from the old `crash_before_logstream_seed_refresh`;
      field `chain_seed`/`chain_seed_` renamed to `init_crc`/`init_crc_` alongside.  `[resiliency]`
- [x] Crash before LogStore rollback-commit — flip `crash_before_logstore_rollback_commit` fires
      between the in-memory rollback (records_.rollback + tail_lsn update) and the sb persist.  On
      recovery, replay delivers all pre-rollback records (the rollback effectively never happened
      from a durability perspective) (CrashBeforeLogstoreRollbackCommit)
- [x] Cross-consumer CP crash — watermark stays behind certified data.  A test CPCallbacks consumer
      registered at rank 500 fires a crash flip inside its cp_flush; LogStore (rank 999) runs
      afterward but its sb writes are gated by is_crash_simulated().  On recovery, LogStore's
      persisted checkpt_lsn == pre-crash value — the watermark did NOT race ahead of the durable
      state (CrossConsumerCpCrashWatermarkNotAdvanced)

## 6. Checkpoint manager

Existing coverage (`test_cp_mgr` 10/10)
- [x] Initial CP creation, simulated IO + flush, back-to-back CPs, nested CP guards, cp-id advance
      on flush and survival across restart, IO running parallel to a flush, flushed-query
      (BackToBackCP, NestedCPGuard, CPIdSurvivesRestart, IOParallelToFlush, HasCPFlushed)
- [x] Consumer registration: switchover and flush walks fire in ascending rank order regardless of
      registration order (ConsumerRankOrderingEnforced); duplicate rank triggers a debug assert
      (documented in CPRank header, not exercised — no death-test pattern in this repo)
- [x] Watchdog detects a stalled CP and invokes repair_slow_cp on lagging consumers
      (WatchdogDetectsStalledCP).  Uncovered three latent bugs in `watch_cp` fixed alongside:
      `||`→`&&` guard tautology that short-circuited every tick, inverted progress-advance check
      that never reset the stall timer, and misplaced HS_REL_ASSERT that never fired past the
      tolerance window.  The panic branch itself (elapsed past 12x-timer tolerance) is not
      test-exercised — asserting would kill the process and no death-test pattern exists here.
      `[resiliency]`

## 7. Btree (COW persistent + in-memory production backend)

Existing coverage (`test_cow_btree_local` 28/28, `test_cow_btree_io` 32/32, `test_btree_node` 36/36,
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
- [x] User-superblock payload round-trips create/restart
      (CowBtreeLocalUserSbTest.UserSuperBlockPayloadRoundtrip).  Product follow-up flagged:
      MetaClient::create_meta_blk ignores its `estimated_data_size` arg and always allocates
      one meta_vdev block (512 B); user_sb sizes past the inline capacity silently corrupt
      memory today (ASAN caught the OOB at 500 B).
- [x] Overflow (multi-block) node coverage — 4× block-size nodes exercised through insert / RMW /
      restart (CowBtreeLocalOverflowNodeTest.MultiBlockNodeRoundtrip)
- [x] Full-map flush threshold: automatic full-map trigger by delta volume advances
      last_full_map_cp_id without the force flip (FullMapFlushThresholdTrigger)

Crash windows
- [x] Crash after full-map flush, before SB write — recovery uses previous generation stream and
      replays the incr journal on top (CrashAfterFullMapFlush)
- [x] Crash after SB write, before passive-stream truncate — recover_full_map's stale-stream sweep
      completes the truncate on the boot path (CrashAfterFullMapSbWrite)
- [x] Crash after passive truncate, before incr_map truncate — stale incr records skipped on replay
      (all with hdr.cp_id < last_full_cp), storage reclaimed by the next successful full CP
      (CrashBeforeIncrMapTruncate)
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
- [x] Member reboots while its join invitation is in flight — the invitation is lost, the creator's
      retry lands after the reboot, membership completes (JoinInvitationInFlightRestart)
- [x] Leader dies holding accepted-but-unreplicated entries — orphaned tail resolved by consensus, no
      duplicates (LeaderCrashUncommittedTail)

Crash recovery
- [x] Crash after apply, before checkpoint — proof-gated replay, unproven tail to consensus
      `[resiliency]`
- [x] Crash after log-durable, before apply — re-commit via consensus, no double-apply
- [x] Crash at the destroy-pending superblock write — replayed destroy re-executes, no leak
- [x] Crash at the term/vote persist during an election — vote safety holds

Elections
- [x] Elections settle from every boot order; stale claims never absorb writes
- [x] Dead outbound connections dropped and rebuilt during elections
- [x] Dropped vote messages — election rounds die on the wire, cluster heals and elects exactly one
      leader, writes resume, never two leaders per term (DroppedVotesElection)
- [x] Leader killed outright mid-commit — survivors elect, interrupted batch completes, dead node
      rejoins and converges, writes resume (LeaderCrashOutright)
- [x] Rollback of divergent entries after leader change — a lone leader accepts an entry without
      quorum, the returning followers elect a new leader at a higher term, and the deposed leader
      rolls its orphaned entry back and adopts the cluster's history (RollbackDivergentEntries)
- [x] Same divergence with the deposed leader crashing mid-rollback — recovery completes the repair,
      no half-rolled-back state, cluster converges (RollbackDivergentCrash)  `[resiliency]`

## 10. Replication — membership and leadership operations  **[needs product work: membership hooks are stubs]**

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

## 13. Replication — large log entries (`blob_opt` indirect payloads)  **[needs config enablement — disabled in every config; zero coverage]**

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
