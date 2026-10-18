# sift_col: a virtual columnar table over a heap table (and the column engine under it)

`CREATE INDEX ... USING sift_col (col, ..., (expression), ...)` stores the values of its columns once more, column by column, per
superblock of the heap, with zone maps. An analytic query reads those columns instead of the heap rows. There is no second table: the
heap stays the only copy that is authoritative, and the index is reached through the same TID space, visibility map and generic WAL as
the `sift` (BM25) access method, whose store, directory, mutable lanes, tiered merge and maintenance it reuses. The BM25 paths are not
touched.

Status of this document: the contract of the column track (index AM `sift_col`; the table AM `sift_table` of col-tam reuses the engine, section 12). `src/sift_col.h` is the C interface (column source, metadata, shred
interface) the other agents build against, `src/sift_col_kern.h` the kernel table. Where this file and the code disagree, fix the code or
this file in the same commit.

User-facing names: index access method `sift_col` (`pg_am.amname` is unique across index and table AMs; the table AM is `sift_table`), operator
class `sift.col_ops` (the default one, for any type), GUC `sift.enable_col`, functions `sift.col_index_info`, `col_segments`,
`col_verify`, `col_promote`, `col_merge`, test functions `sift._test_col_*`. Code: the engine is `sift_col*.c` (and `sift_col_kern*`), the index AM glue `sift_col_am*.c`.

---

## 1. Model

- **Leaf column.** The index definition becomes a list of leaf columns (`SiftColDef`, ids 0..ncols-1, at most `SIFT_COL_MAX_COLS` = 128).
  An index attribute that is a plain type is one leaf; a `jsonb` attribute with declared paths is several leaves (section 6); an array of
  objects inside it is a **group** leaf plus one **child** leaf per object field.
- **Storage class** of a leaf (`SiftColStore`): `INT` (int2, int4, int8, bool, date, timestamp, timestamptz: an int64; `numeric(p, s)` with p <= 18 and s >= 0: the number times 10^s, `SiftColDef.scale` = s, exact like the sums PostgreSQL computes while they fit int64; NaN is NULL with the overflow flag, an unconstrained or wider numeric is refused at CREATE INDEX; constants are scaled with `sift_col_numeric_key`, values come back with `sift_col_key_numeric`), `F4`, `F8`
  (float4/float8) and `STR` (text, varchar, char(n), name: a per-segment dictionary code, values up to 255 bytes; a longer value is an
  error at CREATE INDEX and INSERT, as btree rejects a too large row: index `left(col, 255)` or a hash instead). The
  other types are refused at CREATE INDEX in this version.
- **Key domain.** Every chunk holds integers: an `INT` value as it is, a float as its **order key** (the IEEE bits turned into a signed
  int64 that sorts like the float; -0 is stored as +0 and every NaN as one NaN, sorted after +Inf as PostgreSQL does), a `STR` value as a
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

## 2. Format (format major 1, col minor 0)

The metapage (block 0), directory (block 1), free list, extents, pages (`SiftPageOpaque`), generations and generic WAL protocol are those
of `sift_store.h` and `sift_page.h`, unchanged (P1-P6 of `sift_store.h`). What differs:

- `meta->flags & SIFT_COL_META_FLAG` (0x10000) marks a sift_col index; `meta->analyzer_hash` holds the **schema hash** (hash of the
  leaf list: ids, kinds, stores, type OIDs, paths) and a mismatch with the index definition is "REINDEX needed" (a reloption change that
  alters the leaves). `meta->k1 / b / sum_dl` are 0. `meta->ndocs` counts the rows of immutable segments.
- Every `SiftSegEntry` carries `format` = the col segment minor (0 now). A reader refuses a minor it does not know; older minors are
  read as written and rewritten by merge. `SiftSegEntry.ndocs` is the number of rows, `sum_dl` is 0, `bytes` the size of the stream.
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
store, u16 flags, i16 attno, u16 pathlen, u32 typid`) and `pathlen` path bytes. The hash is `hash_bytes` of the same serialization of
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
payload
```

