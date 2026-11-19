# Operating pg_sift

What to watch, what to switch off in an emergency, what to run after a crash, and how to read the locks and waits of the extension. The outputs below were taken from a scratch cluster (PostgreSQL 18, a 300,000-row `orders` table with a `sift_col` index and a `sift` index, the defaults of 5.0, UTC) after a bulk load, an update and a `VACUUM`; the names are generic and the figures are those of that small lab. Settings and options: [OPTIONS.md](OPTIONS.md); every function and view: [REFERENCE.md](REFERENCE.md); upgrades: [UPGRADING.md](UPGRADING.md).

Every view below shows only the objects the caller may look at: the owner of the table, a role with `SELECT` on it or with `MAINTAIN`, or a member of `pg_monitor`. Run the monitoring as such a role, not as a superuser, to see what an application role sees.

## 1. What to monitor

Five queries cover a running server. The alert conditions are columns of the output, so they can go into any monitoring system as they are.

**Background work: `sift.workers`.** One row for each `sift` and `sift_col` index and each `sift_table`, with its directory, whether a worker runs, and the last failure and the last success.

```
    index    |   kind   | segments | sealed | running | failures | failed_at | next_retry | due |            last_ok            | last_job | last_ms | failures_total
-------------+----------+----------+--------+---------+----------+-----------+------------+-----+-------------------------------+----------+---------+----------------
 orders_col  | sift_col |        5 |      0 | f       |        0 |           |            |     | 2026-11-15 06:29:31.408459+00 | vacuum   |    1005 |              0
 orders_note | sift     |        2 |      0 | f       |        0 |           |            |     | 2026-11-15 06:29:30.777199+00 | promote  |      89 |              0
```

`failures` counts the failures in a row and is cleared by the next success; `failed_at`, `error_code` and `last_error` keep the last failure after a later success, and `failures_total` counts them since the index was built. `next_retry` is when the worker tries again (the delay doubles from `sift.maintenance_naptime` up to an hour). `due` is the reason a `sift_col` index waits for maintenance (`sift.col_maintenance_due`), NULL when nothing is due. `sift.workers` also has `relation` and `"table"` columns (the index and its table; for a `sift_table`, both are the table).

**One index in detail: `sift.col_index_status`.** Sizes, directory, dead rows, the write buffer and what blocks maintenance.

```
 table  | index_bytes | free_bytes | segments | max_segments | dead_pct | buffer_rows | sealed_buffers | running | failures | next_retry | due | blocked_by
--------+-------------+------------+----------+--------------+----------+-------------+----------------+---------+----------+------------+-----+------------
 orders |     5627904 |      57344 |        5 |          128 |     0.00 |           0 |              0 | f       |        0 |            |     | {}
```

It also reports `format`, `min_segment_format`, `max_segment_format` and `library_reads` (`1.2`, `41`, `41` and `3 to 51` here), the formats the index holds against the formats this library reads, for a rollout where some nodes still run an older release.

**What runs now: `sift.progress`.** One row for each maintenance job that runs on an index you may look at: the merges of a `VACUUM` (in the process of the session) and the jobs of the background workers. The rows live in the shared memory of the server and go when the job ends or fails.

```
 pid     | index       | job     | phase   | segments_done | segments_total | bytes_written | started
---------+-------------+---------+---------+---------------+----------------+---------------+-------------------------------
 3774914 | orders_note | vacuum  | merge   |             0 |              3 |             0 | 2026-11-15 06:29:31.267223+00
 3774927 | orders_col  | promote | promote |             0 |              1 |             0 | 2026-11-15 06:29:30.509237+00
```

`job` is `promote`, `merge`, `rewrite` or `vacuum`; the `phase` of a rewrite is `rewrite` or `merge`, that of a `VACUUM` `promote`, `merge` or `space`. `CREATE INDEX` and `REINDEX` report in `pg_stat_progress_create_index` as any index build does. In `pg_stat_activity` a worker says what it is: `backend_type` is `sift <job>` (`sift promote`, `sift merge`, `sift rewrite`, `sift vacuum`, `sift track`, `sift rollup`, `sift orphans`, `sift table`), the process title is `sift <job> <schema.index>`, and a running worker has `state` `active` and the query `sift <job> of <schema.index>`:

