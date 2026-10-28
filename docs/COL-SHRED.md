# sift_col shredder: jsonb to typed columns

The shredder turns a `jsonb` document into typed column values for a list of declared paths. `src/col/shred.[ch]` (portable, `src/portable-col.list`) is the only code that knows JSON; `src/col/shred_pg.[ch]` drives it from a `JsonbContainer`. The storage and the planner see typed arrays, element offsets and per-value states.

## Declaring paths (reloption `paths`)

```
paths = 'attributes->locale text, metrics.* float4,
         items[]{category text, sku text, kind text, qty int4, price float4}, notes[]{type text, author_kind text}'
```

```
spec  := item { ',' item }
item  := path ( type | '{' item { ',' item } '}' )     -- a group is relative to the element its path ends with
path  := step { sep step }        sep := '.' | '->'    -- the two separators are the same
step  := key | key '[]' { '[]' } | '*'
key   := bare ([A-Za-z0-9_$] and bytes >= 0x80) | "quoted" ("" is a double quote)
type  := bool | int2 | int4 | int8 | float4 | float8 | text [ '(' N ')' ] | astext [ '(' N ')' ]
```

Aliases: `boolean`, `smallint`, `int`, `integer`, `bigint`, `real`, `double [precision]`. A path starts with a key. `key[]` is an array
of the following element (`tags[] text`: an array of scalars; `items[].sku text` or `items[]{sku text}`: an array
of objects; `matrix[][] float4`: an array of arrays of scalars). `*` is every member of an object (`metrics.* float4`; `stats.*{n int4, avg float8}`
members that are objects). A wildcard cannot share its object with explicit keys. A path may be declared once; items that share a
prefix share the node (`a.b int4, a.c text`, `a[].x int4, a[].y text`). Errors name the position
(`ERRCODE_SYNTAX_ERROR`) or the conflict (`ERRCODE_INVALID_PARAMETER_VALUE`, `ERRCODE_PROGRAM_LIMIT_EXCEEDED`).

`sift_shred_spec_deparse` gives the canonical form, one flat item per column in declaration order (`attributes.locale text,
metrics.* float4, items[].category text, ...`), always with `.`; `fingerprint` is the `sift_term_hash64` of the canonical text; a segment stores both, so a
changed spec is told from the one the segment was built with. Limits: 256 columns, 64 groups, 16 steps per path, 64 KiB of spec.

`sift.suggest_paths(table, column, sample => 2000)` writes this spec from the documents of a `jsonb` column (advice only; NULL when no key qualifies). The documents come from 16 stretches of blocks evenly spaced over the table. A key becomes a typed path when most of its values are one scalar type (`astext` for a mix of numbers, booleans and text); a map of more than 4 keys of one type becomes a wildcard (it serves `jsonb_each` and every named key); an array of objects becomes a group (the root of a document may be that array). A key in fewer than 1% of the documents, without a single shape, or a text whose values average more than 64 bytes (free text is no filter or group key) is left out. It refuses a table the caller cannot read and one with row-level security.

An index with a `jsonb` column and no `paths` option (or `paths = 'auto'`) takes that answer at `CREATE INDEX`: the build reads the sample, writes the spec to the option as `auto: spec` (so the build, its workers and every later `REINDEX` share one schema; `pg_dump` restores it as written) and says so in a NOTICE. Paths written by hand are never touched. When no key qualifies (an empty table, documents without a repeated scalar key) nothing is written and the column is refused. To read the documents again, `ALTER INDEX ... SET (paths = 'auto')` (or `RESET (paths)`) and `REINDEX` at once: as after any change of `paths` the index has no layout between the two, and a write to the table fails until the `REINDEX` (`col_paths_auto`).

## Output model (Dremel / DuckDB LIST)

A **group** is a repeated level: the root (exactly one element, the document), every `[]` and every `*`. Every **column** (a typed
leaf) belongs to one group and has one slot per element of that group, in document order:

| per column | meaning |
|---|---|
| `state[i]` | `ABSENT` (path missing or a scalar/null on the way), `NULL` (JSON null), `VALUE`, `OVERFLOW` |
| `val[i]` | 8 bytes: `i` (bool 0/1, int2/4/8), `f` (float4 is the exact float4 widened to double, float8), `s.off/s.len` into `doc.text` (text, UTF-8, no NUL) |
| `ovf` | OR of the `SIFT_SHRED_OVF_*` reasons seen in the document for this column, also from lists below that overflowed |

