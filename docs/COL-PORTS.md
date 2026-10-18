# The column engine on other hosts (SQLite, MariaDB)

Question: "SQLite and MariaDB do not compile the column engine". Answer in one line: the engine's encoding, codec, kernels, shredder
and segment writer/reader **do compile and run** on both host shims (checked below); what is missing is a host feature that uses them.
Verdict and phases: section 5. Status: research, 2026-10; nothing here changes what 3.0 ships.

## 1. What is host independent

`src/portable-col.list` names the files that reach their host only through `src/sift_host.h` (`scripts/check-portable.sh` enforces
the includes). Checked by building them, not only by reading includes (`make -C sqlite col-smoke`, tests/col_smoke.c):

| Part | Files | Lines | On the SQLite host |
|---|---|---|---|
| chunk and unit encoding, zones, dictionaries, batches | `sift_col_enc.c` | 2.1k | builds, round trip passes |
| chunk codec (cascades, FOR/delta/RLE/dict/ALP), LZ4/zstd layer | `sift_colcodec*.c`, `sift_colz.c` | 3.6k | builds, round trip passes |
| segment writer/reader of units over the page store | `sift_col_seg.c` | 0.9k | builds, linked (not yet exercised) |
| column kernels (filters, zone tests, unpack) | `sift_col_kern*.c` | 1.7k | builds |
| shredder core (JSON paths into leaves, no JSON parser) | `sift_shred*.c` | 1.6k | builds |
| aggregation kernels (sum/min/max/count, group-by) | `sift_aggkern*.c` | 1.2k | builds |
| runtime SIMD (AVX2/AVX-512 tables, `sift_cpu.c`) | `*_avx2.c`, `*_avx512.c` | | baseline objects have no POPCNT/vector instruction (same rule as `make isa-check`); `SIMD=none` builds and passes |

The MariaDB host (same C-library shim) syntax-checks all of them with its flags. aarch64: `clang --target=aarch64` accepts them with
no vector files (the dispatchers are `__x86_64__`-guarded); nothing was linked or run there.

PostgreSQL-bound, and staying so: the page/buffer store behind `sift_store.h` (PG: `sift_store.c`, buffer manager, generic WAL; SQLite
already implements the same interface in `sqlite/sqstore.c` over a BLOB table), MVCC (visibility map, `Snapshot`, heap rechecks:
`sift_col_scan.c`, the `SiftColVisibility` callbacks), mutable lanes and WAL (`sift_col_mut.c`), index AM/build/maintenance/verify
(`sift_col_am.c`, `_build.c`, `_maint.c`, `_verify.c`, 2.5k lines), the jsonb driver (`sift_shred_pg.c`), planner and custom scan
(`sift_agg*.c`, 12k lines, `docs/COL-PLAN.md`), the table AM (`sift_table*.c`, 5.6k lines, `docs/COL-TAM.md`). The MVCC hook is one
interface (docs/COL.md): the segment's liveness bits and an atomically published manifest give a single-writer host a snapshot by
construction (how the BM25 SQLite host already works).

## 2. What a useful feature is on each host

Industry shapes, and what they teach:

| System | Shape | Lesson for sift |
|---|---|---|
| SQLite FTS5, R-Tree, sqlite-vec `vec0` | virtual table; state in shadow tables (`_data` BLOB rows, `_idx`, `_config`...) declared by `xShadowName`, so `DROP`/`RENAME`/`VACUUM`/backup/`PRAGMA defensive` treat them right | the standard SQLite design; `sqlite/` already does it for BM25 (`_chunks`, `_segs`, `_meta`, `_config`) |
| SQLite Parquet vtab, DuckDB `sqlite` scanner | read-only view of a foreign columnar file | interchange only: no write path and no zone maps of its own to maintain |
| DuckDB / chDB embedded in a host (pg_duckdb, pg_mooncake) | a whole second engine; the host feeds it | not applicable: sift is the engine; it would only be the host-side store |
| MariaDB ColumnStore, S3 engine, MyRocks | storage engines (C++ `handler`, GPLv2, in-tree or `INSTALL SONAME`) | the only route to real table semantics and to aggregate pushdown (`group_by_handler`/`select_handler`, as ColumnStore and Spider use) |
| MariaDB UDF/aggregate over an InnoDB table | row-at-a-time calls (about 7 us each, mariadb/README.md) | a vectorised engine behind a row API loses the point; only a *set-returning* call (JSON like `sift_topk`) works |

