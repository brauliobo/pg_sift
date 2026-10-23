# Format numbers

The one table of every number that is written to disk or checked on upgrade, and who claims it. A new feature takes the next free number here
before it takes one in the code; two features never share a number or a flag.

## 1. Extension script

| version | script | from |
|---|---|---|
| 3.0 | `sql/pg_sift--3.0.sql` | `sql/pg_sift--1.2--3.0.sql` |
| 3.1 | `sql/pg_sift--3.1.sql` | `sql/pg_sift--3.0--3.1.sql` (3.0.x, every patch release, update to 3.1 with `ALTER EXTENSION pg_sift UPDATE`) |
| 3.2 | `sql/pg_sift--3.2.sql` | `sql/pg_sift--3.1--3.2.sql` |
| 3.3 | `sql/pg_sift--3.3.sql` | `sql/pg_sift--3.2--3.3.sql` |

`sql/pg_sift--3.3.sql` is the complete script and `sql/pg_sift--3.2--3.3.sql` adds the same objects to a 3.2 install (`test/t/extension_lifecycle.sh`
compares the two). The scripts of a released version never change, so that an install updated in place gets what a fresh one does: the objects added
after a release go into the next version's two scripts, and `scripts/ci.sh --selftest` checks the released ones against `scripts/released-sql.sha256`. The on-disk formats below do not depend on the script: the library reads whatever the files hold.

## 2. Metapage (`SiftMetaPageData`, `src/core/sift.h`)

Format major 1. The minor is what a build must read to understand an index or table; a build refuses a larger one with a REINDEX hint
(`check_meta`). A metapage starts at `SIFT_FORMAT_MINOR_BASE` (2) and only the first segment that needs more raises it, nothing lowers it, so an
index that never got a newer structure stays readable by 3.0.

| minor | meaning | raised by |
|---|---|---|
| 1.1 | term blobs may end with a position stream | BM25 index with positions |
| 1.2 | segment format 2 | BM25 index |
| 1.3 | BM25: segments of segment format 3. `sift_table`: a column group with unfrozen rows (`SIFT_SEG_VERS`) | the first such segment |

`sift_col` indexes are never raised: their segment formats are the column engine's (section 4).

Metapage flags: `SIFT_COL_META_FLAG` 0x00010000 (a `sift_col` index), `SIFT_TBL_META_FLAG` 0x00020000 (a `sift_table`); the low byte is the
failure count (`SIFT_META_FAILURES_MASK`).

## 3. BM25 segments (`SiftSegEntry.format`, access method `sift`)

| format | name | written by |
|---|---|---|
| 1 | `SIFT_SEGFMT_RANKBITS`: one liveness bit per doc in rank order | SQLite and MariaDB hosts |
| 2 | `SIFT_SEGFMT_LIVEROWS`: liveness rows per superblock, dead-posting table | 3.0 |
| 3 | `SIFT_SEGFMT_PACKED`: dictionary blocks, narrower docmap rows, packed containers, position blocks (perf/BM25-SIZE.md section 5) | 3.1 PostgreSQL |

A 3.1 build reads 1, 2 and 3 side by side; a merge writes the current format. 3.0 refuses a segment of format 3 through the metapage minor
(section 2).

## 4. Column segments (`SiftSegEntry.format` of access methods `sift_col` and `sift_table`)