```
SELECT pid, backend_type, state, query, wait_event_type, wait_event FROM pg_stat_activity WHERE backend_type LIKE 'sift%';
 3774914 | sift vacuum | active | sift vacuum of public.orders_col  | Timeout   | VacuumDelay
 3774927 | sift merge  | idle   | sift merge of public.orders_col   | Extension | SiftMaintenanceNap
```

**Shared memory: `sift.shared_memory()`.** The tables the backends of the server share and whether this backend has them.

```
        name         | configured_bytes | mapped_bytes | used_bytes | hits | misses | shared | reason
---------------------+------------------+--------------+------------+------+--------+--------+--------
 sift_stats          |                  |         1416 |            |      |        | t      |
 sift_col_leaf_stats |         16777216 |     17006640 |       6416 |    1 |      3 | t      |
 sift_progress       |                  |         4616 |            |      |        | t      |
 sift_col_dict       |                  |        49352 |            |      |        | t      |
 sift_col_wbuf_arena |                  |       475208 |            |      |        | t      |
```

`shared` false with a `reason` (a full `/dev/shm`, a directory with no free entry) means the table is private to each backend: nothing fails and answers are the same, only the sharing is lost. The reason is also logged once for the server.

**Soundness: `sift.check()`.** Reads every index and `sift_table` the caller may maintain and returns one row for each problem; no row means sound. It never raises on damage, takes the maintenance lock one segment at a time, and works on a standby.

```
SELECT * FROM sift.check();                         -- every object you may maintain
SELECT * FROM sift.check('orders_col', true);       -- one index, and compare its rows with the table
 index | segment | problem | detail
-------+---------+---------+--------
(0 rows)
```

On the 300,000-row lab the check takes 0.2 s, and 0.35 s with the comparison against the table (the second argument, `heapallindexed`; it keeps a hash of each row, so give a large table `maintenance_work_mem`). A damaged segment is one row with the segment number and a message; a total of the metapage or a row count that disagrees with the table is a row without a segment. `sift.check` stops at the first problem of a segment, and the segments after a damaged one are checked.

### Alert conditions

| Watch | Column | Alert when | Meaning and first step |
|---|---|---|---|
| a worker fails | `sift.workers.failures` | `> 0` | the step failed; `last_error` and `error_code` say why; it retries at `next_retry` |
| failures over time | `sift.workers.failures_total` | rises | the index fails now and then even if `failures` is 0 |
| nothing finishes | `sift.workers.last_ok` | older than your window while `due` is not NULL | a worker cannot start (no slot, see below) or keeps failing |
| maintenance is wanted | `sift.workers.due`, `sift.col_maintenance_due(index)` | not NULL for longer than a few `sift.maintenance_naptime` | the reason says what (dead rows, a full write buffer, `value store epoch`); `SELECT sift.col_maintain('<index>')` applies the rule now |
| a job that does not end | `sift.progress.started` | older than the longest job you expect | look at `pg_stat_activity` for the pid; a merge of a 2 GB index takes minutes |
| growth of the index | `sift.col_index_status.free_bytes` against `index_bytes` | free space stays high after a `VACUUM` | `sift.col_shrink_history(index).shrink_blocked_by` says why the file was not cut: `standby`, `lock` or `snapshot` |
| dead rows | `sift.col_index_status.dead_pct`, `dead_rows` | high and rising | the table is not vacuumed, or a snapshot holds the rows; a `VACUUM` of the table cleans the index |
| write buffer | `sift.col_index_status.buffer_rows`, `sealed_buffers` | `sealed_buffers` stays above 0 | promotions are not running |
| directory | `segments` against `max_segments` (128) | within a few dozen of the maximum | merges are behind; inserts wait for the worker at 112 entries |
| format rollout | `library_reads`, `format` | a node that reads fewer formats than the index holds | see [UPGRADING.md](UPGRADING.md) |
| shared memory | `sift.shared_memory().shared` | `false` | read `reason`; the server needs more room in `/dev/shm` or `dynamic_shared_memory_type` |
| a quarantined index | `sift.quarantine` | any row | the index found damage and is out of service; queries run on the heap. Rebuild it as above |
| soundness | `sift.check()` | any row | follow section 3 |
| object state | `sift.indexes.state` | `invalid` (an interrupted `CREATE INDEX CONCURRENTLY`), `failing`, `locked` for long | `maintaining` and `ready` are normal |

