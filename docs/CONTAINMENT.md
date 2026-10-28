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

## Rows, not counts

A plain `sift_col` index (the default class, `CREATE INDEX .. USING sift_col (r, ip)`) returns the rows of `WHERE r @> x`, `r && range`, `r = x`, `ip <<= net`, `ip = x` and the other containment
operators of a range or a network with an index scan of the access method, from the same integer lanes the counts use; a statement with an aggregate keeps the aggregate pushdown ([COL-SCAN.md](COL-SCAN.md)).
The planner chooses the index by its cost. `SELECT * FROM t WHERE ..` returning the rows, instructions and median ms of one backend, planning included, no parallel workers, each index kind on its own copy
of the rows (`id`, `r`, `ip`, `u` uuid, a 128-character note); the plain index is `sift_col (r, ip, u)`:

| 3,000,000 rows | plain `sift_col` (111.7 MB) | b-tree (63.1 + 90.3 + 90.3 MB) | GiST (95.8 + 123.7 MB) |
|---|---|---|---|
| `r @> 5000000` (about 750 rows) | 11.6 M | sequential scan, 2,310 M, 218 ms | 6.60 M, 0.764 ms |
| `r && int4range(5000000, 5000100)` | 11.8 M | sequential scan, 2,897 M, 286 ms | 7.14 M, 0.845 ms |
| `r = '[6425569,6429691)'` | 1.85 M | 0.13 M, 0.058 ms | 0.73 M, 0.118 ms |
| `ip = '20.64.51.179'` | 13.95 M | 0.12 M, 0.058 ms | 0.22 M, 0.053 ms |
| `ip <<= '20.7.7.0/24'` (about 46 rows) | 51.6 M | 0.52 M, 0.102 ms (a range of the b-tree) | 0.58 M, 0.126 ms |
| `ip >>= '20.7.1.5/32'` | 68.6 M | sequential scan, 1,800 M, 131 ms | 0.26 M, 0.066 ms |
| `u = 'a1c6a693-..'` (uuid, unique) | 127.1 M | 0.12 M, 0.057 ms | - |

| 1,000,000 rows | plain `sift_col` (36.2 MB) | b-tree (21.3 + 30.1 + 30.1 MB) | GiST (32.1 + 47.7 MB) |
|---|---|---|---|
| `r @> 5000000` | 3.12 M | sequential scan, 768 M, 81.7 ms | 2.65 M, 0.361 ms |
| `r && int4range(5000000, 5000100)` | 3.18 M | sequential scan, 965 M, 92.1 ms | 2.89 M, 0.411 ms |
| `r = '[6066155,6067798)'` | 1.11 M | 0.13 M, 0.063 ms | 0.58 M, 0.147 ms |
| `ip = '20.88.91.139'` | 5.03 M | 0.12 M, 0.066 ms | 0.23 M, 0.069 ms |
| `ip <<= '20.7.7.0/24'` | 18.7 M | 0.28 M, 0.108 ms | 0.34 M, 0.077 ms |
| `ip >>= '20.7.1.5/32'` | 22.4 M | sequential scan, 597 M, 54.8 ms | 0.23 M, 0.050 ms |

- A containment of a range costs the lanes (0.7 M instructions with the cut by the start of the range, [OPTIONS.md](OPTIONS.md)) plus the heap fetch of its rows: 1.0 to 1.7 times a GiST's instructions and
  1.1 to 1.6 times its time, the b-tree having no plan for it.
