# What the pre-aggregate defaults of a column cost

One table of 200,000 rows and 16 columns (an id, eight low-cardinality integers, an integer of 500 values, three dates, a timestamptz, an integer of 1,024 values, a numeric) plus a 150-byte text pad, one index per variant on PostgreSQL 18; bytes from `sift.col_sections` (section `preagg`).
The pad compresses to almost nothing, so the shares below are larger than on wide rows; the documented shares on wide rows are in the options table.

| Structure | Bytes added | Share of this index | Share documented on wide rows |
|---|---:|---:|---:|
| Cube histograms (the default cube) | 90,168 | 8.4% | |
| Min/max per group (`cube_minmax`) | 46,544 | 4.3% | +0.17% |
| Calendar cells (4 date and timestamp columns) | 100,384 | 9.4% | +1.0% per column |
| Superunits (`superunit` 16) | about 27,000 | 2.0% | +1.0% |
| All of them | 279,544 | 20.9% | |

Each default passes the rule of 3.3 (at most 3% growth) alone on wide rows; stacked they do not, and the calendar cells grow with the number of date columns. No rule that drops one of them per column without losing a measured gain was found (the gains depend on the queries a column is grouped by), so none is chosen per column: `sift.index_advice` shows which dimensions and pairs qualify, and the options replace the defaults.

## Ranking the cube's dimensions (`preagg = 'auto'`)

The cube holds 16 dimensions and the default gives them to the columns in their order; a column with more than 256 values in a unit has no histogram there, so on an index of more plain columns than that, the columns past the 16th are scanned even when they are the ones queries group by. One table of 300,000 rows and 26 index columns (an id, a bigint measure, 16 integers of 100,003 values, then 8 integers of 3 to 200 values) plus a 40 MB heap; `preagg = 'default'` against `preagg = 'auto'`, median of 9 runs on PostgreSQL 18, SiftAgg execution time.

| Query | default | auto | Rows read, default / auto |
|---|---:|---:|---:|
| `GROUP BY` a low-cardinality column with `count(*), sum(v)` | 2.74 ms | 0.86 ms | 300,000 / 3,040 |
| `GROUP BY` the last column, `count(*)` | 1.78 ms | 0.28 ms | 300,000 / 0 |
| `WHERE` the 22nd column, `count(*), sum(v)` | 1.33 ms | 0.31 ms | 300,000 / 0 |
| `WHERE` one column `GROUP BY` another (a pair) | 0.96 ms | 1.12 ms | 300,000 / 300,000 |

The pair is answered by neither: it needs a rollup of both (`rollup_dims`). The cost, by size of the table (median of 5; the ranking reads at most 2,048 blocks, so it stops growing with the table):

| Rows | Index, default | Index, auto | Growth | `REINDEX`, default / auto | `CREATE INDEX`, default / auto |
|---:|---:|---:|---:|---:|---:|
| 40,000 | 1,155,072 | 1,236,992 | 7.1% | 50.7 / 56.6 ms | 51.7 / 65.7 ms |
| 80,000 | 2,908,160 | 3,170,304 | 9.0% | 59.8 / 71.8 ms | 66.0 / 89.4 ms |
| 160,000 | 6,144,000 | 6,496,256 | 5.7% | 112.3 / 121.1 ms | 108.2 / 142.5 ms |
| 300,000 | 11,034,624 | 11,722,752 | 6.2% | 174.2 / 212.6 ms | 166.8 / 240.4 ms |

The growth is the histograms of the 8 columns that now have one (the cost the first 16 columns of any index already pay: 8.4% of the index above), and the rebuild takes 8% to 22% longer for them; the ranking itself took 23 to 25 ms of the `CREATE INDEX` at 300,000 rows (timed alone). That is more than the 3% growth a default may add, so the ranking is opt-in: `preagg = 'auto'` on `CREATE INDEX`, or `ALTER INDEX ... SET (preagg = 'auto')` and `REINDEX`; the NOTICE of the cube's left-out columns names it.

## The rollup of the rows (`rollup_dims = 'auto'`)

