# The bitmap scan of a sift_col index, and replacing a btree with one

A `sift_col` index built with the operator classes `sift.col_*_ops` is a **bitmap index scan** for the planner: `WHERE id = 5`, `IN` lists,
ranges, `IS [NOT] NULL`, `LIKE 'abc%'` on integer, date, timestamp, boolean, float, numeric(p <= 18, s), char(n), text and uuid columns return rows through
`Bitmap Heap Scan` or `Index Scan` (and `Index Only Scan` for the fixed-width columns), for any subset of the columns of a multicolumn index, combined
with `BitmapAnd` and `BitmapOr`. Without them (the default class `sift.col_ops`) the index serves only the
column scans of SiftAgg (aggregates, `GROUP BY`, `DISTINCT`, `ORDER BY .. LIMIT`, [COL-PLAN.md](COL-PLAN.md)) and a query that returns rows
reads the heap. Nothing stored changes: the classes are catalog entries over the same index, so an existing index keeps its class and its bytes.

```sql
CREATE INDEX ON items USING sift_col (id sift.col_int4_ops, kind sift.col_text_ops, ts sift.col_timestamptz_ops);
SET sift.enable_col_scan = on;      -- off by default
SELECT * FROM items WHERE kind = 'cat_a' AND ts >= '2024-03-01' AND ts < '2024-03-02';   -- Bitmap Heap Scan, no leading column needed
```

| class | type | operators |
|---|---|---|
| `col_int2_ops`, `col_int4_ops`, `col_int8_ops` (one family: cross-type comparisons are in it) | smallint, integer, bigint | `<` `<=` `=` `>=` `>` |
| `col_date_ops`, `col_timestamp_ops`, `col_timestamptz_ops` | date, timestamp, timestamptz | `<` `<=` `=` `>=` `>` (same type) |
| `col_bool_ops`, `col_numeric_ops` (numeric(p <= 18, s) only) | boolean, numeric | `<` `<=` `=` `>=` `>` (same type) |
| `col_float4_ops`, `col_float8_ops` (one family) | real, double precision | `<` `<=` `=` `>=` `>`, NaN above everything as in SQL |
| `col_text_ops` | text, varchar (by binary coercion) | `=`, `LIKE` and `^@` with a literal prefix |
| `col_bpchar_ops` (char(n) only), `col_uuid_ops` | char(n), uuid | `=` only |

`= ANY (array)` is one scan (a set test per unit); `x < ANY (array)` is the test against the largest element. `IS NULL`/`IS NOT NULL` are index
quals. A column of a multicolumn index may carry another class (or the default one: the scan then ignores it).

## How it works

`ambeginscan`/`amgetbitmap` (`src/col/amscan.c`) turn the scan keys into the predicates of the column source (`sift_col_begin`: the zone maps skip
the units that cannot hold a match), filter the chunks of the units that remain, and give the TIDs of the rows that pass to the TID bitmap. The
heap scan applies MVCC and does the recheck of the lossy pages, as for any bitmap scan. The write buffer is read like any unit, and a row the
index holds for a dead tuple is dropped by the heap. The stored values are the heap's (an update that keeps the indexed columns is a HOT update and
keeps the entry; any other update adds a new entry), so the scan sets no recheck, except for a key the leaf cannot test exactly: any key on a numeric column (a
NaN is stored as a NULL with the overflow flag, a candidate of every test), a NaN constant, a numeric constant too large to scale and a `LIKE` pattern with a
wildcard after its prefix are replaced by `IS NOT NULL` or a prefix test and the heap scan applies the clause. Text is compared bytewise: `CREATE INDEX` refuses
`col_text_ops` on a column of a nondeterministic collation, and a query's collation must equal the index column's for the planner to use the index.

