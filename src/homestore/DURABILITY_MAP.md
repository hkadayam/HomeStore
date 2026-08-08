# HomeStore Durability Map — superblock inventory, crash-window coverage, pressure points

Companion to `replication/tests/TEST_MATRIX.md`.

## TL;DR — the whole study in ten lines

Superblock crash windows alone do NOT cover all crash scenarios — bulk data (log records, btree nodes,
blob blocks, raft entries) is deliberately certified by its own mechanisms, not by its anchoring SBs.
But SB windows PLUS five ordering invariants (all of the shape "data lands before the record naming it")
DO cover everything, and crash flips already exist for most of them.  What actually needs action:

1. FIXED: `on_chunks_added` `return`-vs-`continue` bug (recovery dropped every chunk after a pooled one).
2. POINT ADDED (`crash_after_chunk_info_free`): the chunk-remove info-freed/bit-set resurrection window —
   test still owed.
3. DEVICE LAYER: six crash points now exist (`crash_before_commit_formatting`,
   `crash_after_chunk_info_write/free`, `crash_after_vdev_chunks_create`, `crash_after_vdev_info_write`,
   `crash_after_vdev_info_free`); the F7/F8 cleanup tests are still owed.
4. FIXED: all four checksums are now verified on read (FirstBlock, VDevInfo, metablk payload, cow journal
   footer), and a torn cow-journal tail now ends replay cleanly (WARN + stop) instead of crash-looping —
   completed-CP corruption still asserts, as it must.
5. Most cross-subsystem windows need NO new code — tests arming `crash_before_sb_write` with a
   client-name condition express them (CP rank tears, cow SB vs maps, raft cfg saves).
6. Settings JSON persists outside the durability model entirely (no anchor, no checksum, no CP).

Everything below is per-claim evidence with file:line anchors — reference, not reading material.

## The table — every area, what needs crash consistency, what needs work

Legend: 🎯 test needed (mechanism/flip exists) · ⛔ crash point AND test needed · ‼ product defect.

| Area / block | Info needing crash consistency | Protected today by | Needs work |
|---|---|---|---|
| FirstBlock (per pdev) | `formatting_done` — the format commit point | re-format on next boot; checksum verified on read | 🎯 test for `crash_before_commit_formatting` |
| PDevInfoHeader | geometry (`data_offset`, size, attrs), `system_uuid` | written once inside FirstBlock; uuid mismatch throws | — (dead `max_pdev_chunks` cleanup only) |
| Chunk bitmap + ChunkInfo[] | bit↔info pairing across add/remove; `chunk_allocated` pool transitions | ChunkInfo crc32 verified; info-first/bit-second order; `on_chunks_added` bug FIXED | 🎯 tests for `crash_after_chunk_info_write/free` (incl. resurrection case); bitmap unchecksummed |
| VDev slot bitmap + VDevInfo[] | bit↔info pairing across create/destroy (bit written LAST both ways) | F7/F8 cleanups; crc16 verified on read (corrupt = halt, never stale) | 🎯 F7/F8 tests via `crash_after_vdev_chunks_create/info_write/info_free` |
| MetaClientInfo slots | `slot_allocated`, `first_blkid` chain head | slot crc verified at load; `crash_during_sb_remove` covers head-advance | 🎯 tests |
| MetaBlk chains (all clients) | `next_bid` links, payloads | 3 generic flips; orphans self-heal; `data_crc` verified on read | 🎯 tests per client |
| CPSuperBlock | `m_last_flushed_cp` — the global cp gate | `crash_during_cp_flush`; rank-ordered flush | 🎯 dedicated test |
| Blob bitmap blocks | allocator state = sole validity predicate for blk data | data-before-bitmap order; generic flips | 🎯 test; blob_opt=off ⇒ zero runtime coverage today (A5) |
| AppendByte stream SBs | `head/tail/chunk_ids/chain_seed` vs released chunks | `crash_after_stream_chunk_release`, `crash_before_logstream_seed_refresh` | 🎯 two logstore-level tests |
| LogStore SBs | `checkpt_lsn` (replay floor/watermark), `head_lsn`, rollback records | CP rank order; C2 test GREEN | 🎯 rollback + truncate crash tests |
| COWBtree SB | `last_full_map_cp_id` join key, stream ids (`root_node_id` stale BY DESIGN — rides journal) | journal `cp_id` gates; footer checksum verified; torn tail ends replay cleanly | 🎯 full-map crash test |
| ReplicaSet SB | `destroy_pending` two-phase, log-store ids, `last_snapshot_lsn`, `replace_member_sb` | B6 test GREEN; SB↔config pairing | 🎯 replace-member crash tests (F); snapshot rides E |
| RaftConfig blk | cluster config, `{term, voted_for}` vote safety | pairing rule destroys widowed halves; saves via generic flip | 🎯 half-created-group + C9 tests |
| Settings JSON | raft timeouts, reaper intervals | nothing — outside the model | policy decision |

