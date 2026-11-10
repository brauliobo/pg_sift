# What a `sift_col` index stores besides the columns

A `sift_col` index keeps every column in units of a few thousand rows. Beside the column data it can keep small extra structures, each of which makes one kind of query cheaper and costs some bytes, build time or both. Most are chosen by `CREATE INDEX` and `REINDEX` from the data (the column says `auto` in `pg_class.reloptions`, and a NOTICE names the choice); a few are opt-in because the gain does not pay for the growth. This page lists them by what they are for. Every entry has the same lines:

- **What**: the query it helps, in one sentence.
- **Default**: what it is when you set nothing, and why.
- **Off**: how to turn it off (a build or a setting).
- **Measured**: one absolute figure, with the benchmark case (`bench/cases.tsv`), or the regression test it comes from. A figure is one backend, warm, on the rows named.
- **Message**: the NOTICE or HINT of `CREATE INDEX` that names it (the id in [docs/MESSAGES.md](MESSAGES.md)).

Every setting and option has its full row in [docs/OPTIONS.md](OPTIONS.md); the byte layout of each structure, by column format minor, is in [docs/FORMATS.md](FORMATS.md). `ALTER INDEX .. SET (option = ..)` applies to the segments written afterwards; a `REINDEX` (or the next merge) rewrites the older ones, and the older ones keep answering meanwhile.

## Point lookups

An equality, an `IN` list or a short range on a column of many distinct values.

- **`point_columns`** (opt-in).
  - **What**: an equality, `IN` list or short range on a column of many distinct keys reads only the units that hold the rows, which the heap then checks again.
  - **Default**: off. An equality on a unique id reads 1.58 M instructions with the key filter and 0.78 M with the directory (2.03x), for +58% of the index and +3.4% of the build; that growth, over a quarter of the index, is more than the gain.
  - **Off**: `ALTER INDEX .. RESET (point_columns)`; new segments get no directory, older ones keep theirs until a merge or `REINDEX`.
  - **Measured**: integer equality 0.119 to 0.020 ms on 200,000 rows (a b-tree 0.006 ms), case `point_in_100`.
  - **Message**: `point-tid-key-cut`.
- **`point_tid_columns`** (opt-in; chosen by the default formula, below).
  - **What**: the same lookup, with the directory listing the heap addresses of the rows, so no unit is read.
  - **Default**: off. An equality on a unique id reads 1.67 M instructions with the key filter and 0.66 M with the addresses (2.51x), for +57.6% of the index and +3.6% of the build. Its segments are column format minor 37 or more, which only 3.7 and later read.
  - **Off**: `ALTER INDEX .. RESET (point_tid_columns)`; a column kept in `point_columns` then has the rows form. `sift.col_format_minor <= 14` writes no addresses.
  - **Measured**: integer equality 0.013 ms, flat from 26,700 to 1,000,000 rows (a b-tree 0.006 ms); a 100-id `IN` 45.7 to 0.25 ms on 1,000,000 rows; case `point_in_100`.
  - **Message**: `point-directory-auto`, `point-tid-explicit`, `point-tid-key-cut`.
- **Point directory by the default formula** (automatic).
  - **What**: when neither option is set, the build writes `point_tid_columns` for an integer column of nearly unique values that does not follow the order of the table, when the directory gains more than it grows the index and the build.
  - **Default**: on, written as `point_tid_columns = 'auto: col'`: a lookup of 0.17 M instructions plus 5 a row becomes 0.2 M (9.5 times fewer), for 16.5 bytes a row more and a build 21% longer.
  - **Off**: `ALTER INDEX .. SET (point_tid_columns = 'off')` and `REINDEX`.
  - **Measured**: an equality on a random unique int8 of a 7-column index, 400,000 rows: 1.9 M instructions without the directory, 0.2 M with it (a b-tree 0.2 M); test `col_point_auto`.
  - **Message**: `point-directory-auto`.