- A network or an address on random values has no zone to prune unless the units are cut by it. Without the cut every test reads the node lane of every row in one pass (the offset lane of an `=` only for the rows
  the node kept), 4.5 to 5.2 instructions a row for `ip =` and 7 to 28 for the others (3,000,000 random addresses and a 128-character note, an index on the address alone, `key_cut = 'off'`, instructions of one backend; the
  column is `count(*)` and rows returned; 0.5 M, 1 M and 3 M rows, the release before this change in brackets):

  | no cut | 0.5 M | 1 M | 3 M |
  |---|---|---|---|
  | `count(*)` `ip = x` | 2.77 M (9.97) | 5.31 M (19.65) | 15.5 M (323.9) |
  | rows `ip = x` | 2.38 M (3.77) | 4.63 M (7.22) | 13.6 M (21.4) |
  | `count(*)` `ip <<= '20.7.7.0/24'` | 3.9 M (4.2) | 7.59 M (8.01) | 22.3 M (23.6) |
  | `count(*)` `ip >>= '20.7.1.5/32'` | 11.8 M (16.3) | 23.4 M (32.2) | 69.6 M (96.3) |
  | `count(*)` `ip && '20.7.7.0/24'` | 14.2 M (53.9) | 28.1 M (107) | 83.8 M (321.6) |

  The count of `ip =` read the dictionary of the image of every segment that held the constant (it grew faster than the rows: 20 instructions a row at 1 M, 108 at 3 M); the offset of an IPv4 address inside its block is
  now a lane, so the node and the offset are exact and the dictionary is read only for IPv6 (the rows of its node go to PostgreSQL's operator). The membership of the blocks that contain a network (`>>=`, `>>`, `&&`) is a
  load from a table of 64 slots by the lowest set bit of the node, which is the depth of the block: a scan of the node lane is the floor of these, 50 to 300 times a GiST's instructions at 3 M rows.
  An index on the address alone, cut by its node (`key_cut = 'auto: ip#1'`, which `CREATE INDEX` picks by itself from 64 units), prunes the units to the few whose nodes meet the constant:

  | `sift_col (ip)`, 3,000,000 rows | no cut | cut by `ip#1` | GiST | b-tree |
  |---|---|---|---|---|
  | rows `ip = x` | 13.6 M | 1.34 M, 0.19 ms | 0.24 M, 0.059 ms | 0.10 M, 0.045 ms |
  | rows `ip <<= '20.7.7.0/24'` | 20.6 M | 1.56 M, 0.20 ms | 0.47 M, 0.077 ms | 0.40 M, 0.093 ms |
  | rows `ip >>= '20.7.1.5/32'` | 67.6 M | 3.43 M | 0.25 M, 0.049 ms | sequential scan, 1,590 M |
  | `count(*)` `ip = x` | 15.5 M | 0.47 M | | |
  | `count(*)` `ip <<= '20.7.7.0/24'` | 22.3 M | 0.52 M | | |
  | `count(*)` `ip >>= '20.7.1.5/32'` | 69.6 M | 2.62 M | | |
  | index | 26.6 MB | 24.6 MB | 95.8 MB | 63.1 MB |

  The cut closes the 100 to 400 times of the scan to 4 to 14 times a GiST (and the index is 7% smaller). It is one order per index: a table with a range and an address column can cut by one
  of them (the default picks the first lane that qualifies, the range). The chooser prices the dictionary-coded image of the address like its node: both are listed in the order of the node in a unit of the
  cut, and the sample predicts the 3,000,000-row index 6.8% smaller where the built one is 7.4% smaller (it priced the image at one byte a row and predicted a growth, so the cut was declined).
  A node of 3-byte values (the node lane of a unit of 10,000 addresses) is written bit-packed when that is at most 1.6% larger than the byte-aligned form: only the bit-packed form has a range kernel, and the byte-aligned one
  was decoded value by value (7 instructions a row against 1).
- The `uuid` equality of a plain index reads the whole index at these sizes (413 buffers at 200,000 rows, 5,977 at 3,000,000; 4.1 M instructions at 200,000 rows, 18.6 M at 1,000,000, 119 to 136 M at 3,000,000, the same
  for a one-column index and for `key_cut = 'off'`; a `text` equality at 200,000 rows measures 4.1 M too, an `integer` one 0.34 M). COL-SCAN.md lists 0.29 M for both strings at 200,000 rows; that figure is not reproduced on this build.
- A range the lanes do not cover (`numrange`, a range of a user type) is offered the same operators and left to the heap: the index returns every row and the heap decides.
- The counts of the same shapes are the matrix above (`count(*)` is the aggregate pushdown, not this scan).

## What is not served

- **Rows from an address or a lane that does not prune:** see the table above; an index scan of random addresses reads every address lane. `IN` and `= ANY (array)` on a range or a network are left to the heap.
- **Types that keep the image only:** multiranges, `numrange`, ranges of a user type, `point`, `box`, `circle`, `polygon`, `macaddr`. Their containment is PostgreSQL's operator on each distinct value, as before.
- **Operators that keep the image:** `IN`, ordering of a range or a network and `<->`. `|v| >= 2^62` of a range bound (a timestamp `infinity`, an `int8` near its limits) and an IPv6 mask of 62 bits or more are
  decided by PostgreSQL's operator on the candidate rows, so a column full of them prunes nothing.
- **A larger index than the image alone:** the lanes add about 6 MB (range) and 3 MB (inet) per 1,000,000 rows of random values. They are written for every range and network column; there is no option to leave them out.
- **REINDEX:** an index built by 3.5 or older with such a column does not match its column layout; `REINDEX` rebuilds it ([UPGRADING.md](UPGRADING.md)).
- **`&&` on a network:** the answer is the union of two tests (the networks inside the constant, the networks that contain it), and the zones of a unit are tested for one range of the lane, so no unit is pruned: the scan reads the node lane of every row. 1,000,000 random IPv6 hosts, `ip && '2001:db8:1::/48'`: 21.2 million instructions without the cut, 19.8 million with it (a GiST: 0.23 million); 3,000,000: 63 and 57 million. Mixed IPv4 and IPv6 networks of random masks, the same query: 133 and 33.6 million at 1,000,000 rows, 415 and 95 million at 3,000,000 (a GiST: 55 and 123 million), so there it is ahead.
- **Equality and long masks of an IPv6 address:** the node holds the first 62 bits of the address, so `=` on a value of a family 6 and a mask of 62 bits or more are decided by PostgreSQL's operator on the candidate rows, and the operator reads the dictionary of the segment, whole: its cost grows with the index and not with the candidates. 1,000,000 random IPv6 hosts, `ip = x`: 6.3 million instructions (a GiST: 0.2 million); 3,000,000: about 220 million (the dictionary inflated with LZ4). On mixed networks `ip = x/53` the default cut makes this worse past the size where the parsed dictionary fits the cache of the backend (`dict_cache_kb`, 64 MB): 0.69 million at 1,000,000 rows (uncut 43.8), 527 million at 2,000,000 (87), 430 to 520 million at 3,000,000 (131), because the cut index writes the dictionary with zstd, which costs 3 to 4 times the instructions of LZ4 to inflate. The index is 21% smaller (52.6 MB against 66.8 MB at 3,000,000), and at 3,000,000 rows `<<=`, `&&` and `>>=` read 50, 4.4 and 1.4 times fewer instructions. An IPv4 address has an offset lane and is exact on the lanes.
