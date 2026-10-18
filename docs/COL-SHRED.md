# sift_col shredder: jsonb to typed columns

`src/sift_shred.[ch]` (portable, `src/portable-col.list`) turns one JSON document into typed column values for a list of declared
paths. `src/sift_shred_pg.[ch]` drives it from a `JsonbContainer`. The shredder is the only code that knows JSON; the storage
(`sift_col`, docs/COL.md) and the planner see typed arrays, element offsets and per-value states.

## Declaring paths (reloption `paths`)

```
paths = 'attributes->locale text, metrics.* float4,
         items[]{category text, sku text, kind text, qty int4, price float4}, notes[]{type text, author_role text}'
```

```
spec  := item { ',' item }
item  := path ( type | '{' item { ',' item } '}' )     -- a group is relative to the element its path ends with
path  := step { sep step }        sep := '.' | '->'    -- the two separators are the same
step  := key | key '[]' { '[]' } | '*'
key   := bare ([A-Za-z0-9_$] and bytes >= 0x80) | "quoted" ("" is a double quote)
type  := bool | int2 | int4 | int8 | float4 | float8 | text [ '(' N ')' ]
```

Aliases: `boolean`, `smallint`, `int`, `integer`, `bigint`, `real`, `double [precision]`. A path starts with a key. `key[]` is an array
of the following element (`tags[] text`: an array of scalars; `items[].sku text` or `items[]{sku text}`: an array
of objects; `matrix[][] float4`: an array of arrays of scalars). `*` is every member of an object (`metrics.* float4`; `stats.*{n int4, avg float8}`
members that are objects). A wildcard cannot share its object with explicit keys. A path may be declared once; items that share a
prefix share the node (`a.b int4, a.c text`, `a[].x int4, a[].y text`). Errors name the position
(`ERRCODE_SYNTAX_ERROR`) or the conflict (`ERRCODE_INVALID_PARAMETER_VALUE`, `ERRCODE_PROGRAM_LIMIT_EXCEEDED`).

`sift_shred_spec_deparse` gives the canonical form, one flat item per column in declaration order (`attributes.locale text,
metrics.* float4, items[].category text, ...`), always with `.`; `fingerprint` is its 64-bit `sift_term_hash64` of the canonical text. Segments store
the canonical text and the fingerprint. Limits: 256 columns, 64 groups, 16 steps per path, 64 KiB of spec.

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

Order is stable and deterministic: columns in declaration order, elements in array order, map members in jsonb storage order (key
length, then bytes), dictionary ids in order of first appearance across the documents shredded through one `SiftShred`.

### Wildcard dictionary

`*` members are `(keyid, value)` pairs of a map group. Ids come from a `SiftShredDict` (created with the shredder, replaceable
before the first document): `intern` assigns the next id, `find` and `key` look up. A key over 255 bytes, or beyond `max_keys`
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
| text | string, at most `N` bytes (default 255 bytes, what the text dictionary of sift_col holds) | `VALUE` |
|  | longer | `OVERFLOW` `LENGTH` |
| any | JSON null | `NULL` |
| any scalar type | a different JSON scalar (string in a number column, number in a text column, ...) | `OVERFLOW` `TYPE` (no coercion: `->>` would give `'12'` for the number 12) |
| any scalar type | an object or array | `OVERFLOW` `SHAPE` |
| object position | array | all columns below `OVERFLOW` `SHAPE` (lax jsonpath would unwrap it) |
| object position | scalar, null | columns below `ABSENT` |
| list position | an object / scalar (lax jsonpath would wrap it) | list `OVERFLOW` `SHAPE`, columns below flagged |
| list over `max_elems` / `max_bytes` | | the rest of the list is dropped, list `OVERFLOW` `TRUNC`, columns below flagged |

Negative zero is stored as `+0` (numeric has none), so no NaN and no `-0` is ever produced. Integer sums, `min`, `max`, `count` over
`VALUE` slots are exact. Float aggregates equal plain SQL when the aggregator uses the same transition functions in the same
order (`float4pl` in float4, `float8pl` in float8; a float4 column summed in float8 differs from `sum(float4)` exactly as in SQL).

Numbers arrive as the decimal text of the jsonb numeric (`numeric_out`, no exponent). Integers are parsed from the digits, exact up to
19 digits; floats go through `strtof` / `strtod` (the process keeps `LC_NUMERIC` at "C", as PostgreSQL does), with a shortcut for
integers that are exact in the type.

## Memory

A document never materializes: the jsonb iterator keeps one frame per nesting level, subtrees no declared path reaches are skipped
without descending, and the output is bounded by `SiftShredLimits`: `max_elems` list elements per document (default 1 Mi) and
`max_bytes` of text per document (default 16 MiB). The arrays of the `SiftShredDoc` are reused between documents and released when a
document grew them past 64 Ki elements.

## C interface

```c
SiftShredSpec *sift_shred_spec_parse(const char *text);          /* errors raise */
const char    *sift_shred_spec_deparse(const SiftShredSpec *);   /* canonical; spec->fingerprint */
SiftShred     *sift_shred_create(const SiftShredSpec *, const SiftShredLimits *);  /* own memory context */
void           sift_shred_free(SiftShred *);

/* portable event interface: any driver (jsonb iterator, a JSON text parser) */
void sift_shred_begin(SiftShred *);                 /* next document; the dictionary survives */
SiftShredKey sift_shred_key(SiftShred *, const char *key, int len);   /* SKIP | TAKE | STOP (rest of the object is irrelevant) */
bool sift_shred_enter(SiftShred *, bool is_array);  /* a container value; false: do not descend */
void sift_shred_leave(SiftShred *);                 /* after every enter that returned true */
bool sift_shred_wants_number(SiftShred *);          /* false: pass sift_shred_number(NULL) */
void sift_shred_null / _bool / _string / _number(...);   /* scalar values; number text is NUL terminated */
bool sift_shred_full(SiftShred *);                  /* the current list is cut short: stop iterating it */
void sift_shred_finish(SiftShred *);

/* PostgreSQL driver (sift_shred_pg.h) */
void sift_shred_jsonb(SiftShred *, Jsonb *);  void sift_shred_container(SiftShred *, JsonbContainer *);

/* the output: sh->doc.col[c], sh->doc.grp[g]; sift_shred_group_range(), sift_shred_expand_parents() */
```

A driver contract: keys of an object are unique; every value event follows a `key` that returned `TAKE` (or `STOP`), or is an array
element, or is the root.

## SIMD kernels

`SiftShredKernels` (`sift_shred_kern*.c`) follows docs/KERNELS.md: the scalar table is the specification, `sift_shred_kern_avx2.c` and
`sift_shred_kern_avx512.c` are bit-identical, dispatch through `sift_cpu_register` (`sift.simd`), `sift_shred_kernels_for(level)` for
tests. Kernels: `state_counts` (histogram of the four states of a column), `minmax_i64` / `minmax_f64` (zone map over the `VALUE`
slots), `expand_parents` (offsets to per-element parent ordinals).

## Tests

`sift._test_shred(jsonb, spec)` (testing build) returns one row per slot and per list; `test/sql/shred_*.sql` cover the grammar and the
errors, the edge cases and a differential test against `jsonb_to_recordset`, `jsonb_array_elements` and `jsonb_path_query` on random
documents; `shred_kernels` compares the SIMD levels with the scalar one. `bench/bench shred` measures documents per second.