- **Point directory in the aggregate scan** (automatic).
  - **What**: the aggregate scan asks the directory for the rows of a short `IN` list, an equality, a short range or the keys of an inner or semi lookup join, and reads only the units that hold them.
  - **Default**: on, costed: the planner prices the units the directory leaves, and keeps it off where it costs more than the zones. No format change, no growth.
  - **Off**: `sift.enable_agg = off` turns the aggregate scan off; `RESET (point_columns)` and `RESET (point_tid_columns)` with a `REINDEX` drop the directory.
  - **Measured**: `point_keys_agg` on 1,000,000 rows, 80.9 to 2.85 ms.

## Order and top-N

`ORDER BY .. LIMIT n`, with or without a filter, an offset or a range.

- **`order_columns`** (opt-in).
  - **What**: each unit keeps a sorted run of the column, so `ORDER BY col LIMIT n` reads the first rows of each unit only, and a `LIMIT` of up to 32 over clean, all-visible units reads no unit.
  - **Default**: off. An `ORDER BY` a date `LIMIT 100` reads 18.3 M instructions without the runs and 16.9 M with them (1.08x), for +8.3% of the index and +2.5% of the build.
  - **Off**: `ALTER INDEX .. RESET (order_columns)`; new units get no run. `sift.col_format_minor <= 12` writes no runs, 13 to 14 writes runs without heads.
  - **Measured**: `ORDER BY` an integer `LIMIT 100` on 1,000,000 rows, 37.9 ms by the normal plan, 3.5 ms without runs, 1.4 ms with them; case `topn_sorted_runs`.
- **`sift.topn_runs`** (setting, on).
  - **What**: lets the top-N scan read the stored runs.
  - **Default**: on; it costs nothing without `order_columns`.
  - **Off**: `SET sift.topn_runs = off` (session, role, database or `postgresql.conf`): whole units are read, the stored runs stay.
  - **Measured**: `ORDER BY k LIMIT 100` with `order_columns`, 1,000,000 rows, 3.5 ms off and 1.4 ms on; case `topn_sorted_runs`.
- **Heads of the runs** (automatic with `order_columns`, column format minor 18).
  - **What**: the first and last keys of a run (up to 32 at each end, with the heap addresses of their rows) answer `ORDER BY .. LIMIT n` (n at most 32, one sort key, no condition, join or aggregate) without reading the unit.
  - **Default**: on with `order_columns`: 2 to 3 times faster on an unfiltered `LIMIT` up to 32, for 0.6% to 2.4% of the index and write cost inside noise.
  - **Off**: `sift.col_format_minor = 14` writes runs without heads, or `sift.topn_runs = off` / `sift.enable_topn = off`.
  - **Measured**: `ORDER BY k LIMIT 10` 0.128 to 0.051 ms on 800,000 rows (a b-tree 0.010 ms).
- **Top-N run probes** (automatic).
  - **What**: a top-N over a table with `order_columns` reads the sort key of a unit's rows only for the rows it keeps, and sizes its candidate buffers by the unit.
  - **Default**: on; the same rows and units are read with less work a scan. No format change, no growth.
  - **Off**: `SET sift.topn_runs = off`, or `SET sift.enable_topn = off`.
  - **Measured**: 40 scans `ORDER BY` a key column `LIMIT 10..49` on 1,000,000 orders, 48 to 24.8 ms; case `topn_run_probes`.
- **`order_array_columns`** (default `auto`, column format minor 38).
  - **What**: the sorted keys and heap addresses of a whole segment answer a range of a column whose values are scattered over the heap (up to 16,384 rows) without reading a unit, and `ORDER BY col LIMIT n` (also with an offset) merges the arrays of the segments and fetches n rows of the heap, whatever the number of units.
  - **Default**: on where it pays: 100,000 rows or more, 1,000 or more distinct values, correlation under 0.99, not a column that cuts the units. A narrow range reads 18.4 M instructions without it and 2.4 M with it, for 21% more index and 22% more build.
  - **Off**: `ALTER INDEX .. SET (order_array_columns = 'off')` and `REINDEX`; `RESET` decides again at the next `REINDEX`.
  - **Measured**: a range of 790 of 800,000 random int4 rows, 18.4 M to 2.4 M instructions; test `col_range_order`.
  - **Message**: `order-array-auto`, `order-array-listed`.
