# Format numbers

The one table of every number that is written to disk or checked on upgrade, and who claims it. A new feature takes the next free number here
before it takes one in the code; two features never share a number or a flag.

## 1. Extension script

| version | script | from |
|---|---|---|
| 3.0 | `pg_sift--3.0.sql` | `pg_sift--1.2--3.0.sql` |
| 3.1 | `pg_sift--3.1.sql` | `pg_sift--3.0--3.1.sql` (3.0.x, every patch release, update to 3.1 with `ALTER EXTENSION pg_sift UPDATE`) |

`pg_sift--3.1.sql` is the complete script and `pg_sift--3.0--3.1.sql` adds the same objects to a 3.0 install (`test/t/extension_lifecycle.sh`
compares the two). The on-disk formats below do not depend on the script: the library reads whatever the files hold.

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
| 3 | `SIFT_SEGFMT_PACKED`: dictionary blocks, narrower docmap rows, packed containers, position blocks | 3.1 PostgreSQL |

A 3.1 build reads 1, 2 and 3 side by side; a merge writes the current format. 3.0 refuses a segment of format 3 through the metapage minor
(section 2).

## 4. Column segments (`SiftSegEntry.format` of access methods `sift_col` and `sift_table`)

| format | meaning | written by |
|---|---|---|
| 3 | 3.0 layout (dictionaries as codec string lists) | 3.0, and 3.1 with `sift.col_format_minor = 3` (a `sift_table` group without a version section) |
| 4 | 3.1: type tags (`SIFT_COLC_TAGS`) and kinds (`SIFT_COLC_KINDS`) of overflowing values, element rollups (`COLP` directory v2), optional version section | 3.1 default |
| 5 | 3.2: a schema with wide leaves; written only for such an index | 3.2 |

`SIFT_COL_FORMAT_MIN_READ` is 3; 3.0 reads only 3 and refuses 4 with a REINDEX hint. 3.2 also reads 5 (`SIFT_COL_FORMAT_MAX_READ`), which a 3.1 library refuses. A `sift_table` follows `sift.col_format_minor` too,
except that a group with a version section (unfrozen rows) is always `SIFT_COL_FORMAT_VERS` (4). Minors 0 to 2 were never released. A `sift_col` index goes back to 3.0.x by `REINDEX` there; a `sift_table`, which cannot be reindexed, stays readable by 3.0.x (a standby on 3.0.x included) when `sift.col_format_minor = 3` (superuser) is set before its first write or before an `ALTER TABLE ... SET ACCESS METHOD` rewrite. `pg_upgrade` copies the files as they are. The chunk
flags are `SIFT_COLC_NULLS` 0x1, `SIFT_COLC_OVF` 0x2, `SIFT_COLC_TAGS` 0x4, `SIFT_COLC_KINDS` 0x8 (`src/col/sift_col_enc.h`).

## 5. Segment entry flags (`SiftSegEntry.flags`)

| flag | value | meaning |
|---|---|---|
| `SIFT_SEG_DIRTY` | 0x0001 | some liveness bit cleared |
| `SIFT_SEG_POSITIONS` | 0x0002 | written with word positions |
| `SIFT_TBL_SEG_WBUF` | 0x0100 | `sift_table` write buffer |
| `SIFT_TBL_SEG_TOMB` | 0x0200 | `sift_table` tombstone segment |
| `SIFT_SEG_VERS` | 0x0400 | column segment with unfrozen rows (`reserved` is the smallest xid); cleared when the rows are frozen |

`SiftSegEntry.reserved` is the owner's tag for an immutable `sift_col` segment and the smallest xid when `SIFT_SEG_VERS` is set.
A group written by 3.0.1 under pressure with a tag but no version section is read as a constant version vector.

## 6. Upgrade tests

| what | test |
|---|---|
| BM25 files of 2.x and 3.0 read and maintained by 3.1 | `test/t/compat_2x_fixture.sh`, `test/t/compat_30_fixture.sh` |
| every 3.1 object after `CREATE EXTENSION` and after `ALTER EXTENSION .. UPDATE` from 3.0; a `sift_col` index and a `sift_table` written by 3.0 (column minor 3) read across the update | `test/t/extension_lifecycle.sh` |
| an older library refuses what it cannot read, with the REINDEX or VACUUM FULL hint | `test/t/old_lib_refusal.sh` |
| a 3.2 index with wide leaves (minor 5) beside the older minors, merges and `REINDEX` | `test/sql/col_format5.sql` |
| standby and crash recovery of the new structures | `test/t/col_index_replica.sh`, `col_index_crash.sh`, `store_replica.sh`, `store_crash.sh`, `014_tbl_writepath.pl` |
