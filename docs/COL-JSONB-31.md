# jsonb arrays and objects on `sift_col` and `sift_table` (3.1)

What the shredder (`docs/COL-SHRED.md`) gives today on arrays of objects, objects with dynamic keys, nested objects, arrays of scalars and
mixed types, measured against plain PostgreSQL jsonb, GIN and ClickHouse arrays; and two proposals that need a decision (`sift_table` paths,
column format minor 4). Nothing in section 4 or 5 is implemented.

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

## 3. Shapes (ms, heap plan against `SiftAgg`)

| | shape | PostgreSQL | sift | |
|---|---|---:|---:|---|
| j01 | `count(*) FILTER` per flag over `jsonb_to_recordset(items)` | 332 | 6.9 | |
| j02 | `GROUP BY e.kind`, `count`, `avg(e.score)` | 325 | 11 | clean table (1) |
| j03 | two keys, element and document predicates | 142 | 13.6 | clean table (1) |
| j04 | `EXISTS` element with two conditions | 130 | 4.5 | |
| j05 | `jsonb_path_exists(items, '$[*] ? (...)')` | 68 | 4.1 | |
| j06 | `items @> '[{"kind": .., "flag": ..}]'` (10% of the documents; GIN bitmap scan: 27.5) | 52.6 | 3.0 | |
| j07 | per-document `avg` per kind, then an outer aggregate | 1067 | 758 | the outer aggregate runs in PostgreSQL (2) |
| j08 | `count(DISTINCT d.id) FILTER (WHERE e.score >= 8)` per kind | 1165 | 11 | clean table (1) |
| j10 | `attrs #>> '{nested,a,b}'` | 60 | 4.3 | |
| j11 | `attrs->>'channel'` and `date_trunc('month', day)` | 93 | 3.7 | |
| j17 | two keys, `HAVING count(*) > 100` | 415 | 11 | clean table (1) |
| j18 | element keys joined to a lookup table | 688 | 16.5 | clean table (1) |
| j09 | `jsonb_each_text(scores)` with a regex on the value | 437 | 383 | not pushed |
| j12 | `jsonb_array_elements_text(attrs->'tags')` | 133 | 132 | not pushed |
| j13 | `attrs->'tags' ? 't3'` | 50 | 49 | not pushed |
| j14 | `(e->>'score')::float8` of `jsonb_array_elements(items)` | 468 | 483 | not pushed |
| j15 | `jsonb_array_length(items)` | 70 | 62 | not pushed |
| j16 | `jsonb_typeof(attrs->'mixed') = 'number'` | 69 | 72 | not pushed |

(1) With strings among the numbers (`strs = 0.05`) these raise `a column of the aggregate scan holds values its index cannot store`: the
fix is on the 3.0.1 branch (`fix/v301-agg`), not here. (2) Coverage gap J13 of `docs/coverage.tsv`.

The shapes that run in `SiftAgg` are 10 to 100 times faster than the heap plan (j07 aside). Against GIN on containment: 9 times faster at
10% of the documents (3.0 against 27.5 ms), level when the match is selective (three conditions: GIN 8.0, sift 8.7 ms: GIN reads a posting
list and sift scans the element columns, so a selective containment is the case where the GIN index stays useful). The ones
that do not run in it are plan-level coverage (a scalar-array unnest, `?` and `@>` on scalar arrays, `jsonb_array_length`, `jsonb_typeof`, a
cast of `->>`, a regex on a `jsonb_each_text` value): the shredder already holds what they read (list lengths are the `end[]` offsets, the
element states are the `ABSENT / NULL / VALUE / OVERFLOW` of the leaf), and each is expected to land near the pushed shapes (4 to 15 ms). They
belong to the coverage track and are not implemented here.

The documents carry absent keys (the members of `scores`), JSON nulls, nested objects, empty arrays and, with `strs`, numbers written as
strings; the differential run (on and off) is exact on the clean table for all 18 shapes. Nested arrays and elements that are not objects
are covered by the shredder tests, not by this data.

ClickHouse, `Array(Tuple(kind, sub, flag, score))` loaded from the same documents into `clickhouse-local` (2 threads, warm), j01 / j02 / j03 /
j04 shapes: 1.2 / 1.3 / 1.5 / 1.0 ms. ClickHouse is 4 to 9 times faster than `SiftAgg` at this size, and the gap is the per-element cost of the engine: 34M instructions for j01 are
88 per element for three filtered counts, 80% of them in the per-tuple selection of the engine (`agg_chunk`, `agg_selection`, the
`count_gid` scatter), not in decoding the paths (`cc_unpack` and the other codec kernels are under 5%). A dense path for an ungrouped
filtered count (the filter bitmap counted with popcount, no tuple list) is the lever; it is an engine change, independent of jsonb, and
not done here.

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
- **Expected.** The `sift_col` numbers of section 3 without a second copy of the columns: j01 354 to about 7 ms, j02 408 to about 11 ms,
  j10 42 to about 4 ms (50 times), at the cost of the shredded leaves next to the compressed bytes (the index of section 2 holds all of them
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
- **Expected.** j02 and j17 at 100,000 documents from 11 ms to under 1 ms (28 units, a few microseconds each), and the cost grows with
  the units, not the elements: 10,000,000 documents read about 2,800 sidecars. Queries with a predicate on another leaf are scanned as
  now. The sidecar grows by at most the 2% budget (+1.2% measured for top-level cubes), the build by about 4%.
- **Without a format change.** The exact leaf sums the sidecar already stores for the children of lists answer the ungrouped element
  aggregates (`count(*)`, `sum(e.score)`, `avg(e.score)` with no filter) from metadata; the executor's pre-aggregate shape rules today
  exclude an unnest. That is a coverage change on the engine side and is left to that track.

## 6. Reproducing

```
createdb jb && psql jb -c 'CREATE EXTENSION pg_sift'
psql jb -v n=100000 -v strs=0 -f bench/sql/jsonb/gen.sql -f bench/sql/jsonb/index.sql
# each bench/sql/jsonb/queries/*.sql with sift.enable_agg on and off, max_parallel_workers_per_gather = 0
```