- **`order_array_group`** (default `auto`, column format minor 38).
  - **What**: a second order inside each value of a group column answers `WHERE group = x ORDER BY col LIMIT n` and `min/max(col) WHERE group = x` from n entries of the group in each segment, the shape a b-tree on `(group, col)` serves. An index whose first two columns are the group (`NOT NULL`) and the column, with whole chunks (`order_layout = 'whole'`), also gives `ORDER BY group, col` (both ascending or both descending) and `WHERE group = x ORDER BY col` as an index-only scan that reads the groups one after the other, where a table scan and a Sort were: the scan is priced for the open of each group, so a table of about 4 rows a group or fewer keeps the sort.
  - **Default**: on for a date, timestamp or integer array that passes the formula: a group of 400 values costs +13.2% of the index and +5.7% of the build. No column with fewer than 100 values or fewer than 3 rows a value gets one.
  - **Off**: `order_array_group = 'off'` and `REINDEX`; the plain order answers meanwhile.
  - **Measured**: `WHERE grp = x ORDER BY ts DESC LIMIT 10`, 500,000 rows, 3.34 M to 0.42 M instructions (a b-tree on `(grp, ts)` 0.27 M); test `col_gorder_autogroup`.
  - **Message**: `order-array-auto`.
- **`order_layout`** (default `auto`, column format minor 41).
  - **What**: the sorted array of a column keeps each chunk of 512 entries whole (the key of every row) or compact (the heap address and the key to within a bucket; the exact key is read from the row where a scan needs it).
  - **Default**: `auto`: a chunk is compact where that is at least a fifth smaller (3.6 bytes a row for a microsecond timestamp against 6.5), so the array is never larger than the whole order; the reads are as fast.
  - **Off**: `ALTER INDEX .. SET (order_layout = 'whole')` and `REINDEX`; `RESET` returns to auto.
  - **Measured**: `OFFSET 100,000` of a timestamp, 1,000,000 rows: whole 3.08 ms (37.0 M instructions), compact 3.27 ms (36.3 M), a b-tree 4.10 ms; the index 15.02 MB whole, 12.12 MB compact; case `order_layout_offset`.
  - **Message**: `order-layout`.

## Counts and keys

`count(DISTINCT x)`, `count(*)`, sums and minima by a key, and an equality that most units cannot hold.

- **`key_columns`** (default `auto`, column format minors 10 and 15).
  - **What**: each unit keeps the exact keys, a bloom filter or a bitmap of a column: `count(DISTINCT col)` over whole units reads no key chunk, and an equality or `IN` skips the units that lack the key.
  - **Default**: on for the foreign keys and `*_id` columns whose lists fit within 3% of a unit: +1.1% to +1.4% of the index, and `count(DISTINCT)` of a foreign key 2.0 to 8.6 times faster.
  - **Off**: `SET (key_columns = 'off')` then `REINDEX`; `RESET` returns to auto.
  - **Measured**: `count(DISTINCT fk)` over whole units 16.1 to 1.11 ms on 1,000,000 rows (123 units); case `key_presence_distinct`.
  - **Message**: `key-lists`, `keys-no-summary`, `keys-no-list`.
- **`key_table`** (default on, column format minor 22).
  - **What**: a segment keeps one record for each distinct key of a `key_columns` leaf with more than 256 values in a unit, with the rows, sums, minima and maxima: `count`, `sum`, `min`, `max` `WHERE key = x`, an `IN`, a range or `GROUP BY` the key read one record of each segment, not its units.
  - **Default**: on; a table is written only when all of them take at most 3% of the segment, so the growth is under 3% by construction.
  - **Off**: `ALTER INDEX .. SET (key_table = off)`, then `REINDEX` or the next merge.
  - **Measured**: `count, sum, min, max WHERE key = x` over 900,000 rows, 5.20 to 0.107 ms (300 keys); case `key_table_point`.
