# BM25 segment format 6: the length of each field

A BM25 index built with `fields` (`sift.sections(...)`) scores a leaf of a field (`title:word`, `field_query`) with the length of that field in the document and the average of that length (BM25F), not with the length of the whole text. Format 6 (`SIFT_SEGFMT_FIELDDL`) is what keeps those lengths. Formats 1 to 5 are unchanged and read and merge as before; an index without `fields` still writes the format it wrote.

## The length of a field

`field_dl[f]` of a document is the sum of the stored tf (saturated at 255, as any tf) of the words of field `f` that are terms of their own (`SIFT_FIELD_BYTE(f)` followed by the word; the prefix terms of `prefix_index` are not counted), saturated at 65,535. It is the number of tokens of the section, but for a word that occurs more than 255 times in it, or one too long to be a term. `sift_field_dl()` computes it from the bag of terms of the document, so the build, the write buffer, the promotion and the merge of format 1 to 5 segments (from the tf of the field words of the postings) give the same number.

## Where it is stored

`SiftSegHeaderData` and the sections are unchanged. The lengths are in `DOCMAP`, after the INCLUDE records, and their directory ends the section, before the footer of format 5 (`u64 recs_end, u32 ncols, u32 nsb`, where `ncols` may be 0):

```
SiftDocSBRef[nsb]  SiftAttrRef[nsb][ncols]  SiftFieldDlRef[nsb][nfields]  SiftFieldDlTrailer  footer
```

```c
typedef struct SiftFieldDlRef       /* one per SB and field; 16 bytes (StaticAssertDecl) */
{
    uint64  off;        /* stream offset of the record */
    uint32  len;        /* its bytes, 0: no doc of the SB has a word in the field */
    uint8   dlcode;     /* width of the rows, SIFT_DL_CODE_BITS: 0 = 16, 1 = 8, 2 = 4 bits */
    uint8   pad[3];
} SiftFieldDlRef;

typedef struct SiftFieldDlTrailer   /* 272 bytes (StaticAssertDecl) */
{
    uint32  magic;                          /* "FDL2", SIFT_FIELDDL_MAGIC */
    uint32  nfields;                        /* 1..SIFT_MAX_FIELDS */
    uint64  fdir_off;                       /* stream offset of SiftFieldDlRef[nsb][nfields] */
    uint64  sum_dl[SIFT_MAX_BM25_FIELDS];   /* the length of each field over all the docs, vacuumed ones included */
    uint64  ndocs_nz[SIFT_MAX_BM25_FIELDS]; /* the docs that have a word in the field */
} SiftFieldDlTrailer;
```

A record is the `dl` rows of the SB's present pages one after the other, in the layout and width codes of the docmap's own `dl` rows (the row of a page has as many slots as its highest present offset), taken from the longest length of the field in the SB. The records follow the INCLUDE records, SB by SB and field by field. `sift.verify` checks every length against the tf of the field words of the postings and the trailer against the rows.

## Write buffer

`SIFT_WBUF_FIELDDL` (`0x0004`) in `SiftWbufDocData.flags`: `uint16 field_dl[nfields]`, `nfields` in `SiftWbufDocData.reserved`, padded to 4 bytes, follows the 16-byte header and precedes `hash[]`. A document written without it (an earlier build) is scored with the length of the whole text.

## Scores and bounds

- The average of a field is the sum of its lengths over the live documents of the segments of format 6 and of the write buffers, divided by their number. A vacuumed document leaves both sums.
- A segment of format 5 or older does not keep the lengths: its documents score the length of the whole text with the average of the whole text, as before. A merge writes format 6 for an index with fields, taking the lengths of an older segment from the tf of the field words of its postings.
- The stored bound of a posting of a field term is the bound of a document of length 0 (it does not depend on any length or average), so it stays above every score whatever the document, the field or the average: WAND and block-max bounds are unchanged in kind and need no scaling when documents are vacuumed.

# Part format: the sections that read a row by address

Format 2.0 parts of a `sift_table` carry two optional sections that make the fetch of one row by its address (a ctid, as an index scan or a join hands it over) cost one block read and not one chunk of every column.

