# pg_sift as a SQLite extension

The search engine of `../src` (codec, dictionary, BM25 scoring with block-max pruning, SiftQL, evaluator) loaded as an
SQLite virtual table. 18 source files of `../src` compile unmodified; the PostgreSQL API they use is provided by a small
shim (`shim/`, `shim.c`, `shim_regex.c`), and the storage, the write path, merging, the virtual table, snippets,
facets and parallel scans are new (`sq*.c`, 7.8k lines with the shim).

```sql
.load build/sift
CREATE VIRTUAL TABLE docs_fts USING sift(title, body);        -- options below
INSERT INTO docs_fts(rowid, title, body) VALUES (1, 'Foxes', 'a quick brown fox');
SELECT rowid, rank FROM docs_fts WHERE docs_fts MATCH 'quick AND "brown fox"' ORDER BY rank LIMIT 10;   -- rank = -BM25
SELECT sift_highlight(docs_fts, 1, '<b>', '</b>'), sift_snippet(docs_fts, 1, '[', ']', '...', 12)
  FROM docs_fts WHERE docs_fts MATCH 'quick fox';             -- also sift_bm25(docs_fts)
SELECT sift_count('docs_fts', 'fox OR dog');                  -- count without scoring
SELECT value, n FROM sift_facets('docs_fts', 'docs_category', 'fox');   -- matches per value of another sift table
INSERT INTO docs_fts(docs_fts) VALUES('optimize');            -- commands below
```

Queries are SiftQL (`../docs/USAGE.md`): AND/OR/NOT, phrases (positions), `pre*`, `te?t`, `/regex/`, `word~1`, `^boost`,
`{a b c}`, `field:term` with several columns. `MATCH` without `ORDER BY rank` returns every match in rowid order;
`ORDER BY rank LIMIT k [OFFSET n]` is pushed into the scan. Regex uses the system's POSIX ERE (glibc `regcomp`), not
PostgreSQL's engine: same syntax for the common subset, different back-references and some bracket classes.

## Run

```
./test.sh                  # build, 59 tests, FTS5 differential, the same with scalar kernels forced and with a vector-free build, ~40 s
SIFT_SANITIZE=1 ./test.sh  # + the tests under ASan/UBSan and the thread stress test under TSan and ASan
SIFT_ARTICLES=articles.db SIFT_PG_EXPECTED=expected.tsv ./test.sh   # + real corpus differential, + comparison with pg_sift
make [SIMD=none B=build-scalar] | asan | tsan-run | cross-aarch64     # builds (build/sift.so is the default)
```

Needs `cc`, `make`, python3 with `sqlite3` loadable extensions and FTS5, SQLite 3.38 or newer. Nothing is installed.
`bench/`: `build.py` (build time, size, RSS), `bench.py` (latency/qps per class), `cold.py` (first touch, cache, RSS),
`merge.py` (merge time/RSS/identical answers), `readpath.c` (blob_read vs column_blob), `dml.py`, `compare_pg.py`,
`golden.py`, `gen_trace.py`, `load_base.py`.

## Options, commands, functions

`CREATE VIRTUAL TABLE t USING sift(col, ..., opt=value, ...)`

| option | default | meaning |
|---|---|---|
| `stemmer`, `positions`, `k1`, `b`, `case_fold`, `accent_fold`, `prefix_index`, `analyzer` | english, 1, ... | the engine's index options (`analyzer=keyword` indexes the whole value as one term; needs `stemmer=none`) |
| `content` | internal | `content='tbl'` reads the text from the user's table (with `content_rowid='col'`, default `rowid`); `content=''` keeps none (needs `positions=1`) |
| `chunk` | 16384 | bytes per row of `<t>_chunks`, fixed at CREATE (4096 .. 65536, powers of two) |
| `cache_size` | 65536 | page cache of this connection, KiB |
| `automerge` | 8 | segments of a tier that start a merge after a flush, 0 = never (persistent) |
| `pending_postings` | 64000000 | the in-memory buffer becomes a segment above this many postings (persistent) |
| `parallel` | 0 | threads of a scan, below 2 = serial |