---

## 1. The durability hierarchy

```
Level 0  Device superblocks    FirstBlock, PDevInfoHeader, chunk slot bitmap + ChunkInfo[],
         (per pdev, fixed        vdev slot bitmap + VDevInfo[]           — written via
          offsets, no metablk)    PhysicalDev::write_super_block / write
Level 1  MetaBlk system         MetaBlkSuperHeader + 255 MetaClientInfo slots (block 0 of meta_vdev
                                 chunk 0), per-client singly-linked chains of 512B blocks
Level 2  Subsystem superblocks  6 production MetaClients: CPSuperBlock, BlobDevManager,
         (metablk payloads)      cow_btree_mgr, LogStoreManager, ReplicaSet, ReplicaRaftConfig
Level 3  Bulk data              log records, btree nodes + incr/full-map journals, blob blocks,
         (chunk contents)        raft entries — anchored BY level-2 SBs, validated by their OWN
                                 mechanisms (CRC chain, chain seed, cp_id gates, rollback records,
                                 commit-proof stamps, allocator bitmaps)
```

## 2. Verdict on "cover the SB crash windows and you cover everything"

**False as stated, true in refined form.**  Level-3 data is deliberately NOT certified by its anchoring
SBs:

| Data class | What the SB does NOT say | The real validity mechanism |
|---|---|---|
| Log records | LogStream SB `tail_offset` is a deliberate no-op on flush (`logstore/log_stream.cpp:544-549`) | forward CRC chain walk: `prev_crc`/`cur_crc` per group (`log_stream.cpp:411-426`), `chain_seed` for group 0 when head==0 (`:361-362,417`), torn-write probe (`:484-542`) |
| Log record *liveness* | chain says "bytes intact", not "still counts" | LogStore SB `head_lsn` gate + rollback_records `{above_lsn, max_log_id}` (`log_store.cpp:367-374`), checkpt floor only for watermark-registered stores (`:345-349`) |
| Btree root + nodes | SB `root_node_id` is stale by design after incr flushes | root rides the incr-journal header (`cow_btree.cpp:922-925`); journal records gated by `cp_id < cur_cp_id` (`:865-869`) and `cp_id > last_full_map_cp_id` (`:873-877`) |
| Blob/blk data | no per-block magic or CRC anywhere | the per-chunk allocator bitmap IS the validity predicate (`raw_blk_stream.h:80-81`); data written before bitmap (`append_blk_stream.cpp:224-245`) |
| Raft entry applyability | log durability ≠ applied | `ReplLogHeader.commit_lsn_at_write` proof stamps; replay proof-queue + unproven-tail drop (`replica_set.cpp:664-683,729-735`) |

**Refined theorem**: crash coverage = (every SB write/link/remove window) ∧ (every level-3 mechanism's
ordering invariant).  The invariants are all of the shape *data lands before the record naming it* /
*release lands before the anchor forgetting it* — so the pressure points are exactly the orderings in §5.

The generic metablk crash points (`crash_before_sb_write` / `crash_before_sb_linked` /
`crash_during_sb_remove`, conditioned on client + block name — `meta/meta_client.cpp:168,186,282`) give
level-2 coverage for ALL six clients with three flips.  Level-0 and level-3 windows need the specific
points/tests in §5.

## 3. Level-0 inventory (device superblocks)

Layout per pdev (`device/hs_super_blk.h:36-45,277-314`): FirstBlock @0 → vdev slot bitmap @4096 →
VDevInfo[1024] → chunk slot bitmap → ChunkInfo[max_chunks] → data area.