- **`count_columns`** (default on, column format minor 24).
  - **What**: the dictionary of a text column also holds the rows of each string, so `count(*)` of an equality, a prefix (`LIKE 'abc%'`) or a range in the C collation is read from two places of the dictionary.
  - **Default**: on, because it pays: 38 times fewer instructions for 0.17% more index and 0.44% more write cost.
  - **Off**: `ALTER INDEX .. SET (count_columns = 'off')`, then `REINDEX` or the next merge.
  - **Measured**: `count(*) WHERE txt LIKE 'k2a%'` (4,012 of 1,000,000 rows) 5.3 to 0.14 ms; test `col_dict_count`.
- **`hll_columns`** (opt-in, column format minor 20).
  - **What**: each unit keeps a HyperLogLog sketch of the column, so `sift.approx_count_distinct(col)` merges the sketches of the units instead of hashing every row.
  - **Default**: off: +23% of the index for four columns of a million rows and a build +70%. `count(DISTINCT)` stays exact whatever this is.
  - **Off**: `ALTER INDEX .. RESET (hll_columns)`; `sift.col_format_minor <= 14` writes none.
  - **Measured**: 5.4 ms against 63 ms for the same aggregate by the normal plan, 1,600,000 rows; case `approx_distinct_sketch`.

## Cutting the units by a key

Units that follow a declared key, so a lookup or a range of the key reads the units of its range.

- **`key_cut`** (default `auto`, column format minor 17).
  - **What**: the units are cut by the range of a key column, not by the order of the heap, so an equality, a list or a range of the key reads the few units of its range.
  - **Default**: on where an equality on the key would read at least 8 times the units its rows fill and at least 64 units, and the index is predicted from a sample to grow by at most 3%; a build 41% longer and a promotion 21% longer pay for it.
  - **Off**: `ALTER INDEX .. SET (key_cut = 'off')` and `REINDEX` (or `CREATE INDEX .. WITH (key_cut = 'off')`).
  - **Measured**: `count/sum` of one key 5.06 to 0.35 ms on 1,500,000 rows, 419 units; case `keycut_customer_agg`.
  - **Message**: `key-cut-chosen`, `key-cut-advised`, `key-order`.
- **`key_cut_rows`** (deprecated in 4.3, removed in 4.4).
  - **What**: the rows a key-cut unit aims at. It is accepted and ignored: the units aim at the rows of an average superblock, which keeps the number of units and the planner's per-unit costs.
  - **Why**: at 100,000 rows 256 rows is +69.0% of the index and 4,096 is -9.4%, with no query gain; case `keycut_customer_agg`.
  - **Off**: `RESET (key_cut_rows)`. `CREATE INDEX` and `REINDEX` say so once (`key-cut-rows-deprecated`).
- **Second key of the cut** (automatic).
  - **What**: when the cut is chosen by default and the index has a date or timestamp leaf, the rows of a key are sorted by that leaf too, so a window of dates on the key skips the units outside it.
  - **Default**: on; the index shrinks by 20.9% and the build pays 11% more.
  - **Off**: `ALTER INDEX .. SET (key_cut = 'key')` and `REINDEX`.
  - **Measured**: 3,000,000 rows, 6,000 keys: the aggregate over a 90-day window 536.3 to 498.2 M instructions; test `col_key_cut_second`.
- **Range and network lanes** (automatic).
  - **What**: a range column or an `inet` / `cidr` column can cut the units by its first lane, so `@>`, `<@`, `&&`, `<<`, `>>`, `-|-` of a range and `<<=`, `>>=` of a network read the few units whose lane meets the constant.
  - **Default**: on: 42 times fewer instructions, and the index shrinks by 6.0% for a build 59% longer; a niche class of columns.
  - **Off**: `ALTER INDEX .. SET (key_cut = 'off')` and `REINDEX`.
  - **Measured**: `r @> x` on 3,000,000 random int4range rows, 27.9 M instructions without the cut and 0.67 M with it; case `contain_key_cut`.