Commands: `INSERT INTO t(t [, rank]) VALUES('cmd' [, arg])` (the column named like the table is the command, `rank` the
argument, as in FTS5): `optimize` (all segments into one), `merge` [steps] (the tiered policy, optionally at most N merges),
`flush`, `rebuild` (re-reads the text), `delete-all`, `delete` (with `rowid`, see content modes), `integrity-check`
(rank=1: also compares the text with the index), `cache-size` N (KiB), `parallel` N, `automerge` N, `pending-postings` N,
`x-fault` (fault injection for tests). Functions: `sift_count(t, q)`, `sift_stat(t)` (segments, docs, bytes, cache
hits/misses/evictions/pinned/pin_leaks, parallel_scans), `sift_drop_cache(t)`, `sift_isa()`, `sift_simd(level)`,
`sift_version()`, `sift_highlight`, `sift_snippet`, `sift_bm25`, `sift_highlight_text`, `sift_snippet_text`
(the same on any text with the analyzer of a table), `sift_facets`.

**Content modes.** `internal`: the table keeps the text in `<t>_content`; INSERT/UPDATE/DELETE as usual. `external`
(`content='tbl'`): only the index is stored (no text copy). `content=''`: only the index, no text at all: the rows return
NULL columns, `DELETE`/`UPDATE`/`count(*)`/`rebuild` are errors, phrases need no text because positions are indexed.
Protocol for the two (like FTS5's, but the index needs only the rowid to delete):

```sql
INSERT INTO t(rowid, body) VALUES (7, 'new text');          -- index a document; its rowid must not be indexed already
INSERT INTO t(t, rowid) VALUES ('delete', 7);               -- remove it from the index (before or after changing the user's table)
-- update = delete + change the user's table + insert again
```
Inserting a rowid that is already indexed without deleting it first indexes the document twice (as FTS5's external
content does); `INSERT OR REPLACE` handles it for the internal mode.

**Version / ABI.** `sift_version()` returns `sift-sqlite 0.2 (engine format 1.0, storage format 2)`. The loadable
file exports `sqlite3_sift_init` (for `sqlite3_load_extension(db, "sift", 0)`, `.load build/sift`, the file name
`sift` is taken for `sqlite3_sift_init`) and `sqlite3_extension_init` (any other file name); it checks SQLite >= 3.38
(LIMIT/OFFSET pushdown) and that a second load into one connection is a no-op. It uses only the loadable-extension API
(`sqlite3ext.h`), no private symbols. Storage format 2 is recorded in `<t>_config`; a table of another format is refused
with an error (there is no migration from the format of the proof of concept: rebuild). The engine's segment format is
the one of pg_sift (`SIFT_FORMAT_MAJOR.MINOR`).

## Design

- **Key space.** rowid r maps to the codec key (block `r>>5`, offset `(r&31)+1`). Valid rowids are 0 .. 2^37-1.
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
- **Merge.** Streaming: the inputs are read in lockstep (their live documents, then the dictionary 256 terms at a time,
  one term's postings and positions decoded at once) and the output segment is written as it is produced; memory is the
  live documents of the inputs (10 bytes each) plus one term, never the inverted index. Tiers are `bytes / 1 MiB`;
  after a flush, `automerge` segments of a tier (at most 32 inputs) are merged. The new segment is published and the
  inputs retired in one SQLite transaction: a rollback, an error or a killed process leaves the old segments valid
  (tests inject a fault and `_exit` in the middle of the write). Merging purges deleted documents from df/N/avgdl, so
  ranked scores of a table with deletes move at the merge, not before (as in pg_sift until vacuum).
- **Threads.** The shim's allocation and error state is thread-local; the engine's two process-global mutable objects are
  made safe by the build (the scan limit of `sift_bitmap.c` becomes `_Thread_local` through a `sed` copy in `build/gen`,
  the wide-prefix cache of `sift_prefix.c` is behind a mutex), `src/` is not touched. Each connection has its own
  tables, cache and statements, so threads with their own connections, and one connection shared by threads in
  serialized mode, work (stress test, below). The loader sets `sqlite3_api` once under a lock.
- **Parallel scans** (`parallel=N`, default off). A ranked top-k or a count of a phrase runs on N participants: the
  caller and threads of a process-wide pool, all over the same segments, using the engine's own distribution
  (`sift_parallel.c`: morsels of units, shared threshold, last finisher merges; result identical to the serial one). It
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
- **PostgreSQL shim.** Memory contexts over malloc, `ereport` as `siglongjmp` (`SQ_TRY`), StringInfo, List, dynahash, UTF-8
  helpers, BufFile on `tmpfile()`, `Page/Buffer/Relation` over SQLite rows, atomics/spin locks/dsa for `sift_parallel.c`
  as threads of one process.

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
    in the loader (the global `sqlite3_api`). One benign race inside the unmodified `sift_parallel.c` is suppressed
    (`tests/tsan.supp`): the emitting participant reads the attach count without its lock, and a participant that attaches
    after the work is gone publishes an empty slot.
  - ASan+UBSan (stress with parallel scans, both test files, FTS5 differential): clean. The engine's `qsort(NULL, 0, ...)` in
    `sift_rank_begin` trips `-fsanitize=nonnull-attribute`, which the sanitizer build turns off (harmless in glibc).
  - Bugs found by them or by the new tests: the page cache kept deleted-document edits after a rollback; a blob handle kept
    SQLite's read lock after a statement (writers of other connections starved); facet values were freed too early;
    two participants shared one parallel slot.

## Results

Simple English Wikipedia, 218,058 articles (about 300 MB of text), AVX-512 CPU, one thread pinned to core 14 with `nice` on a
+-10 percent), 1000 queries per class from the benchmark's trace, `stemmer=english` for sift against `porter unicode61
remove_diacritics 2` for FTS5, top 10 by BM25, median of 3 runs, page cache 512 MiB (the whole index resident). qps is
queries over total time.

|  | before (PoC, scalar only) | now, AVX-512 | now, scalar forced | FTS5 | now vs FTS5 |
|---|---|---|---|---|---|
| index, with positions | 134.7 MB | 130.8 MB | 130.8 MB | 125.1 MB | 1.05x larger |
| index, no positions | 48.4 MB | 46.9 MB |  | 35.0 MB (`detail=none`) | 1.34x larger |
| build (218k docs) | 7.2 to 8.6 s | 7.5 to 8.3 s (10 to 15 s under contention) | same | 7.5 to 8.3 s + 0.9 to 1.1 optimize | equal |
| AND top 10 | 2266 to 2325 qps | **5779 qps**, 0.154 ms | 2129 qps, 0.307 ms | 97.7 qps, 8.85 ms | 59x |
| OR top 10 | 505 to 511 qps | **1437 qps**, 0.663 ms | 458 qps, 2.14 ms | 5.0 qps, 193 ms | 287x |
| phrase top 10 | 1642 to 1711 qps | **4040 qps**, 0.154 ms | 1551 qps, 0.320 ms | 220.7 qps, 2.24 ms | 18x |
| count AND | 18.0k qps | 16.6k qps | 15.4k qps | 500 qps | 33x |
| count OR | 5.8k qps | 10.7k qps | 5.2k qps | 61.5 qps | 175x |
| count phrase | 2.2k qps | 2.1k qps | 1.8k qps | 523 qps | 4x |

With the default 64 MiB cache (smaller than the 130 MB index, so it evicts): AND 5751, OR 1426, phrase 3881, count AND 16.7k,
count OR 10.9k, count phrase 1.9k qps. The `SIMD=none` build runs like the forced-scalar runtime (AND 2143, OR 467, phrase
1572 qps) and contains no `ymm`/`zmm` instruction.

**Honest comparison with the earlier numbers.** The proof of concept never switched its SIMD dispatch on, so its figures are
the scalar path; the forced-scalar column is the like-for-like one, and there this version is **5 to 14 percent slower**
(AND 2129 vs 2266 to 2325, OR 458 vs 505 to 511, phrase 1551 vs 1642 to 1711, count AND 15.4k vs 18.0k, count phrase 1.8k vs
2.2k; count OR is the same). Profiling the scalar run puts the cache layer (hash lookup, pins, unpin) at 2.7 percent of the
cycles, scalar decoding at 60; the rest of the gap is per-query fixed cost (call context, store enter/leave, releasing the
Count AND, which takes 50 microseconds, shows it
most; with the vector kernels on (the default), count AND is the only class that is slower than the old figure (16.6k vs 18.0k, -8 percent) and
count phrase -5 percent. The index is 3 percent smaller (130.8 vs 134.7 MB, overflow waste of the 8 KiB rows is gone). Build time is
not distinguishable from the old 7.2 to 8.6 s given the load.
pg_sift inside PostgreSQL (earlier run, one backend, load 12.8, not pinned and not rerun here): 3115 (AND), 609 (OR),
2536 (phrase) qps; this extension now runs at 1.9x, 2.4x and 1.6x of that, (different systems and loads; indicative only).

### Memory and the page cache

`cold.py`, 3000 mixed top-10 queries (1002 in the 16 MiB row), RSS of the process (12 MiB before opening the index), 130 MB index:

| cache | peak RSS | qps (second pass) | misses / evictions (3000 queries) | pin leaks |
|---|---|---|---|---|
| 1 MiB (floor of 64 units) | 26 MiB | 2221 | 150k / 150k | 0 |
| 4 MiB | 29 MiB | 2383 | 40k / 40k | 0 |
| **16 MiB** | **41 MiB** (1002 queries: 41 MiB) | 2548 | 24k / 23k | 0 |
| 64 MiB (default) | 89 MiB | 2496 | 9.6k / 5.5k | 0 |
| 1 GiB | 128 MiB (the whole index) | 2785 | 6.5k / 0 | 0 |

The cost of a small cache is latency, not correctness: 1 MiB is 20 percent slower than the resident index, 16 MiB 9 percent, 64 MiB between 0 and 10 percent (two passes: 2620 and 2496 qps against 2634 and 2785). The old
version kept every page for the life of the process (121 MiB; unbounded in general).

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
The previous version rewrote through a buffer of the whole inverted index (the build's 1.5 GiB peak). Building with
`pending_postings=2000000` bounds the build too: 140 MiB peak RSS (no merge), 162 MiB with `automerge=8` (2 segments at the end,
10 to 12 s vs 8 s), against 687 MiB for the default one-segment build (`bench/build.py --sqlite-cache-mb 16`).

### Content modes

Same 218k articles: internal copy of the text 362.7 MB + index 130.8 MB (the database file with the user's table 856 MB); `content='articles'`
or `content=''` store the index only, **130.8 MB, 73 percent less than internal** (493 MB file, which still holds the
user's own `articles`). Query speed is the same (AND 6.0k, OR 1.5k, phrase 4.2k qps on 300 queries; contentless AND 4.7k).

### Parallel scans

`parallel=N` on the 218k index, `bench/bench.py` classes and a heavy-query probe:

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
100,000 (counts only for phrases).

## Limits (what is not done, and what is slower than it could be)

- **Parallel scans** need the whole index in the cache and no document text; they help few queries, as measured.
- **aarch64**: all 24 sources pass `clang --target=aarch64-linux-gnu -fsyntax-only` against a generated LP64 stand-in for two glibc
  headers (`make cross-aarch64`), with the scalar kernels and no x86 CPU probes. Not linked and **not executed**: `qemu-aarch64` is installed
  but no aarch64 libc or libsqlite3 is, so no test ran on that architecture.
- **Scores and deletes.** Deleted documents stay in N, avgdl and df until a merge purges them (as pg_sift until vacuum); ranked
  order can change at the merge.
- **Duplicates.** The external and contentless modes trust the caller not to index a rowid twice (see the protocol).
- **Writers.** One writer (SQLite's). Another connection's commit is seen at its next statement; each connection keeps its own page cache
  (N connections cost N caches).
- **Index larger than the cache** works (evicts) but a parallel scan is then serial, and a merge needs only the live documents of its inputs in RAM
  (10 bytes each, 2.2 MB for the whole corpus).
- **Snippets** need the text (internal or external content); the match test is token based, so a regex or fuzzy match is highlighted as the whole
  token and a phrase is highlighted only where the stored occurrence is exact.
- **Not implemented:** `phrase_index=pairs` (by request), filter pushdown of other columns, the PostgreSQL heap-tid machinery. Scalar speed is
  5 to 14 percent under the proof of concept's (see above); the vector kernels more than make up for it on x86-64.
- **Portability.** Linux/glibc, GCC or Clang; SQLite >= 3.38. `strlcpy` is provided by the shim. macOS and Windows were not tried.
- **Analyzers.** Snowball stemmers and the folding tables of pg_sift, not unicode61 or ICU; see the differential above.
