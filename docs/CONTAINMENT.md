# Range and network containment: what `sift_col` serves and what it does not

A `sift_col` index on a range (`int4range`, `int8range`, `daterange`, `tsrange`, `tstzrange`) or an `inet` / `cidr` column keeps integer lanes after the image of the value
(the start and end of the set; the node of the block in the tree of prefixes), so that a containment test is a comparison of integers with zones, not PostgreSQL's operator
on each value.
This page states the cost and the limits.

## Numbers

One backend, warm medians, 1,000,000 random rows, a table of its own (`bench/cases/contain_range.sql`, `bench/cases/contain_inet.sql`; `bench/bench cases --cases contain_range,contain_inet`).
`count(*)` of the rows that pass the test; the GiST is the same rows with a GiST index and no `sift_col` index.

| shape | heap | `sift_col` | GiST | `sift_col` index | GiST index |
|---|---|---|---|---|---|
| `r @> 5000000` (int4range of up to 5,000 values) | 57.0 ms | 2.10 ms | 0.128 ms | 13.7 MB | 50.0 MB |
| `ip <<= '20.7.0.0/16'` (IPv4) | 30.6 ms | 1.22 ms | 0.582 ms | 9.1 MB | 33.7 MB |

## The matrix

User instructions of one backend (`perf stat`, warm, 20 runs, median ms) on random rows with an address and a 128-character note beside the range. `sift_col (r, ip)` is one table
with the cut chosen by default (`cut`) and the same table indexed with `key_cut = 'off'` (the superblock cut); the GiST has one index on each column of its own copy.
`bench/cases/contain_key_cut.sql` reproduces the range at 3,000,000 rows.

| rows | query | `sift_col`, cut | `sift_col`, no cut | GiST |
|---|---|---|---|---|
| 1,000,000 | `r @> 5000000` | 0.42 M, 0.143 ms | 9.7 M, 1.01 ms | 0.82 M, 0.138 ms |
| 1,000,000 | `r && int4range(5000000, 5100000)` | 0.48 M, 0.128 ms | 9.8 M, 1.30 ms | 16.2 M, 1.41 ms |
| 1,000,000 | `ip <<= '20.7.0.0/16'` | 8.1 M, 1.06 ms | 8.9 M, 1.00 ms | 5.5 M, 0.54 ms |
| 1,000,000 | `ip >>= '20.7.1.5/32'` | 31.4 M, 2.75 ms | 32.2 M, 3.16 ms | 0.23 M, 0.084 ms |
| 3,000,000 | `r @> 5000000` | 0.67 M, 0.148 ms | 27.9 M, 6.56 ms | 1.64 M, 0.183 ms |
| 3,000,000 | `r && int4range(5000000, 5100000)` | 0.87 M, 0.180 ms | 28.3 M, 6.88 ms | 47.3 M, 3.24 ms |
| 3,000,000 | `ip <<= '20.7.0.0/16'` | 23.8 M, 3.73 ms | 25.6 M, 7.31 ms | 15.8 M, 1.02 ms |
| 3,000,000 | `ip >>= '20.7.1.5/32'` | 93.9 M, 8.17 ms | 95.7 M, 11.30 ms | 0.26 M, 0.073 ms |

The times of the `sift_col` columns were taken on a loaded host; the instructions do not depend on the load.

| index | 1,000,000 rows | 3,000,000 rows |
|---|---|---|
| `sift_col (r, ip)`, cut by the range | 20.2 MB | 62.5 MB |
| `sift_col (r, ip)`, no cut | 21.6 MB | 66.5 MB |
| GiST on `r` + GiST on `ip` | 47.7 + 32.1 MB | 123.7 + 95.8 MB |

Build (3,000,000 rows, one process): 20.4 G instructions without the cut, 32.4 G with it (+58.8%).

## Where a GiST is still faster

- A containment of a range on rows in no order of the range, with no cut: `sift_col` tests the zones of every unit, and on random ranges nearly every unit overlaps the constant, so
  it reads the lanes of most of them (9.7 and 27.9 million instructions against 0.8 and 1.6 million for the GiST). The default cut by the start of the range removes this
  (0.42 and 0.67 million, ahead of the GiST on instructions and equal on time). `CREATE INDEX` and `REINDEX` choose it when a sample of the table shows the ranges scattered over
  the units and the index does not grow (`sift.key_cut_decision`, the `auto(key-cut-range-lane)` row of [OPTIONS.md](OPTIONS.md)). It needs 64 units, and it is not chosen when another leaf
  of the index would lose its zones: a sequential `id`, or a date that follows the table or drifts over it (an equality on it keeps fewer than all the units), keeps the superblock cut.
  The address column of the matrix has no zones to lose (random addresses), so the cut changes its instructions by 1% to 9% fewer.
- A network. `<<=` and `<<` over random addresses read the lane of every row (8.9 and 25.6 million instructions, 1.6 times the GiST's); `>>=`, `>>` and `&&` (the blocks that contain an address)
  walk the ancestors of the address with a scalar loop over the rows (32 and 96 million instructions) where a GiST goes down one path (0.23 and 0.26 million, 0.07 to 0.08 ms): keep the GiST for them.

## What is not served

- **Rows are not returned from the index.** Only the aggregate pushdown (`SiftAgg`: `count(*)`, `GROUP BY`, `min` / `max`, `count(DISTINCT)` over the rows that pass the test) uses the lanes.
  `SELECT * FROM t WHERE r @> x` is not an index scan of the access method: `sift_col` has no bitmap operator class for ranges or networks, so the planner takes the heap plan or a GiST, if there is one.
  Keep the GiST for the queries that return rows; `sift_col` replaces it for the counts and the groups.
- **Types that keep the image only:** multiranges, `numrange`, ranges of a user type, `point`, `box`, `circle`, `polygon`, `macaddr`. Their containment is PostgreSQL's operator on each distinct value, as before.
- **Operators that keep the image:** `=`, `IN`, ordering of a range or a network and `<->`. `|v| >= 2^62` of a range bound (a timestamp `infinity`, an `int8` near its limits) and an IPv6 mask of 62 bits or more are
  decided by PostgreSQL's operator on the candidate rows, so a column full of them prunes nothing.
- **A larger index than the image alone:** the lanes add about 6 MB (range) and 3 MB (inet) per 1,000,000 rows of random values. They are written for every range and network column; there is no option to leave them out.
- **REINDEX:** an index built by 3.5 or older with such a column does not match its column layout; `REINDEX` rebuilds it ([UPGRADING.md](UPGRADING.md)).
