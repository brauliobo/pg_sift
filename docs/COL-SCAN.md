# The bitmap scan of a sift_col index, and replacing a btree with one

A `sift_col` index built with the operator classes `sift.col_*_ops` is a **bitmap index scan** for the planner: `WHERE id = 5`, `IN` lists, ranges, `IS [NOT] NULL`, `LIKE 'abc%'` on integer, date, timestamp, boolean, float, numeric(p <= 18, s), char(n), text and uuid columns return rows through `Bitmap Heap Scan` or `Index Scan` (and `Index Only Scan` for the fixed-width columns), for any subset of the columns of a multicolumn index, combined with `BitmapAnd` and `BitmapOr`. Since 3.6 the default class `sift.col_ops` (what `CREATE INDEX .. USING sift_col (id, name)` gives) is in the same position: its family holds the operators of every class, so one plain index serves the column scans of SiftAgg (aggregates, `GROUP BY`, `DISTINCT`, `ORDER BY .. LIMIT`, [COL-PLAN.md](COL-PLAN.md)) and the filters of a statement that returns rows, including the `WHERE` of an `UPDATE` or `DELETE`. A statement that aggregates, groups, deduplicates, has a window function or sorts under a `LIMIT` is not offered the index and bitmap paths of an index whose columns are all of the default class, so its plan is the one it had (SiftAgg, or the normal plan); name the classes to give it both. The classes are catalog entries over the same index: an existing index keeps its class and its bytes, and `ALTER EXTENSION pg_sift UPDATE` is all it needs.