`sift.index_advice` recommends a pair of dimensions whose combinations fit a rollup of the rows of a unit; `rollup_dims = 'auto'` writes one at `CREATE INDEX` from a sample of the table (the pair of the columns that fit the cube's histograms with the fewest combinations, `auto: a, b` in the option). The same 300,000-row table with an index of 10 columns (an id, a bigint, 8 integers of 3 to 200 values; the pair chosen was two columns of 10 and 20 values), PostgreSQL 18, median of 9 runs:

| Query | no rollup | auto | rows read |
|---|---:|---:|---:|
| `GROUP BY` both columns, `count(*), sum(v)` | 3.50 ms | 0.66 ms | 300,000 / 0 |
| `WHERE` both columns, `count(*), sum(v)` | 1.70 ms | 0.73 ms | 300,000 / 0 |
| `WHERE` one of them `GROUP BY` a third | 1.56 ms | 1.69 ms | not helped |

Cost by size of the table (median of 5; `REINDEX` keeps the recorded pair, so it has no sampling):

| Rows | `REINDEX`, none / pair | `CREATE INDEX`, none / auto | Index, none / pair | `sift.col_merge` of 10% more rows, none / pair |
|---:|---:|---:|---:|---:|
| 40,000 | 44.0 / 39.6 ms | 39.2 / 51.2 ms | 147,456 / 147,456 | 2.5 / 3.0 ms |
| 80,000 | 57.0 / 49.5 ms | 45.8 / 87.5 ms | 286,720 / 286,720 | 71.6 / 67.0 ms |
| 160,000 | 103.5 / 87.5 ms | 80.1 / 126.8 ms | 548,864 / 548,864 | 155.7 / 143.3 ms |
| 300,000 | 150.9 / 154.8 ms | 149.9 / 187.5 ms | 868,352 / 942,080 (+8.5%) | 206.7 / 230.9 ms |

A 20-cell rollup costs nothing measurable to build or merge (the differences are inside the run-to-run noise, which is about 10% here; inserts and updates go to the write buffer and do not see it). What grows is the index at 300,000 rows (+8.5% here, +11% in the query run: the cells hold the sums of 8 measures per unit) and `CREATE INDEX`, by the two passes over at most 2,048 blocks the choice reads (12 to 47 ms). The index growth is more than 3%, so the choice is opt-in: `rollup_dims = 'auto'` on `CREATE INDEX`, or `ALTER INDEX ... SET (rollup_dims = 'auto')` and `REINDEX`. The pair is the cheapest that fits, not the one the queries use: the columns the statements of a workload group by are what `sift.index_advice` and the per-column budget of a later change look at.

## What the API workload uses of them (156 statements, `events` table of 353,029 rows)

The `events` index of a web API (13 columns, three of them `jsonb` read through `paths`, `key_columns = auto`; PostgreSQL 18, `work_mem` 4 GB, the 156 analytics statements of the captured requests, 236 SiftAgg nodes, read from `EXPLAIN (ANALYZE)`), with each structure switched off and the index rebuilt; the total is the best of two passes over the 156 statements.

| Variant | Index | Pre-aggregate bytes | Units from pre-aggregates | Total of 156 statements |
|---|---:|---:|---:|---:|
| Defaults | 8,806,400 | 715,552 | 0 of 7,646 | 19.05 s |
| `cube_minmax = false` | 8,577,024 (-2.6%) | 551,568 | 0 | 19.19 s |
| `superunit = 0` | 8,577,024 (-2.6%) | 644,928 | 0 | 19.40 s |
| `calendar_columns = 'off'` | 8,830,976 | 715,400 | 0 | 19.66 s |
| `preagg = 'off'` | 8,101,888 (-8.0%) | 83,040 | 0 | 19.25 s |

What the 236 nodes do with each column: every node filters `deleted_at IS NULL` and an `event_date` range (or two); `event_date` is also grouped in 71, `owner_id` in 99 (and its `count(DISTINCT)` in 82), `origin_id` in 59, `id` in 37; `kind_a` and `kind_b` in 8; the `jsonb` paths are grouped in 129 (`doc_a`) and 89 (`doc_b`) and filtered in 117; `item_id` and `country` are read by none. The aggregates are `count` (660), `sum` (280), `avg` (194), `count(DISTINCT)` (158) and `max` (8); no `min`. No node groups by plain columns alone: they group by `jsonb` paths and the output of lookup joins.

So none of the structures a budget could rank answers a unit of this workload, and no per-column rule derived from it would change an answer or a time: the pre-aggregates are 8.1% of the index and 0 of 7,646 units use them, but the same columns without the two filters are answered from the cube (a path-free copy of the index: `GROUP BY owner_id` in 171 of 171 units, 0.35 ms against 4.4 ms scanned). The filters are what stop it:

- `deleted_at IS NULL`: 655 of 353,029 rows are deleted, scattered, so every unit holds a few and no unit can answer from its histogram or sums (5 of 171 units do in the real index, 0 of 13 in the copy).
- A `event_date` range: 7,349 distinct days over 24 years, more than the 256 groups of a histogram, and the calendar cells of the dates are not written for an index with `jsonb` paths (`calendar_columns` needs a schema without paths); a path-free copy, which does write them, answers 0 of 13 units of a month range too.

Dropping `cube_minmax` and `superunit` would save 5.2% of this index, but they answer the filterless shapes of the indexes that have them (the 171 of 171 above), and nothing in a workload of statements always filtered by a soft-delete column and a date range shows which columns will want them, so no budget was set. What this workload needs is not a structure to rank but an answer for a filter that most rows pass: the cube of a unit minus the few rows `deleted_at IS NULL` rejects, and the date range from cells that also exist beside `jsonb` paths. Those change the executor and the format, not a default.

## Calendar cells beside `jsonb` paths (`calendar_columns`)

The same index and statements as above, with `calendar_columns = 'all'` (the 3 date and timestamp columns), against the index without cells: the index grows from 8,806,400 to 9,625,600 bytes (+9.3%; 9,871,360, +12.1%, in an earlier build of it), so the cells are named, not the default, beside paths. What they answer on the date shapes of the statements (units of the index answered from the pre-aggregates, `EXPLAIN (ANALYZE)`; before, none):

| Shape | Units | From cells | Rows scanned |
|---|---:|---:|---:|
| `count(*)`, range aligned to months | 123 | 123 | 0 (253,491 before) |
| `count(*)`, range with day bounds | 122 | 57 | 133,689 (251,433) |
| `count(*)`, OR of two ranges | 148 | 57 | 187,790 (305,272) |
| `GROUP BY date_trunc('month', d)`, a range | 122 | 15 | 220,323 (220,323) |
| the same `AND deleted_at IS NULL` | 122 | 0 | 251,433 |

The 156 statements: 0 of 7,646 units from the pre-aggregates before, 0 after. Each of them also filters `deleted_at IS NULL`, which no cell answers (the next change), and groups by paths and lookup outputs. The total of the 156 did not move beyond the noise of a host at load 40 (28.8 to 29.4 s before, 26.9 to 29.8 s after): a unit of this index holds about 2,000 rows, as cheap to scan as its cells are to read. A range with day bounds is cut by the month cell of every unit that has a row in the boundary month, so only the units written when the table was young are answered whole; ranges aligned to months are answered everywhere.

## Paths read from the documents of the web API (`jsonb` columns, 156 statements)

The index of the web API with the paths written by hand, against `paths` chosen by the sample at `CREATE INDEX` (the three `jsonb` columns: a map of 10 float keys read by key and by `jsonb_each`, an array of objects, a document with a list of objects below one key). The sampler reads 2,000 documents from 16 stretches; statements 156, `work_mem` 4 GB, alternating pairs as above, median of 3.

| Paths | Index | Statements with SiftAgg | SiftAgg nodes | Total of 156 |
|---|---:|---:|---:|---:|
| by hand | 8,830,976 | 156 | 237 | 17.1 s |
| 10 named keys (the sampler before) | 12,058,624 (+36.6%) | 12 | 28 | 478 s |
| wildcard for the map, every key of the objects | 11,354,112 (+28.6%) | 116 | 138 | 221 s |
| wildcard, free text left out (this change) | 9,314,304 (+5.5%) | 116 | 138 | 241 s |
| the same with the one key the sample cannot see | 9,330,688 (+5.7%) | 156 | 237 | 20.0 s (hand 26.6 s in the same pairs) |

What each rule did: a map of ten keys of one type read with `jsonb_each` is served only by a wildcard (`unnest_leaf`), so the sampler now takes a wildcard from 5 keys instead of 33; a text key whose values average over 64 bytes (a comment, +2.1 MB, 23% of the index) is no filter or group key and is left out; an array that is the whole document is a group of the column's own path; and arrays inside the elements of a group are left out (the build refused `one level of arrays and objects`). What the sample cannot do: the 40 statements that read a key which no document of the table holds (a flag the application filters on and has never written) fall to the heap, 85.2 s of the 241 s; write the spec by hand (the sampler's answer plus that key, the last row) and the 156 are answered again.

