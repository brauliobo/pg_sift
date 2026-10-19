# sift_col: a virtual columnar table over a heap table (and the column engine under it)

`CREATE INDEX ... USING sift_col (col, ..., (expression), ...)` stores the values of its columns once more, column by column, per
superblock of the heap, with zone maps. An analytic query reads those columns instead of the heap rows. There is no second table: the
heap stays the only copy that is authoritative, and the index is reached through the same TID space, visibility map and generic WAL as
the `sift` (BM25) access method, whose store, directory, write buffer lanes, tiered merge and maintenance it reuses. The BM25 paths are not
touched.

Status of this document: the contract of the column track (index AM `sift_col`; the table AM `sift_table` of col-tam reuses the engine, section 12). `src/col/sift_col.h` is the C interface (column source, metadata, shred
interface) the other agents build against, `src/col/sift_col_kern.h` the kernel table. Where this file and the code disagree, fix the code or
this file in the same commit.

User-facing names: index access method `sift_col` (`pg_am.amname` is unique across index and table AMs; the table AM is `sift_table`), operator
class `sift.col_ops` (the default one, for any type), GUC `sift.enable_col`, functions `sift.col_index_info`, `col_segments`, `col_leaves`, `col_sections`,
`col_verify`, `col_promote`, `col_merge`, test functions `sift._test_col_*`. `col_segments` and `sift.table_segments` list the column format minor of each segment (`format`, 3.1) and `col_segments` the bytes of its pre-aggregates (`preagg_bytes`). Code: the engine is `sift_col*.c` (and `sift_col_kern*`), the index AM glue `sift_col_am*.c`.

---

## 1. Model

- **Leaf column.** The index definition becomes a list of leaf columns (`SiftColDef`, ids 0..ncols-1, at most `SIFT_COL_MAX_COLS` = 128).
  An index attribute that is a plain type is one leaf; a `jsonb` attribute with declared paths is several leaves (section 6); an array of
  objects inside it is a **group** leaf plus one **child** leaf per object field.
- **Storage class** of a leaf (`SiftColStore`): `INT` (int2, int4, int8, bool, date, timestamp, timestamptz: an int64; `numeric(p, s)` with p <= 18 and s >= 0: the number times 10^s, `SiftColDef.scale` = s, exact like the sums PostgreSQL computes while they fit int64; NaN is NULL with the overflow flag, an unconstrained or wider numeric is refused at CREATE INDEX; constants are scaled with `sift_col_numeric_key`, values come back with `sift_col_key_numeric`), `F4`, `F8`
  (float4/float8) and `STR` (text, varchar, char(n), name: a per-segment dictionary code, or the string itself in a unit that has a long one, section 2.9; values up
  to 1 GB; char(n) keeps its blank padding and name its bytes up to the first NUL, so equal values have equal bytes; a `bpchar` without a length does not, and its leaf is flagged lossy: no predicate or group is pushed on it). The other types are refused at CREATE INDEX in this version.
- **Key domain.** Every chunk holds integers: an `INT` value as it is, a float as its **order key** (the IEEE bits turned into a signed
  int64 that sorts like the float; the sign of zero is kept: -0 is the key just below +0, so a float comes back with its bits; every NaN is one NaN, sorted after +Inf as PostgreSQL does. PostgreSQL compares -0 and +0 equal, so a predicate constant of zero spans both keys (`sift_ctest_make`)), a `STR` value as a
  dictionary code. All filters are unsigned range tests on `value - base` (section 2.4), whatever the type.
