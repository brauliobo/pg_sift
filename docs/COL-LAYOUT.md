# How a `sift_col` index is laid out

From the whole index down to one value. The examples use a table `orders(id, customer_id, status, placed_at, total, tags text[], doc jsonb)`. What each extra structure is for, and how to turn it off, is in [COLUMN-STORAGE.md](COLUMN-STORAGE.md); the format numbers are in [FORMATS.md](FORMATS.md) section 4; the shredder of `jsonb` is [COL-SHRED.md](COL-SHRED.md); the wide types are [COL-LEAFTYPES-32.md](COL-LEAFTYPES-32.md). The last section lists where each claim is in the code.

## 1. Containers from the top

The index is a file of 8 kB blocks. Block 0 is the metapage, block 1 the directory, and the directory lists every container: the immutable segments (sorted runs of rows), the write buffer rows land in first, the tombstone log, and, for a shared value store, runs of values. A segment holds units (consecutive rows), a unit one chunk for each leaf (a typed column), a chunk the encoded values of one leaf for the rows of the unit.

```
index file (8 kB blocks)
 |-- block 0   metapage        format, next generation, failure note, scope floor, read sums
 |-- block 1   directory       one page, at most 128 entries of 56 bytes (a build writes at most 121)
 |      entry kinds:
 |      IMMUTABLE ---> SEGMENT            a sorted run, never changed in place
 |      WBUF_OPEN / WBUF_SEALED ---> WRITE BUFFER   8 lanes of pages of row records
 |      TOMB ---> TOMBSTONE LOG           32-byte entries; also the key journal records
 |      VALRUN ---> run of a shared value store (in the store's own relation, not in the index)
 `-- other blocks: segment pages, buffer pages, log pages, free list

SEGMENT (directory entry: generation, format minor, flags, header block, unit id range, rows, bytes)
 |-- header page      extents of the stream, offsets of the sections, mirror of the small ranges
 |-- stream (data pages), in this order:
 |     UNITS    unit 0 | unit 1 | ... | unit n-1            8-byte aligned records
 |     DICT     one dictionary per text leaf (all units share it),
 |              then the point directories and global order arrays of the leaves that have them
 |     ZONES    zone arrays: for each leaf, min / max / sum / nnull / nvals / flags of every unit
 |              then the pre-aggregates: exact sums, cubes, element rollups, key lists and blooms,
 |              superunit groups, sketches, key tables
 |     SCHEMA   the leaf list, the cut record, the stamps of connections and scopes
 |     SBDIR    one 32-byte entry per unit: id, rows, offset, length, rank of its first row
 |     (leaf statistics, net pre-aggregates of dirty units, when present)
 `-- LIVE pages    liveness: a dirty bit per superblock of the id range (per unit when key-cut), then one bit per row (1 = live)

UNIT = the rows of one superblock (256 heap pages), or of a range of a key (key_cut)
 |-- row directory    the heap addresses of its rows
 |-- chunk table      offset and length of each leaf's chunk
 `-- chunk of leaf 0 | chunk of leaf 1 | ...

CHUNK = head (encoding, bits, flags, n) | base | null bitmap | overflow bitmap | tags | kinds | payload
```

Side structures:

| structure | what it is | where |
|---|---|---|
| write buffer | rows inserted since the last promotion, as records in pages | directory entries `WBUF_OPEN`, `WBUF_SEALED`; 8 lanes of page chains |
| tombstone log | 32-byte entries (heap address, transaction, command, kind) of entries that stopped being live, and deletion records (one head, descriptor and body slots for the rows of a unit that a statement ended, `docs/COL-HEAPFREE.md`); only with `track_changes` | directory entry `TOMB` |
| journal | not a structure of its own: records of kind `KEYCHG` (parent index) and `KEYADD` (a scope's pair relation) in a tombstone log | the log of the parent index or of the journal index |
| scope segments | segments of the rows replicated once for each scope key, cut by that key | `IMMUTABLE` entries with the flag `SIFT_SEG_SCOPE`; the entry's `pad` is the scope's ordinal |
| connected child | the child index holds leaves of its parent (`<fk>#found`, `<fk>.<column>`); its segments carry the stamp each was filled under | leaves in the schema, stamps in the SCHEMA section |
| value store | the distinct strings of a column shared by several indexes of a table, a relation `<table>_<column>_sift_values`; a member's dictionary is a list of codes into it | its own directory of `VALRUN` entries |
| parsed dictionaries | a per-backend cache, and a shared-memory cache; memory only, nothing on disk | `col/dictcache.c`, `col/dictshm.c` |