| per group | meaning |
|---|---|
| `n` | number of elements in the document |
| `end[p]`, `lstate[p]` | for each element `p` of the **parent** group: the elements of this group that belong to `p` are `[end[p-1], end[p])` (`end[-1]` = 0), and the state of the list itself: `ABSENT` (key missing), `NULL` (JSON null), `VALUE` (an array / object, possibly empty), `OVERFLOW` (not a list, or cut short) |
| `keyid[i]` | maps only (`*`): the dictionary id of the member name |

Elements keep their array position: a scalar, null or nested array inside an array of objects is an element whose columns are
`ABSENT` (scalar, null) or `OVERFLOW` (array), so offsets equal jsonb array indexes. Correlation is by construction: all columns of a
group at element `i` come from the same JSON object, so `EXISTS (one element with category = x AND kind = y)` is a scan of the
group's slots `i` and never mixes elements; `sift_shred_expand_parents` gives the per-element parent ordinal when a flat parent column
is easier. Nested groups chain through the same `end` arrays.

Absent versus null, in SQL terms: `doc #>> path IS NULL` is `ABSENT` or `NULL`; `doc #> path` is a JSON null (`jsonb_path_exists`, the `?`
operator) for `NULL` and a missing member for `ABSENT`; an empty array is a list that is `VALUE` with no elements, a missing or null array is
`ABSENT` or `NULL`.

Order is stable and deterministic: columns in declaration order, elements in array order, map members in jsonb storage order (key
length, then bytes), dictionary ids in order of first appearance across the documents shredded through one `SiftShred`.

### Wildcard dictionary

`*` members are `(keyid, value)` pairs of a map group. Ids are the codes of a col-core `SiftCDict` (`col/dict.c`) created with the
shredder (`sh->dict`): first-seen order, `sift_cdict_str` gives the bytes of an id. A key over 255 bytes (`SIFT_COL_DICT_MAX_ENTRY`), or beyond `max_keys`
(default 65536), gets `SIFT_SHRED_NO_KEY` and an `OVERFLOW` value: the member is still counted, never silently dropped.

## Types and the overflow rule

The shredder never errors on data and never converts to fit: a value that is not exactly representable in the declared type is
recorded as `OVERFLOW` (with a reason) and the column is flagged, so a consumer must not prune with a zone map or a presence bitmap
of that column for that block, and recheck the row from the heap. Elements already emitted are correct; an `OVERFLOW` list is
incomplete or unrepresentable.

| declared | JSON | result |
|---|---|---|
| int2 / int4 / int8 | number with an integral value in range (`3`, `3.0`, `1e2`) | `VALUE` |
|  | `3.5`, out of range | `OVERFLOW` `RANGE` (SQL would round with the jsonb cast and fail with the text cast) |
| float4 / float8 | any number whose conversion does not overflow | `VALUE`: the correctly rounded value of the decimal text, i.e. `strtof` / `strtod`, which is what `float4in` / `float8in` do, hence equal to `(doc #> path)::float4`, `(doc #>> path)::float4`, and to `jsonb_to_recordset` |
|  | rounds to infinity (`1e400`) or to zero from a non-zero value (`1e-400`) | `OVERFLOW` `RANGE` (PostgreSQL raises "out of range"); denormals are values |
| bool | `true` / `false` | `VALUE` |
| text | string, at most `N` bytes | `VALUE` |
|  | longer | `OVERFLOW` `LENGTH` |
| astext | a string (as it is), a number (the text `numeric_out` prints, which is what `->>` gives: `1.50` stays `1.50`, `1e2` is `100`), `true`, `false`; at most `N` bytes like `text` | `VALUE` |
|  | an object or array, longer | `OVERFLOW` `SHAPE`, `LENGTH` |
| any | JSON null | `NULL` |
| any scalar type but astext | a different JSON scalar (string in a number column, number in a text column, ...) | `OVERFLOW` `TYPE` (no coercion: `->>` would give `'12'` for the number 12) |
| any scalar type | an object or array | `OVERFLOW` `SHAPE` |
| object position | array | all columns below `OVERFLOW` `SHAPE` (lax jsonpath would unwrap it) |
| object position | scalar, null | columns below `ABSENT` |
| list position | an object / scalar (lax jsonpath would wrap it) | list `OVERFLOW` `SHAPE`, columns below flagged |
| list over `max_elems` / `max_bytes` |  | the rest of the list is dropped, list `OVERFLOW` `TRUNC`, columns below flagged |