**Cost** (`sift_col_amcostestimate`): the share of the units whose zones can hold the constants of the quals, counted on the real zones of a sample of the
units (`sift_col_zones`, the sample SiftAgg's planner uses), times the rows and columns they decode, plus a fixed price per unit; quals the zones cannot
judge (text, uuid, a parameter) count every unit, and an equality or `IN` on a column named by `set_columns` keeps the share of the units
that the small-set summaries leave, as SiftAgg counts it (`1 - exp(-s * n)` for a test on the share `s` of a column's values and `n` keys in a unit). The planner compares it with SiftAgg and the heap: 12 of 96 (query, size) picks in the table below were
more than 1.3 times slower than the best of the other plans, none by more than 0.5 ms (the worst: a `count(*)` with an `IN` list of 100 values at 25,000
rows, SiftAgg 0.27 ms against 0.74 ms). The one larger loss measured is `count(*) WHERE rnd IS NULL` on 1,000,000 rows with no NULL, where the
planner keeps SiftAgg (2.45 ms) and the bitmap scan takes 0.06 ms (the zones hold no NULL). A range of rows at 0.1% to 80% of the table picks the
bitmap scan up to 40% and the heap scan at 80%, where the plans are within 10% of each other.

**Setting** `sift.enable_col_scan` (default off): off, the planner builds no bitmap path on a `sift_col` index (it also holds when `enable_seqscan` is
off). The index stays in the relation's index list, SiftAgg reads it as before.

**Tuple by tuple** (`amgettuple`, `amcanreturn`, also under `sift.enable_col_scan`): the index offers an `Index Scan` and an `Index Only Scan` in the order of the
units (it has no order to give: `amcanorder` is off). A plan that stops early (`LIMIT 10` without `ORDER BY`), reads a few rows or runs on the inner side of a nested
loop does not build a bitmap, and a query that needs only integer, date, timestamp, boolean, float and numeric columns of the index does not read the heap (the
visibility map decides, as for a btree: `Heap Fetches` in `EXPLAIN`). Text, char(n) and uuid are not returnable: returning one loads the dictionary of the
segment (a million strings, 10 to 30 ms), and the executor asks for every returnable column whether the query needs it or not. `pg_stat_user_indexes`
counts the scans and `EXPLAIN` shows `Index Searches`.

**Limits** (measured below or by construction): no ordered scan (`amcanorder`, backward scans, `CLUSTER USING`) so `ORDER BY .. LIMIT` stays with the top-N of
SiftAgg and an `ORDER BY` of all rows sorts; no `UNIQUE`, `PRIMARY KEY`, `EXCLUDE` or foreign key target (the AM has no `amcanunique`, and the design of
one is in the study the research notes keep); no `INCLUDE`, `ASC`/`DESC`, `text_pattern_ops`; `LIKE` is indexed as a literal prefix (`starts_with(col, 'a')`
is not rewritten by the planner: write `col ^@ 'a'`); no classes for money, interval, time, enums, arrays, ranges, wide numeric or jsonb paths; no parallel
index scan (a parallel bitmap heap scan works). `CREATE INDEX CONCURRENTLY`, `REINDEX CONCURRENTLY`, partial and expression indexes, a nested loop with the
index on the inner side, and the referential check of a foreign key on the referencing column use it. `pg_dump` writes the class.

## Replacing a btree

threads) shared with other jobs, `shared_buffers` 256 MB, `jit` off, no parallel workers, tables `items_N` of the neutral
byte payload; about 460 bytes a row), `EXPLAIN (ANALYZE)` execution time, median of 3 after a warm run, one backend. The btrees are one per query
column (`id`, `rnd INCLUDE (id)`, `(grp, ts)`, `s_short`, `u`); the `sift_col` index is one index over the same columns. Planner pick unless
noted.

**Space and build** (index bytes per row, sift_col / btree; build time median of 3, serial, `maintenance_work_mem` default):

