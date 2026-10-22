# Dirty units: a value a leaf cannot store

Design note for 3.2. Status: the diagnostic (C) and the recheck of option A for the columns of `jsonb_to_recordset`, text paths, `jsonb_each` casts and scalar paths read as a cast are implemented ("What is implemented"); other reads of a flagged leaf keep the normal plan.

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
(`sift: the aggregate pushdown does not read "doc.parts[].score": some unit holds values the leaf cannot store`). The planner
never raises for it and the message is at `DEBUG1` (`SET client_min_messages = debug1`), so an installation sees why a plan stayed on
the heap without any change of plans or formats. It is the entry point for A: the same site is where a per-row recheck would
replace the refusal.

## Follow-up with numbers (synthetic workload, 300,000 items, 0.3% non-object documents, 3.5% of the parts with a string score)

Per-item arms with a count of the items, serial, best of 2 and 3 (`bench/sql/workload/run.sh time`):

| Form | sift off | sift on |
| --- | --- | --- |
| typed `float8` leaf over the raw document | the heap plan | the same heap plan (units flagged) |
| typed leaf over `coldiff.load_norm(doc)` (scores repaired, non-objects NULL) | 2.43 s | 0.026 s |
| `astext` leaf + guarded cast, fused arms (g5) | 2.83 s | 0.073 s |

The first row is the gap: it needs option A.

## What is implemented (option A, no format change)

The flagged rows are read again by the source, not by a per-row path through the executor: the unit is presented as the stored
unit without those rows (they leave the live mask, their TIDs are kept) followed by a replacement unit built from the heap, so
selection, group ids, accumulators, the cell path, parallel workers and partitions need no change.

- **Fetch.** `table_index_fetch_tuple` with the scan's snapshot (a HOT chain followed, a version the snapshot does not see gives no
  row), the index expressions evaluated with `FormIndexDatum`, the document shredded again by the walk of `col/shred_recheck.c`.
- **Which rows.** The rows a unit's flag takes out are only candidates: the scan tests them with the conditions on the columns the stored unit holds (the
  source's filters, text tests, and the comparison, IN and NULL conjuncts of a row; a row with no stored value of a conjunct's leaf passes it) and fetches those
  left. A condition of another shape, or on a column a candidate cannot store, keeps the row.
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
  expressions). A row read again is shredded with wider limits (2^24 list elements, 256 MB of text) and the string `-0` of a float
  column is stored as 0 (a leaf never holds -0): the scan raises its generic `0A000` error only for a value written after the plan was made
  and for a list or a text beyond those limits. A result that is all zeros prints the sign of the zero the scan accumulates, not of the `-0` SQL gives.
  A filter or a key on the member of one key of a map leaf (`jsonb_extract_path(doc, 'k')`) next to a `jsonb_each` cast reads the rows read again like the others: a dirty value of another key
  raises nothing for the rows that filter drops, as the normal plan. A string `NaN` or `Infinity` converts, as the input function of the column gives it. The price is `AGG_RECHECK_FRAC` (2%) of the
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
  cast is evaluated for some members only (a `FILTER`, a `CASE` branch: no aggregate evaluates it for all of them), when the member is cast to
  two types (the error names the type of the cast that raises first, which the row read again does not know), when a conjunct reads
  the member of a key taken from the leaf, or tests the member together with another test of the members, a join or a conjunct that may raise
  (the planner decides which runs first). A comparison, `IN` list or `IS NULL` test of the member alone (`WHERE CAST(e.value AS float8) > 3`)
  is the cast of every member of a row that reaches it, so the row raises before the test, whatever the test says, as the normal plan does.
  The plan stays normal for the member of one key (`LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`), whose error
  would be the row's, not the key's. Malformed documents that no unit flags (a JSON `null` list, a scalar element) are read by the clean path of the scan
  as before.