`sift.indexes` lists every `sift`, `sift_col` and `sift_table` object of the database with its size, segment count, format and `state` (`invalid`, `locked`, `failing`, `maintaining` or `ready`), whatever its access method; the inspectors of one access method (`sift.segments`, `sift.col_segments`) name the right function when you call them on the other kind.

**Why a query did not use the index.** `EXPLAIN (SIFT)` (PostgreSQL 18 and later, with the library loaded in the session, see section 5) prints the index it used with its segments, buffered rows and dead share, or `Sift: not used: <slug>: <reason>` for each candidate it refused; `sift.explain_why(query)` returns the same refusals as rows on every version and `sift.refusal_actions()` says what to change for each slug. A refusal is recorded when the statement is planned, so the `EXPLAIN` of a cached generic plan prints none.

```
EXPLAIN (COSTS OFF, SIFT) SELECT category, count(*), sum(amount) FROM orders GROUP BY 1;
 Custom Scan (SiftAgg)
   Index: orders_col
   Sift Index State: 2 segments, 0 buffered rows, 0.0% dead
   Group Keys: category
   Pushed Aggregates: count(*), sum(orders.amount)
```

## 2. Emergency

The order matters: look first (`sift.workers`, `pg_stat_activity`), then switch off the smallest thing that stops the harm.

| Problem | Switch | What it stops | What it does not stop |
|---|---|---|---|
| a query is slow or wrong on the index | `SET sift.enable_agg = off` (a session, a role or a database; `ALTER SYSTEM` for the server) | the planner's choice of `Custom Scan (SiftAgg)`: the plan is PostgreSQL's own | the index is still written by every `INSERT`, `UPDATE` and `DELETE`, and still maintained |
| the bitmap scan of a `sift_col` index misbehaves | `SET sift.enable_col_scan = off` | the bitmap scan on the `sift_col` operator classes | the aggregate scan (that is `enable_agg`) |
| background maintenance hurts (slots, I/O, locks) | `ALTER SYSTEM SET sift.async_maintenance = off`, reload | every promotion, merge and rewrite in a worker, for `sift`, `sift_col` and `sift_table` | the inserting sessions promote and merge themselves when the write buffer or the directory is full (writes get slower, not wrong); `VACUUM` still merges; `sift.merge`, `sift.promote` and `sift.col_maintain` called by hand still work; the workers of the rollup refresh and of the orphan sweep (below) |
| the incremental maintenance hurts | `ALTER SYSTEM SET sift.col_auto_merge = off`, reload | the promotion of the buffer, the merge of the small segments, the table `VACUUM` and the re-baseline of a tracked log that a `sift_col` index asks a worker for | promotion and merge of the buffer (that is `async_maintenance`); read by the worker at the server and database level only, a session `SET` has no effect. Nothing rewrites an index whole by itself |
| a `sift_col` index is slower than it was after churn, a parent table changed its keys, or its value store has garbage | `SELECT * FROM sift.col_maintenance_status('idx')`: `rewrite_due` is the reason and `blocked_by` says `REINDEX recommended (reason)`. For a connected child index `stale connection` appears when parent keys were deleted or changed, or parent rows were inserted, since its segments were filled, by at least `sift.col_connect_refresh_pct` (10) percent of the parent rows; updates of the parent's other columns and the merges and `VACUUM` of the parent index never make it appear. Parent rows inserted since count however the parent index was merged since, a `key_cut` parent included (the stamps of the child hold the parent's insert horizon: the count of rows that came into it); a child stamped before 5.0.2 is told only by deleted or changed keys until `REINDEX` writes it again, and a rolled-back insert counts as one that came in. Nothing rewrites the index by itself, so nothing needs stopping; reads answer correctly from the rows of the units meanwhile | none: the server does not rewrite a `sift_col` index | `REINDEX INDEX CONCURRENTLY idx` in a window of your choosing (the disk holds both files until the swap, a connected index reads the keys of its parent once); `sift.col_maintain(index)` rewrites in place, holding the maintenance lock |
| the `sift_table` pass hurts | `ALTER SYSTEM SET sift.table_auto_defrag = off`, reload | the background pass of every `sift_table` | `VACUUM` and `VACUUM FULL` of the table |
| the rollup refresh worker | `ALTER SYSTEM SET sift.col_rollup_worker = off`, reload | the worker; its requests stay queued | `sift.col_rollup_refresh(index)` applies a queued refresh by hand; a scan that wants rollups does not wait for it |
| event triggers break DDL (a library that does not match the SQL) | `SELECT sift.ddl_hooks_enable(false)` (a superuser) | all ten event triggers of the extension in one statement; `SELECT * FROM sift.ddl_hooks` lists them with what each does | what they checked and cleaned up is not done meanwhile (a `DROP` leaves rows in `sift.table_paths` and `sift.table_option_values`, a rename is not written into the options of an index, a `CREATE INDEX` is not serialised by the build lock); `SELECT sift.ddl_hooks_enable(true)` enables them again and sweeps what the drops left |