| Section | Tag | `ext_flags` bit | Written when |
|---|---|---|---|
| SEC_TID_HASH | `0x0103` | `1 << 6` | level 1 and up, the table has an index, 4096 rows or more |
| SEC_ROW_PACK | `0x0104` | `1 << 7` | the part has SEC_TID_HASH and the pack is at most 40% of SEC_GRANULES |

There is no setting: a table with no index (`pg_class.relhasindex`) gets neither section, and a part whose pack would be over 40% of its granules gets the hash alone. A reader that does not know a section ignores it.

## SEC_TID_HASH, version 2

A header, `nbuckets + 1` entry numbers (`uint32`, the first entry of each bucket), then one entry per row of the part, ascending in a bucket. `nbuckets` is a power of two, `1 << log2_buckets`, with 2 rows a bucket on average. The bucket of an address is the top `log2_buckets` bits of `sift_fmix64(tid)`; the hash never changes.

```c
typedef struct SiftTidHashHdr           /* 16 bytes */
{
    uint32      magic;                  /* 0x48444954, "TIDH" */
    uint16      version;                /* 2 */
    uint16      flags;                  /* bit 0: the entries name rows of SEC_ROW_PACK */
    uint32      log2_buckets;           /* at byte 8 */
    uint32      nrows;
} SiftTidHashHdr;

typedef struct SiftTidHashSlotV2        /* 8 bytes */
{
    uint32      row_idx;                /* the row of the part, from 0 */
    uint16      fp16;                   /* at byte 4: the low 16 bits of sift_fmix64(tid) */
    uint16      granule_id;             /* at byte 6 */
} SiftTidHashSlotV2;
```

The section is `16 + 4 * (nbuckets + 1) + 8 * nrows` bytes; a reader checks that size, so a lookup reads the two entry numbers of its bucket and then the entries between them. `fp16` drops most rows of the bucket; it is never trusted alone, the reader checks the address in the row (the pack row or the granule) it names.

Version 1 (entry `granule_id << 13 | row in the granule`, 4 rows a bucket, no tag) is still read, and names granules, not rows. A part with SEC_ROW_PACK always has version 2.

## SEC_ROW_PACK

The rows of the part in blocks of 64 (`SIFT_ROW_PACK_ROWS`), in the order of the part, each block compressed with LZ4 (the raw codec when LZ4 does not make it smaller). Analytical scans never read it.

```
SiftRowPackHdr | SiftRowPackCol[ncols] | SiftRowPackBlockEntry[nblocks] (at dir_off) | blocks (8-aligned, from data_off)
```

```c
typedef struct SiftRowPackHdr           /* 32 bytes */
{
    uint32      magic;                  /* 0x52504B32, "RPK2" */
    uint16      version;                /* 1 */
    uint16      flags;                  /* bit 0: some string is left to SEC_GRANULES */
    uint16      ncols;                  /* the columns of the part, in the order of its SEC_SCHEMA */
    uint16      rows_per_blk;           /* 64 */
    uint32      nblocks;                /* at byte 12 */
    uint32      nrows;
    uint32      dir_off;                /* from the start of the section */
    uint64      data_off;               /* at byte 24, from the start of the section */
} SiftRowPackHdr;

typedef struct SiftRowPackCol           /* 16 bytes */
{
    uint8       kind;                   /* SiftCellKind */
    uint8       width;                  /* the bytes of an integer: 1, 2, 4 or 8 */
    uint16      pad16;
    uint32      pad32;
    int64       base;                   /* what an integer's stored distance is from */
} SiftRowPackCol;

typedef struct SiftRowPackBlockEntry    /* 16 bytes */
{
    uint32      off_qwords;             /* from data_off, in 8-byte units */
    uint32      comp_len;               /* the bytes on disk */
    uint32      raw_len;                /* at byte 8 */
    uint16      nrows;                  /* 1..64 */
    uint16      codec_flags;            /* at byte 14, bits 0..3: 0 raw, 1 LZ4 */
} SiftRowPackBlockEntry;
```

A block, uncompressed: `uint32 row_off[nrows + 1]` (from the start of the block), then the rows. A row is `uint64 tid`, `uint8 null_bm[(ncols + 7) / 8]` (a set bit is NULL), `uint16 fixed_bytes`, the non-null integer and float columns in column order (an integer is its distance from `SiftRowPackCol.base` in `width` bytes, a float is 8 bytes), then the non-null string columns in column order (`uint16` length, and the bytes).

