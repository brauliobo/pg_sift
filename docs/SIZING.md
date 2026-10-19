# Sizing and operating a large pg_sift index

What an index costs in disk, memory and time beyond its steady-state size, and which settings matter. Numbers are measured on 1.3.1 code
(PostgreSQL 18, a shared host) unless a sentence says otherwise; they show the size of an effect, not a guarantee. Statements marked
*(changes)* describe behavior that the maintenance, build and read changes in progress alter; this page is updated as they land.

## Disk

- **Merge and promotion need room beside the old segments.** A segment is written whole before the segments it replaces are freed, so
  while a merge runs the index holds the inputs and the output (2x of the segments being merged), and blocks freed by earlier merges wait
  for old snapshots (next item): in the review's churn tests the file reached 3.3x the live data, 6x under heavy churn. Plan for about 3x
  the index as free space; a forced `sift.merge(index, true)` merges every immutable segment. *(changes: a streaming merge writes the
  output while it reads and stages less.)*
- **Freed blocks are reused only after the transactions that could still read them are gone.** Blocks released by a merge, a promotion or
  VACUUM carry the releasing transaction's xid and are taken again once every snapshot that may hold the old directory has ended. An idle
  `REPEATABLE READ` or long transaction therefore stops reuse: in a test the file grew 41 to 135 MB for 21 MB of live data while one
  transaction sat idle, and the file shrinks only at its end, when `VACUUM` truncates the free blocks there (see [USAGE](USAGE.md#maintenance-and-operations)); free
  space in front of live segments waits for reuse and only `REINDEX` compacts it. The maintenance worker also keeps one snapshot for its whole run,
  so a long run holds back its own frees. `sift.index_info(index)` shows `free_blocks` and how many of them are `reusable_blocks` now.
  *(changes: the worker releases its snapshot and recycles after each step.)*
- **Temporary files.** A merge reads its input segments into temporary files first: a merge of 117 MB of segments wrote 224 MB of
  temporary files (about 1.9x), and `CREATE INDEX` writes its sorted runs there too, so `temp_file_limit` and the free space of the
  `temp_tablespaces` must cover them. With `maintenance_work_mem` of 64MB a build of the test corpus wrote 206 MB, with 1MB 1127 MB (and
  left 11 segments instead of 1). *(changes: the streaming merge stages nothing.)*
- **`positions = on`** (the default) makes the index about 2.6x larger than `positions = off` ([USAGE.md](USAGE.md#limits)).

## Memory

- **Maintenance work is not bounded by `maintenance_work_mem`.** Promotion, merge and the rewrite of a segment by VACUUM keep about 50
  bytes per document in memory besides the budget: rewriting a segment of 2.88M documents peaked at 150 MB of resident memory. A segment
  of 500M documents would need about 25 GB, in the background worker or in an autovacuum worker. *(changes: the streaming merge bounds it.)*
- **`CREATE INDEX`** gives every participant (the leader and each parallel worker, `max_parallel_maintenance_workers`) its own
  `maintenance_work_mem`. Any value from 1MB works; a smaller one only takes longer (up to 1.5x at 1MB) and leaves more, smaller
  segments, which `sift.merge` joins afterwards. Queries are slower while there are many segments (a ranked AND took 4.0 ms on 11
  segments and 1.8 ms merged): run `SELECT sift.merge('idx')` or `VACUUM` after a low-memory build. At the default settings a build
  writes one segment.
- **Wide prefixes** (`ab*` over a large vocabulary) gather their postings in the backend: `sift.prefix_cache_size` (default 8MB) caps the
  cache, `sift.max_expansions` the terms of a wildcard, regex or fuzzy leaf. Both are per session settings.
- A ranked scan keeps `LIMIT + OFFSET` hits in memory (16 bytes each); very deep pages (`OFFSET` in the millions) cost that.

## Dead documents and statistics

Rows of rolled-back transactions and deleted rows stay in the index's document count, document frequencies and average length until
VACUUM has marked them dead. From then on a segment written by this version (format 2) leaves them out of all three, so a dirty index
scores exactly as the merged one does (`test/sql/live_stats.sql`). A segment written by 2.x (format 1) keeps counting them until it is
rewritten (a merge, or a VACUUM that finds it more than `dead_rewrite_pct` (50) percent dead); no REINDEX is needed. The effect of
counting them depends on how the dead rows differ from the live ones (`bench/sql/dead_docs.sql`, 50,000 live documents, four
rolled-back loads of 50,000):

| dead documents | top-score change | top 10 of 5 queries |
|---|---|---|
| like the live ones | -1.3% to +0.4% | unchanged |
| all about one term | -24% to +1.7% | 3 of 10 kept for `w40 OR w41 OR w7`, the others unchanged |
| after `VACUUM` | exactly the original (2.x: -5.7% to +0.3%) | unchanged |
| after `VACUUM`, `sift.promote` and `sift.merge(index, true)` | exactly the original | unchanged |

`sift.segments(index)` shows `dead_docs` of immutable segments (marked by VACUUM); rolled-back rows not yet vacuumed appear only as the
difference between `sum(ndocs)` and the table's live rows. Autovacuum counts aborted inserts as dead tuples, so a table whose loads fail
often should have a low `autovacuum_vacuum_scale_factor` (or a manual `VACUUM` after the failed load).

Vacuumed documents also cost query time until their segment is rewritten: a query that walks postings drops the dead ones through the
liveness rows of every page it reads. Measured in user instructions per query (`bench/dirty-query.sh` data, 200,000 documents, 10 to 45%
deleted and vacuumed; "clean" is the same live rows rebuilt by `REINDEX`), the dirty segment costs:

| query | 2.0.0 | 2.0.1 | now |
|---|---|---|---|
| `count(*)` of a term | 18.5 to 18.6x | 6.6 to 6.7x | 1.16 to 1.28x |
| `count(*)` of two terms | 3.5 to 4.4x | 1.7 to 2.3x | 1.0 to 1.25x |
| `count(*)` of three with OR | 4.5 to 6.2x | 1.5 to 2.1x | 1.0 to 1.2x |
| ranked AND, top 10 | | | 1.0 to 1.3x |
| ranked OR of three terms, top 10 | | | 1.3 to 1.8x |
| a rare term, top 10 | | | 1.3 to 2.0x |

The count of a term is the sum of its live document frequencies (each segment stores the vacuumed postings of every term) corrected for
the pages the heap decides, so it does not depend on how much of the segment is dead. Queries that read postings still read the dead
ones before dropping them, up to twice the clean cost at 45% dead. Dead rows of transactions that were rolled back (not yet vacuumed)
are not in the liveness rows: they cost a heap check each, as many as the dead rows among the candidates (`bench/dirty-query.sh -m rollback`).

## Updates

A `HOT` update does not touch the index; any other update of a row inserts the row's text into the index again, even when the indexed
column did not change: the index finds rows by their TID and the new row version has a new one, so PostgreSQL's `indexUnchanged` hint
cannot let the insert be skipped. A table whose indexed texts are large (100 kB) and whose other columns change often should leave room
for HOT updates: `ALTER TABLE t SET (fillfactor = 80)` (applies to new pages) and keep other indexes off the updated columns.

## Settings that matter

| Setting | Effect |
|---|---|
| `maintenance_work_mem`, `max_parallel_maintenance_workers` | `CREATE INDEX` speed and segment count (above) |
| `temp_file_limit`, `temp_tablespaces` | `CREATE INDEX` runs and merge staging (above) |
| `autovacuum` and its scale factors | marks dead documents, promotes and merges; the index does not shrink without it |
| `hot_standby_feedback = on` on replicas | merges free blocks a replica query may still read; without feedback such a query fails with a retryable "segment recycled during scan" (40001) |
| `max_worker_processes` | the maintenance worker needs a slot; when none is free the log says so and the inserting session promotes and merges itself |
| `sift.log_min_duration` | logs promotions, merges and worker runs that take at least that long (default 1s, -1 off) |
| `sift.async_maintenance` | off: only VACUUM and the inserting sessions promote and merge |

## Features and their costs

- **Materialized views** can carry a sift index and get ranked scans and `count(*)` pushdown like tables.
- **`SERIALIZABLE`**: a SiftScan takes a predicate lock on the whole heap relation, and a bitmap scan on the whole index (PostgreSQL does
  this for every index access method that does not implement predicate locks), so a concurrent write to the table can make a
  serializable transaction fail with `40001` that a btree-indexed query would not. Finer locks would need one per term searched (a new
  document conflicts only if it has that term) and one for the vocabulary range of every wildcard, prefix or regex; a lock per page
  does not work because a document is appended to the write buffer, not to the page where its terms are read. Retry on `40001`.
- **Row-level security and security barrier views.** A role under a policy (or reading a security barrier view) gets no ranked scan, no
  `count(*)` pushdown and no estimate from the index: the match is not leakproof and the index statistics (document count, document
  frequencies, expansion limits) include rows the role cannot see, so a score and an `EXPLAIN` estimate would depend on hidden rows (before
  2.0.1 a score of 4.99 for a term of 20 visible documents was `ln(1 + (3020 - 20 + 0.5) / (20 + 0.5))`, computed with the 3,000 rows a policy hid).
  Its matches come from a sequential scan, which is slower on a large table, and `sift.score()` raises an error. The table owner, who
  bypasses the policy, keeps the scan.
- **`ALTER INDEX ... SET/RESET`** of `k1`, `b` or an analyzer option (`stemmer`, `fields`, `analyzer`, `prefix_index`, folding)
  needs a `REINDEX`. Until then queries and inserts on the table fail ("built with other analyzer options"), and the statement warns about
  it. To change them without downtime build the new index with `CREATE INDEX CONCURRENTLY` and drop the old one.
- **`ANY (array)`**: ``col @@> ANY (ARRAY['a', 'b'])`` is an OR of the elements through a bitmap scan; it has no ranked scan, so
  `sift.score()` cannot be used with it (write the OR in the query text: `col @@> 'a OR b'`).
- **`FOR UPDATE`, `UPDATE`, `DELETE`, `MERGE`**: matching works through a bitmap scan; `sift.score()` in those statements is an error
  (rank in a `SELECT` and change the rows by key).
- **Indexes beyond 2^31 blocks (16 TB).** The orphan sweep and `sift.verify` collect block numbers in PostgreSQL `Bitmapset`s, which stop at
  2^31 members: an index larger than that is untested and unsupported (the format itself allows 32 TB).
