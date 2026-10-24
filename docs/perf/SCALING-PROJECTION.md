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
| a layout in arrival order (keys interleaved) | units read = 40% of the units (the false positives of a bloom filter over 51 keys); measured 35% at 10x. Not a power law: [measured below](#measured-the-arrival-order-layout) | 1,210 -> 5,408 units from 1x to 10x for 51 keys (76% -> 35%), and the small model below |
| partitions | by range of the key, 2,000 units (400 keys) each | one level |

## Per request, ms

| keys | units | one index, clustered: zone pass + decode | total | range partitions: partitions | total | one index, arrival order: units read | total |
|---|---|---|---|---|---|---|---|
| 3,160 (10x, measured) | 15.8k | 0.8 + 23 | **24** | 8 | 25 (measured 19 on 256) | 5.4k | 272 |
| 100,000 | 500k | 24 + 23 | **48** | 250 | **29** | 200k | 10,000 |
| 1,000,000 | 5M | 240 + 23 | **264** | 2,500 | **29** | 2M | 100,000 |
| 10,000,000 | 50M | 2,400 + 23 | **2,400** | 25,000 | **29** | 20M | 1,000,000 |

## Reading

- On a table clustered by key the cost of a request is the 23 ms of decoding the units of its 51 keys plus a zone pass that is **linear in the units that exist** (48 ns each).
  The pass is negligible up to about 100k keys (24 ms) and dominates from 1M (240 ms, 2.4 s at 10M). The gate asserts the pass slope (<= 1.15) and the flat classes (<= 0.5).
- Range partitioning of the key makes the request independent of the size of the table: pruning leaves the partitions of the 51 keys, so the zone pass covers 51 x 2,000 units (4.9 ms)
  and planning follows the partitions touched. One level is enough: 25,000 partitions at 10M keys cost nothing for a request. They do cost a statement over all of them (planning
  about 6.5 µs a partition, 160 ms at 25,000) and locks: a statement touching every partition needs `max_locks_per_transaction` of at least the partitions plus their indexes
  (a `DROP SCHEMA` over 4,096 partitions failed at the default of 64 slots a connection).
- **The layout is the larger risk.** A heap in arrival order, where a key's rows are spread over the table, reads a fraction of the units that does not fall with the keys (the last column is
  40% of the units at 50 us a unit, arithmetic; the earlier keys^0.65 extrapolation is withdrawn): ten seconds at 100k keys, minutes at 1M, whatever the index. It must be clustered by key (an ordered load, `CLUSTER`, or one partition per key range with ordered partitions).
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

## Measured: the partitioned layout, 100k keys of padded rows

`bench/scaling/cases/keyparts.py`, in the gate: 20 rows of 200 bytes a key, 100 keys to a range partition with one sift_col index each, 6,250 to 100,000 keys (63 to 1,000 partitions, 125k to 2M rows),
a request of 51 keys spread over the key range (51 partitions touched at every size), 8-page units (the gate flavour, units are 1/32 of the shipped size), `work_mem = 4GB`, one backend, median of 5, ms of planning + execution.

| keys | partitions | `GROUP BY` of the request | with a one-year window | `count(*)` | the same on one table with one index |
|---|---|---|---|---|---|
| 6,250 | 63 | 3.7 | 3.1 | 2.5 | 0.4 |
| 12,500 | 125 | 3.2 | 3.5 | 3.2 | 0.2 |
| 25,000 | 250 | 3.3 | 3.6 | 3.3 | 0.3 |
| 50,000 | 500 | 3.5 | 4.3 | 3.6 | 0.4 |
| 100,000 | 1,000 | 4.7 | 4.8 | 4.1 | 0.6 |
| slope (fit, last segment) | | 0.08, 0.41 | 0.16, 0.17 | 0.15, 0.21 | 0.14-0.27 |

- The request is flat in the size of the table: 16x the keys and 16x the partitions cost 1.3x. Planning is 2.1-2.9 ms of it, 45 us a touched partition (the projection above assumed 30 us), the scan 1-2 ms; 306 of 357 units
  of the touched partitions are pruned and 51 read, at every size.
- At this size one index is faster (0.4-0.6 ms): its zone pass walks 6,200 units at 100k keys (0.3 ms). The partitions pay off once that linear pass passes their 2-3 ms of planning, about 50k units at the 48 ns
  of the pass, which is the 100k-key row of the table above (500k units of the shipped size).

## Measured: the arrival-order layout

`bench/scaling/cases/arrival.py`, in the gate: 1,562 to 25,000 keys of 16 rows, rows in arrival order (a key appears at its place of the timeline and its rows arrive in the next 4% of it), 8-page units, a
request of 51 keys, `sum` of a column that is not a dimension of the cube. Units read (the plan's `Units`) against the keys, by what prunes the key:

| keys | units | min/max zones only | exact key summary (units of at most 32 keys) | bloom filter (units of 56 keys) | units of the bloom table |
|---|---|---|---|---|---|
| 1,562 | 1,042 | 1,039 | 567 | 380 | 447 |
| 3,125 | 2,084 | 2,078 | 695 | 559 | 893 |
| 6,250 | 4,167 | 4,155 | 757 | 1,060 | 1,786 |
| 12,500 | 8,334 | 8,310 | 784 | 1,878 | 3,572 |
| 25,000 | 16,667 | 16,624 | 800 | 3,335 | 7,143 |
| slope of the units read (fit, last) | | 1.00, 1.00 | **0.12, 0.03** | 0.80, 0.83 | |

- **Min/max zones cannot prune interleaved keys:** every unit holds keys from the whole range of the units created in the last 4% of the timeline, so a 51-key request reads all the units, linear.
- **An exact key summary** (`key_columns`, at most 32 keys a unit; or a key list or bitmap, which needs 3% of the unit's bytes, `presence_pct`) bounds the units read by the rows the request asks for (51 x 16 = 816 units): the
  slope falls from 1 to 0.03 as the table outgrows that bound. The time does not follow: every unit still pays for the test of its summary (about 2 us a unit here), so the time slope stays 0.8-1.0.
- **A bloom filter** (more than 32 keys a unit, 10 bits a key) keeps the false positives of 51 probes, 1 - 0.99^51 = 40% of the units: 85% -> 47% of the units here, on its way to 40% and linear from there.
- So keys^0.65 is the transition between reading everything and the floor of the pruning, not a power law, and no exponent below 1 holds beyond it. The 35% of the units read at 10x in arrival order is the size of the bloom
  floor; whether the real index holds blooms or key lists there (the share of `presence_pct` of a wide unit) was not checked on the 10x data. Remedies: clustered or partitioned by key (above), or exact key lists in every unit.
