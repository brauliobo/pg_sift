# Dirty units: a value a leaf cannot store

Design note for 3.2. Status: the diagnostic (C) and the recheck of option A for the columns of `jsonb_to_recordset` are implemented ("What is implemented"); other reads of a flagged leaf keep the normal plan.

## The problem

A typed leaf (`float8`, `int4`, `text(N)`, ...) cannot store a value of another JSON type (a string in a number leaf, an
`OVERFLOW` of docs/COL-SHRED.md). The chunk keeps the slot as NULL with the overflow flag, and the unit flags the leaf
(`SIFT_COLZ_OVERFLOW`, `has_ovf`). Two layers decide what happens next:

1. **The planner** (`reads_overflow`) refuses a spec that reads a leaf some unit of the index flags (`SiftColStats.ovf[leaf]`, a
   property of the whole index, not of the units the query reads). A spec that only tests `jsonb_typeof` of the leaf, and a numeric leaf
   whose only overflow is NaN, are exempt.
2. **The scan** raises an error when a row of its snapshot holds such a value (`check_overflow`): a plan that was priced before the
   overflow was written must not answer wrongly.

So one string-valued number in a `float8` path keeps every query that reads the path on the heap plan, and nothing says so. A
workload with a few percent of dirty rows spread over all units loses the pushdown for that path entirely.

What does not change: the value exists in the heap, and the heap plan reads it with the SQL semantics of the expression (`->`
then a cast, `jsonb_to_recordset` with an input function). The index is not wrong, only unable to answer for those rows.

## What a client can do today

Read the path as `astext` and cast in the query (`avg(CASE WHEN s ~ '^-?[0-9.]+$' THEN s::float8 END)`): an `astext` leaf holds the
text of strings, numbers and booleans, so no value overflows. The cast runs for every tuple of the scan. Since 3.2 the per-item arms
of a two-level query fuse over such a leaf as well (an inner `sum` or `avg` of a `float8` expression the scan evaluates), which was the
33x gap of `bench/sql/workload/shapes/g5_*.sql`. Normalizing the document in an index expression (a function that repairs the values)
keeps typed leaves clean and is the faster form.

## Options

**A. Recheck the flagged rows from the heap (the one the 3.0.1 notes announced; implemented for recordset columns, below).**
The unit stays usable. The scan evaluates the unit's rows as usual, with the flagged slots as NULL, but marks the rows whose slot
overflowed in a row mask (the overflow bitmap of the chunk already has it). Those rows leave the vectorized path (selection,
group ids, accumulators) and go to a per-row path that fetches the heap tuple by TID (visibility as for `sift_col_vet`), deforms the
document, re-evaluates the spec's leaves with the SQL semantics, and feeds the same group tables. What this needs:

- a leaf evaluation of a heap row for every leaf kind the spec reads (scalar, element, group counts, dictionary codes): the group
  tables take dictionary ids and element rows, so the per-row path must produce the same ids (it can: it is the shredder over the
  datum, then the encoder's dictionary lookup);
- the aggregates' states must accept a row from either path in any order: counts, sums and min/max do; `SAA_X_*` and `SAA_GEN` hold
  per-group Datum state and also do; float sums must keep the order of the heap plan (they already differ from it in the last bit
  where the scan is vectorized: the coldiff harness bounds this);
- the two-level (cell) path, which accumulates per row and flushes cells, needs the same hook per row;
- the planner stops refusing on `ovf` and charges the rows the statistics say are flagged (`n_ovf` per leaf, not kept today: a
  per-leaf counter in the stats, a format change of the stats page, or a sample at plan time).

Cost: a heap fetch per flagged row (a random read when the page is cold), against the whole table on the heap plan. Size of the
change: a per-row path through `agg/agg.c` (about the size of the heap-vetting path), the planner gate and the cost, and the whole
coldiff matrix for a mixed unit (all five variants, every scenario). Too large for the 3.2 branch that carries this note.

**B. Let a typed leaf coerce.** A numeric string in a `float8` leaf becomes the number. This changes what the leaf means: `->` gives
the string, so `jsonb_typeof(doc -> 'k') = 'number'` and `doc @> '{"k": 7.5}'` would answer from a leaf that is not a number in
the heap. Rejected: the leaf would no longer be the document's value.

**C. Say it.** The smaller part, implemented: when `reads_overflow` refuses a spec, a `DEBUG1` message names the leaf
(`sift: the aggregate pushdown does not read "doc.opinions[].score": some unit holds values the leaf cannot store`). The planner
never raises for it and the message is at `DEBUG1` (`SET client_min_messages = debug1`), so an installation sees why a plan stayed on
the heap without any change of plans or formats. It is the entry point for A: the same site is where a per-row recheck would
replace the refusal.

## Follow-up with numbers (synthetic workload, 300,000 items, 0.3% non-object documents, 3.5% of the opinions with a string score)

Per-item arms with a count of the items, serial, best of 2 and 3 (`bench/sql/workload/run.sh time`):

| Form | sift off | sift on |
| --- | --- | --- |
| typed `float8` leaf over the raw document | the heap plan | the same heap plan (units flagged) |
| typed leaf over `coldiff.wl_norm(doc)` (scores repaired, non-objects NULL) | 2.43 s | 0.026 s |
| `astext` leaf + guarded cast, fused arms (g5) | 2.83 s | 0.073 s |

The first row is the gap: it needs option A.

## What is implemented (option A, no format change)

The flagged rows are read again by the source, not by a per-row path through the executor: the unit is presented as the stored
unit without those rows (they leave the live mask, their TIDs are kept) followed by a replacement unit built from the heap, so
selection, group ids, accumulators, the cell path, parallel workers and partitions need no change.

- **Fetch.** `table_index_fetch_tuple` with the scan's snapshot (a HOT chain followed, a version the snapshot does not see gives no
  row), the index expressions evaluated with `FormIndexDatum`, the document shredded again by the walk of `col/shred_recheck.c`.