| format | meaning | written by |
|---|---|---|
| 3 | 3.0 layout (dictionaries as codec string lists) | 3.0, and 3.1 with `sift.col_format_minor = 3` (a `sift_table` group without a version section) |
| 4 | 3.1: type tags (`SIFT_COLC_TAGS`) and kinds (`SIFT_COLC_KINDS`) of overflowing values, element rollups (`COLP` directory v2), optional version section | 3.1 default |
| 5 | 3.2: a schema with wide leaves; written only for such an index | 3.2 |
| 6 | 3.2: complete JSON kinds for path leaves, including element leaves; only when every input preserves them | 3.2 default for declared JSON paths |
| 7 | 3.3: min and max per group of the cube's integer measures; only an index with `cube_minmax` (the default) | 3.3 default (setting 7 or more), for an index with the option |
| 8 | 3.3: the cube holds, for every date, timestamp and timestamptz column, the groups of its calendar cells as a dim, at the finest grain that fits (days, months or years; the directory leaf is `grain * SIFT_PAGG_CAL` plus the column); only a segment of a schema without JSON path leaves | 3.3 default of `sift_table` and of `sift_col` (`calendar_columns`, default all) |
| 9 | 3.3: a unit also holds the rollup of its rows by the scalar columns of `rollup_dims` (the element-rollup blob, leaf `ncols`); only an index with the option | 3.3 default (`sift.col_format_minor` 9 or more), for an index with the option |
| 10 | 3.3: the exact keys or bloom filter of the leaves of `key_columns` in a unit; only for an index with the option under `sift.col_format_minor` 10 or more | 3.3 default (setting 10 or more), for an index with the option |
| 11 | 3.3: the write buffer of a `sift_table` that holds a row on tail pages (`table/table.h`); a build that does not know it cannot write the table | 3.3 |
| 12 | 3.3: the sidecar also holds, per group of units, the merged leaf sums and cube (a superunit summary); only for an index with `superunit` (the default is 16) | 3.3 default (setting 12), for an index with the option |
| 13 | 3.3: the sorted run of the leaves of `order_columns` in a unit; only for an index with the option | 3.3 default (setting 13), for an index with the option |
| 14 | 3.3: a string dictionary of 1,024 strings or more is stored in blocks of 64 strings that are read in place where that adds at most 3% to the segment, and with the codes in byte order too (form 2) for the columns of `key_columns`, `point_columns` and `return_columns`, with `dict_lookup = on` for every column, and in a `sift_table` with `sift.table_dict_lookup = on`; also the point directory of the leaves of `point_columns` at the end of the `DICT` section, only for an index with the option | 3.3 default (setting 14), for a dictionary of that size that is stored in blocks or asks for the order, or an index with the option |
| 15 | 3.3: the key list or bitmap of the leaves of `key_columns` in a unit; written by default for the int2, int4 and date foreign key columns and `*_id` columns that fit | 3.3 default (setting 15) |

`SIFT_COL_FORMAT_MIN_READ` is 3; 3.0 reads only 3 and refuses 4 with a REINDEX hint. 3.2 reads 3 through 6, 3.3 3 through 15 (`SIFT_COL_FORMAT_MAX_READ`). A 3.1 library refuses minors 5 and 6, and a 3.2 library minors 7 to 15, with a format error and a REINDEX hint; `sift.col_format_minor = 7` or less writes a `sift_table` without the month dims, 8 or less ignores `rollup_dims`, and 9 or less the summaries of `key_columns`, 11 or less `superunit`, 12 or less `order_columns`, 13 or less the stored form of the dictionaries, 14 or less the key lists and bitmaps of `key_columns`, which every one of them reads. Old segments remain readable; predicates needing unavailable kinds use the normal plan. REINDEX and a table VACUUM FULL rebuild from the original documents and restore complete kinds. `sift.col_format_minor` accepts 3 to 15 (default 15, the newest 3.3 writes; 3.2 defaulted to 6); 5 is selected only by a wide schema. Schemas without JSON paths stay at 4 (or 5 for wide leaves). Minor 6 guarantees complete JSON kinds, including element leaves. Its chunk bytes are the minor-4 layout; the new number distinguishes them from a minor-4 merge that lost the kinds of older inputs. New column write buffers carry format 6 in their directory entry; an older unmarked buffer is decoded with uncertainty for untagged NULL/astext slots and promotion never labels those slots complete.

The default (15) allows every section, and a segment is stamped with the lowest minor that describes what it holds. A `sift_col` index created with `cube_minmax = false`, `superunit = 0` and `calendar_columns = 'off'` stays at 4 or 6 and a 3.2 library reads it. With the defaults a 3.3-built index is no longer readable by a 3.2 library (it carries minors 7 and 12 at least), nor is a `sift_table` with a `date`, `timestamp` or `timestamptz` column (8), nor an index with `rollup_dims`, `key_columns`, `order_columns`, nor an index with a dictionary of 1,024 strings or more that is stored for a lookup (14, not the default): the 3.2 library refuses it with the REINDEX hint unless `sift.col_format_minor` was lowered to 6 before it was written. UPGRADING.md section 4 lists the setting that keeps each minor and the sequence that takes a 3.3 database back to a 3.2 library.