`sift.upgrade_commands()` lists a required row of kind `hooks` for a superuser while some event triggers are disabled, so a switch left off is not forgotten.

**Worker slots.** The background workers never take the last slots of `max_worker_processes`: a worker of sift starts only while `Max(2, max_worker_processes / 4)` slots stay free after it, so a parallel query or another extension still finds one. With `max_worker_processes` of 4 and the logical replication launcher running, one worker of sift runs at a time; with 2 or less none starts and the sessions do the work. What could not start says so in one line an hour for each job (`sift: no background worker for <job> of index "<schema.index>"`, with the slots in the detail and which jobs hold them; DEBUG1 in between). A worker does its pass and ends: after its work it looks twice more, at growing intervals up to a second, and leaves when both looks find nothing, so an idle worker lives well under `sift.maintenance_naptime` and writes no line to the log unless it did work that took `sift.log_min_duration` or failed; what is left is asked for again by the next read or write of the table, so `DROP DATABASE` is not held up by an idle worker.

**A damaged index leaves service.** A sift or sift_col index that finds bytes its format does not allow (SQLSTATE `XX001` or `XX002`) is taken out of service, and the database keeps serving:

- The statement that found the damage fails once, with the original error and `REINDEX INDEX <schema.index> rebuilds it from the table.` in the hint.
- The index is recorded in `sift.quarantine` (`index`, `detected_at`, `persisted`, `error_code`, `reason`, `detections`, `skipped_writes`, `skipped_vacuums`, `refused_scans`) in the registry of the server at once, and in its metapage by a background worker, so a restart and the standbys keep it (`persisted` says it is there). A standby that meets damage on its own confirms it on a second read at least a second later and keeps it in its own registry. Damage that a parallel worker meets is recorded the same way, and the leader fails once with the error of the first worker that failed: the damage error, or SQLSTATE `40001` (retry) from a worker that opened the index after another had recorded the damage.
- The server log says why, whoever catches the error: a `LOG` line `sift index "<name>" is out of service after damage` with the SQLSTATE and text of the first cause in its detail.
- Every statement planned afterwards runs on the heap: PostgreSQL's own plans, with the answers the table gives, never a partial answer from the index. A plan made earlier gets SQLSTATE `40001` once (retry the statement) and is planned again without the index.
- `INSERT`, `UPDATE`, `DELETE`, `VACUUM` and the background workers skip the index with a `WARNING` once a minute for each index and session. Nothing is lost by it: the index does not keep up with the table while it is out, so it leaves quarantine only by being rebuilt.
- `REINDEX INDEX`, `REINDEX INDEX CONCURRENTLY`, `CREATE INDEX CONCURRENTLY` with `DROP INDEX`, and `sift.check` always work on a quarantined index; the rebuild gives a new file and with it a clean index.
- A `VACUUM` that is running when another session quarantines the index stops reading it at its next segment and ends with the `WARNING`; a background worker ends at its next step.
- `sift.workers`, `sift.col_index_status` and `sift.indexes` list the index with `quarantined`, `quarantined_since` and `quarantine_reason` (the state `quarantined` in `sift.indexes`) and read nothing from its pages, so the views that find a damaged index do not fail on it.
- A function that has no result without the index says so: `sift.score()` and `sift.max_score()` name the quarantined index in their hint, and `sift.facets()` refuses with SQLSTATE `55000` and `REINDEX INDEX ... puts it back in service`. `sift.approx_count_distinct` and the aggregates that sift answers from a column index are plain aggregates on the heap plan, with the same results.
- When no background worker slot is free the registry still holds the index out of service and `persisted` stays `false`: the next plan that leaves the index out asks for the worker again, at most every 10 seconds, and the line of the worker slots says why it did not start. A restart before the metapage is flagged forgets the quarantine, and the next read of the damage records it again. A metapage or directory that is damaged itself cannot be flagged: the registry alone holds the index out of service (`persisted` stays `false`, one `LOG` line says so) until REINDEX.