- **Several errors.** Two dirty rows, or one row with two dirty members, that raise different errors (a string and a boolean) raise the one the
  plan reaches first: a sort by the group key, a hash aggregate and the order of an access path reach the pairs in another order, and the order is
  no property of the query, so the scan cannot take the normal plan's and raises the first of its own (the rows in the order of the index's units,
  the members in the order of the document). Both raise one of the errors, never an answer; a test that needs the same message holds one kind of dirty value.
- **A text path.** A path read only as `doc ->> 'k'` (an `astext` or `text` leaf) is read again like a recordset column: a container is the JSON text `->>` gives it, a key of a document that is an array or a scalar is NULL. The text of a value cannot raise, so there is no order of errors to keep and a conjunct or a key may read the path. The plan stays normal when anything else reads the leaf, such as a `jsonb_typeof` test of the same path (a row read again has the type of its text).
- **The member of one key.** `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` with the casts of `r.value` to `float8`, `integer` or `bigint` (one type for the query, evaluated for every row that reaches the aggregates, no conjunct reads the member): the value column of the object is read again for the rows that hold a value it cannot store, and the shredder gives an error only for the member of the key `k`, the one the cast of PostgreSQL names (`cannot cast jsonb string to type integer`). The rows are found by the element with that key (`member_overflow_rows`), so a dirty member of another key raises nothing, as in the normal plan, which never casts it. The stored unit still sends every row with a dirty member of any key to the heap (the flags are per element of the column, not per key).
- **EXISTS over a list.** `EXISTS (SELECT 1 FROM jsonb_to_recordset(doc -> 'l') p(..) WHERE ..)` is a conjunct that passes the row whose list raises when it is read again (`SiftAggPred.pass_poison`); the row raises where the rows that keep it reach the aggregates. Every call that reads the list must declare the same columns that have a leaf: a call of fewer columns cannot raise for the value of another, so the query keeps the normal plan. An `EXISTS` under `OR`, a path read or `jsonb_array_length` of the same list, and `jsonb_array_elements` keep it too.
- **EXISTS over a list with a lookup per element.** `EXISTS (SELECT .. FROM jsonb_to_recordset(doc -> 'l') p(..) LEFT JOIN LATERAL (lookup of one row) m ON true WHERE ..)` tests the joined tuples, after their joins, so it raises the error of a row's list itself when it reaches that row (a row a join or another conjunct drops raises nothing), instead of passing it to the error of the rows. The unnest of the same list in the FROM keeps the error of the rows.
- **A conjunct that may raise next to an unnest of the list.** The integer cast of a member (`CAST(doc -> 'k' AS integer) = 3`) is applied to the rows that passed the other conjuncts before the error of a list is raised: a row it drops raises nothing, a row it fails for raises its own error, as the normal plan, which evaluates it with the scan of the rows before the call that reads the list.
- **A scalar path read as a cast.** A path of the document declared `float8`, `int4`, `int8` or `bool` that the query reads only as `(doc ->> 'k')::T` is read again like a record column: the text of the scalar through the input function of `T`, so a numeric string converts and a text that is no number raises `22P02`, a container and a boolean in a number path raise the same way. For `float8` the cast of the jsonb value, `(doc -> 'k')::float8`, is read the same way (a number as the cast gives it, any other JSON type the error the cast raises, `22023`). The read is an argument of an aggregate or a grouping expression, which PostgreSQL evaluates for every row that reaches the aggregates, so a row raises where it reaches the aggregate, after its conjuncts, and never when a conjunct on another column drops it. The plan stays normal when anything else reads the path (a conjunct, a join, a sub-select, a function in the `FROM`), when the cast is evaluated for some rows only (a `CASE` branch, a `FILTER`), when the path is read both ways, and for a query with an unnest or a lookup join. The key of an array or scalar document is NULL, a JSON `null` NULL (the cast of a jsonb `null` raises before PostgreSQL 18, as the normal plan does).
- **Left to the normal plan.** A list that is a JSON `null` or holds a scalar is flagged by no unit, so the scan answers where the normal plan raises, with or without this.
