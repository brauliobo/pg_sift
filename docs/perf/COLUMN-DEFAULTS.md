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