Detect with the `WARNING`, with `SELECT * FROM sift.quarantine`, and with `sift.check()` (damage that no statement has read yet); alert on any row of `sift.quarantine`. Recover with `REINDEX INDEX CONCURRENTLY <index>`, or `CREATE INDEX CONCURRENTLY` of the same definition and `DROP INDEX` of the damaged one. The rebuild of a large index is a decision of yours, at the time you choose: sift never starts one by itself, because an unattended rebuild of a multi-gigabyte index is a load nobody planned for.

Plan for slower queries while an index is out: the heap plans of a large table can take far longer than the index did, and there is no fail-fast mode. A statement that must not run that long needs `statement_timeout`, or `SET sift.on_corruption = error` (a superuser; per database or role with `ALTER DATABASE` or `ALTER ROLE`) to have every read of damage fail as in 4.3.

A result that differs from the table's with no error means damage that no check in the read path can see: the structure of a page (type, generation, lengths, bounds) is checked at every read, the bytes of a posting list or of a column chunk inside a structure that is still well formed decode to other rows, and only a comparison with the heap finds them (a changed byte in the bits that count the postings of a BM25 posting list block is such a case). `sift.check(index)` reports the malformed encodings and `sift.check(index, true)` (`heapallindexed`) compares every heap row with the index; run them after a storage incident, a crash of the storage layer or a restore, and `REINDEX` an index that reports a row. The read path does not verify the bytes it does not need. PostgreSQL's data checksums (`initdb --data-checksums`) find a page that changed on disk or in transit; they do not find a change made in memory before the page was written.

What it does not cover, on purpose: errors that PostgreSQL itself raises for an invalid page (`invalid page in block`, a checksum failure) are not sift's to swallow and are not recorded; a `sift_table` is the data and has no heap to fall back to; the diagnostics (`sift.check`, `sift.col_verify`, `sift.verify`) report damage and do not quarantine; a refusal of a newer format, a mismatch of options and cancellations (`57014`, `53200`, `40001`) are no damage.

## 3. After a crash

The index pages are logged with generic WAL, so crash recovery, a standby and PITR restore them like any relation; no sift-specific step is needed to start. After an unclean shutdown, a failed storage or a restore from a base backup:

```
SELECT * FROM sift.check();                                   -- every object you may maintain; 0 rows = sound
SELECT * FROM sift.check(heapallindexed => true);             -- also compares each index with the table (slower)
```

then `REINDEX INDEX CONCURRENTLY <index>` for each index that has a row (`sift.check` names it in the `index` column and the damaged segment in `segment`). An interrupted `CREATE INDEX CONCURRENTLY` or `REINDEX CONCURRENTLY` leaves an invalid index (`sift.indexes.state = 'invalid'`): drop it and build it again. A `sift_table` cannot be reindexed; `sift.check` and `sift.table_verify(table)` name its damaged segment, and the table is restored from a backup. `sift.check` works on a standby, so a replica can be verified without promoting it.

**Damaged liveness pages (4.2.1 to 4.3.5).** A `VACUUM` or a delete that marked rows dead in a column segment of more than 65,151 superblocks set the dirty bit of a superblock on the wrong page of the liveness bitmap: it overwrote the type, flags or generation of a page (every later read of the index failed with `sift index "<name>" is corrupted`), or the bit was lost and the deleted rows of that unit read as live, with no error. 4.3.6 and 5.0 no longer write it. An index that an older release touched is checked and restored in place, no `REINDEX` needed:

```
SELECT * FROM sift.col_verify('orders_col');            -- also sift.check(): the liveness pages of every segment, one row for each finding
SELECT * FROM sift.col_repair('orders_col', true);      -- dry run: one row for each segment it would change, none when the index is sound
SELECT * FROM sift.col_repair('orders_col');            -- restores them: one row for each segment changed (damaged_pages, units_fixed, ndead_before, ndead_after)
```

`sift.col_repair` takes the maintenance lock (a `VACUUM`, a merge or a rewrite of the index waits for it), writes one WAL record for each page it restores, and runs on the primary; a standby replays it. It sets the dirty bit of each unit that has a cleared liveness bit and the count of dead rows to the number of cleared bits. When it says that a page cannot be restored, `REINDEX INDEX CONCURRENTLY <index>` is the way. Run the check once on every index of a server that ran 4.2.1 to 4.3.5 with a large table.

