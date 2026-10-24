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