| Structure | Writers (trigger → site) | Read/validation |
|---|---|---|
| FirstBlock (magic, checksum, formatting_done, FirstBlockHeader, PDevInfoHeader) | format W1 `physical_dev.cpp:125`; commit_formatting W2 `:141` (RMW, formatting_done=1).  Never rewritten after. | `is_valid()` checks magic+product+formatting_done; **stored checksum never verified** (`hs_super_blk.h:145-149`) |
| PDevInfoHeader | embedded in FirstBlock only; `max_pdev_chunks` persisted as 0 forever (`physical_dev.cpp:87`); `mirror_super_block` always 0 → footer-mirror branch dead (`:231-237`) | geometry source of truth at load (`physical_dev.cpp:216,178`); uuid mismatch throws (`:209-214`) |
| chunk slot bitmap | format `:315`; chunk create `:367`; batch create `:429`; remove `:517`; batch remove `:552`.  deactivate/reactivate do NOT write it | raw read, **no checksum** (`physical_dev.cpp:441-447`) |
| ChunkInfo (512B, crc32) | create `:359`; batch `:419`; remove(free) `:513,:542`; deactivate `:580` / reactivate `:595` (**plain write(), not write_super_block**) | crc32 verified, mismatch THROWS (`physical_dev.cpp:469-475`) |
| vdev slot bitmap | single writer `device_manager.cpp:565-585` mirrored to EVERY pdev; callers: format `:148`, create_vdev `:245` (last step), destroy_vdev `:259` (last step), stale-slot cleanup `:561` | read from pdev[0] only, **no checksum** (`:419-427`) |
| VDevInfo (512B, crc16) | `virtual_dev.cpp:659-675` to the vdev's backing pdevs only; create D1 `:127`; destroy stage-1 D2 `:227` | validity = `is_allocated()` only; **crc16 never verified** (`device_manager.cpp:474,485`) |

Recovery cross-references (the level-0 self-healing): stale slot (bit set, no allocated VDevInfo
anywhere) → `cleanup_stale_slot_vdevs` (`device_manager.cpp:543-563`); dangling chunk (vdev_id not
loaded) → removed (`:523-538`).  `chunk_id_bm` never persisted — rebuilt from ChunkInfos (`:385-389`).

## 4. Level-1/2 inventory (metablk system + six clients)

Level-1 mechanics (`meta/`): super header + 255 slots written as ONE I/O at format
(`meta_blk_manager.cpp:74`); registration = one 512B slot write (`meta_client.cpp:53`); chain is
singly-linked on disk (prev_bid memory-only, `meta_blk.h:108-113`); `write_meta_blk` = payload write →
[link: tail rewrite or client-info head write] (`meta_client.cpp:162-209`); `remove_meta_blk` = relink or
head-advance → free (`:221-288`); meta vdev allocator is NON-persistent — rebuilt from chain-walk
`commit_blk`s each boot (`meta_blk_manager.cpp:35-44,86`), so orphaned meta blocks self-heal by
construction.  `MetaBlkHeader.data_crc` written, **never verified on read** (`meta_blk.cpp:57` vs
`:68-87`).

| Client | Blocks | Write triggers (all pass the generic crash flips) | Boot tee-off |
|---|---|---|---|
| `CPSuperBlock` | 1 (`CPManagerSuperBlock{m_last_flushed_cp}`) | first boot `cp_mgr.cpp:80`; every CP completion `:311-312` (after ALL consumers, rank-ordered) | `cur_cp_id = last+1` (`:98`) — the global gate for every CP-flushed class |
| `BlobDevManager` | per-chunk bitmap blocks `<dev>_<type>_<sid>_<cid>_<blksz>`; AppendByte SBs `<dev>_appendbyte_sb_<sid>` | bitmap: CP flush per dirtied chunk (`append_blk_stream.cpp:244`, `raw_blk_stream.cpp:269`); AB SB: create/flush/truncate/chunk-change (`append_byte_stream.cpp:66,383,212,431-440`) | pure metablk scan, dead-vdev/chunk blocks dropped; bitmap payload fed to `load_blk_allocator` (`blob_dev_mgr.cpp:154-241`) |
| `cow_btree_mgr` | 1 per btree (`COWBtreeSuperBlock` + user_sb tail) | create `cow_btree.cpp:82`; full-map CP flush only `:733-734` (incr flush writes NO metablk — root rides journal); destroy `:199` | ordinal reservation + stream-id fan-out (`cow_btree_mgr.cpp:32-53`, `cow_btree.cpp:109-131`); user_sb tail persisted ONLY by the create write |
| `LogStoreManager` | `LogStore_<sid>` (LogStoreSb: head/checkpt/rollbacks); `logstore_vdev_logstream_sb_0` (AppendByteStreamSb) | store SB: create/truncate/rollback/CP checkpt advance (`log_store.cpp:67,229-231,285,246-256`); stream SB: create + chunk-change + truncate + seed bump (`log_stream.cpp:81,247-271`) — per-flush persist suppressed | sid parse + `next_store_id_` seed from SBs AND stream records (`log_store_mgr.cpp:95-158,255-256`) |
| `ReplicaSet` | 1 per group `rs_<b64(gid)>` (`ReplicaSetSuperBlk`; watermark deliberately NOT here) | all via `write_sb()` (`replica_set.h:615-619`): create, log-store-ids, destroy_pending, snapshot lsn, replace-member start/complete/divergence | `destroy_pending` re-stage (`replica_set.cpp:604-608`); fresh-vs-reload via `raft_log_store_id==UINT32_MAX`; watermark seeded from LogStore SB (`:709`) |
| `ReplicaRaftConfig` | 1 per group `cfg_<b64(gid)>` (msgpack json: group_id, config, state) | nuraft `save_config` (`replica_set.cpp:923-927`) and `save_state` (`:929-937`) | FIRST recovery pass; SB-without-config ⇒ SB destroyed; config-without-SB ⇒ config destroyed (`repl_manager.cpp:147-169,499-546`) — the SB↔config pairing is the two-block atomicity resolution |