A `sift_table` follows `sift.col_format_minor` too,
except that a group with a version section (unfrozen rows) is at least `SIFT_COL_FORMAT_VERS` (4). Minors 0 to 2 were never released. A `sift_col` index goes back to 3.0.x by `REINDEX` there; a `sift_table`, which cannot be reindexed, stays readable by 3.0.x (a standby on 3.0.x included) when `sift.col_format_minor = 3` (superuser) is set before its first write or before an `ALTER TABLE ... SET ACCESS METHOD` rewrite. `pg_upgrade` copies the files as they are. The chunk
flags are `SIFT_COLC_NULLS` 0x1, `SIFT_COLC_OVF` 0x2, `SIFT_COLC_TAGS` 0x4, `SIFT_COLC_KINDS` 0x8 (`src/col/enc.h`).

## 5. Segment entry flags (`SiftSegEntry.flags`)

| flag | value | meaning |
|---|---|---|
| `SIFT_SEG_DIRTY` | 0x0001 | some liveness bit cleared |
| `SIFT_SEG_POSITIONS` | 0x0002 | written with word positions |
| `SIFT_TBL_SEG_WBUF` | 0x0100 | `sift_table` write buffer |
| `SIFT_TBL_SEG_TOMB` | 0x0200 | `sift_table` tombstone segment |
| `SIFT_SEG_VERS` | 0x0400 | column segment with unfrozen rows (`reserved` is the smallest xid); cleared when the rows are frozen |

`SiftSegEntry.sum_dl` of a `sift_col` write buffer is the mask of the leaves that hold a JSON null; 3.2 only.
`SiftSegEntry.reserved` is the owner's tag for an immutable `sift_col` segment and the smallest xid when `SIFT_SEG_VERS` is set.
A group written by 3.0.1 under pressure with a tag but no version section is read as a constant version vector.

Zone flags: `SIFT_COLZ_NOSUM` 0x1, `SIFT_COLZ_NAN` 0x2,
`SIFT_COLZ_CONST` 0x4, `SIFT_COLZ_OVERFLOW` 0x8, `SIFT_COLZ_INLINE` 0x10, `SIFT_COLZ_JNULL` 0x20 (3.2, segment format 6: a NULL slot of the leaf is
a JSON null; informational, a segment written before the flag does not have it and is read as one that may).

## 6. Upgrade tests

| what | test |
|---|---|
| BM25 files of 2.x and 3.0 read and maintained by 3.1 | `test/t/compat_2x_fixture.sh`, `test/t/compat_30_fixture.sh` |
| every 3.1 object after `CREATE EXTENSION` and after `ALTER EXTENSION .. UPDATE` from 3.0; a `sift_col` index and a `sift_table` written by 3.0 (column minor 3) read across the update | `test/t/extension_lifecycle.sh` |
| an older library refuses what it cannot read, with the REINDEX or VACUUM FULL hint | `test/t/old_lib_refusal.sh` |
| real 3.0/3.1 write buffers, promotion, minor-6 REINDEX and 3.1 refusal | `test/t/compat_31_column.sh` |
| the releases 3.0.2, 3.1.0 and 3.2.0 write, this tree takes over (update, verify, REINDEX, writes), and the release reads what this tree writes at the lowest minor, its newest and 4, and with the default options off, and refuses every minor above (7 to 15, and an index at the defaults) and the calendar `sift_table`; 3.2.0 gets the downgrade of UPGRADING.md section 4 end to end | `test/t/compat_matrix.sh` |
| the stored unit, chunk (flags, tags, kinds) and leaf list (wide lanes, type tags) of a segment cut and damaged byte by byte are refused with a corruption error or still sound; the cube with min and max (minor 7) | `test/sql/col_damage.sql`, `test/sql/col_preagg_minmax.sql` |
| a kill before and after each directory update of a build, promotion, merge, VACUUM, table sealing and the promotion worker, with minor 7 and generic WAL page checks | `test/t/col_crash_matrix.sh` |
| mixed minors 3/4/5/6, partition parents, table rewrite and JSON kinds | `test/sql/agg_jsonpath_kinds.sql` |
| a 3.2 index with wide leaves (minor 5) beside the older minors, merges and `REINDEX` | `test/sql/col_format5.sql` |
| `scripts/upgrade-check.sh` reports the formats of every index and table, and what a given release could not read (`--to`) | `test/t/upgrade_check.sh` |
| standby and crash recovery of the new structures | `test/t/col_index_replica.sh`, `col_index_crash.sh`, `store_replica.sh`, `store_crash.sh`, `014_tbl_writepath.pl` |