| index on | 200,000 rows: sift_col vs btree | 1,000,000 rows: sift_col vs btree | build, 1,000,000 rows |
|---|---|---|---|
| int ascending | 1.43 vs 22.6 B/row (0.06x) | 0.68 vs 22.5 (0.03x) | 128 ms vs 150 |
| int random | 4.06 vs 22.4 (0.18x) | 3.64 vs 22.3 (0.16x) | 111 ms vs 234 |
| timestamptz random | 7.33 vs 22.6 (0.32x) | 6.67 vs 22.5 (0.30x) | 140 ms vs 323 |
| int (100 values) + int random + timestamptz | 4.79 vs 31.6 (0.15x) | 4.81 vs 31.5 (0.15x) | 138 ms vs 407 |
| text, 8 distinct characters | 5.37 vs 31.7 (0.17x) | 5.38 vs 31.6 (0.17x) | 272 ms vs 347 |
| uuid | 17.8 vs 31.7 (0.56x) | 16.9 vs 31.6 (0.54x) | 227 ms vs 269 |
| text, 240 distinct characters | 45.5 vs 331 (0.14x) |  |  |
| text, 20 values / int, 100 values | 1.43 vs 7.05 / 2.09 vs 7.25 (0.2x / 0.29x) |  |  |

One `sift_col` index over six columns (`id`, `rnd`, `grp`, `ts`, `s_short`, `u`) of the 1,000,000-row table is 25.9 MB, the five btrees on them (`id`, `rnd`, `(grp,
ts)`, `s_short`, `u`) 139.4 MB (0.19x). The smallest index file is 280 kB (an ascending int column is that size from 25,000 to 200,000 rows). Churn at 100,000 rows (20,000 inserted, 10% updated in an indexed column, 10% updated elsewhere, 10% deleted, then `VACUUM`,
then `REINDEX`): the index of a random int column is 440 kB built, 944 kB dirty and after `VACUUM`, 536 kB reindexed (btree 2,184, 4,360 and 2,272 kB):
both grow to about 2x and `VACUUM` gives nothing back, so the absolute bloat is 4.6 times smaller. WAL of the same statements over a table without the
index, kB: 20,000 inserted rows 1,580 (btree 4,247), 10,000 rows updated in the indexed column 955 (3,074), 10,000 rows updated in a column no index
holds 114 (2,461); the statements ran 1% to 33% longer with `sift_col`, 39% to 76% longer with the btree. A checkpoint before each statement makes
the first touch of each page a full page image in both.

**Verdicts**, execution time in ms at 100,000 and 1,000,000 rows (btree / sift_col with `sift.enable_col_scan` on; `sift_col` alone is the third
figure where the scan is not used and the planner reads the heap or SiftAgg):

