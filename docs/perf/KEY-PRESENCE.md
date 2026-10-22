# Per-unit key presence: built as an opt-in option (October 2026)

The idea: store in each unit's sidecar the distinct values of a column, so that a flat `count(DISTINCT key)` is the union of the units' sets and reads no key
chunk. It is the reloption `key_presence` (column format minor 15, [COL.md](../COL.md) section 2.16), off by default, for top-level `int2`, `int4` and `date`
columns. A unit keeps the keys as a bitmap from its smallest key to its largest, only when that takes at most 3% of the unit's bytes. The unit must be exact and every filter
of the query must hold for the whole of it; any other unit is scanned as before. Key lists (`k IN (..)`) are not served: the `set_columns` summary of
[COL.md](../COL.md) section 2.13 prunes nearly as many units as the exact set, and a second structure would add under 1.1x.

## Measured

PostgreSQL 18.6, one backend, no parallel workers, median of 9 runs, the index with the option and the same table without it interleaved under a lock that keeps other
timed jobs off the machine (`docs/results/col-key-presence.tsv`). Table `events(id, parent_id, ts, kind, amount)`, 6,313 distinct `parent_id`.

| rows (units), rows per unit | query | normal plan (ms) | units scanned (ms) | with the option (ms) | gain over scanning |
|---|---|---|---|---|---|
| 1,000,000 (123), 8,130 | `count(DISTINCT parent_id)` | 314 | 16.1 | 1.11 | **14.5x** |
| | with `count(*), sum(amount)` | 563 | 16.5 | 0.97 | **17.1x** |
| | with a `ts` window that holds for every unit | 336 | 13.3 | 1.42 | **9.4x** |
| | `count(*), sum(amount)` (no distinct) | 145 | 0.84 | 0.92 | 0.9x |
| 200,000 (10), 20,000 | `count(DISTINCT parent_id)` | 43.6 | 2.55 | 0.42 | 6.0x |
| | with `count(*), sum(amount)` | 85.6 | 2.67 | 0.28 | 9.6x |
| | with a `ts` window | 30.4 | 2.40 | 0.67 | 3.6x |

Index size: at 8,130 rows per unit the section is 99,648 bytes, **2.5%** of the index (3,825,664 bytes without the option, 3,956,736 with it: the file is 3.43% larger because it
grows by 8 KB pages). At 20,000 rows per unit the section is 8,256 bytes, **1.0%** (the file of the 10-unit index grows 4.0%, all page rounding). The share falls as a unit holds
more rows. User cycles of the 1,000,000-row query: 2.9 million with the option, 2.3 million for `count(*), sum` alone, 51.7 million scanning the units. What remains per unit is the
read of the unit's pre-aggregate and its bitmap, about 10 microseconds, so the gain is the cost of a key chunk over that.

**Near-unique keys get no section.** The bitmap takes one bit for every integer between a unit's smallest and largest key. With 100,000 values spread uniformly (`p2`), a unit of
8,130 rows holds 7,800 of them and its bitmap is 11% of its bytes, over the 3% limit: the unit has no section and is scanned, so the option costs nothing there and gains nothing.

## Why gated runs matter

A first set of runs of this implementation, taken while other jobs were running on the same machine, gave 4.6x (`count(DISTINCT)` alone), 5.1x (with `count(*), sum`) and 7.8x (with a
window) at 8,130 rows per unit, and would have failed a 5x bar. Repeated alone under the lock, with the cycle counts above as a cross-check (17.8x fewer user cycles), the same index gave
14.5x, 17.1x and 9.4x. The timings of an aggregate that takes a millisecond move with the load; compare the two indexes in one interleaved run, and check the cycles.

## What was measured before it was built

The numbers that decided the density: at 1,020 rows per unit (a wide row) the gain was 2.0 to 2.7x and the sets were 1.7 to 8.2% of the index, a miss on both counts; narrow rows hold 5,000
to 50,000 rows per unit, where one set is amortised over many more rows. A key list is useful only where a unit holds few of the column's values: `parent_id IN (1 / 10 / 100 keys)`
read 6.2 / 6.5 / 8.2 ms on zones alone and 5.7 / 7.5 / 9.2 ms with `set_columns`; exact presence keeps 54.2 / 120.9 / 123 of 123 units, and for 6,313 values one key is in 44% of the units or
more, so no summary can skip them.

A soft-delete filter (`deleted_at IS NULL`) with 1.6% of the rows deleted, spread evenly, covers no unit, so the units are scanned; per-key row counts would repair that at about 2.4x the
bytes and are not built. A `GROUP BY` of an independent dimension would need a set per (unit, value): +33% of the index for 2 values, +117% for 8.

## The two-level shape under an outer `count(DISTINCT id)` and `count(DISTINCT parent_id)`

200,000 facts, 442,754 elements, scanned. Of the node's user cycles the distinct is 21.5% (`dist_bits_slow` 16.2%, `dist_chunk_for` 5.3%), `arm_chunk`
18.7%, `cell_into` 12.6%; the presence summary can remove at most that share (1.0 to 1.2x), and it is not used there. An inline fast path for the bitmap test and set in
`dist_add` removes 3.6% of the instructions and moves its cycles into `cell_into`: the time is in the miss on the bitmap word of the group, not in the call, so it is not built.