**The type of an overflow value.** An `OVERFLOW` slot also keeps the JSON type of the value in `tag[i]` (`SIFT_COLTAG_STRING`, `NUMBER`, `BOOLEAN`, `OBJECT`, `ARRAY`: the event that delivered it, so a number out of range is a number and a string in a number column is a string). A leaf below the array that sits at an object position has `SIFT_COLTAG_NONE`: its path leads to no value, as for `->`. Where the leaf is not what the event delivered, and for what a writer cannot know (a heap column, a `sift_table` segment), the tag is `UNKNOWN` (0). The writers store the tags of overflow slots in the chunks of column format minor 4; `jsonb_typeof(path)` is answered from them (docs/COL-PLAN.md, "jsonb_typeof"). A `NULL` slot has `SIFT_COLTAG_JNULL` (7), a JSON null as against a missing key; the `VALUE` of an `ASTEXT` leaf has the tag of the value it was (`NUMBER`, `BOOLEAN`; a string and the other leaf types have 0); `ABSENT` has none.

A slot that is not `VALUE` has a zero value that means nothing: NULL is never a sentinel, the states (or `state_bits`) say which slots are NULL. A codec that takes the `int64` keys of a column plus a NULL bitmap needs nothing else from the shredder (integers are the keys as they are; floats are exact doubles for the engine's order keys; text is bytes for its dictionary; map key ids and parent ordinals are plain integers, `end[]` is non-decreasing).

Negative zero is stored as `+0` (numeric has none), so no NaN and no `-0` is ever produced. Integer sums, `min`, `max`, `count` over
`VALUE` slots are exact. Float aggregates equal plain SQL when the aggregator uses the same transition functions in the same
order (`float4pl` in float4, `float8pl` in float8; a float4 column summed in float8 differs from `sum(float4)` exactly as in SQL).

Numbers arrive as the decimal text of the jsonb numeric (`numeric_normalize`: no exponent, no trailing fraction zeros). Integers are parsed from the digits, exact up to
19 digits; floats go through `strtof` / `strtod` (the process keeps `LC_NUMERIC` at "C", as PostgreSQL does), with a shortcut for
integers that are exact in the type.

### jsonpath `!=`, `!()` and `@>` with a JSON null (tests in the fixtures `jp_*`, `t_path_*`, `t_contains_null`; no new format)

- **Why the kinds.** `@.k != "x"` is true for a JSON null member and false for a missing key, and `!(@.k == "x")` is true for both. The slot of a
  leaf is NULL in both cases; the kinds of column format minor 4 (`docs/FORMATS.md`, column minor 4) tell them apart, so the source reads the JSON nulls of the leaf
  (`SNULL_JSON`) and the missing keys (`SNULL_MISSING`: NULL and not a JSON null) next to the values.
- **Forms.** For a literal of the JSON type of the member's leaf (text, integer, boolean): `!=`, `!()` over a comparison, `&&` and `||` of
  them, a `null` literal (`== null`, `!= null`, under `!()`), variables of a constant `vars` (`$x`), a strict path without `!()` (strict and lax
  differ for these only under `!()`), and `@>` with a JSON null in an object. A path of an element of an array of objects only.
- **What stays on the normal plan.** A literal of another JSON type than the leaf's (the other values are unknown, the overflow ones would not be
  read); a number against a `float4` / `float8` leaf (the leaf holds the rounded value, `100.00000000000000001 > 100` is true for jsonpath); strict
  `!()`; `jsonb_path_match` and `@@`; `@>` with a float, `{}` or a null among scalars.
- **Old units.** A unit written by minor 3 has no kinds: the test raises the `REINDEX` error of the other type tests rather than answering.

## Memory

A document never materializes: the jsonb iterator keeps one frame per nesting level, subtrees no declared path reaches are skipped, and output is bounded by `SiftShredLimits`: `max_elems` list elements per document (default 1 Mi) and `max_bytes` of text per document (default 16 MiB). The `SiftShredDoc` arrays are reused between documents and released when a document grew them past 64 Ki elements.

## C interface

