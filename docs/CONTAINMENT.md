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

## Where a GiST is still faster

- The rows are in no order of the column. A GiST finds the few pages of a containment; `sift_col` tests the zones of every unit, and on random ranges nearly every unit
  overlaps the constant, so it reads the lanes of most of them. This is the 2.10 ms above.
- When the units are cut by the start of the range, the zones prune: on 3,000,000 random ranges beside an address and a 128-character note (`bench/cases/contain_key_cut.sql`) `r @> 5000000`
  reads 0.66 million instructions and takes 0.164 ms (4.22 ms without the cut), against 0.256 ms for a GiST of 129.7 MB (the `sift_col` index is 61.2 MB with the cut, 68.8 MB without).
  `CREATE INDEX` and `REINDEX` choose the cut by the start lane themselves when a sample of the table shows the ranges scattered over the units and the index does not grow
  (`sift.key_cut_decision`, the `auto(key-cut-range-lane)` row of [OPTIONS.md](OPTIONS.md)); the build costs 59.6% more instructions. A sequential `id` or another column that follows the order of
  the table blocks the cut, as for any key.
- A cut by the start of a range gains 4% on `ip <<= net` (25.6 to 24.6 million instructions): the cut is by the range lane. A cut by the node of an inet helps `<<=` and `<<` (a range of nodes). A test for the blocks that contain an address (`>>=`, `>>`, `&&`) reads the units whose nodes can lie on the way to it,
  which is most of them.

## What is not served

- **Rows are not returned from the index.** Only the aggregate pushdown (`SiftAgg`: `count(*)`, `GROUP BY`, `min` / `max`, `count(DISTINCT)` over the rows that pass the test) uses the lanes.
  `SELECT * FROM t WHERE r @> x` is not an index scan of the access method: `sift_col` has no bitmap operator class for ranges or networks, so the planner takes the heap plan or a GiST, if there is one.
  Keep the GiST for the queries that return rows; `sift_col` replaces it for the counts and the groups.
- **Types that keep the image only:** multiranges, `numrange`, ranges of a user type, `point`, `box`, `circle`, `polygon`, `macaddr`. Their containment is PostgreSQL's operator on each distinct value, as before.
- **Operators that keep the image:** `=`, `IN`, ordering of a range or a network and `<->`. `|v| >= 2^62` of a range bound (a timestamp `infinity`, an `int8` near its limits) and an IPv6 mask of 62 bits or more are
  decided by PostgreSQL's operator on the candidate rows, so a column full of them prunes nothing.
- **A larger index than the image alone:** the lanes add about 6 MB (range) and 3 MB (inet) per 1,000,000 rows of random values. They are written for every range and network column; there is no option to leave them out.
- **REINDEX:** an index built by 3.5 or older with such a column does not match its column layout; `REINDEX` rebuilds it ([UPGRADING.md](UPGRADING.md)).