`enc`:

| enc | payload |
|---|---|
| `CONST` (0) | none; every non-NULL value is `base` |
| `FOR` (1) | `n` unsigned values `key - base` of `bits` = 8, 16, 32 or 64 bits, plain little endian array. NULL slots hold 0. |
| `PACK` (2) | `n` unsigned values of `bits` = 1..31 bits (not 8 or 16), LSB first, in blocks of 64 values of `8 * bits` bytes (the last block padded) |
| `RLE` (3) | reserved, not written in 0 |
| `CC` (4) | the payload is one self-describing blob of the chunk codec (`src/sift_colcodec.h`, docs/COL-CODEC.md): `bits` and `base` are 0, the blob holds its own frame of reference, `min` and `max`. Readers dispatch to `sift_cc_open` + `sift_cc_filter` / `sift_cc_decode(_sel)` (a LUT test takes the codes with `sift_cc_decode_off32`). The blob starts 8-aligned and ends on a multiple of 8, so the payload is the rest of the chunk. Written by every segment writer (`sift_colw_begin` takes the policy of each leaf); an in-memory unit that is only scanned, a constant chunk and a unit built with no policy keep `CONST`/`FOR`/`PACK`, which stay readable. The reloption `compression` (`auto` default, `none`, `lz4[:N]`, `zstd[:N]`, encodings joined by `+`; per column `'auto; col=zstd:9'`, the column being the attribute name or the shredder path) sets the policy; a change applies to the chunks written afterwards (merge rewrites). `sift.col_leaves` counts the blobs in `codec_chunks`. |

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

Per segment and STR leaf, in the order the values were first seen (code = first-seen order): `u32 n`, `u32 off[n + 1]`, bytes (no padding;
`sift_cdict_parse` reports the bytes used, and refuses a value over 255 bytes). Codes are
dense 0..n-1 inside the segment and are **not comparable across segments or ordered**; a query that groups across segments maps codes to
values (`SiftColDict`), and the filter translates constants to codes per segment. A merge rebuilds the dictionary (kept in first-seen order
of the merged rows). The zone `min/max` of a STR leaf are code bounds (equality pruning), the `set` is the exact set when the segment has
at most 256 codes, else a bloom of `code & 255`.

### 2.4 Zone map pruning

`SiftColSource` evaluates, before reading anything of a unit, the predicates of the scan against the ZONES arrays of the segment
(`SiftColKernels.zone_range`: `min > hi || max < lo` clears the unit; `nnull == nvals` clears every non-null predicate; the set test for
STR `EQ`/`IN`). Units that pass are the **candidate units**. Pruning is conservative: the filters then run on the chunks.

### 2.5 Mutable segments (the write lane)

`aminsert` appends one **row record** to one of `SIFT_LANES` lane chains of the active mutable segment: the structure of `sift_mutable.c`
(lane header page with the heads and tails, `SIFT_PT_MUTDATA` pages, tail-lock ordering, sealing under `SIFT_ACTIVE_LOCK_BLKNO`,
back-pressure at the same thresholds, one generic WAL record per append). The lane machinery is one module, `sift_lane.c`, used by the BM25
doc records and by the row records; the record and its page header are `sift_col_mut.c`'s. The data page header is `SiftLanePageHead`
(`next`, `nrecs`, `flags`: no term bloom), which starts with the `next` the store's walkers (`sift_store_referenced_blocks`,
`sift_store_orphans`, `sift_store_verify`) follow in either kind of lane. A record is

```
SiftColRecHead {tid(6), u16 flags (SIFT_COLR_DELETED, SIFT_COLR_CONT), u32 len}   then len payload bytes, 4-byte aligned
payload: for every top level leaf in id order: u8 flag (bit0 NULL, bit1 OVERFLOW: also NULL) and, unless NULL;
         INT: zigzag varint; F4: 4 bytes; F8: 8 bytes; STR: u8 len + bytes; GROUP: varint count (its children then carry
         count values each, after the row's scalars, child by child, every value with its own flag byte)
```