```c
SiftShredSpec *sift_shred_spec_parse(const char *text);          /* errors raise; allocated in the current context */
const char    *sift_shred_spec_deparse(const SiftShredSpec *);   /* canonical; spec->fingerprint */
SiftShred     *sift_shred_create(const SiftShredSpec *, const SiftShredLimits *, bool cached);  /* own memory context (cached: named "sift cache shred", for a relcache lifetime), NULL limits = defaults */
void           sift_shred_free(SiftShred *);

/* portable event interface: any driver (the jsonb iterator, a JSON text parser) */
void sift_shred_begin(SiftShred *);                 /* next document; the dictionary survives */
SiftShredKey sift_shred_key(SiftShred *, const char *key, int len);   /* SKIP | TAKE | STOP (the rest of the object is irrelevant) */
bool sift_shred_enter(SiftShred *, bool is_array);  /* a container value; false: do not descend */
void sift_shred_leave(SiftShred *);                 /* after every enter that returned true */
bool sift_shred_wants_number(const SiftShred *);    /* false: pass sift_shred_number(sh, NULL, 0) */
void sift_shred_null(SiftShred *);  sift_shred_bool(sh, b);  sift_shred_string(sh, s, len);  sift_shred_number(sh, text, len);
bool sift_shred_full(const SiftShred *);            /* the current list is cut short: stop iterating it */
void sift_shred_finish(SiftShred *);

/* PostgreSQL driver (col/shred_pg.h) */
void sift_shred_jsonb(SiftShred *, Jsonb *);                 /* begin, the document, finish */
void sift_shred_container(SiftShred *, JsonbContainer *);    /* one jsonb as the next value (see below) */

/* the output: sh->doc.col[c], sh->doc.grp[g], sh->doc.text; helpers: */
void sift_shred_group_range(doc, g, p, &lo, &hi);      void sift_shred_expand_parents(doc, g, parent);
```

Driver contract: object keys are unique; every value event follows a `key` that returned `TAKE` or `STOP`, or is an array element, or is the root; `leave` follows every `enter` that returned true. The shredder never reads ahead, so the caller can wrap a tuple's events: `sift_col` shreds the object `{attribute name: jsonb value, ...}` of a tuple (spec paths start with the index attribute name) with `begin`, `enter(object)`, `key(attname)`, `sift_shred_container(attribute's jsonb)`, `leave`, `finish`. A SQL NULL attribute is not sent (its columns stay `ABSENT`).

The jsonb driver walks with `JsonbIteratorNext(skipNested)`: unwanted members are not entered, an object is left as soon as its last declared key was seen (`STOP`), a cut list stops the iteration. Numbers are rendered with `numeric_normalize` only when the target is a numeric column (`wants_number`).

## SIMD kernels

Dispatch goes through `sift_cpu_register` (`sift.simd`, registered by the first `sift_shred_create` or `sift_shred_init()`); `sift_shred_kernels_for(level)` is for tests. Detection is at run time, so one library runs on any x86-64. On AArch64 `col/shred_kern_neon.c` is the NEON level (compiled in, nothing to detect); elsewhere the scalar table is used.

Kernels: `state_counts` (histogram of the four states of a column), `state_bits` (the NULL and overflow bitmaps of a column, for the engine's chunk header), `minmax_i64` / `minmax_f64` (zone map over the `VALUE` slots). The offsets to per-element parent ordinals (`sift_shred_expand_parents`) are the column table's `expand_parents`: one implementation for the shredder, the column scan and the aggregation executor. Number parsing is not dispatched: the digit runs are a few bytes, shorter than an indirect call.

## Measured

`bench/bench shred`, one host (PostgreSQL 18, the testing build, `-march=x86-64` baseline with run-time dispatch), best of 3 rounds with other jobs running: the figures are lower bounds, the ratios indicative.

| workload | shredder | plain SQL |
|---|---|---|
| orders of about 1 KiB, 9 columns in 3 groups (`items[]`, `notes[]`, `tags[]`, `metrics.*`), documents/s | 251 k | 2.0 M reading one key, 726 k `jsonb_to_recordset` of `items` only |
| 500 objects x 5 columns per document, elements/s | 3.2 M | 2.0 M `jsonb_to_recordset` |
| 82 keys, 3 declared paths, documents/s | 434 k | 616 k one `#>>` |

Kernels, 10^9 elements/s, scalar / AVX2 / AVX-512, 4096 elements (cache) and 4 Mi elements (memory):