**SQLite: a `sift_col` virtual table.** `CREATE VIRTUAL TABLE t USING sift_col(a INT, b TEXT, ts INT)`; shadow tables exactly as the BM25
vtab (`t_chunks` BLOB rows holding segments, `t_segs` manifest with per-segment min/max, `t_meta`, `t_config`; `xShadowName` declared).
Writes buffer in memory, flush as one immutable segment of units at commit (`xSync`), published in the manifest in the same
transaction; deletes set liveness bits; merge reuses `sift_merge.c`. Reads: `xBestIndex` turns `=, <, <=, >, >=, IN, IS NULL, LIKE
'p%'` and LIMIT into `SiftColPred`s, zone maps and dictionaries prune units, kernels build the selection vector, `xColumn` yields
values. Honest limit: SQLite calls `xColumn` per value and cannot push an aggregate into a vtab, so the win is **filtered scans** and
a **table-valued function** `sift_col_agg(t, 'sum(b) group by a where ...')` over `sift_aggkern` (eponymous vtab, one pass). Row
key: rowid maps to `blk = r >> 8, off = (r & 255) + 1` (256 rows x 256 pages = 64k-row units, near the engine's 74k design point;
the BM25 mapping `r >> 5` would make 8k-row units).

**MariaDB: a storage engine, not a UDF.** A UDF library (what `mariadb/` ships) can serve `sift_col_agg(...)` as a JSON-returning
call over side segment files fed by a trigger queue (2 weeks, same model as BM25, no new tables); it cannot own a column table, a
transaction or the optimizer. A real `ENGINE=SIFTCOL` (handler class in C++, `write_row`, `rnd_*`/`index_*`, condition and group-by
pushdown, append-only segments published by manifest swap, table-level locking, no multi-statement rollback at first, as ColumnStore
and S3) is the standard shape and the only one that matches "highest industry standard".

## 3. Licensing and build

- Core: PostgreSQL License (permissive, GPL-compatible). MariaDB server and `handler` API are GPLv2 with no plugin exception for the
  server (the FLOSS exception covers client libraries); a loaded plugin is a combined work, so the **plugin binary** ships under GPLv2
  terms (source offered) while `src/` stays PostgreSQL-licensed. The existing UDF library already makes this assessment
  (mariadb/README.md); an engine is a stronger link. Needs a lawyer's sign-off before a release, not before a prototype.
- Plugin ABI is per server series (`MYSQL_VERSION_ID` checked at compile time, `tests/version.sh`); an engine adds a CI matrix
  (10.6, 10.11, 11.4, 11.8) of C++ builds and mtr runs; the UDF library needs only headers.
- Compression: LZ4 library is BSD-2, zstd is BSD-3/GPLv2 (take BSD). `sift_colz.c` compiles to "not available" with neither
  (chunks then use the lightweight cascades only; a chunk written elsewhere with a wrapper fails to open with a clear error). A
  single-file SQLite extension must not depend on system `.so`s: vendor `lz4.c`/`lz4hc.c` and zstd's single-file amalgamation
  behind `-DSIFT_HAVE_*`; no static libs are installed on the build host. Without them ratios drop but nothing breaks.
- SQLite loadable extension limits: only `sqlite3ext.h` API (no pager, no mmap of the DB file, so one memcpy per cache miss through
  `sqlite3_blob_read`, as `sqstore.c`); no intra-query parallelism but an own thread pool works (done for BM25); many distributions
  and mobile builds set `SQLITE_OMIT_LOAD_EXTENSION`, so the extension must also link statically (`sqlite3_auto_extension`, done for the
  stress test); WASM has no AVX (`SIMD=none`). Object size: the column list adds about 170 KiB of x86 code including the vector kernels.

## 4. Exact blockers found by compiling with the portable core's flags

Fixed on `feat/v3-ports` (pure refactors, PostgreSQL behaviour unchanged):

