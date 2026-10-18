# Naming: the write buffer

Decision for 3.0. The one-pass rename is `scripts/rename-write-buffer.sh` (not run yet; the coordinator runs it with the folder restructure).

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

## Marketing: what can be said from the code and the tests

About pg_sift (verified in `test/sql/mutable_lifecycle.sql`, `test/isolation/specs/facets_writes.spec`, `docs/COL-TAM.md` section 3):
- A row is searchable when its transaction commits: `aminsert` appends it to the write buffer under WAL and every scan reads the write
  buffer with the segments (DESIGN.md (c)). There is no refresh step, and an update needs no merge to become correct.
- Visibility is the heap's MVCC; for `sift_table` it is native: row version with `xmin`, tombstone log entry per delete or update.
- An update appends one record (none for a HOT update); immutable segments are not rewritten for it, only their liveness pages
  (invariant I2). Merges still rewrite segments, to bound their number. No speed claim here: the update benchmark is measured separately.

About the others, from the official documentation (re-checked 2026-10-18):
- Elasticsearch: an update gets the document, runs the script and indexes the result, and "the document must still be reindexed"
  ([Update API](https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-update)); changes "are not visible to search
  immediately" until a refresh, every second by default on indices searched in the last 30 s
  ([near real-time search](https://www.elastic.co/docs/manage-data/data-store/near-real-time-search)); the old version is "not immediately removed but
  instead soft-deleted" and cleaned up by segment merges ([force merge](https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-indices-forcemerge));
  durability comes from a separate translog ([translog](https://www.elastic.co/docs/reference/elasticsearch/index-settings/translog)).
  Lucene buffers "added documents and deletions" in RAM until flushed
  ([IndexWriterConfig](https://lucene.apache.org/core/9_0_0/core/org/apache/lucene/index/IndexWriterConfig.html)).
- ClickHouse: `ALTER ... UPDATE` is "a heavy operation not designed for frequent use", asynchronous by default
  ([ALTER UPDATE](https://clickhouse.com/docs/sql-reference/statements/alter/update)); mutations work by "rewriting whole data parts",
  with "no atomicity" and queries that see mutated and unmutated parts
  ([ALTER](https://clickhouse.com/docs/sql-reference/statements/alter)); lightweight updates are patch parts, visible at once, add
  read overhead and are "designed to update small amounts of rows (up to about 10% of the table)"
  ([UPDATE](https://clickhouse.com/docs/sql-reference/statements/update)); lightweight deletes mask rows and merges remove them later
  ([lightweight delete](https://clickhouse.com/docs/guides/developer/lightweight-delete)); ReplacingMergeTree deduplicates "only during a
  merge" at an unknown time and needs `FINAL` at query time
  ([ReplacingMergeTree](https://clickhouse.com/docs/engines/table-engines/mergetree-family/replacingmergetree)).
- GIN: pending list searched with the main structure, "a large list of pending entries will slow searches significantly", cleanup spike
  ([GIN](https://www.postgresql.org/docs/18/gin.html)). Vespa: writes go to a memory index, flushed and merged
  ([Proton](https://docs.vespa.ai/en/content/proton.html)). Tantivy: a memory budget per writer ([Index::writer](https://docs.rs/tantivy/latest/tantivy/index/struct.Index.html)).