## 2. A leaf is a typed column

A leaf holds one typed column of the unit's rows, as 64-bit keys. A plain column is one leaf; a value that does not fit 64 bits is several leaves of one column (lanes); a `jsonb` path is one leaf of a group. There are five storage classes: INT (integers as they are), F4 and F8 (the order-preserving key of the IEEE bits: -0 sorts just below +0, one NaN above +Inf), STR (a code of the segment's dictionary, or the string inline) and VAR (opaque bytes; the table access method only). An index has at most 128 leaves.

| SQL type | leaves | what is stored |
|---|---|---|
| `smallint`, `integer`, `bigint` | 1 INT | the value |
| `boolean` | 1 INT | 0 or 1 |
| `date`, `timestamp`, `timestamptz`, `time` | 1 INT | days, or microseconds (since midnight for `time`) |
| `numeric(p, s)`, p <= 18, s <= 18 | 1 INT | the number times 10^s |
| `numeric(p, s)`, p <= 38, s <= 38 | 2 INT lanes | the same as a 128-bit integer: high word, low word with the sign bit flipped |
| other `numeric` | STR head + 1 INT lane | canonical image of the number; the lane is its display scale |
| `money` | 1 INT | the count of the smallest currency unit |
| `interval` | 3 INT lanes | total as a 128-bit integer (2 lanes), then month and day |
| `real`, `double precision` | 1 F4 / F8 | the order key of the IEEE bits |
| `text`, `varchar`, `char(n)`, `name` | 1 STR | dictionary code; see below |
| `uuid`, `bytea` | 1 STR | the 16 raw bytes, or the detoasted bytes; same dictionary rule as text |
| enum | 1 INT | the OID of the value (the sort order of the enum is read from the catalog at query time) |
| array | STR head (the image) + 2 INT lanes with `array_lengths` | the image; `cardinality` and `array_length(x, 1)`. Arrays of `text`, `varchar`, `int2`, `int4` and `int8` can also have their elements as a group of the `paths` spec |
| `int4range`, `int8range`, `daterange`, `tsrange`, `tstzrange`, `inet`, `cidr` | STR head + 2 INT lanes | the image, and the lanes the containment operators test (start and end of a range; the node of the block and the offset inside it for a network) |
| other ranges, multiranges, `bit`, `varbit`, `macaddr`, `macaddr8`, `timetz` | 1 STR | the image of the datum |
| `jsonb` | one leaf for each declared path, plus group leaves | section 3 |

A text leaf has one dictionary for each segment (section 1, DICT), and the unit's chunk holds codes into it. A string over 255 bytes is never a dictionary entry: a unit with such a string in the leaf, or whose strings would take the dictionary past 64 MB, stores that leaf inline instead (an `ISTR` chunk: a blob of the 8-byte prefix keys of the strings, then the string list). The other units of the segment keep using the dictionary. When several indexes of the table hold the same text column the same way, the dictionary is a list of codes of the shared value store.

## 3. jsonb: paths, groups and slots

A `paths` declaration (reloption, or `auto` from a sample) turns a document into leaves. A root column is a scalar leaf; every `[]` and `*` is a group, stored as an INT leaf that holds the element count of each row and children that hold the elements of all the rows of the unit one after the other. One level of arrays or objects is held: a group nested inside another is refused at `CREATE INDEX`.

