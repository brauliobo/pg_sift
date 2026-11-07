# pg_sift

[![ci](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brauliobo/pg_sift/actions/workflows/ci.yml) [![packages](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml/badge.svg)](https://github.com/brauliobo/pg_sift/actions/workflows/release.yml) [![PostgreSQL](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018%20%7C%2019-336791?logo=postgresql&logoColor=white)](docs/SUPPORT.md#postgresql-versions) [![release](https://img.shields.io/badge/release-4.1.3-blue)](https://github.com/brauliobo/pg_sift/releases/latest) [![license](https://img.shields.io/github/license/brauliobo/pg_sift)](LICENSE)

pg_sift makes analytics on PostgreSQL fast: you keep your queries and add an index. A `sift_col` index, or a `sift_table`, answers counts, sums, distinct counts, group-bys and top-N from compressed columns; the planner picks it, and a query it cannot prove equal keeps PostgreSQL's own plan. It also adds ranked BM25 full-text search (boolean, phrase, prefix, regex, fuzzy, facets) with a `sift` index, which has its own query syntax. One `CREATE EXTENSION pg_sift`: no separate analytics database, no search server, no `shared_preload_libraries`.
It is for applications that keep their data in PostgreSQL and serve dashboards, reports and search from the same rows. It supports PostgreSQL 17, 18 and 19[^pg19]; the search engine also runs as a SQLite extension and a MariaDB library.

[^pg19]: PostgreSQL 19 is tested on its release candidate (a server built from `REL_19_STABLE`, the same jobs as for 17 and 18); see [docs/SUPPORT.md](docs/SUPPORT.md#postgresql-versions).

[In five minutes](#in-five-minutes) · [At a glance](#at-a-glance) · [Quick start](#quick-start) · [Features](#features) · [Compared with](#compared-with) · [SQLite and MariaDB](#sqlite-and-mariadb) · [Operations and limits](#operations-and-limits) · [SQL reference](#sql-reference) · [Docs](#docs)


## In five minutes

Install from source (PostgreSQL 17, 18 or 19 with server headers and a C compiler; packages and Docker are under [Install](#install)):

```sh
make USE_PGXS=1 && sudo make USE_PGXS=1 install
psql -d mydb -c 'CREATE EXTENSION pg_sift'      # as a superuser
```

Then, in `psql -d mydb`, a generated table of 1,000,000 orders, nothing to download:

```sql
CREATE TABLE orders AS
SELECT g AS id,
       1 + (random() * 50000)::int AS customer_id,
       (ARRAY['US','DE','BR','JP','IN','FR'])[1 + (random() * 5)::int] AS country,
       (ARRAY['books','toys','garden','music'])[1 + (random() * 3)::int] AS category,
       round((random() * 300)::numeric, 2)::numeric(10,2) AS amount,
       timestamp '2025-01-01' + random() * interval '365 days' AS created_at
FROM generate_series(1, 1000000) g;
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, amount, created_at);
VACUUM ANALYZE orders;

EXPLAIN (COSTS OFF) SELECT category, count(*), sum(amount), avg(amount) FROM orders GROUP BY 1;   -- Custom Scan (SiftAgg) on orders_col
SET sift.enable_agg = off;                                                                           -- the plan PostgreSQL has without pg_sift
```

Medians of six runs, warm, PostgreSQL 18.6 on a shared x86-64 host (2026-10-24), the same table and answers with `sift.enable_agg` on and off:

| system | count, sum, avg<br>by category | distinct customers<br>where country = 'DE' | top 5 customers<br>by sum | a month of orders<br>by country, category |
|---|---:|---:|---:|---:|
| pg_sift | 0.14 ms | 2.5 ms | 12 ms | 20 ms |
| PostgreSQL | 54 ms | 68 ms | 177 ms | 30 ms |

The last column is a month of random dates, which no unit can skip: the gain is the smallest there. Declare `amount` as `numeric(10,2)`: with a bare `numeric` the scan is chosen and still wins, but 4 to 19 times less than with the precision ([known gap](#limitations-and-trade-offs)). Full-text search is [below](#try-it-full-text-search).

**Use it** when the data stays in PostgreSQL and you want fast dashboards and reports from the same rows, with counts, sums, distinct counts, group-bys and top-N that do not scan the table (your queries stay as they are; you add an index), and ranked search as a second kind of index. **Do not use it** for a primary-key lookup (the btree is as fast), for update-heavy tables through `sift_table` (a write costs more than a heap write), or for warehouse-size tables that need sharding; every measured loss is in [Limitations and trade-offs](#limitations-and-trade-offs). **Versions:** PostgreSQL 17, 18 and 19 on Linux, 17 and 18 on macOS ([docs/SUPPORT.md](docs/SUPPORT.md)). **Upgrades:** `ALTER EXTENSION pg_sift UPDATE`, and a rebuild only when the on-disk format changes ([docs/UPGRADING.md](docs/UPGRADING.md)). **License:** the [PostgreSQL License](LICENSE).

## At a glance

**Use it** when the question reads many rows and the answer is small (your queries stay as they are; you add an index), or when ranked search should sit next to the data (it has its own query syntax):

- **Aggregates over a filtered table:** counts, sums, distinct counts, group-bys, time buckets, top-N and window reports over hundreds of thousands to millions of rows, answered from pre-aggregates and compressed columns; the time grows slower than the rows.
- **Ranked search next to the data:** BM25 with filters, counts and facets in the same transaction as the rows and no second system to keep in step; updates, deletes, VACUUM and replicas are PostgreSQL's.

**Do not use it** (every measured loss is in [Limitations and trade-offs](#limitations-and-trade-offs)) for:

- a lookup by key where a btree exists: the btree is as fast or faster, and the planner keeps it;
- a report whose whole query takes a few milliseconds over a small result: flat PostgreSQL tables are as fast or faster than the scan;
- tables that need a warehouse: it is one PostgreSQL node, without sharding or distributed execution; ClickHouse loads faster and stores the same table in less space, and ClickHouse and Elasticsearch are ahead on some filtered averages and range counts at 10,000,000 rows;
- text analysis beyond English stemming and one key per index, and the search classes where Elasticsearch is ahead;
- writes, spilling distinct counts and queries that keep a few rows: slower than PostgreSQL's own plan on some shapes;
- a shape the planner cannot prove equal: it keeps the normal plan.

### The benchmark cases

The 26 cases of [bench/cases.tsv](bench/cases.tsv) marked `headline`, measured on release 4.0 (tip 3c5e625d, `track_changes` on) with `bench/bench cases --rows 300000` (run 3c5e625dfb80-20261103T202442Z-8ca1): PostgreSQL 18.6, one x86-64 host, the data in memory, one backend, warm medians of 5 runs. Each cell is the time of the statement (a read: the execution time of `EXPLAIN ANALYZE`; a load: the wall time of its statements), the millions of instructions of the backend and its workers, and the buffers it touched (shared-buffer hits and reads of every relation, 8 kB each: with the data in memory, the IO of the plan in blocks). The other column is the faster of PostgreSQL's plan with the index left out (a sequential scan) and, where the table has one, the plan with its b-tree; the ratio is its time divided by pg_sift's, so a value under 1 is a statement the other plan answers faster. One case per row, as the registry lists them; reproduce one with `bench/bench cases --cases ID --rows 300000`.

| case | against | pg_sift: ms · M instr · blocks | the other: ms · M instr · blocks | other ÷ pg_sift |
|---|---|---|---|---:|
| `topn_page_totals`: A page of rows with the total of its set | heap plan | 0.61 ms · 7.69 M · 473 | 29 ms · 374 M · 4,107 | 48x |
| `topn_coalesce`: ORDER BY coalesce(column, constant) LIMIT n | heap plan | 0.4 ms · 5.58 M · 98 | 32.1 ms · 299 M · 4,105 | 80x |
| `key_presence_distinct`: count(DISTINCT key) of a whole table from the key lists and bitmaps of its units | heap plan | 0.14 ms · 2.09 M · 26 | 56.7 ms · 410 M · 4,105 | 405x |
| `bulk_copy_wal`: COPY into a sift_table, written as column groups, against a heap COPY | heap plan | 422 ms · 5,210 M · 802 | 229 ms · 3,164 M · 12,061 | 0.5x |
| `topn_sorted_runs`: ORDER BY an unsorted integer column LIMIT n from the sorted run of each unit (order_columns) | heap plan | 0.115 ms · 0.873 M · 281 | 22.4 ms · 150 M · 4,105 | 195x |
| `lateral_first_row`: a LIMIT 1 child with an ORDER BY, read as a first-row lookup when a unique index holds the key and the order columns | heap plan | 4.17 ms · 65.6 M · 2,973 | 1,449 ms · 25,536 M · 455,241 | 347x |
| `cte_two_readers`: a common table expression that two aggregates read is planned inline when the scans are the cheaper part | heap plan | 2.48 ms · 25.8 M · 500 | 68.4 ms · 542 M · 13,639 | 28x |
| `list_elements`: the elements of a jsonb array leaf counted with a filter on them (the decoded list lengths are kept for the backend) | heap plan | 3.36 ms · 35.9 M · 623 | 113 ms · 781 M · 13,639 | 34x |
| `cte_series_arms`: three UNION ALL arms over the elements of a jsonb array, counted DISTINCT above a spine: the shared common table ... | heap plan | 8.76 ms · 133 M · 768 | 858 ms · 8,283 M · 13,731 | 98x |
| `grouping_sets_report`: a report with four levels in one statement: GROUPING SETS over two dimensions, a date filter and three aggregates | heap plan | 2.85 ms · 38.9 M · 198 | 65.9 ms · 744 M · 4,107 | 23x |
| `ranked_top_per_group`: the three best categories of every country by revenue: a window function over the groups of an aggregate, filtered by ... | heap plan | 2.82 ms · 33.1 M · 248 | 63.2 ms · 645 M · 4,105 | 22x |
| `window_json_report`: a ranked report as one document per category: the rank of the category, its share of the total, and the top 3 countries ... | heap plan | 4.53 ms · 61.8 M · 298 | 71.5 ms · 724 M · 4,105 | 16x |
| `two_level_spend`: the distribution of spend per customer: an aggregate of an aggregate, 200,000 customers at 1,000,000 orders | heap plan | 24.4 ms · 245 M · 338 | 153 ms · 748 M · 4,105 | 6.3x |
| `group_by_id_top`: one group per row: the 10 largest orders by value, as a GROUP BY of the primary key (1,000,000 groups at 1,000,000 ... | heap plan | 13 ms · 103 M · 253 | 162 ms · 1,936 M · 4,105 | 12x |
| `count_distinct_unique`: count(DISTINCT) of a key that is unique: the set holds one entry per row | heap plan | 1.13 ms · 14.7 M · 126 | 238 ms · 2,962 M · 4,105 | 211x |
| `semi_exists_self`: a semi join on the same table: the orders of the customers that have a cancelled order | heap plan | 2.77 ms · 19.3 M · 4,381 | 57.2 ms · 468 M · 8,212 | 21x |
| `anti_not_in_self`: an anti join as NOT IN (SELECT ..) on the same table: the orders of the customers that never cancelled | heap plan | 10.1 ms · 76.6 M · 4,464 | 56.1 ms · 577 M · 8,212 | 5.6x |
| `keyset_page`: keyset pagination: the page after a (created_at, id) bar, a row comparison with a two-column order | b-tree | 0.104 ms · 0.997 M · 91 | 0.039 ms · 0.491 M · 56 | 0.4x |
| `point_agg_fk`: a lookup of the rows of one foreign key with no btree on it: the aggregate of one customer | heap plan | 0.063 ms · 0.391 M · 95 | 10.7 ms · 121 M · 4,107 | 170x |
| `point_agg_id`: a lookup by the primary key through the aggregate scan, against the btree of the key | b-tree | 0.035 ms · 0.339 M · 25 | 0.012 ms · 0.173 M · 5 | 0.3x |
| `keycut_customer_agg`: a lookup of the rows of one customer when the heap is in order of arrival and the index cuts its units by the customer: ... | heap plan | 0.069 ms · 0.719 M · 34 | 10.3 ms · 121 M · 4,107 | 150x |
| `jsonb_each_member`: one member of a JSON object read through jsonb_each: a LEFT JOIN LATERAL that keeps the rows without it | heap plan | 3.35 ms · 35.9 M · 673 | 117 ms · 916 M · 13,639 | 35x |
| `softdel_live_groupby`: a report over the rows that are not soft-deleted: the units with no deleted row are answered from their pre-aggregates, ... | heap plan | 0.057 ms · 0.667 M · 32 | 50.8 ms · 596 M · 4,107 | 891x |
| `point_in_100`: an IN list of 100 ids through the access method: the point directory looks each key up (up to 64 keys it did; a longer ... | heap plan | 0.076 ms · 1.07 M · 143 | 14.8 ms · 161 M · 4,107 | 195x |
| `topn_scattered_time`: the n best rows by a time column whose rows are in no order, for an n above the longest head of a run: a superblock ... | heap plan | 0.188 ms · 1.89 M · 27 | 24.6 ms · 163 M · 1,911 | 131x |
| `key_table_point`: the rows of three keys of a foreign key column (6,313 values): one record of each segment's key table, not the units of ... | heap plan | 0.075 ms · 0.748 M · 28 | 19.3 ms · 273 M · 4,107 | 257x |

### Tables that change: `track_changes`

`track_changes` is on by default (`'auto'`: the primary key of the table; `WITH (track_changes = 'off')` turns it off for an index). It writes a tombstone for each entry an UPDATE, a key change or a DELETE ended and installs two statement triggers (AFTER UPDATE and AFTER DELETE; one NOTICE says so).

**What a table that changes ends as.** 100,000 rows, a primary key and one `sift_col` index, 10 rounds of an UPDATE of every row (1,000,000 updates) with no VACUUM, then autovacuum (naptime 1 s) and the worker of `sift.col_auto_merge`, both on as by default, until the index keeps one state for 30 s; `track_changes` as it is (the default) and `'off'` (`bench/churn/heapfree_settle.py`, run 2783fcb3884a-20261104T000858Z-444e, 3 repetitions of a grouped count of 10 tenants; the REINDEX column is the index built again over the same rows):

| `track_changes` | fresh | after the updates, before the VACUUM | settled | REINDEX | settled over REINDEX | grouped count before the VACUUM: ms · M instr · heap blocks | settled |
|---|---:|---|---|---:|---:|---|---|
| default | 0.57 MB | 47.6 MB (999,038 entries in the log) | 0.71 MB, 1 segment | 0.63 MB | +13.0% | 14.8 ms · 161 M · 1,406 | 0.029 ms · 0.40 M · 2 |
| `'off'` | 0.26 MB | 20.2 MB | 0.47 MB, 6 segments | 0.28 MB | +67.7% | 27.2 ms · 253 M · 10,280 | 0.043 ms · 0.50 M · 3 |

So the index does not end within 1% of a rebuilt one: the settled index is 13.0% over its REINDEX with the default and 67.7% with `'off'`, the log is empty after the vacuum, and a grouped count after it reads 2 heap blocks (3 with `'off'`), as on the fresh table. The updates take 4.7 s and write 338 MB of WAL with the default, 2.7 s and 288 MB with `'off'`. At 300,000 rows (3,000,000 updates, run 2783fcb3884a-20261104T001112Z-3429) the default index was 55.7 MB 30 s after the VACUUM, its rewrite not yet run (1.8 MB after a REINDEX), and the one with `'off'` 1.39 MB (+91.0% over 0.73 MB).

What it costs a write: 100,000 rows, a table with a primary key and one `sift_col` index, vacuumed, medians of 3 (`bench/churn/heapfree_default.py`, run fb9b4acd6f58-20261103T182341Z-00ce on the tree of the commit that merged the option); each cell is the time, the millions of instructions, the MB of WAL and the buffers touched:

| statement | `track_changes = 'off'`: ms · M instr · WAL · blocks | default (`auto`): ms · M instr · WAL · blocks | instructions |
|---|---|---|---:|
| UPDATE of 1 row | 0.223 ms · 0.363 M · 0.04 MB · 39 | 0.303 ms · 0.415 M · 0.04 MB · 54 | +14.2% |
| UPDATE of 100 rows | 0.651 ms · 3.51 M · 0.11 MB · 650 | 1.11 ms · 4.77 M · 0.12 MB · 1,061 | +35.9% |
| UPDATE of 5% of the rows (a covered column) | 18.8 ms · 145 M · 11.06 MB · 32,079 | 27.5 ms · 208 M · 11.25 MB · 52,256 | +43.6% |
| UPDATE of the key of 1% of the rows | 10 ms · 66.4 M · 7.98 MB · 6,936 | 12.9 ms · 86.9 M · 8.02 MB · 11,224 | +30.8% |
| DELETE of 2% of the rows | 8.19 ms · 53.6 M · 7.78 MB · 3,871 | 9.09 ms · 72.3 M · 7.85 MB · 7,404 | +34.9% |
| INSERT of 5% more rows | 9.29 ms · 86.5 M · 1.03 MB · 15,223 | 10.2 ms · 87 M · 1.05 MB · 15,241 | +0.6% |
| CREATE INDEX | 44.8 ms · 672 M · 0.32 MB · 3,763 | 61.1 ms · 858 M · 0.64 MB · 104,713 | +27.8% |
| REINDEX | 48.1 ms · 672 M · 0.26 MB · 3,763 | 58.9 ms · 858 M · 0.60 MB · 104,713 | +27.8% |


The triggers of several indexes of one table each look the key up in the unique index: with 2 and 3 indexes an UPDATE of the key of 1% of the rows takes 99.8 and 111.9 M instructions (86.9 with one), a DELETE of 2% 88.5 and 104.9 M (72.3 with one). A table that no read touches between its writes and its `VACUUM` pays that and gains nothing, which is what `'off'` is for.

**A table changed and not yet vacuumed (the case the option is for).** 300,000 rows, the recent 20% updated (`bench/churn/heapfree_churn_reads.py`): a grouped count reads 2 heap blocks instead of 1,241 (21.8 to 10.7 M instructions, 1.87 to 0.77 ms) and one tenant's rows 0.41 MB instead of 10.09 MB read cold. After a `VACUUM` no read differs, and point reads and index-only scans gain nothing either way.

### Limitations and trade-offs

**Known gap: the load.** A load of 1,000,000 orders (9 columns) takes 1.8 s into a `sift_table` with its primary key and BM25 index (`COPY` 1.1 s, the two indexes 0.4 s, `VACUUM (FREEZE, ANALYZE)` 0.3 s) against ClickHouse 0.7 s (2.6x) and Citus columnar 1.4 s (1.3x). ClickHouse loads without a primary key or a text index, which are 0.4 s of the 1.8 here; against the same `COPY` alone it is 1.1 s to 0.7 s. The `COPY` costs 22.7 G instructions of the backend against 12.9 G for a heap table (1.8x, the parse of the CSV is 44% of it for both) because the rows are encoded into column units, dictionaries and pre-aggregates as they come, which a heap table does at query time or never. What is left would need the encoding to leave the backend: the flush of a run is 16.6% of the instructions of the load, so a background worker could save at most that share of the wall time; handing the rows to the batch without per-row allocations saves 2.8% (measured), below what justified a change. Without workers one open segment takes the units as they fill, with no merge afterwards. Against a heap table `COPY` plus `sift.table_maintain` of 10,000,000 rows (the `bulk_copy_wal` case, one CPU) takes 13.8 s against 8.1 s (1.7x slower) and 168 G instructions against 107 G (1.6x), and writes 14 bytes of WAL a row against 84 and a table of 137 MB against 1,023 MB. What is left is the encoding itself (dictionaries, pre-aggregates, zstd), which a heap table does not do at load time; reading the table back is what it pays for. The gap to ClickHouse stays; the measurements. (The figures of the 1,000,000-row comparison with the other engines are not from records files: they were typed from the sessions of the dates in the changelog; those of the 10,000,000-row `bulk_copy_wal` runs are.)

**Known gap: text ordered reads.** `ORDER BY txt LIMIT n` over a text column is served from the index when the build chose the column (the default formula, strings up to 64 bytes) or `order_array_columns` names it (`'off'` drops it), under the collation of the column, with one key and no condition: `ORDER BY title LIMIT 10` over 500,000 rows takes 0.2 M instructions and 0.10 ms against 473.7 M and 30.0 ms for PostgreSQL's plan, and a page from a bound, `WHERE code >= 'c' ORDER BY code LIMIT 20` over 500,000 32-character strings, 0.4 M instructions and 0.17 ms against 241.2 M and 16.9 ms for the same index without the array (PostgreSQL's plan 329.9 M and 22.7 ms). What the array costs is the build: `CREATE INDEX` over 500,000 distinct 26-byte strings takes 6,215 M instructions and 707 ms against 2,248 M and 273 ms for a b-tree on the column (2.8x the instructions), and the index holds 10.8 MB against the b-tree's 24.9 MB. (Cases `topn_text_key`, `order_array_text_range` and `text_order_array_size`, 500,000 rows, one process, warm medians; the two reads have no b-tree arm, so their distance to a b-tree is not stated.)

**Known gap: the dictionary of a large text column.** The order array of a text column names its strings by their dictionary codes only while the dictionaries of the index, parsed and added up, take three quarters of the 64 MB cache of parsed dictionaries or less (about 700,000 distinct 32-byte strings: 800,000 hashes take 48.4 MB with the strings kept, 1,000,000 take 60.7 MB, a b-tree 59.1 MB, and 23.6 MB with codes); past that it keeps the strings, because with codes every equality parsed a dictionary again (10 to 25 ms for 0.13). The first backend after a server start parses each dictionary it meets and writes it to the shared ring (`sift.col_dict_shared_cache_kb`): 26 to 167 ms once per dictionary at 100,000 to 1,000,000 strings, and the other backends find it (1.4 to 2.8 ms for their first statement, a b-tree 2.0 to 2.7); with a ring of 0 or under twice a dictionary's record each backend parses its own once (23 to 47 ms for an equality over 500,000 to 700,000 hashes, then 0.1 ms). (The figures of this paragraph are not from records files; the cases `bigdict_point_probes`, `dict_shared_first_statement` and `dict_shared_find` hold the same shapes with records.)

**Known gap: a `numeric` without precision.** `GROUP BY` two columns (16 groups) with `count`, `sum` and `avg` of a bare `numeric` column takes the scan and, since the sum runs in workers, beats PostgreSQL's plan at every size and worker count measured: 6.8 against 11.5 ms at 100,000 rows (2 workers); 23.3 against 43.4 ms at 1,000,000 (2 workers; 22.7 against 43.4 with 4) and 46.6 against 109.9 ms in one process (693 against 2,110 million instructions); 160 against 389 ms at 10,000,000 (2 workers; 112 against 248 with 4). With `numeric(10,2)` the same query takes 5.8 ms at 1,000,000 rows and 20.6 ms at 10,000,000 (2 workers), 4 to 19 times faster than with a bare `numeric`, so declare the precision of money-like columns. What remains: a bare `numeric` column of almost unique values (2,000,000 rows, 6 groups, `round(random() * 100000, 2)`) took the scan with the default 2 workers in 79.6 ms against 61.7 ms for PostgreSQL's plan (29% slower), because every unit holds its own dictionary of values there; the planner now prices the dictionaries that the workers inflate and keeps PostgreSQL's plan with 2, 4 and 8 workers (79, 62 and 62 ms), and still takes the scan without workers. (Measured 2026-10-25 on the 3.4 tip (the 79, 62 and 62 ms on the 3.5 line), PostgreSQL 18.6, an x86-64 host, `work_mem` 4GB unless said, medians of 5 runs; `sift.enable_agg = off` gives PostgreSQL's plan.) (The figures of this paragraph are not from records files; the cases `numeric_sum` and `numeric_sum_records` hold the same shapes with records, at 1,000,000 rows and one process, run d1150b9ef5ae-20261102T231942Z-f7af: a sum of a bare `numeric` by `country` takes 392.8 M instructions against 1,840.9 M for PostgreSQL's plan, and the almost unique values, 6 groups, 622.5 M against 1,925.6 M.)

## Quick start

### Install

PostgreSQL 17, 18 and 19 on Linux (x86-64 and arm64); 17 and 18 on macOS arm64 (Homebrew has no `postgresql@19` yet: build 19 from source). Release packages (x86-64 and arm64/aarch64, any aarch64 Linux: AWS Graviton, Ampere, Azure and Google Arm, Raspberry Pi 4/5) cover PostgreSQL 17, 18 and 19: take the file of your server's major `N` (the `19` packages are built against PGDG's `19~rc1` until it ships 19.0).

| Where | How |
|---|---|
| Debian 12 and 13, Ubuntu 22.04 and 24.04 (PGDG `postgresql-N`) | download `postgresql-N-pg-sift_*_amd64.deb` from the [latest release](https://github.com/brauliobo/pg_sift/releases/latest), then `sudo apt install ./postgresql-N-pg-sift_*_amd64.deb` (`_arm64.deb` on arm64) |
| RHEL, Rocky, Alma 9 and 10 (PGDG `postgresqlN-server`) | download `pg_sift_N-*.rpm` from the latest release, then `sudo dnf install ./pg_sift_N-*.rpm` (`.x86_64.rpm` or `.aarch64.rpm`, the architecture of the server) |
| Amazon Linux 2023 (the distribution's `postgresqlN-server`, N = 17 or 18) | download `pg_sift_N-*.amzn2023.x86_64.rpm` (or `.aarch64.rpm`) from the latest release, then `sudo dnf install ./pg_sift_N-*.amzn2023.*.rpm`; the `el9` package does not work there (the distribution keeps its PostgreSQL files in `/usr/lib64/pgsql`) |
| Arch Linux | the AUR package `pg_sift` (recipe in [packaging/](packaging/)) |
| Docker | `packaging/docker/build.sh 18` builds the image `pg_sift:pg18` on the official `postgres` image: [docs/QUICKSTART.md](docs/QUICKSTART.md), [docs/CONTAINERS.md](docs/CONTAINERS.md) (Docker Compose, CloudNativePG) |
| macOS arm64 | the Homebrew formula below |
| anything else | from source (the first snippet of [In five minutes](#in-five-minutes); needs server headers with `pg_config` on `PATH` and a C compiler; `PG_CONFIG=/path/to/pg_config` picks another installation) |

Then, as a superuser, `CREATE EXTENSION pg_sift` in each database (after an upgrade: `ALTER EXTENSION pg_sift UPDATE`). Details: [docs/USAGE.md](docs/USAGE.md#install), [docs/UPGRADING.md](docs/UPGRADING.md), [docs/SUPPORT.md](docs/SUPPORT.md).

<details>
<summary>macOS with Homebrew</summary>

The [formula](Formula/pg_sift.rb) builds the latest release (`brew install --HEAD` builds `main`) for Homebrew PostgreSQL 17 and 18 on macOS arm64:

```sh
brew tap brauliobo/pg_sift https://github.com/brauliobo/pg_sift.git
brew install brauliobo/pg_sift/pg_sift          # --HEAD for main
brew test brauliobo/pg_sift/pg_sift
"$(brew --prefix postgresql@18)/bin/psql" -d mydb -c 'CREATE EXTENSION pg_sift;'   # a running server, as a superuser; postgresql@17 for 17
```

CI builds the stable formula and the HEAD one, runs `brew style`, `brew audit --strict --online` and `brew test` (a BM25 smoke, an extension upgrade and a `sift_col` aggregate) on macOS arm64 with PostgreSQL 17 and 18, and runs a few regression tests there and the quick tier on Linux arm64; it was also tried by hand on Apple Silicon with 17.11 and 18.6.
`make check` needs bash 4 or newer (`brew install bash`; the stock bash 3.2 stops with a message). A stage the host cannot run (the Perl TAP tests, which need the server's `src/test/perl`; the compatibility tests that build releases older than 3.2.0; the Python 3.12 benchmark generators) is skipped with a message.

</details>

### Try it: analytics

Your queries stay as they are: add the index, and the planner uses it when it can prove the answer equal; otherwise PostgreSQL's own plan runs.

```sql
CREATE TABLE orders (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), created_at timestamp);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, created_at);

SELECT category, count(*), sum(amount) FROM orders
WHERE country = 'DE' AND created_at >= '2025-06-01' GROUP BY category;          -- Custom Scan (SiftAgg) on orders_col
CREATE TABLE events (id bigint, kind text, price numeric(10,2), at timestamp) USING sift_table;   -- the table itself in column format
```

`SET sift.enable_agg = off` gives the plan PostgreSQL has without sift. Types, options and limits: [docs/USAGE.md](docs/USAGE.md#column-indexes-aggregates-and-sift_table).

### Try it: full-text search

Search has its own query syntax (`@@>`, `sift.score`): a search query is written for the `sift` index.

```sql
CREATE TABLE docs (id int PRIMARY KEY, title text, body text, published date, category int);
INSERT INTO docs VALUES
  (1, 'Wireless headphones', 'Battery was great and the support was friendly. The case was squeaky.', '2026-01-10', 1),
  (2, 'Charger', 'Friendly support, no battery, but a sturdy case and clip.', '2026-02-11', 1),
  (3, 'Resume tips', 'A good résumé is short. Cafés near the office help.', '2026-03-12', 2),
  (4, 'Running', 'Runners were running; she runs every day.', '2026-04-13', 2),
  (5, 'Squeaky headphones', 'The headphones were squeaky at night, squeak everywhere. Friendly support though.', '2026-05-14', 1);
CREATE INDEX docs_body ON docs USING sift (body);

SELECT id FROM docs WHERE body @@> 'battery friendly';                     -- all terms: 1, 2
SELECT id, sift.score(ctid) FROM docs WHERE body @@> 'case OR clip' ORDER BY sift.score(ctid) DESC LIMIT 10;   -- BM25 top-10
SELECT count(*) FROM docs WHERE body @@> 'friendly';                        -- 3, from the index
```

Words are `AND`ed; `OR`, `NOT`, `"phrases"`, `batt*`, `sq?eaky`, `/regex/`, `frendly~1` and `^3` boosts combine them: [docs/USAGE.md](docs/USAGE.md#queries).

## Features

- **Column engine** (`USING sift_col`, `USING sift_table`): `count`, `count(DISTINCT ...)`, `sum`, `avg`, `min`, `max` with filters and `GROUP BY` on compressed columns as `Custom Scan (SiftAgg)`; `sift_table` stores the table itself in column format.
- **BM25 search index** (`USING sift`): boolean, phrase, prefix, wildcard, regex and fuzzy queries, exact BM25 ranking, top-k with filters applied inside the scan (the columns of `INCLUDE (...)` are tested before a document is scored, and a join with a small table adds its keys as one more test: [docs/USAGE.md](docs/USAGE.md)), counts from the index, facets, autocomplete.
- **Pre-aggregates, top-N and lookups:** units whose rows are all visible answer from the exact count/sum/min/max stored with them (on by default); `ORDER BY ... LIMIT n` stops after the first rows of the units that can hold them; an equality or `IN` lookup on a key column reads only the units that hold it (`point_columns`, `point_tid_columns`; opt-in per index).
- **Reading a heap table's index without the heap (`track_changes`, on by default):** `track_changes` (default `'auto'`: the primary key; `WITH (track_changes = 'off')` turns it off) keeps a log of the entries an UPDATE, a key change or a DELETE ended and installs two statement triggers on the table (one NOTICE says so), and a grouped count or top-N over rows changed and not yet vacuumed reads 2 heap blocks instead of 1,241 (300,000 rows, the recent 20% updated: 21.8 to 10.7 M instructions, 1.87 to 0.77 ms, 10.09 to 0.41 MB read cold; the top 20 2.3 to 0.8 M). After 1,000,000 updates of 100,000 rows and the vacuums the log is empty and the index 13.0% over a REINDEX (67.7% with `'off'`). It costs writes (100,000 rows: an UPDATE of 5% of the rows +44%, a key change +31%, a DELETE of 2% +35%, CREATE INDEX +187 M instructions) and gains nothing on point reads, index-only scans or after a VACUUM, so it pays when a table is read many times for each write while rows are unvacuumed ([docs/USAGE.md](docs/USAGE.md#reading-without-the-heap-track_changes)).
- **Faster paths:**
  - *Top-N by a float key, or by a column that follows the table:* a float sort key goes through the same min/max and compare kernels as an integer one, so units whose best value cannot pass the n-th are not read: `ORDER BY value DESC LIMIT 10` over 1,000,000 readings 47.5 to 0.80 ms (`ts_top_value`, 60x); the newest 10 rows of a time column with no btree 80.6 to 0.22 ms (`hits_latest_time`, 367x).
  - *The first statement of a new backend:* the choices a statement needs several planner runs for are kept in shared memory (`sift.verdict_cache_kb`, [docs/USAGE.md](docs/USAGE.md)), so a new backend plans the winning form only: a report of 21 CTEs and 16 lateral sub-selects takes 29.1 ms on the first statement against 35.2 ms with the cache off (`verdict_shared_report`); 12 lateral aggregates take 45.9 ms first and 41.7 ms second against 3,507 ms on PostgreSQL (`lateral_flat_plan`, 76x).
- **Plain PostgreSQL:** MVCC, VACUUM, `CREATE INDEX CONCURRENTLY`, partitions, generic WAL (crash and replica safe), `pg_dump`; a query the planner cannot prove equal keeps its normal plan.
- **Vulkan (experimental, opt-in):** `make VULKAN=1` and `SET sift.gpu = auto|on` run grouped and filtered integer counts and sums of a `sift_col` index on a GPU ([below](#vulkan-experimental), [docs/GPU.md](docs/GPU.md)).
- **Ports:** [SQLite](#sqlite) and [MariaDB](#mariadb) run the same search core.

## Compared with

Feature facts checked against each project's own documentation on 2026-10-18; `?` is not verified. Analytics (pg_sift against PostgreSQL, Citus columnar, ClickHouse, Elasticsearch, DuckDB and TimescaleDB) and search (against PG FTS, pg_search, pg_textsearch, rum, pgx-bm25, TIN and Elasticsearch) tables, a note per row, sources and the unverified cells: [docs/COMPARISON.md](docs/COMPARISON.md).

pg_sift is weaker in text analysis (one key per index; case and accent folding, a `keyword` analyzer, `term*` prefixes and the stemmers and stop words of any PostgreSQL text search configuration, `stemmer = 'german'`, are there; n-gram and CJK tokenizers are not) and where its column engine is narrow: joins are accelerated for small tables only, and `sift_table` scans the superblocks that hold a tombstone or a write buffer row instead of reading their pre-aggregates ([where it is weaker](docs/COMPARISON.md#where-pg_sift-is-weaker)).

## SQLite and MariaDB

The search engine runs outside PostgreSQL too, with the same core and the same results; the column engine is PostgreSQL-only ([docs/COL-PORTS.md](docs/COL-PORTS.md)).

### SQLite

The search engine as a loadable extension, for apps that want search without a server (Linux x86-64, SQLite 3.38 or newer): exact BM25, phrase, prefix, wildcard, regex and fuzzy queries, highlights, snippets, facets; 89x, 242x and 10x the throughput of FTS5 on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles). Install, options and numbers: [sqlite/README.md](sqlite/README.md).

```sql
.load ./sift
CREATE VIRTUAL TABLE docs USING sift(title, body);
SELECT rowid, rank FROM docs WHERE docs MATCH 'case OR clip' ORDER BY rank LIMIT 10;   -- BM25 top-10, best first
```

### MariaDB

MariaDB 10.5 to 13.x, with results identical to pg_sift: `libsift_udf.so` and two SQL files, with no storage engine and no server patch; 183x to 794x faster than InnoDB FULLTEXT on top-10 `'a b'`, `'a OR b'` and `'"a b"'` (218,058 articles), with an index 1.7x smaller. Install and guide: [mariadb/README.md](mariadb/README.md).

```sql
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);   -- index, queue, triggers
SELECT sift_count('news', 'power AND plant');
```

## Operations and limits

- **Options:** index options go in `WITH (...)` (`k1`, `b`, `analyzer`, `stemmer`, `positions`, `prefix_index`, `write_buffer_limit`, `merge_fanin`, ...); `merge_fanin` defaults to 4: an index that takes batches keeps 4.6 segments on average instead of the 6.5 of `merge_fanin = 8` and reads 4 to 10% fewer instructions and 13% fewer cold MB, for 37% more merge time and 11% more WAL while it ingests (`ALTER INDEX .. SET (merge_fanin = 8)` merges less often); `sift_table` is tuned with settings (`sift.table_*`), not per table.
- **Maintenance** is on by default: a background worker (`sift.async_maintenance`) promotes write buffers and merges segments, `sift.col_auto_merge` vacuums and merges a `sift_col` index after churn, `sift.table_auto_defrag` clears the dead rows of a `sift_table`; VACUUM clears dead rows as usual.
- **Large `work_mem` and unusual planner costs:** a `seq_page_cost` above `random_page_cost` moves the planner's choice between the scan and the normal plan most; the results are the same as the normal plan's either way.
- **Inspection:** `sift.index_info`, `sift.segments` and `sift.verify`.
- **Replication:** physical replicas work through generic WAL; publishing a `sift_table` under logical replication needs `shared_preload_libraries`.
- **One library:** the BM25 index and the column engine share one library, about twice the size of the search-only build, at a cost of at most 2% of the instructions of an operation[^lib].
- **Platforms:** PostgreSQL 17, 18 or 19 (x86-64 tested; arm64 tested with 18); SQLite port on Linux x86-64 with SQLite 3.38 or newer; MariaDB port on 10.5 to 13.x.
- **Planner:** what is not accelerated keeps the normal plan, and a few cases raise an error that names the rewrite.
- **Which options to touch (about ten of 150):** [docs/TUNING.md](docs/TUNING.md).
- **Column engine types, options and limits:** [docs/USAGE.md](docs/USAGE.md#limits); settings, maintenance and replication: [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations); disk, memory and time of a large index: [docs/SIZING.md](docs/SIZING.md).

[^lib]: Against 2.0.x (`bench/steps/cpu_gate.sh`, user instructions per operation) every cell is within 1.02 except three BM25 cells: the first query of a session (about 1.04 to 1.05; connection pooling amortizes it), a `VACUUM` with nothing to free (about 1.04, on 3.9M instructions) and a ranked query on a one-segment index (1.02 to 1.03).

<!-- sift-reference:start -->
## SQL reference

Schema `sift` has 64 functions, 5 views, 2 tables, 2 operator classes, 1 operator. Signatures, results, the release that added each and a line on what it does are in [docs/REFERENCE.md](docs/REFERENCE.md), generated from the SQL scripts.

- **[Search and ranking](docs/REFERENCE.md#search-and-ranking)**: `@@>`, `facet_counts`, `facet_sql`, `facets`, `field_query`, `highlight`, `index_info`, `json_build_object`, `match`, `match_idx`, `max_score`, `parse_query`, `score`, `sections`, `segment_postings`, `segments`, `text_ops`, `tokenize`, `verify`
- **[Column index](docs/REFERENCE.md#column-index)**: `approx_count_distinct`, `cluster_advice`, `col_*_ops`, `col_clustering`, `col_extents`, `col_index_info`, `col_index_status`, `col_leaves`, `col_maintenance_status`, `col_ops`, `col_sections`, `col_segments`, `col_space`, `col_stamps`, `col_track_old_rows`, `col_tracking`, `col_verify`, `index_advice`, `index_choices`, `key_cut_decision`, `suggest_paths`
- **[Tables](docs/REFERENCE.md#tables)**: `table_get_option`, `table_option_values`, `table_option_values_trg`, `table_options`, `table_paths`, `table_paths_maintained`, `table_reset_option`, `table_segments`, `table_set_option`, `table_set_paths`, `table_space`
- **[Maintenance and workers](docs/REFERENCE.md#maintenance-and-workers)**: `col_maintain`, `col_maintenance_due`, `col_merge`, `col_promote`, `col_refresh`, `col_rollup_counters`, `col_rollup_queue`, `col_rollup_refresh`, `col_rollup_requests`, `col_rollup_state`, `merge`, `promote`, `table_maintain`, `worker_state`, `workers`
- **[Diagnostics and upgrade](docs/REFERENCE.md#diagnostics-and-upgrade)**: `col_rollup_misses`, `explain_why`, `help`, `refusal_actions`, `refusal_slugs`, `stats`, `stats_reset`, `upgrade_commands`, `version`

Settings (`sift.*`) are in the [settings table](docs/USAGE.md#maintenance-and-operations), fixed limits in [docs/LIMITS.md](docs/LIMITS.md).
<!-- sift-reference:end -->

<!-- sift-options:start -->
## Options

173 index options (`WITH (...)`), `sift.*` settings and automatic behaviors, each with its default, class (`default`, `opt-in`, `diagnostic`, `safety`, `deprecated`), measured gain and cost, and off switch, are in [docs/OPTIONS.md](docs/OPTIONS.md); types, ranges and contexts are in the tables of [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations).
<!-- sift-options:end -->

Every NOTICE, WARNING and `VACUUM VERBOSE` line of the library, with the decision it reports, its benefit and trade-off in numbers, and the switch that changes it, is one row of [docs/MESSAGES.md](docs/MESSAGES.md). `CREATE INDEX` and `REINDEX` of a `sift_col` index print one short NOTICE; `SELECT * FROM sift.index_choices('index')` gives the reason, the cost and the command that turns each choice off, and `sift.help('slug')` returns the row of a message.

## Vulkan (experimental)

Opt-in and off by default: build with `make VULKAN=1` and `SET sift.gpu = auto|on` (`auto`: grouped aggregates on indexes of at least `sift.gpu_min_rows`, <!-- gen:guc:sift.gpu_min_rows -->5,000,000<!-- /gen:guc:sift.gpu_min_rows --> rows; `on`: also ungrouped). Grouped and filtered integer and `numeric` counts and sums of a `sift_col` index run on a Vulkan device (clean all-visible units on the device, the rest on the CPU in the same query); a missing or failed device falls back to the CPU. The compressed chunks stay on the device, so use persistent connections (each backend pays about 0.2 s to open it). Not eligible: floats, `min`/`max`, `count(DISTINCT)`, text filters, more than one key, `sift_table`. Measurements and limits: [docs/GPU.md](docs/GPU.md#when-it-helps).

## Docs

- [docs/USAGE.md](docs/USAGE.md): install, queries, ranking, options, settings, maintenance, replication, limits.
- [docs/SIZING.md](docs/SIZING.md): disk, memory and time of a large index.
- [docs/COLUMN-STORAGE.md](docs/COLUMN-STORAGE.md): what a `sift_col` index stores besides the columns (point directories, sorted arrays, key tables, dictionaries): what each is for, its default, how to turn it off and one measured figure.
- [docs/COMPARISON.md](docs/COMPARISON.md): feature tables against other search and analytics systems, with sources and the date checked.
- [docs/CONTAINMENT.md](docs/CONTAINMENT.md): range and network containment from `sift_col`, its numbers and what it does not serve.
- [docs/FORMATS.md](docs/FORMATS.md): the on-disk format numbers (metapage minor, segment formats, flags).
- [docs/UPGRADING.md](docs/UPGRADING.md): the upgrade preflight (`scripts/upgrade-check.sh`), `ALTER EXTENSION UPDATE`, `pg_upgrade` to a newer PostgreSQL major and going back to an older release.
- [docs/GPU.md](docs/GPU.md): the experimental, opt-in Vulkan aggregate path.
- [docs/COL-PORTS.md](docs/COL-PORTS.md): the column engine on SQLite and MariaDB; [docs/SAFETY.md](docs/SAFETY.md): memory safety in the C code.

## Security and license

Tests: `make check`, and `make check-full` with sanitizers, soak, fuzz and mutants ([CONTRIBUTING.md](CONTRIBUTING.md)). Report crashes, wrong results and index corruption privately as in [SECURITY.md](SECURITY.md).
pg_sift is released under the [PostgreSQL License](LICENSE); third-party notices are in [NOTICE](NOTICE).