| kernel | 4096 | 4 Mi |
|---|---|---|
| `state_counts` | 0.63 / 22.5 / 27.5 | 0.57 / 25.3 / 16.2 |
| `state_bits` | 0.95 / 34.4 / 57.7 | 0.81 / 35.1 / 36.4 |
| `minmax_i64` | 0.93 / 4.3 / 10.2 | 0.50 / 2.0 / 1.1 |
| `minmax_f64` | 0.66 / 6.5 / 8.3 | 0.55 / 2.0 / 1.1 |
| `expand_parents` (runs of 4; now `SiftColKernels.expand_parents`) | 2.46 / 2.24 / 3.04 | 2.37 / 2.27 / 1.32 |

`expand_parents` gains nothing from vectors on runs shorter than a register; on a column that does not fit the cache, AVX2 is as fast or faster than AVX-512 (memory bound on this host).

On AArch64 (clang `-O2`, one thread, best of 300 batches and of three runs), 10^9 elements/s, scalar / NEON:

| kernel | 4096 | 4 Mi |
|---|---|---|
| `state_counts` | 0.86 / 35 | 0.82 / 35 |
| `state_bits` | 2.3 / 48 | 2.1 / 46 |
| `minmax_i64` | 1.5 / 2.3 | 1.4 / 2.2 |
| `minmax_f64` | 0.82 / 3.6 | 0.78 / 3.5 |

The NEON kernels count with byte compares (a byte lane takes 255 vectors before it is added up), build bitmaps with the pairwise-add movemask of `core/neon.h`, and select `VALUE` lanes of min / max with `BSL` (`FMIN`/`FMAX` for doubles, never NaN; integers compare and select, NEON has no 64-bit min).

### Arrays of objects, dynamic keys and nested objects (3.1)

Historical 3.1 baseline: 100,000 synthetic documents (387,593 elements: `items[]{kind,sub,flag,score}`, `scores.*` with 2 to 7 of 20 dynamic keys, `attrs{channel, nested.a.b, tags[], mixed}`), PostgreSQL 18.6, one backend (`max_parallel_workers_per_gather = 0`), best of 5, warm, a busy host: the ratios are the finding. `bench/sql/jsonb/` has the generator, the index (`paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, scores.* float8, items[]{kind text, sub text, flag text, score float8}'`) and the queries `count-filter` to `typeof-filter`; each runs with `sift.enable_agg` on and off (equal rows). `index_typeof.sql` adds `attrs.mixed float8` for mixed-numeric, and `bench/sql/jsonb/rollup/` measures the element rollups ([COL-PREAGG.md](COL-PREAGG.md)).

|  | size | build |
|---|---:|---:|
| heap table, jsonb inline | 56 MB |  |
| `sift_col` on `id, grp, day, attrs, scores, items` with the paths above | 2.3 MB | 609 ms |
| GIN `jsonb_path_ops` on `items`, `attrs`, `scores` | 6.2 MB | 651 ms |
| the same documents as a `sift_table`, no paths | 11 MB |  |

|  | shape | PostgreSQL ms | sift ms |
|---|---|---:|---:|
| count-filter | `count(*) FILTER` per flag over `jsonb_to_recordset(items)` | 175 | 3.7 |
| group-elem | `GROUP BY e.kind`, `count`, `avg(e.score)` | 192 | 4.9 |
| group-two-filter | two keys, element and document predicates | 75 | 3.7 |
| exists-elem | `EXISTS` element with two conditions | 88 | 2.4 |
| jsonpath-exists | `jsonb_path_exists(items, '$[*] ? (...)')` | 45 | 2.2 |
| contains | `items @> '[{"kind": .., "flag": ..}]'` (10% of the documents; GIN bitmap scan: 27.5) | 23 | 2.4 |
| per-parent-avg | per-document `avg` per kind, then an outer aggregate (the outer one runs in PostgreSQL) | 592 | 284 |
| distinct-parent-filter | `count(DISTINCT d.id) FILTER (WHERE e.score >= 8)` per kind | 514 | 5.6 |
| object-each | `jsonb_each_text(scores)` with a regex on the value (`float8` paths: not pushed; with `astext` paths, `index_astext.sql`: 7.0) | 229 | 254 |
| nested-scalar | `attrs #>> '{nested,a,b}'` | 47 | 2.1 |
| channel-month | `attrs->>'channel'` and `date_trunc('month', day)` | 53 | 2.3 |
| tags-unnest | `jsonb_array_elements_text(attrs->'tags')` grouped by the tag | 85 | 2.6 |
| tags-contains | `attrs->'tags' ? 't3'` | 33 | 1.8 |
| score-cast-text | `(e->>'score')::float8` of `jsonb_array_elements(items)` with a regex on `e->>'score'` (`float8` paths: not pushed; `astext` paths: 6.6) | 257 | 263 |
| array-length | `jsonb_array_length(items)` as a key | 42 | 3.0 |
| mixed-numeric | `jsonb_typeof(attrs->'mixed') = 'number'` (column format minor 4) | 100 | 2.1 |
| item-agg-having | two keys, `HAVING count(*) > 100` | 233 | 5.1 |
| join-lookup | element keys joined to a lookup table | 317 | 9.1 |