The shredder describes a slot by a state: `ABSENT`, `NULL` (a JSON null), `VALUE` or `OVERFLOW` (the value does not fit the declared type). On disk the state is bits of the chunk: ABSENT is a null bit; a JSON null is a null bit and a bit in the `JNULL` kind bitmap; OVERFLOW is a null bit, an overflow bit and a type tag; VALUE is the value. The `end[]` offsets of the shredder are not stored: the group's counts are, and the ends are their running sums. A group slot is NULL for an absent or null list and NULL with the overflow bit for a list that is not one; the group's slot does not tell a missing list from a JSON null. A `*` group has one more child, `<group>#key`, an ordinary text leaf with the member name of each element.

Three documents, `paths = 'doc->channel text, doc->tags[] text, doc->items[]{sku text, qty int4}, doc->stats.* int4'`:

```
r0 {"channel":"web", "tags":["new","gift"], "items":[{"sku":"a","qty":2},{"sku":"b","qty":1}], "stats":{"x":1,"y":2}}
r1 {"channel":null,  "tags":[],             "items":[{"sku":"c"}],                              "stats":{"x":5}}
r2 {                                        "items":[]}

one unit of 3 rows: the chunk of each leaf
leaf                      n   slots (n = rows for a root column or a group, elements for a child)
channel (STR)             3   [code(web), NULL+JNULL, NULL]            null bitmap 011, kind JNULL 010
tags[] (group, INT)       3   [2, 0, NULL]                             ends 2,2,2
  tags[] elements (STR)   2   [code(new), code(gift)]
items[] (group, INT)      3   [2, 1, 0]                               ends 2,3,3: row p owns [end[p-1], end[p])
  items[].sku (STR)       3   [code(a), code(b), code(c)]
  items[].qty (INT)       3   [2, 1, NULL]                             "c" has no qty: ABSENT, a null bit
stats.* (group, INT)      3   [2, 1, NULL]                             ends 2,3,3
  stats.*#key (STR)       3   [code(x), code(y), code(x)]
  stats.* values (INT)    3   [1, 2, 5]
```

All the children of a group at element i come from the same object, so a test of two children of `items[]` never mixes elements. `r1.tags` is an empty list (count 0, not NULL) and `r2.tags` a missing one.

## 4. Encoding of a chunk

A chunk is the leaf's keys for the unit, with its own null and overflow bitmaps, stored by the smallest encoding that fits. The zone and the pre-aggregates of the chunk are not inside it: they sit in the segment's ZONES section, indexed by unit.

- **Head**: 8 bytes (encoding, bits, flags, n), then a `base` word, then a null bitmap (flag `NULLS`), an overflow bitmap (`OVF`) with one type-tag nibble for each overflow slot (`TAGS`), and the kind bitmaps (`KINDS`: JSON null, numbers and booleans in text leaves), then the payload.
- **Encodings**: `CONST` (every non-NULL value equal: the base only), `CC` (the chunk codec: the writer picks the smallest cascade by exact size), `STRS` and `ISTR` (strings of a VAR leaf and of an inline text leaf). A plain frame of reference with bit packing (`FOR`, `PACK`) is what an in-memory unit uses, with no compression policy.
- **Chunk codec nodes** (`col/codec.h`): constant, frame of reference with bit packing or byte packing, delta, run length, sorted dictionary, decimal floats, common divisor, and a wrapper that compresses a stream with LZ4 or zstd when the cost rule of the policy allows it (a few levels at most). A text dictionary is separate: it maps strings to codes, and the codes are then encoded like any integers.
- **Dictionary forms** (`col/enc.h`): compact (a string list), blocks of 64 strings read in place, with the strings' order for a search, the rows of each string, a hash, or trigram signatures; or a reference to the value store.
- **Zone** of a leaf and unit: min, max, sum, null count, value count, flags (`NOSUM`, `NAN`, `CONST`, `OVERFLOW`, `INLINE`, `JNULL`), and for a text leaf a 256-bit set of `code & 255`. An overflow flag means the zone does not describe the unit.
- **Pre-aggregates** of a unit: exact sum and sum of squares of each integer leaf, the cube (counts, sums, minima and maxima by the dimensions), element rollups, key lists, bitmaps or blooms of `key_columns`, HyperLogLog sketches, and, for each group of consecutive units (superunit), the merged sums and cube. Segment-wide: key tables, point directories, global order arrays, all in the DICT and ZONES sections. See [COL-PREAGG.md](COL-PREAGG.md).

