# More leaf types for `sift_col` (3.2)

`numeric` of any precision, `money` and `interval` as columns of a `sift_col` index, and what the aggregate pushdown (`SiftAgg`) does with
them. Also `time`, enums, `uuid`, `bytea`, arrays, ranges, and `inet`, `cidr`, `macaddr`, `macaddr8`, `timetz`, `bit` and `varbit` (section 7). Status: implemented on the 3.2 branch; section 8 is what is pushed down, section 10 the format ids. The
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

The constant of a SQL predicate is turned into one of those tests by `sift_colw_test` (`col/widescan.c`), which decides what the constant
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

The value code (lane keys, the 128-bit decimal text codec, the image encoder and decoder, the interval total) is `src/col/wide.[ch]`,
in `src/portable-col.list`: C library and `core/host.h` only, no allocation (the caller passes the buffers), no host type. It uses `__int128`
(`unsigned __int128`), as `col/preagg.h` and `agg/spec.h` already do; a compiler without it (MSVC) cannot build the column engine, which
neither SQLite nor MariaDB ships yet (they build `portable.list` only; `make -C sqlite col-smoke` compiles `portable-col.list`, and
`scripts/check-portable.sh` checks both lists). The PostgreSQL side (`col/types.c`) maps `Datum`s and OIDs to it.

## 7. The other types: time, enum, uuid, bytea, arrays, ranges, network, timetz and bit strings

| SQL type | leaf | encoding | why this one |
|---|---|---|---|
| `time` | INT, type `TIME` | the `int64` microseconds since midnight (`24:00:00` included) | PostgreSQL's own `TimeADT`; equality is the integer's, there are no ties |
| enum | INT, type `ENUM` | the OID of the value as an `int32` | the *order* of an enum is the catalog's (`pg_enum.enumsortorder`, which `ALTER TYPE ... ADD VALUE BEFORE/AFTER` changes without touching a row), so no rank is stored: the order is read when a query runs |
| `uuid` | STR, type `UUID` | the 16 raw bytes | equality and `memcmp` order are PostgreSQL's (`uuid_cmp` is `memcmp`) |
| `bytea` | STR, type `BYTEA` | the detoasted varlena image | `byteacmp` is `memcmp` then length; bytea has no ties |
| arrays, ranges, multiranges | STR, type `VARLENA` | the detoasted varlena image of the datum | the image of a datum is its canonical form; equality, hash and every operator are PostgreSQL's on the decoded value |
| `inet`, `cidr`, `bit`, `varbit` | STR, type `VARLENA` | the detoasted varlena image of the datum | as above: `10.0.0.1/24` and `10.0.0.1/32` are two images and two values; `inet` and `cidr` are one type to the executor (`cidr` has no operators of its own) |
| `macaddr`, `macaddr8` | STR, types `MACADDR`, `MACADDR8` | the 6 or 8 bytes of the datum | PostgreSQL's input folds the spellings (`08-00-2B-01-02-03`, `0800.2b01.0203`) into one datum |
| `timetz` | STR, type `TIMETZ` | the 12 bytes of the datum (`typlen`: the microseconds, then the zone in seconds; the 4 bytes of padding of the C struct are not stored) | `timetz` compares the instant first and the zone second (`12:00+01 = 11:00+00` is false), which no byte order of the image gives, so the executor uses PostgreSQL's own functions on the decoded datum |

Only columns that are in a `sift_col` index are affected; `sift_table` keeps its own layout for these types (it stores them by size) and a table with
them still writes minor 3 or 4. The column engine keeps the value and its null bitmap and applies no order of its own to an enum, a uuid, an image
(`sift_colw_test` has none), so a conjunct on one of them is PostgreSQL's operator evaluated by the executor on the value of each distinct entry
(section 8); an enum is never compared as an OID, because its order is the catalog's, which `ALTER TYPE ... ADD VALUE BEFORE/AFTER` changes without
touching a row.