- **Row.** A row is a heap tuple (`SiftKey = blk << 16 | off`, as in sift). The rows of a segment inside one superblock (SB, 256 heap
  pages) form a **unit** `(segment, sb)`. Rows of a unit are numbered 0..n-1 in key order (the **row ordinal**); that number indexes every
  column chunk of the unit, the liveness bits (with the unit's rank base) and the selection vectors.
- **Group (nested) leaf.** The value of a group leaf in a row is the element count of the array (NULL: the array is absent, 0: empty).
  Its children hold the elements of all rows of the unit, concatenated in row order; element `j` of child A and element `j` of child B
  are the same array element, so `category = x AND kind = y` evaluated on elements keeps the same-element semantics of `jsonb_to_recordset`
  / `EXISTS (SELECT ... FROM jsonb_array_elements ...)`. The parent ordinal of an element is recovered from the counts (prefix sums).
  One level only in this version (a group is a top-level leaf, its children are scalar leaves).
- **No duplicate table, no heap column storage.** All values the index holds are copies taken at `ambuild` / `aminsert`. A row is read
  from the index only if the MVCC rule of section 3 allows it, else the heap is asked.

## 2. Format (format major 1, col minor 4; minor 3 is read side by side)

The metapage (block 0), directory (block 1), free list, extents, pages (`SiftPageOpaque`), generations and generic WAL protocol are those
of `sift_store.h` and `sift_page.h`, unchanged (P1-P6 of `sift_store.h`). What differs:

- `meta->flags & SIFT_COL_META_FLAG` (0x10000) marks a sift_col index; `meta->analyzer_hash` holds the **schema hash** (hash of the
  leaf list: ids, kinds, stores, type OIDs, paths) and a mismatch with the index definition is "REINDEX needed" (a reloption change that
  alters the leaves). `meta->k1 / b / sum_dl` are 0. `meta->ndocs` counts the rows of immutable segments.
- Every `SiftSegEntry` carries `format` = the col segment minor (3 or 4). A reader refuses a minor it does not know with a REINDEX hint (no release wrote minors 0 to 2: section 4); a 3.1 library reads minors 3 and 4 in one index
  as written and rewrites them as minor 4 by merge (docs/COL-JSONB-31.md 5.2). `SiftSegEntry.ndocs` is the number of rows, `sum_dl` is 0, `bytes` the size of the stream.
- `sift.verify`, `sift.segments` and the BM25 maintenance functions refuse a col index (`ERRCODE_WRONG_OBJECT_TYPE`); the `col_*` functions
  refuse a BM25 one. The AM check is `relam`, the metapage flag is the second line of defence.

### 2.1 Immutable segment

A segment is a byte stream over extents (`sift_sw_*` / `sift_sr_*`) plus one SEGHEADER page and the LIVE pages, exactly the BM25
shape, so `sift_store_immutable_extents`, `sift_store_referenced_blocks`, the orphan sweep and the free-list protocol serve it as they are.
The header page is a `SiftSegHeaderData` whose fields mean:

| field | meaning in a col segment |
|---|---|
| `ndocs` | rows |
| `nterms` | leaf columns (`ncols`) |
| `sb_lo`, `sb_hi` | first and last SB with rows |
| `dictroot_n` | units (SBs with rows) `nsb` |
| `dictroot_off` | unused (0) |
| `sections[SIFT_SEC_DOCMAP]` | **SBDIR**: `SiftColSBRef[nsb]` |
| `sections[SIFT_SEC_POSTINGS]` | **UNITS**: the unit records |
| `sections[SIFT_SEC_DICT]` | **DICTS**: the dictionaries of the STR leaves |
| `sections[SIFT_SEC_DICTIDX]` | **SCHEMA + ZONES** (leaf descriptors, then the zone arrays) |
| `live_first`, `live_npages` | LIVE pages (section 2.7) |

Stream order: UNITS, DICTS, ZONES, SBDIR, SCHEMA (written in that order, because the dictionaries and zones are only complete when the
last unit has been added; the header page, written last, points to all of them). All multi-byte integers are little endian, structures
are 8-byte aligned in the stream.

**SCHEMA**: `u32 magic 'COLS'`, `u32 ncols`, `u32 schema_hash`, then per leaf a 16-byte `SiftColDefDisk` (`i16 id, i16 parent, u8 kind, u8
store, u16 flags, i16 attno, u16 pathlen, u8 type, u8 scale, u16 reserved`) and `pathlen` path bytes. `type` is the leaf's logical type, a tag of the
format (`SiftColType`: bool, int2/4/8, float4/8, numeric (a scaled integer), text, bytea, date, timestamp(tz), uuid, jsonb, or OTHER) that
never changes how a chunk is stored; a host maps its own types to it at its boundary (PostgreSQL: `sift_col_type_of_oid`, and
`sift_col_def_typid` for the type a leaf yields to SQL, which for a plain attribute is the attribute's own type, so `varchar` stays `varchar`) and
resolves what the tag cannot name (OTHER) from its own catalog. The hash is `hash_bytes` of the same serialization of
all leaves with the flags zeroed. A reader checks the hash and the leaf list against the index definition (the schema of a segment never changes).

**SBDIR**: per unit, ascending `sb`: `{u32 sb, u32 nrows, u64 off (of the unit record in UNITS), u64 rank_base (liveness rank of its first
row), u32 len}`. Read once when the segment is opened by a scan (24 bytes per unit).

**ZONES** (structure of arrays, so one SIMD pass tests every unit of the segment): for each leaf, in id order:
`i64 min[nsb]`, `i64 max[nsb]` (key domain, of the non-NULL values; both 0 when all NULL), `u32 nnull[nsb]`, `u32 nvals[nsb]` (rows, or
elements for a child leaf), `i64 sum[nsb]` (INT, wrapping flag below) or `f64 sum[nsb]` (F4/F8: the sum in double, the order of summation
being row order), `u64 set[nsb][4]` (STR only: a 256-bit set of `code & 255`), `u32 flags[nsb]` (`SIFT_COLZ_NOSUM` the int sum
is outside int64 (exactly: not merely an intermediate overflow; `sum` is then 0), `SIFT_COLZ_NAN` a float chunk holds a NaN, `SIFT_COLZ_CONST`
at least one value is not NULL and all non-NULL values are equal, `SIFT_COLZ_OVERFLOW` some value overflowed). Zone checks never read a unit.
`sum`/`nnull`/`nvals` also answer `count(col)`, `sum(col)`, `min`, `max` of a unit that is entirely visible without reading its chunks.

**PAGG** (minor 2): the pre-aggregates of every unit, written at the end of the ZONES section (header `COLP`): the exact int128 `sum` and `sum of squares` of the INT leaves, and per unit the cube of low-cardinality dimensions (`GROUP BY dim` -> count, per measure non-NULL count and sum). Computed wherever a unit is written (build, parallel build, promotion, merge), never updated; minor 4 adds the element cubes (directory version 2: one cube per list, whose rows are the elements); format, option `preagg`, the rule for using it and the `SiftAgg` consumption: docs/COL-PREAGG.md.

**Unit record** (UNITS, at `SiftColSBRef.off`), 8-byte aligned fields:

```
u32 magic 'COLU', u32 nrows, u32 npages, u32 reserved
u64 pagemask[4]                         pages (of the SB) that hold rows
u32 rowstart[npages + 1]                row ordinal of the first row of each present page; [npages] = nrows
u16 maxoff[npages]                      highest offset of the page's rows
(sparse pages in page order) ceil(maxoff/8) bytes of offset bitmap, only for a page with maxoff != rowstart[p+1]-rowstart[p]
u32 chunk_off[ncols], u32 chunk_len[ncols]     offsets from the record start; len 0 = absent leaf (all NULL, the leaf had no values)
chunks ...                                      the column chunks, each 8-byte aligned
```

A page whose rows are the offsets 1..cnt is **dense** (nothing but `maxoff == cnt` is stored): the usual case of a freshly loaded table. The
TID of row `r` is found by binary search of `rowstart` and, on a sparse page, by selecting the k-th set bit of the bitmap (bit `off - 1`
is offset `off`, the same bit order as the 512-bit offset masks of `sift_vis`). The directory arrays are padded to 4 bytes before the chunk
tables and the tables to 8 before the first chunk; every leaf has a chunk (a leaf all NULL or a group child without elements gets a CONST
one), `chunk_len` 0 is only read, for a foreign writer. A full dense superblock (256 pages of 291 rows) has a 1.6 kB directory.

### 2.2 Column chunk

One chunk is the values of one leaf for the rows (or, for a child of a group, the elements) of one unit.

```
u8 enc, u8 bits, u8 flags, u8 pad, u32 n          n values (rows, or elements)
i64 base                                          frame of reference (the smallest non-NULL key value)
[u64 nullbits[(n+63)/64]]                          when flags & SIFT_COLC_NULLS; bit set = NULL; tail bits zero
[u64 ovfbits[(n+63)/64]]                           when flags & SIFT_COLC_OVF: the values the leaf could not hold (shredder OVERFLOW); also set in nullbits
[u8 tags[8 * ceil(novf / 16)]]                     when flags & SIFT_COLC_TAGS (minor 4, only with OVF): one nibble per set bit of ovfbits, in slot order
[u64 kmask][u64 kind[t][(n+63)/64] per set bit t]  when flags & SIFT_COLC_KINDS (minor 4): the tags that have a bitmap, then those bitmaps in tag order
payload
```

`flags`: `NULLS` (0x1), `OVF` (0x2), `TAGS` (0x4), `KINDS` (0x8). **The type of an overflowing value** (minor 4): the nibble of an overflow slot is the JSON type of
the value the leaf could not hold: 0 unknown (a writer that did not know it, a unit merged from a minor 3 segment), 1 string, 2 number, 3
boolean, 4 object, 5 array, 6 none (the leaf lies below an array where its path declares an object: the slot overflows because a lax jsonpath would unwrap the
array, but the path of `->` leads to no value and `jsonb_typeof` of it is NULL). The nibbles are LSB first (the even slot in the low nibble), the unused ones and the padding to 8 bytes are zero, and a chunk
without overflow has no flag and no bytes, so a clean index costs nothing. The segment's minor decides: every chunk of a minor 4 segment that has
overflow has `TAGS`, none of a minor 3 segment does. A stored value has no nibble, its type is the type of its leaf (bool boolean, integers,
floats and numeric number, text string). The write buffer keeps the type in the high nibble of the flag byte of an overflowing value. Readers:
`sift_col_filter_types` (the rows of a unit whose value has a type of a mask), `sift_col_row_types` (the type of every row), `sift_col_ovf_bits`,
the tag of `SiftColValue.tag` in a merge.

**Kinds** (minor 4). The slots that do not overflow can have a type their leaf does not say: a JSON null is a NULL slot like a missing key, and the
text of an `astext` leaf is a string, a number or a boolean. `KINDS` is a word whose low byte has bit `t` set for each tag that has a bitmap (7
JSON null, 1 to 3 the type of a value), then those bitmaps in tag order, `n` bits each, tail bits zero. A bitmap is never empty, no slot is in two,
none is in `ovfbits`, JSON null (tag 7) is a subset of `nullbits` and the others are disjoint from it. A chunk without any has no flag and no bytes (a
leaf of no JSON null and no astext value costs nothing); a string of an `astext` leaf is the leaf's own type and has no bit. The write buffer keeps a
kind in the high nibble of the flag byte of the value, like the type of an overflow value. Together: the JSON type of a row of a leaf is its tag
(overflow value, kind), else the leaf's own type for a value and none for a NULL slot (jsonb_typeof is NULL for a missing key and for a path that leads to no
value). A segment of minor 3 has no kinds, so it cannot tell a JSON null from a missing key or the types of an astext leaf's values: its answer for a test that needs it
is "not known" (`sift_col_filter_types` sets `*known` false), exactly as for an overflow value without a tag.

`enc`:

| enc | payload |
|---|---|
| `CONST` (0) | none; every non-NULL value is `base` |
| `FOR` (1) | `n` unsigned values `key - base` of `bits` = 8, 16, 32 or 64 bits, plain little endian array. NULL slots hold 0. |
| `PACK` (2) | `n` unsigned values of `bits` = 1..31 bits (not 8 or 16), LSB first, in blocks of 64 values of `8 * bits` bytes (the last block padded) |
| `RLE` (3) | reserved, not written |
| `CC` (4) | the payload is one self-describing blob of the chunk codec (`src/col/sift_colcodec.h`, docs/COL-CODEC.md): `bits` and `base` are 0, the blob holds its own frame of reference, `min` and `max`. Readers dispatch to `sift_cc_open` + `sift_cc_filter` / `sift_cc_decode(_sel)` (a LUT test takes the codes with `sift_cc_decode_off32`). The blob starts 8-aligned and ends on a multiple of 8, so the payload is the rest of the chunk. Written by every segment writer (`sift_colw_begin` takes the policy of each leaf); an in-memory unit that is only scanned, a constant chunk and a unit built with no policy keep `CONST`/`FOR`/`PACK`, which stay readable. The reloption `compression` (`auto` default, `none`, `lz4[:N]`, `zstd[:N]`, encodings joined by `+`; per column `'auto; col=zstd:9'`, the column being the attribute name or the shredder path) sets the policy; a change applies to the chunks written afterwards (merge rewrites). `sift.col_leaves` counts the blobs in `codec_chunks`. |
| `STRS` (5) | VAR leaves: the codec's string list (section 2.8); `bits` and `base` 0 |
| `ISTR` (6) | STR leaves stored inline: prefix blob and string list (section 2.9); `bits` and `base` 0; `sift.col_leaves` counts them in `inline_chunks` |

The writer takes CONST when `max == min` (or every value is NULL: `bits` 0, `base` 0), else the smaller of FOR (the narrowest of 8/16/32/64
bits that holds `max - min`) and, when that span is below 32 bits and not exactly 8 or 16, PACK of as many bits as the span needs (counting
the padding of the last block of 64). `PACK` is decoded by the decode kernels
(`SiftColKernels.unpack`, section 8) into u8/u16/u32 scratch before a filter; a FOR chunk is filtered in place. GROUP leaves are INT chunks
of element counts. Group children store `n` = total elements of the unit.

The values at NULL slots of a decoded chunk are unspecified (any in-range key: the codec leaves the previous non-NULL value there, `FOR` the base); every consumer masks with the null bitmap, and the `sift_ctest_run` results are ANDed with the non-NULL bits.

**Filters on chunks.** A predicate on a leaf is translated (`sift_col_pred_range`) into an unsigned range test `lo <= key <= hi` in the key
domain, an `IN` set, or a LUT over the dictionary codes; then `lo - base`, `hi - base` select the kernel (`range_u8/16/32/64`) and the
bitmap of matching rows comes out, NULL rows removed (`ISNULL`/`NOTNULL` use the null bitmap alone). `NE` / `NOTIN` are the complement
within the non-NULL rows. Floats: constants are mapped to their order key (a float4 column holds the keys of the promoted float8 values, so comparing in
the key domain against a float8 constant is exactly PostgreSQL's float48 comparison; the constant needs no rounding). `IN` / `NOT IN`
over integers and floats is one range when the constants collapse to one value, else a sorted set (`SIFT_CT_SET` / `SIFT_CT_NSET`: up
to 8 members run as OR-ed range tests, more are searched).
Strings: `EQ`, `IN`, `PREFIX` and the range operators are evaluated once on the dictionary (bytewise) into a 1-bit-per-code LUT; the
rows are then filtered by `lut_u8/u16/u32`. Range operators on text are only meant for the C collation: the planner decides.

### 2.3 Dictionaries

Per segment and STR leaf, in the order the values were first seen (code = first-seen order), starting 8-aligned: `u32 form`, `u32 n`, then for form 0 (plain)
`u32 off[n + 1]` and the bytes (no padding), for form 1 the codec's string list (`sift_cc_encode_strs`: the lengths as an integer cascade, the bytes
under the cold wrapper rule of docs/perf/COMPRESSION.md 5.4: LZ4 or zstd where they save a share worth the unwrap). The writer takes form 1 when the leaf's
`compression` is not `none` and the list is smaller than the plain form (a dictionary of 30 countries stays plain, one of 150,000 titles is 4.4 MB plain
and 0.95 MB as a list); `sift_cdict_parse` reports the bytes used and refuses an entry over 255 bytes: longer strings are never entries, section 2.9.
The dictionary is read whole by the first scan that needs a code of the leaf, once per segment and statement (per worker for a parallel one). Form 1 costs about 9 more instructions per entry than form 0 (the lengths cascade, the running sum, the unwrap): a point lookup of a `sift_table` row with a 58,000-entry title dictionary takes about 3 times the instructions, 760,000 to 800,000 against 257,000 (203 against 163 microseconds of CPU, 1M orders), and a grouped scan on the title 17 percent more instructions (349 million against 298 million); no other query of the store suite reads that dictionary, and a dictionary of a few dozen entries stays plain. The price is paid for 3.4 MB of 17 per million orders. Codes are
dense 0..n-1 inside the segment and are **not comparable across segments or ordered**; a query that groups across segments maps codes to
values (`SiftColDict`), and the filter translates constants to codes per segment. A merge rebuilds the dictionary (kept in first-seen order
of the merged rows). The zone `min/max` of a STR leaf are code bounds (equality pruning), the `set` is the exact set when the segment has
at most 256 codes, else a bloom of `code & 255`.

**Reading text from SiftAgg (applied for the `sift_col` provider: `col_prune_str` and `col_filter_str` in `sift_aggcol.c`, the calls in `sift_agg.c`, docs/COL-PLAN.md "Filters in the source"; the `sift_table` provider has neither callback).** Nothing needs to change for
correctness: an inline unit reaches the executor as a `SiftSrcDict` of its own with its own `id`. What it costs is that the executor
evaluates a text predicate by building a LUT over the unit's dictionary, which for an inline unit means decoding and interning every
string of the unit (measured: 300 000 distinct strings of 290 bytes, `s = const` through SiftAgg 80 ms, a heap scan 26 ms, `sift_col_filter`
on the encoded form 2.5 ms). `src/agg/sift_colsrc.h` therefore carries two optional provider callbacks, `prune_str` and `filter_str`, with the
tests `SiftSrcStrTest` (EQ, NE, IN, NOT IN for any deterministic collation; the range operators and PREFIX only where the comparison is
bytewise, i.e. the C collation or `text_pattern_ops`), and the inline wrappers `sift_colsrc_prune_str` / `sift_colsrc_filter_str`. As applied:

1. Providers (the index in `sift_aggcol.c`, the table in `sift_table_src.c`) are the same, over `sift_col_begin` / `sift_col_filter`:
   `col_prune_str` converts each test to a `SiftColPred` (`SSTR_EQ` -> `SIFT_COLOP_EQ`, ... `SSTR_PREFIX` -> `SIFT_COLOP_PREFIX`, `vals[i]` ->
   `SiftColValue{.v.s, .len}`) and keeps them in the source; `col_begin` appends them to the `preds` it passes to `sift_col_begin` (zone
   pruning: the segment dictionary's test and the zone set for dictionary units, the prefix zone for inline ones). `col_filter_str` converts the
   one test, copies `rows` into a `SiftColSel` made with `sift_col_sel_alloc(src->cs, unit->nrow, &sel)` (bits padded as the kernels need),
   calls `sift_col_filter(&src->unit, &pred, 1, &sel)` and copies the bits back; it returns true. A leaf the provider marks lossy (an
   overflow value is stored as NULL) must make it return false.
2. Executor (`sift_agg.c`): for a conjunct that is a text leaf compared with constants in one of those forms, collect it as a
   `SiftSrcStrTest`, give the list to `sift_colsrc_prune_str` before `sift_colsrc_begin`, and per unit, before reading the leaf, narrow the
   row mask with `sift_colsrc_filter_str`; when it returns true the conjunct is done (do not evaluate it again, and the leaf need not be
   read if no other expression uses it), when false evaluate as today. EXPLAIN may show the test as pushed.
3. Per-dictionary caches keyed by `SiftSrcDict.id` (LUTs, group maps) see one new id per inline unit read: bound their size or drop an
   entry when the next unit comes.

### 2.4 Zone map pruning

`SiftColSource` evaluates, before reading anything of a unit, the predicates of the scan against the ZONES arrays of the segment
(`SiftColKernels.zone_range`: `min > hi || max < lo` clears the unit; `nnull == nvals` clears every non-null predicate; the set test for
STR `EQ`/`IN`). Units that pass are the **candidate units**. Pruning is conservative: the filters then run on the chunks.

### 2.5 Write buffers (the write buffer)

`aminsert` appends one **row record** to one of `SIFT_LANES` lane chains of the open write buffer: the structure of `sift_wbuf.c`
(lane header page with the heads and tails, `SIFT_PT_WBUFDATA` pages, tail-lock ordering, sealing under `SIFT_ACTIVE_LOCK_BLKNO`,
back-pressure at the same thresholds, one generic WAL record per append). The lane machinery is one module, `sift_lane.c`, used by the BM25
doc records and by the row records; the record and its page header are `sift_col_wbuf.c`'s. The data page header is `SiftLanePageHead`
(`next`, `nrecs`, `flags`: no term bloom), which starts with the `next` the store's walkers (`sift_store_referenced_blocks`,
`sift_store_orphans`, `sift_store_verify`) follow in either kind of lane. A record is

```
SiftColRecHead {tid(6), u16 flags (SIFT_COLR_DELETED, SIFT_COLR_CONT), u32 len}   then len payload bytes, 4-byte aligned
payload: for every top level leaf in id order: u8 flag (bit0 NULL, bit1 OVERFLOW: also NULL) and, unless NULL;
         INT: zigzag varint; F4: 4 bytes; F8: 8 bytes; STR: u8 len + bytes; GROUP: varint count (its children then carry
         count values each, after the row's scalars, child by child, every value with its own flag byte)
```

A record that does not fit the tail page is split in pieces of its payload (first piece without CONT, the rest with it) written while the
tail is held, as in sift; a row that fits an empty page moves there whole. A piece's page has `SIFT_WBUFPAGE_SPANS` set. A writer that dies
between two pieces (crash, error) leaves a **torn** row: it never committed, and a reader skips it (the pieces end where the next record is not a
continuation). A string is a varint length and its bytes, up to `SIFT_COL_MAX_VALUE`. A repeated TID keeps its
first live record in the batches (a crashed insert is not repeated). A write buffer is read by decoding its records into per-SB batches (key
order) and presenting each batch as a unit whose chunks are encoded in memory by the chunk writer with a throwaway dictionary
(`SIFT_COLO_NOWBUF` skips them: build and verification paths). The deleted flag is set by VACUUM on the first piece. The directory being
full when no active segment exists is `sift_col_maint_make_room`'s (the AM's maintenance), as `sift_maint_make_room` is for sift.