| btree workload | btree | sift_col | verdict |
|---|---|---|---|
| `WHERE id = c`, rows, ascending key | 0.04 / 0.05 | 0.10 / 0.11 (heap scan 10.0 / 78.8) | replaces |
| `WHERE id = c`, `count(*)` | 0.04 / 0.07 | 0.11 / 0.10 | replaces |
| `WHERE rnd = c`, rows, random int | 0.03 / 0.05 | 0.26 / 1.99 | replaces up to about 100,000 rows; at 1,000,000 rows 40 times slower (every unit is read: zones do not prune a random key) |
| `WHERE s_short = c`, `WHERE u = c` (distinct text, uuid) | 0.06 / 0.05, 0.04 / 0.03 | 4.45 / 41.7, 4.18 / 64.4 | cannot replace for point lookups: linear in the rows, 70 to 2,000 times slower; 2 to 4 times faster than the heap scan |
| `id IN (10 values)`, `IN (100 values)`, rows | 0.04 / 0.07, 0.48 / 0.61 | 0.15 / 0.43, 1.58 / 5.41 | replaces with caveat (3 to 9 times slower, under 6 ms at 1,000,000 rows) |
| narrow range (0.1%), ascending key, rows | 0.07 / 0.35 | 0.11 / 0.24 | replaces |
| range of 20%, rows | 2.54 / 29.7 | 3.39 / 24.3 | replaces |
| range of 20%, `count(*)` | 1.66 / 10.5 | 0.25 / 0.86 | replaces, 7 to 12 times faster (SiftAgg) |
| narrow range of a random key (0.1%), rows | 0.24 / 2.62 | 0.80 / 5.90 | replaces with caveat (2 to 3 times slower) |
| equality on a column and a range on another, rows | 0.20 / 1.71 | 0.35 / 1.59 | replaces |
| equality on the leading column only (1%), rows | 1.51 / 15.9 | 2.58 / 15.9 | replaces |
| range on the second column of `(grp, ts)` only (skip scan) | 0.23 / 0.48 | 0.15 / 0.23 | replaces; any subset of the columns is usable |
| rows at 0.1% .. 80% of the table, clustered / random key (100,000 rows) | 0.05 .. 10.5 / 0.24 .. 12.4 | 0.08 .. 11.0 / 0.67 .. 10.0 | replaces: 0.9x to 1.6x of the btree time on the clustered key, 0.8x to 2.8x on the random one (the slowest at 0.1%) |
| `ORDER BY rnd LIMIT 10`, `LIMIT 1000` | 0.03 / 0.04, 0.15 / 0.30 | 0.31 / 0.52, 3.82 / 8.37 (SiftAgg top-N) | replaces with caveat: 10 to 28 times slower, under 10 ms |
| `WHERE grp = c ORDER BY ts LIMIT 10` | 0.04 / 0.04 | 0.28 / 0.23 | replaces with caveat (6 times slower, 0.3 ms) |
| `ORDER BY rnd`, all rows | 8.0 / 78 (index-only order) | 27 / 295 | cannot: 3.4 to 3.8 times slower (a sort) |
| `min(rnd), max(rnd)` | 0.06 / 0.06 | 0.21 / 1.44 | replaces with caveat (3.5 to 24 times slower, under 1.5 ms) |
| `DISTINCT grp`, `GROUP BY grp` with `sum` | 7.5 / 61.7, 26.4 / 248 | 0.71 / 4.9, 0.47 / 3.0 | better, 10 to 80 times faster (SiftAgg) |
| `count(*) WHERE rnd BETWEEN` 20% | 1.91 / 18.5 | 0.48 / 2.84 | better, 4 to 6.5 times |
| `LIKE 'abc%'` count | 0.06 / 0.10 | 2.93 / 22.6 | cannot: `LIKE` with a literal prefix is an index condition now, but the prefix test reads every entry of the segment dictionary (rows: 30 to 150 ms at 1,000,000 rows, btree 0.07) |
| nested loop of 100 id lookups | 0.17 / 0.19 | 0.19 / 0.29 | replaces |
| referential check of a foreign key on the referencing column (100,000 rows) | 0.31 | 0.27 (heap scan 9.75) | replaces |
| index-only scan of a covering btree (`SELECT id WHERE rnd BETWEEN` 0.1%) | 0.04 / 0.18 | 0.71 / 6.13 as a bitmap scan + heap; 0.93 / 4.45 as an `Index Only Scan` | replaces with caveat (the first version read the dictionaries of the text columns: see Tuple by tuple) |
| `UNIQUE`, `PRIMARY KEY`, `EXCLUDE`, foreign key target |  |  | cannot (stay btree) |
| merge join order, `ORDER BY .. DESC NULLS LAST` index scans, `INCLUDE` |  |  | cannot |

Cold shared buffers (relation evicted with `pg_buffercache_evict_relation`, the operating system cache warm), 200,000 rows, ms: `id = c` 1.2 (btree 1.1),
`rnd = c` 1.9 (1.0), narrow range 1.5 (0.4), range of 20% 6.6 (13.6), `grp = c` rows 8.4 (5.7).

**What to replace.** A btree whose workload is analytical or mixed (ranges, many columns, `IN` lists, counts, `GROUP BY`, a `WHERE` on several
columns in any combination) and not unique: one `sift_col` index replaces the btrees on those columns at 3% to 56% of the space, builds 1.1 to 3
times faster, writes 0.3 to 0.4 times the WAL and answers the aggregates 4 to 80 times faster. **What not to replace**: unique constraints and
primary keys; any index that serves single-row lookups of a high-cardinality key (a random int, text or uuid) on a table that grows past about
100,000 rows, which is the point lookup of an OLTP table; ordered scans without a `LIMIT` and `ORDER BY .. LIMIT` where a few hundred
microseconds count; covering indexes that avoid the heap; `LIKE 'abc%'` row lookups.

