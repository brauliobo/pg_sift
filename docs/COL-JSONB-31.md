# jsonb arrays and objects on `sift_col` and `sift_table` (3.1)

What the shredder (`docs/COL-SHRED.md`) gives today on arrays of objects, objects with dynamic keys, nested objects, arrays of scalars and
mixed types, measured against plain PostgreSQL jsonb, GIN and ClickHouse arrays; the three shapes added in 3.1 and why three others stay
in the normal plan; and proposals that need a decision (`sift_table` paths, column format minor 4). Nothing in sections 4 and 5 is
implemented.

## 1. Method

`bench/sql/jsonb/gen.sql` writes synthetic documents (`items[]{kind,sub,flag,score}` with 0 to 8 elements, `scores.*` with 2 to 7 of 20
dynamic keys, `attrs{channel, nested.a.b, tags[], mixed}`); about 3% of the elements are dropped, some `score` values are JSON null, and
`strs` (default 0) writes that share of numbers as strings in the numeric paths. `index.sql` is the `sift_col` index
(`paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, scores.* float8, items[]{kind text, sub text, flag text, score float8}'`)
and the GIN twins; `queries/j01..j18` are the shapes of section 3. 100,000 documents, 387,593 elements, PostgreSQL 18.6, one backend
(`max_parallel_workers_per_gather = 0`), best of 3, warm. Every query returns the same rows with `sift.enable_agg` on and off.

## 2. Build time and size (100,000 documents)

| | size | build |
|---|---:|---:|
| heap table, jsonb inline | 56 MB | |
| `sift_col` on `id, grp, day, attrs, scores, items` with the paths above | 2.3 MB | 609 ms |
| GIN `jsonb_path_ops` on `items`, `attrs`, `scores` | 2.6 + 1.5 + 2.1 = 6.2 MB | 336 + 132 + 183 = 651 ms |
| the same documents as a `sift_table`, no paths (exact bytes, compressed) | 11 MB | |
| ClickHouse `Array(Tuple(...))` of the items only, for scale | 4.6 MiB | |

The index holds three plain columns and the typed paths of the three jsonb columns in a third of the bytes of the GIN indexes, which answer
containment and nothing else. The `sift_table` is five times smaller than the heap because the jsonb bytes compress.

## 3. Shapes (ms, normal plan against `SiftAgg`)

A clean table (`strs = 0`), 100,000 documents, best of 5 on a busy host (the ratios are the finding; ClickHouse and the build figures of
section 2 come from the same documents).

| | shape | PostgreSQL | sift | |
|---|---|---:|---:|---|
| j01 | `count(*) FILTER` per flag over `jsonb_to_recordset(items)` | 175 | 3.7 | |
| j02 | `GROUP BY e.kind`, `count`, `avg(e.score)` | 192 | 4.9 | |
| j03 | two keys, element and document predicates | 75 | 3.7 | |
| j04 | `EXISTS` element with two conditions | 88 | 2.4 | |
| j05 | `jsonb_path_exists(items, '$[*] ? (...)')` | 45 | 2.2 | |
| j06 | `items @> '[{"kind": .., "flag": ..}]'` (10% of the documents; GIN bitmap scan: 27.5) | 23 | 2.4 | |
| j07 | per-document `avg` per kind, then an outer aggregate | 592 | 284 | the outer aggregate runs in PostgreSQL (1) |
| j08 | `count(DISTINCT d.id) FILTER (WHERE e.score >= 8)` per kind | 514 | 5.6 | |
| j10 | `attrs #>> '{nested,a,b}'` | 47 | 2.1 | |
| j11 | `attrs->>'channel'` and `date_trunc('month', day)` | 53 | 2.3 | |
| j12 | `jsonb_array_elements_text(attrs->'tags')` grouped by the tag | 85 | 2.6 | 3.1: `agg: jsonb: jsonb_array_elements_text` |
| j13 | `attrs->'tags' ? 't3'` | 33 | 1.8 | 3.1: `?`, `?\|`, `?&`, `@>` on an array of strings |
| j15 | `jsonb_array_length(items)` as a key | 42 | 3.0 | 3.1: the length is a leaf |
| j17 | two keys, `HAVING count(*) > 100` | 233 | 5.1 | |
| j18 | element keys joined to a lookup table | 317 | 9.1 | |
| j09 | `jsonb_each_text(scores)` with a regex on the value | 229 | 254 | not pushed (2) |
| j14 | `(e->>'score')::float8` of `jsonb_array_elements(items)` with a regex on `e->>'score'` | 257 | 263 | not pushed (2) |
| j16 | `jsonb_typeof(attrs->'mixed') = 'number'` | 46 | 44 | not pushed (3) |

(1) Coverage gap J13 of `docs/coverage.tsv`. With strings among the numbers (`strs = 0.05`) the aggregates that read a numeric path raise
`a column of the aggregate scan holds values its index cannot store`: the fix is on the 3.0.1 branch (`fix/v301-agg`), not here.