```sql
CREATE INDEX ON items USING sift_col (id sift.col_int4_ops, kind sift.col_text_ops, ts sift.col_timestamptz_ops);
SET sift.enable_col_scan = on;      -- on by default; off keeps the planner from bitmap scans of the index
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

`= ANY (array)` is one scan (a set test per unit); `x < ANY (array)` is the test against the largest element. `IS NULL`/`IS NOT NULL` are index quals. A column of a multicolumn index may carry another class (or the default one: the scan then ignores it).

## A plain index and a point lookup, per column type

One single-column `sift_col` index per column, 200,000 rows, instructions of the backend for `SELECT * FROM p WHERE c = K` and for an `IN` list of 16 keys, the statement planned and run in a loop of `EXECUTE` (the loop with the statement only formatted subtracted), warm; "seq scan before" is what a plain index gave in 3.5 (the rows came from the heap). The index is the plain one (`USING sift_col (c)`); the point directory of `point_tid_columns` is the opt-in column, with the size it costs. A `bool` column and a `float8` one: the plain index answers a `float8` equality in 0.42 M and cannot hold a directory; a `bool` equality returns half the table and keeps the sequential scan.

| column (200,000 rows) | seq scan before | plain index | plain + `point_tid_columns` | b-tree | plain / +point_tid / b-tree index MB |
|---|---|---|---|---|---|
| smallint, 6 rows a key | 84 M / 105 M | 1.14 M / 19.23 M | 0.24 M / 0.98 M | 0.23 M / 0.85 M | 0.50 / 2.25 / 2.20 |
| integer, unique | 89 M / 110 M | 0.29 M / 2.29 M | 0.21 M / 0.48 M | 0.18 M / 0.39 M | 0.20 / 3.43 / 4.51 |
| bigint, 6 rows a key | 95 M / 117 M | 0.36 M / 3.71 M | 0.21 M / 0.51 M | 0.18 M / 0.41 M | 0.20 / 2.65 / 4.38 |
| date, 40 rows a key | 101 M / 123 M | 1.35 M / 21.84 M | 0.39 M / 3.02 M | 0.36 M / 2.44 M | 0.45 / 1.38 / 1.43 |
| timestamp, unique | 106 M / 128 M | 0.32 M / 2.33 M | 0.21 M / 0.52 M | 0.18 M / 0.43 M | 0.20 / 3.43 / 4.51 |
| timestamptz, unique | 112 M / 134 M | 0.32 M / 2.34 M | 0.21 M / 0.52 M | 0.18 M / 0.43 M | 0.19 / 3.43 / 4.51 |
| uuid, unique | 128 M / 159 M | 0.29 M / 2.35 M | 0.22 M / 0.81 M | 0.19 M / 0.50 M | 3.44 / 8.45 / 6.33 |
| text, unique | 157 M / 182 M | 0.29 M / 2.43 M | 0.25 M / 1.16 M | 0.19 M / 0.50 M | 3.71 / 8.64 / 11.83 |
| text, 200 rows a key | 164 M / 183 M | 2.14 M / 182.58 M | 1.08 M / 14.21 M | 1.08 M / 11.01 M | 0.45 / 0.85 / 1.38 |
| float8, unique | 154 M / 179 M | 0.42 M / 2.51 M | n/a | 0.18 M / 0.41 M | 0.32 / nan / 4.51 |
| numeric(12,2), 1.3 rows a key | 178 M / 188 M | 0.38 M / 4.51 M | 0.21 M / 0.54 M | 0.19 M / 0.58 M | 0.22 / 3.43 / 4.51 |

The plain index reads the zones and key lists of the units (about 0.3 M instructions to find one key, 1.6 times a b-tree, 0.05 ms) and costs 0.2 to 0.5 MB in 200,000 rows for a number or a date, the dictionary for a string. Where a key has many rows (a smallint or date with 6 to 40 rows a key, a text with 200) or an `IN` list is long, the units are read and the directory pays: smallint 1.14 M to 0.24 M, date 1.35 M to 0.39 M, a list of 16 smallints 19.2 M to 0.98 M. It costs 16 bytes a row for a unique key (the size of a b-tree) and 1 to 10 for a key that repeats, so it is opt-in for the columns whose lookups matter.

## The first statement of a backend, and the next ones

A pooled application connection is often new, so the first equality of a backend counts as much as the warm ones above. `SELECT * FROM p WHERE c = K` in a fresh backend (buffers of its execution as `EXPLAIN (ANALYZE, BUFFERS)` shows them, the milliseconds and the instructions of the backend from `perf stat`, which include about 2.0 million for starting and reading the catalogs, a b-tree pays them too), then the instructions of the next statement of that backend. The plain index is `USING sift_col (c)`; `point_columns` is that with `WITH (point_columns = 'c')`. One single-column index, tables of the column and its row number only:

| rows | column | b-tree: buffers / ms / instructions | plain: buffers / ms / instructions, next statement | `point_columns`: buffers / ms / instructions |
|---|---|---|---|---|
| 200,000 | uuid, unique | 4 / 1.9 / 2.04 M | 19 / 1.9 / 2.33 M, 0.32 M | 22 / 2.6 / 2.26 M |
| 200,000 | text, unique | 4 / 1.8 / 2.05 M | 20 / 2.0 / 2.35 M, 0.35 M | 24 / 2.2 / 2.29 M |
| 200,000 | bigint, random, unique | 4 / 1.7 / 2.04 M | 268 / 2.3 / 3.07 M, 1.07 M | 18 / 1.8 / 2.21 M |
| 1,000,000 | uuid, unique | 4 / 1.7 / 2.04 M | 24 / 2.5 / 2.34 M, 0.33 M | 25 / 2.4 / 2.27 M |
| 1,000,000 | text, unique | 4 / 1.8 / 2.05 M | 34 / 2.6 / 2.40 M, 0.37 M | 45 / 2.5 / 2.38 M |
| 1,000,000 | bigint, random, unique | 4 / 1.8 / 2.04 M | 1,287 / 3.6 / 6.67 M, 4.66 M | 19 / 2.5 / 2.21 M |
| 3,000,000 | uuid, unique | 4 / 2.8 / 2.04 M | 51 / 5.0 / 2.42 M, 0.37 M | 55 / 5.5 / 2.43 M |
| 3,000,000 | text, unique | 5 / 2.5 / 2.05 M | 31 / 1.9 / 2.39 M, 0.36 M | 80 / 2.6 / 2.59 M |
| 3,000,000 | bigint, random, unique | 4 / 3.7 / 2.04 M | 3,831 / 18.6 / 15.65 M, 13.63 M | 20 / 4.0 / 2.22 M |

Until 3.6 the dictionary of a plain index was parsed whole by the first equality of a backend: 410 buffers (uuid) and 445 (text) on 200,000 rows, the whole index, and 5,977 buffers, 31 ms and 136 M instructions on 3,000,000 uuids. A dictionary of 1,024 strings or more now carries the hash of its strings, so the code of the string is found with a window of slots and one block, and the lookup of its rows reads the head of the unit: 19 to 51 buffers for a uuid and 20 to 34 for a text, against 4 for a b-tree, and the next statement 0.3 to 0.4 M instructions (the first costs the process start and the read of the head, not the index).

A key that the index cannot place without reading the column still reads all of it, on every statement, warm or not: a bigint of random values (a customer or user id) has no zone that excludes a unit and no dictionary, so the equality reads the 25 MB of 3,000,000 values (3,831 buffers, 18.6 ms), where a point directory (`point_columns`) reads 20. Keys that arrive in order (an increasing id, a timestamp) are skipped by their zones and read 11 to 24 buffers on 200,000 rows.

## A probe whose key a parent query passes down

An `EXISTS (SELECT .. FROM fact WHERE fact.k = outer.k)` (or a scalar subselect) is planned twice by PostgreSQL: as a correlated subplan, which probes the index for each outer row, and as a hashed subplan, which reads the fact once. `setrefs.c` chooses by multiplying the price of one probe by twice the rows it expects of the node that holds the filter, and a few correlated filters (four columns that move together) put that estimate 1,000 times below the rows that arrive: 5 expected, 5,374 evaluated. A probe of a point directory (0.2 to 0.4 ms, 270 cost units) against one pass of a 720,000-row table (90 ms, 28,000 units) then looked cheaper by 9 times and ran 14,000 times: 2,860 ms against 92 ms with `sift.enable_col_scan = off`.

The price of a probe of that kind (an index qual with a `PARAM_EXEC` that a parent query hands down, `rescan_floor` in `src/col/amscan/amscan.c`) is at least 90% of the sequential scan of the table, the ratio PostgreSQL then divides by the rows a key matches to price an EXISTS. The hashed subplan wins where it fits in `hash_mem` (82 to 91 ms against 83 to 92 ms with the scan off) and where it does not, or for a scalar subselect, the probe is still the plan: 226 ms for 9,091 probes against 18 s of sequential scans. A constant key, a join (the outer value is a `Var`) and `userid = C` are not touched.

## An aggregate of a column the index does not hold

An index of the default class (`CREATE INDEX .. USING sift_col (d)`) gives a statement that aggregates, groups or sorts under a `LIMIT` no index or bitmap path, so that SiftAgg and the normal plan are chosen as in 3.5. SiftAgg reads the index alone: a statement that reads a column the index does not hold (`avg(score)` over a window of dates, `count(*)` with `score = 3`) is not its to answer, and that rule left such a statement only the sequential scan. Such an index keeps its index and bitmap paths when the statement reads a column outside it (`index_holds_columns_read` in `src/col/amscan/amscan.c`); an index that holds every column the statement reads is still SiftAgg's.

3,000,000 rows of 190 bytes with 1,826 dates scattered over the table, `sift_col (d)` with `key_cut = 'd'`, `SELECT avg(score) FROM r WHERE d >= x AND d < x + 1 month` (51,000 rows), instructions of the backend: 1,470 M and 75,000 buffers before (sequential scan), 54.9 M and 3,104 buffers after (a b-tree 53.8 M and 3,048). A window of a year (600,000 rows) stays a sequential scan, 1,634 M (a b-tree 602 M): the price of the rows an index hands over is the next step.

## How it works

`ambeginscan`/`amgetbitmap` (`src/col/amscan/amscan.c`) turn the scan keys into predicates of the column source (`sift_col_begin`: zone maps skip units that cannot hold a match), filter the chunks of the remaining units, and give the TIDs of passing rows to the TID bitmap. The heap scan applies MVCC and rechecks lossy pages as for any bitmap scan. The write buffer is read like any unit; a row held for a dead tuple is dropped by the heap. Stored values are the heap's (an update that keeps the indexed columns is HOT and keeps the entry; any other update adds an entry), so the scan sets no recheck, except for a key the leaf cannot test exactly: any key on a numeric column (a NaN is stored as a NULL with the overflow flag), a NaN constant, a numeric constant too large to scale, and a `LIKE` pattern with a wildcard after its prefix are replaced by `IS NOT NULL` or a prefix test and the heap applies the clause. Text is compared bytewise: `CREATE INDEX` refuses `col_text_ops` on a nondeterministic collation, and the query's collation must equal the index column's.

**Cost** (`sift_col_amcostestimate`; cost units are about microseconds of a warm run):
- Start-up (a `LIMIT` does not wait for it): 24 for opening the index, snapshot and segment directory, plus 0.03 a unit for the zones and summaries of every unit, or 4 a key and 0.002 a unit when the point directory answers. Then, per unit kept, 22 for reading its head and pages and 0.002 per value of the quals' columns it filters. Then 0.01 for each row handed over (0.003 to an index-only scan): 10 to 12 ns a row measured in the bitmap node, 600,000 rows in 7 ms and all 3,000,000 in 30 ms.
- Units kept = the share whose zones can hold the quals' constants, counted on the real zones of a sample of units (`sift_col_zones`, the sample SiftAgg's planner uses; an `IN` list is bisected, so 50,000 values add about 3 ms of planning). For an equality or `IN` on a `key_columns` column, the share the small-set summaries leave, as SiftAgg counts it (`1 - exp(-s * n)` for a test on the share `s` of a column's values and `n` keys in a unit).
- A column with a point directory (`point_columns`) keeps the units that hold its keys' rows, `1 - exp(-rows / units)` of them for a random layout, and only those rows are filtered; rows come from the clause's selectivity, so a generic plan prices a parameter as its constant. An `IN` list is answered by the directory up to 65,536 keys (past 64, while it names under a sixteenth of the rows): a key costs 1.2 to 2 microseconds, 100 keys 0.13 ms, 1,000 keys 1.0 ms at 800,000 rows (answered from the units instead: 2.8 and 14 ms).
- Pick quality against SiftAgg and the heap: 12 of 96 (query, size) picks in the tables below were more than 1.3 times slower than the best other plan, none by more than 0.5 ms (worst: `count(*)` with an `IN` list of 100 values at 25,000 rows, SiftAgg 0.27 ms against 0.74 ms). The one larger loss: `count(*) WHERE rnd IS NULL` on 1,000,000 rows with no NULL, where the planner keeps SiftAgg (2.45 ms) and the bitmap scan takes 0.06 ms (the zones hold no NULL). A range at 0.1% to 80% of the table picks the bitmap scan up to 40% and the heap scan at 80%, where the plans are within 10% of each other.

**Setting** `sift.enable_col_scan` (default on): off, the planner builds no bitmap path on a `sift_col` index (it also holds when `enable_seqscan` is off). The index stays in the relation's index list, SiftAgg reads it as before.

**Tuple by tuple** (`amgettuple`, `amcanreturn`, also under `sift.enable_col_scan`): the index offers an `Index Scan` and an `Index Only Scan` in the order of the units (it has no order to give: `amcanorder` is off). A plan that stops early (`LIMIT 10` without `ORDER BY`), reads a few rows or runs on the inner side of a nested loop does not build a bitmap. A query that needs only integer, date, timestamp, boolean, float and numeric columns of the index does not read the heap (the visibility map decides, as for a btree: `Heap Fetches` in `EXPLAIN`). `pg_stat_user_indexes` counts the scans and `EXPLAIN` shows `Index Searches`.

Text, varchar and uuid columns are returnable only when the index names them in the reloption `return_columns = 's, u'` (default none; plain columns, not expressions; char(n) never; taken on the next scan). A string is read out of the dictionary of its segment one code at a time, without parsing the dictionary when few rows are read of a dictionary stored plain or for a lookup, and from the heap by the AM for a value the index could not keep (a leaf whose strings in a unit pass `unit_bytes`). Off by default: the executor asks for every returnable column whether the query needs it or not, so `SELECT id` reads strings too, and an Index Scan that fetches the rows is cheaper when the heap is in memory. Measured without / with the option: `SELECT id` 100 rows 0.066 / 0.158 ms, 10,000 rows 1.49 / 1.81 ms (the first query of a new backend 2.2 / 15 ms: it parses the dictionaries); `SELECT id, u, s` 100 rows 0.055 (Index Scan) / 0.155 ms (Index Only Scan), 10,000 rows 0.95 / 1.54 ms. The option is meant for a table much larger than memory, where heap fetches are disk reads (not measured).

**Limits**:
- No ordered scan (`amcanorder`, backward scans, `CLUSTER USING`): `ORDER BY .. LIMIT` stays with the top-N of SiftAgg and an `ORDER BY` of all rows sorts.
- No `UNIQUE`, `PRIMARY KEY`, `EXCLUDE` or foreign key target (the AM has no `amcanunique`); no `INCLUDE`, `ASC`/`DESC`, `text_pattern_ops`.
- `LIKE` is indexed as a literal prefix (`starts_with(col, 'a')` is not rewritten by the planner: write `col ^@ 'a'`).
- No classes for money, interval, time, enums, arrays, ranges, wide numeric or jsonb paths; no parallel index scan (a parallel bitmap heap scan works).
- `CREATE INDEX CONCURRENTLY`, `REINDEX CONCURRENTLY`, partial and expression indexes, a nested loop with the index on the inner side, and the referential check of a foreign key on the referencing column use it. `pg_dump` writes the class.

## Replacing a btree

Measured on PostgreSQL 18.6 (2026-10-22, release build of 3.2 plus this change, 256 MB `shared_buffers`, 32 queries at 25,000 to 1,000,000 rows of about 460 bytes). Summary at 1,000,000 rows, sift_col / btree:

|  | figure |
|---|---|
| index bytes per row | 0.03x to 0.56x (an ascending int 0.68 vs 22.5 B/row; uuid 16.9 vs 31.6); six columns in one index 25.9 MB against 139.4 MB of five btrees (0.19x) |
| build, serial | 111 to 272 ms vs 150 to 407 ms |
| bloat after churn | both grow about 2x and `VACUUM` gives nothing back; absolute bloat is 4.6 times smaller |
| WAL | 0.3 to 0.4 times |
| ascending key, narrow range, composite equality + range, skip scan, nested loop, foreign key check | within 1 to 1.7x of the btree or faster |
| `count(*)`, `DISTINCT`, `GROUP BY` | 4 to 80 times faster (SiftAgg) |
| random int point lookup | up to about 100,000 rows replaces; at 1,000,000 rows 40 times slower (every unit is read: zones do not prune a random key) |
| `IN` lists, narrow random ranges, `ORDER BY .. LIMIT`, `min`/`max` | 2 to 28 times slower, under 10 ms |
| `ORDER BY` of all rows | 3.4 to 3.8 times slower (a sort) |

**What to replace.** A btree whose workload is analytical or mixed (ranges, many columns, `IN` lists, counts, `GROUP BY`, a `WHERE` on several columns in any combination) and not unique: one `sift_col` index replaces the btrees on those columns at 3% to 56% of the space, builds 1.1 to 3 times faster, writes 0.3 to 0.4 times the WAL and answers the aggregates 4 to 80 times faster.

**What not to replace**: unique constraints and primary keys; any index that serves single-row lookups of a high-cardinality key (a random int, text or uuid) on a table that grows past about 100,000 rows, which is the point lookup of an OLTP table; ordered scans without a `LIMIT` and `ORDER BY .. LIMIT` where a few hundred microseconds count; covering indexes that avoid the heap; `LIKE 'abc%'` row lookups.

**What would make it a drop-in for the rest.** The point lookup of a random int at 1,000,000 rows reads the one unit holding the key (the `key_columns = 'rnd'` summary leaves it; without one every unit is read); what is left is a lookup inside the unit (a sorted run or hash per unit: a format change). Text and uuid equality and `LIKE 'abc%'` go through the segment dictionary: a backend parses it once (cost of the distinct values, so a new connection pays it on its first query), and the dictionary it keeps (`dict_cache_kb`, docs/LIMITS.md) gets a table of its codes after 8 equalities and its codes in byte order after 16 prefix or range tests, so a repeated lookup is a probe and a prefix costs the matches (`key_columns` takes uuid columns since 3.3). What a new connection still pays needs the lookup structure stored with the dictionary (a format change).

## Which of your btree indexes can be dropped

It compares the planner's pick on a `sift_col` index with the classes and `key_columns = 'id, rnd, s_short, u'` against the btree on the column, `sift.enable_col_scan = on`, for two clients: one that opens a connection per query (a backend's first query parses what it reads) and one that keeps its connection (the same query after 24 runs). A btree that serves several rows of a table can go only if every one of them says drop.

<!-- col-scan-table:start -->
**A client that connects for each query**: the first query of a new backend (median of three sessions), which parses the dictionaries of its string
columns and fills its caches.

| a btree that serves | sift_col / btree, 1,000,000 rows (ms) | 100,000 rows (ms) | ratio | verdict |
|---|---|---|---|---|
| point lookup of a clustered key (`id = c`), rows | 0.16 / 0.032 | 0.13 / 0.086 | 5.0x | drop it if 5 times the time is fine |
| point lookup of a random int (`rnd = c`), rows | 0.34 / 0.038 | 0.12 / 0.10 | 8.9x | drop it if 9 times the time is fine |
| covering lookup (`SELECT id, rnd WHERE rnd = c`) | 0.39 / 0.048 | 0.16 / 0.081 | 8.2x | drop it if 8 times the time is fine |
| `IN` of 10 / 100 values, rows | 0.58 / 0.072, 7.44 / 0.35 | 0.17 / 0.16, 2.38 / 1.26 | 8.0x, 21.2x | keep the btree (21 times slower) |
| narrow range of a clustered key, rows | 0.50 / 0.27 | 0.20 / 0.17 | 1.9x | drop the btree |
| range of 0.1% of a random int, rows / covering | 8.38 / 2.79, 7.08 / 0.23 | 0.98 / 1.09, 0.78 / 0.12 | 3.0x, 31.4x | keep the btree (31 times slower) |
| equality on one column and a range on another, rows | 3.44 / 3.59 | 0.29 / 0.41 | 1.0x | drop the btree (faster) |
| unordered `LIMIT 10` | 0.20 / 0.052 | 0.12 / 0.074 | 3.9x | drop it if 4 times the time is fine |
| `count(*)` of an `IN` of 100 values | 6.73 / 0.32 | 1.93 / 0.44 | 21.2x | keep the btree (21 times slower) |
| `count(*)` of a range of 20% | 1.14 / 13.61 | 5.53 / 2.79 | 0.08x | drop the btree (faster) |
| `DISTINCT`, `GROUP BY` | 7.54 / 82.64 | 0.49 / 13.97 | 0.09x | drop the btree (faster) |
| text / uuid equality, rows | 31.60 / 0.072, 34.22 / 0.077 | 1.96 / 0.10, 3.64 / 0.076 | 438.8x, 444.4x | keep the btree (444 times slower) |
| `LIKE 'abc%'`, rows | 39.38 / 0.082 | 2.70 / 0.082 | 480.3x | keep the btree (480 times slower) |
| `ORDER BY .. LIMIT 10` / `LIMIT 1000` | 0.89 / 0.065, 10.01 / 0.33 | 0.39 / 0.073, 5.03 / 0.24 | 13.7x, 30.7x | keep the btree (31 times slower) |
| `min(rnd), max(rnd)` | 3.01 / 0.11 | 0.25 / 0.13 | 26.8x | keep the btree (27 times slower) |
| `UNIQUE`, `PRIMARY KEY`, `EXCLUDE`, the target of a foreign key |  |  |  | keep the btree (no unique check, docs/COL-SCAN.md) |

**A backend that has run the query before**: the median of the last three of 24 runs in one session (a pooled connection).

| a btree that serves | sift_col / btree, 1,000,000 rows (ms) | 100,000 rows (ms) | ratio | verdict |
|---|---|---|---|---|
| point lookup of a clustered key (`id = c`), rows | 0.032 / 0.005 | 0.033 / 0.009 | 6.4x | drop it if 6 times the time is fine |
| point lookup of a random int (`rnd = c`), rows | 0.15 / 0.007 | 0.034 / 0.010 | 21.3x | keep the btree (21 times slower) |
| covering lookup (`SELECT id, rnd WHERE rnd = c`) | 0.12 / 0.009 | 0.040 / 0.010 | 13.1x | keep the btree (13 times slower) |
| `IN` of 10 / 100 values, rows | 0.30 / 0.022, 7.08 / 0.10 | 0.047 / 0.026, 1.54 / 0.15 | 13.5x, 69.5x | keep the btree (69 times slower) |
| narrow range of a clustered key, rows | 0.22 / 0.12 | 0.071 / 0.021 | 1.9x | drop the btree |
| range of 0.1% of a random int, rows / covering | 4.26 / 0.55, 4.00 / 0.22 | 0.26 / 0.058, 0.41 / 0.019 | 7.7x, 18.0x | keep the btree (18 times slower) |
| equality on one column and a range on another, rows | 0.79 / 0.51 | 0.062 / 0.064 | 1.5x | drop the btree |
| unordered `LIMIT 10` | 0.044 / 0.013 | 0.031 / 0.016 | 3.4x | drop it if 3 times the time is fine |
| `count(*)` of an `IN` of 100 values | 6.80 / 0.076 | 1.29 / 0.13 | 89.5x | keep the btree (89 times slower) |
| `count(*)` of a range of 20% | 0.67 / 11.23 | 5.16 / 2.53 | 0.06x | drop the btree (faster) |
| `DISTINCT`, `GROUP BY` | 7.10 / 85.90 | 0.29 / 10.42 | 0.08x | drop the btree (faster) |
| text / uuid equality, rows | 0.045 / 0.011, 0.048 / 0.009 | 0.030 / 0.010, 0.028 / 0.008 | 4.1x, 5.3x | drop it if 5 times the time is fine |
| `LIKE 'abc%'`, rows | 1.12 / 0.011 | 0.10 / 0.012 | 102.3x | keep the btree (102 times slower) |
| `ORDER BY .. LIMIT 10` / `LIMIT 1000` | 0.33 / 0.014, 7.64 / 0.19 | 0.14 / 0.014, 3.54 / 0.18 | 23.8x, 40.0x | keep the btree (40 times slower) |
| `min(rnd), max(rnd)` | 1.75 / 0.028 | 0.062 / 0.028 | 62.4x | keep the btree (62 times slower) |
| `UNIQUE`, `PRIMARY KEY`, `EXCLUDE`, the target of a foreign key |  |  |  | keep the btree (no unique check, docs/COL-SCAN.md) |

| 1,000,000 rows, 7 columns | index bytes | bytes a row | of the btrees |
|---|---|---|---|
| btree (6 indexes) | 170,221,568 | 170.22 | 1.00x |
| sift_col classes | 29,122,560 | 29.12 | 0.17x |
| sift_col classes + key_columns | 30,269,440 | 30.27 | 0.18x |
| sift_col classes + point_columns | 93,724,672 | 93.72 | 0.55x |
| sift_col classes + point_tid_columns | 91,381,760 | 91.38 | 0.54x |
<!-- col-scan-table:end -->