- **Semantics.** The scalar under a declared column goes through the input function of the column's type as its text (`"8.5"` into
  `float8`, `12` into `text`, `true` into `text`), a container into a `text` column as its JSON text, a key of an array or of a scalar is
  NULL. Errors are those of `jsonb_to_recordset` (a string that is no number `22P02`, `cannot call jsonb_to_recordset on a non-array`
  `22023`, `argument of jsonb_to_recordset must be an array of objects` `22023`) and are kept in the replacement unit with the list
  poisoned; the executor raises the recorded error after the row-level conjuncts (`check_overflow` late), so a row filtered out by a
  leaf of its own row raises nothing, as in the normal plan. The columns the query does not read are coerced too (PostgreSQL coerces
  every declared column), so a flagged unread column is read as well.
- **Planner.** `reads_overflow` no longer refuses a flagged leaf that is a column of a recordset (`bool`, `int2`, `int4`, `int8`,
  `float4`, `float8`, `text` leaves, read as the record column reads them, alone or inside an expression: a key, a filter, a bucket). It still refuses when anything else reads the leaf or its
  list (a path read, `jsonb_array_length` of the list, a test of the list at the top level, a top-level conjunct that may fail
  and is applied after the others, search) and when the provider cannot read rows again (`sift_table`, whose rows have no index
  expressions). A value the shredder cannot store even after the coercion (the string `-0` in a float column, a list of more than
  2^20 elements) stays an overflow and the scan raises its generic `0A000` error for it: the planner cannot know it beforehand.
  A string `NaN` or `Infinity` converts, as the input function of the column gives it. The price is `AGG_RECHECK_FRAC` (2%) of the
  rows of the units the leaf flags (the share of units whose zone carries the overflow flag, from the zones the planner reads anyway; one row
  for each flagged unit at least) at `AGG_RECHECK_ROW` (60) operator costs, so a few dirty rows in one unit cost one unit's rows, not 2% of the
  table. The per-leaf count of flagged slots (`n_ovf`) is not kept, a format change that would make the price exact and was not needed for the numbers
  below. `EXPLAIN ANALYZE` shows `Units Read Again From The Heap` (the stored units that had flagged rows, of `Units`) and `Rows Read Again From The Heap`.
- **Leaves after the source's.** `(doc -> 'm' ->> 'k')::float8` of an object leaf with free keys and `jsonb_array_length(doc -> 'l')` of an
  array leaf take their values from a column of the index (the `value` column of the members, the array itself): `reads_overflow` follows
  them to it, so a flagged column keeps the normal plan (a numeric string converts, anything else raises PostgreSQL's error), and the scan
  checks it for a plan made before the value was written (`0A000`). They were read as NULL.
- **`jsonb_each` with a cast.** A member read only through `CAST(e.value AS float8)` (a wildcard path `col.* float8`) is read again like a
  recordset column, with the cast's reading: a number as the cast gives it, any other JSON type the error the cast raises
  (`cannot cast jsonb string to type double precision`, `22023`), kept for the row. The row raises it where it reaches the aggregate, after
  its lookups, as the cast does in the normal plan, and not at all when a conjunct or a join drops it first. The plan stays normal when the
  cast is evaluated for some members only (a `FILTER`, a `CASE` branch: no aggregate evaluates it for all of them), when a conjunct reads
  the member or the key of the member, and for the member of one key (`LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`), whose error
  would be the row's, not the key's. Malformed documents that no unit flags (a JSON `null` list, a scalar element) are read by the clean path of the scan
  as before.