The shapes that run in `SiftAgg` are 10 to 100 times faster than the normal plan (j07 aside). Against GIN on containment: 9 times faster at
10% of the documents (2.4 against 27.5 ms), level when the match is selective (three conditions: GIN 8.0, sift 8.7 ms: GIN reads a posting
list and sift scans the element columns, so a selective containment is the case where the GIN index stays useful).

The documents carry absent keys (the members of `scores`), JSON nulls, nested objects, empty arrays and, with `strs`, numbers written as
strings; the differential run (on and off) is exact on the clean table for all 18 shapes. Nested arrays and elements that are not objects
are covered by the shredder tests, not by this data.

ClickHouse, `Array(Tuple(kind, sub, flag, score))` loaded from the same documents into `clickhouse-local` (2 threads, warm), j01 / j02 / j03 /
j04 shapes: 0.6 / 0.6 / 0.8 / 0.65 ms, 4 to 6 times faster than `SiftAgg`. The gap is the per-element cost of the engine: in the profile of
j01, 34M instructions are 88 per element for three filtered counts, 80% of them in the per-tuple selection (`agg_chunk`, `agg_selection`, the
`count_gid` scatter), not in decoding the paths (`cc_unpack` and the other codec kernels are under 5%). A dense path for an ungrouped
filtered count (the filter bitmap counted with popcount, no tuple list) is the lever; it is an engine change, independent of jsonb, and
not done here.

### The shapes added in 3.1 (test `agg_jsonb_array`, no format change)

- **j12, `jsonb_array_elements_text`.** The element of an array of strings is the one unnamed child of the array leaf; the text column of
  the function is that child. Keys, `count(DISTINCT t)`, `LIKE`, `length(t)` and filters on the tag run on the column.
- **j13, `?`, `?|`, `?&`, `@>`, `<@`.** An existence test over the elements of the array leaf, the shape of the containment of an array
  of objects: for each (`?&`, `@>`) or one (`?|`) of the constant strings an element equals it. Kept in the normal plan: a negation (the
  document may be NULL), a JSON null, a nested array or an object in the contained array, a number against a text element.
- **j15, `jsonb_array_length`.** A leaf after the source's, an int4 whose values are the element counts of the array leaf (like the member
  of an object by key); it works as a key, in a filter, inside an expression and as the argument of `sum`, `min`, `max`, `count`.
  Differences from the normal plan, only on documents the plan rejects with an error: a JSON null in the array's place counts as absent
  (the column holds NULL for both), where `jsonb_array_length('null')` raises "cannot get array length of a scalar"; a scalar or an object
  there is a value the index cannot hold and raises the overflow error of the pushdown.
- **A value the text leaf cannot hold** (a number or a boolean among the strings, an object where the array is) raises the error of the
  pushdown, whose hint turns it off; the normal plan answers (the last statements of the test show both).

### Why the others stay in the normal plan

- **j09, j14: a regular expression (or any text function) on the text of a number.** `jsonb_each_text(scores)` and `e->>'score'` give the
  text of the jsonb numeric (`7.50` is `7.5`, `1e2` is `100`); the leaf declared `float8` holds the double, from which that text cannot
  be rebuilt (a number of more than 15 digits, `0.1 + 0.2`, an integer over 10^15 print differently), and the shredder refuses to store a
  number in a text leaf (`OVERFLOW TYPE`) because `"12"` and `12` differ for `=`, `@>` and jsonpath. A rule that only looks at the
  digits (`'^[0-9.]+$'`, which only a non-negative number matches) could be answered from the double, but a planner that recognizes
  patterns would be a guess, not a proof. The cast `s.value::float8` alone is pushed already (the `value` leaf), and so is a text
  comparison on a leaf declared `text`. Proposal below (a path type that keeps the `->>` text of any scalar).
- **j16, `jsonb_typeof(x) = 'number'`.** The JSON type of a value is not in the index: a string in a number path is an `OVERFLOW` slot
  (NULL with the overflow bit), and the bit does not say whether the value is a string, a boolean, an object or a number out of range.
  Without the type the filter cannot tell the rows it must keep; the rows would need the heap, and the aggregate scan has no recheck of an
  overflow row (the 3.0.1 branch adds one for float8 paths: the rows of a `float8` path with strings are rechecked, which is the same
  gap seen from the aggregate). The type tag of a slot (two bits per value of a path: string, number, boolean, null, object, array) is a
  column format change (section 5) and is not done.

## 4. Proposal: paths for `sift_table` (no format change)

Today a `sift_table` keeps a jsonb column as one opaque leaf (`docs/COL-TAM.md`, layout of a group), so `SiftAgg` over it runs PostgreSQL's
`jsonb_to_recordset` on every row: j01 354 ms, j02 408 ms, j10 42 ms on the 11 MB table of section 2, the same as the heap. The column
segments are self-describing (a segment carries its schema, which already lists "table columns + shredded leaves"), so the segment format
can hold the typed paths next to the exact bytes as it is.

