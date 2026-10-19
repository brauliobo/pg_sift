# More leaf types for `sift_col` (3.2)

`numeric` of any precision, `money` and `interval` as columns of a `sift_col` index, and what the aggregate pushdown (`SiftAgg`) does with
them. Status: implemented on the 3.2 branch; section 8 is what is pushed down, section 10 the format ids. The
format itself is section 2.11 of `docs/COL.md`; where the two disagree, fix the code or this file in the same commit.

## 1. Before (3.1)

| column | `CREATE INDEX ... USING sift_col` | a query that reads it |
|---|---|---|
| `numeric(p, s)`, p <= 18, 0 <= s <= 18 | a scaled `int64` leaf (`docs/COL.md` 1) | pushed down |
| `numeric(p, s)`, p <= 18 and s > 18 (`numeric(5, 30)`) | error `invalid sift_col leaf list: a scale belongs to an integer leaf and is at most 18` | normal plan |
| `numeric(p, s)`, p > 18, unconstrained `numeric`, `numeric(p, -s)` | error `sift_col holds numeric(p, s) with p <= 18 and s >= 0` | normal plan |
| `money`, `interval` | error `sift_col does not support the type ...` | normal plan |

A column outside the index never blocks the index: the other columns of the table are served as before, a query that reads the missing one
runs the normal plan (`test/col/fixtures/n_numeric_trap_wide.sql`). `numeric` with a typmod cannot hold an infinity (`numeric field overflow`),
only NaN; an unconstrained `numeric` holds NaN, `Infinity` and `-Infinity`. The `sift_table` access method already stores every one of these
types exactly as opaque bytes (`SIFT_COLS_VAR`) and does not change.

## 2. Model: a wide value is several ordinary leaves

The column engine holds integers (`INT`, the key domain of `docs/COL.md` 1), floats and dictionary strings. A value that is not one of them is
stored as `lanes` consecutive leaves of one index attribute, each an ordinary leaf with its own chunk, null bitmap, zone and pre-aggregates.
Nothing below the schema changes: the chunk encodings, the dictionaries, the write buffer record, the merge and the pre-aggregates see more
leaves. `SiftColDef` gets two fields, `lanes` (on the head: 2 or 3, else 0) and `lane` (0 for the head, 1 and 2 for the lanes after it), kept in
the two bytes of `SiftColDefDisk` that were reserved (zero in every 3.1 schema, so the schema hash of a 3.1 leaf list does not change). The head
is the leaf a planner addresses; the lanes are named `<attribute>#1`, `<attribute>#2` (a path no shredder produces).

| SQL type | leaves | encoding |
|---|---|---|
| `money` | 1, INT, type `MONEY` | the `int64` count of the smallest currency unit, exactly PostgreSQL's `Cash` |
| `numeric(p, s)`, 0 <= s <= 38, p <= 38, not narrow | 2, INT, `scale` = s | the number times 10^s as an `int128` v: lane 0 = v >> 64 (signed), lane 1 = low 64 bits with the sign bit flipped, so the signed order of (lane 0, lane 1) is the order of v |
| `numeric` unconstrained, p > 38, s < 0 or s > 38 | 2: STR head, INT lane | head: the canonical image of the number (3.2); lane 1: the display scale the stored value had |
| `interval` | 3, INT | lanes 0 and 1: the canonical total in microseconds as an `int128` (as above); lane 2: `month << 32 \| day` (two `uint32`) |

"Narrow" is the 3.1 form (p <= 18, 0 <= s <= 18: one scaled `int64` leaf); a column that was accepted before is stored exactly as before.

### 2.1 numeric(p, s), 19 <= p <= 38 (and narrow precision with a scale above 18)

The value is `round(x * 10^s)` (exact: x has at most s digits), which has at most p <= 38 digits and so fits an `int128` (10^38 < 2^127). A
NaN is a NULL with the overflow flag in both lanes, as for the narrow form: the row stays a candidate of every predicate and the heap decides
(`docs/COL.md` 6, overflow). The zone of each lane is the zone of an INT leaf; a unit with a NaN has no zone pruning (as narrow numeric today).

### 2.2 numeric of any other shape: the canonical image

The image of a number is a byte string whose bytewise order (`memcmp`, then length) is the order of `numeric_cmp` and whose bytewise equality
is the equality of numbers: `1.0` and `1.00` have one image; `-Infinity < negative < 0 < positive < Infinity < NaN`, and a NaN equals a NaN as in
PostgreSQL's btree order. Layout: a tag byte (`0x10` -Infinity, `0x20` negative, `0x30` zero, `0x40` positive, `0x50` Infinity, `0x60` NaN);
for a non-zero finite number the decimal exponent e (value = 0.DIGITS x 10^e, DIGITS with no leading or trailing zero) as 3 bytes big endian
biased by 2^23, then the digits in pairs as the bytes 1..100 (an odd last digit is padded with 0). A negative number has every byte after the
tag complemented and ends with 0xFF, which reverses the order of magnitudes and keeps a prefix greater than its extensions. PostgreSQL's
range (131072 digits before the point, exponent below 2^15) fits the exponent field with room; the image is at most 5 + ndigits/2 bytes.

