# sift_table: a columnar table access method on the sift_col engine

`CREATE TABLE orders (...) USING sift_table` keeps the rows of the table in compressed column groups with
**native MVCC**: no heap copy, no heap visibility map, no heap checks. The table is a normal PostgreSQL table: `INSERT`, `COPY`, `UPDATE`,
`DELETE`, `SELECT ... FOR UPDATE`, `TRUNCATE`, `VACUUM [FULL]`, `ANALYZE`, `CLUSTER`, `ALTER TABLE` rewrites, btree and BM25 (`sift`)
indexes, unique constraints, foreign keys, triggers, `pg_dump`, `pg_basebackup`, `pg_upgrade`, physical replication and (with one
requirement, section 5) logical replication.

Names. Table access method `sift_table` (`pg_am.amname` is unique across index and table AMs: the BM25 index AM is `sift`, the column
index AM is `sift_col`). Handler `sift.table_handler`, SQL in `sql/pg_sift--3.0.sql` and `sql/pg_sift--1.2--3.0.sql`. C identifiers `sift_tbl_*`, files
`src/table/*.c/h` (`sift_table_*` is taken by core/compat.h macros). Test functions `sift._test_tbl_*`. This document is the contract of
the table AM; where it and the code disagree, fix one in the same commit.

Running example (used in tests too): `orders(id bigint, tenant_id int, placed_at timestamptz, status text, total numeric(12,2), items jsonb,
metrics jsonb)` where `items` is an array of `{sku, qty, price}` objects and `metrics` an object of floats, and `stores(id int, name text)`.

---

## 1. Decisions in one page

| area | decision |
|---|---|
| Storage | the page store of `core/store.h` (metapage, directory, free list, extents, generic WAL, P1-P6 crash protocol), reused unchanged except one generalization (`sift_store_removable` on a table). |
| TIDs | **synthetic and never reused**: a TID is `(vblock, offset)` with `offset` in 1..`MaxHeapTuplesPerPage` (291) so every index AM, TID bitmaps and GIN accept it. `vblock` comes from a counter in the metapage and never decreases; it is *not* a physical block number. A superblock (SB, 256 vblocks, 74,496 TIDs) is the engine's unit, so the engine's row directory returns the table's own TIDs. |
| Row versions | **write buffer + groups.** New rows go to a row-format *write buffer* (`SIFT_PT_WBUFDATA` pages, directory kind `SIFT_SEG_WBUF_*`) with `xmin`/`cmin` per row. Promotion (section 7) turns a write buffer whose rows are all final into a frozen **column group** (an engine immutable segment, same TIDs) and, under directory pressure, any full write buffer into an **unfrozen group** (per-row `xmin`/`cmin` in version vectors, format 4; VACUUM freezes it when its rows are final). COPY goes through the write buffer. VACUUM FULL and CLUSTER write frozen column segments directly. |
| Deletes, updates, locks | **tombstones**: one 32-byte entry (`tid`, deleter `xid`, `cid`, kind, successor TID) appended to one per-relation **tombstone segment**. A row version is visible iff its insert is visible and no non-aborted DELETE/UPDATE tombstone of it is visible. UPDATE = new row in the write buffer + UPDATE tombstone carrying the successor TID (the update chain `FIND_LAST_VERSION` and EvalPlanQual follow). HOT does not exist (`TU_All`). |
| Why tombstones | the deleter's `xid`/`cid` in an append-only log gives MVCC for every snapshot type with the PostgreSQL transaction machinery (subtransactions, savepoints, own-command visibility, aborts) and never rewrites a column group. |
| Lock modes | all four `FOR ...` modes are supported and behave as one exclusive row lock between different transactions (no MultiXact): `FOR SHARE`/`KEY SHARE` of two transactions on the same row serialize. Foreign keys to a `sift_table` parent therefore serialize concurrent children of the same parent row; correct, not fast. |
| TOAST | none (`relation_needs_toast_table` false): values are stored inline, external TOAST pointers are expanded at insert. A row larger than a write buffer page continues in overflow pages; one too large for the buffer's page table (`sift.table_wide_rows`, directory format 11) continues in tail pages (`SIFT_TBL_PAGE_OVFDATA \ | SIFT_TBL_PAGE_TAIL`) that are in the page chain only, take no vblock, and are read through the record's `aux` and `ovf_next` links. In a group a value is an engine leaf (compressed with the column, section 8); jsonb keeps its exact bytes next to the shredded paths of col-json. |
| WAL | **hybrid** (section 5): generic WAL for every page change (no preload needed; physical replication, PITR, `pg_basebackup`, `pg_rewind` work), plus one custom-rmgr *logical change record* only when the relation is published and `wal_level = logical` (needs `pg_sift` in `shared_preload_libraries` on the primary and every standby; without it DML on a published table fails with a clear error, never a silent gap). |
| Isolation | READ COMMITTED, REPEATABLE READ, SERIALIZABLE (relation-level SIREAD lock, conflict-in on every write, conflict-out on invisible concurrent versions: conservative, more false positives than heap). |
| Parallel | parallel sequential scan over write buffer pages and (engine) SB morsels, one frozen directory copy shared by all workers. Parallel DML is not offered by PostgreSQL for any AM. |
| Aggregates | the SiftAgg pushdown of col-plan runs over the table through the engine's `SiftColVisibility` hook (`sift_col_open_table`, section 12); write buffer rows are fed as in-memory batches (section 6). |
| Options | **settings, not reloptions** (PostgreSQL has no extensible table AM options: an unknown option in `CREATE TABLE ... WITH (...)` is an error): `sift.table_write_buffer_limit` (kB), `sift.table_inline_promote`, `sift.table_compression`, `sift.table_dead_rewrite_pct` (section 8; docs/USAGE.md, Maintenance and operations); heap reloptions PostgreSQL parses for every table (`autovacuum_*`, `vacuum_truncate`) apply. Per-column compression is the `column=policy` form of `sift.table_compression`. `sift_group_rows` and `sift_paths` do not exist: jsonb is stored as exact bytes (section 8). Kernels follow `sift.simd`. |
| Maintenance | VACUUM does everything (tombstone sweep, dead-row liveness, index cleanup, promotion, merge, stats); autovacuum triggers through the AM's `pgstat` counters (inserts alone trigger it: `autovacuum_vacuum_insert_threshold`); inline promotion when a write buffer fills (`sift.table_inline_promote`); without VACUUM a worker does the space work that needs no index (`sift.table_maintain`, section 7, `sift.table_auto_defrag`). |

## 2. Layout

### 2.1 Relation, directory, metapage

Main fork only (no FSM, no VM: `pg_visibility` and index-only scans see a table without a VM, which is correct: they take the AM path). Blocks 0 (meta) and 1 (directory) as in `core/store.h`; everything else comes from the free list / high-water mark via `sift_alloc_*`. The metapage carries `flags & SIFT_TBL_META_FLAG` (0x00020000) and reuses fields a table does not need:

| field | in a table |
|---|---|
| `sum_dl` | **next vblock** (the TID block counter). Reserving a range is part of the directory change that creates the segment, so a crash never hands out a range twice. |
| `ndocs` | rows of the immutable segments (statistics) |
| `k1`, `b`, `analyzer_hash` | 0 |

`table/table.h` has the one accessor (`SIFT_TBL_NEXT_VBLOCK`).

Directory entries (`SiftSegEntry`):