- **Cut by a date** (automatic).
  - **What**: for a date or timestamp column the gain of the cut against its growth and build decides, so a window of dates, an equality on a date and a group by day read the units of their range on a table of any size.
  - **Default**: on: 34 times fewer instructions for a predicted +44.5% of the index and 67% more build (3,000,000 rows of 56 bytes).
  - **Off**: `ALTER INDEX .. SET (key_cut = 'off')` and `REINDEX`.
  - **Measured**: `count(*)` of a day on 3,000,000 rows scattered over 1,000 days, 25.4 M instructions without the cut and 0.7 M with it; test `col_key_cut_auto`.
- **`sift.vis_page_cache`** (setting, on).
  - **What**: the rows of a heap page that a scan reads twice (the units of a key-cut index list rows of the same page) are checked for visibility once.
  - **Default**: on: about 48 bytes for each heap page visited (at most 1,048,576 pages) and nothing written; no gain at 100,000 rows.
  - **Off**: `SET sift.vis_page_cache = off`; the page is checked each time.
  - **Measured**: a key-cut range query on 100,000 rows, 0.20 ms off and 0.23 ms on (within noise).

## Dictionaries and text

Text and other columns stored as a dictionary of distinct values.

- **`dict_lookup`** (default `auto`, column format minor 14).
  - **What**: an equality, `IN` or prefix test reads a dictionary of 1,024 strings or more in place, from blocks of 64 strings, without parsing it whole.
  - **Default**: `auto`: the order of the strings is 4 bytes a string (+10% to +70% of a one-column index), so only `point_columns`, `key_columns` and `return_columns` get it.
  - **Off**: `ALTER INDEX .. SET (dict_lookup = off)` stores no order; `sift.col_format_minor <= 13` writes neither the blocks nor the order.
  - **Measured**: first query of a new backend, 300,000 rows: uuid equality 12.1 to 0.43 ms, text equality 29.3 to 0.54 ms; case `dict_equality`.
- **`sift.col_dict_shared_cache_kb`** (setting, 64 MB).
  - **What**: a new backend, a pooled connection or a parallel worker reads the parsed dictionary that another backend parsed, in place, instead of parsing it again.
  - **Default**: 64 MB of shared memory, allocated by the first backend that keeps a dictionary of 128 kB or more; it holds the dictionaries of about 1,000,000 distinct values.
  - **Off**: `SET sift.col_dict_shared_cache_kb = 0` (superuser; `postgresql.conf` for all backends).
  - **Measured**: first statement of a new backend, `ip = x/51` over 1,000,000 rows, 286 M instructions to 2.5 M once another backend has kept the dictionary; test `col_dict_shared_size`.
- **Trigram signature of a dictionary** (automatic, column format minor 29).
  - **What**: a substring test (`LIKE '%part%'`) of 3 bytes or more on a text column whose dictionary no longer fits the cache reads only the blocks of 64 strings whose trigram filter holds the part.
  - **Default**: on for a dictionary that does not fit the cache: +0.33% of the index on 6 columns, +9.4% for the URL column alone.
  - **Off**: `SET sift.col_format_minor = 14` writes leaves without the signature.
  - **Measured**: `LIKE '%part%'` over 3,000,000 URLs, a part in 42 rows, 1,336 to 944 M instructions; test `col_text_tsig`.
- **LZ4 for text** (automatic).
  - **What**: strings stored inline (over 255 bytes, or more distinct text than the dictionary holds) are wrapped with LZ4 instead of zstd where it pays: a substring, equality or `GROUP BY` reads 1.1 to 3.4 times fewer instructions a row.
  - **Default**: decided per chunk, from the cost of unwrapping against the growth: sentences take LZ4 (+23.7% of the index on the measured table), titles keep what they have.
  - **Off**: `compression = 'auto; col=zstd'` and `REINDEX` (the NOTICE gives the line); `sift.col_format_minor = 14` writes no LZ4 dictionary blocks.
  - **Measured**: substring of a sentence 2,371 to 683 instructions a row, 3,000,000 rows (a heap scan 4,286); test `col_text_scanwrap`.
  - **Message**: `text-compression`.