The display scale (`dscale`) is what the image drops: lane 1 keeps it, so the pair (image, dscale) is the stored number exactly as PostgreSQL
holds it (`1.00` stays `1.00`) and `sift_col_read` can give the value back. Equality, order, `IN`, zone and dictionary all use the image.
The head is a STR leaf: a dictionary entry up to 255 bytes, longer images stored inline (`docs/COL.md` 2.9); the zone of an inline unit is
the prefix key of the first 8 image bytes, which holds the tag, the exponent and the first four digits.

### 2.3 interval

PostgreSQL stores `{time (us, int64), day (int32), month (int32)}` and compares, hashes and groups on one number: `time + day * 86400e6 +
month * 30 * 86400e6` in 128 bits (`interval_cmp_value`; `'1 month' = '30 days'` and `'1 day' = '24 hours'` are true, `'2 months' > '59
days'`; checked against the server, section 9). Lanes 0 and 1 hold that total, so equality, order, `IN`, zones and group identity are the
integer ones of section 2.1; lane 2 holds month and day, which the total does not tell apart. The time part is the total minus the
other two, so the triple comes back exactly. `infinity` and `-infinity` (PostgreSQL 17+: the fields all at their extreme) have the greatest
and least totals and round-trip as any value.

### 2.4 money

PostgreSQL's `money` is an `int64` of the smallest currency unit; the number of fraction digits, the symbol and the separators come from
`lc_monetary` when the value is printed or cast. The engine stores and compares the integer and never formats it, so the stored data does not depend on
the locale; only the text form of a constant, a cast to `numeric` or a `to_char` is PostgreSQL's, at the locale of the session, as for a heap
column. The `lc_monetary` a value was read with decides how many digits an integer means: changing it between writing and reading re-reads
the same integers, in the index as in the heap.

## 3. Predicates and constants (engine)

`sift_col_filter` takes a predicate on the head. For a two-lane INT head the constants are the two lane keys (`SiftColValue.v.i` for lane 0, a
new `SiftColValue.lo` for lane 1) and the filter is the comparison of the pair: `(a, b) < (c, d)` is `a < c OR (a = c AND b < d)`, built
from the ordinary lane tests and bitmap algebra, `EQ` is both lanes, `IN` is the OR of the pairs, `NE` and `NOT IN` are the complement within
the non-NULL rows. For a STR head (the image) the constants are images and the tests are the bytewise ones of `docs/COL.md` 2.9; lane 1
is never tested alone. Money is an ordinary INT leaf.

The constant of a SQL predicate is turned into one of those tests by `sift_colw_test` (`sift_col_widescan.c`), which decides what the constant
means against the stored domain, with PostgreSQL's order (NaN above everything, equal to itself): on the scaled integers `x = c` needs c to be a
multiple of 10^-s (else no row), `x < c` is `x < ceil(c * 10^s)`, `x <= c` is `x <= floor`, `x > c` is `x > floor`, `x >= c` is `x >= ceil`; a
constant beyond the 128-bit range makes the predicate true for every non-NULL value or for none (a clamp to the extreme `int128`, which no
38-digit value reaches, is exact); NaN, `Infinity` and `-Infinity` as constants are decided without reading (`x < 'Infinity'` is every
number, `x > 'Infinity'` only the NaNs). The result is one of: no row, every row (`x <> ALL('{}')`), every non-NULL row with or without the NaNs,
the NaNs only, or an engine predicate plus whether a NaN row passes it. The engine keeps a NaN row (stored as NULL with the overflow flag in both
lanes) as a candidate of every predicate; `sift_colw_apply` then adds or removes those rows as the verdict says. `IN` ignores NULL elements,
`NOT IN` with a NULL element passes no row, a NaN element of a list is the NaN rows.

Zone maps. Each lane has its own zone, and the values of a unit lie in the box `[min0, max0] x [min1, max1]`. A pair predicate is pruned
with the two corners of the box compared as pairs: `x > c` and `x >= c` keep a unit only if the greatest corner `(max0, max1)` reaches c, `x < c`
and `x <= c` only if the least corner `(min0, min1)` is below it, `=` needs both and c inside the box, `IN` one constant like that. A unit
whose lanes flag an overflow (a NaN) describes nothing and stays. `<>`, `NOT IN` and the NULL tests do not prune on the pair.

