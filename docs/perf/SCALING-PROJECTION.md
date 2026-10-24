# Projection to 100k, 1M and 10M keys

What one request costs as the number of keys grows, from the slopes and constants measured at 1x and 10x (3,160 keys, 31.7M rows) and by the scaling gate
(`bench/scaling`, [SCALING.md](../SCALING.md)). **Arithmetic from measured constants, not a measurement beyond 10x.** The request is an aggregate over 51 scattered keys
(`count(*)` and the average of a json float), one backend, warm cache, `work_mem = 4GB`.

## Assumptions

| quantity | value | where it comes from |
|---|---|---|
| rows per key | 10,000, about 1 KB a row, 2,000 rows a unit: 5 units a key | the measured data sets (1x: 316 keys, 3.3M rows) |
| zone pass (every unit tested, none read) | 48 ns a unit, linear | 0.748 ms for 15,389 units at 10x, 0.089 ms for 1,582 at 1x (build with the word-level zones) |
| units read by the 51 keys, clustered by key | about 350 (about 150 with a one-year window) | 348 / 152 units at 10x in pure key order |
| decode of one unit | 50 µs with the json average, 13 µs for `count(*)` | 17.1 ms / 152 units; 4.5 ms / 348 units |
| planning | 0.5 ms on one index; 1.5 ms on a range-partitioned table (the partitions the request touches, about 30 µs each) | 10x layouts: 1.4-1.8 ms for 43-48 partitions |
| a layout in arrival order (keys interleaved) | units read grow as keys^0.65 | 1,210 -> 5,408 units from 1x to 10x for 51 keys, one decade only |
| partitions | by range of the key, 2,000 units (400 keys) each | one level |

## Per request, ms

| keys | units | one index, clustered: zone pass + decode | total | range partitions: partitions | total | one index, arrival order: units read | total |
|---|---|---|---|---|---|---|---|
| 3,160 (10x, measured) | 15.8k | 0.8 + 23 | **24** | 8 | 25 (measured 19 on 256) | 5.4k | 272 |
| 100,000 | 500k | 24 + 23 | **48** | 250 | **29** | 51k | 2,600 |
| 1,000,000 | 5M | 240 + 23 | **264** | 2,500 | **29** | 228k | 11,600 |
| 10,000,000 | 50M | 2,400 + 23 | **2,400** | 25,000 | **29** | 1.0M | 53,000 |

## Reading

- On a table clustered by key the cost of a request is the 23 ms of decoding the units of its 51 keys plus a zone pass that is **linear in the units that exist** (48 ns each).
  The pass is negligible up to about 100k keys (24 ms) and dominates from 1M (240 ms, 2.4 s at 10M). The gate asserts the pass slope (<= 1.15) and the flat classes (<= 0.5).
- Range partitioning of the key makes the request independent of the size of the table: pruning leaves the partitions of the 51 keys, so the zone pass covers 51 x 2,000 units (4.9 ms)
  and planning follows the partitions touched. One level is enough: 25,000 partitions at 10M keys cost nothing for a request. They do cost a statement over all of them (planning
  about 6.5 µs a partition, 160 ms at 25,000) and locks: a statement touching every partition needs `max_locks_per_transaction` of at least the partitions plus their indexes
  (a `DROP SCHEMA` over 4,096 partitions failed at the default of 64 slots a connection).
- **The layout is the larger risk.** A heap in arrival order, where a key's rows are spread over the table, reads units in proportion to keys^0.65 (measured over one decade):
  seconds at 100k keys, a minute at 10M, whatever the index. It must be clustered by key (an ordered load, `CLUSTER`, or one partition per key range with ordered partitions).
- Not covered: the cost of the write path and of maintenance at these sizes, the cost model's choice of plan (the cost agent), statements that touch every key, and a cold cache.

## Measured: the size of the request, 10x data (3,160 keys, 31.7M rows)

The same replay of 237 captured analytic statements (every third one, 13-14 per family), one client, `work_mem = 4GB`, warm, execution time summed in seconds; a statement that
reaches the 60 s timeout counts 60 s, so the plain figure is a lower bound where it timed out. Natural = the planner's own choice with sift; the keys of the request are spread over the table.

| family | 32 keys: plain / natural | 500 keys: plain / natural | 3,160 keys (all): plain / natural |
|---|---|---|---|
| json average per group, by source | 11.7 / **3.6** (11 of 13 statements on SiftAgg) | 195 / **36.5** (13 of 13) | 729 (12 timeouts) / **135** (11 of 13, 2 timeouts) |
| the same, overall only | 9.9 / **4.5** (10 of 14) | 187 / **74** (13 of 14) | 778 (12) / **191** (11 of 14) |
| json documents with nested lists | 46.3 / **38.9** (2 of 14) | 580 (2) / **76** (13 of 14) | 785 (12) / **168** (13 of 14) |
| two-level scans over nested lists | 67.1 / 67.2 (1 of 13) | 737 (10) / **106** (12 of 13) | 741 (12) / **186** (12 of 13) |

- From 500 keys up the natural plan is the forced SiftAgg plan in 12-13 of 13-14 statements of every family; the plain engine times out in up to 12 of 13.
- Natural cost per key falls with the size of the request: the first family takes 0.11 s a key at 32 keys, 0.073 at 500 and 0.043 at 3,160 (the fixed cost of a statement is shared; the scan is linear in the
  units the request reads, so the per-key cost cannot fall below the decode of a key's units).
- At 32 keys the last two families are not faster than plain (1-2 statements on SiftAgg): a small request does not pay for the scan of these shapes yet.
