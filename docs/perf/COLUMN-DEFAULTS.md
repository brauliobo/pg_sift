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