1. `sift_col.h` declared PostgreSQL-only items (`Node`, `PG_FUNCTION_ARGS` entry points, reloptions struct, relcache accessors,
   shredder adapter on `Relation`): moved to the new `sift_col_pg.h`; includes added to `sift_col_am.h`, `sift_col_mut.c`, `sift_aggcol.c`.
2. `sqlite/host/host.h` lacked `PG_INT16_MAX/MIN`, `PG_INT64_MAX/MIN` (used by `sift_col_enc.c`, `sift_shred.c`, `sift_shred_kern_avx2.c`).
3. `sift_colz.h` promised `sqlite/host/colz.c`, which did not exist: four unresolved `sift_colz_*` symbols at link. `sift_colz.c`
   includes only `sift_host.h`, so it joined the list; `check-portable.sh` allows the optional `lz4.h`/`lz4hc.h`/`zstd.h`.
4. `sift_col_seg.c` was PostgreSQL-free but unlisted: listed.
5. The Makefile had no way to build the list: `make col-smoke` (also `SIMD=none`, which drops the vector files and aliases their tables).

Open (not fixed, each a work item of the phases below): no host owns a `sift_colz` build option (vendored LZ4/zstd); `sift_col_seg.c` is
linked but its writer/reader path is only tested on PostgreSQL; the unit key space is heap TIDs (`MaxHeapTuplesPerPage` = 291 in the
host shim, 256-page superblocks) and each host must choose its rowid mapping; `sift_shred_pg.c` has no counterpart (SQLite needs a
driver over `json_each`/JSONB text, MariaDB over its JSON text); `SiftColVisibility` has no non-PG implementation (a manifest-only one
is trivial); the aggregate planner/executor (`sift_agg*.c`) and partial-state types
are PG; `make isa-check` and the sanitizer targets do not cover the column objects yet.

## 5. Recommendation

| Phase | Work | Size | Risk |
|---|---|---|---|
| 0 (done here) | portable subset compiles on both shims; header split; `col-smoke` (ASan/UBSan clean, `SIMD=none`) | 0.5 kLOC | none |
| 1 | CI job for `col-smoke` (x86, `SIMD=none`, aarch64 syntax), `isa-check` over the list, vendored LZ4/zstd option, a `sift_col_seg` round trip in the smoke test | 1 week | low |
| 2 | SQLite `sift_col` vtab: write/flush/delete/merge, `xBestIndex` pushdown, `sift_col_agg` TVF, differential test against plain tables (the repo's difftest pattern) | 3.5-4.5 kLOC, 5-7 weeks | medium: per-value `xColumn` cost, rowid reuse after delete, update semantics |
| 3a | MariaDB UDF `sift_col_agg` over side segments | 1.5 kLOC, 2 weeks | low; limited value |
| 3b | MariaDB `ENGINE=SIFTCOL` with group-by pushdown | 3 kLOC C++, 8-12 weeks, ongoing per-series upkeep | high: handler API drift, crash recovery, GPL packaging |

**Verdict for 3.0: do not ship a column feature on SQLite or MariaDB in 3.0.** 3.0 is the PostgreSQL column track and still moves
(table AM, aggregates). Ship what is already done (phase 0: the portable subset stays buildable and tested, so the engine cannot
drift into PostgreSQL-only code) and reword the gap as "the column engine is portable and checked on the SQLite and MariaDB shims;
no SQL feature on those hosts yet". Take phase 1 in 3.0.x, phase 2 as an opt-in preview in 3.1 (it is the only host where the
shadow-table pattern is established and the infrastructure, `sqstore.c`, merge, thread pool, exists), and decide on 3b only after
phase 2 shows demand and legal has answered; 3a is optional and only worth it if MariaDB users ask before that.

Decisions needed: (a) accept the `sift_col_pg.h` split and its three one-line includes in files other work touches; (b) rowid mapping
for SQLite (`>> 8`, above) and whether its segments may differ in unit size from PostgreSQL's; (c) vendoring LZ4/zstd in the SQLite
release artifact; (d) GPL sign-off for any MariaDB engine; (e) none: the leaf type in the format is the host-independent tag `SiftColType` (done, column format version 1), PostgreSQL OIDs are mapped at the PG boundary only.