A cell the pack does not hold has the length `0xFFFF` and is read from SEC_GRANULES: a string over 128 bytes (`SIFT_ROW_PACK_INLINE_MAX`), a value a column keeps by reference, a string of a column with exceptions. A part with a column of arrays of objects, or an exception in an integer or float column, gets no pack.

## Policy

A part gets the pack when the first 4 blocks, scaled to the part, are at most 40% (`SIFT_ROW_PACK_SHARE_PERCENT`) of the bytes of SEC_GRANULES. The pack keeps whole rows uncompressed across columns, so a table of narrow, well coded columns (the granules hold a row in a few bits) is over the share and keeps the granule path; a table with wide, incompressible columns is under it.

## Reading

The fetch of a row (`table/f2/fetch.c`) takes the fast path for an MVCC snapshot when the snapshot sees the insert of the granule without a version of each row (the granule is frozen, the inserters of the part are settled, or its rows are of one transaction), the granule has no deletion vector and the row is not a pending delete; a column delta of the part is laid over the row. Any other row takes the granule path, which gives the same row. The decoded blocks are kept in a per-backend cache (`SIFT_F2_GCACHE_MAX_BLOCKS`), charged to `sift.backend_cache_kb` and dropped with the part, the table or a relcache invalidation.

# Format 2.0 on-disk and WAL reference

Every Format 2.0 structure that is written to disk or to the log, with the size and the offsets that the build asserts (`StaticAssertDecl`). A change of a structure here is a change of its assertion in the same commit.

## sift_table visibility map

A `sift_table` has the visibility map fork (`VISIBILITYMAP_FORKNUM`) that PostgreSQL's index-only scan reads. It is indexed by the **virtual block** of a row (`vblock`, the block number of its TID), never by a page of the main fork: the main fork holds the manifest, the parts and their extents, and `vblock` can be past its end. PostgreSQL's `visibilitymap_set()` is never called: its redo (`heap_xlog_visible`) sets `PD_ALL_VISIBLE` in the main fork block of the same number. The map is written by `core/f2/vm_xlog.c` only.

### Compatibility bit and counters

| Name | Where | Value |
|---|---|---|
| `SIFT_ROCOMPAT_TABLE_VM` | `SiftMetaCommon.ro_compat_flags` (`core/f2/meta.h`) | `1 << 5`. A reader that ignores the map reads correct rows. A writer that cannot clear the bits (an older build, or a server that does not preload `pg_sift`) refuses to delete or update. Set by the first VACUUM that sets a bit. |
| `vm_all_visible_vblocks` | `SiftColMetaPageData` (`col/f2/meta.h`) | `uint32`: the vblocks that are all visible, counted in the map fork at the last VACUUM |
| `vm_all_frozen_vblocks` | `SiftColMetaPageData` | `uint32`: the vblocks that are all frozen |
| `vm_total_vblocks` | `SiftColMetaPageData` | `uint64`: the vblocks that hold at least one row at the last VACUUM (the vblocks a compaction emptied and the ones nothing reserved are not counted) |

`sizeof(SiftColMetaPageData) == 424` is unchanged: the counters sit in the bytes `maint_errmsg` gave up. The same three counters are copied into `SiftShmIndexMeta` (`core/shm/manifest.h`), whose slot stays two cache lines. A table that was never vacuumed has zeros: `allvisfrac = 0.0`.

The planner reads `allvisfrac = vm_all_visible_vblocks / vm_total_vblocks` (0 where the server cannot log the map) and `VACUUM` stores `relallvisible` and `relallfrozen` scaled to the blocks of the file, `rint(nblocks * allvisfrac)`, so that `pg_class` and `table_relation_estimate_size` agree.

### When a vblock is all visible

A vblock is marked `VISIBILITYMAP_ALL_VISIBLE` by VACUUM, under a share lock on the table (no writer holds it, so nothing is staged in a backend that VACUUM cannot see), when all of these hold:

