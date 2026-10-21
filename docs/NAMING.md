# Naming: the write buffer

The decision of 3.0, kept for the glossary and the compatibility policy. The one-pass rename is `scripts/rename-write-buffer.sh`: run once on the 3.0 branch before the `src/` restructure (`scripts/restructure-src.sh`); run it again, then `--check`, on a branch that was open during it.

## Decision: one term, "write buffer"

New rows are written first to a small writable area of the index or table. Queries read it together with the segments. It is sealed
when it reaches a size limit, and `sift.promote`, a merge or VACUUM turns the sealed part into a segment. Until now this was the
*mutable lane / mutable segment* (BM25 index, `sift_col`), the *delta segment* (`sift_table`), the *write lane* (README) and the
*write segment* (GUC text): four names, and "mutable segment" next to "immutable segment" reads as a kind of segment.

One term, not two, because the code says it is one structure:
- Same page family and code: `SiftMutHeaderData` pages in lane chains, `sift_lane.c` (create, seal, tail lock, grow, walk, VACUUM pass)
  serves the BM25 doc records, the `sift_col` row records and the `sift_table` row versions (`sift_lane_flag_sealed_pages`).
- Same lifecycle and knobs: one open buffer, sealed at a size limit, promoted by the maintenance worker, VACUUM or `sift.promote`; same
  directory kinds (`SIFT_SEG_MUTABLE_ACTIVE/_SEALED`); same limit in kB (`mutable_limit`, `sift.table_delta_limit`).
- Only the record differs: postings records (BM25), column rows (`sift_col`), row versions with `xmin` (`sift_table`). That belongs in
  the glossary entry, not in the name. The `sift_table` tombstone log is not a buffer (it outlives promotion) and keeps its name.

"Write buffer" over the alternatives: it says what it is for (where recent writes live) and fits the sentence users should read:
rows are searchable the moment the transaction commits; the write buffer is only where recent rows live until a segment is built.
It is in the same family as Lucene's RAM buffer, Elasticsearch's indexing buffer and the LSM memtable, but it is pages of the relation,
WAL-logged and replicated, not memory (docs say so once, like InnoDB's change buffer).

## Glossary

| Term | Meaning | Where it appears |
|---|---|---|
| segment | immutable: pages never change but the liveness pages; built by promotion, build or merge | `sift.segments`, docs, `immutable` kind |
| write buffer | where new rows go first; read with the segments by every query; WAL-logged pages of the relation | reloption, GUC, docs, messages |
| open write buffer | the one that takes appends (at most one per relation) | `buffer` kind, `SIFT_SEG_WBUF_OPEN` |
| sealed write buffer | closed to appends, still read, waits for promotion | `sealed buffer` kind, `SIFT_SEG_WBUF_SEALED` |
| lane | one of the 8 parallel append chains of an open write buffer (`SIFT_LANES`); internal | `sift_lane.c`, verify messages |
| seal | close the open write buffer, open a fresh one (one directory record) | docs, `sift_*_seal` |
| promote | turn a sealed write buffer into a segment; for `sift_table` the row format to columns conversion | `sift.promote`, `sift.col_promote`, `sift.table_inline_promote` |
| merge | segments into one segment | `sift.merge` |
| tombstone log | `sift_table` log of deletes, updates, locks | `COL-TAM.md`, `kind = 'tombstones'` |

Retired: mutable segment, mutable lane, active mutable segment, write lane, write segment, delta segment, delta limit. `delta` stays
only for the column encoding and the generic WAL page delta.
Identifier prefixes: `wbuf` (`sift_wbuf_*`, `sift_cwbuf_*`, `SiftWbuf*`, `SIFT_PT_WBUF*`). On-disk values (page type numbers, magics) do not change.

## Names

| Old | New |
|---|---|
| reloption `mutable_limit` (sift, shipped 2.0.0; sift_col) | `write_buffer_limit` (kB, 64-1048576, default 4096); sift_col is new in 3.0: new name only |
| GUC `sift.table_delta_limit` / `sift.table_inline_seal` (unreleased) | `sift.table_write_buffer_limit` / `sift.table_inline_promote` |
| `sift.col_index_info` column `mutable_segments` (unreleased) | `buffers` (write buffer entries, open and sealed) |
| `kind` of `sift.segments`, `sift.col_segments`: `mutable`, `sealed` | `buffer`, `sealed buffer` (`immutable` unchanged) |
| `kind` of `sift.table_segments`: `delta`, `sealed delta` (unreleased) | `buffer`, `sealed buffer` (`columns`, `tombstones` unchanged) |
| files `sift_mutable.[ch]`, `sift_col_mut*.[ch]` | `sift_wbuf.[ch]`, `sift_col_wbuf*.[ch]`; `sift_lane.[ch]` stays |
| tests `mutable_lifecycle`, `col_mut`, `tbl_seal` | `wbuf_lifecycle`, `col_wbuf`, `tbl_promote` |

Rejected:
- **delta** (SAP HANA, Vertica WOS, C-Store, Citus): the convention for column stores, but `delta` is already a column encoding and a
  generic WAL term here, it reads as "difference to the main store", and it does not fit the BM25 index.
- **pending list** (GIN): means "not yet in the main structure" and carries GIN's reputation (slow searches, cleanup spikes, GIN docs);
  ours is bounded page chains read by SIMD scan, not a list.
- **mutable ...**: reads as a segment kind; **memtable / RAM buffer**: wrong, the data is not in memory; **staging**: suggests uncommitted.

## Compatibility policy

- `mutable_limit` stays accepted forever on `USING sift` as a deprecated alias of `write_buffer_limit` (same bounds, lock mode, `ALTER
  INDEX .. SET / RESET`), so dumps and scripts of 1.x and 2.0 restore. Registered as a second option; `sift_amoptions` uses it when
  `write_buffer_limit` is at its default; the new name wins if both are set. `pg_class.reloptions` keeps what the user typed. No
  warning (a restore must stay quiet). Documented in one README line; never in new examples. Tested by `write_buffer_alias`.
- Scripts that must run against old builds (`scripts/perf-gate-maint.sh`) keep `mutable_limit`: every release accepts it.
- `sift.segments` `kind` values change: breaking for a script that matches `'mutable'` or `'sealed'`, allowed in the 3.0 major. Same for
  the text of corruption and verify messages ("Mutable block %u ..." becomes "Write buffer block %u ..."), the bound error of
  `mutable_limit` out of range (still names `mutable_limit` when that spelling was used), and the memory-context names.
- Changelog list for 3.0 (against 2.0.0), complete: the alias note; `kind` `mutable`/`sealed` -> `buffer`/`sealed buffer`; the message
  texts. Nothing else released changes. Unreleased in 2.0.0, so only "new": `sift_col`, the `sift_table` GUCs, `table_segments` kinds,
  `col_index_info.buffers`.
- No EXPLAIN output names the buffer (checked: `sift_cscan.c`, `sift_agg.c` print nothing of the kind), so nothing changes there.
- Ports: SQLite and MariaDB have no write buffer (their `stubs` say so); their in-memory insert buffer keeps "in-memory buffer".

## Source files

A file of `src/<area>/` has no `sift_` prefix and no prefix of its folder: `bm25/query.c`, `col/enc.c`, `table/seg.c`. Headers follow the same rule. The
C identifiers keep their `sift_` names; only files are renamed. A source includes another by its path from `src/` (`#include "bm25/query.h"`, `-I src`),
so the same short name in two folders is fine. A file past about a thousand lines is split by responsibility into files of its folder, and what the
pieces share goes into one `NAME_int.h` next to them. `scripts/restructure-src.sh --check` enforces the layout.