- **Declaration.** A table access method has no reloptions, so: `sift.table_set_paths(tbl regclass, col name, spec text)`, the spec being
  the `paths` grammar for one column, stored in an extension table `sift.table_paths(relid, attnum, spec)` that an event trigger clears with
  the table. Like `sift.table_compression`, a change applies to the segments written afterwards (promotion, merge, rewrite).
- **Writing.** Promotion, merge and VACUUM rewrite call the shredder for the declared columns and add the leaves to the segment schema;
  the original bytes stay in the VAR leaf, so every other query is as before.
- **Reading.** The scan source of a table (`sift_aggcol.c` for a `sift_col` index) maps path expressions to leaves per segment. A scan can use
  a path only if every segment it reads has the leaf: old segments and the write buffer do not, so the planner uses the paths when the
  declaration is older than all the segments (the write buffer is read as rows and shredded on the fly), and a VACUUM merge rewrites the
  old ones with the new schema. Until then the plan is today's.
- **Expected.** The `sift_col` numbers of section 3 without a second copy of the columns: j01 354 to about 4 ms, j02 408 to about 5 ms,
  j10 42 to about 2 ms (20 to 90 times), at the cost of the shredded leaves next to the compressed bytes (the index of section 2 holds all of them
  in 2.3 MB for the three columns, so about 4% of the 56 MB heap size, 20% of the 11 MB table).
- **Risks.** Statistics and EXPLAIN need the path names; schema evolution per segment is already handled by the layout code (segments of
  another layout are read side by side); `ALTER TABLE ... SET ACCESS METHOD` and dumps must carry the declaration (a function call after the
  data in the dump, or the extension table as a configuration table).

## 5. Proposal: element cubes in the pre-aggregate sidecar (column format minor 4, needs a decision)

The sidecar of a unit (`docs/COL-PREAGG.md`) keeps a histogram per top-level dimension with the count and the sum of up to three
measures. A list-element dimension has none: `GROUP BY e.kind` with `count(*)` and `avg(e.score)` (j02, j17 without a per-document
predicate) scans every element.

- **Change.** Allow a dimension and its measures to be leaves of one group (`items[].kind` with `items[].score`): the histogram counts
  elements, not rows. Same encoding, same cap (256 groups, 2% of the unit's encoded bytes, 4096 with an explicit `cube(...)`); a new
  section kind in the `COLP` directory for the group the histogram counts. Segments of minor 3 stay readable: their units scan as today.
  A minor 3 library refuses minor 4 segments (the existing `REINDEX` hint), which is why it is a decision and not a patch.
- **Expected.** j02 and j17 at 100,000 documents from 5 ms to under 1 ms (28 units, a few microseconds each), and the cost grows with
  the units, not the elements: 10,000,000 documents read about 2,800 sidecars. Queries with a predicate on another leaf are scanned as
  now. The sidecar grows by at most the 2% budget (+1.2% measured for top-level cubes), the build by about 4%.
- **Without a format change.** The exact leaf sums the sidecar already stores for the children of lists answer the ungrouped element
  aggregates (`count(*)`, `sum(e.score)`, `avg(e.score)` with no filter) from metadata; the executor's pre-aggregate shape rules today
  exclude an unnest. That is a coverage change on the engine side and is left to that track.

### Also needing a decision: the text of any scalar (j09, j14) and the type of a value (j16)

- **A path type that keeps the `->>` text of any scalar** (name open; today `text` holds strings only, and a number there is an
  `OVERFLOW`). No new storage class: the leaf is a dictionary string column like `text`, written with the text a number or a boolean has in
  `->>` (`numeric_normalize`, `true`, `false`); what changes is the meaning, so the index definition gets a flag (`SiftColDef.flags`, not
  the segment) that tells the planner such a leaf answers `->>` and `jsonb_each_text` (text equality across types) and never a jsonpath, `@>` or
  `?` string test, where `"12"` and `12` differ. With it `s.value ~ '^[0-9.]+$'` and `e->>'score' ~ ...` become a lookup table over the
  dictionary, evaluated by PostgreSQL once per distinct text, and the cast to `float8` is evaluated per distinct text as any expression.
  Expected: j09 and j14 from 229 / 257 ms to the 5 to 10 ms of the other grouped shapes. Cost: the dictionary of the numbers' text, which
  is large for numbers with many distinct decimals.
- **A type tag per slot** (column format minor 4): two bits per value of a path (string, number, boolean, null, object or array, missing)
  stored with the overflow bitmap. `jsonb_typeof(path)` and the strings among the numbers of a path become exact (j16, and the 3.0.1
  recheck would not be needed). Cost: about 3 bits per overflowing value, nothing for a path without overflow.

## 6. Reproducing

```
createdb jb && psql jb -c 'CREATE EXTENSION pg_sift'
psql jb -v n=100000 -v strs=0 -f bench/sql/jsonb/gen.sql -f bench/sql/jsonb/index.sql
# each bench/sql/jsonb/queries/*.sql with sift.enable_agg on and off, max_parallel_workers_per_gather = 0
```
