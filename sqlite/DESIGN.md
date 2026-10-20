# pg_sift for SQLite: design, correctness and measurements

Internals of the SQLite extension (`sift` for SQLite, release artifact `sift-sqlite`). Usage, installation and the headline numbers are in
[README.md](README.md). The extension compiles the portable core of `../src` (the files of `src/portable.list`: codec, dictionary, segment writer and reader, BM25 scoring
with block-max pruning, SiftQL, evaluator, merge policy) unmodified, against the host implementation in `host/` (`host.h`, `host.c`, `host_regex.c`) that
`src/core/sift_host.h` selects with `-DSIFT_HOST_SQLITE`; the storage, write path, merge driver, virtual table, snippets, facets and parallel scans are new (`sq*.c`).
The shared core and the host interface are described in [../DESIGN.md](../DESIGN.md#shared-core-and-hosts).

## Design

- **Key space.** rowid r maps to the codec key (block `r>>5`, offset `(r&31)+1`). Valid rowids are 0 .. 2^37-1.
- **Version and ABI.** `sift_version()` returns `sift-sqlite <release> (engine format 1.2, storage format 2)`; the release is `sqlite/VERSION`, the
  repository tag without the `v`. The loadable file exports `sqlite3_sift_init` (for `.load ./sift`, where the file name `sift` selects it) and
  `sqlite3_extension_init` (any other file name), checks SQLite >= 3.38 (LIMIT/OFFSET pushdown) and makes a second load into one connection a no-op.
  It uses only the loadable-extension API (`sqlite3ext.h`), no private symbols, and links only libc and libm. The storage format is recorded in
  `<t>_config`; a table of another format is refused. The engine's segment format is the one of pg_sift (`SIFT_FORMAT_MAJOR.MINOR`).
- **Storage (format 2).** The engine's 8 KiB blocks are laid end to end in `<t>_chunks(id INTEGER PRIMARY KEY, data BLOB)`,
  `chunk` bytes per row; segments start on a unit boundary (`max(chunk, 8 KiB)`), so a retired segment owns whole rows
  that are deleted with it. `<t>_segs` is the manifest, `<t>_meta` the versions and high-water mark, `<t>_config` the
  persistent settings (`format`, ...), `<t>_content` the text of the internal mode. `ALTER TABLE ... RENAME` renames the
  shadow tables (`xShadowName` declares them); `DROP TABLE` removes them.
- **Page cache.** A bounded slab of 64-byte-aligned slots (`cache_size`, default 64 MiB; at least 64 units), a hash and
  CLOCK eviction that never takes a pinned or dirty slot. A miss is one `sqlite3_blob_read` of the row straight into the
  slot (one copy). Every pointer the engine holds is a pinned buffer (`ReadBuffer`/`ReleaseBuffer` count pins; a pin left
  after a call is counted in `pin_leaks` and recovered). Edits to the liveness pages (delete) are written back with
  `sqlite3_blob_write` in the same transaction; a rollback drops every cached edit. SQLite's read lock is not held
  between statements (blob handles are closed when no cursor or transaction needs them).
- **Writes.** One writer (SQLite's). INSERT/UPDATE/DELETE go to an in-memory inverted buffer; commit (xSync), the
  posting threshold, or a savepoint boundary flushes it as one immutable segment published in the manifest in the same
  SQLite transaction, so rollback, crash and WAL readers are consistent by construction.
- **Merge.** Through sorted runs, as in the PostgreSQL host (`src/bm25/runs.c`): the live part of every input (documents, then every term with its
  postings and positions) is spilled to a run, a temporary file, and the runs are merged by a k-way merge into the output segment, in several
  passes when they do not fit the memory budget (`maintenance_work_mem`, 64 MiB here); memory is the reader buffers plus one term, never the inverted
  index. Before this unification the SQLite host merged the inputs in lockstep without a temporary file (same result, byte for byte; on the 12-segment 218k
  corpus `bench/merge.py` took 3.65 s against 3.55 s now, mean of six alternating runs, 0.98 +- 0.05 of the time, with a peak RSS of 68 MiB against 62). Tiers are `bytes / 1 MiB`;
  after a flush, `automerge` segments of a tier (at most 32 inputs) are merged. The new segment is published and the
  inputs retired in one SQLite transaction: a rollback, an error or a killed process leaves the old segments valid
  (tests inject a fault and `_exit` in the middle of the write). Merging purges deleted documents from df/N/avgdl, so
  ranked scores of a table with deletes move at the merge, not before (as in pg_sift until vacuum).
- **Threads.** The shim's allocation and error state is thread-local; the engine's two process-global mutable objects are
  made safe by the build (the scan limit of `bm25/bitmap.c` becomes `_Thread_local` through a `sed` copy in `build/gen`,
  the wide-prefix cache of `bm25/prefix.c` is behind a mutex), `src/` is not touched. Each connection has its own
  tables, cache and statements, so threads with their own connections, and one connection shared by threads in
  serialized mode, work (stress test, below). The loader sets `sqlite3_api` once under a lock.
- **Parallel scans** (`parallel=N`, default off). A ranked top-k or a count of a phrase runs on N participants: the
  caller and threads of a process-wide pool, all over the same segments, using the engine's own distribution
  (`bm25/parallel.c`: morsels of units, shared threshold, last finisher merges; result identical to the serial one). It
  needs the index resident in the page cache and no document text, and is used only when the estimated postings are
  100,000 or more; otherwise the scan is serial. See the measurements for what it buys.
- **SIMD.** AVX2/AVX-512 kernels are chosen at load from the CPU (`sift_isa()`), capped by `SIFT_SIMD=scalar|avx2|avx512`
  (environment) or `sift_simd(level)`; `make SIMD=none` links no vector code at all (the vector levels dispatch to the
  scalar tables). Other architectures build as `SIMD=none` automatically. Scores are bit-identical across paths
  (`-ffp-contract=off`).
- **Snippets, facets.** `sift_highlight`/`sift_snippet` re-analyze the stored text of the row (so they need content), mark
  the tokens that match a term, prefix, wildcard, regex or fuzzy leaf outside a NOT or belong to a phrase occurrence,
  and snippets are the window of N tokens with most matches, centred. `sift_bm25(t)` is the rank column of the row.
  `sift_facets(t, facet_table, q [, limit [, nulls]])` evaluates the query on `t`, then for every term of a keyword-analyzer
  table (one value per rowid, the same rowids) counts the matches: ordered by count then value; `nulls=1` adds a NULL row
  for matches without a value.
- **Host implementation** (`host/`). The vocabulary of `src/core/sift_host.h` over the C library: memory contexts over malloc, `ereport` as `siglongjmp`
  (`SQ_TRY`), StringInfo, List, dynahash, UTF-8 helpers, BufFile on `tmpfile()`, `Page/Buffer/Relation` over SQLite rows (`host_store.h`),
  atomics/spin locks/dsa for `bm25/parallel.c` as threads of one process. There are no fake PostgreSQL headers: the portable files include only
  `sift_host.h`, which `scripts/check-portable.sh` enforces. `sqstore.c` implements the store interface of `src/core/sift_store.h` (extent index, padding and
  extent list come from the shared inline helpers and `sift_stream.c`); `sqstubs.c` and `host/host_stubs.c` the collaborators SQLite has no use for (write buffers, visibility).
- **Shared with the PostgreSQL host, not copied.** The merge policy (`sift_merge_pick`, the tiers start at 1 MiB here and at `write_buffer_limit` there), the run merge
  (`sift_run_from_segment`, `sift_runs_write`), the deletion
  of one document (`sift_seg_delete_key`, the code of VACUUM's `sift_seg_bulkdelete`), the live-term dump of facets (`sift_seg_dump`, the code of the
  PostgreSQL merge's input), option validation of the analyzer (`sift_analyze_validate_*`).

## Correctness and tests

- **pg_sift's evaluator** (same corpus, 3,044 queries: 1000 AND, 1000 OR, 1000 phrase plus 44 hand-written with NOT,
  groups, prefix, wildcard, regex, fuzzy, boosts): counts equal, top-100 ids equal in order, all 137,558 scores bit-equal
  as float32 (single merged segment, `stemmer=none`), rechecked on this version (`bench/compare_pg.py`).
- **Brute force oracle** (`tests/test_sift.py`, 22 tests): independent BM25 over random documents, across inserts, updates,
  deletes, merges, rollbacks, savepoints and reopen.
- **Product tests** (`tests/test_features.py`, 37 tests): a cache far smaller than the index (identical answers, 0 pins
  left, evictions > 0), chunk sizes, two connections, merge steps/automerge/optimize (identical answers), rollback of a
  merge, an injected error and a killed process in the middle of a merge, rolled back deletes in old segments, content
  modes and their protocol, highlights/snippets/bm25, facets against `GROUP BY` (with deletes), commands, integrity-check
  finding damage, rename/drop, a format mismatch, `.load` in the sqlite3 shell and in `python3 -I`, parallel scans equal to
  serial ones.
- **FTS5 differential** (`tests/diff_fts5.py`, `stemmer=none`): identical rowid sets on the synthetic corpus and on the
  ASCII documents of the Wikipedia corpus; on all 218k articles phrases are identical and 3 percent of AND, 7 of OR, 6 of
  NOT and 26 of prefix queries differ by a handful of documents, all through the tokenizer (script changes, stroke letters,
  soft hyphens, combining marks). The stemmed benchmark uses Snowball English against FTS5's `porter`.
- **Stress** (`tests/stress.c`, `make tsan-run`): one writer (insert, delete, rollback, flush, merge, optimize) and N readers
  with their own connections (or sharing one in serialized mode), 6 query shapes including prefix, phrase and shared scans;
  each reader checks inside a read transaction that `count(*)` equals `sift_count`, ranks are ordered, highlights are
  well formed, `LIMIT` is honoured, then the index is integrity-checked.
  - TSan (multi-thread and serialized, 4 readers; one shared connection; parallel scans on): clean after fixing one race
    in the loader (the global `sqlite3_api`). One benign race inside the unmodified `bm25/parallel.c` is suppressed
    (`tests/tsan.supp`): the emitting participant reads the attach count without its lock, and a participant that attaches
    after the work is gone publishes an empty slot.
  - ASan+UBSan (stress with parallel scans, both test files, FTS5 differential): clean. The engine's `qsort(NULL, 0, ...)` in
    `sift_rank_begin` trips `-fsanitize=nonnull-attribute`, which the sanitizer build turns off (harmless in glibc).
  - Bugs found by them or by the new tests: the page cache kept deleted-document edits after a rollback; a blob handle kept
    SQLite's read lock after a statement (writers of other connections starved); facet values were freed too early;
    two participants shared one parallel slot.

## Measurements

Simple English Wikipedia, 218,058 articles (about 300 MB of text), the method of the table in [README.md](README.md#numbers-against-fts5) (one thread, no CPU pinning, warm cache, `nice`) on a **shared** machine, so figures are noisy (about +-10 percent, up to 1.6x between runs on the heavy OR class). Only that table was re-measured on 2026-10-17 (load average 10 to 17); the tables below were measured earlier at a load average of 8 to 12 and are not repeated. Scripts (`bench/`): `build.py` (build time, size, RSS), `bench.py` (latency/qps per class, against FTS5), `cold.py` (first touch, cache, RSS), `merge.py` (merge time/RSS/identical answers), `readpath.c` (blob_read vs column_blob), `dml.py`, `compare_pg.py` (against pg_sift's evaluator), `golden.py`, `gen_trace.py`, `load_base.py`.

### Memory and the page cache

`cold.py`, 3000 mixed top-10 queries (1002 in the 16 MiB row), RSS of the process (12 MiB before opening the index), 130 MB index:

| cache | peak RSS | qps (second pass) | misses / evictions (3000 queries) | pin leaks |
|---|---|---|---|---|
| 1 MiB (floor of 64 units) | 26 MiB | 2221 | 150k / 150k | 0 |
| 4 MiB | 29 MiB | 2383 | 40k / 40k | 0 |
| **16 MiB** | **41 MiB** (1002 queries: 41 MiB) | 2548 | 24k / 23k | 0 |
| 64 MiB (default) | 89 MiB | 2496 | 9.6k / 5.5k | 0 |
| 1 GiB | 128 MiB (the whole index) | 2785 | 6.5k / 0 | 0 |

The cost of a small cache is latency, not correctness: 1 MiB is 20 percent slower than the resident index, 16 MiB 9 percent, 64 MiB between 0 and 10 percent (two passes: 2620 and 2496 qps against 2634 and 2785).

**Chunk size and SQLite page size** (130 MB index, 3000 queries, 15 combinations built and measured): query time does not depend on
either (1.04 to 1.36 s for 3000 queries, within noise; the file read cold with `posix_fadvise(DONTNEED)` first-pass vs second-pass
differ by under 10 percent on NVMe); size and RSS do. Index size at the default 4 KiB pages: chunk 4 KiB 142.7 MB, 8 KiB 134.8,
**16 KiB 130.8**, 32 KiB 128.8, 64 KiB 127.8; RSS after the run 123, 123, 128, 136, 140 MiB (a bigger chunk loads more per miss). 16 KiB is the default: size within 2.5
percent of the best, RSS +4 percent over the smallest. With larger SQLite pages a chunk must be at most a quarter of the page, or it must
overflow: 16 KiB pages with 8 KiB chunks give 253.8 MB (one row per page, 94 percent bigger), 64 KiB pages with 32 KiB chunks the same.

**Reading from SQLite** (`bench/readpath.c`, every chunk row of the 127 MB table, MB/s of payload, 3 runs): `sqlite3_blob_read` straight
into the aligned buffer 5.0 to 5.2 GB/s; `SELECT` + `column_blob` + `memcpy` 3.5 to 3.6 GB/s; `column_blob` alone (the most a zero-copy
design could skip) 3.2 to 3.8 GB/s, because SQLite assembles an overflow chain into its own buffer first. With `PRAGMA mmap_size`
= 1 GiB: 8.3 to 8.9 GB/s for blob_read (1.7x), 5.1 to 5.3 for the other two. The extension therefore uses `blob_read` into the cache slot
(one copy, the fastest of the three); the rows are not zero-copy (a pointer SQLite returns dies with its statement and cannot back a
cache), and `PRAGMA mmap_size` is worth setting for cold starts (the reads dominate only the first pass: 11k misses cost under 0.1 s).
Cold vs warm per 3000 queries: first pass 1.14 s, second 1.08 to 1.18 s.

### Merge

`bench/merge.py` on the full corpus built as 12 segments of 2M postings (`pending_postings=2000000,automerge=0`): `optimize` takes
3.2 s with **69 MiB peak RSS at a 16 MiB cache** (179 MiB at 1 GiB), 300 of 300 top-10 and counts identical before and after.
Building with
`pending_postings=2000000` bounds the build too: 140 MiB peak RSS (no merge), 162 MiB with `automerge=8` (2 segments at the end,
10 to 12 s vs 8 s), against 687 MiB for the default one-segment build (`bench/build.py --sqlite-cache-mb 16`).

### Write path

`bench/dml.py` on the built index (1000 short documents per step): insert 2 ms, update 8 ms, delete 7 ms per commit of 1000; a rollback
leaves no trace; `optimize` after deleting 1000 articles from the 218k-document index takes 2.1 s (one segment, 207,495 matches where
208,481 were); reopening is immediate (the manifest is read, chunks load on demand).

### Content modes

Same 218k articles: internal copy of the text 362.7 MB + index 130.8 MB (the database file with the user's table 856 MB); `content='articles'`
or `content=''` store the index only, **130.8 MB, 73 percent less than internal** (493 MB file, which still holds the
user's own `articles`). Query speed is the same (AND 6.0k, OR 1.5k, phrase 4.2k qps on 300 queries; contentless AND 4.7k).

### Parallel scans

`parallel=N` on the 218k index (512 MiB cache, N cores of a shared host), `bench/bench.py` classes and a heavy-query probe:

| query | serial | 2 threads | 4 | 8 |
|---|---|---|---|---|
| OR of 6 stop words, top 10 | 0.95 ms | 0.46 ms | 0.57 | 0.57 |
| 5 frequent words OR, top 10 | 0.70 | 0.44 | 0.44 | 0.52 |
| phrase "in the united states", count | 15.8 ms | 8.0 ms | 6.9 | 5.4 |
| benchmark OR class, qps | 1409 | 1734 | 2181 | 1945 |
| benchmark count phrase class, qps | 2026 | 2899 | 2718 | 3167 |

It pays on heavy ranked OR queries (1.2 to 2x) and on counting phrases (1.4 to 3x). An AND, a phrase top 10 or a bitmap count takes 50 to 200
microseconds, less than handing the work over; those classes show no systematic change (the host's run-to-run noise on them is +-25 percent:
serial reruns of AND gave 4534 and 5782 qps). Hence it stays **off by default** and, when on, the scan is serial unless the estimated postings are at least
100,000 (counts only for phrases). More than 2 threads buys little on this host (shared, memory-bound decoding).

## What is not shared yet

Measured, not guessed: 8-token shingles of the code (3.8 percent of `sqbuild.c` appears in `bm25/build.c` before the split, 5 percent of the old `sqmerge.c` in `bm25/maint.c`);
the likeness is in the idea. The merge is now one implementation; what is left:

| SQLite | PostgreSQL | what differs | verdict |
|---|---|---|---|
| `sqbuild.c` `SqInv` (malloc buffer, key-hash `dmap`, per-term posting arrays, `sq_inv_write`) | `bm25/runs.c` `SiftInvBuf` (delta-packed slices within a budget, spill to a sorted run) | SqInv takes documents in any order, deletes a buffered document and has no budget; SiftInvBuf requires strictly ascending keys (its postings are deltas of document ordinals) and spills | keep separate for now. To share: SQLite would keep the pending documents as (key, dl, terms) like PostgreSQL's promotion (`bm25/step.c` `read_docs`), sort them at flush and feed `SiftInvBuf` (spilling to runs over the `pending_postings` budget); deletes of pending documents drop them from the list. Costs memory per pending document (a term list instead of 16-byte postings), so the default threshold must come down; decide on `bench/dml.py` and `bench/build.py` peak RSS |
| `sqaux.c` highlight/snippet | `bm25/funcs.c` `sift.highlight` | different results by design: SQLite marks a phrase occurrence as one span and snippets windows; PostgreSQL wraps every matching token | none: a shared helper would change one of the two outputs |
| `sqaux.c` facets | `bm25/facet.c` | PostgreSQL counts on bitmaps with the visibility map and the write buffers; SQLite on the matches of the evaluator (the terms come from the shared `sift_seg_dump`) | none |