The directory entry of a write buffer carries, in `SiftSegEntry.reserved` (the segment tag of an immutable entry), the leaves it holds an overflow value of
(`SIFT_COL_OVF_BIT`: bit i for leaf i, bit 31 for every leaf from 31 on): an appender ORs the leaves its row overflows in before the row goes in
(`sift_col_wbuf.c`, one directory update when that changes the entry) and then invalidates the relcache entry of the table, so that the plans made
before the value was written are made again, by a planner that reads the mask (`SiftColIndexStats.wbuf_ovf`, docs/COL-PLAN.md). The mask goes with the
buffer: a promotion writes the rows into a segment whose zones flag the overflow.

### 2.6 Build, promotion, merge

All three use one writer: `sift_colw_*` takes **batches** (`SiftColBatch`: the decoded rows of one SB, keys ascending, typed vectors
per leaf, group offsets) in strictly ascending SB order and writes the unit records, dictionaries, zones and the header. A batch for an
SB that several inputs hold (a promotion of write buffer rows next to old rows, a merge of segments with overlapping SB ranges) is made by
the **batch merge**: decode every input unit's live rows, merge them by key, re-encode.
(An SB held by a single input segment is rewritten from its batch too: chunks are cheap to decode, and dead rows are dropped.)

- **Serial build**: `table_index_build_scan` -> row -> `SiftColBatch` of the current SB -> `sift_colw_add`; the writer targets a BULK
  `SiftWriteTarget`; one segment per `maintenance_work_mem` of buffered *encoded* bytes (rows are encoded SB by SB, so the buffer is the
  encoded size, a fraction of the heap).