Destroy ordering (crash-resumable teardown): raft cfg → log stores → RS SB **last**, leaving a
discoverable destroy_pending SB (`replica_set.cpp:888-897`); AppendByte destroy nulls its own sb handle
before chunk removal to break the write-after-remove re-append cycle (`append_byte_stream.cpp:126-127`,
`persist_stream_sb:399`).

## 5. Pressure-point matrix

Legend: ✅ covered (existing flip/test) · 🎯 expressible with an EXISTING generic flip + condition,
test needed · ⛔ needs a new crash point · ◇ benign by design, worth one verifying test · ‼ defect (§6).

### Level 0 — device sequences (no crash points exist at this level today)

| Window | Torn state | Recovery mechanism | Status |
|---|---|---|---|
| Format A1-A3 → commit_formatting A4 | formatting_done=0, managers formatted | next boot re-formats from scratch — pre-commit crash loses nothing by contract | ◇ needs one test (⛔ point `crash_before_commit_formatting`) |
| Chunk add: ChunkInfo B1 → bitmap B2 | info written, bit unset → slot invisible | slot treated free, overwritten later; upper-layer data into the vanished chunk is discarded by cp_id gates | ◇ (⛔ point between B1-B2 to prove the cp_id gate holds) |
| Chunk remove: info-freed D1 → bitmap D2 | bit still set, info says free | recovery loads it as a POOLED chunk with the old vdev_id retained | ‼ pooled-resurrection edge + hits §6.1 bug; needs point + test |
| VDev create C1-C2 → VDevInfo C3 | chunks durable, no VDevInfo | dangling-chunk cleanup (`device_manager.cpp:523-538`) | 🎯-adjacent: ⛔ point, test proves F8 fires |
| VDev create C3 → slot bit C4 | VDevInfo allocated, bit unset | vdev never loaded → chunks dangling → F8 | ◇ same test family |
| VDev destroy E1 → E2/E3 → E4 | info freed first / bit freed last | F8 (mid) and F7 stale-slot (late) cleanups | ◇ — F7/F8 currently have ZERO tests |
| any two steps, all sequences | — | **no fsync barrier exists anywhere in level-0 writes** | ‼ §6.9 — ordering rests on write completion order alone |

### Level 1/2 — metablk windows

| Window | Status |
|---|---|
| SB write never lands (any client/block) | ✅ `crash_before_sb_write` (client, block) — C4-class; tests per client 🎯 |
| fresh block durable, unlinked | ✅ `crash_before_sb_linked` — orphan self-heals via non-persistent meta allocator; test 🎯 |
| unlinked/head-advanced, storage unfreed | ✅ `crash_during_sb_remove`; test 🎯 |
| overflow rewrite: new durable, old_ovf unfreed (`meta_blk.cpp:60-65`) | ◇ self-heals (non-persistent allocator) — cover inside the sb_write test |
| registration slot write (`meta_client.cpp:53`) | 🎯 via `crash_before_sb_write`? NO — client-info writes bypass write_meta_blk; low value (re-registration is idempotent at next boot) ◇ |

### Cross-subsystem (CP rank order + boot order)