## `sift_table`

A `sift_table` writes the same structures when it promotes and vacuums, not in a statement that could report them, so there is no NOTICE; `sift.table_segments(table)` gives the bytes and format of its segments.

- **`sift.table_dict_lookup`** (setting, on).
  - **What**: a test for a string reads a dictionary of 1,024 strings or more in place (the same blocks as `dict_lookup`).
  - **Default**: automatic where the orders of a segment (4 bytes a string) take at most 25% of it: 20.9 times fewer instructions on the first statement of a backend, for 22.9% more segment and 3.0% more build.
  - **Off**: `SET sift.table_dict_lookup = off`; new segments return to the compact form, older ones keep the order until a merge or `VACUUM FULL`.
  - **Measured**: first statement of a new backend on 300,000 distinct 33-character strings: prefix test 154.6 M to 7.4 M instructions, equality 137.6 M to 3.4 M; tests `tbl_topn_order` and `tbl_fetch_dict`.
- **`sift.table_key_table`** (setting, on).
  - **What**: the key table of `key_table`, for the foreign key and `*_id` columns of a `sift_table`.
  - **Default**: on, within 3% of a segment by construction.
  - **Off**: `SET sift.table_key_table = off`, then `VACUUM FULL` or the next merge.
  - **Measured**: `count, sum, min, max WHERE key = x` over 900,000 rows, 0.577 to 0.068 ms (300 keys); test `tbl_key_table`.
- **Key lists of a table** (automatic, column format minor 15).
  - **What**: `count(DISTINCT col)` of an `int2`, `int4`, `int8` or `date` foreign key or `*_id` column that is not unique reads the key list or bitmap of each clean, all-visible unit, not its rows.
  - **Default**: on: 40 times fewer instructions on `count(DISTINCT fk)`, growth under 0.6%, write cost 0.0% to 1.6% of the writer's instructions.
  - **Off**: `SET sift.col_format_minor = 14`, or `SET sift.agg_preagg = off`.
  - **Measured**: `count(DISTINCT fk)` of 500,000 rows, 2.10 to 0.053 ms; test `tbl_lists_runs`.
- **Run heads of a table** (automatic, column format minor 18).
  - **What**: `ORDER BY ts LIMIT n` (n at most 32) of a timestamp column the rows follow reads the first and last keys of each clean, all-visible unit, not the unit.
  - **Default**: on: 3.7 times fewer instructions at `LIMIT 10 DESC`, growth 0.55% to 1.42%.
  - **Off**: `SET sift.col_format_minor = 12`, or `SET sift.topn_runs = off`, or `SET sift.enable_topn = off`.
  - **Measured**: `ORDER BY ts DESC LIMIT 10` of 500,000 rows, 0.140 to 0.038 ms; test `tbl_lists_runs`.
- **Batched appends** (automatic).
  - **What**: the rows a transaction inserts into the index after its first go to the write buffer a page at a time, one WAL record for a run of rows instead of one for each row.
  - **Default**: on: a standby replays one record a page, not one a row.
  - **Off**: no setting; a one-row transaction, an `ON CONFLICT` statement and a row of more than a page are written at once.
  - **Measured**: `COPY` of 50,000 rows into a table with a 6-column index: WAL 245 to 186 bytes a row, standby replay 6,205 to 3,390 instructions a row; test `col_wbuf_batch`.

## Column format

`sift.col_format_minor` is the newest column format the segments written use. The default writes every structure above; a lower value keeps the segments readable by an older library (6 for a 3.2 library, 4 for 3.1) and writes the structures of the later minors nowhere. The minors, the library that first reads each and the one that refuses it are in [docs/FORMATS.md](FORMATS.md) section 4; [docs/UPGRADING.md](UPGRADING.md) covers the change of a server.
