# sift_table: a columnar table access method on the sift_col engine

`CREATE TABLE orders (...) USING sift_table` keeps the rows of the table in compressed column groups (the engine of docs/COL.md) with
**native MVCC**: no heap copy, no heap visibility map, no heap checks. The table is a normal PostgreSQL table: `INSERT`, `COPY`, `UPDATE`,
`DELETE`, `SELECT ... FOR UPDATE`, `TRUNCATE`, `VACUUM [FULL]`, `ANALYZE`, `CLUSTER`, `ALTER TABLE` rewrites, btree and BM25 (`sift`)
indexes, unique constraints, foreign keys, triggers, `pg_dump`, `pg_basebackup`, `pg_upgrade`, physical replication and (with one
requirement, section 5) logical replication.

Names. Table access method `sift_table` (`pg_am.amname` is unique across index and table AMs: the BM25 index AM is `sift`, the column
index AM of the column track is `sift_col`). Handler `sift.table_handler`, SQL in `pg_sift--3.0.sql` and `pg_sift--1.2--3.0.sql`. C identifiers `sift_tbl_*`, files
`src/table/sift_table*.c/h` (`sift_table_*` is taken by sift_compat.h macros). Test functions `sift._test_tbl_*`. This document is the contract of
the table AM track; where it and the code disagree, fix one in the same commit.

Running example (used in tests too): `orders(id bigint, store_id int, placed_at timestamptz, status text, total numeric(12,2), items jsonb,
metrics jsonb)` where `items` is an array of `{sku, qty, price}` objects and `metrics` an object of floats, and `stores(id int, name text)`.

Status legend in the tables below: **done** (in the branch and tested), **todo** (designed, planned), **stub** (the glue exists, the engine
piece it calls is a stub, section 12), **no** (unsupported, with the error given).

---

## 1. Decisions in one page

| topic | decision |
|---|---|
| Storage | the page store of `sift_store.h` (metapage, directory, free list, extents, generic WAL, P1-P6 crash protocol), reused unchanged except one generalization (`sift_store_removable` on a table). |
| TIDs | **synthetic and never reused**: a TID is `(vblock, offset)` with `offset` in 1..`MaxHeapTuplesPerPage` (291) so every index AM, TID bitmaps and GIN accept it. `vblock` comes from a counter in the metapage and never decreases; it is *not* a physical block number. A superblock (SB, 256 vblocks, 74,496 TIDs) is the engine's unit, so the engine's row directory returns the table's own TIDs (docs/COL.md section 12). |
| Row versions | **write buffer + groups.** New rows go to a row-format *write buffer* (write buffer family: pages of `SIFT_PT_WBUFDATA`, directory kind `SIFT_SEG_WBUF_*`) with `xmin`/`cmin` per row. Promotion (section 7) turns a write buffer whose rows are all final into a **column group** (an engine immutable segment with the same TIDs; rows frozen: no per-row xmin). COPY goes through the write buffer (there is no segment-level xmin); the write buffer is sealed as soon as it fills. A rewrite (VACUUM FULL, CLUSTER) writes frozen column segments directly. |
| Deletes, updates, locks | **tombstones**: one 32-byte entry (`tid`, deleter `xid`, `cid`, kind, successor TID) appended to one per-relation **tombstone segment** (same lane page family). A row version is visible iff its insert is visible and no non-aborted DELETE/UPDATE tombstone of it is visible. UPDATE = new row in the write buffer + UPDATE tombstone carrying the successor TID (this is the update chain `FIND_LAST_VERSION` and EvalPlanQual follow). HOT does not exist (`TU_All`). |
| Why tombstones | the deleter's `xid`/`cid` in an append-only log gives MVCC for every snapshot type with the PostgreSQL transaction machinery (subtransactions, savepoints, own-command visibility, aborts) and never rewrites a column group. |
| Lock modes | all four `FOR ...` modes are supported and behave as one exclusive row lock between different transactions (no MultiXact): `FOR SHARE`/`KEY SHARE` of two transactions on the same row serialize. Foreign keys to a `sift_table` parent therefore serialize concurrent children of the same parent row; correct, not fast. |
| TOAST | none (`relation_needs_toast_table` false). Values are stored inline, external TOAST pointers of the source are expanded at insert. A row larger than a write buffer page continues in overflow pages of its write buffer. In a group a value is a leaf of the engine (compressed with the column, section 8); jsonb keeps its exact original bytes next to the shredded paths of col-json. |
| WAL | **hybrid**: generic WAL for every page change (no `shared_preload_libraries` needed, physical replication, PITR, `pg_basebackup`, `pg_rewind` work), plus one custom-rmgr *logical change record* emitted only when the relation is published and `wal_level = logical` (needs `pg_sift` in `shared_preload_libraries` on the primary and every standby; without it a DML on a published table fails with a clear error, never a silent gap). Section 5. |
| Isolation | READ COMMITTED, REPEATABLE READ, SERIALIZABLE (relation-level SIREAD lock, conflict-in on every write, conflict-out on invisible concurrent versions: conservative, more false positives than heap). |
| Parallel | parallel sequential scan over write buffer pages and (engine) SB morsels, one frozen directory copy shared by all workers. Parallel DML is not offered by PostgreSQL for any AM. |
| Aggregates | the SiftAgg pushdown of col-plan runs over the table through the engine's `SiftColVisibility` hook: `sift_col_open_table` (section 12) opens a `SiftColSource` over the column groups with the table's visibility (a unit of a superblock without tombstones is entirely visible; others are filtered per row); write buffer rows are fed as in-memory batches (section 6). |
| Options | **settings, not reloptions** (PostgreSQL has no extensible table access method options: `CREATE TABLE ... WITH (...)` of an unknown option is an error): `sift.table_write_buffer_limit` (kB), `sift.table_inline_promote`, `sift.table_compression`, `sift.table_dead_rewrite_pct` (README, Configuration); the heap reloptions PostgreSQL parses for every table (`autovacuum_*`, `vacuum_truncate`) apply. Per-column compression is the `column=policy` form of `sift.table_compression`. `sift_group_rows` and `sift_paths` do not exist: jsonb is stored as exact bytes (section 8). Kernels follow `sift.simd`. |
| Maintenance | VACUUM does everything (tombstone sweep, dead-row liveness, index cleanup, promotion, merge, stats); autovacuum triggers through the `pgstat` counters the AM maintains (inserts alone trigger it: `autovacuum_vacuum_insert_threshold`). Inline promotion when a write buffer fills (`sift.table_inline_promote`). There is no background worker for tables. |

---

## 2. Layout

### 2.1 Relation, directory, metapage