## Calendar cells and the size of the table (slope)

The same index with and without `calendar_columns = 'all'`, on prefixes of the table (the first n rows of the heap, so a unit keeps the dates it has in the whole table; 24 years of dates in every prefix), `work_mem` 4 GB, one session per run, the two indexes switched with `indisvalid` and timed in alternating pairs (the order flips each round, the first pair is dropped, median of 9) at a host load of 20 to 56. Statements: the OR of two ranges of a year, aligned to months (`aligned`) or to days (`days`), `GROUP BY date_trunc('month', d)` over the day-bounded ranges (`month`), and the same `AND deleted_at IS NULL`. Units / from cells; ms without cells / with cells:

| Rows | Units | `aligned` | `days` | `month` | `month` + `deleted_at` |
|---:|---:|---|---|---|---|
| 25,000 | 13 | 13: 0.55 / 0.52 | 4: 0.56 / 0.72 | 0: 0.74 / 0.91 | 0: 0.79 / 0.81 |
| 46,000 | 22 | 22: 0.78 / 0.73 | 3: 0.87 / 0.96 | 0: 0.90 / 1.11 | 0: 1.07 / 1.07 |
| 85,000 | 41 | 41: 0.98 / 1.03 | 16: 1.13 / 1.27 | 9: 1.35 / 1.70 | 0: 1.49 / 1.43 |
| 179,000 | 84 | 84: 1.67 / 2.01 | 37: 1.60 / 2.31 | 22: 1.80 / 2.92 | 1: 2.13 / 2.57 |
| 353,029 | 150 | 150: 2.26 / 2.64 | 57: 2.26 / 3.17 | 22: 2.78 / 3.87 | 1: 3.44 / 3.59 |
| log-log slope | | 0.54 / 0.64 | 0.51 / 0.57 | 0.50 / 0.58 | 0.54 / 0.57 |

The slope does not flatten: with cells the time grows as fast as without them (0.57 to 0.64 against 0.50 to 0.54, where the fixed cost of a statement bends both below 1), and every shape is 0% to 40% slower with them. A unit holds about 2,000 rows, whatever the size of the table, and the rows of a unit are scattered over the 24 years (the correlation of the date with the heap order is 0.04), so a unit has about as many cells as rows in its recent years, a cell holds one to a few rows, and reading the cells costs what scanning the date of the unit costs (7 ns a row, 15 us a unit). More units mean more of them to answer from cells, not a cheaper unit: the table size cannot make cells pay while the dates are scattered inside the units. The same 353,029 rows written in the order of the date (every unit holds a few days): 68 of 69 units of the aligned range are answered by the zone alone with or without cells (1.01 / 1.29 ms), the day-bounded count 0.83 / 1.01 ms, and the month grouping, which a zone cannot answer, 70 of 70 units from cells, 2.60 -> 2.04 ms (-22%); with `deleted_at IS NULL` 2 of 70, 2.42 / 2.60 ms. So cells stay named, not a default: they pay for a group-by-month over a table stored in date order, and nothing else measured.