| Window | Torn state | Status |
|---|---|---|
| between CP consumers of different ranks (cow flushed, LogStore checkpt not) | checkpt stale → wider replay | 🎯 arm `crash_before_sb_write` cond=(LogStoreManager, LogStore_<sid>) during a CP |
| all consumers flushed → CP SB not advanced | whole CP "never happened" | ✅ `crash_during_cp_flush` (cp_mgr.cpp:303); test pending 🎯 |
| cow full-map flushed → cow SB write → passive/incr truncate | stale passive map + already-covered incr records | gates at `cow_btree.cpp:811,873-877`; 🎯 via `crash_before_sb_write` cond=(cow_btree_mgr) + `crash_after_stream_chunk_release` |
| RS SB written → raft cfg not (and reverse), group create | half-created group | ✅ design: pairing destroys the widow (`repl_manager.cpp:509-521,161-167`); test 🎯 |
| destroy: cfg destroyed → log stores → RS SB | resumable teardown | ✅ B6 test + order comment; crash-mid-teardown variant 🎯 via `crash_during_sb_remove` cond=(ReplicaSet) |

### Level 3 — data-vs-anchor orderings

| Window | Mechanism on trial | Status |
|---|---|---|
| log appended+acked, nothing applied | unproven-tail drop / re-commit | ✅ point `crash_after_log_append`; test pending 🎯 |
| applied, no CP | proof-gated replay | ✅✅ C2 test green (`CrashAfterCommitFollower`) |
| data/config commit + rollback pairs | listener-side effects vs durable log | ✅ points (`crash_after_data_commit/rollback`, `crash_after_config_commit/rollback`); tests 🎯 |
| chunks released, stream SB stale | head-into-freed-chunks tolerance | ✅ point `crash_after_stream_chunk_release`; test 🎯 |
| empty-stream reset durable, old chain seed | anti-resurrection prev_crc check | ✅ point `crash_before_logstream_seed_refresh`; test 🎯 |
| indirect value blks written → journal record → commit promotion | uncommitted blkids revert via bitmap | ⛔ untested AND currently unreachable (blob_opt=off everywhere — TEST_MATRIX A5) |
| btree nodes written → incr journal record | cp_id discard of the naming record | 🎯 rides `crash_during_cp_flush` + cow-side validation |

## 6. Defects and gaps found by this audit (pre-existing, unfixed)

1. **`VirtualDev::on_chunks_added` early-`return` on a pooled chunk** (`virtual_dev.cpp:498-506`) —
   aborts registration of the REMAINDER of the chunk batch at recovery.  Real bug whenever a pooled
   chunk precedes live chunks in load order.
2. FirstBlock checksum computed, never verified (`hs_super_blk.h:145-149`).
3. VDevInfo crc16 computed, never verified (`device_manager.cpp:474,485`).
4. MetaBlkHeader.data_crc computed, never verified on read (`meta_blk.cpp:68-87`).
5. Cow incr-journal footer checksum computed (`cow_btree.cpp:651-672`), recovery checks magic only
   (`:919`); torn tail record HS_REL_ASSERTs (`:862`) → crash-loop instead of clean replay stop.
6. No node-level checksum/nodeid cross-check in `read_node` (`cow_btree.cpp:234-278`).
7. Chunk-remove window D1→D2 resurrects a freed chunk as pooled with stale vdev_id (§5, feeds bug 1).
8. Settings (`HS_SETTINGS_FACTORY`) persist to a host-filesystem JSON with no anchor, checksum, or CP
   participation — raft params, reaper intervals etc. sit outside the durability model entirely.
9. No fsync/barrier between ANY two device-superblock writes; all §5 orderings rest on completion order.
10. Dead/stale artifacts: `DeviceManager::read_vdev_info` has no callers (`device_manager.cpp:588-598`);
    `meta/README.md` documents a removed API; `to_remove/…/meta_service.hpp` is dead.
11. `PDevInfoHeader.max_pdev_chunks` persisted as 0 and never populated (`physical_dev.cpp:87`).

## 7. Existing crash-point inventory (product code, all `SISL_FLIP_ENABLED`-only)

`crash_after_log_append` (home_raft_log_store.cpp) · `crash_after_data_commit`,
`crash_after_data_rollback`, `crash_after_config_commit` (leader-arg), `crash_after_config_rollback`
(replica_set.cpp) · `crash_during_cp_flush` (cp_mgr.cpp) · `crash_before_sb_write`,
`crash_before_sb_linked`, `crash_during_sb_remove` (client+block args, meta_client.cpp) ·
`crash_after_stream_chunk_release` (append_byte_stream.cpp) · `crash_before_logstream_seed_refresh` (log_stream.cpp).