Main fork only (no FSM, no VM: `pg_visibility` and index-only scans see a table without a VM, which is correct: they take the AM
path). Blocks 0 (meta) and 1 (directory) as in `sift_store.h`; everything else is allocated from the free list / high-water mark by
`sift_alloc_*`. The metapage carries `flags & SIFT_TBL_META_FLAG` (0x00020000) and reuses fields that a table has no use for:

| field | in a table |
|---|---|
| `sum_dl` | **next vblock** (the TID block counter). Reserving a range is part of the directory change that creates the segment, so a crash never hands out a range twice. |
| `ndocs` | rows of the immutable segments (statistics) |
| `k1`, `b`, `analyzer_hash` | 0 |

(Request to the store owners, section 12: name the field. Until then `sift_table.h` has the one accessor.)

Directory entries (`SiftSegEntry`):

| kind | entry | meaning |
|---|---|---|
| `SIFT_SEG_IMMUTABLE` | `reserved` = xmin (0 = frozen), `sb_lo..sb_hi` = SBs with rows, `ndocs` = rows | an engine column segment (docs/COL.md 2.1), schema = table columns + shredded leaves |
| `SIFT_SEG_WBUF_OPEN` / `_SEALED` | `flags & SIFT_TBL_SEG_WBUF`, `sb_lo..sb_hi` = SBs of its vblock range | a write buffer; at most one active, sealed ones wait for maintenance |
| `SIFT_SEG_WBUF_OPEN` | `flags & SIFT_TBL_SEG_TOMB` | the one tombstone segment (swapped for a rewritten one by VACUUM) |

At most `SIFT_MAX_SEGMENTS` (128) entries: the policy keeps 1 open write buffer, 1 tombstone segment, a few sealed write buffers and the tiered
immutable segments; an insert that cannot open a new write buffer because 112 entries are in use runs maintenance and, if a long
transaction holds the horizon so nothing can be promoted, fails with `ERRCODE_PROGRAM_LIMIT_EXCEEDED` (detail "Too many write buffers waiting for promotion: the rows of running transactions cannot be promoted"). With the default 4 MB write buffer limit that is more than 400 MB of rows waiting for promotion.

Because the store walks write buffer lane chains to mark referenced blocks, write buffers and tombstone segments use the lane header
(`SiftWbufHeaderData`, lane 0 only) and the chain `next` pointer at the start of every data page; the orphan sweep, `sift_store_verify`
and the free-list protocol serve a table unchanged.

### 2.2 TIDs

`TID = (vblock, off)`, `off` in 1..291; `key = vblock << 16 | off` is the engine's `SiftKey`; SB = `vblock >> 8`. A write buffer owns
the vblock range `[vbase, vbase + vcap)` (one vblock per page of the segment, in the order the pages were added). A bulk segment is
numbered densely (vblock `base + i / 291`, offset `i % 291 + 1`), which is the engine's dense-page case. A vblock is never reused, so an
index entry never has to be removed before a TID is recycled: a dangling index entry resolves to "no such row" (the fetch returns
false), and VACUUM removes index entries only to keep indexes small, not for correctness. TIDs beyond `RelationGetNumberOfBlocks` are
normal (PostgreSQL asks the AM; `PrefetchBuffer` past EOF is harmless); `pg_relation_size` is the physical size.

`TID -> storage`: directory (binary search by SB range, then the unit) for groups; for a write buffer the header page holds
`page[i]` (physical block of vblock `vbase + i`).

### 2.3 Write buffer (row format, the write buffer)