What a reader reads for a query:

```
directory -> segments whose key range can match
  -> zone arrays of the leaves in the predicate (and key lists, key tables, point directory): drop units
  -> dirty bit of each unit: a clean, all-visible unit can be answered from its pre-aggregates, no chunk read
  -> unit head (the first 2 kB: row directory and chunk table), then only the byte ranges of the chunks of the leaves the query names
  -> dictionary of a text leaf, searched in place or parsed once and cached
  -> write buffer rows, decoded into batches the backend keeps; the tombstone log for rows that are not live
```

## 5. How rows get in and change

Units and segments are immutable: no operation appends to a unit or a leaf chunk. A row inserted into the table becomes a record in the write buffer; a later promotion writes the buffer's rows as a new segment, with new units. Merges and rewrites make larger segments from smaller ones. A delete never touches a unit: it clears a liveness bit, and the dead row's bytes go when a merge or rewrite rebuilds the segment.

The question of whether one row creates a unit: it does not, at insert. It goes to the buffer, which every scan reads. At promotion the rows of the buffer become one small segment with one unit for each superblock they reach (or, when the rows are sparse, units of `sift.col_unit_floor` rows; or key ranges with `key_cut`). The units of a small segment can hold a handful of rows each, and merges fold them into larger ones.

| event | what is written | rewrites a unit? |
|---|---|---|
| `INSERT` | one record in an open write buffer lane (lane = backend number mod 8). The first row of a transaction goes in at once; later rows wait in the backend and are written a page at a time (one WAL record for a run of rows), at commit or before a read. With `sift.col_wbuf_ref` on, a heap table that tracks changes keeps 16 bytes a row (the heap address and the transaction) and a read fetches the values from the heap | no |
| buffer reaches `write_buffer_limit` (default 4096 kB, 512 pages) | the buffer is sealed and a new one opened | no |
| promotion (worker, `VACUUM`, `sift.col_promote`, or an inserter that needs room) | one new segment from the live rows of the sealed buffer, plus scope segments in the same directory switch; the buffer's pages are freed in that switch | writes new units; older units untouched |
| merge or rewrite (worker, tiers, `sift.col_merge`, `VACUUM`, after a parallel build) | the live rows of the input segments are written as one segment or partitions; with `key_cut` the rows are cut again by key; the inputs are freed at the directory switch | yes: new units, dead rows dropped |
| `DELETE` with `track_changes` | a 32-byte entry in the tombstone log for each row (statement triggers), or one deletion record for each unit when the statement deletes enough rows of it. A fold or `VACUUM` then clears the liveness bits | no |
| `DELETE` without `track_changes` | nothing at delete time; reads check the heap; `VACUUM` clears the bit | no |
| `UPDATE` that changes an indexed column | the old entry ends (a tombstone with `track_changes`, else it stays until `VACUUM`) and the new version is a new record in the buffer. An update that changes no indexed column (HOT) writes nothing | no |
| fold (a worker, from the tombstone log) | the bits of the entries whose deleting transaction every snapshot sees; their tombstones are dropped | no |
| `VACUUM` | liveness bits of rows the heap removed (the unit's dirty bit first); net pre-aggregates of dirty units as a new header page and sidecar of the same segment; a lone segment is rewritten above `dead_rewrite_pct` | only the lone-segment rewrite |
| `ALTER INDEX .. SET` | nothing; applies to units written afterwards | no |

WAL: buffer appends and tombstone appends are generic WAL records of the changed bytes (at most 4 pages a record); clearing bits is the same. Promotion, merge, rewrite and the net sidecar write each new page through a shared target and log it as a full page image (the maintenance worker can throttle its WAL rate). A build writes through a bulk target.

## 6. Build

`CREATE INDEX` and `REINDEX` scan the heap, turn the rows of each superblock into a batch and encode it as a unit. Without `key_cut`, units follow the superblocks and a segment ends when its writer reaches its memory budget. With `key_cut` (or a scope), batches are held, sorted by the key and written as runs; a big table is first cut into key ranges so the runs do not overlap.

```
heap scan (256-page superblocks; a parallel build: participants claim superblocks)
  -> batch of a superblock
  -> no key_cut:  unit = batch; segment ends at the writer's budget
  -> key_cut:     batches held up to the run budget (about half of maintenance_work_mem, at least 64 MB serial)
        first run full and the table is larger: sample of keys (100,000 keys from 512 superblocks)
          -> boundaries at quantiles; ranges = ceil(4 x rows / rows of the first run), 3 to 512
          -> one temporary file for each range: rows packed as varints, in 32 kB blocks compressed with LZ4 (zstd without LZ4)
          -> read back in key order, packed into runs, each sorted by key and cut into units -> one segment per run
        else: the held rows are sorted and written as a run; runs overlap; compact merges them when the directory fills
  -> directory published in one update; a parallel build merges its participants' segments afterwards
```

A key of a range-built `key_cut` index lies in one unit (two where its rows cross a boundary); a key with more rows than a run is read back in pieces that overlap, and the first rewrite orders them. The aim of a key-cut unit is the rows of 256 average heap pages (from `reltuples`/`relpages`), at least 64 and at most 32,768; `sift.col_unit_rows` sets it. A build is refused when the temporary files of the ranges would not fit `temp_file_limit` or the temporary tablespaces. A scope index writes the entities and each scope's segments in the same scan; the stamps of the connections and scopes are written with each segment.

`CREATE INDEX CONCURRENTLY` runs the same build with these differences in the code: the merge after a parallel build is made by the leader alone, the segments carry no insert stamp, and a scope index writes only the entities in the first pass (the scope segments are filled in a second pass under the journal's stamp). Rows that arrive during the build reach the index through `aminsert`, the write buffer (PostgreSQL's validation phase; not checked in this guide).

## 7. Numbers in the code and docs

| what | value |
|---|---|
| block size | 8,192 bytes (compile-time check) |
| directory | one page; at most 128 entries of 56 bytes; a build writes at most 121 (`SIFT_COL_BUILD_SEGMENTS`) |
| leaves in an index | at most 128 |
| superblock | 256 heap pages |
| rows of a unit | at most 74,496 (256 pages of 291 line pointers); key-cut: aim = 256 average pages, 64 to 32,768 (1,024 with no statistics); `sift.col_unit_floor` 2,048 for sparse promotions |
| rows of a segment | at most 2^26, whatever the budget |
| dictionary entry | at most 255 bytes; a segment dictionary at most 64 MB; blocks of 64 strings |
| write buffer | 8 lanes; `write_buffer_limit` default 4,096 kB; a buffer of format 55 has the range of one integer key in the head of each page (16 bytes: `kmin`, `kmax`) and a scan that asks for keys skips the pages that hold none of them (`sift.col_wbuf_key_range`, `docs/FORMATS.md` 52) |
| tombstone entry | 32 bytes |
| unit head read first | 2,048 bytes |
| superunit fan | default 16 units, at most 256 |
| key ranges of a build | at most 512; sample of 100,000 keys |
| shared dictionary cache | `sift.col_dict_shared_cache_kb`, default 64 MB |

## Where in the code

| claim | file and function |
|---|---|
| metapage, directory, segment entry, block numbers | `src/core/page.h` (`SiftMetaPageData`, `SiftDirectoryData`, `SiftSegEntry`); `src/core/sift.h` (`SIFT_MAX_SEGMENTS`) |
| segment stream order, header, entry flags | `src/col/seg_write/seg_write.c` (`sift_colw_finish`); `src/col/seg.h`; `src/col/seg_int.h` |
| unit record: row directory, chunk table | `src/col/enc.c` (`sift_cunit_encode`, `unit_put_directory`, `unit_put_directory_keyed`) |
| chunk header and encodings | `src/col/enc.c` (`chunk_encode`, `chunk_encode_istr`); `src/col/enc.h` (`SiftCChunk`, `SIFT_COLC_*`, `SIFT_COLENC_*`) |
| chunk codec nodes and wrappers | `src/col/codec.h` |
| dictionaries, forms, inline rule | `src/col/seg_write/dict.c` (`segw_write_dicts`); `src/col/enc.h` (`DICT_*`, `sift_cbatch_str_inline`); `src/col/api/format.h` (limits) |
| zones, pre-aggregates, key tables | `src/col/seg_write/summary.c` (`segw_write_zones`, `write_pagg`); `src/col/seg_int.h` |
| point directories, global orders | `src/col/seg_write/points.c` (`segw_write_points`); `src/col/point.h`; `src/col/gorder.h` |
| liveness pages | `src/core/live.h`; `src/col/seg_maint.c` (`sift_cseg_bulkdelete`, `sift_cseg_clear_keys`) |
| leaf lists of the SQL types | `src/col/types.c` (`sift_col_att_leaves`); `src/col/shape.c`; `src/col/api/leaf.h`; `src/col/wide.h` |
| jsonb groups and slots | `src/col/jsonb.c` (`add_group`, `fill_group`); `src/col/shred.h`; `src/col/shred_spec.c` |
| insert into the write buffer | `src/col/am/am.c` (`sift_col_aminsert`); `src/col/wbuf/wbuf.c`; `src/col/wbuf/pending.c` (`sift_cwbuf_pend`, `sift_cwbuf_seal`) |
| scan reads the buffer | `src/col/scan/memload.c` (`scan_load_wbuf`, `scan_load_mem`); `src/col/wbuf/batches.c` (`sift_cwbuf_batches_keys`); `src/col/wbuf/read.c` (`keys_skip`) |
| promotion | `src/col/maint/promote.c` (`promote_one`, `maint_floor_rows`) |
| merge and rewrite | `src/col/maint/merge.c` (`maint_merge_segments`); `src/col/maint/merge_pick.c`; `src/col/order.c` (`sift_colk_merge`) |
| `VACUUM`, fold, net sidecar | `src/col/maint/vacuum.c`; `src/col/maint/fold.c`; `src/col/maint/net.c` |
| update and tombstones | `src/col/supersede.c` (`sift_col_supersede`); `src/core/tomb.h`; `src/col/tomblog.c` |
| scope segments | `src/col/scope.h`; `src/col/maint/scope.c`; `src/col/build/scope.c` |
| connected child | `src/col/conn_decl.c`; `src/col/conn.c` |
| value store | `src/col/vstore/`; `src/col/enc.h` (`DICT_REF`) |
| dictionary caches | `src/col/dictcache.c`; `src/col/dictshm.c` |
| build | `src/col/build/build.c`; `src/col/build/run.c`; `src/col/build/spill.c`; `src/col/build_par.c`; `src/col/rowfile.c`; `src/col/rowpack.c` |
| unit rows of a key-cut unit | `src/col/order_units.c` (`sift_colk_aim_rows`, `sift_colk_unit_rows`) |
| reading: zones, unit head and fill | `src/col/seg/prune.c` (`sift_cseg_prune`); `src/col/seg/seg.c` (`sift_cseg_unit_head`, `sift_cseg_unit_fill`) |