| kind | entry | meaning |
|---|---|---|
| `SIFT_SEG_IMMUTABLE` | `flags & SIFT_SEG_VERS` when some of its rows are not final (an **unfrozen group**), then `reserved` = the smallest xid among them (a group of 3.0.1 that is not frozen has `reserved` = its writer's xid, format 3); `sb_lo..sb_hi` = SBs with rows, `ndocs` = rows | an engine column segment, schema = table columns + shredded leaves; an unfrozen group keeps the `xmin` and `cmin` of its rows in the version section of the segment (section 7); the entry's `sum_dl` is not touched: the store totals it into the metapage, where a table keeps the next vblock |
| `SIFT_SEG_WBUF_OPEN` / `_SEALED` | `flags & SIFT_TBL_SEG_WBUF`, `sb_lo..sb_hi` = SBs of its vblock range | a write buffer; at most one active, sealed ones wait for maintenance |
| `SIFT_SEG_WBUF_OPEN` | `flags & SIFT_TBL_SEG_TOMB` | the one tombstone segment (swapped for a rewritten one by VACUUM) |

At most `SIFT_MAX_SEGMENTS` (128) entries: the policy keeps 1 open write buffer, 1 tombstone segment, a few sealed write buffers and the tiered immutable segments. An insert that cannot open a write buffer because 112 entries are in use runs maintenance and, if the write buffers cannot be promoted, fails with `ERRCODE_PROGRAM_LIMIT_EXCEEDED` (detail "Too many write buffers waiting for promotion"). Once the directory is half full every write buffer becomes an unfrozen group (section 7), so a load of any size (one command or single-row INSERTs, one writer or several, with savepoints) does not reach the limit; a transaction whose write buffers hold a deleted row VACUUM has not seen, or a speculative insertion in progress, does. The limit is the testing setting `sift.table_max_segments` (default and maximum 128), which the regression tests lower.

Write buffers and tombstone segments use the lane header (`SiftWbufHeaderData`, lane 0 only) and the chain `next` pointer at the start of every data page, because the store walks write buffer lane chains to mark referenced blocks; the orphan sweep, `sift_store_verify` and the free-list protocol serve a table unchanged.

### 2.2 TIDs

`TID = (vblock, off)`, `off` in 1..291; `key = vblock << 16 | off` is the engine's `SiftKey`; SB = `vblock >> 8`. A write buffer owns the vblock range `[vbase, vbase + vcap)` (one vblock per page of the segment, in the order the pages were added). A bulk segment is numbered densely (vblock `base + i / 291`, offset `i % 291 + 1`), the engine's dense-page case. A vblock is never reused, so an index entry need not be removed before a TID is recycled: a dangling entry resolves to "no such row" (the fetch returns false), and VACUUM removes index entries only to keep indexes small. TIDs beyond `RelationGetNumberOfBlocks` are normal (`PrefetchBuffer` past EOF is harmless); `pg_relation_size` is the physical size.

`TID -> storage`: directory (binary search by SB range, then the unit) for groups; for a write buffer the header page holds `page[i]` (physical block of vblock `vbase + i`).

### 2.3 Write buffer (row format, the write buffer)

Header page (`SIFT_PT_WBUFHEADER`): `SiftWbufHeaderData` (lane 0 = the chain) then `SiftTblWbufHdr { magic, flags, vbase, vcap, npages, nrows, page[vcap] }` (`vcap` <= 1024, set by `sift.table_write_buffer_limit`, default 4 MB = 512). Data page (`SIFT_PT_WBUFDATA`): `SiftTblPageHdr { next, nrows, flags (SEALED, OVERFLOW), vblock, ovf_next, slot[291] u16 }` then records forward; a record is `{u32 len; xmin; cmin; u16 flags; u16 natts-hint}` plus a MinimalTuple (as `heap_form_minimal_tuple` makes it, so `ALTER TABLE ADD COLUMN` with a default is handled by `heap_deform_tuple`'s missing-attribute rule). TID offset = slot number + 1; slots never move.

Appending: a backend keeps a *target page* (a vblock it owns for appends) in `rd_amcache`, locks it exclusively, appends and logs the page delta (generic WAL), at most one WAL record per call, or per page for `multi_insert`. When the page is full it claims the next page under the header page lock (header, then chain tail: never the reverse), checking `SEALED`. Concurrent inserters never share a page. Overflow records (a row over ~7.5 kB) take consecutive pages marked `OVERFLOW`, linked by `ovf_next`; the TID's slot holds the head and the total length.

### 2.4 Tombstone segment

Header page as above (`SiftTblTombHdr { magic, nentries, tail }`), chain of data pages of `SiftTblTomb[]` (32 bytes: `u64 key`, `u32 xid`, `u32 cid`, `u64 succ`, `u16 kind` (DELETE, UPDATE, LOCK), `u16 flags`, `u32 pad`). Entries are written once and never edited; whether one counts is decided by the status of its `xid` at read time. A backend caches the entries as a sorted array and extends the cache incrementally from `nentries` (append-only): a lookup is a binary search, and with no entry (`nentries = 0`, an insert-only table) a row check costs one comparison. VACUUM writes a new tombstone segment without the entries that no longer matter (section 7) and switches it in with `sift_dir_update` (old pages are released with a `safexid`, so readers of the old copy keep valid pages: P4).

DELETE/UPDATE/LOCK of one TID is serialized by a heavyweight `LockTuple(rel, tid, ExclusiveLock)` held while the caller checks the cache against the log and appends, as `heap_delete` does, so deadlock detection and `XactLockTableWait` work as for any heap table.

## 3. MVCC

A row version has (insert state, delete state):

- **insert state**: write buffer row: the record's `(xmin, cmin)`, plus the `DEAD` record flag (aborted speculative insertion, or removed by VACUUM); group row: its `xmin` and `cmin` from the version vectors of an unfrozen group (`sift_tbl_ver_state`, `sift_tbl_vers_rowver`), else always inserted (frozen).
- **delete state**: the non-LOCK tombstone entries for its TID, in log order; a row can have several (aborted ones, then a committed or in-progress one). LOCK entries never delete.

Visibility per snapshot type (`sift_tbl_visible`, one function for every caller: scan, fetch, bitmap, tuple_satisfies_snapshot, index_delete_tuples, analyze, index build, SiftAgg's hook):

| snapshot | row is visible when |
|---|---|
| MVCC | inserted: `xmin` is the current transaction with `cmin < curcid`, or not in the snapshot and committed (`XidInMVCCSnapshot`, `TransactionIdDidCommit`; a hint on the record flags it so a committed record costs no clog lookup) **and** no tombstone is *deleted-visible*: same rule on the tombstone's `xid`/`cid` (`cid < curcid` for the current transaction). |
| Self | as MVCC with every command of the current transaction counted as done |
| Any | always (aborted rows included, as `SnapshotAny` on a heap; deleted rows included) |
| Dirty | inserter in progress (not me): visible, `snapshot->xmin = xmin`, and for a speculative record `snapshot->speculativeToken`; aborted inserter: no. A tombstone of an in-progress transaction (not me): row visible, `snapshot->xmax = xid`; of me or committed: row deleted. The btree uniqueness check, `INSERT ... ON CONFLICT` and the foreign key checks rely on exactly this. |
| NonVacuumable | not deleted, or the deleter is not yet removable (`GlobalVisTestIsRemovableXid`); used by the planner's `get_actual_variable_range` |
| historic MVCC, toast | not used on this AM |

**Updates and chains.** `tuple_update` locks the TID (`LockTuple`), syncs the cache and evaluates the TID's tombstones:

- none live and the row visible to `snapshot`: insert the new version (write buffer, `xmin = me`), append `UPDATE{succ = new}`;
- a live tombstone of a transaction in progress: `TM_BeingModified` (`wait = false`) or release the tuple lock, `XactLockTableWait`, retry;
- committed UPDATE: `TM_Updated` with `tmfd->ctid = succ`, `xmax`, `cmax`; committed DELETE: `TM_Deleted`; by me with `cmax >= cid`: `TM_SelfModified`; REPEATABLE READ and above see the same results and the executor raises the serialization failure, as for heap.

`tuple_lock` with `TUPLE_LOCK_FLAG_FIND_LAST_VERSION` follows `succ` while the entry is a committed UPDATE and fetches the latest version into the slot (so READ COMMITTED's EvalPlanQual, the logical-replication apply worker and `ON CONFLICT DO UPDATE` work). `ctid` and `tableoid` work (`slot->tts_tid`). `xmin`, `xmax`, `cmin`, `cmax` are the row's stored versions, read by TID when asked (`sift_slot_getsysattr`): `xmin`/`cmin` of the write buffer record or of the unit's version section (a frozen row: `FrozenTransactionId`, command 0), `xmax`/`cmax` of the row's last tombstone, delete, update or lock, aborted or not as the heap keeps them (none: 0). A row locked by several transactions shows the last locker, not a MultiXactId. The logical-replication apply worker reads `xmin` for its conflict report.

**Speculative insertion** (`INSERT ... ON CONFLICT`): the record carries the token; `tuple_complete_speculative(succeeded = false)` sets its `DEAD` flag, so a killed insertion is never visible and other inserters waiting on the token proceed; `succeeded = true` clears the speculative flag (a row left speculative would keep its write buffer from being promoted).

**SERIALIZABLE.** Scans and fetches take `PredicateLockRelation`; every write calls `CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber)`; an invisible row or tombstone of a concurrent transaction calls `CheckForSerializableConflictOut`. Relation granularity is the price of having no heap pages: correct, with more false positives than heap.

**Unfrozen groups.** The rows of a group that is not frozen are as invisible as write buffer rows until their writer's snapshot rule says otherwise. The scan, index scan, bitmap scan, `tuple_fetch`, parallel scan, SiftAgg's hook and the analyze and index build paths read each row's `xmin` and `cmin` from the version vectors of its unit (`SiftTblVers`, built per directory snapshot, cached by generation) and apply the write buffer rule (`sift_tbl_visible`); a unit whose summary decides for all its rows is not read row by row (`sift_tbl_ver_state`: all visible, none, or per row), and `CheckForSerializableConflictOut` is called as for an invisible row. A row's unit is found from its superblock: a group's superblocks belong to it alone, because a write buffer is given whole superblocks of vblocks (`create_wbuf`).

- Aborted writers (rollback, savepoint rolled back, crash): VACUUM finds the rows whose xid aborted in the vectors, the index cleanup forgets them and their liveness bits are cleared (the next merge drops the rows; a group with nothing live left is dropped).
- A group stays unfrozen until its live rows are committed and older than the horizon; then VACUUM clears the entry's flag (`sift_tbl_freeze_groups`), the vectors are ignored and the next merge rewrites it frozen.
- Live-row counts are exact for a frozen group and count the committed or own-transaction rows for an unfrozen one (`unfrozen_live_rows`), as `relation_estimate_size` and ANALYZE read them.

**Freezing and wraparound.** Frozen group rows have no xid. Write buffer rows older than the horizon are promoted into frozen groups; the xids of an unfrozen group are frozen with it; tombstones and bulk-segment `xmin`s are removed or reset to 0 by VACUUM. `relation_vacuum` computes the oldest xid still stored (write buffer rows, tombstones, segment xmins, `xmin`s of in-flight work) and passes it to `vac_update_relstats` as `relfrozenxid`; MultiXacts are not used, `relminmxid` follows the cutoff. So anti-wraparound autovacuum works and ends.

**Rows and the snapshot of the scan.** A scan registers its snapshot (`MyProc->xmin`, so P4 protects the extents), copies the directory once and reads write buffer pages as it goes: a row appended after the scan began has a later `xmin` (or a later `cmin` in this transaction) and is invisible; a promotion that replaces a write buffer with a group publishes the same TIDs and rows, and the scan keeps its old copy, whose pages are not reused until the scan's snapshot is gone.

## 4. TableAmRoutine: every callback

PostgreSQL 17, 18 and 19; differences are marked. Every callback below is implemented and tested.

| callback | decision | status |
|---|---|---|
| `slot_callbacks` | `TTSOpsHeapTuple`: every row is built as a HeapTuple (`t_self` = its TID) and handed to the slot with `sift_tbl_store`, which moves it into the slot's own memory context (a scan runs in a per-tuple context that is reset under it). All system columns work (`xmin`, `xmax`, `cmin`, `cmax` are read by TID when asked). | done |
| `scan_begin` / `scan_end` / `scan_rescan` | walks write buffers (page by page, rows in slot order), then groups (engine units, in SB order). Snapshot registered, directory copied once. `SO_TYPE_SEQSCAN`, `SO_TYPE_ANALYZE`, `SO_TYPE_BITMAPSCAN`, `SO_TYPE_TIDRANGESCAN` honored; scan keys unsupported (the planner never passes them for a non-heap AM). | done |
| `scan_getnextslot` | both directions, as a heap: the cursor is on the row last returned, so a backward step gives the one before it, an end resets the position (forward restarts at the first row, backward at the last). Backward goes over the column groups from their last unit, then the write buffer pages from the last; the units of the range are listed on the first backward step and found again by superblock and place in it; no unit is decoded twice per step. | done |
| `scan_set_tidrange` / `scan_getnextslot_tidrange` | by SB/vblock range on the directory, then the filter `mintid <= tid <= maxtid`. | done |
| `parallelscan_estimate` / `_initialize` / `_reinitialize` | `ParallelBlockTableScanDescData`-like header + a frozen copy of the directory + an atomic unit counter (a write buffer page, then one superblock of the column groups per unit). | done (`t/008_tbl_promote.pl`) |
| `index_fetch_begin/reset/end` | per-fetch state caches the directory copy, the open segment readers, the last decoded unit and the tombstone cache; no buffer is held between calls. | done |
| `index_fetch_tuple` | TID -> storage (2.2), visibility (3), materialize into a virtual slot. `call_again` always false (no HOT chains); `all_dead` true when the row is dead for every snapshot (no such row, final tombstone, aborted insert) so btree can kill the index entry. Honors Dirty, Self, Any, NonVacuumable. | done |
| `tuple_fetch_row_version` | as above with `slot->tts_tid`; used by triggers, RETURNING of old rows, MERGE, `WHERE CURRENT OF`. | done |
| `tuple_tid_valid` | `off` in 1..291 and `vblock < next_vblock`. | done |
| `tuple_get_latest_tid` | follows `succ` of committed UPDATE tombstones visible to the snapshot. | done |
| `tuple_satisfies_snapshot` | `sift_tbl_visible` on the slot's TID. | done |
| `index_delete_tuples` | per TID: no such row, or a deleter removable now: `knowndeletable`; conflict horizon = the largest deleter xid among them (else `InvalidTransactionId`, nothing deletable). | done |
| `tuple_insert` | write buffer append (2.3), `slot->tts_tid` set, `xmin = GetCurrentTransactionId()`, `cmin = cid`; `pgstat_count_heap_insert`; conflict-in; logical record (5). `options` `TABLE_INSERT_SKIP_FSM`/`FROZEN`/`NO_LOGICAL` accepted (FROZEN is valid only in a rewrite and writes a frozen row). | done |
| `tuple_insert_speculative` / `tuple_complete_speculative` | record with `SPEC` flag and token; completion kills or confirms (3). | done |
| `multi_insert` | one write buffer page fill per WAL record; above `sift_group_rows` rows (or a COPY of that size) writes a column segment directly with segment-level xmin (bulk path). | done (row at a time through the write buffer; no bulk column path) |
| `tuple_delete` | tombstone (3); `wait`, `crosscheck`, `changingPart` honored (a partition move records `tmfd->ctid` = the "moved partition" TID like heap: `ItemPointerSetMovedPartitions`). | done |
| `tuple_update` | new version + UPDATE tombstone (3); `*update_indexes = TU_All`, `*lockmode = LockTupleExclusive`. | done |
| `tuple_lock` | LOCK tombstone unless the transaction already holds one or deleted the row; `LockWaitPolicy` block/skip/error; `FIND_LAST_VERSION`; `TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS`; fills the slot with the locked version. | done |
| `finish_bulk_insert` | seals the write buffer if the bulk filled it and nothing else is pending; flushes a partial bulk segment; nothing to `fsync` (the store handles `wal_level = minimal`, as heap through `smgrDoPendingSyncs`). | done |
| `relation_set_new_filelocator` | creates the fork, writes meta + directory + the empty tombstone segment (init fork for UNLOGGED, as `ambuildempty`), returns `*freezeXid = RecentXmin`, `*minmulti = GetOldestMultiXactId()`. | done |
| `relation_nontransactional_truncate` | rewrites meta/directory empty (TRUNCATE itself creates the new relfilenode; this callback is the `TRUNCATE` of a relation created in the transaction and of temp tables). | done |
| `relation_copy_data` | `ALTER TABLE SET TABLESPACE`, `SET LOGGED/UNLOGGED`: block-for-block copy of the fork (`RelationCopyStorage` through smgr, WAL-logged per page); the store is position independent, so the copy is exact. | done |
| `relation_copy_for_cluster` | `CLUSTER`, `VACUUM FULL`: reads the old table's stored versions (live rows kept; recently dead rows counted in `tups_recently_dead` and dropped, as are rows the rewriting transaction deleted), writes frozen column segments directly (section 7), sorted by the btree `OldIndex` for CLUSTER, new dense TIDs; reports `num_tuples`, `tups_vacuumed`, `*xid_cutoff = OldestXmin`. | done |
| `relation_vacuum` | section 7. | done |
| `scan_analyze_next_block` / `_next_tuple` | PG 17+: the `ReadStream` is only a budget (its buffers are released unread); the AM walks sampling units (write buffer pages, engine units chosen at random up to `300 * max statistics target` rows) and reports `liverows` so that `totalrows` equals the exact live count. | done |
| `index_build_range_scan` | `CREATE INDEX [CONCURRENTLY]`, `REINDEX`: iterates the SB range `[start_blockno, start_blockno + numblocks)` in vblock space (the parallel btree build splits it), `SnapshotAny` + own visibility for the concurrent build (rules of `heapam_index_build_range_scan`: in-progress inserters and deleters are waited for with `XactLockTableWait` when not concurrent, indexed anyway when concurrent), partial index predicates, expression indexes, `anyvisible`, progress reporting. | done |
| `index_validate_scan` | second pass of `CREATE INDEX CONCURRENTLY`: merge-join of the sorted index TIDs (`tuplesort`, `ValidateIndexState`) with a scan of the table at the validation snapshot, inserting the missing visible rows. | done |
| `relation_size` | physical size of the main fork (`RelationGetNumberOfBlocks * BLCKSZ`). | done |
| `relation_needs_toast_table` / `relation_toast_am` / `relation_fetch_toast_slice` | false / unused / unused (no TOAST table is ever created). | done |
| `relation_estimate_size` | pages = the larger of the physical blocks and twice the pages the rows take in a heap (a scan builds every row: 1.7 times the instructions of a heap scan, whatever the compressed size), tuples = group rows - tombstoned + write buffer rows, from the meta counters (not `reltuples`, which ANALYZE of a sampled table only estimates), `allvisfrac` = 0 (no VM). Widths as the heap rule. | done |
| `scan_bitmap_next_block` (17) / `scan_bitmap_next_tuple` (17, 18) | the TID bitmap is walked here (18: iterator inside the scan): for a page `(vblock, offsets...)` rows are fetched in TID order, exact pages recheck-free, lossy pages test every row of the vblock; `exact_pages`/`lossy_pages` counted for EXPLAIN. Serves btree, BM25 `sift`, GIN, BRIN indexes unchanged. | done |
| `scan_sample_next_block` / `_next_tuple` | `TABLESAMPLE SYSTEM` picks vblocks, `BERNOULLI` walks them; the visible rows of a vblock are read at once and the method's offsets pick among them. | done |

Planner: no custom path is needed for a plain scan (AM costs through `relation_estimate_size` and the scan cost model suffice once the page estimate stands for the work of building rows, not the compressed size, which a VACUUM that gives the free space back makes a tenth of it). SiftAgg and SiftScan paths (col-plan) come from their own hooks and take the table through the engine source of section 6.

## 5. WAL, replication and what works where (the decisive part)

**PostgreSQL facts this rests on** (PostgreSQL 17 and 18 sources: `access/xlog_internal.h`, `replication/logical/decode.c`; docs "Generic WAL Records" and "Custom WAL Resource Managers"):

1. Generic WAL records (`RM_GENERIC_ID`) are replayed by every server but *not* decoded: rows written only through generic WAL are invisible to logical replication.
2. A custom resource manager (`RegisterCustomRmgr`, id 128..255 reserved for development or registered on the PostgreSQL wiki) must be registered from `_PG_init` during `shared_preload_libraries` processing, on the primary **and on every server that replays its WAL** (a standby without it stops recovery with "resource manager not registered"). `RmgrData.rm_decode` (present in 17 and 18) is called by `LogicalDecodingProcessRecord` for the rmgr's records and may call `ReorderBufferQueueChange` with `REORDER_BUFFER_CHANGE_INSERT/UPDATE/DELETE`, a `RelFileLocator` and a heap-format tuple; the built-in `pgoutput` then publishes it like a heap change (maps the locator to the relation, `heap_deform_tuple`, applies the row filter and column list).
3. `pgoutput` takes UPDATE/DELETE keys from the old-tuple image; the executor checks replica identity (`CheckCmdReplicaIdentity`) for every AM, so a published `sift_table` needs one for UPDATE/DELETE like a heap table.

**Options compared.**

|  | generic WAL only | custom rmgr for everything | **hybrid (chosen)** |
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

|  | status | notes |
|---|---|---|
| crash recovery | works | generic WAL replays pages; a half-written segment is an orphan swept by VACUUM, never referenced (P1, P3); the directory switch is atomic (P2). TAP test: `kill -9` during insert, promotion, vacuum, bulk load, then `sift.verify` on the table's BM25 index and a result comparison with a heap table. |
| physical standby (hot standby reads) | works | the same snapshot rules (`XidInMVCCSnapshot` with the standby's snapshot), `sift_store_check_page` turns a page recycled under a standby query into a serialization failure to retry (P5; `hot_standby_feedback` avoids it). No recovery conflicts are raised by generic WAL. |
| `pg_basebackup`, PITR, `pg_rewind` | works | block-level WAL, no extra requirement unless the logical record is on (then every node preloads). |
| `pg_upgrade` | works | files copied; BLCKSZ 8192; the format major/minor in the metapage is checked on open (a newer minor asks for `VACUUM FULL`). |
| `pg_dump` / restore | works | `COPY` of the live rows through the scan, `CREATE TABLE ... USING sift_table`, indexes and constraints recreated; `ALTER TABLE ... SET (...)` options dumped as reloptions. `pg_dump --no-table-access-method` restores into heap. |
| logical replication, publisher | works with the hybrid record | requires preload; UPDATE/DELETE need a replica identity; partitioned tables and `publish_via_partition_root` as for heap. |
| logical replication, subscriber | works | the apply worker uses the AM callbacks. |
| `wal_level = minimal` | works | new relfilenodes skip WAL and are synced at commit by the core (`RelationNeedsWAL`). |
| UNLOGGED, TEMP | works | the init fork holds the empty layout; temp tables use local buffers. |
| logical decoding of the table by a *plugin* (test_decoding, wal2json) | works with the record, same preload requirement | a plugin sees ordinary changes. |

**Unsupported, with the error**: logical decoding of an unpreloaded published table (`0A000`, above); `BLCKSZ` other than 8192 (`ERRCODE_FEATURE_NOT_SUPPORTED` at table creation).

**Blocker check.** No decisive blocker for logical replication: it works through the hybrid record, at the stated cost
(preload everywhere the logical record is replayed, double WAL for published tables). The cheapest way out for a deployment that
cannot preload is to publish a *heap* table (or a view over it) kept in sync by `INSERT INTO ... SELECT` or a trigger, or to
replicate the heap staging table of an ingest pipeline; the AM refuses to publish silently-incomplete data.

---

## 6. Reading: scans, indexes, the engine, SiftAgg

- **Sequential/bitmap/TID/index scans** are the AM callbacks above. A scan owns a `SiftTblScan` (directory copy, write buffer page cursor, group reader, tombstone cache). The slot of a `sift_table` (`table/slot.c`, a virtual slot) holds a group row as a reference only: the group reader, its unit (a token that changes with every unit) and the row's position in the unit's vectors.
  - `slot_getsomeattrs` receives the number of leading columns the query reads; only those leaf columns are decoded, one vector per column and unit on the first row that needs it (`fill_attr`); `count(*)` decodes nothing. Decoded by-reference values belong to the slot (its memory context, freed when cleared).
  - A slot whose unit is no longer the reader's current one (another row was fetched, a cursor moved on) finds its row again by TID through the table's fetch scan. Rows of row-format segments are copied into the slot as heap tuples.
  - TID fetches (index scans, bitmap, UPDATE) keep the decoded units and TID lists of up to 32 superblocks in a backend-wide fetch scan (valid while the directory and the backend's xmin stay; pages unpinned after every fill; the oldest are dropped past 8 MB per table, `SIFT_TBL_FETCH_BYTES`: a miss re-reads the superblock's TIDs, about 600,000 instructions, and 4 slots missed once in 5 fetches of a 100,000-row table whose updated keys sit in 7 superblocks). A fetch that finds a tombstone of the current transaction that deletes the row for the snapshot answers invisible without reading the row, and the first fetch after a begin does not sync the tombstones twice: the uniqueness check of a key updated 20 times in one transaction went from 441,000 to 137,000 instructions an UPDATE (heap 111,000; 600,000-row bench case `tbl_update_keys` 3,076 to 950 ms, heap 705 ms). A caller that needs a row's version, not its values (row locks, uniqueness checks, `index_delete_tuples`), asks without decoding (`sift_tbl_fetch_lazy`: the unit's live-row list answers). `xmin`, `xmax`, `cmin`, `cmax` are read by TID from the same fetch and the tombstones.
- **BM25 `sift` and btree on a `sift_table`**: `ambuild` runs `table_index_build_scan`; `aminsert` receives the synthetic TID from `tuple_insert`; scans return TIDs and the bitmap/index scan nodes call `table_index_fetch_tuple` / `scan_bitmap_next_*`. The sift custom scans and `count(*)` fast paths ask `sift_vis_*` (src/core/visibility.c), which reads the heap's visibility map and calls `heap_hot_search_buffer`; for a `sift_table` the AM provides `sift_tbl_filter_page` (visibility of the candidate offsets of one vblock) and `sift_tbl_nblocks` (the vblock count), and `sift_vis_begin` branches on the table AM.
- **SiftAgg over the table** (`src/table/src.c`, the table provider of the seam `src/agg/colsrc.h`, docs/COL-PLAN.md): the leaves are the table's columns in layout order. The groups are read by the engine, one `sift_col_open_table` source per column layout the segments were written for (leaves of an older layout are matched by attribute number; a column the segment lacks reads as the catalog default: null or the constant of `ADD COLUMN .. DEFAULT`), with the table's visibility as the engine's `SiftColVisibility` hook: `sb_mask` is all-ones for a superblock with no tombstone and no write buffer row, else zero, and `filter_page` is `sift_tbl_filter_page` (insert state, tombstones).
  - Write buffer rows are encoded as one in-memory batch per superblock (`sift_tbl_wbuf_batch`, the promotion's row conversion, fed with `sift_col_add_batches` when a morsel begins) and read like a stored unit; the sources' units are merged in ascending superblock. Aggregates therefore see exactly the rows a sequential scan sees (`tbl_agg` and TAP test 008 compare the two plans in every state: write buffer, promoted, tombstones, own and other transactions' changes, rolled back, added columns).
  - `numeric(p<=18, s>=0)` leaves are the engine's scaled integers (`SiftSrcLeaf.scale`, `sift_col_numeric_key`), `char(n)` is a dictionary leaf of the padded values; leaves stored as exact images (`SIFT_COLS_VAR`: other types, wider numerics) are marked lossy: no query uses them. A column group keeps an odd NaN float apart as an exception: a query that reads such a column of a unit holding one raises 0A000 with the hint to set `sift.enable_agg` off (as the index does). A string of any length and a float `-0` are stored exactly in their leaf.

## 7. Maintenance

**Promotion** (`sift_tbl_promote`, `src/table/promote.c`): a write buffer is sealed, closed to appends (page `SEALED` flags under page locks, directory
kind `SEALED`; `create_wbuf` does it when the segment fills, VACUUM closes the active one once it has 8 pages), and becomes a column
segment when every row is *final* (`row_fate`): `xmin` committed and older than the horizon (kept, frozen), or flagged `DEAD` by VACUUM
(dropped); a row whose insert is aborted or not yet old enough keeps the segment as it is (retried by the next promotion). A row with a
tombstone is promoted as a normal row (its tombstone applies by TID wherever the row lives). The segment is written with the engine writer
(`sift_colw_*`, one unit per superblock of the vblock range, TIDs unchanged), and `sift_dir_update` swaps it for the write buffer entry; the write buffer
pages are released with a `safexid` (reusable once no snapshot can read them: the second VACUUM, or the next allocation, recycles them;
the new table of a rewrite is private, so its extents are reusable at once). The inline promotion takes the maintenance lock only if it is free.

**Bulk load into groups** (`sift.table_bulk_groups`, on by default since 3.3; `src/table/load.c`). `COPY`, `CREATE TABLE AS` and `REFRESH MATERIALIZED VIEW` pass a bulk insert state. With the setting on, the rows of such a command skip the write buffer: each takes the next TID of a run of superblocks the command reserved (4 at a time, under the lock of the write buffers); the rows of a superblock become one unit of a column group, the units of a run one group, published in the directory when the run is full and at the end of the command (`finish_bulk_insert`; the transaction's pre-commit is the last chance).

- The group carries the writer's `xmin` and `cmin` in version vectors (frozen under `COPY ... FREEZE` and the refresh of a materialized view): it is the unfrozen group of the write path above, with no new format; a 3.0 library refuses the table as it refuses one with unfrozen groups (`scripts/old-lib-check.sh`).
- The rows wait in the backend (at most a write buffer's bytes per unit) until their unit is written, so nothing may read them mid-command. The table qualifies only if nothing does: no unique or exclusion index (its check reads the rows already inserted), no trigger of any kind (a foreign key is one), not published (its changes are logged row by row), and no default, generated column, `CHECK`, domain, index expression or predicate that calls a user-written function (a volatile one sees the rows its own command inserted before it, which are not on disk yet; the built-in ones cannot read the table). Any other table keeps the write buffers (`sift._test_tbl_bulk_refusal` of the testing build names why).
- An index entry of the table (btree, BM25) is added as the row is and points at a TID that no group holds until its unit is written: a fetch of such a TID finds no row, as for any uncommitted row.
- A group is published like a promotion's: when the directory is full or past `sift.table_backpressure` the command waits for the maintenance lock and merges first, then settles as a closing writer does. A failed command or dead backend leaves groups of an aborted writer (dropped by VACUUM, as for promotions) and written units nobody published (swept as orphans).
- 200,000 rows of seven columns: the WAL and time are in the changelog.

*Rows of a running transaction* cannot be final. While the directory is more than half full (`nsegs > sift.table_max_segments / 2`) a sealed write buffer is promoted whatever its rows are: the final rows are frozen (xmin 0), the others keep their `xmin` and `cmin` in the group's **version vectors** of format 4. Only a write buffer with a row VACUUM flagged `DEAD` or a speculative insertion in progress waits (the error of section 2.1 is then the end of the line). Version note: 3.0.1 promoted only the write buffers that one command of one transaction wrote, as a tagged group, so a transaction was limited to about 500 MB by that error.

The promotion is part of `sift_tbl_settle`, which an insert runs when it needs a new write buffer and which takes the maintenance lock only if it is free; it also freezes the unfrozen groups whose live rows are committed and older than the horizon, and under pressure merges the smallest groups so the directory keeps shrinking while the load goes on. A 10M-row COPY in one transaction holds about 180 MB in the backend whatever its size, and a loop of 10M single-row INSERTs in one transaction ends with a handful of unfrozen groups.

*Version vectors* (`src/col/ver.[ch]`, `src/col/seg.c`, `src/col/seg_write.c`). A segment with rows that are not final is written as column format 4, with the directory entry flag `SIFT_SEG_VERS` (`reserved` = the smallest xid among them) and a section after the superblock directory of the DOCMAP section:

- a header (magic, version, the unit count, the smallest and largest xid);
- one record per unit (the range of its xids and the highest cmin, a flag for a unit that has frozen rows, the location of its vectors);
- the vectors: the xmin and the cmin of each row of the unit in row order, each as a constant (one transaction, one command) or as a first value, a smallest step and bit-packed residuals, so that a run of single-row inserts (the same xmin, a cmin that goes up by one) takes no bits per row.

A unit all of whose rows are frozen has no vectors; a segment that has none is written in the format `sift.col_format_minor` says (3, as 3.0 writes it, or 4). A group written by 3.0.1 under pressure (one writer: the directory entry's `reserved` = xid, the segment header's `sum_dl` = cid, format 3) is read as constant vectors. The reader asks for the summary of a unit before its vectors: a unit whose xids are all visible, or all invisible, to the snapshot needs no per-row test (`sift_tbl_ver_state`). The metapage minor goes to 3 when the first such group is published (and is never lowered): a 3.0.x library refuses the table, which it would read as final, until `VACUUM FULL` rewrites it (new files have minor 2).

*Layout of a group* (`SiftTblLayout`): one leaf per live column in column order (typed: integers and `numeric(p<=18, s>=0)` as INT with the
scale, `float4/8` as F4/F8, text kinds as STR; any other type, including `jsonb`, `uuid`, arrays and `bytea`, as VAR holding the exact datum
image), then the leaves of the declared jsonb paths (`sift.table_set_paths`, section 11), plus one `exceptions` VAR leaf. A value a typed leaf cannot hold exactly (numeric NaN/Infinity, a float non-canonical NaN)
is stored as NULL with the overflow flag in its leaf and its image in `exceptions`, so every value round-trips
exactly. A segment carries its own schema; after `ADD/DROP COLUMN` it is read by attribute number (missing: the default), segments of
another layout are read side by side and are not merged with the current ones. At most 127 columns (`CREATE TABLE` fails otherwise).

**Merge** (`sift_tbl_merge`, VACUUM: until it finds nothing, a worker until its budget): the tiered policy of `sift_merge_pick` over the segments of the current layout (fan-in 8) plus "mostly dead": a segment more than `sift.table_dead_rewrite_pct` percent dead (default 30; 100 never) is rewritten alone. A `sift_col` index uses 10 because a unit with dead rows is scanned instead of answered from its pre-aggregates; a table keeps its segments until a third is dead, so its file stays near 1.4 times its live rows. Dead rows are dropped by the engine's `sift_col_merge_segments`, which sizes its output runs to its input so that free space left by earlier merges is reused instead of extending the file. A segment of an older layout is not rewritten.

Unfrozen groups merge with everything: the output keeps the version vectors of the rows it holds (`sift_cbatch_merge` carries them) and is frozen outright when every row it keeps is committed and older than the horizon (the freeze callback of `sift_col_merge_segments`); the rows of an aborted writer are dropped not by the merge but by the VACUUM that cleared their liveness. Under directory pressure `merge_smallest` merges the smallest immutable segments of the current layout, unfrozen ones included.

**VACUUM (`relation_vacuum`)**, one pass, restartable:

1. Classify tombstone entries against `OldestXmin`: aborted -> drop; committed and removable -> its TID is **dead**; others kept.
2. Dead TIDs of write buffer rows get the `DEAD` record flag; dead TIDs of group rows clear the engine's liveness bit
   (`sift_cseg_bulkdelete` with a "TID in dead set" callback, WAL logged). Aborted write buffer inserts and the rows of aborted bulk segments are dead too.
3. `index_bulk_delete` on every index with the dead set (rounds bounded by `maintenance_work_mem`), then `index_vacuum_cleanup`. TIDs are
   never reused, so an interrupted VACUUM cannot expose a recycled TID; only index bloat stays until the next one.
   The rows of an aborted writer's group are dead like those of an aborted write buffer: VACUUM finds them from the version vectors (the rows whose xid aborted, as key runs of the dead set that `dead_has` searches), clears their liveness bits after the index cleanup, and the next merge drops them (`sift_tbl_aborted_runs`; `sift_tbl_drop_dead_groups` for a group with nothing live left).
4. Close the open write buffer (8 pages or more), promote, freeze the unfrozen groups whose live rows are committed and older than the horizon, merge (the mostly dead segments included).
5. Rewrite the tombstone segment without dropped entries and without LOCK entries of finished transactions; swap it in (skipped when
   no entry would go).
6. Space: `sift_recycle` clears the free-list records whose releasing transaction no snapshot can see any more, the orphan sweep releases what a crash left, then the reusable free run at the end of the file is cut (`sift_store_truncate`, `core/store.h` P7) as for a heap.
   - Before the cut, the segment whose blocks end last in the file (column segment, tombstones, or the closed and sealed write buffer) is rewritten into the reusable free runs below it while they hold it (`compact_tail`, at most 16 steps; only with the cut enabled; a column segment of an older layout stays). A load seals its column segment above the write buffers it replaces, so without this the file stays the size of the load (1M orders: 163 MB to 14 MB; the dead rows of a rewritten segment go with it).
   - The cut happens when it is 1000 blocks or a 16th of the file, `vacuum_truncate` is on (heap reloption, or `VACUUM (TRUNCATE)`), and `AccessExclusiveLock` is free right now (`ConditionalLockRelation`, released as soon as the file is cut). Under it no backend holds a scan, directory copy or target page; the store sends a relcache invalidation before the lock goes, marking every backend's `SiftTblRel` invalid (target page, write buffer page tables and fetch scan are rebuilt on next use), and a standby replays the lock first, cancelling queries that hold the table there.
   - A lazy VACUUM is in no snapshot or horizon, so its own releases carry the next xid (`xid_safexid`) and are reused once another transaction has ended; under the lock every record is reusable (`sift_store_horizon`), so the cut takes the free run at the end whatever released it. On an idle database a second VACUUM right after the first finds the same blocks not yet reusable (3.1 measurement, 300,000 narrow rows: 2,907 of 2,994 blocks free after the first VACUUM, 16 reusable, the same after the second); one transaction that takes an xid (`SELECT txid_current()`) and a VACUUM cut the file to 103 blocks (824 kB). `sift.table_space` reports the free blocks a size benchmark must add.
   - Live segments in the middle of the file stop the cut, as live rows do for a heap; later writes reuse their space.
7. `vac_update_relstats` (relpages, reltuples = live rows, `relfrozenxid`), `pgstat_report_vacuum` (live rows; dead rows = deleted by a
   committed transaction but not removable yet), counted as a heap does: an in-progress insertion is not live yet, an in-progress deletion
   still is; group rows from the liveness bits less the recently deleted ones. `VACUUM VERBOSE` reports the rows removed, the rows that
   remain and the dead rows not yet removable, and the truncation. The counters of insert/update/delete that the AM reports keep
   autovacuum's thresholds meaningful (`pgstat_count_heap_insert/update/delete`).

`pg_repack` cannot rewrite a `sift_table` (it copies the rows into a heap table and swaps the files, which would replace the column segments with heap pages): the event trigger `sift_table_repack_guard` refuses its `repack.repack_trigger` on one, `pg_repack --only-indexes` is fine, and a run without `-t` stops at the first `sift_table`.

`VACUUM FULL` and `CLUSTER` are `relation_copy_for_cluster`: the kept versions are written straight into frozen column segments of the new table with dense TIDs (`sift_tbl_bulk_*`: no row-format pages, no promotion).

- VACUUM FULL re-keys the rows of the old units as they decode (no tuple is formed) when every column segment has the current layout, then adds the write buffer rows; otherwise (after `ALTER TABLE`) it reads the scan's tuples.
- CLUSTER sorts the scan's tuples by a btree index whatever the planner's `use_sort` says (a TID fetch decodes the unit of its superblock, so an index scan in random order would decode a unit per row) and fetches by index scan only for an index that cannot order a sort.
- A version deleted or updated by the rewriting transaction itself is dropped (the rewrite commits or aborts with it).

`VACUUM (INDEX_CLEANUP off)` skips step 3 (indexes keep dangling entries). `sift.table_space(tbl)` shows the file, the high-water mark, the free list and the virtual blocks of TIDs left (`vblocks_left`).

**`sift.table_maintain(tbl)`** does steps 2, 4 and 6 and nothing that needs the indexes: it promotes the write buffers, clears the liveness bits of group rows whose deletion is committed and older than the horizon (`GetOldestNonRemovableTransactionId`, as the inline promotion uses), merges and rewrites the mostly dead segments, recycles, sweeps, relocates the tail segments and cuts the free end. Tombstone entries stay: they are the dead set the next VACUUM hands to the indexes (a cleared bit with its tombstone is the state of a VACUUM stopped before step 3), so the file ends near the VACUUM's size plus the tombstone log, which only that VACUUM shortens; the rows of an aborted writer are left to it too (only the live bits say which they are). It takes `ShareUpdateExclusiveLock` and the maintenance lock only if free (`busy` in the result otherwise) and the file's lock as VACUUM does (`ConditionalLockRelation`, given up at once), never waits for a reader or writer, refuses a read-only transaction, a standby and a role without `MAINTAIN`, and writes only what VACUUM writes (liveness pages, directory switches, free list, truncation: same crash points).

*Background pass* (`sift.table_auto_defrag`, off by default for 3.2, needs `sift.async_maintenance`). A backend asks for a worker (`sift_maint_request_worker` as for the indexes; at most one request per `sift.maintenance_naptime` of a table, backing off after failures; one worker per table) when:

1. tombstone entries are at least `sift.table_auto_defrag_dead_pct` percent (5; autovacuum starts at 20) of the column segments' rows and have grown by half of that since this backend last asked (only tombstones older than every snapshot count, so a long transaction holding the horizon makes the pass wait instead of reading the table to remove nothing); or
2. free blocks are at least `sift.table_auto_defrag_free_pct` percent of the file, worth the exclusive lock by VACUUM's rule (1000 blocks or a 16th of the file), and grown by a 32nd of it since the last request.

The backends that look are those that close a write buffer (every `sift.table_write_buffer_limit` of writes; not inside a VACUUM) and, at most once per `sift.maintenance_naptime` each, those that delete, update, lock or read the table (scan, index fetch, SiftAgg): a table of small writes or only deletes closes no write buffer for a long time, and nobody sees a table nobody uses. The check reads no page of a table never written.

- For (1) the worker runs the whole table VACUUM (`sift.table_auto_defrag_indexes`, on: without the index cleanup the tombstone log, which every scan reads and which holds the file above the live rows, cannot shrink; with it off the pass is `sift.table_maintain` and the tombstones outlive it until a VACUUM); for (2) `sift.table_maintain`. Both run in transactions of their own with no snapshot, at most 32 MB copied under the table lock.
- A pass rewrites at most `sift.table_auto_defrag_budget` MB in its merge steps (mostly dead segments first, the most dead first: what a rewrite frees per byte copied is its dead share), and passes again while the budget stopped the last one. Small groups next to the one a rewrite picks join it (`sift.table_auto_defrag_coalesce`).
- Foreground impact: a cost delay as autovacuum (page accesses charged against `vacuum_cost_limit`; `sift.table_auto_defrag_delay` is the sleep, after each page of a write buffer VACUUM judges and after each merge step; the table VACUUM of autovacuum is charged the same way); a lock of its own (`SIFT_WORKER_LOCK_BLKNO`) that no delete or update takes; a wait for a quiet moment when `sift.table_auto_defrag_max_active` clients run a statement (also between merges); and `sift.table_auto_defrag_wal_rate` kB of WAL a second.
- What remains is the cost of the new segments: every backend that reads a segment inflates and indexes its string dictionaries once (`col/dictcache.c`, keyed by the generation, which a rewrite changes), 100 ms for a column of 200,000 distinct values, and a pass writing many segments sends that to every client at once; the WAL rate bounds it. Measured (200,000 rows, 4 clients for 40 s, 10% threshold): about 9% of the throughput (966 against 1,058 TPS, same p99) and, in about half of the runs, stalls of 100 to 130 ms of every client at once that no lock, cost delay, truncation or index cleanup explain and no setting removes (250 to 355 ms before the page lock was told apart from the worker's).
- The request may come while the writer still works (its rows are not final, the end-of-file lock is not free), so the worker stays and passes again every fourth of `sift.maintenance_naptime` while a pass finds something to do (two in a row that do not end it, 20 at most).

Autovacuum is the other trigger and needs none of this: its dead-row threshold is 20% of the rows and its naptime a minute; the worker answers in `sift.maintenance_naptime` and also covers free space, which autovacuum's counters do not see.

**ANALYZE** (section 4): statistics columns as PostgreSQL computes them from the sample; jsonb shredded paths are not statistics targets.

---

## 8. Compression, options, defaults

- Compression is on by default (col-compress: algorithm per leaf, `auto` picks per chunk from FOR/PACK/dictionary and zstd/lz4 when the
  library is present); SIMD kernels are chosen at runtime from `sift.simd` (`SET sift.simd = 'scalar'` for the specification).
- Settings (GUCs; a table access method cannot have reloptions): `sift.table_write_buffer_limit` (kB, 2048-8192, default 4096: size of a write
  segment, whole superblocks of 2 MB, so 2 MB is the effective minimum; a row has to fit one segment), `sift.table_inline_promote` (default on), `sift.table_bulk_groups` (default on, section 7),
  `sift.table_compression` (the `compression` grammar of `sift_col`: `auto`, `none`, `lz4[:N]`, `zstd[:N]`, encodings; `'auto; col=zstd:9'` per column;
  taken from the session that promotes, autovacuum uses the server setting), `sift.table_dead_rewrite_pct` (1-100, default 30: section 7,
  merge). Segments are self-describing, so changing a setting affects only
  the segments written afterwards. `sift_group_rows` (rows per unit: one superblock) is not provided; the jsonb paths of a column are declared with `sift.table_set_paths` (section 11).
- `ALTER TABLE ... SET ACCESS METHOD sift_table` rewrites the table through `relation_copy_for_cluster`-style bulk writes.
- Types: every type, exactly (section 7, layout of a group): typed leaves for what the engine stores natively, opaque `SIFT_COLS_VAR` leaves
  for the rest, an `exceptions` leaf for the values a typed leaf would change.

---

## 9. Limits

BLCKSZ 8192; PostgreSQL 17, 18 and 19; rows up to 1 GB (varlena limit).

- At most 127 columns: `CREATE TABLE` fails past it (error code `54000`), and so does an `ALTER TABLE` that adds a column or brings declared paths back past it, by the event trigger `sift_table_ddl_check`. `DROP COLUMN` is not checked, so that a table made too wide by 3.2, whose writes fail with the same error, can be narrowed.
- 128 directory entries: under directory pressure every write buffer becomes an unfrozen group whatever wrote it, and the groups merge, so a load of any size by any mix of commands, writers and savepoints is not bounded by them. What still waits is a write buffer that holds a deleted row VACUUM has not seen or a speculative insertion in progress; the insert that cannot open a write buffer then fails with the error of section 2.1.
- vblocks 2^32 (about 1.2 x 10^12 rows ever written, counting dead ones: a table that exhausts them is rewritten by `VACUUM FULL`).
- Unsupported: multixact-based shared row locks (they degrade to exclusive), HOT, speculative insertion through foreign tables (n/a), rewrite of a table with RLS-bypassing rewrites (n/a).

## 10. Tests

Run by `scripts/ci.sh` on PostgreSQL 17, 18 and 19; `SANITIZE=1` (ASan + UBSan) runs the same jobs.

- Regression: `test/sql/tbl_basic` (DDL, DML, rollback, savepoints, locks, unique and `ON CONFLICT`, VACUUM/ANALYZE, wide rows, TRUNCATE, CTAS, partitions, temp tables, `ALTER ... SET ACCESS METHOD`, `ADD/DROP COLUMN`), `tbl_bm25` (BM25 index agrees with the sequential scan through DML, VACUUM, REINDEX [CONCURRENTLY], CIC), `tbl_promote`, `tbl_space` (released blocks reused; the free end cut one VACUUM after it is freed, not with `vacuum_truncate` off), `tbl_dead` (live/dead counts; rewrite under `sift.table_dead_rewrite_pct`), `tbl_rewrite` (CLUSTER by btree sort and GiST scan, VACUUM FULL over two layouts), `tbl_sample`, `tbl_cap` (the limit lowered by `sift.table_max_segments`), `tbl_bigtx` (one transaction with more write buffers than directory entries, at a limit of 16; a heap twin must agree).
- **Differential suite** `tbl_diff`: the same statements on a heap table and a sift_table, EXCEPT ALL both ways plus index scans, across DML, rollbacks, MERGE, CTEs, aggregates, windows, locking, VACUUM [FULL], REINDEX, CLUSTER, rewrite to heap and back, TRUNCATE.
- Isolation: `tbl_bigtx` (rows invisible to every scan kind at READ COMMITTED, REPEATABLE READ and SERIALIZABLE until commit, gone after rollback, with VACUUM running) and `test/isolation/specs/tbl_concurrency.spec` (update/delete races at READ COMMITTED, REPEATABLE READ first-updater-wins, SERIALIZABLE write skew, FOR UPDATE / SHARE / NOWAIT / SKIP LOCKED, unique wait, `ON CONFLICT`).
- TAP: `test/t/006_tbl.pl` (crash recovery, hot standby, `wal_level = minimal` with both `wal_skip_threshold` settings, `pg_dump`/`pg_restore` plain and custom), `test/t/007_tbl_logical.pl` (publication with and without preload, 0A000 with the hint, replication to heap and sift_table subscribers, physical standby of the publisher), `test/t/008_tbl_promote.pl` (autovacuum promotion and reclaim, parallel workers, a reader across a promotion), `test/t/013_tbl_bigtx.pl` (standby replay with WAL consistency checking, crash mid-transaction, immediate stop, promotion, restored `pg_dump`).
- Benchmarks: `bench/tbl-bigcopy.sh` (COPY of 5M rows or more in one transaction with a primary key: time, peak backend memory, aggregate scan before and after VACUUM); `bench/ab-query.sh` (two builds on a heap table; the BM25 paths differ only by `sift_vis_nblocks` and the non-heap branch of the page filter).

## 11. Open decisions and risks

- Tombstone cache memory is O(live tombstones) per backend; the upgrade path is a per-SB tombstone index (pointer pages) so only the SBs a
  query touches are loaded. Mass deletes should be VACUUMed (autovacuum does) or avoided (TRUNCATE, partition drop).
- Shared row locks degrade to exclusive; implementing MultiXact would need a lock-owner set per row.
- Relation-level SSI locks; a per-SB granularity is possible once the engine reports the SBs a scan touched.
- Typed jsonb paths for a `sift_table`: `SELECT sift.table_set_paths('t', 'doc', 'doc->ch text, doc->w int4, doc->items[]{k int4, p float8}')` (the
  `paths` grammar of `sift_col`, the column named in each path; `NULL` or an empty spec drops the declaration). Without it a jsonb column is one opaque
  leaf of exact bytes and `SiftAgg` runs PostgreSQL's `jsonb_to_recordset` on every row. The declaration is a row of `sift.table_paths(relid, attname,
  spec)` (an extension configuration table, so `pg_dump` carries it; keyed by column name, so a renamed or dropped column has no paths); an event trigger
  clears it with the table. Promotion, merge and `VACUUM FULL` shred the declared column into leaves next to the exact bytes (no format change: a segment
  carries its schema), and the write buffer is shredded as rows are read. The planner uses the paths only when every segment it reads has them, so the
  plan stays the normal one until `VACUUM FULL` has rewritten the older segments, and a rewrite of the table that is not `VACUUM FULL` (`ALTER TABLE ...
  TYPE`) drops the leaves until the next one. A value of another JSON type than its path declares is kept exactly (overflow) and a query that reads
  that path keeps the normal plan while such a row exists; the `jsonb_typeof` forms still run (`filter_types`). Not done: rollups of the elements for a
  table (the scan reads the elements), and ANALYZE gives the paths no statistics.
- The logical record doubles WAL for published tables; a custom rmgr for all page changes would remove the double write but demands
  preload everywhere.

## 12. Interfaces to the engine

The table AM is written against `src/col/col.h`, `col/enc.h`, `col/seg.h` of the column track and has no second implementation of
anything the engine does: group segments are written by `sift_colw_*` (and `sift_col_merge_segments`), read by `sift_col_open_table` /
`sift_col_begin/next/read`, cleared by `sift_cseg_bulkdelete`, with the table's visibility as the engine's `SiftColVisibility` hook.

| piece | status |
|---|---|
| segment writer `sift_colw_*`, `sift_col_merge_segments` | used by promotion and merge |
| `sift_col_open_table(rel, schema, dir, vis, heap_sbs, mcxt)`: a source over the groups of a table | used by scans, fetches, ANALYZE |
| `SIFT_COLS_VAR` (exact datum images, engine), `exceptions` leaf (table layout) | used |
| liveness `sift_cseg_bulkdelete` with a TID callback | VACUUM step 2 |
| `sift_colw_set_tag` (segment xmin of a bulk write) | removed with the write path of 3.1: the engine writer takes the `xmin`/`cmin` of every row from the batch (`sift_cbatch_add_ver`), and the direct column writes of a rewrite are frozen (no versions) |
| `sift_col_add_batches` (write buffer rows as in-memory units), the `SiftSrcProvider` seam of `agg/colsrc.h` | used by the table provider (`table/src.c`) |