A record that does not fit the tail page is split in pieces of its payload (first piece without CONT, the rest with it) written while the
tail is held, as in sift; a row that fits an empty page moves there whole. A piece's page has `SIFT_MUTPAGE_SPANS` set. A writer that dies
between two pieces (crash, error) leaves a **torn** row: it never committed, and a reader skips it (the pieces end where the next record is not a
continuation). A string over `SIFT_COL_MAX_STR` bytes is `ERRCODE_PROGRAM_LIMIT_EXCEEDED` at append, before any lock. A repeated TID keeps its
first live record in the batches (a crashed insert is not repeated). A mutable segment is read by decoding its records into per-SB batches (key
order) and presenting each batch as a unit whose chunks are encoded in memory by the chunk writer with a throwaway dictionary
(`SIFT_COLO_NOMUTABLE` skips them: build and verification paths). The deleted flag is set by VACUUM on the first piece. The directory being
full when no active segment exists is `sift_col_maint_make_room`'s (the AM's maintenance), as `sift_maint_make_room` is for sift.

### 2.6 Build, promotion, merge

All three use one writer: `sift_colw_*` takes **batches** (`SiftColBatch`: the decoded rows of one SB, keys ascending, typed vectors
per leaf, group offsets) in strictly ascending SB order and writes the unit records, dictionaries, zones and the header. A batch for an
SB that several inputs hold (a promotion of mutable rows next to old rows, a merge of segments with overlapping SB ranges, the worker
chunks of a parallel build that split an SB) is made by the **batch merge**: decode every input unit's live rows, merge them by key, re-encode.
(An SB held by a single input segment is rewritten from its batch too: chunks are cheap to decode, and dead rows are dropped.)

- **Serial build**: `table_index_build_scan` -> row -> `SiftColBatch` of the current SB -> `sift_colw_add`; the writer targets a BULK
  `SiftWriteTarget`; one segment per `maintenance_work_mem` of buffered *encoded* bytes (rows are encoded SB by SB, so the buffer is the
  encoded size, a fraction of the heap).
- **Parallel build**: every participant scans its chunks of blocks, forms batches per SB and spills each as an encoded partial unit to
  a file of the leader's `SharedFileSet`; the leader k-way merges the files by SB (batch merge when an SB is split between workers) into the
  segments a serial build would write.
- **Promotion** of a sealed mutable segment: read records, drop deleted, sort by key, group per SB, write one immutable segment.
- **Merge**: the tiered policy of `sift_merge_pick` (fan-in 8, tier base `mutable_limit`), plus "mostly dead" through a policy callback.
  Dead rows (cleared liveness bits) are dropped; one segment results. Memory: one SB per input in flight.

### 2.7 Liveness and VACUUM

LIVE pages as in sift (`SiftLiveHeaderData`, a dirty-SB bitmap then one bit per row in rank order, rank = `rank_base` of the unit + row
ordinal). `ambulkdelete` walks every row of every immutable segment of the SBs, calls the callback with its TID and clears the bit of the
dead ones (a unit's TIDs come from its row directory; chunks are not read), marks the SB dirty; mutable records get the deleted flag.
A scan ANDs the liveness bits only in dirty SBs. Dead rows are removed from the segment by the next merge, which a segment with more
than half of its rows dead triggers.

### 2.8 Shared with the table AM: VAR leaves, the segment tag, the next vblock, delta batches

- **VAR leaves** (`SIFT_COLS_VAR`): opaque bytes of any length with an exact round trip (numeric, uuid, long text, jsonb, arrays). The
  chunk holds the null bitmap, the value lengths (FOR-packed) and the concatenated bytes; there is no dictionary, no zone min/max/sum
  (zones carry `nvals`/`nnull`) and the only predicate is IS NULL. The engine never interprets the bytes: the owner of the segment
  defines the image. Read through `SiftColVec.voff/vbytes`. The index AM does not offer VAR (CREATE INDEX refuses those types).
  Implemented after the index milestone, in the one chunk writer and reader (no table-specific fork).
- **Segment tag**: `SiftSegEntry.reserved` is the owner's tag (the table AM keeps the segment xmin there, 0 = none). The writer sets it
  (`sift_colw_set_tag`); the index AM leaves it 0; whoever rewrites a segment sets the tag of the new one.
- **Next vblock**: `SIFT_COL_META_NEXT_VBLOCK(meta)` names the metapage field (`sum_dl`) the table AM uses for its TID block counter.
  Col segments always have `sum_dl` 0, so the generic directory switch never changes it.
- **Delta batches**: `sift_col_add_batches` gives a source in-memory units (one SB each, rows in key order, all live) that are read
  with the stored units of the same SB as `in_mutable` units; the installed `SiftColVisibility` decides their visibility.

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
- **Versioning**: metapage major/minor as sift's (1.0 for the BM25 minor bump rules does not apply: a col index has `meta->format_minor`
  0 and the per-segment `format` carries the col minor). A newer col minor is refused with a REINDEX hint; layout changes bump it.