- **Parallel build** (`amcanbuildparallel`, with the workers PostgreSQL plans for `CREATE INDEX` and `REINDEX`, `CONCURRENTLY` too:
  `max_parallel_maintenance_workers`, `min_parallel_table_scan_size`, the table's `parallel_workers`, 32 MB of `maintenance_work_mem` per
  participant; none launched = the serial build): the leader writes the metapage, then every participant (the workers and the leader)
  **claims whole superblocks** from a shared counter, the first one by its slot so that each has a share whatever the speed of the workers'
  start. It scans the 256 heap pages of the SB (`table_index_build_range_scan` on the block range, with the leader's snapshot when concurrent), forms the batch and
  encodes the unit into segments **of its own** through a SHARED write target (extents taken under the metapage lock, pages logged one by one,
  generations from `sift_store_new_gen`). A participant cuts a segment at its share of the serial budget (at least 16 MB) and writes at most
  `(SIFT_MAX_SEGMENTS - 7) / participants` segments (16 participants at most), so the directory limit holds as in the serial build; the
  leader publishes all of them in one `sift_dir_update`, ordered by first SB.
- **Promotion** of a sealed write buffer: read records, drop deleted, sort by key, group per SB, write one immutable segment.
- **Merge**: the tiered policy of `sift_merge_pick` (fan-in 8, tier base `write_buffer_limit`), plus "mostly dead" through a policy callback: a lone segment more than the `dead_rewrite_pct` option (default 10) dead is rewritten, so that its units carry exact pre-aggregates again; so is a lone segment that flags the overflow of a leaf in a unit while no live row of the unit holds an overflow value in it (`sift_cseg_stale_ovf`), so that a query reads the leaf with the scan again (docs/COL-PLAN.md). A VACUUM that skips the vacuuming of the indexes (`amvacuumcleanup` without a prior `ambulkdelete`) first clears the liveness of the rows that hold an overflow value and are dead to every snapshot in the heap (`sift_cseg_reap_overflow`), so that the rows that held it count as dead.
  Dead rows (cleared liveness bits) are dropped; one segment results. Memory: one SB per input in flight.

Why this model: the serial build spends about a third of its CPU encoding and writing, a fifth shredding and forming rows and the rest scanning the
heap and parsing jsonb (`perf`, 3 million order lines), so workers that send rows or batches to the leader would leave it a third of the work (a
speedup of 3 at most) plus the cost of moving the rows, while a participant that runs the whole pipeline on whole superblocks scales with the
participants, merges nothing and writes no temporary file; the chunks of PostgreSQL's parallel heap scan are not superblock aligned, so a unit
would be split between workers, encoded twice and merged again. A superblock is in exactly one segment, so segments never share a unit, but their SB
ranges interleave; the scan (a merge of the segments' unit cursors by SB), `col_merge`, promotion and VACUUM take segments with overlapping ranges
as they take them next to write buffer rows, and `sift.col_index_info.units` counts the SBs the ranges cover once. What differs from a serial build is the bytes,
never the rows read: dictionaries are per segment, so a parallel-built index has one segment per participant, a dictionary for each and slightly
more chunk bytes (32.3 MB of segments serial, 32.7 MB with 2 workers, 33.1 MB with 4), and its file is longer by the unused end of
each participant's last run (50 MB and 41 MB: free blocks, reusable by the next writes; those at the end are cut by the truncation of VACUUM, a BULK write
trims them all). Measured on 3 million lines (616 MB of heap, 7 columns, 5 shredded paths), on a loaded host (load average 9 of 16 cores), median of 3
builds: serial 2.6 s, 2 workers 0.9 s (2.9x), 4 workers 0.8 s (3.2x).

### 2.7 Liveness and VACUUM

LIVE pages as in sift (`SiftLiveHeaderData`, a dirty-SB bitmap then one bit per row in rank order, rank = `rank_base` of the unit + row
ordinal). `ambulkdelete` walks every row of every immutable segment of the SBs, calls the callback with its TID and clears the bit of the
dead ones (a unit's TIDs come from its row directory; chunks are not read), marks the SB dirty; write buffer records get the deleted flag.
A scan ANDs the liveness bits only in dirty SBs; whether an SB is dirty is read from the LIVE page every time (not from the directory
snapshot or an earlier read), see section 3, "Order of reads". Dead rows are removed from the segment by the next merge, which a segment
with more than half of its rows dead triggers.

### 2.8 Shared with the table AM: VAR leaves, the segment tag, the next vblock, delta batches

- **VAR leaves** (`SIFT_COLS_VAR`): opaque bytes of any length with an exact round trip (numeric, uuid, long text, jsonb, arrays). The
  chunk (encoding 5, `STRS`) holds the null bitmap and the codec's string list (`sift_cc_encode_strs`: the lengths as an integer
  cascade, the concatenated bytes under the cold wrapper rule; NULL slots are empty strings); there is no dictionary and no zone
  min/max (the zone of a VAR leaf is the INT zone of the value lengths, so only `nvals`/`nnull` are of use). Predicates: IS NULL / IS NOT NULL,
  and EQ, NE, IN, NOT IN, the range operators and PREFIX bytewise on the strings (`sift_ctest_make_strs`, section 2.9), and a length range
  (`sift_cchunk_strs_length`). The engine never interprets the bytes: the owner of the segment defines the image. Read through
  `SiftColVec.voff/vbytes` (`n + 1` byte offsets). A VAR leaf is a top level scalar; the index AM does not offer VAR (CREATE INDEX
  refuses those types). Implemented in the one chunk writer and reader (no table-specific fork); `sift._test_col_enc_roundtrip` covers it.
- **Segment tag**: `SiftSegEntry.reserved` is the owner's tag (the table AM keeps the segment xmin there, 0 = none). The writer sets it
  (`sift_colw_set_tag`); the index AM leaves it 0; whoever rewrites a segment sets the tag of the new one.
- **Next vblock**: `SIFT_COL_META_NEXT_VBLOCK(meta)` names the metapage field (`sum_dl`) the table AM uses for its TID block counter.
  Col segments always have `sum_dl` 0, so the generic directory switch never changes it.
- **Delta batches**: `sift_col_add_batches` gives a source in-memory units (one SB each, rows in key order, all live) that are read
  with the stored units of the same SB as `in_wbuf` units; the installed `SiftColVisibility` decides their visibility.

### 2.9 Text of any length: inline STR chunks

A value is at most `SIFT_COL_MAX_VALUE` (1 GB, PostgreSQL's limit) bytes. The dictionary of a segment holds strings of at most
`SIFT_COL_DICT_MAX_ENTRY` (255) bytes and at most `SIFT_COL_DICT_MAX_BYTES` (64 MiB) in all, because a reader loads it whole and a
group-by builds a table over its codes. A unit whose STR leaf has a string over 255 bytes, or would take the dictionary past 64 MiB, does not
use the dictionary for that leaf: the writer (`sift_cbatch_str_inline`, decided per unit and leaf) stores the **strings themselves**, as chunk
encoding 6 (`ISTR`, the zone flag `SIFT_COLZ_INLINE`). The other units of the segment, and the other leaves of the unit, keep their codes; a
segment mixes both and a merge re-decides every unit. This is what the dictionary-or-plain choice of other columnar formats comes to
(Parquet falls back to plain pages once the dictionary page is too large, DuckDB to uncompressed or FSST strings): the dictionary stays the form of low-cardinality text, long and distinct text is
stored as a string list.

`ISTR` chunk: the common head and null/overflow bitmaps (`bits` and `base` 0), then `u32 plen, u32 0`, the **prefix blob** (a chunk-codec
blob, `plen` bytes, of the `n` keys `sift_col_str_prefix`: the first 8 bytes of the string big endian, zero padded, sign bit flipped, so
signed order of keys follows bytewise order of strings; NULL slots are filler) and the **string list** of the `STRS` chunk (section 2.8:
lengths as an integer cascade, bytes under the cold wrapper rule; NULL slots are empty). The prefix blob follows the leaf's compression
policy like any key stream, the strings the cold rule (LZ4 or zstd where the saved bytes are worth it). A reader validates both parts
before it uses either; an unknown wrapper or a damaged part is `ERRCODE_DATA_CORRUPTED`.

Zone of an inline chunk: `min` and `max` are the prefix keys of the smallest and largest prefix (so `s < t` implies `key(s) <= key(t)`; equal
keys mean "differs after byte 8"), `set` is all ones, flags carry `INLINE`; `sum` and `CONST` are not used. Pruning
(`sift_ctest_zone`, `sift_cseg_prune`) answers for an inline unit from the predicate itself (`SiftCTest.pred`): EQ and IN by the constants'
prefix keys inside `[min, max]`, `<` / `<=` by `min <= key(c)`, `>` / `>=` by `max >= key(c)`, PREFIX by the interval from the prefix padded
with 0x00 to the prefix padded with 0xFF (a prefix of 8 or more bytes is one key), NE and NOT IN never prune; the SIMD passes over the
segment's zones do not apply to such units, their bit is recomputed afterwards. Source-level zone data (`SiftColZone`) of an inline unit
reports `min = 0`, `max = INT64_MAX` and not constant: a unit without codes has no code bounds.

Tests on the encoded form (`sift_ctest_make_strs`, kind `SIFT_CT_STRS`; the only test a VAR leaf has besides NULL): EQ, NE, IN, NOT IN, the range
operators and PREFIX compare **whole strings bytewise** with no dictionary to translate the constants. On an `ISTR` chunk the prefix blob
is filtered first (`sift_cc_filter`, the codec's scalar / AVX2 / AVX-512 kernels, bit-identical, selected by `sift.simd`): one range test per
constant (IN lists above 16 constants skip it), and only the rows whose prefix can match have their string compared; when no row can
match, the string list is not decoded at all (a selective equality over high-cardinality text reads only the prefix blob, the
lengths and no bytes). `sift_cchunk_strs_length` answers "byte length in [lo, hi]" for `STRS` and `ISTR` chunks: it decodes the lengths cascade
(never the bytes) and runs the `range_u32` column kernel. FSST was measured against and not taken: the codec's cold wrapper (zstd) already
leaves the bytes of this layout small, and FSST gives random access to a string that this layout never needs (a filter either
rejects on the prefix and the lengths, or has to compare the string whole).

Reading: the scan turns an inline chunk into the form every consumer knows (`sift_cchunk_istr_codes`): a dictionary of its own, built
from the chunk's strings in first-seen order, and a chunk of its codes; the source reports it as a `SiftColDict` with a **unique `id`**
(a fresh one per chunk read, so a consumer that caches work per dictionary id, as SiftAgg does, never mixes it with another unit's), so
group-by, distinct and LUT filters need no change. Row-level code (`sift_cunit_decode`, merges, verify) interns the strings into the destination's dictionary. The
write buffer stores a string as a varint length and the bytes (a row may span pages as before); a write buffer batch keeps any length in its dictionary,
and its in-memory unit uses that dictionary directly.

### 2.10 Where the bytes go

`sift.col_sections(rel)` (a sift_col index or a sift_table; `src/col/sift_col_acct.c`) returns `(section, leaf, bytes)` summed over the immutable segments,
and the rows add up to `pg_relation_size(rel)`: `chunks` and `dict` per leaf (the dictionary as stored, section 2.3), `unit_dir` (the unit records less
their chunks: page directory, offsets, alignment), `dict_other`, `zones`, `preagg` (the sidecar, section 2.1), `sbdir_schema`, `page_overhead` (page headers and the
unused end of each stream's last page), `headers` (SEGHEADER and LIVE pages), `free` (blocks on the free list) and `other` (the metapage, directory, free list pages,
the write buffer, tombstones). `sift.col_leaves` counts the chunks of a leaf by encoding, `sift.col_index_info` the pre-aggregate bytes.

## 3. MVCC: when a row may be read from the index

The values in the index are those of the heap tuple at the moment of `aminsert`/`ambuild`. Indexed columns of a tuple never change in place:
an UPDATE that changes an indexed column or any column used by an index expression is not HOT, it makes a new TID with a new index
record, and the old TID is dead for every snapshot after the updater commits. So the value stored for a TID is the value of that tuple
version, whatever the snapshot, and the only question is visibility:

1. Take the unit's all-visible page mask (`sift_vis_sb_mask`, 256 bits from the VM fork; zero for a non-MVCC snapshot).
2. A row that is **live** (its liveness bit set or its SB clean) on an **all-visible** page is visible: no heap access.
3. Every other candidate row (page not all-visible: recent inserts, updates, deletes, unvacuumed aborts) is checked against the heap
   (`sift_vis_filter_page`: one heap page read, `heap_hot_search_buffer` per candidate offset under the scan snapshot; a HOT chain answers
   for its root TID, whose indexed values are still the visible version's).
4. Rows are vetted **after** the value filters, so the heap is touched only for rows that matter (`sift_col_vet`).

**Order of reads.** Step 2 trusts two facts, "live" and "all-visible", that VACUUM changes in a fixed order: `ambulkdelete` first (the
index entries of the dead tuples go), the heap pass and the VM bit after. A scan can use the pair only in that order's reading direction:
the all-visible mask first, the liveness of the unit after (`unit_clean` in `sift_col_scan.c`; `sift_cseg_dirty` reads the LIVE page each
time). A mask that says "all-visible" was set after the index pass, so liveness read later shows the removals; liveness read first (a
directory snapshot taken when the scan opened, a cached dirty bit) and the mask later counts rows of tuples that VACUUM removed after the
scan's snapshot, on pages it made all-visible. Two more rules complete it:

- **The segments must be current.** The pass flags the segments of the directory as it is; a merge replaces its inputs with an output that
  has the liveness of its moment, so a scan holding the older directory copy reads inputs that no later pass flags. After the mask is read,
  `segments_current` checks that every immutable segment of the scan's copy is still in the directory (one version read; a full comparison
  only when the version moved); if one is gone, no page of the scan counts as all-visible and its rows are checked against the heap. A
  pass and a merge exclude each other by the maintenance lock, so while the segments are current every pass that finished before the mask
  was read ran on them.
- **The write buffer has no such order**, since its batch is a copy made when the source begins: no page of a lane batch counts as
  all-visible, its rows are always checked against the heap (`ColUnit.lane`; measured with 400 000 rows in the lane: 41 to 50 ms for a
  grouped sum either way, the batch decode dominates).

Tests: `col_vacuum_scan` (statements run between the open and the reads of a scan: a VACUUM on stored units, on a dirty unit, a merge and
then a VACUUM, the lane, DML with promote and merge), `scripts/colsoak.sh` (checkers in REPEATABLE READ against writers, VACUUM and
maintenance; the CI job `soak-col`). In testing builds `sift.test_col_paranoid` checks the rows on all-visible pages against the heap too and
logs the ones the snapshot does not see, `sift.test_col_scan_jitter_us` sleeps at the points of a scan where VACUUM can interleave.

Corollaries: rolled-back inserts fail step 3 until VACUUM, whose `ambulkdelete` clears their bits before the heap page can become
all-visible; a TID reused after VACUUM gets a new row record (I1: a TID is live in at most one segment, as in sift). Write-hot SB ranges
make steps 3 expensive; the planner costs the share of pages that are not all-visible (`SiftColIndexStats.allvisible_frac`) and falls
back to the ordinary plan.

**Isolation.** READ COMMITTED / REPEATABLE READ use the snapshot of the scan. SERIALIZABLE: `sift_col_open` takes
`PredicateLockRelation(heap)` (and `ampredlocks` is false, so inserts check the index relation as for sift). **RLS**: `sift_col_open`
refuses a table with row-level security enabled for the user (`ERRCODE_FEATURE_NOT_SUPPORTED`), as `sift.facets` does; the planner must
not offer the path either. **Privileges**: the executor reads only the leaves of the columns the query references; those have been
checked by the normal permission check of the query.

## 4. Concurrency, WAL, crashes, replicas, upgrade

- **Locks, WAL, free list, safexid reuse, directory switch**: unchanged from sift (`sift_store.h`). Readers copy the directory once with
  a registered snapshot (`sift_dir_read`) and keep it for as long as they use it.
- **Maintenance** (promotion, merge, VACUUM's liveness pass, recycle, orphan sweep) holds `LockPage(index, SIFT_MAINT_LOCK_BLKNO)`; the
  async worker is a second entry point (`sift_col_maint_worker_main`) bound to `sift_col` indexes only.
- **Crash safety**: the P1-P6 protocol; a segment is written to blocks that are logged before the directory record that publishes it.
  Recovery needs nothing of its own. A crash during build leaves a relfilenode that is dropped; during promotion/merge orphans, swept.
- **Physical replica**: generic WAL replays everything; readers check page type and `gen` (`sift_store_check_page`), a recycled page of a
  segment the replayed directory no longer lists is `ERRCODE_T_R_SERIALIZATION_FAILURE` (retry); a col scan on a standby is read-only and
  works with the standby's snapshot and VM.
- **Logical replication**: the subscriber builds its own index; nothing is shipped.
- **pg_upgrade**: the files are copied as they are; the layout depends only on `BLCKSZ` 8192 and the VM layout (already pinned by a test).
- **Versioning**: metapage major/minor as sift's (the BM25 minor bump rules do not apply: a col index's metapage is written with `SIFT_FORMAT_MINOR`
  (1.2) and the per-segment `format` carries the col minor). Any other col minor is refused with a REINDEX hint; layout changes bump it. Minor 3 is the first a release reads, minor 4 (3.1: the type tag of an overflowing value, chunk flag `SIFT_COLC_TAGS` of section 2.2, the element cubes of the sidecar) is written by 3.1 and refused by 3.0.x with the same hint, `sift.col_format_minor = 3` keeps writing minor 3: segments
  of the pre-release minors 0 (leaf types as PostgreSQL OIDs), 1 (no pre-aggregates) and 2 (plain dictionaries only) are not read.

## 5. Planner-facing metadata

`sift_col_index_meta(index)` returns the leaf list (id, kind, store, logical type tag and the type OID it yields to SQL, index attribute number, parent group, path text),
the attribute expressions (`sift_col_att_expr`), whether the index is partial and whether it is valid for scans, and
`SiftColIndexStats` (rows, segments, bytes, share of all-visible pages). `amcostestimate` prices the index out of every ordinary plan: the operator class has no operators, so
the planner never forms an index path; only the custom scan of the plan track uses it, through this API.

### Statistics of a leaf

`SiftSrcStats` (`sift_colsrc.h`, what `provider->stats` fills) carries, per leaf, `ndistinct`, `min`, `max` and, for a group leaf, `elems_per_row`; 0 means unknown. The `sift_col` provider derives them from what the immutable segments hold without reading a chunk: the unit zones (`nvals`, `nnull`, `min`, `max`, the integer sum) and the first word of each dictionary (its entry count). The counts are over the non-NULL values of the leaf (the elements, for a child), and include rows deleted since the build; rows still in a write buffer are not counted until they are promoted.

| Leaf | `ndistinct` | `min`, `max` | `elems_per_row` |
|---|---|---|---|
| text, no inline unit | the largest segment dictionary, capped by the non-NULL values: exact for one segment, a lower bound when segments hold different values (a merge makes it exact) | 0 | |
| text with an inline unit, float | the non-NULL values (an upper bound) | 0 | |
| integer, date, timestamp, bool, numeric (scaled) | the smaller of the non-NULL values and `max - min + 1`: exact for a dense column, an upper bound otherwise | the smallest and largest value of any unit (numeric: the scaled integer) | |
| group | 0 | 0 | the sum of the counts over the rows, when no unit's sum overflowed |

`sift_col_leaf_stats(index)` (`sift_col_pg.h`) is the per-leaf accumulation; it is memoized per backend (8 indexes) under a key made of the generations of the immutable segments, so a build, promotion, merge or VACUUM rewrite refreshes it on the next plan and nothing else reads the zones again. This is the statistics hook: it runs when the directory changes, not per query, and no ANALYZE is needed for an index. A `sift_table` table has no dictionary or zone per column in the layout the planner sees; its provider reports the `n_distinct` that ANALYZE of the table stored in `pg_statistic` (a negative value is a share of the rows) and 0 before the first ANALYZE. `sift._test_col_leaf_stats(rel)` returns the six numbers per leaf of an index or a table (test extension); `col_index_stats` is its test.

## 6. Shredding jsonb (col-json's `sift_shred.h`, through the adapter `sift_col_shred.c`)

The core never parses jsonb. col-json's shredder (`docs/COL-SHRED.md`, `src/col/sift_shred.[ch]`) turns a document into typed slots
(`SiftShredDoc`: per column a state ABSENT / NULL / VALUE / OVERFLOW and a value per group element, per group the `end[]` ranges of its
elements under every parent element, maps with a key id). The **adapter** (`sift_col_shred.c`, section 4 of `sift_col.h`, owned by the core)
is the only code that sees both models:

- **Spec and documents.** The reloption `paths` is one specification whose first step is the name of an **index attribute** (the index
  column name, `CREATE INDEX ... USING sift_col (store_id, status, metrics, items) WITH (paths = 'metrics.* float4, items[]{category text, sku text, kind text, qty int4, price float4}')`):
  the adapter shreds the *object* `{attname: value, ...}` of the jsonb attributes of the tuple (events: enter object, `key(attname)`, the
  attribute's jsonb as the value). An attribute no path names is not shredded (an error for a jsonb attribute with no path: it is not a
  scalar type the engine holds).
- **Leaves.** A column in the root group is a SCALAR leaf (store from its `SiftShredType`: bool/int2/int4/int8 -> INT, float4 -> F4, float8 -> F8,
  text -> STR; `path` = the canonical flat name). A list group (`key[]`) is a GROUP leaf (path `items[]`) whose columns are its
  children. A map group (`key.*`) is a GROUP leaf (path `metrics.*`) with an extra STR child `metrics.*#key` (the member name) before its
  value columns, so `metrics.* float4` gives rows (key, value) per element. One level of groups only: a group below another group is
  refused at CREATE INDEX (`ERRCODE_FEATURE_NOT_SUPPORTED`, "nested lists deeper than one level").
- **States.** ABSENT and NULL are NULL; VALUE is a value; OVERFLOW (a value that does not fit the declared type: a text longer than its `text(N)`,
  a number out of range) is NULL with the `overflow` flag: the value exists in the heap but the index cannot hold it. A chunk with any overflow sets
  `SIFT_COLC_OVF` (the per-value overflow bitmap) and its zone `SIFT_COLZ_OVERFLOW`; the source reports it (`SiftColVec.ovf`,
  `SiftColZone.has_ovf`) and the rule is the shredder's: **no pruning, no zone/metadata answer and no definite answer from such a leaf in
  such a unit**: the plan track either refuses the pushdown or rechecks those rows against the heap. A list under a parent whose list
  state is OVERFLOW flags every column below; the adapter turns that into overflow values for the elements it has, and, when the list is
  cut or not a list, an `overflow` NULL element is *not* invented: the row's GROUP count is NULL with `overflow` (the whole array is the
  heap's).
- **Absent array vs empty array**: GROUP count NULL (ABSENT / NULL list) vs 0 (empty list).
- **Memory**: strings point into `doc.text` (reset by the caller per row). Wildcard keys use the shredder's `SiftCDict` (`sift_cdict_str`).

Planner side (col-plan): a pushdown matches the *jsonb expression of the attribute* (`sift_col_att_expr`, an `equal()` match after
planning) and the leaf `path`; `jsonb_to_recordset(<expr>-> 'items')` / `EXISTS`-over-elements shapes map to the GROUP leaf and its
children.

## 7. The column source API

`src/col/sift_col.h` is normative. In outline (the executor owns control flow, the core owns data):

```
src = sift_col_open(heap, index, snapshot, mcxt, flags)       directory snapshot, VM/visibility state, privilege/RLS/SSI checks
sift_col_begin(src, preds, npreds, sb_lo, sb_hi)              candidate units of the SB range [sb_lo, sb_hi) after zone pruning
while (sift_col_next(src, &unit))                             ascending SB, segments in directory order, write buffer units last
    sel = all live rows of the unit                           sift_col_sel_all
    sift_col_filter(unit, preds, n, &sel)                     scalar predicates, AND
    sift_col_filter_group(unit, gp, &sel)                     array-element predicates, same-element semantics
    sift_col_filter_tids(unit, filter, &sel)                  AND with a sift_filter.c TID mask (a BM25 match bitmap, btree bitmaps)
    sift_col_vet(unit, &sel)                                  MVCC (section 3)
    sift_col_read(unit, col, &sel, &vec)                      decode a leaf for the selected rows (children: their elements)
```

Parallelism: a worker takes **SB-range morsels** `[m*K, (m+1)*K)` from a shared counter and runs the loop above with its own source (own
directory copy; any consistent directory covers every live row, section 3 makes the answer independent of which one). `sift_col_heap_sbs`
gives the number of SBs to split. EXPLAIN data: `sift_col_counters(src)` (units seen, pruned by zones, rows scanned, rows vetted by the
heap, heap pages, bytes decoded, `simd` level name).

`sift_col_read_lane(unit, col, lane, dst, isnull, &info)` is the form the aggregation scan uses: the whole leaf decoded straight into the caller's int32, int64, double or dictionary-code array (the codec's 32-bit domain for int4, date and codes, `sift_cc_decode_f8` for floats, which divides an ALP chunk into doubles without forming float keys), NULL slots 0, the NULL bitmap expanded to 0/1 flags (`bits_to_flags`). `dst` has room for `sift_col_lane_n()` values and `SIFT_COL_PAD` bytes more. `sift_col_read` stays for selections. Reads through one selection share its positions: the unit keeps the row positions of the last selection (keyed by its bits, so another selection replaces it) and, per group, the element positions and offsets of those rows. A unit's page table (the page of the p-th page of the unit) is filled when it is parsed.

The source seam (`sift_colsrc.h`) adds `sift_colsrc_leaf_n` and `sift_colsrc_read_into(src, unit, leaf, &SiftSrcBuf, &vec)` for storage the caller owns (room for `leaf_n + SIFT_AGG_PAD` values, the pad zero on return); `sift_colsrc_read` is the same over the provider's own buffers, and a provider without `read_into` is served by a copy.

Memory: vectors returned by `sift_col_read` live in the source's scan arena and stay valid until the next read of the same leaf or the
next `sift_col_next`; a selection is allocated by the caller or the source arena (`sift_col_sel_alloc`).

## 8. SIMD kernels and the dispatch pattern (shared by every column module)

Every hot loop over column data (filters, selection vectors, null bitmaps, zone-map checks, dictionary LUTs, decode) is a function of a
**kernel table**, chosen at runtime from the CPU and `sift.simd`, never at build time. The pattern is that of `docs/KERNELS.md`:

1. One header declares `typedef struct SiftXxxKernels {...}` of function pointers, the semantics in comments; the **scalar table is the
   specification** (`sift_xxx_kernels_scalar`, baseline x86-64 code, no POPCNT/vector instruction: use `sift_pc64`-style SWAR where
   needed); `sift_xxx_avx2.c` and `sift_xxx_avx512.c` fill the same slots with `SIFT_TARGET_AVX2` / `SIFT_TARGET_AVX512` functions and must
   produce **bit-identical results** (integers: exactly; no float arithmetic in the kernels that is order dependent. Float sums are done by
   the aggregate operators with a fixed lane-independent order, defined by their owner). A slot a level does not implement points to the
   next lower level's function.
2. `extern const SiftXxxKernels *sift_xxxk;` the active table, and `const SiftXxxKernels *sift_xxx_kernels_for(SiftSimdLevel level)` (never
   NULL, falls back downwards for a level the CPU lacks) so tests can run every level.
3. `static void xxx_rebind(const SiftCpu *cpu) { sift_xxxk = sift_xxx_kernels_for(cpu->active); }` registered once with
   `sift_cpu_register(xxx_rebind)` from `sift_xxx_kernels_init()`, called by `_PG_init`. `sift_cpu_register` calls it immediately and
   again on every `SET sift.simd`. `SIFT_MAX_REBINDS` in `sift_cpu.c` is raised from 8 to 16 for the column modules (this change is in
   the first commit of the core branch; other branches take it by rebase).
4. Kernels take plain arrays and lengths: no `Relation`, no `palloc`, no ereport; portable files (`src/portable.list`, only `sift_host.h`).
   Inputs are padded per the table's comments (`SIFT_COL_PAD` = 64 bytes readable past the last used byte), outputs written in whole words.
5. A test (`sift._test_col_kernels(level)`, regression `col_kernels_simd`) runs random inputs through every level the CPU supports and
   compares against scalar bit for bit; ranges exercise all widths, thresholds at the boundaries and unaligned lengths.

Tables of the track: `SiftColKernels` (`src/col/sift_col_kern.h`, core: filters, selection, zone, LUT, null ops, unpack/decode), and the
tables of col-plan (the aggregation executor's own operators: `SiftAggKernels`, their own header and rebind), col-json (the shredder's
state byte kernels: `SiftShredKernels`) and col-compress (additional decode/encode slots go into `SiftColKernels.decode` through that
header, owner announced in its comments).

**One implementation per operation.** A generic operation on bit vectors, selections or integer columns is a slot of `SiftColKernels`
and nobody else keeps a copy: the aggregation executor's integer comparisons and `BETWEEN` are `range[SIFT_COLW_32 / SIFT_COLW_64]`
(`x op c` is one wrapping range `[lo, hi]`, `x <> c` is `[c + 1, c - 1]`), a null flag array to a bitmap is the u8 range `[1, 255]`
(`sift_colk_nonzero`), dictionary predicates are `lut[SIFT_COLW_32]` over a bit set, `and` / `or` / `andnot` / popcount are `bits_*`, the
selection vector of a bitmap is `bits_to_idx` (its output has `SIFT_COL_IDXPAD` entries of room after the last position), gathers are
`gather_u32` and `for_i64_sel[SIFT_COLW_64]` with base 0, and the owner of each element of a list column is `expand_parents`. A table
other than this one keeps an operation only when it is different in kind, and says why at the declaration: the aggregation table
keeps what is specific to the executor (float8 order comparisons, a byte LUT fused with a bound check, the row mask of array elements,
group ids, reductions, `count_changes`), the shredder table keeps its histogram, state bitmaps and min / max over *state bytes*
(no bit vector, no padded value arrays; the composition `range` + `SiftAggKernels.minmax_*` was measured and is slower for float8
and at the scalar level).

Measured (Zen 5, one pinned core, GCC 16, `-O2 -march=x86-64`, best of 300 batches under heavy unrelated host load: read ±15 % as
noise), ns per call over a vector of 4096 elements (a bit vector of 64 words) as the aggregation executor calls them:

| slot | scalar | avx2 | avx512 |
|---|---|---|---|
| `bits_to_idx` 1 % / 10 % / 50 % / 100 % set | 33 / 145 / 784 / 1690 | 41 / 156 / 219 / 216 | 42 / 74 / 94 / 256 |
| `bits_and` (64 words) | 15 | 6 | 4 |
| `bits_count` (64 words) | 57 | 8 | 3 |
| `gather_u32` (2048 of 4096) | 447 | 448 | 538 |
| `range[SIFT_COLW_8]` non-zero test | 1802 | 40 | 21 |
| `range[SIFT_COLW_32]` `<` / `=` / between | 1782 / 1779 / 1776 | 112 / 113 / 113 | 64 / 64 / 64 |
| `range[SIFT_COLW_64]` `<` | 1788 | 224 | 137 |
| `lut[SIFT_COLW_32]` (1024 codes) | 1642 | 1056 | 932 |

(`gather_u32` is a masked `VPGATHERDD` at AVX-512, the aggregation executor's kernel, moved here unchanged; the scalar and AVX2 rows are the same loop and show the noise. Against scalar the AVX-512 gather measured 0.8x to 1.7x over six runs, depending on what the sibling thread did.) `sift.simd` level info: `sift.col_index_info`
returns `simd` (the active level name), and the column scan's EXPLAIN prints `SIMD: avx512` (the level in effect, from
`sift_simd_level_name(sift_cpu.active)`).

### Measured

`tools/kernels/bench col` (`make -C tools/kernels bench-col`), one thread pinned to a core, arrays in L1/L2, best of 300 batches and the
better of two runs, G rows per second (higher is better). AMD Ryzen 7 9700X (Zen 5), GCC 16.2, `-O2 -march=x86-64`. **The host was under
heavy unrelated load (load average 13 to 41 on 16 threads)**: treat the figures as lower bounds, a repeat can differ by 1.5x either way.

| Kernel (4096 rows unless noted) | scalar | AVX2 | AVX-512 |
|---|---|---|---|
| range u8, 25 % pass | 2.6 | 113 | 215 |
| range u16 | 2.1 | 71 | 114 |
| range u32 | 2.3 | 44 | 42 |
| range u64 | 2.4 | 13 | 24 |
| lut u8 (256 codes) | 1.6 | 60 | 113 |
| lut u16 (65536 codes, gathers) | 1.6 | 3.0 | 3.9 |
| lut u32 (4096 codes, gathers) | 2.6 | 3.3 | 3.7 |
| bits_count (73728 bits) | 75 | 566 | 1735 |
| bits_and (73728 bits) | 308 | 1151 | 1962 |
| bits_to_idx, 50 % set / 0.8 % set | 5.7 / 143 | 7.1 / 136 | 46 / 148 |
| zone_range (1024 units) | 3.2 | 12 | 20 |
| zone_set (1024 units) | 2.0 | 2.9 | 3.9 |
| unpack 13 bits | 2.9 | 12 | 22 |
| for_i64 u8 | 4.3 | 17 | 26 |
| for_i64_sel u32, gather_u32 | 4.3, 4.5 | scalar | scalar |

- **Range tests** are one subtraction and one compare per vector. AVX2 has only signed compares, so the sign bit of the element width
  is added to `lo` and xored into `span` (`(x - lo) <= span` unsigned is `!((x - lo') > span')` signed, no min or blend); AVX-512 compares
  unsigned into a mask register that is the bit vector word. u32 and u64 run at the L1 load rate of the host (170 to 200 GB/s), so AVX2 and
  AVX-512 meet there.
- **Gathers lose**: `for_i64_sel` and `gather_u32` as gathers (AVX2 8 lanes, AVX-512 8 and 16 lanes) ran at 0.5x to 0.8x of the scalar loop
  (4.3 to 4.6 G rows/s, about one row per cycle), so both levels use the scalar kernels there. The u16 / u32 LUTs, which have no scalar
  alternative that avoids two dependent loads, gain only 1.2x to 2.5x from gathers; the 8-bit LUT (a dictionary of at most 256 codes) is a
  register lookup and 38x to 70x faster than the scalar loop, so chunks of small dictionaries should stay u8.
- `bits_to_idx` is 8x faster with `VPCOMPRESSB` on dense selections; a sparse one is bound by walking the words (all levels equal), and the
  AVX2 tzcnt loop gains only 1.25x on a dense one.

## 9. Invariants

- **C1** A TID is live in at most one col segment (I1 of sift).
- **C2** A unit record is immutable; only its liveness bits change (I2).
- **C3** Row `r` of a unit is the `r`-th live-or-dead row in key order; every chunk of the unit has `n = nrows` values (children:
  the sum of the group's counts) and the liveness rank of the row is `rank_base + r`.
- **C4** The zone arrays are exact for the unit's chunks (min/max/sum/nnull computed over all rows of the unit, dead ones included):
  conservative for the live rows, which is all pruning needs. `sift.col_verify` recomputes them.
- **C5** The values of a row equal the values the index definition computes from the heap tuple (`heapallindexed` checks it).
- **C6** The directory changes only through `sift_dir_update` (I3).

## 10. Limits

BLCKSZ 8192, heap tables only, PostgreSQL 17/18. At most 128 leaves, 128 segments, 74,496 rows per unit. Strings up to 1 GB (a segment dictionary holds entries up to 255 bytes and 64 MiB in all: longer or more text is stored inline per unit). Types:
integers, bool, date, timestamp(tz), numeric(p <= 18, s), float4/8, text-like. A group's elements: any number, one level. Not supported: wider `numeric`, `uuid`,
`INCLUDE` columns, unique, ordered scans, `amgettuple`, RLS. Dead rows count in zone sums until a merge rewrites the segment (the zone is
only used where the scan is entirely visible and clean, see `sift_col_unit_zone_exact`).

### Measured (index on a table of 1.2 million orders: 6 plain columns, 4 jsonb attributes with 14 shredded paths, two nested arrays; busy host)

| | plain FOR/PACK chunks | chunk codec (`auto`) |
|---|---|---|
| index size | 116 MB | 39 MB |
| `CREATE INDEX` (serial) | 6.0 s | 5.4 s |
| scan of all rows, live + visibility only | 22 ms (18 ns/row) | 3.5 ms (2.9 ns/row), after stored units were read lazily |
| one or two filters on plain leaves | 24-28 ms | 3-5 ms |
| same-element filters on a nested array / a map | 52-54 ms | 17-22 ms |

A stored unit is read as its head (row directory and chunk table) and then only the chunks a scan touches (`sift_cseg_unit_head` and
`sift_cseg_unit_fill`), so a filter on two leaves of thirty reads two chunks. The numbers vary with the load of the host by a factor of two.

## 11. Tests

Regression (`test/sql/col_*.sql`): DDL/options/refusals, build equivalence (every filter shape against the heap), insert/update/delete/
rollback/VACUUM vs the heap in every state of the VM, parallel build (`col_index_parallel`: workers, segments, REINDEX [CONCURRENTLY], a failing participant, against a serial build), nested same-element predicates against `jsonb_array_elements`, verify, kernels at every
SIMD level. TAP: `kill -9` in build/insert/promotion/merge/vacuum then verify + equivalence; physical standby. Differential: random
tables and predicates, col read vs SQL. `sift._test_col_*` exposes the read API for these.

## 12. Layers: the engine and the index AM glue

The storage engine is separate from the index AM so that the table AM `sift_table` (col-tam) can reuse it with native MVCC.

| layer | files | knows about | reused by the table AM |
|---|---|---|---|
| L0 kernels | `sift_col_kern*.c/.h` (portable) | arrays and bit vectors | yes |
| L1 chunks, batches, dictionaries, zones | `sift_col_enc.c/.h` (portable): `SiftColBatch`, chunk encode/decode, predicate -> key range / LUT, zone computation, batch merge | values, not pages | yes |
| L2 segment | `sift_col_seg.c/.h`: segment writer (`sift_colw_*`), reader (units, dictionaries, zone arrays, row directory, liveness) over `sift_store.h` | extents, pages, the directory | yes: a table AM keeps its row groups as such segments; the row directory's TIDs are then the table's own TIDs |
| L3 write buffer | `sift_col_wbuf.c/.h` (lanes: `sift_lane.c/.h`, shared with sift) | lane pages, row records | yes, or its own write buffer |
| L4 source | `sift_col_scan.c`: `SiftColSource`, units, filters, reads, selections (`sift_col.h` section 3) | a visibility callback | yes: it asks `SiftColVisibility` (below) which rows are visible |
| L5 index glue | `sift_col_am*.c`: `ambuild`, `aminsert`, `ambulkdelete`, handler, options, the heap/VM visibility (`sift_col_vet`), SQL | the heap table | no |

The one hook between L4 and L5 is the visibility rule: L4 asks the `SiftColVisibility` callbacks (`sift_col_set_visibility`, `sift_col.h`) which rows of a unit are visible; the index
glue answers with the VM mask plus heap checks (section 3), a table AM with its own row versions / delete vectors. Chunks, zone maps,
the nested column model, the dictionaries, the kernels and the write buffer record format are independent of the heap.

## Changes (contract log, newest first; rebase on these)

- 2026-10 (col minor 4, element cubes): the `COLP` directory is version 2 in a minor 4 segment (`ecubedir_off`, `ecube_bytes`; the cube blob of each list follows the cubes of the columns), `sift_cpagg_cube(..., StringInfo out, StringInfo eout)`, `sift_cpagg_elem_cube`, `sift_cseg_ecube`, `sift_cseg_dim_cube`, `SIFT_PAGG_SPEC_DIMS` 32 and `SIFT_SRC_MAX_DIMS` 32; `sift_aggspec_pred_leaf` takes the `group` whose elements the predicate tests (-1: the rows); `sift_aggspec_preagg` accepts the unnest of an array leaf with one element-level key; `preagg_hist` folds the float8 sums of a group (`PaVal.sumf`). The writer remembers the minor it began under (`SiftColSegWriter.minor`), so `sift.col_format_minor` is read once per segment.
- 2026-10 (preagg2 maint): `SiftColOptions.dead_rewrite_pct` (reloption `dead_rewrite_pct`, default `SIFT_COL_DEAD_PCT` 10, maintenance only: no format change); `sift_col_mostly_dead` uses it instead of a fixed half.
- 2026-10 (col minor 3): the dictionaries of a segment are written as `u32 form, u32 n` and either the plain list or the codec's string list (section 2.3); `sift_cdict_serialize` takes the leaf's policy, `sift_cseg_dict_stored`, `sift.col_sections` (section 2.10). A minor 2 segment is refused with a REINDEX hint.
- 2026-10 (preagg2): `SiftSrcPreagg.aggs`; `sift_aggspec_preagg`, `sift_aggspec_pred_leaf` (sift_aggspec.h); SiftAgg answers the units that are exact from `unit_agg` / `unit_hist` (`sift_agg.c` `preagg_unit`), `sift.agg_preagg` (GUC, on), the cost of the exact share of the units and no workers from 98% exact (`sift_aggplan.c`), `Units From Pre-aggregates` in EXPLAIN ANALYZE. A provider that has `unit_exact`, `unit_agg`, `unit_hist` and `preagg` gets all of it; the others scan.
- 2026-10 (col minor 2): the pre-aggregates (docs/COL-PREAGG.md): the PAGG block at the end of the ZONES section, writer `sift_colw_set_preagg` (default spec unless the owner sets one; the index passes the `preagg` reloption), reader `sift_cseg_agg` / `sift_cseg_cube`; `sift_col_pagg.h` (`sift_col_unit_preagg_ok`, `sift_col_unit_agg`, `sift_col_unit_hist`); source callbacks `unit_exact`, `unit_agg`, `unit_hist`, `preagg` in `sift_colsrc.h`; `sift.col_unit_agg`, `sift.col_unit_hist`, `col_index_info` columns `preagg_bytes`, `preagg_units`. `sift_col_unit_exact` tests the page mask word wise. Segments of the minors 0 and 1 are not read.
- 2026-10 (decode): `sift_col_read_lane`, `sift_col_lane_n`, `SiftColLane`, `SiftColLaneInfo`; `sift_colsrc_leaf_n`, `sift_colsrc_read_into`, `SiftSrcBuf` and the provider slots `leaf_n`, `read_into`; `SiftColKernels.f8_from_keys`, `bits_to_flags`; `SiftCcKernels.scale32`, `scale64`, `alp_f8`, `alp_f4`, `sift_cc_decode_u32`, `sift_cc_decode_f8`, `sift_cchunk_decode_u32`, `sift_cchunk_decode_f8`; **`SiftColKernels.expand_parents` writes up to `SIFT_COL_PAD` bytes past `end[np - 1]`**: allocate the room; `SiftCUnitView.page`; `SiftColLeafStats`, `sift_cseg_leaf_stats`, `sift_col_leaf_stats`; `SiftSrcStats.ndistinct`, `min`, `max` and `elems_per_row` are filled (section 5).
- 2026-10 (col minor 1): `SiftColDef.typid` (Oid) is `SiftColDef.type`, the `SiftColType` tag (section 2.1); chunk encoding 6 `ISTR`, zone flag `SIFT_COLZ_INLINE`
  (section 2.9); `SIFT_COL_MAX_STR` is gone (`SIFT_COL_MAX_VALUE`, `SIFT_COL_DICT_MAX_ENTRY`, `SIFT_COL_DICT_MAX_BYTES`); `sift_cunit_encode` takes `inl[]`
  after `dictmap`; `SiftCTest.pred`, `SIFT_CT_STRS`, `sift_ctest_make_strs`, `sift_cchunk_istr_codes`, `sift_cchunk_strs_length`, `sift_cbatch_str_inline`;
  the lane stores string lengths as varints; `sift.col_leaves` has `inline_chunks`. `-0.0` keeps its sign in the float key.
- 2026-10: the history of the column engine is squashed into seven commits (contract, kern, enc, shred, codec, lane, index) on `feat/col-core-clean`, which has the tree of `feat/col-core`; the contract commit stays first. Take that branch for the integration.
- 2026-10: chunk encoding 4 (`CC`) is written: reloption `compression`, `sift_colw_begin(..., pol, ...)` and `sift_cunit_encode(..., pol, ...)` take the policy of each leaf (array, NULL = plain chunks), `SiftColRel.policy`, `sift.col_leaves.other_chunks` is now `codec_chunks`. The column engine headers and sources (`sift_col.h`, `sift_col_enc.[ch]`, `sift_colcodec*`) are listed in `src/portable-col.list`: they are checked by `check-portable.sh` but SQLite and MariaDB do not compile them yet (`sift_col.h` still carries the PostgreSQL types of the SQL entry points and the planner hooks).
- 2026-10: `numeric(p <= 18, s)` leaves: `SiftColDef.scale` (serialized in the schema, hash included), INT storage of the scaled value, `sift_col_numeric_key` / `sift_col_key_numeric` in `sift_col_am.h`.
- 2026-10: chunk encoding 4 (`CC`, the chunk codec blob) is reserved in section 2.2; the values at NULL slots of a decoded chunk are unspecified (they were 0).
- 2026-10: for the table AM (section 2.8): `SIFT_COLS_VAR` and `SiftColVec.voff/vbytes` (implementation after the index milestone),
  `sift_colw_set_tag` (`SiftSegEntry.reserved`), `SIFT_COL_META_NEXT_VBLOCK`, `sift_col_add_batches`.
- 2026-10: the shredder interface is col-json's `sift_shred.h` (section 6); the core adapts it (`sift_col_shred_*` in `sift_col.h` section 4).
  `SiftColValue.overflow`, `SiftColVec.ovf`, `SiftColZone.has_ovf`, chunk flag `SIFT_COLC_OVF`, zone flag `SIFT_COLZ_OVERFLOW` added.
- 2026-10: the index AM is `sift_col`, the table AM `sift_table`; engine headers `sift_col_enc.h` (L1), `sift_col_seg.h` (L2), `sift_col_wbuf.h` (L3).
- 2026-10: first contract: `sift_col.h`, `sift_col_kern.h`, kernel dispatch pattern (section 8), `SIFT_MAX_REBINDS` 16.
- 2026-10: `SIFT_COLS_VAR` is written and read: chunk encoding 5 (`STRS`, the codec's string list), `SiftColVec.voff/vbytes`, `sift_cchunk_strs`.
