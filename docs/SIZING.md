# Sizing and operating a large pg_sift index

Disk, memory and time an index costs beyond its steady-state size, and the settings that matter. *(changes)* marks 1.3.1 behavior that 2.0.0 altered (figures are the 1.3.1 baseline). Since 2.0.0 ([CHANGELOG.md](../CHANGELOG.md)):

- the memory of a merge no longer grows with the segments (4 segments of 3M documents, 690 MB, at 16MB of `maintenance_work_mem`: 140 MB of anonymous memory and 17.4 s before, 16 MB and 8.3 s after);
- the worker runs each step in a transaction of its own and reuses what the previous step freed (60 small segments merged by the worker: the file was 2.6 times the live segments, now 1.6);
- a merge stages only the dictionary index and the postings of large terms in temporary files.

## Disk

- **Merge and promotion need room beside the old segments.** A segment is written whole before the segments it replaces are freed, so a merge holds the inputs and the output (2x of the segments being merged), and blocks freed by earlier merges wait for old snapshots (next item). In churn tests the file reached 3.3x the live data, 6x under heavy churn: plan for about 3x the index as free space. `sift.merge(index, true)` forces a merge of every immutable segment. *(changes: a streaming merge writes the output while it reads and stages less.)*
- **Freed blocks are reused only after every transaction that could read them is gone.** Blocks released by a merge, a promotion or VACUUM carry the releasing xid and are reused once every snapshot that may hold the old directory has ended. An idle `REPEATABLE READ` or long transaction stops reuse: in a test the file grew 41 to 135 MB for 21 MB of live data while one transaction sat idle. The file shrinks only at its end, when `VACUUM` truncates the free blocks there (see [USAGE](USAGE.md#maintenance-and-operations)); free space in front of live segments waits for reuse and only `REINDEX` compacts it. The maintenance worker keeps one snapshot for its whole run, so a long run holds back its own frees. `sift.index_info(index)` shows `free_blocks` and how many are `reusable_blocks` now. *(changes: the worker releases its snapshot and recycles after each step.)*
- **A primary with a streaming standby: the index file, the WAL and the lag of a sustained write load.** Measured on a `sift_col` index over six columns, PostgreSQL 18.6, x86-64, one primary and one hot standby (standby `shared_buffers` of 64 MB, `fsync` on, `synchronous_commit` off), the primary on four cores shared with other work; the load is 4,000 transactions a second for 300 s (50% updates of a covered column, 20% of a key column, 10% deletes, 20% inserts), with autovacuum and the maintenance worker at their defaults. **Disk:** with the standby streaming and `hot_standby_feedback = on` the file ended at 4.4x and 6.4x its live size on 1,000,000 rows (78.2 and 115.6 MB against 17.6 and 18.1 MB for a fresh index; with and without a long query on the standby) and 2.3x on 3,000,000 rows (112.4 against 49.6 MB), and no `VACUUM` or merge shrank it in the 600 s after the load (10,515 of 12,618 blocks free and reusable in the 103 MB case); with feedback off 2.1x, the merge output written before its inputs are freed; without a standby 1.1 to 1.2x. Plan the disk of the primary for about 2x the live index, and for up to 6x for an index under about 32 MB (the first merge tier: its segments are folded into one at every eighth promotion, 13 to 16 MB every 75 to 80 s here) when the standby lags; the space is reused, and returned by `REINDEX` ([USAGE](USAGE.md#maintenance-and-operations)). **WAL:** 571 to 650 bytes a transaction at 1,000,000 rows with a standby (a table with three btree indexes: 577), 730 without one; 1,119 at 3,000,000 rows with the standby (1,590 without one, a longer run at a higher rate). **Replay lag:** the standby fell behind by 103 to 594 MB at 1,000,000 rows and 741 MB at 3,000,000 (the table with btree indexes: 488 MB), whether it shared cores or had its own, because its replay of random page writes is the limit at this rate, not the index; an earlier run at 1,500 and 4,000 transactions a second (`fsync` off) held 15.5 MB (8.8 s) and 29.3 MB (11.4 s).
- **Temporary files.** A merge reads its input segments into temporary files first (117 MB of segments wrote 224 MB, about 1.9x), and `CREATE INDEX` writes its sorted runs there too: `temp_file_limit` and the free space of the `temp_tablespaces` must cover them. A build of the test corpus wrote 206 MB with `maintenance_work_mem` of 64MB, 1127 MB with 1MB (and left 11 segments instead of 1). *(changes: the streaming merge stages nothing.)*

## Memory

- **Maintenance work is not bounded by `maintenance_work_mem`.** Promotion, merge and a VACUUM segment rewrite keep about 50 bytes per document in memory besides the budget: rewriting a segment of 2.88M documents peaked at 150 MB of resident memory; 500M documents would need about 25 GB, in the background worker or an autovacuum worker. *(changes: the streaming merge bounds it.)*
- **`CREATE INDEX`** gives every participant (the leader and each parallel worker, `max_parallel_maintenance_workers`) its own `maintenance_work_mem`. Any value from 1MB works; a smaller one is slower (up to 1.5x at 1MB) and leaves more, smaller segments for `sift.merge` to join. Many segments slow queries (a ranked AND took 4.0 ms on 11 segments, 1.8 ms merged), so run `SELECT sift.merge('idx')` or `VACUUM` after a low-memory build. At default settings a build writes one segment.
- **Wide prefixes** (`ab*` over a large vocabulary) gather their postings in the backend: `sift.prefix_cache_size` (default 8MB) caps the
  cache, `sift.max_expansions` the terms of a wildcard, regex or fuzzy leaf. Both are per-session settings.
- A ranked scan keeps `LIMIT + OFFSET` hits in memory (16 bytes each); very deep pages (`OFFSET` in the millions) cost that.

## Dead documents and statistics

Rolled-back and deleted rows stay in the index's document count, document frequencies and average length until VACUUM marks them dead. From then on a segment of format 2 or later leaves them out of all three, so a dirty index scores exactly as the merged one does (`test/sql/live_stats.sql`). A format 1 segment (2.x) keeps counting them until it is rewritten (a merge, or a VACUUM that finds it more than `dead_rewrite_pct` (50) percent dead); no REINDEX is needed. Effect of counting them (`bench/sql/dead_docs.sql`, 50,000 live documents, four rolled-back loads of 50,000):

| dead documents | top-score change | top 10 of 5 queries |
|---|---|---|
| like the live ones | -1.3% to +0.4% | unchanged |
| all about one term | -24% to +1.7% | 3 of 10 kept for `w40 OR w41 OR w7`, the others unchanged |
| after `VACUUM` | exactly the original (2.x: -5.7% to +0.3%) | unchanged |
| after `VACUUM`, `sift.promote` and `sift.merge(index, true)` | exactly the original | unchanged |

`sift.segments(index)` shows `dead_docs` of immutable segments (marked by VACUUM); rolled-back rows not yet vacuumed appear only as the difference between `sum(ndocs)` and the table's live rows. Autovacuum counts aborted inserts as dead tuples, so a table whose loads often fail should have a low `autovacuum_vacuum_scale_factor` (or a manual `VACUUM` after the failed load).

Vacuumed documents also cost query time until their segment is rewritten, because a query that walks postings drops the dead ones through the liveness rows of every page it reads. Cost of the dirty segment relative to a clean one (the same live rows rebuilt by `REINDEX`), in user instructions per query (`bench/dirty-query.sh` data, 200,000 documents, 10 to 45% deleted and vacuumed; not 1.3.1 code: the 2.0.0 and 2.0.1 builds and 3.0, format 2 segments):

| query | 2.0.0 | 2.0.1 | 3.0 |
|---|---|---|---|
| `count(*)` of a term | 18.5 to 18.6x | 6.6 to 6.7x | 1.16 to 1.28x |
| `count(*)` of two terms | 3.5 to 4.4x | 1.7 to 2.3x | 1.0 to 1.25x |
| `count(*)` of three with OR | 4.5 to 6.2x | 1.5 to 2.1x | 1.0 to 1.2x |
| ranked AND, top 10 |  |  | 1.0 to 1.3x |
| ranked OR of three terms, top 10 |  |  | 1.3 to 1.8x |
| a rare term, top 10 |  |  | 1.3 to 2.0x |

A term count is the sum of its live document frequencies (each segment stores the vacuumed postings of every term) corrected for the pages the heap decides, so it does not depend on how much of the segment is dead. Queries that read postings still read the dead ones before dropping them: up to twice the clean cost at 45% dead. Not-yet-vacuumed rolled-back rows are not in the liveness rows and cost one heap check each, as many as the dead rows among the candidates (`bench/dirty-query.sh -m rollback`).

## Updates

A `HOT` update does not touch the index; any other update inserts the row's text into the index again, even when the indexed
column did not change: rows are found by TID and the new row version has a new one, so PostgreSQL's `indexUnchanged` hint
cannot let the insert be skipped. A table with large indexed texts (100 kB) and frequently changing other columns should leave room
for HOT updates: `ALTER TABLE t SET (fillfactor = 80)` (applies to new pages) and keep other indexes off the updated columns.

## A `sift_col` index at 30 million rows

One run each on 2026-10-23, PostgreSQL 18.6, `shared_buffers = 8GB`, autovacuum off,
a table of 29,526,504 rows in arrival order (28 GB of heap, 13 columns, 22 leaves with five JSON paths). Sizes and WAL are exact; times are indicative. Nothing here is measured above 30 million rows.

| build | workers | `maintenance_work_mem` | seconds | WAL | segments | private memory, all processes |
|---|---|---|---|---|---|---|
| `CREATE INDEX` | 0 | 1GB | 65.6 | 746 MB | 1 | 150 MB |
| `CREATE INDEX` | 2 | 1GB | 31.1 | 750 MB | 3 | 160 MB |
| `CREATE INDEX` | 8 | 2GB | 17.0 | 750 MB | 9 | 186 MB |
| `CREATE INDEX` | 16 | 4GB | 16.9 | 750 MB | 9 | 188 MB |
| `CREATE INDEX CONCURRENTLY` | 8 | 2GB | 26.1 | 750 MB | 9 | 766 MB |
| `REINDEX` | 8 | 2GB | 17.2 | 750 MB | 9 | 188 MB |
| `REINDEX CONCURRENTLY` | 8 | 2GB | 25.5 | 750 MB | 9 | 766 MB |

- **Size and WAL:** 743 MB of segments (25 bytes a row, 2.6% of the heap); the build writes that much WAL plus 0.5% (pages are logged once). The build is linear in the rows (2.2 us a row serial); no worker count above 8 helped on this host. PostgreSQL grants one participant per 32 MB of `maintenance_work_mem`, so 16 workers asked with less than 544 MB run with fewer, without a message. `pg_stat_progress_create_index` shows `blocks_done` rising to `blocks_total` (3,728,640 here) during a parallel scan.
- **Concurrent build:** 1.5x the time and about 600 MB more memory (the validation pass sorts the row identifiers of the whole table); it also waits for every older transaction, as any `CREATE INDEX CONCURRENTLY` does.
- **Parallel build:** one segment per participant (3 with 2 workers, 9 with 8, also 9 when 16 were asked for). The first `VACUUM` after it rewrites them into one: 31.5 s and 747 MB of WAL, and the file doubles (1.42 GB for 0.71 GB of live segments; the next `VACUUM` changed nothing) because the free space is in front of the live segment; `REINDEX` compacts it, and while a physical standby streams (VACUUM does not cut the file then) `CREATE INDEX` and `REINDEX` do that rewrite and the cut themselves, in the file no query of the standby can hold before the commit: 3,000,000 rows, 13 columns, 8 workers, the file 212 MB with 108 MB live after the first VACUUM, 115 MB with 115 MB live now; the build takes 40.0 against 21.0 CPU seconds and writes 325 against 104 MB of WAL, `REINDEX CONCURRENTLY` keeps the old behavior. `sift.col_merge(index, true)` on the same index took 31.0 s and 747 MB of WAL.
- **A rewrite costs the whole index, not the change.** Six daily cycles of 29,526 inserts, 14,763 scattered updates and 29,526 updates of one burst each: `VACUUM` took 0.5 to 3 s and `sift.col_maintain` 5 ms until the write buffers and dirty units crossed the threshold, then rewrote everything (31 and 33 s, 1.0 and 1.2 GB of WAL with the table's own), once every 2 to 3 days of such changes. The file stayed at 2.1x the live segments (1.52 GB for 0.72 GB). Plan the free space of a rewrite (about 2x the index) and its WAL. `sift.col_index_status` shows `index_bytes`, `free_bytes`, `dirty_segments`, `dead_pct` and why a rewrite is `due` in 13 ms (`sift.col_leaves` takes 0.4 s, the other views 4 to 36 ms at 14,565 units).
- **Hot standby:** same answers, same plan, a faster scan than the primary (the aggregate over 29.8M rows: 134 to 138 ms against 163 to 166 ms). It replays a 100,000-row insert 0.2 s after the commit, a rewrite of 765 MB of WAL in 0.8 s and a `REINDEX` of 769 MB in 1.0 s (700 to 900 MB/s); a base backup of the 33 GB database took 47 s. Long queries need `hot_standby_feedback`, as above.
- **Crash recovery** is the redo of generic WAL and nothing else: after a rewrite and a `REINDEX` (1,463 MB of WAL since the checkpoint) the server accepted connections 3.3 s after `pg_ctl stop -m immediate` (a b-tree `REINDEX` and insert, 463 MB: 1.3 s), with the same answers.
- **`sift.col_verify(index, true)`** holds a 40-byte hash for every live row in one array that doubles: 29.7M rows took 70 s and 2.3 GB of private memory, about 80 bytes of memory a row. Earlier builds failed with `invalid memory alloc request size` above 16.7 million rows.
- **`pg_dump` writes only `CREATE INDEX`**: a restore builds the index again, at the times above.

## Settings that matter

| Setting | Effect |
|---|---|
| `maintenance_work_mem`, `max_parallel_maintenance_workers` | `CREATE INDEX` speed and segment count (above) |
| `temp_file_limit`, `temp_tablespaces` | `CREATE INDEX` runs and merge staging (above) |
| `autovacuum` and its scale factors | marks dead documents, promotes and merges; the index does not shrink without it |
| `hot_standby_feedback = on` on replicas | merges free blocks a replica query may still read; without feedback the replay waits for such a query up to `max_standby_streaming_delay` and then cancels it (`sift.standby_conflicts`, on), as for a heap query; with that setting off it fails at once with a retryable "segment recycled during scan" (40001) |
| `max_worker_processes` | the maintenance worker needs a slot; when none is free the log says so and the inserting session promotes and merges itself |
| `sift.log_min_duration` | logs promotions, merges and worker runs that take at least that long (default 1s, -1 off) |
| `sift.async_maintenance` | off: only VACUUM and the inserting sessions promote and merge |

## Features and their costs

- **Materialized views** can carry a sift index and get ranked scans and `count(*)` pushdown like tables.
- **`SERIALIZABLE`**: a SiftScan takes a predicate lock on the whole heap relation, and a bitmap scan on the whole index (PostgreSQL does this for every index access method without predicate locks), so a concurrent write to the table can make a serializable transaction fail with `40001` where a btree-indexed query would not. Retry on `40001`. Finer locks would need one per term searched (a new document conflicts only if it has that term) and one for the vocabulary range of every wildcard, prefix or regex; a lock per page does not work because a document is appended to the write buffer, not to the page where its terms are read.
- **Row-level security and security barrier views.** A role under a policy (or reading a security barrier view) gets no ranked scan, no `count(*)` pushdown and no index estimate: the match is not leakproof and the index statistics (document count, document frequencies, expansion limits) include rows the role cannot see, so a score or `EXPLAIN` estimate would depend on hidden rows (before 2.0.1 a score of 4.99 for a term of 20 visible documents was `ln(1 + (3020 - 20 + 0.5) / (20 + 0.5))`, computed with the 3,000 rows a policy hid). Its matches come from a sequential scan, slower on a large table, and `sift.score()` raises an error. The table owner, who bypasses the policy, keeps the scan. With `sift.rls_scores = on` (superuser) the role gets the ranked scan, and a score then does depend on the hidden rows ([USAGE.md](USAGE.md#row-level-security)).
- **`ALTER INDEX ... SET/RESET`** of `k1`, `b` or an analyzer option (`stemmer`, `fields`, `analyzer`, `prefix_index`, folding) needs a `REINDEX`. Until then queries and inserts on the table fail ("built with other analyzer options") and the statement warns. To change them without downtime, build the new index with `CREATE INDEX CONCURRENTLY` and drop the old one.
- **`ANY (array)`**: ``col @@> ANY (ARRAY['a', 'b'])`` is the OR of the elements, scanned and scored like ``col @@> 'a OR b'`` (`sift.score()`, `ORDER BY`, `LIMIT` and `count(*)` work). A NULL element is skipped; a NULL or empty array matches no rows. A list with no score or order that is estimated above 100,000 rows (after a `LIMIT`) is left to the bitmap scan, as fast or faster there on a 1,000,000-row table.
- **`FOR UPDATE`, `UPDATE`, `DELETE`, `MERGE`**: matching works through a bitmap scan; `sift.score()` read straight from a table those statements join is an error; a subquery or `WITH` query that reads it is planned apart (rank there and change the rows by key).
- **Indexes beyond 2^31 blocks (16 TB).** The orphan sweep and `sift.verify` collect block numbers in PostgreSQL `Bitmapset`s, which stop at 2^31 members: a larger index is untested and unsupported (the format itself allows 32 TB).