## 5. Planner-facing metadata

`sift_col_index_meta(index)` returns the leaf list (id, kind, store, type OID, index attribute number, parent group, path text),
the attribute expressions (`sift_col_att_expr`), whether the index is partial and whether it is valid for scans, and
`SiftColIndexStats` (rows, segments, bytes, per-leaf distinct counts from the dictionaries, zone-based min/max per leaf, share of
all-visible pages). `amcostestimate` prices the index out of every ordinary plan: the operator class has no operators, so
the planner never forms an index path; only the custom scan of the plan track uses it, through this API.

## 6. Shredding jsonb (col-json's `sift_shred.h`, through the adapter `sift_col_shred.c`)

The core never parses jsonb. col-json's shredder (`docs/COL-SHRED.md`, `src/sift_shred.[ch]`) turns a document into typed slots
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
- **States.** ABSENT and NULL are NULL; VALUE is a value; OVERFLOW (including a text longer than 255 bytes, which the STR dictionary
  cannot hold) is NULL with the `overflow` flag: the value exists in the heap but the index cannot hold it. A chunk with any overflow sets
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

`src/sift_col.h` is normative. In outline (the executor owns control flow, the core owns data):

```
src = sift_col_open(heap, index, snapshot, mcxt, flags)       directory snapshot, VM/visibility state, privilege/RLS/SSI checks
sift_col_begin(src, preds, npreds, sb_lo, sb_hi)              candidate units of the SB range [sb_lo, sb_hi) after zone pruning
while (sift_col_next(src, &unit))                             ascending SB, segments in directory order, mutable units last
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

Tables of the track: `SiftColKernels` (`src/sift_col_kern.h`, core: filters, selection, zone, LUT, null ops, unpack/decode), and the
tables of col-plan (aggregation operators: `SiftAggKernels`, their own header and rebind) and col-compress (additional decode/encode
slots go into `SiftColKernels.decode` through that header, owner announced in its comments). `sift.simd` level info: `sift.col_index_info`
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

BLCKSZ 8192, heap tables only, PostgreSQL 17/18. At most 128 leaves, 128 segments, 74,496 rows per unit. Strings up to 255 bytes. Types:
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
rollback/VACUUM vs the heap in every state of the VM, nested same-element predicates against `jsonb_array_elements`, verify, kernels at every
SIMD level. TAP: `kill -9` in build/insert/promotion/merge/vacuum then verify + equivalence; physical standby. Differential: random
tables and predicates, col read vs SQL. `sift._test_col_*` exposes the read API for these.

## 12. Layers: the engine and the index AM glue

The storage engine is separate from the index AM so that the table AM `sift_table` (col-tam) can reuse it with native MVCC.

| layer | files | knows about | reused by the table AM |
|---|---|---|---|
| L0 kernels | `sift_col_kern*.c/.h` (portable) | arrays and bit vectors | yes |
| L1 chunks, batches, dictionaries, zones | `sift_col_enc.c/.h` (portable): `SiftColBatch`, chunk encode/decode, predicate -> key range / LUT, zone computation, batch merge | values, not pages | yes |
| L2 segment | `sift_col_seg.c/.h`: segment writer (`sift_colw_*`), reader (units, dictionaries, zone arrays, row directory, liveness) over `sift_store.h` | extents, pages, the directory | yes: a table AM keeps its row groups as such segments; the row directory's TIDs are then the table's own TIDs |
| L3 mutable lane | `sift_col_mut.c/.h` (lanes: `sift_lane.c/.h`, shared with sift) | lane pages, row records | yes, or its own write buffer |
| L4 source | `sift_col_scan.c`: `SiftColSource`, units, filters, reads, selections (`sift_col.h` section 3) | a visibility callback | yes: it asks `SiftColVisibility` (below) which rows are visible |
| L5 index glue | `sift_col_am*.c`: `ambuild`, `aminsert`, `ambulkdelete`, handler, options, the heap/VM visibility (`sift_col_vet`), SQL | the heap table | no |

The one hook between L4 and L5 is the visibility rule: L4 asks the `SiftColVisibility` callbacks (`sift_col_set_visibility`, `sift_col.h`) which rows of a unit are visible; the index
glue answers with the VM mask plus heap checks (section 3), a table AM with its own row versions / delete vectors. Chunks, zone maps,
the nested column model, the dictionaries, the kernels and the mutable-lane record format are independent of the heap.

## Changes (contract log, newest first; rebase on these)

- 2026-10: the history of the column engine is squashed into seven commits (contract, kern, enc, shred, codec, lane, index) on `feat/col-core-clean`, which has the tree of `feat/col-core`; the contract commit stays first. Take that branch for the integration.
- 2026-10: chunk encoding 4 (`CC`) is written: reloption `compression`, `sift_colw_begin(..., pol, ...)` and `sift_cunit_encode(..., pol, ...)` take the policy of each leaf (array, NULL = plain chunks), `SiftColRel.policy`, `sift.col_leaves.other_chunks` is now `codec_chunks`. The column engine headers and sources (`sift_col.h`, `sift_col_enc.[ch]`, `sift_colcodec*`) are listed in `src/portable-col.list`: they are checked by `check-portable.sh` but SQLite and MariaDB do not compile them yet (`sift_col.h` still carries the PostgreSQL types of the SQL entry points and the planner hooks).
- 2026-10: `numeric(p <= 18, s)` leaves: `SiftColDef.scale` (serialized in the schema, hash included), INT storage of the scaled value, `sift_col_numeric_key` / `sift_col_key_numeric` in `sift_col_am.h`.
- 2026-10: chunk encoding 4 (`CC`, the chunk codec blob) is reserved in section 2.2; the values at NULL slots of a decoded chunk are unspecified (they were 0).
- 2026-10: for the table AM (section 2.8): `SIFT_COLS_VAR` and `SiftColVec.voff/vbytes` (implementation after the index milestone),
  `sift_colw_set_tag` (`SiftSegEntry.reserved`), `SIFT_COL_META_NEXT_VBLOCK`, `sift_col_add_batches`.
- 2026-10: the shredder interface is col-json's `sift_shred.h` (section 6); the core adapts it (`sift_col_shred_*` in `sift_col.h` section 4).
  `SiftColValue.overflow`, `SiftColVec.ovf`, `SiftColZone.has_ovf`, chunk flag `SIFT_COLC_OVF`, zone flag `SIFT_COLZ_OVERFLOW` added.
- 2026-10: the index AM is `sift_col`, the table AM `sift_table`; engine headers `sift_col_enc.h` (L1), `sift_col_seg.h` (L2), `sift_col_mut.h` (L3).
- 2026-10: first contract: `sift_col.h`, `sift_col_kern.h`, kernel dispatch pattern (section 8), `SIFT_MAX_REBINDS` 16.