## 4. Special values

| value | `numeric(p,s)` wide | `numeric` image | `interval` | `money` |
|---|---|---|---|---|
| NaN | NULL + overflow in both lanes; a candidate of every predicate | image `0x60`: greatest, equal to itself | - | - |
| `Infinity`, `-Infinity` | cannot occur (typmod) | images `0x50`, `0x10` | `infinity` / `-infinity`: the extreme totals | - |
| zero, `-0` | 0 | image `0x30`, `dscale` kept | 0 | 0 |
| precision 38 boundary | `99..9` (38 digits) and its negative fit (|v| < 2^127) | no limit | - | - |
| above 38 digits | does not exist in a `numeric(38, s)`; a constant of more digits is "above" or "below" all values | exact | - | - |
| scale above 38, below 0 | image form | image form | - | - |
| ties of different representation (`1.0`, `1.00`; `1 mon`, `30 days`) | none (the scale is the typmod's) | equal; lane 1 tells them apart | equal; lane 2 tells them apart | none |

## 5. Zones, pre-aggregates, statistics

The INT lanes get the zones and the exact int128 `sum` and `sumsq` of the pre-aggregates (`docs/COL-PREAGG.md`) like any INT leaf; the sums of a
wide pair are linear (`sum(v) = sum(lane 0) * 2^64 + sum(lane 1 as stored) + n * 2^63`), so a future change can answer `sum` of a wide
numeric from them. Nothing consumes a lane's sum today: a lane is not a leaf of the layout the planner sees (section 8), and the leaves of a
wide type are neither a dimension nor a measure of a cube. `sift.col_leaves` lists the lanes as `<attribute>#k` with the type of their head and
"(lane k)"; `sift._test_col_read` reads a wide leaf as the text PostgreSQL prints (a money as its integer). `sift.col_verify` checks that the lanes
of a value agree on NULL and overflow, that a numeric image is canonical with a display scale PostgreSQL has, and that an interval's total is that
of its month, day and time.

## 6. Portable core and the ports

The value code (lane keys, the 128-bit decimal text codec, the image encoder and decoder, the interval total) is `src/col/sift_col_wide.[ch]`,
in `src/portable-col.list`: C library and `sift_host.h` only, no allocation (the caller passes the buffers), no host type. It uses `__int128`
(`unsigned __int128`), as `sift_col_preagg.h` and `sift_aggspec.h` already do; a compiler without it (MSVC) cannot build the column engine, which
neither SQLite nor MariaDB ships yet (they build `portable.list` only; `make -C sqlite col-smoke` compiles `portable-col.list`, and
`scripts/check-portable.sh` checks both lists). The PostgreSQL side (`sift_col_types.c`) maps `Datum`s and OIDs to it.

## 7. Where the engine stops

`sift_table` keeps its own mapping (`SIFT_COLS_VAR` for what the INT class cannot hold); `sift_col_numeric_scale` still answers "one narrow INT
leaf" for it, and now also requires s <= 18, which makes `numeric(5, 30)` a lane pair for the index and a variable-length leaf for the table
(it was an error in the index before). `uuid` and `bytea` are not part of this change (the STR class could hold them; no query shape reads them yet). `time`, `timetz`,
`enum`, arrays and ranges stay refused.

## 8. What `SiftAgg` does with them

The planner sees one leaf per head, but no expression reads its values: a lane and a head of these types have `typid = InvalidOid` in the layout
(`SiftSrcLeaf.wtypid` keeps the SQL type for the one place that tests them). Only a conjunct of the WHERE can test such a leaf (`SAP_WIDE`), and
the column engine decides it on the stored lanes; the executor never reads the values, so a NaN, an image or a lane pair needs no vector type.

| | `money` | `numeric(p, s)` wide | `numeric` image | `interval` |
|---|---|---|---|---|
| WHERE conjunct `= <> < <= > >=`, `BETWEEN`, `IN` / `= ANY`, `NOT IN` / `<> ALL`, `IS [NOT] NULL` | pushed | pushed | pushed | pushed |
| the same under `OR`, `CASE`, `FILTER`, as a key, in an expression, a cast | normal plan | normal plan | normal plan | normal plan |
| `count(x)`, `min`, `max`, `sum`, `avg`, `GROUP BY x`, `count(DISTINCT x)` | normal plan | normal plan | normal plan | normal plan |

Operands are constants, parameters (a generic plan evaluates them when the scan starts, a NULL parameter passes no row) or stable expressions; an IN
list is a constant array or an array parameter. At most 8 such conjuncts per scan (`SIFT_AGG_MAX_WIDE`), more keep the normal plan. A scan with
a wide conjunct skips the pre-aggregates of its units (it must test rows) but keeps zone pruning, parallel workers, the BM25 match set, the top-N scan and
the write buffer. `EXPLAIN ANALYZE` shows `Wide Filters In Source`.

What stays on the normal plan, and why. Reading the value is what the executor has no vector for: the lane pair, the image and the interval would
each need a vector type and kernels. For the image and the interval it is also an equality class problem: the value of a group key or of `min` /
`max` is observable beyond its equality class (`1.0` against `1.00`, `1 mon` against `30 days`), and PostgreSQL's choice among equal values depends on
its plan (the first seen for a hash group, the later for `min`/`max`); the stored totals and images do not say which row's representation a group
should show. The display-scale and payload lanes are stored so a later version can return the representation of the first row of each group.
`sum`/`avg` of an `interval` need the three components added separately (PostgreSQL adds month, day and time apart), which the canonical total does
not give. Money is the cheap next step (its leaf is an `int64` with no ties): `min`/`max`/`sum`/`GROUP BY` need the type in the executor's operator and
function tables. A wide column in the index never stops the pushdown of the other columns of a query.

## 9. Tests

- `tools/colwide/test.c`: the portable value code against a reference (a decimal-text big-number reference and `__int128`), the image order
  against decimal comparison for random and adversarial pairs (carry-over zeros, 10^k boundaries, odd digit counts, signs, the exponent
  limits), decode of arbitrary bytes, built sanitized (`scripts/fuzz.sh colwide`, also in `scripts/ci.sh quick`).
- `test/sql/col_index_wide.sql`: the column source against the heap for every operator on every type, NULL and NaN rows, constants beyond the
  range, zone pruning of the lane pair, the values read back, the write buffer, promotion, merge, VACUUM, `REINDEX`, `sift.col_verify`.
- `test/sql/col_format6.sql`: an index of the old types still writes minor 3 or 4 whatever the type of its table's other columns; an index with a
  wide leaf writes minor 6 (and refuses `sift.col_format_minor = 6`); minors side by side, merge, REINDEX.
- `test/sql/col_agg_wide.sql`: SiftAgg with and without the pushdown for every shape of section 8: constants of other scales, beyond the range,
  NaN and the infinities, NULL and empty lists, parameters of a generic plan, the write buffer, updates, merge, parallel workers; and the shapes
  that must keep the normal plan.
- `test/col/fixtures/w_*.sql` (table `t_wide`): coldiff against the normal plan with the traps of section 4 (NaN, the precision 38 boundary, 39 and
  400 digit values, huge and negative scales, NULLs, ties of scale and of interval spelling, money extremes, infinite intervals, parameters).
- Interval ordering was checked on a live server (the facts of section 2.3, and the extreme totals `2147483647 months 2147483647 days
  9223372036854775807 us` against `... 2147483646 days ...`), not only read from source.

## 10. Format ids

- Column segment minor **6** (`SIFT_COL_FORMAT_WIDE`), claimed on `feat/v32-leaftypes`. 3.1 uses 4 on `origin/main` (the type of an overflowing
  value and the element cubes) and `feat/v31-writepath` also writes a 4 (version vectors) which the integration must renumber to 5, so 6 is the
  next free one; if 3.1 leaves 5 unused, only this constant changes. Minor 5 is not claimed here.
- A segment is written as minor 6 only when its schema has a leaf of the new kind (a lane, `MONEY` or `INTERVAL`). Every other segment is
  written as before (minor 3 or 4 by `sift.col_format_minor`), so an index of old types stays readable by 3.1. `sift.col_format_minor` keeps its
  range 3 to 4; it does not apply to a wide schema, which has no older form.
- Readers: this build reads minors 3, 4 and 6 (and 5 where 3.1 defines it). A 3.1 library refuses a minor 6 segment (`format > SIFT_COL_FORMAT_MINOR`,
  hint "Use a newer pg_sift, or REINDEX") and cannot open the index of a wide column at all (its column mapper refuses the type): downgrade means
  dropping that index.
- `SiftColType` tags 15 (`MONEY`) and 16 (`INTERVAL`); `SiftColDefDisk` bytes 14 and 15 are `lanes` and `lane`. The schema hash is over that
  serialization, so an old leaf list hashes as before.
- The metapage and the BM25 minor do not change.
- Upgrade tests: `test/sql/col_format6.sql` (side by side, merge, REINDEX). The extension script does not change (the access method and operator
  class are the 3.1 ones), so `ALTER EXTENSION ... UPDATE` needs no step; an index of the old types built before is read as before.
