# Per-unit key presence: measured, not built (October 2026)

The idea: for a high-cardinality integer or text column (`parent_id`), store in each unit's sidecar the exact set of its distinct values, so that
`count(DISTINCT key)` is the union of the units' sets and a key list skips the units whose set meets none of its keys. It was prototyped (sizes and
union times of six encodings, scalar C) and the node's costs were measured on the engine; nothing is in the format. The bar for 3.3: opt-in, at least 3x
on a flat `count(DISTINCT key)` **and** on key-list queries, under 3% of the index at 7,700 rows per unit or more. It clears the first and the third
for dense keys and misses the second, so it is not built.

Setup: PostgreSQL 18.6, 32 shared CPUs at a load average of 24 to 47 (absolute milliseconds are noisy, ratios are the point), median of 3, scanned
plan (`SiftAgg`) against the normal plan, table `events(id, parent_id, ts, kind, amount, pad)` with `pad` setting the rows per unit (a unit is 256 heap
blocks). "Predicted" is measured parts added up: the node's time with every unit answered from pre-aggregates (`count(*), sum(amount)`), 2 us per
unit to read one more sidecar blob, and the measured time to union the sets.

## 1,000 rows per unit is the wrong density (why the rule is at least 7,700)

At 1,020 rows per unit (a wide row) the same prediction gave 2.0 to 2.7x over the scanned plan and 1.7 to 8.2% of the index (Zipf 5.3%, batched 2.7%,
uniform 8.2% at 200,000 rows): a miss on both counts. Narrow rows hold 5,000 to 50,000 rows per unit, where one set is amortised over many more rows.

## 1,000,000 rows, 123 units of 8,130 rows (index 6.03 MB)

| shape | zones only (ms) | with the summary | note |
|---|---|---|---|
| `count(*), sum(amount)` (floor) | 1.8 | | all units from pre-aggregates |
| `count(DISTINCT parent_id)`, 6,313 values, Zipf | 16.0 | 2.1 predicted, **7.8x** | the sets are 98 KB, **1.6%** of the index |
| `count(DISTINCT p2)`, 100,000 values, uniform | 10.5 | no section | the smallest exact set is 672 KB, 11.1% (a unit holds 7,800 keys for 8,130 rows) |
| `parent_id IN (1 / 10 / 100 keys)` | 6.2 / 6.5 / 8.2 | `set_columns`: 5.7 / 7.5 / 9.2 | exact presence keeps 54.2 / 120.9 / 123 of 123 units |
| `p2 IN (1 / 10 / 100 keys)` | 2.7 / 5.5 / 4.3 | `set_columns`: 1.3 / 3.2 / 5.1 | exact presence keeps 8.9 / 68.0 / 122.5 units, the bloom filters 9.7 / 72.1 / 122.8 |

A key list is useful only where a unit holds few of the column's values, and there the `set_columns` summary of 3.3 ([COL.md](../COL.md) section 2.13)
already prunes nearly as many units as the exact set (within 4 of 123). A second structure would add under 1.1x to a gain of at most 2.0x, below the bar.
For 6,313 values every unit holds about 2,200 of them and one key is in 44% of the units or more: no summary can skip them.

## 200,000 rows, 6,313 values, by rows per unit (flat `count(DISTINCT parent_id)`)

| rows per unit (units) | predicted gain over the scanned plan | smallest exact sets, share of the index |
|---|---|---|
| 8,000 (25) | 2.1 to 6.1x | 0.7 to 2.5% |
| 20,000 (10) | 3.8 to 6.9x | 0.4 to 1.0% |
| 40,000 (5) | 4.8 to 9.5x | 0.1 to 0.5% |

(Zipf, batched and uniform keys; the 200,000-row floor is 0.2 to 0.7 ms, so the low ends are load noise. With 100,000 values the share is 6.0 to 13.5%.)
The gain comes from replacing a scan of 7 to 15 ns per row by a union of 800-byte bitmaps; it needs a unit whose filters hold for every row. A
soft-delete filter (`deleted_at IS NULL`) with 1.6% of the rows deleted, spread evenly, covers no unit; per-key row counts repair that at about 2.4x the
bytes. A `GROUP BY` of an independent dimension needs a set per (unit, value): +33% of the index for 2 values, +117% for 8.

## The two-level shape under an outer `count(DISTINCT id)` and `count(DISTINCT parent_id)`

200,000 facts, 442,754 elements, scanned. Of the node's user cycles the distinct is 21.5% (`dist_bits_slow` 16.2%, `dist_chunk_for` 5.3%), `arm_chunk`
18.7%, `cell_into` 12.6%; the presence summary can remove at most that share (1.0 to 1.2x). Row-local cell ids landed in 3.2 (scanned 549 to 156 ms at
200,000 facts, 2.35x fewer cycles). An inline fast path for the bitmap test and set in `dist_add` removes 3.6% of the instructions (918.5 to 885.5
million per run) and moves its cycles into `cell_into` (34.1% and 34.6% of the profile for the pair): the time is in the miss on the bitmap word of the
group, not in the call, so it is not built.