Header page (`SIFT_PT_WBUFHEADER`): `SiftWbufHeaderData` (lane 0 = the chain) then `SiftTblWbufHdr { magic, flags, vbase, vcap, npages,
nrows, page[vcap] }` (`vcap` <= 1024, set by `sift.table_write_buffer_limit`, default 4 MB = 512). Data page (`SIFT_PT_WBUFDATA`): `SiftTblPageHdr {
next, nrows, flags (SEALED, OVERFLOW), vblock, ovf_next, slot[291] u16 }` then records forward; a record is `{u32 len; xmin; cmin;
u16 flags; u16 natts-hint}` plus a MinimalTuple (the row as `heap_form_minimal_tuple` makes it, so `ALTER TABLE ADD COLUMN` with a
default is handled by `heap_deform_tuple`'s missing-attribute rule). TID offset = slot number + 1; slots never move.

Appending: a backend keeps a *target page* (a vblock it owns for appends) in `rd_amcache`, locks it exclusively, appends and logs the
page delta (generic WAL), at most one WAL record per call, or per page for `multi_insert`. When the page is full it claims the next page
under the header page lock (header, then chain tail: never the reverse), checking `SEALED`. Rows in different pages interleave only by
vblock, so concurrent inserters never share a page. Overflow records (a row over ~7.5 kB) take consecutive pages of the segment marked
`OVERFLOW`, linked by `ovf_next`; the TID's slot holds the head and the total length.

### 2.4 Tombstone segment

Header page as above (`SiftTblTombHdr { magic, nentries, tail }`), chain of data pages of `SiftTblTomb[]` (32 bytes: `u64 key`, `u32 xid`,
`u32 cid`, `u64 succ`, `u16 kind` (DELETE, UPDATE, LOCK), `u16 flags`, `u32 pad`). Entries are written once and never edited; whether an
entry counts is decided by the status of its `xid` at read time. A backend caches the entries as a sorted array and extends the cache
incrementally from `nentries` (append-only), so a lookup is a binary search; with no entry at all (`nentries = 0`, the case of an
insert-only table) a row check costs one comparison. VACUUM writes a new tombstone segment without the entries that no longer matter
(section 7) and switches it in with `sift_dir_update` (the old pages are released with a `safexid`, so readers of the old copy keep
valid pages: P4).

Row-level mutual exclusion for DELETE/UPDATE/LOCK of one TID is a heavyweight `LockTuple(rel, tid, ExclusiveLock)` held while the
caller checks the cache against the log and appends: the same protocol as `heap_delete`, so deadlock detection and `XactLockTableWait`
work as for any heap table.

---

## 3. MVCC

A row version has (insert state, delete state):

- **insert state**: write buffer row: `(xmin, cmin)` of the record, plus the `DEAD` record flag (aborted speculative insertion, or removed by
  VACUUM); group row: the segment's `xmin` (0: frozen, always inserted).
- **delete state**: the non-LOCK tombstone entries for its TID, in the order of the log; a row can have several (aborted ones, then a
  committed or in-progress one). LOCK entries never delete.

Visibility per snapshot type (`sift_tbl_visible`, one function for every caller: scan, fetch, bitmap, tuple_satisfies_snapshot,
index_delete_tuples, analyze, index build, SiftAgg's hook):

| snapshot | row is visible when |
|---|---|
| MVCC | inserted: `xmin` is the current transaction with `cmin < curcid`, or not in the snapshot and committed (`XidInMVCCSnapshot`, `TransactionIdDidCommit`; a hint on the record flags it so a committed record costs no clog lookup) **and** no tombstone is *deleted-visible*: same rule on the tombstone's `xid`/`cid` (`cid < curcid` for the current transaction). |
| Self | as MVCC with every command of the current transaction counted as done |
| Any | always (aborted rows included, as `SnapshotAny` on a heap; deleted rows included) |
| Dirty | inserter in progress (not me): visible, `snapshot->xmin = xmin`, and for a speculative record `snapshot->speculativeToken`; aborted inserter: no. A tombstone of an in-progress transaction (not me): row visible, `snapshot->xmax = xid`; of me or committed: row deleted. The btree uniqueness check, `INSERT ... ON CONFLICT` and the foreign key checks rely on exactly this. |
| NonVacuumable | not deleted, or the deleter is not yet removable (`GlobalVisTestIsRemovableXid`); used by the planner's `get_actual_variable_range` |
| historic MVCC, toast | not used on this AM |

**Updates and chains.** `tuple_update`: lock the TID (`LockTuple`), sync the cache, evaluate the tombstones of the TID: none live
and the row visible to `snapshot`: insert the new version (write buffer, `xmin = me`), append `UPDATE{succ = new}`; a live tombstone of a transaction in
progress: `TM_BeingModified` (`wait = false`) or release the tuple lock, `XactLockTableWait`, retry; committed UPDATE: `TM_Updated` with
`tmfd->ctid = succ`, `xmax`, `cmax`; committed DELETE: `TM_Deleted`; by me with `cmax >= cid`: `TM_SelfModified`; REPEATABLE READ and above
see the same results and the executor raises the serialization failure, as for heap. `tuple_lock` with `TUPLE_LOCK_FLAG_FIND_LAST_VERSION`
follows `succ` while the entry is a committed UPDATE, fetching the latest version into the slot (so READ COMMITTED's EvalPlanQual, the
logical-replication apply worker and `ON CONFLICT DO UPDATE` work). `ctid` and `tableoid` work (`slot->tts_tid`); `xmin`, `xmax`, `cmin`,
`cmax` read zero (the transaction ids of a row are not kept as a heap keeps them; the logical-replication apply worker reads `xmin` for its conflict report).

**Speculative insertion** (`INSERT ... ON CONFLICT`): the record carries the token; `tuple_complete_speculative(succeeded = false)` sets
its `DEAD` flag, so a killed insertion is never visible to anyone and other inserters waiting on the token proceed.

**SERIALIZABLE.** Scans and fetches take `PredicateLockRelation`; every write calls `CheckForSerializableConflictIn(rel, NULL,
InvalidBlockNumber)`; an invisible row or tombstone of a concurrent transaction calls `CheckForSerializableConflictOut`. Relation granularity is the
price of not having heap pages; it is correct and has more false positives than heap.

**Freezing and wraparound.** Group rows have no xid. Write buffer rows older than the horizon are promoted into groups (frozen); tombstones
and bulk-segment `xmin`s are removed or reset to 0 by VACUUM. `relation_vacuum` computes the oldest xid still stored (write buffer rows,
tombstones, segment xmins, the `xmin`s of in-flight work) and passes it to `vac_update_relstats` as `relfrozenxid`; MultiXacts are
not used, `relminmxid` follows the cutoff. So anti-wraparound autovacuum works and ends.

**Rows and the snapshot of the scan.** A scan registers its snapshot (`MyProc->xmin`, so P4 protects the extents), copies the
directory once and reads write buffer pages as it goes: a row appended after the scan began has a later `xmin` (or a later `cmin` in this
transaction) and is invisible, a promotion that replaces a write buffer with a group publishes the same TIDs and rows: the scan keeps its
old copy, whose pages are not reused until the scan's snapshot is gone.

---

## 4. TableAmRoutine: every callback

PostgreSQL 17 and 18; the differences are marked. "Engine" = the stubbed piece of section 12.

| callback | decision | status |
|---|---|---|
| `slot_callbacks` | `TTSOpsHeapTuple`: every row is built as a HeapTuple (`t_self` = its TID) and handed to the slot with `sift_tbl_store`, which moves it into the slot's own memory context (a scan runs in a per-tuple context that is reset under it; the heap's buffer slots never allocate there). `ctid`/`tableoid` work, other system columns error. | done |
| `scan_begin` / `scan_end` / `scan_rescan` | walks, in order: write buffers (page by page, rows in slot order), groups (engine units, in SB order). Snapshot registered, directory copied once. `SO_TYPE_SEQSCAN`, `SO_TYPE_ANALYZE`, `SO_TYPE_BITMAPSCAN`, `SO_TYPE_TIDRANGESCAN` honored; scan keys unsupported (the planner never passes them for a non-heap AM). | done |
| `scan_getnextslot` | both directions, as a heap: the cursor is on the row last returned, so a backward step gives the one before it, an end resets the position (a forward step then starts at the first row, a backward one at the last). Backward goes over the column groups from their last unit, then the write buffer pages from the last. The units of the range are listed on the first backward step and found again by superblock and place in it; no unit is decoded twice per step. | done |
| `scan_set_tidrange` / `scan_getnextslot_tidrange` | by SB/vblock range on the directory, then the filter `mintid <= tid <= maxtid`. | done |
| `parallelscan_estimate` / `_initialize` / `_reinitialize` | `ParallelBlockTableScanDescData`-like header + a frozen copy of the directory + an atomic unit counter (a write buffer page, then one superblock of the column groups per unit). | done (`t/008_tbl_promote.pl`) |
| `index_fetch_begin/reset/end` | per-fetch state caches the directory copy, the open segment readers, the last decoded unit and the tombstone cache; no buffer is held between calls. | done |
| `index_fetch_tuple` | TID -> storage (2.2), visibility (3), materialize into a virtual slot. `call_again` always false (no HOT chains); `all_dead` true when the row is dead for every snapshot (no such row, final tombstone, aborted insert) so btree can kill the index entry. Honors Dirty, Self, Any, NonVacuumable. | done |
| `tuple_fetch_row_version` | same as above with `slot->tts_tid`; used by triggers, RETURNING of old rows, MERGE, `WHERE CURRENT OF`. | done |
| `tuple_tid_valid` | `off` in 1..291 and `vblock < next_vblock`. | done |
| `tuple_get_latest_tid` | follows `succ` of committed UPDATE tombstones visible to the snapshot. | done |
| `tuple_satisfies_snapshot` | `sift_tbl_visible` on the slot's TID. | done |
| `index_delete_tuples` | for each TID: no such row, or a deleter removable now: `knowndeletable`; conflict horizon = the largest deleter xid among them (else `InvalidTransactionId`, nothing deletable). | done |
| `tuple_insert` | write buffer append (2.3), `slot->tts_tid` set, `xmin = GetCurrentTransactionId()`, `cmin = cid`; `pgstat_count_heap_insert`; conflict-in; logical record (5). `options` `TABLE_INSERT_SKIP_FSM`/`FROZEN`/`NO_LOGICAL` accepted (FROZEN is only valid in a rewrite and writes a frozen row). | done |
| `tuple_insert_speculative` / `tuple_complete_speculative` | record with `SPEC` flag and token; completion kills or confirms (3). | done |
| `multi_insert` | one write buffer page fill per WAL record; above `sift_group_rows` rows (or a COPY of that size) writes a column segment directly with segment-level xmin (bulk path). | done (row at a time through the write buffer; no bulk column path) |
| `tuple_delete` | tombstone (3); `wait`, `crosscheck`, `changingPart` honored (a partition move records `tmfd->ctid` = the "moved partition" TID like heap: `ItemPointerSetMovedPartitions`). | done |
| `tuple_update` | new version + UPDATE tombstone (3); `*update_indexes = TU_All`, `*lockmode = LockTupleExclusive`. | done |
| `tuple_lock` | LOCK tombstone unless the transaction already holds one or deleted the row; `LockWaitPolicy` block/skip/error; `FIND_LAST_VERSION`; `TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS`; fills the slot with the locked version. | done |
| `finish_bulk_insert` | seals the write buffer if the bulk filled it and nothing else is pending; flushes a partial bulk segment; nothing to `fsync` (the store handles `wal_level = minimal`, as for heap through `smgrDoPendingSyncs`). | done |
| `relation_set_new_filelocator` | creates the fork, writes meta + directory + the empty tombstone segment (init fork for UNLOGGED, as `ambuildempty`), returns `*freezeXid = RecentXmin`, `*minmulti = GetOldestMultiXactId()`. | done |
| `relation_nontransactional_truncate` | rewrites meta/directory empty (new relfilenode is created by TRUNCATE itself in the same transaction; this callback is the `TRUNCATE` of a relation created in the transaction and of temp tables). | done |
| `relation_copy_data` | `ALTER TABLE SET TABLESPACE`, `SET LOGGED/UNLOGGED`: block-for-block copy of the fork (`RelationCopyStorage` through smgr, WAL-logged per page); the store is position independent, so the copy is exact. | done |
| `relation_copy_for_cluster` | `CLUSTER`, `VACUUM FULL`: reads the old table's stored versions (live rows are kept; recently dead rows are counted in `tups_recently_dead` and dropped, as are the rows the rewriting transaction deleted), writes frozen column segments directly (section 7), sorted by the btree `OldIndex` for CLUSTER, new dense TIDs; reports `num_tuples`, `tups_vacuumed`, `*xid_cutoff = OldestXmin`. | done |
| `relation_vacuum` | section 7. | done |
| `scan_analyze_next_block` / `_next_tuple` | PG 17+: the `ReadStream` is only a budget (its buffers are released unread); the AM walks sampling units (write buffer pages, engine units chosen at random up to `300 * max statistics target` rows) and reports `liverows` so that `totalrows` equals the exact live count. | done |
| `index_build_range_scan` | `CREATE INDEX [CONCURRENTLY]`, `REINDEX`: iterates the SB range `[start_blockno, start_blockno + numblocks)` in vblock space (the parallel btree build splits it), `SnapshotAny` + own visibility for the concurrent build (the same rules as `heapam_index_build_range_scan`: in-progress inserters and deleters are waited for with `XactLockTableWait` when the build is not concurrent, indexed anyway when concurrent), partial index predicates, expression indexes, `anyvisible`, progress reporting. | done |
| `index_validate_scan` | the second pass of `CREATE INDEX CONCURRENTLY`: merge-join of the sorted index TIDs (`tuplesort`, `ValidateIndexState`) with a scan of the table at the validation snapshot, inserting the missing visible rows. | done |
| `relation_size` | physical size of the main fork (`RelationGetNumberOfBlocks * BLCKSZ`). | done |
| `relation_needs_toast_table` / `relation_toast_am` / `relation_fetch_toast_slice` | false / unused / unused (no TOAST table is ever created, so these are never called). | done |
| `relation_estimate_size` | pages = the larger of the physical blocks and twice the pages the rows take in a heap (a scan builds every row: 1.7 times the instructions of a heap scan, whatever the size of the compressed file), tuples = group rows - tombstoned + write buffer rows, from the meta counters (not `reltuples`, which ANALYZE of a sampled table only estimates), `allvisfrac` = 0 (no VM). Widths as the heap rule. | done |
| `scan_bitmap_next_block` (17) / `scan_bitmap_next_tuple` (17, 18) | the TID bitmap is walked here (18: iterator inside the scan): for a page `(vblock, offsets...)` rows are fetched in TID order, exact pages recheck-free, lossy pages test every row of the vblock; `exact_pages`/`lossy_pages` counted for EXPLAIN. Serves btree, BM25 `sift`, GIN, BRIN indexes unchanged. | done |
| `scan_sample_next_block` / `_next_tuple` | `TABLESAMPLE SYSTEM` picks vblocks, `BERNOULLI` walks them; the visible rows of a vblock are read at once and the method's offsets pick among them. | done |

Planner-side: no custom path is needed for a plain scan (the AM's costs through `relation_estimate_size` and the scan cost model
are enough once the page estimate stands for the work of building the rows, not for the compressed size, which a VACUUM that gives the free space back makes a tenth of it). SiftAgg
and SiftScan paths (col-plan) are offered by their own hooks and take the table through the engine source of section 6.

---

## 5. WAL, replication and what works where (the decisive part)

**PostgreSQL facts this rests on** (PostgreSQL 17 and 18 sources: `access/xlog_internal.h`, `replication/logical/decode.c`,
docs "Generic WAL Records" and "Custom WAL Resource Managers"):

1. Generic WAL records (`RM_GENERIC_ID`) are replayed by every server (the rmgr is built in) and are *not* decoded: logical decoding of
   an `RM_GENERIC_ID` record queues nothing. Rows written only through generic WAL are invisible to logical replication.
2. A custom resource manager (`RegisterCustomRmgr`, id 128..255 reserved for development or registered on the PostgreSQL wiki) must be
   registered from `_PG_init` during `shared_preload_libraries` processing, on the primary **and on every server that replays its WAL**
   (a standby without it stops recovery with "resource manager not registered"). `RmgrData.rm_decode` (present in 17 and 18)
   is called by `LogicalDecodingProcessRecord` for the rmgr's records and may call `ReorderBufferQueueChange` with
   `REORDER_BUFFER_CHANGE_INSERT/UPDATE/DELETE`, a `RelFileLocator` and a heap-format tuple: the built-in `pgoutput` then publishes it like
   a heap change (it maps the locator to the relation, reads the tuple with `heap_deform_tuple`, applies the publication's row filter
   and column list).
3. `pgoutput` takes UPDATE/DELETE keys from the old-tuple image; the replica identity is checked by the executor (`CheckCmdReplicaIdentity`)
   for every AM, so a published `sift_table` table needs one for UPDATE/DELETE exactly like a heap table.

**Options compared.**

| | generic WAL only | custom rmgr for everything | **hybrid (chosen)** |
|---|---|---|---|
| `shared_preload_libraries` | no | **yes, always, everywhere** | only to publish the table |
| physical standby, PITR, `pg_basebackup`, `pg_rewind` | yes | yes (all nodes preload) | yes (all nodes preload only if the logical record is ever emitted) |
| logical decoding / publications | **no, silently nothing** | yes | yes |
| `wal_consistency_checking` | masks as for sift today | needs `rm_mask` | as generic |
| WAL volume | page write buffers (generic computes a diff against a copy: about 2 pages copied per write) | logical records, smallest | generic + one logical record per row change when published |
| code | small | redo for every page operation: large | small + decode + record writer |
| silent data loss risk | **yes** (a publication is accepted and delivers no rows) | none | none: the AM refuses DML on a published table when the logical record cannot be written |

**Decision: hybrid.**

- Every page change is a generic WAL record (the existing store protocol P1-P6, so crash recovery needs nothing new).
- When `RelationBuildPublicationDesc` says the relation publishes an action and `XLogLogicalInfoActive()`, the DML also writes **one custom-rmgr
  record per change** (`RM_SIFT_TBL`, a *development* rmgr id until one is registered): `{op, relfilelocator, tuple image (heap tuple format,
  every TOAST value expanded), old key image for UPDATE/DELETE}`. `rm_redo` is a no-op (the page changes are in the generic records, so
  a standby needs the rmgr only to *skip* the record), `rm_decode` turns it into a reorder-buffer change, subtransaction and
  transaction ordering come from the record's xid. Old key image: the replica identity columns (`RelationGetIndexAttrBitmap`), all
  columns for `REPLICA IDENTITY FULL`; building it costs fetching the old row of a DELETE/UPDATE only when published.
- If the relation is published and the rmgr is not registered (`pg_sift` not preloaded) the DML raises `ERRCODE_FEATURE_NOT_SUPPORTED`:
  "sift_table tables can only be published when pg_sift is in shared_preload_libraries". The check is cached per relation (the
  publication descriptor is in the relcache and invalidated by publication DDL).
- `TRUNCATE` is decoded by the core (`XLOG_HEAP_TRUNCATE` is written by `ExecuteTruncateGuts` for any AM).
- Initial table synchronization (`COPY` of the table) uses the AM's scan; the subscriber applies with `table_tuple_insert/update/delete/lock`
  of its own `sift_table` or heap table.

**What works where.**

| | status | notes |
|---|---|---|
| crash recovery | works | generic WAL replays pages; a half-written segment is an orphan swept by VACUUM, never referenced (P1, P3); the directory switch is atomic (P2). TAP test: `kill -9` during insert, promotion, vacuum, bulk load, then `sift.tbl_verify` and a result comparison with a heap table. |
| physical standby (hot standby reads) | works | the same snapshot rules (`XidInMVCCSnapshot` with the standby's snapshot), `sift_store_check_page` turns a page recycled under a standby query into a serialization failure to retry (P5; `hot_standby_feedback` avoids it). No recovery conflicts are raised by generic WAL. |
| `pg_basebackup`, PITR, `pg_rewind` | works | block-level WAL, no extra requirement unless the logical record is on (then every node preloads). |
| `pg_upgrade` | works | files copied; BLCKSZ 8192; the format major/minor in the metapage is checked on open (a newer minor asks for `VACUUM FULL`). |
| `pg_dump` / restore | works | `COPY` of the live rows through the scan, `CREATE TABLE ... USING sift_table`, indexes and constraints recreated; `ALTER TABLE ... SET (...)` options dumped as reloptions. `pg_dump --no-table-access-method` restores into heap. |
| logical replication, publisher | works with the hybrid record | requires preload; UPDATE/DELETE need a replica identity; partitioned tables and `publish_via_partition_root` as for heap. |
| logical replication, subscriber | works | the apply worker uses the AM callbacks. |
| `wal_level = minimal` | works | new relfilenodes skip WAL and are synced at commit by the core (`RelationNeedsWAL`). |
| UNLOGGED, TEMP | works | the init fork holds the empty layout; temp tables use local buffers. |
| logical decoding of the table by a *plugin* (test_decoding, wal2json) | works with the record, same preload requirement | a plugin sees ordinary changes. |

**Unsupported, with the error**: logical decoding of an unpreloaded published table (`0A000`, above); `TABLESAMPLE` (`0A000`); `BLCKSZ` other than 8192 (`ERRCODE_FEATURE_NOT_SUPPORTED` at table creation).

**Blocker check (honest).** No decisive blocker for logical replication: it works through the hybrid record, with the stated cost
(preload everywhere the logical record is replayed, double WAL for published tables). The cheapest way out for a deployment that
cannot preload is to publish a *heap* table (or a view over it) that is kept in sync by `INSERT INTO ... SELECT` or a trigger, or to
replicate the heap staging table of an ingest pipeline; the AM refuses to publish silently-incomplete data.

---

## 6. Reading: scans, indexes, the engine, SiftAgg

- **Sequential/bitmap/TID/index scans** are the AM callbacks above. A scan owns a `SiftTblScan` (directory copy, write buffer page cursor,
  group reader, tombstone cache). The slot of a `sift_table` (`sift_table_slot.c`, a virtual slot) is given a row of a column group
  as a reference only: the group reader, the unit it was in (a token that changes with every unit) and the position of the row in the
  unit's vectors. `slot_getsomeattrs` reaches the AM with the number of leading columns the query reads, and only those leaf columns
  are decoded, one vector per column and unit on the first row that needs it (`fill_attr`); `count(*)` decodes nothing. The decoded
  by-reference values belong to the slot (allocated in its memory context, freed when it is cleared, no context of its own); a
  slot whose unit is no longer the reader's current one (another row was fetched meanwhile, a cursor moved on) finds its row again
  by TID through the table's fetch scan. Rows of the row-format segments are copied into the slot as heap tuples. TID fetches
  (index scans, bitmap, UPDATE) keep the last decoded unit of a backend-wide fetch scan (valid while the directory and the
  backend's xmin stay; its pages are unpinned after every fill), and a caller that needs the version of a row, not its values (row
  locks, uniqueness checks, `index_delete_tuples`), asks for it without decoding (`sift_tbl_fetch_lazy`: the live-row list of the
  unit answers). The system columns `xmin`, `xmax`, `cmin` and `cmax` read zero.
- **BM25 `sift` and btree on a `sift_table`**: `ambuild` runs `table_index_build_scan` (callback above); `aminsert` receives the
  synthetic TID from `tuple_insert`; scans return TIDs and the bitmap/index scan nodes call `table_index_fetch_tuple` /
  `scan_bitmap_next_*`. The sift custom scans and `count(*)` fast paths ask `sift_vis_*` (src/core/sift_visibility.c) which today reads the heap's
  visibility map and calls `heap_hot_search_buffer`; for a `sift_table` the table AM provides `sift_tbl_filter_page` (visibility of the
  candidate offsets of one vblock) and `sift_tbl_nblocks` (the vblock count), and `sift_vis_begin` branches on the table AM. This is a
  small patch to a file this track does not own, kept in its own commit (section 12).
- **SiftAgg over the table** (`src/table/sift_table_src.c`, the table provider of the seam `src/agg/sift_colsrc.h`, docs/COL-PLAN.md): the leaves are the
  columns of the table, in the order of its column layout. The groups are read by the engine, one `sift_col_open_table` source per column layout
  the segments were written for (the leaves of an older layout are matched by attribute number; a column the segment lacks reads as the
  catalog's default for rows that lack it, null or the constant of `ADD COLUMN .. DEFAULT`), with the table's visibility as the engine's
  `SiftColVisibility` hook: `sb_mask` is all-ones for a superblock with no tombstone and no write buffer row, else zero, and `filter_page` is
  `sift_tbl_filter_page` (insert state, tombstones). The rows of the write buffers are encoded as one in-memory batch per superblock
  (`sift_tbl_wbuf_batch`, the promotion's row conversion, fed with `sift_col_add_batches` when a morsel begins) and read like a stored unit. The units of
  the sources are merged in ascending superblock. Aggregates therefore see exactly the rows a sequential scan sees
  (`tbl_agg` and the TAP test 008 compare the two plans in every state: write buffer, promoted, tombstones, own and other transactions' changes,
  rolled back, added columns). `numeric(p<=18, s>=0)` leaves are the scaled integers of the engine (`SiftSrcLeaf.scale`, `sift_col_numeric_key`), `char(n)` is a dictionary leaf of the padded
  values; leaves the engine stores as exact images (`SIFT_COLS_VAR`: other types, wider numerics) are marked lossy: no query uses them. A column group keeps an odd NaN float apart as an exception: a query
  that reads such a column of a unit holding one raises 0A000 with the hint to set `sift.enable_agg` off (the same as the index). A string of any length and
  a float `-0` are stored exactly in their leaf (docs/COL.md 2.9).

---

## 7. Maintenance

**Promotion** (`sift_tbl_promote`, `src/table/sift_table_seg.c`): a write buffer is sealed, closed to appends (page `SEALED` flags under page locks, directory
kind `SEALED`; `create_wbuf` does it when the segment fills, VACUUM closes the active one once it has 8 pages), and becomes a column
segment when every row is *final* (`row_fate`): `xmin` committed and older than the horizon (kept, frozen), or flagged `DEAD` by VACUUM
(dropped); a row whose insert is aborted or not yet old enough keeps the segment as it is (retried by the next promotion). A row with a
tombstone is promoted as a normal row (its tombstone applies by TID wherever the row lives). The segment is written with the engine writer
(`sift_colw_*`, one unit per superblock of the vblock range, TIDs unchanged), and `sift_dir_update` swaps it for the write buffer entry; the write buffer
pages are released with a `safexid` (reusable once no snapshot can read them: the second VACUUM, or the next allocation, recycles them;
the new table of a rewrite is private, so its extents are reusable at once). The inline promotion takes the maintenance lock only if it is free.

*Layout of a group* (`SiftTblLayout`): one leaf per live column in column order (typed: integers and `numeric(p<=18, s>=0)` as INT with the
scale, `float4/8` as F4/F8, text kinds as STR; any other type, including `jsonb`, `uuid`, arrays and `bytea`, as VAR holding the exact datum
image) plus one `exceptions` VAR leaf. A value a typed leaf cannot hold exactly (numeric NaN/Infinity, a float non-canonical NaN)
is stored as NULL with the overflow flag in its leaf and its image in `exceptions`, so every value round-trips
exactly. A segment carries its own schema; after `ADD/DROP COLUMN` it is read by attribute number (missing: the default), segments of
another layout are read side by side and are not merged with the current ones. At most 127 columns (`CREATE TABLE` fails otherwise).

**Merge** (`sift_tbl_merge`, VACUUM: up to 16 steps): the tiered policy of `sift_merge_pick` over the segments of the current layout (fan-in 8)
plus "mostly dead" (`sift_tbl_mostly_dead`: a segment more than `sift.table_dead_rewrite_pct` percent dead, default 50 (a `sift_col` index has 10: a unit with dead rows is scanned instead of answered from its pre-aggregates, and a table keeps its segments until half is dead),
is rewritten alone; 100 never); dead rows dropped by the engine's `sift_col_merge_segments`, which sizes its output runs to its
input so that free space left by earlier merges is reused instead of extending the file. A segment of an older layout is not rewritten.

**VACUUM (`relation_vacuum`)**, one pass, restartable:

1. Classify tombstone entries against `OldestXmin`: aborted -> drop; committed and removable -> its TID is **dead**; others kept.
2. Dead TIDs of write buffer rows get the `DEAD` record flag; dead TIDs of group rows clear the engine's liveness bit
   (`sift_cseg_bulkdelete` with a "TID in dead set" callback, WAL logged). Aborted write buffer inserts and the rows of aborted bulk segments are dead too.
3. `index_bulk_delete` on every index with the dead set (rounds bounded by `maintenance_work_mem`), then `index_vacuum_cleanup`. TIDs are
   never reused, so an interrupted VACUUM cannot expose a recycled TID; only index bloat stays until the next one.
4. Close the open write buffer (8 pages or more), promote, merge (the mostly dead segments included).
5. Rewrite the tombstone segment without dropped entries and without LOCK entries of finished transactions; swap it in (skipped when
   no entry would go).
6. Space: `sift_recycle` clears the free-list records whose releasing transaction no snapshot can see any more, the orphan sweep
   releases what a crash left, then the reusable free run at the end of the file is cut (`sift_store_truncate`, `sift_store.h` P7) as for
   a heap. Before the cut, the segment (column segment, tombstones, or the write buffer, which is closed and sealed) whose blocks end last in the file is
   written again into the reusable free runs below it while they hold it (`compact_tail`, at most 16 steps; only with the cut enabled, and a column segment
   of an older layout stays): a load seals its column segment above the write buffers it replaces, and without this the file stays the size of the load
   (1M orders: 163 MB to 14 MB; the dead rows of a rewritten segment go with it). The cut itself is done when it is 1000 blocks or a 16th of the file, `vacuum_truncate` is on (the heap reloption, or `VACUUM (TRUNCATE)`), and
   `AccessExclusiveLock` on the table is free right now (`ConditionalLockRelation`, released as soon as the file is cut). Under that lock no
   backend holds a scan, a directory copy or a target page; the store sends a relcache invalidation of the table before the lock goes, which
   marks every backend's `SiftTblRel` invalid (its target page, write buffer page tables and fetch scan are rebuilt on next use), and a standby
   replays the lock first, cancelling the queries that hold the table there. A lazy VACUUM is in no snapshot or horizon, so its own releases
   carry the next xid (`xid_safexid`) and are reused once another transaction has ended; under the lock every record is reusable
   (`sift_store_horizon`), so the cut takes the free run at the end whatever released it.
   Live segments in the middle of the file stop the cut, as live rows do for a heap; their space is reused by later writes.
7. `vac_update_relstats` (relpages, reltuples = live rows, `relfrozenxid`), `pgstat_report_vacuum` (live rows; dead rows = deleted by a
   committed transaction but not removable yet), counted as a heap does: an in-progress insertion is not live yet, an in-progress deletion
   still is; group rows from the liveness bits less the recently deleted ones. `VACUUM VERBOSE` reports the rows removed, the rows that
   remain and the dead rows not yet removable, and the truncation. The counters of insert/update/delete that the AM reports keep
   autovacuum's thresholds meaningful (`pgstat_count_heap_insert/update/delete`).

`VACUUM FULL` and `CLUSTER` are `relation_copy_for_cluster`: the kept versions are written straight into frozen column segments of the
new table with dense TIDs (`sift_tbl_bulk_*`: no row-format pages, no promotion). VACUUM FULL re-keys the rows of the old units as they decode
(no tuple is formed) when every column segment has the current layout, then adds the write buffer rows; otherwise (after `ALTER TABLE`) it reads
the scan's tuples. CLUSTER sorts the scan's tuples by a btree index whatever the planner's `use_sort` says (a TID fetch decodes the unit of
its superblock, so an index scan in random order would decode a unit per row) and fetches by index scan only for an index that cannot
order a sort. A version deleted or updated by the rewriting transaction itself is dropped (the rewrite commits or aborts with it).
`VACUUM (INDEX_CLEANUP off)` skips step 3 (indexes keep dangling entries). `sift.table_space(tbl)` shows the file, the high-water mark
and the free list.

**ANALYZE** (section 4): statistics columns as PostgreSQL computes them from the sample; jsonb shredded paths are not statistics targets.

---

## 8. Compression, options, defaults

- Compression is on by default (col-compress: algorithm per leaf, `auto` picks per chunk from FOR/PACK/dictionary and zstd/lz4 when the
  library is present); SIMD kernels are chosen at runtime from `sift.simd` (`SET sift.simd = 'scalar'` for the specification).
- Settings (GUCs; a table access method cannot have reloptions): `sift.table_write_buffer_limit` (kB, 2048-8192, default 4096: size of a write
  segment, whole superblocks of 2 MB, so 2 MB is the effective minimum; a row has to fit one segment), `sift.table_inline_promote` (default on),
  `sift.table_compression` (the `compression` grammar of `sift_col`: `auto`, `none`, `lz4[:N]`, `zstd[:N]`, encodings; `'auto; col=zstd:9'` per column;
  taken from the session that promotes, autovacuum uses the server setting), `sift.table_dead_rewrite_pct` (1-100, default 50: section 7,
  merge). Segments are self-describing, so changing a setting affects only
  the segments written afterwards. `sift_group_rows` (rows per unit: one superblock) and `sift_paths` (jsonb shredding) are not provided.
- `ALTER TABLE ... SET ACCESS METHOD sift_table` rewrites the table through `relation_copy_for_cluster`-style bulk writes.
- Types: every type, exactly (section 7, layout of a group): typed leaves for what the engine stores natively, opaque `SIFT_COLS_VAR` leaves
  for the rest, an `exceptions` leaf for the values a typed leaf would change.

---

## 9. Limits

BLCKSZ 8192; PostgreSQL 17, 18 (19 experimental); at most 127 columns (`CREATE TABLE` fails past it, error code `54000`; `ALTER TABLE ADD COLUMN` past it succeeds and the next `INSERT` or promotion fails with the same error until a column is dropped); 128 directory entries; rows up to 1 GB (varlena limit); vblocks
2^32 (about 1.2 x 10^12 rows ever written, counting dead ones: a table that exhausts them is rewritten by `VACUUM FULL`). Unsupported:
`xmin/xmax/cmin/cmax` (they read zero), multixact-based shared row locks (they degrade to exclusive), HOT, speculative
insertion through foreign tables (n/a), rewrite of a table with RLS-bypassing rewrites (n/a).

## 10. Tests

All in the branch, run by `scripts/ci.sh` like the BM25 tests:

- Regression `test/sql/tbl_basic` (DDL, DML, rollback, savepoints, row locks, btree, unique and `ON CONFLICT`, VACUUM/ANALYZE, wide rows,
  TRUNCATE and its rollback, CTAS, partitions, temp tables, `ALTER ... SET ACCESS METHOD` both ways, `ADD/DROP COLUMN`), `tbl_bm25`
  (BM25 `sift` index on a `sift_table`: agreement with the sequential scan through DML, rollback, VACUUM, REINDEX [CONCURRENTLY], CIC) and the
  **differential suite** `tbl_diff` (the same statements on a heap table and on a sift_table, EXCEPT ALL both ways and index scans, after bulk
  load, updates of key and non-key columns, deletes, wide rows, NULLs, savepoint loops, rollbacks, UPDATE FROM, DELETE USING, data-modifying CTE,
  MERGE, INSERT SELECT, aggregates, windows, FOR UPDATE, VACUUM [FULL], REINDEX, CLUSTER, rewrite to heap and back, TRUNCATE).
- Regression `tbl_promote` (a heap twin through bulk load, VACUUM promotion, point/index/bitmap reads, updates and deletes of promoted rows, aborts, dead-row
  clearing, merges, special values (`-0`, NaN, long text, huge numerics), VACUUM FULL, CLUSTER, ADD/DROP COLUMN, a type change, TRUNCATE),
  `tbl_space` (released blocks are reused: an emptied and refilled table keeps its size; the free end of the file is cut one VACUUM after
  it is freed, not with `vacuum_truncate` off),
  `tbl_dead` (live and dead row counts, the rewrite of mostly dead segments under `sift.table_dead_rewrite_pct`),
  `tbl_rewrite` (CLUSTER by a btree sort and a GiST index scan, CLUSTER after deletions of its own transaction, VACUUM FULL over two
  column layouts), `tbl_sample` (TABLESAMPLE over write buffer rows and groups).
- Isolation `test/isolation/specs/tbl_concurrency.spec`: update/update, update/delete, delete/update at READ COMMITTED (the waiter
  re-evaluates), key update, REPEATABLE READ first-updater-wins, SERIALIZABLE write skew, FOR UPDATE / SHARE / NOWAIT / SKIP LOCKED, uncommitted
  rows, unique wait on the inserter (commit and abort), `ON CONFLICT` against an in-progress insert.
- TAP `test/t/006_tbl.pl`: crash recovery (committed, aborted and in-flight work, checkpoint in the middle, BM25 index, VACUUM FULL), hot standby
  (every write kind, stable repeatable-read snapshot under primary VACUUM, promotion), `wal_level = minimal` (both `wal_skip_threshold`
  settings), `pg_dump` / `pg_restore` round trip in plain and custom format. `test/t/007_tbl_logical.pl`: unpublished table without preload, 0A000
  with the hint for a published table without preload, replication of insert / update / delete / TRUNCATE / subtransactions / `ON CONFLICT` to a
  heap and to a sift_table subscriber with preload, and a physical standby of the publisher. `test/t/008_tbl_promote.pl`: autovacuum promotes an insert-only
  table and reclaims updates and deletes, parallel workers over write buffer pages and column groups, a repeatable-read reader across a promotion, a standby of a promoted table.
  Everything runs on PostgreSQL 17 and 18.
- `SANITIZE=1` (ASan + UBSan) runs the same jobs through `scripts/ci.sh`. BM25 A/B: `bench/ab-query.sh` before and after the branch on a heap
  table (the BM25 paths changed only by `sift_vis_nblocks` and the non-heap branch of the page filter).

## 11. Open decisions and risks

- Tombstone cache memory is O(live tombstones) per backend; the upgrade path is a per-SB tombstone index (pointer pages) so only the SBs a
  query touches are loaded. Mass deletes should be VACUUMed (autovacuum does) or avoided (TRUNCATE, partition drop).
- Shared row locks degrade to exclusive; implementing MultiXact would need a lock-owner set per row.
- Relation-level SSI locks; a per-SB granularity is possible once the engine reports the SBs a scan touched.
- The logical record doubles WAL for published tables; a custom rmgr for all page changes would remove the double write but demands
  preload everywhere.

## 12. Interfaces to the engine and the other tracks: stubs and requests

The table AM is written against `src/col/sift_col.h`, `sift_col_enc.h`, `sift_col_seg.h` of the column track and has no second implementation of
anything the engine does: group segments are written by `sift_colw_*` (and `sift_col_merge_segments`), read by `sift_col_open_table` /
`sift_col_begin/next/read`, cleared by `sift_cseg_bulkdelete`, with the table's visibility as the engine's `SiftColVisibility` hook.

| piece | status |
|---|---|
| segment writer `sift_colw_*`, `sift_col_merge_segments` | used by promotion and merge |
| `sift_col_open_table(rel, schema, dir, vis, heap_sbs, mcxt)`: a source over the groups of a table | used by scans, fetches, ANALYZE |
| `SIFT_COLS_VAR` (exact datum images, engine), `exceptions` leaf (table layout) | used |
| liveness `sift_cseg_bulkdelete` with a TID callback | VACUUM step 2 |
| `sift_colw_set_tag` (segment xmin of a bulk write) | not used: the direct column writes of a rewrite are frozen |
| `sift_col_add_batches` (write buffer rows as in-memory units), the `SiftSrcProvider` seam of `sift_colsrc.h` | used by the table provider (`sift_table_src.c`) |

**Files outside this track changed in this branch, each in its own small commit** (owner in brackets):

| file | change |
|---|---|
| `src/core/sift_store.c` [store; also edited by the m2-truncate branch] | `sift_store_removable` takes a table relation (horizon of the table itself) |
| `src/core/sift_visibility.c/.h` [BM25 core] | `sift_vis_nblocks` (virtual blocks for a sift_table), page filter delegated to `sift_tbl_filter_page` for a sift_table |
| `src/bm25/sift_build.c`, `src/bm25/sift_cscan.c` [BM25 core] | the `sift` index accepts a sift_table; block counts through `sift_vis_nblocks` |
| `src/core/sift_handler.c` [BM25 core] | `_PG_init` calls `sift_tbl_init` (registers the change-record resource manager when preloaded) |
| `scripts/isolation.sh` [test tooling] | `# requires: tam` (the spec is skipped when the installed pg_sift has no `sift_table` access method) |

**Requests (for the owners, as notes; nothing here edits their files):**

- col-core: (1) a storage class `SIFT_COLS_VAR` (opaque value bytes with exact round trip, no 255-byte limit, dictionary or plain, compressible)
  and the table-schema -> leaf mapping for every type: a table cannot refuse `numeric`, `uuid`, `jsonb`, long text. (2) `SiftSegEntry.reserved`
  documented as the segment xmin of a table AM (the engine must copy it on rewrite and never merge segments with different non-zero xmin).
  (3) an API to add in-memory batches (`SiftColBatch`) as extra units of a `SiftColSource` (the write buffer rows). (4) a named metapage field for
  the next vblock (table AM) instead of reusing `sum_dl`. (5) the maintenance worker entry point generalized over index and table.
- store owners: `sift_store_removable` must accept a table relation (`index->rd_index->indrelid` is NULL for a table): a one-line change made
  in this branch in its own commit; and the generic WAL record count limits for the tombstone append (header + tail page).
- sift_visibility.c: the non-heap branch of section 6 (own commit).
- col-compress: the per-column option interface (`compression=`); col-json: `paths` grammar for the table-level option.