1. **V1, closed and full.** It is below the floor `min(next_vblock, lease_lo of every live lease of the table)`, and all 291 of its rows are present. A backend that inserts reserves a run of vblocks and publishes `[lo, hi)` in a `SiftShmTidLease`; `lo` is the vblock it fills now and moves as it fills. A row that was staged and rolled back leaves an index entry whose address nothing holds; the vblock of such an address is never full, so no bit stands for it and the scan asks the table.
2. **V2, no slab row.** No row of it is in the slab of a partition (the addresses of the slab are read).
3. **V3, sealed inserters.** Every part whose envelope `[tid_min, tid_max]` covers it is frozen or sealed `SIFT_PART_FLAG_ALL_VISIBLE`, with `max_xid` older than the oldest transaction, and has no `SIFT_PART_EXT_EXPIRED`.
4. **V4, no live tombstone.** No pending delete names a row of it, and a part that covers it has no deletion vector or has one that is `SIFT_PART_FLAG_DV_SETTLED` with `dv_max_xid` older than the oldest transaction. VACUUM removes the addresses of the rows that are dead for everyone from the indexes before it settles or compacts a part, so a settled vector has no entry left in any index.
5. **V5, no column delta.** The partition has no `SIFT_PT_COL_DELTA` chain.

It is also `VISIBILITYMAP_ALL_FROZEN` when every covering part is frozen (`SIFT_PART_FLAG_UNFROZEN` clear), has no deletion vector and has `max_xid` older than the freeze limit. A table with a time to live (`SIFT_ROCOMPAT_TTL`) has no map: a part that expires drops rows without a delete.

Rows go to a vblock once and vblocks are never reused, so an insert cannot make a bit untrue. A temporary table has no map.

### Clearing

`DELETE`, and `UPDATE` (a column delta and a delete plus an insert alike), clear the two bits of the vblock of the old row (`sift_tbl_vm_clear`) before the change is staged, which is before it can be logged: replay never meets a deleted or updated row whose vblock is all visible. The clear is logged `SIFT_XLOG_VM_CLEAR`. `SELECT FOR UPDATE` and friends do not clear. `TRUNCATE` truncates the fork with the main fork, a rewrite starts a new file without one, and an unlogged table's fork is removed with the rest of its forks by the crash reset (its init fork has no map).

### Resource manager `pg_sift_table_vm`

`RegisterCustomRmgr(SIFT_RMGR_ID = 142)` from `_PG_init`, only while `shared_preload_libraries` loads the library; a standby replays the records only if it preloads the library as well. Without it nothing sets a bit and `allvisfrac` is 0.

| Info | Value | Record |
|---|---|---|
| `SIFT_XLOG_VM_SET` | `0x10` | `SiftXlVmSet`, then `uint16 word_idx[nwords]`, then `uint64 bit_or[nwords]`; block 0 is the page of the map fork, registered without the hole of a standard page |
| `SIFT_XLOG_VM_CLEAR` | `0x20` | `SiftXlVmClear`; block 0 is the page of the map fork |

| Structure | Size | Offsets |
|---|---|---|
| `SiftXlVmSet` | 24 | `locator` 0, `vm_blkno` 12, `snapshotConflictHorizon` 16, `nwords` 20, `flags` 22, `pad` 23 |
| `SiftXlVmClear` | 20 | `locator` 0, `vblock` 12, `flags` 16, `pad` 17 |
| `SiftShmTidLease` (shared memory only) | 32 | `owner` 0, `locator` 4, `lo` 16, `hi` 24 |

