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