What stays refused, and why (`CREATE INDEX` says `sift_col does not support the type ...`, the table's other columns still work, a query that reads the
column runs the normal plan):

- `point`, `tsvector`, `xml`, `json` and the other types with no leaf form above: they have no equality, no btree or hash operator class, or no operator
  the executor could use for grouping and `min`/`max` (`json` has no equality at all). An array, range or multirange *of* any of these is accepted (the image is opaque
  and the element type is only PostgreSQL's concern).

## 8. What `SiftAgg` does with them

The planner reads a wide leaf (numeric lanes, image, `money`, `interval`) and the opaque images (uuid, bytea, arrays, ranges, inet, cidr, macaddr, macaddr8, timetz, bit, varbit) through a per-unit
dictionary of Datums (`src/agg/wide.c`): each unit decodes its lanes once into the distinct values (numeric in the short varlena form PostgreSQL
builds, the `Interval` struct, the stored varlena or uuid bytes) and the leaf is a code vector of that dictionary, exposed to the executor as an
ordinary `CODE` leaf whose entries *are* the Datums. Time is an `int64` vector and an enum an `int32` vector of OIDs. A wide leaf is not eligible
for the dense or LUT key forms, native `count(DISTINCT)`, the pre-aggregates, the top-N scan (a numeric, wide or an image, and a uuid, bytea or inet are keys of it: their datums are compared with the type's own function; so is an enum, whose OIDs the scan ranks by the order of the type when it opens) or the GPU; it goes through the code of PostgreSQL's
own functions (`SAA_GEN`: its transition and final functions on the Datums; `combo` keys: its expression per distinct tuple).

| shape | `money` | `numeric` (wide and image) | `interval` | `time` | enum | `uuid`, `bytea`, arrays, ranges, network, `timetz`, bit strings |
|---|---|---|---|---|---|---|
| conjunct of the WHERE `= <> < <= > >=`, `BETWEEN`, `IN`, `NOT IN`, `IS [NOT] NULL` | pushed (lane test) | pushed (lane test) | pushed (lane test) | pushed | pushed | executor (PostgreSQL's operator), array and range operators included |
| boolean trees: `OR`, `NOT`, 3-valued logic, `FILTER (WHERE ...)` on any aggregate, `CASE WHEN` | pushed | pushed | pushed | pushed | pushed | pushed |
| expressions: casts, `n + const`, `IS [NOT] DISTINCT FROM`, `= ANY(array)`, `extract(...)` | pushed | pushed | pushed | pushed | pushed | pushed |
| `GROUP BY x` | pushed | pushed | pushed | pushed | pushed | pushed |
| `count(x)`, `count(DISTINCT x)` | pushed | pushed | pushed | pushed | pushed | pushed |
| `min`, `max` | pushed | pushed | pushed | pushed | pushed (catalog order) | `bytea`, arrays, `inet`, `cidr` (`inet`'s) and `timetz` (ranges, `uuid`, `macaddr`, `macaddr8`, `bit` and `varbit` have no `min`/`max`) |
| `sum`, `avg` | pushed (`cash_pl`: an overflow raises as the heap's) | pushed | pushed (`interval_pl`: month, day and time added apart) | no such aggregate | no such aggregate | no such aggregate |

"Pushed" means: the same function PostgreSQL's plan would call, on the same value, so the answer, its text and its errors are the normal plan's; each shape
is a coldiff fixture (`m_*`, `w_wide_*`, `w_key_*`, `w_agg_*`, `w_tree_*`) and a regress check that compares the rows as text with the pushdown on and off.
Operands are constants, parameters (a generic plan evaluates them when the scan starts) or stable expressions. At most 8 lane-tested conjuncts per scan
(`SIFT_AGG_MAX_WIDE`). A scan with a lane-tested conjunct skips the pre-aggregates of its units but keeps zone pruning, the BM25 match set, the top-N scan
and the write buffer. `EXPLAIN ANALYZE` shows `Wide Filters In Source`.

### 8.1 The tie rule

Values equal under PostgreSQL's equality but spelled differently (`1.0` and `1.00`, `1 mon` and `30 days`, `'{1.0}'` and `'{1.00}'`) are one group, as in
the heap. The spelling a group shows is that of the first selected row of the group in scan order, which is what a serial hash aggregate
shows. PostgreSQL's *sort*-based plans keep an arbitrary one (a sort is not stable and the plan picks by cost), and `min`/`max`/`DISTINCT` aggregates of
tie-prone values (`min(numeric)` over `1.0`, `1.00`; `sum(DISTINCT n)`; `max(interval)` over `1 mon`, `30 days`) pick the first or the last seen in an order
the plan decides, so for those the answer is only one of PostgreSQL's possible answers, not the one of every plan. The fixtures that show a spelling run
the heap plan with `enable_sort = off` (the hash plan); the others compare counts, never the spelling.
Money, time, enum and uuid have no ties.

### 8.2 What stays on the normal plan, and why

- Parallel workers: a spec with a key over a wide leaf, or a `SAA_GEN` aggregate with a wide argument, runs serial. The dictionaries are per unit and per
  worker, and the first-seen spelling of a group would then depend on which worker saw it first; serial keeps the tie rule above. Parallel stays on for
  every other shape (`col_agg_types32`, `col_agg_wide_keys` check workers).
- A unique wide column (a numeric with a distinct value per row): the dictionary is as large as the unit, so building it costs more than the normal plan.
  The planner prices a wide leaf by its `pg_statistic` distinct fraction (`AGG_WIDE_ROW` per row, `AGG_WIDE_ENTRY` per distinct value) and keeps the heap
  plan when that is cheaper.
- `GROUP BY money` in PostgreSQL is sort-based (money has a btree opclass and no hash one); the pushdown hashes the cents (`hashint8`), the rows are the same
  and their order is a hash order, as any hash aggregate's.
- More than 8 lane-tested conjuncts in one scan.
- `GROUP BY` or `SELECT DISTINCT` a `bit` or `varbit` column: PostgreSQL has no hash operator class for them and groups by sorting; the pushdown groups by the image of the value, which is the same for equal values (the padding bits are zero), so the lengths of a `varbit` stay apart. An expression of one keeps the plan
  (`count(DISTINCT)`, conjuncts and expressions over them are pushed; `GROUP BY length(vb)` is a key of an integer).
- A `cidr` column that is a group key and is also read in a conjunct, a `FILTER` or an expression of the same query (`cd = '10.0.0.0/8'`, `host(cd)`): the operators of
  `cidr` are `inet`'s, so the query reads the leaf as `inet` and as `cidr`, and a leaf has one type in a combo. Keying on `cd::inet` (`GROUP BY cd::inet` is pushed)
  keeps the pushdown.
- The types refused in section 7.

## 9. Tests

- `tools/colwide/test.c`: the portable value code against a reference (a decimal-text big-number reference and `__int128`), the image order
  against decimal comparison for random and adversarial pairs (carry-over zeros, 10^k boundaries, odd digit counts, signs, the exponent
  limits), decode of arbitrary bytes, built sanitized (`scripts/fuzz.sh colwide`, also in `scripts/ci.sh quick`).
- `test/sql/col_index_wide.sql`: the column source against the heap for every operator on every type, NULL and NaN rows, constants beyond the
  range, zone pruning of the lane pair, the values read back, the write buffer, promotion, merge, VACUUM, `REINDEX`, `sift.col_verify`.
- `test/sql/col_format5.sql`: an index of the old types still writes minor 3 or 4 whatever the type of its table's other columns; an index with a
  wide leaf writes minor 5 (and refuses `sift.col_format_minor = 5`); minors side by side, merge, REINDEX.
- `test/sql/col_agg_wide.sql`: SiftAgg with and without the pushdown for every shape of section 8: constants of other scales, beyond the range,
  NaN and the infinities, NULL and empty lists, parameters of a generic plan, the write buffer, updates, merge, parallel workers; and the shapes
  that must keep the normal plan.
- `test/col/fixtures/w_*.sql` (table `t_wide`): coldiff against the normal plan with the traps of section 4 (NaN, the precision 38 boundary, 39 and
  400 digit values, huge and negative scales, NULLs, ties of scale and of interval spelling, money extremes, infinite intervals, parameters).
- `test/sql/col_agg_types33.sql`, `test/col/fixtures/n_net_*.sql` (table `t_net`): the network, `timetz` and bit string types against the heap: IPv4 and IPv6, masks, IPv4-mapped
  addresses, equal values spelled differently, `timetz` extremes and the zone tie-break, `bit(8)` with leading zeros, `varbit` of lengths 0 to 3 and out of line; keys,
  conjuncts, `min`/`max`, `count(DISTINCT)`, `OR`, `CASE`, `FILTER`, `host`/`family`/`masklen`, parameters, the write buffer, DELETE, UPDATE, merge and REINDEX; the
  column minor of an index of old types; the types that stay refused.
- `test/sql/col_agg_types32.sql`: the keys, aggregates, conjuncts, OR, FILTER, expressions and parameters over time, enum, uuid, bytea, arrays, ranges and
  multiranges against the heap; enum values added (`BEFORE`/`AFTER`) and renamed after the index was built; the write buffer, DELETE and UPDATE, merge,
  parallel workers; a `sift_table` with these types. `test/col/fixtures/m_*.sql`, `w_key_*`, `w_agg_*`, `w_tree_*`: the same in coldiff with scenarios.
  `test/sql/col_agg_wide_keys.sql`: the shapes of section 8 for the wide types, the tie rule, the planner's pricing.
- Interval ordering was checked on a live server (the facts of section 2.3, and the extreme totals `2147483647 months 2147483647 days
  9223372036854775807 us` against `... 2147483646 days ...`), not only read from source.

## 10. Format ids

- Column segment minor **5** (`SIFT_COL_FORMAT_WIDE`), the next free number after the 3.1 final (which uses 4 only), claimed in `docs/FORMATS.md`
  section 4.
- A segment is written as minor 5 only when its schema has a leaf of the new kind (a lane, `MONEY`, `INTERVAL`, `TIME`, `ENUM`, `VARLENA`, `MACADDR`, `MACADDR8`, `TIMETZ`, or a
  `UUID` or `BYTEA` leaf stored as an image; the exceptions leaf of a `sift_table`, `BYTEA` with a variable store, stays minor 3 or 4). Every other segment is
  written as before (minor 3 or 4 by `sift.col_format_minor`), so an index of old types stays readable by 3.1. `sift.col_format_minor` keeps its
  range 3 to 4; it does not apply to a wide schema, which has no older form.
- Readers: this build reads minors 3, 4 and 5. A 3.1 library refuses a minor 5 segment (`format > SIFT_COL_FORMAT_MINOR`,
  hint "Use a newer pg_sift, or REINDEX") and cannot open the index of one of these columns at all (its column mapper refuses the type): downgrade means
  dropping that index. No further minor is needed: the phase-2 types are leaves of the minor 5 schema (INT and STR chunks as before).
- `SiftColType` tags 15 (`MONEY`), 16 (`INTERVAL`), 17 (`TIME`), 18 (`ENUM`), 19 (`VARLENA`; also the tag of `inet`, `cidr`, `bit` and `varbit`), 20 (`MACADDR`), 21 (`MACADDR8`) and 22 (`TIMETZ`); `UUID` and `BYTEA` already had tags (they were never written
  by an index before); `SiftColDefDisk` bytes 14 and 15 are `lanes` and `lane`. The schema hash is over that
  serialization, so an old leaf list hashes as before.
- The metapage and the BM25 minor do not change.
- Upgrade tests: `test/sql/col_format5.sql` (side by side, merge, REINDEX). The extension script does not change (the access method and operator
  class are the 3.1 ones), so `ALTER EXTENSION ... UPDATE` needs no step; an index of the old types built before is read as before.