The pushed shapes are 10 to 100 times faster than the normal plan. Against GIN on containment: 9 times faster at 10% of the documents (contains), level when the match is selective (three conditions: GIN 8.0, sift 8.7 ms), where GIN stays useful. ClickHouse `Array(Tuple(kind, sub, flag, score))` from the same documents (`clickhouse-local`, 2 threads) answers count-filter to exists-elem in 0.6 to 0.8 ms, 4 to 6 times faster than `SiftAgg`: the gap is the per-tuple selection (80% of 88 instructions per element in count-filter), not the decoding. Strings among the numbers (`strs = 0.05` of `gen.sql`) raise the overflow error of the pushdown for the aggregates that read a numeric path, except for the type filters of mixed-numeric (33 times faster than the normal plan at 1,000,000 documents: 396 against 11.9 ms).

## Integration notes

- `expand_parents` is the column table's. `state_counts`, `state_bits` and `minmax_*` overlap with the engine's kernels only in what they compute: they work on the shredder's `state` bytes before the engine has bitmaps.
- Why the fused kernels stay: the composition from the generic kernels (measured by `tools/kernels`, `make bench-shred`, as `sift_colk->range[SIFT_COLW_8]` on the bytes (`state_bits`: `range(state, VALUE + 1, 254)` for the NULL bitmap, `range(state, OVERFLOW, 0)` for the overflow one; `state_counts`: a range and a `bits_count` per state) and `SiftAggKernels.minmax_*` under the bitmap of the `VALUE` slots) is compared with the fused kernel.
  - The composition loses by more than 3 % at the scalar level for every kernel (`state_counts` on a column of `VALUE`s 6x, `minmax_f64` 2.1x, `minmax_i64` 1.5x, `state_bits` 1.09x), at AVX2 for `state_bits`, `state_counts` and `minmax_f64` (1.5x, 1.4x, 1.14x) and at AVX-512 for `minmax_f64` (1.26x); it also needs the value arrays to have `SIFT_AGG_PAD` readable bytes behind them (the shredder's arrays do not promise it).
  - It wins at AVX-512 for `state_bits` and `state_counts` (0.7x at 4096 elements) and for `minmax_i64` at AVX2 and AVX-512 at 4 Mi elements; a level that is better composed does not make the scalar level's loss go away, and a table that mixes the two would keep both implementations.
  - When the adapter has bitmaps anyway (`state_bits`), the engine's `minmax_*` apply as they are. The AVX2 `minmax_i64` here is the weak one (a single dependency chain): the aggregation kernel's four accumulator pairs would make it 1.8x faster.
- Each module that owns a dispatch table registers one rebind with `sift_cpu_register` (`SIFT_MAX_REBINDS` is 8).

## Tests

`sift._test_shred(jsonb, spec)` (testing build) returns one row per slot and per list. `test/sql/shred_*.sql` cover the grammar and the errors (`shred_spec`), the edge cases (`shred_values`: absent versus null, empty arrays, mixed types, nesting, huge numbers, unicode, duplicate keys, limits), a differential test on random documents (`shred_diff`: a plpgsql oracle over `jsonb_each`, `jsonb_array_elements` and the jsonb casts for every slot of clean and dirty documents; `jsonb_to_recordset`, `jsonb_path_query` and SQL aggregates for the clean ones; the kernel summaries against SQL aggregates) and the SIMD levels against scalar (`shred_kernels`). `bench/bench shred` measures documents per second (`bench/steps/shred.sh`, `bench/sql/shred.sql`).