**What would make it a drop-in for the rest.** The point lookup of a random int at 1,000,000 rows is 1.9 to 2.5 ms with the classes alone and 0.18 to 0.23 ms with
`set_columns = 'rnd'` (the btree 0.04 ms): the summary leaves one unit to decode, and the rest is the price of opening the source and decoding that unit. Text
and uuid equality and `LIKE 'abc%'` cost 30 to 150 ms at 1,000,000 rows with or without a summary (`set_columns` takes integer and text columns, not uuid): they
test every entry of the segment dictionary, which is kept in the order the strings came; a hash over the cached dictionary, or dictionaries in byte order, is what
they wait for. The classes could become the default ones for their types once this is done.

## Which of your btree indexes can be dropped

index with the classes and `set_columns = 'id, rnd, s_short'`, against the btree on the column, `sift.enable_col_scan = on`. A btree that serves several
rows of this table can go only if every one of them says drop.

<!-- col-scan-table:start -->
| a btree that serves | sift_col / btree, 1,000,000 rows (ms) | 100,000 rows (ms) | ratio | verdict |
|---|---|---|---|---|
| point lookup of a clustered key (`id = c`), rows | 0.09 / 0.06 | 0.19 / 0.05 | 1.5x | drop the btree |
| point lookup of a random int (`rnd = c`), rows | 0.23 / 0.04 | 0.12 / 0.05 | 5.6x | drop it if 6 times the time is fine |
| covering lookup (`SELECT id, rnd WHERE rnd = c`) | 0.47 / 0.10 | 0.13 / 0.09 | 4.5x | drop it if 5 times the time is fine |
| `IN` of 10 / 100 values, rows | 0.36 / 0.08, 4.07 / 0.86 | 0.17 / 0.12, 1.69 / 0.93 | 4.5x, 4.7x | drop it if 5 times the time is fine |
| narrow range of a clustered key, rows | 0.34 / 0.30 | 0.21 / 0.10 | 1.1x | drop the btree |
| range of 0.1% of a random int, rows / covering | 6.08 / 3.46, 4.45 / 0.20 | 0.89 / 0.74, 0.93 / 0.10 | 1.8x, 22.8x | keep the btree (23 times slower) |
| equality on one column and a range on another, rows | 1.90 / 3.82 | 0.43 / 0.45 | 0.5x | drop the btree (faster) |
| unordered `LIMIT 10` | 0.11 / 0.09 | 0.20 / 0.11 | 1.3x | drop the btree |
| `count(*)` of an `IN` of 100 values | 3.35 / 0.36 | 1.41 / 0.47 | 9.3x | drop it if 9 times the time is fine |
| `count(*)` of a range of 20% | 0.68 / 9.64 | 8.57 / 2.77 | 0.07x | drop the btree (faster) |
| `DISTINCT`, `GROUP BY` | 4.56 / 81.96 | 0.75 / 10.76 | 0.06x | drop the btree (faster) |
| text / uuid equality, rows | 66.37 / 0.10, 74.17 / 0.08 | 5.83 / 0.08, 5.46 / 0.09 | 684.2x, 976.0x | keep the btree (976 times slower) |
| `LIKE 'abc%'`, rows | 72.70 / 0.07 | 21.70 / 0.09 | 969.4x | keep the btree (969 times slower) |
| `ORDER BY .. LIMIT 10` / `LIMIT 1000` | 0.51 / 0.07, 7.21 / 0.32 | 0.32 / 0.05, 3.35 / 0.22 | 7.4x, 22.3x | keep the btree (22 times slower) |
| `min(rnd), max(rnd)` | 1.54 / 0.18 | 0.24 / 0.11 | 8.5x | drop it if 8 times the time is fine |
| `UNIQUE`, `PRIMARY KEY`, `EXCLUDE`, the target of a foreign key |  |  |  | keep the btree (no unique check, docs/COL-SCAN.md) |

| 1,000,000 rows, 7 columns | index bytes | bytes a row | of the btrees |
|---|---|---|---|
| btree (6 indexes) | 170,000,384 | 170.00 | 1.00x |
| sift_col classes | 28,893,184 | 28.89 | 0.17x |
| sift_col classes + set_columns | 30,269,440 | 30.27 | 0.18x |
<!-- col-scan-table:end -->