The shared memory (statistics, caches, the progress table) starts empty after a restart or a crash, and is rebuilt by use. What the reads of the write buffer and the tombstone log paid is kept in the metapage and survives a crash. A scope index of 5.0 kept what its reads paid for changed pairs there too; the sidecar of 5.1 keeps no such sum, and the maintenance makes it again by the share of changed runs ([COL-SCOPE.md](COL-SCOPE.md#what-it-costs)).

## 4. Locks and waits

**`sift.locks`** lists the locks of pg_sift held or waited for now, named:

| `kind` | `detail` | What it is |
|---|---|---|
| `maintenance` | `block 0` | the maintenance lock of an index: a promotion, a merge, a rewrite, `sift.check` (one segment at a time) |
| `active segment` | `block 2` | the writer of the active write buffer segment |
| `worker` | `block 3` | held by the maintenance worker of the index while it runs |
| `tombstones` | `block 4` | the tombstone log |
| `load` | `block 5` | a load whose segments are written and not yet in the directory shares it; the orphan sweep tries for it exclusively |
| `table vacuum` | | the worker that vacuums the table of a `sift_col` index (advisory lock, on the index) |
| `orphan sweep` | | the sweep of tracked leftovers of a database (advisory lock) |

Columns: `relation`, `kind`, `detail`, `mode`, `granted`, `waiting_since`, `pid`. A row with `granted` false and an old `waiting_since` is a session waiting behind the holder; join `pid` to `pg_stat_activity` for the holder's query.

**Wait events.** A backend that waits in pg_sift says so in `pg_stat_activity.wait_event` (type `Extension`):

| `wait_event` | Who waits |
|---|---|
| `SiftMaintenanceNap` | a maintenance worker between two looks at its index (normal, and it holds a worker slot) |
| `SiftArenaWait` | a writer waits for the rebuild of a slot of the shared arena of the write buffer references |
| `SiftParallelLeader` | the leader of a parallel aggregate scan waits for the rows of its workers |

The waits of a promotion or a merge on the maintenance lock show as `Lock` / `page` in `pg_stat_activity` and as rows of `sift.locks`.

## 5. Pooling

A pool that gives every request a backend of its own loads the library (`dlopen`, `_PG_init`) in that backend unless it is preloaded, and plans the first statement of the session twice for an aggregate workload. Preload it:

```
ALTER DATABASE mydb SET session_preload_libraries = 'pg_sift';     -- new sessions, no restart
-- or shared_preload_libraries = 'pg_sift' in postgresql.conf       -- a restart; the postmaster loads it once
```

`CREATE EXTENSION` and `ALTER EXTENSION UPDATE` print that command when neither setting names pg_sift, and `sift.upgrade_commands()` lists it (kind `preload`, not required). Without a preload `SHOW sift.enable_agg` and `EXPLAIN (SIFT)` fail in a fresh session (`unrecognized configuration parameter`, `unrecognized EXPLAIN option "sift"`) until a statement has loaded the library. Publishing a `sift_table` under logical replication needs `shared_preload_libraries` ([USAGE.md](USAGE.md#replication)).

`sift.stats()` returns the counters of the whole server, summed over its backends since it started, so a pooler that hands a session a different backend for each transaction still sees all of them; `sift.stats(local => true)` returns the session's own. The totals are a table of the shared memory (`sift.shared_memory()` lists it as `sift_stats`), start again with the server, and are not changed by `pg_stat_reset()`. A transaction pooler needs no session setting for correct answers; `SET sift.*` in a session is lost when the pool gives the connection to another client, so set a policy (`sift.enable_agg`, `sift.log_min_duration`) with `ALTER DATABASE` or `ALTER ROLE`.

A session that already holds one file of pg_sift and meets another (a package upgrade replaced the file under a pool that stayed connected) fails with `this session already holds pg_sift build .. and cannot load build ..`; reconnect it. After every library install restart the server or reconnect every pool.

## 6. Before and after an upgrade

`SELECT * FROM sift.upgrade_check()` (a superuser) reads the catalogs and metapages and changes nothing; one row for each finding with `status` `ok`, `warn` or `blocker` and the advice. Before the update, with the files of the new release installed and the extension still at the old version, `psql -f "$(pg_config --sharedir)/extension/pg_sift_upgrade_check.sql"` runs it. `sift.upgrade_check('4.3')` asks what a target release cannot read, for a way back or a standby that stays behind. `sift.upgrade_commands()` lists what is left to do after an update: one row for each command (`reindex`, `rewrite`, `reset`, `hooks`, `preload`) with the reason and whether it is required.

```
   object    |  kind   |           detected           | status | advice
-------------+---------+------------------------------+--------+--------
 pg_sift     | version | extension 5.0, library 5.0   | ok     |
 orders_col  | index   | format 1.2, column format 41 | ok     |
 orders_note | index   | format 1.4                   | ok     |
```

The steps, the order for standbys and the way back are in [UPGRADING.md](UPGRADING.md). `SELECT * FROM sift.build_info()` lists what the loaded library was built with (the version, PostgreSQL, the `lz4` and `zstd` it links, assertions): compare it across servers before moving a file-level backup, since a build without a codec cannot open chunks another server wrote with it.

### After a rollback

Install 4.3.6 on every node (standbys first) before moving to 5.0; `sift.upgrade_check()` warns while the extension is older. A 5.0 server keeps up to 704 bytes in the metapage of an index it has read or maintained (what the reads paid, the last background step, the floor of a scope), past the 400 that 3.3 to 4.3.5 know. An older library reading such an index answers by release:

| Library that reads it | What it answers | Is the index damaged | What to do |
|---|---|---|---|
| 3.0 to 3.2 | accepts the longer metapage and refuses a column format it does not know with the format error and the hint `Use a newer pg_sift, or REINDEX.` | no | `REINDEX` with the old files |
| 3.3 to 4.3.5 (as released) | `sift index "x" is corrupted`, detail `Bad metapage (magic 0x53494654, 704 bytes)`: the metapage is longer than the library knows | no: `sift.check()` and `amcheck` of the old node find nothing wrong | `REINDEX INDEX CONCURRENTLY <index>` with the old files rebuilds it from the table. Do not repair it with `pg_surgery` or by editing the catalogs, which would damage a sound index. These releases check the length when they open the metapage, before any change: a read or a `VACUUM` is refused and the index file is byte for byte as it was, so the counters of 5.0 are not lost by the attempt |
| 4.3.6 and later | keeps the bytes it does not know and reads the index, or refuses a segment in a format from 5.0 (a scope, strings in the shared store) with `index "x" has a segment of column format 1.45, this build reads 1.3 to 1.44`, hint `Use a newer pg_sift, or REINDEX.` | no | the same `REINDEX` with the old files |

A published `sift_table` (`wal_level = logical`) is the one object whose WAL a 4.x server cannot read: 5.0 writes its change records under the resource manager id 152, and 4.x stops at the first of them (`FATAL: resource manager with ID 152 not registered`, in recovery after a crash and on a standby). Stop the 5.0 server with a clean shutdown before the 4.x files go back, and rebuild a standby that still has such WAL to replay from a base backup of the 4.x primary. 5.0 itself replays the records 4.x wrote (registered also under the experimental id 128 as `sift_table_legacy` in `pg_walinspect`), so the way forward needs nothing.

An index created by 5.0 stores options under the names of 5.0. 4.2 and later know them; 3.3 to 4.1 call `rollup_columns` `rollup_dims`, so their `REINDEX` stops at `unrecognized parameter "rollup_columns"`: with the event triggers of 5.0 disabled ([UPGRADING.md](UPGRADING.md#going-back-to-43)), run `ALTER INDEX <index> RESET (rollup_columns)` first, or drop the index and create it again.

The steps back are in [UPGRADING.md](UPGRADING.md#going-back-to-43).

## 7. Disk

A merge or a rewrite writes its output before it frees its inputs, so plan the free space of the volume at about twice the index. `sift.col_space(index)` shows the blocks used, reusable and the point the file can be cut to (`shrink_to`); `sift.col_shrink_history(index)` says why the last cut left free blocks (`shrink_blocked_by`: `standby`, `lock` or `snapshot`, NULL when nothing blocks it) and when the file was last cut. The shared value stores (`<table>_<column>_sift_values`) grow with the strings they hold and are compacted by `VACUUM` or the maintenance worker when most of their codes are named by no segment ([COL-VSTORE.md](COL-VSTORE.md)); `SELECT * FROM sift.col_value_store_status` shows `garbage_pct`.