A word is eight bytes of the page, least significant byte first: 32 vblocks of two bits (the layout of the page of PostgreSQL's map: 4 vblocks per byte, 32,672 per page, 1,021 words).

**Redo of `SIFT_XLOG_VM_SET`:** where the server is a hot standby and `snapshotConflictHorizon` is a valid transaction id, `ResolveRecoveryConflictWithSnapshot(horizon, false, locator)` first, so that a query whose snapshot might not see a transaction the bits stand for ends before it can read them (`hot_standby_feedback = off` needs it); then the page of the map is read (zeroed where missing), the words ORed in, the LSN set. The horizon is the newest `max_xid` or `dv_max_xid` of the clean parts that overlap the page. Redo never reads the main fork. A clear needs no horizon: it only sends a query to the table.

# Format 2.0 on-disk structures

Structures of Format 2.0 that a reader or writer outside the owning source file needs to know byte for byte. Every struct has a `StaticAssertDecl` of its size and of the offsets named here.

## Column delta chain and its directory (Spec 83, WS-83-3)

A partition's `SiftColDeltaAnchor` (32 bytes, `core/f2/col_delta_anchor.h`) names the newest page of its chain of `SIFT_PT_COL_DELTA` (42) pages. A chain of up to
`SIFT_CDDIR_FLAT_PAGES` (4) pages is flat: the pages are linked newest to oldest (`prev_blk`) and `anchor.npages` counts them. The page that would make it longer builds the directory.

| Constant | Value | Where |
|---|---|---|
| `SIFT_PT_KEYJRNL` | 46 | `core/page.h` |
| `SIFT_PT_COL_DELTA_DIR` | 47 | `core/page.h` |
| `SIFT_COMPAT_COL_DELTA_DIR` | `1 << 7` of `compat_flags` | `core/f2/meta.h`; set by the manifest change that publishes the first anchor with `SIFT_CDLA_INDEXED_DIR`; a build that does not know it refuses to open the relation |
| `SIFT_CDLA_INDEXED_DIR` | `1 << 4` of `SiftColDeltaAnchor.flags` | `anchor.delta_head_blkno` names the newest `SIFT_PT_COL_DELTA_DIR` page, not a data page; `anchor.npages` still counts data pages |
| `SIFT_CDDIR_MAGIC` | `0x43444452` ("CDDR") | `table/f2/col_delta/dir.h` |
| `SIFT_CDDIR_MAX_ENTRIES` | 250 | entries of one directory page |
| `SIFT_CDDIR_SOFT_FOLD_PAGES` | 256 | a flush that leaves a chain this long asks the maintenance worker for a fold of the partition; the update that flushed does not wait and is not refused |
| `SIFT_COL_DELTA_MAX_PAGES` | 60000 | what the 16-bit `anchor.npages` can name; the only length at which an UPDATE is not admitted |

`SiftColDeltaDirHdr` (48 bytes): `magic` 0, `version` 4, `flags` 6, `crc32c` 8 (`SIFT_PAGE_CRC_OFF`), `part_id` 12, `nentries` 16, `total_delta_pages` 20, `next_dir_blk` 24 (the next older
directory page), `prev_dir_blk` 28 (reserved, `InvalidBlockNumber`), `min_xid` 32, `max_xid` 40.

`SiftColDeltaDirEntry` (32 bytes), one per data page, oldest first: `blkno` 0, `nrecs` 4, `flags` 6 (`SIFT_CDD_FLAG_TID_ONLY` 1, `SIFT_CDD_FLAG_HAS_BATCH` 2, `SIFT_CDD_FLAG_HAS_BLOB` 4), `part_gen` 8
(0 when the page holds records of several parts), `granule_lo` 16, `granule_hi` 20, `attnum_bloom` 24 (bit `attnum & 63` of every attribute a record of the page patches).

Rules:

- `total_delta_pages` of a directory page is the number of data pages up to and with its own entries. A reader takes the first `anchor.npages` entries: entries added in place since the anchor
  was published are not seen. Only the newest directory page is written in place, under the partition lock, before the anchor that counts its entries is published.
- An entry only widens. The entry of the newest data page is widened in the same generic WAL record that appends the records, so a reader that prunes by an entry never misses a record of its page.
- A reader that wants one part, a few granules or a few attributes skips the entries that cannot hold them (`SiftColDeltaPrune`); a writer, a fold and `VACUUM` read the whole chain.
- Directory pages are freed with the data pages in the manifest change that replaces the chain (`sift_col_delta_chain_extents`).

## Column limits of a record (Spec 83, WS-83-3)

`SIFT_COL_DELTA_MAX_COLS_PER_REC` is 16 for a `sift_table` record; a record of a `sift_col` index keeps `SIFT_COL_DELTA_MAX_COLS_INDEX` (8) changed leaves and the address of the row
(`SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR`, 9). An UPDATE of a `sift_table` row is a patch whenever no column of an index other than a summarising one changes and at most 16 columns change; the share
of the columns that change no longer matters. One that changes a column of a summarising index (BRIN) is a patch too and returns `TU_Summarizing`: the row keeps its address and only the
summarising indexes are told of the new values.
