# pg_sift as a SQLite extension (proof of concept)

The search engine of `../src` (codec, dictionary, BM25 scoring with block-max pruning, SiftQL, evaluator) loaded as an
SQLite virtual table. 17 source files of `../src` (12.4k lines) compile unmodified; the PostgreSQL API they use is
provided by a small shim (`shim/`, `shim.c`, `shim_regex.c`), and the storage, the write path and the virtual table are
new (`sqstore.c`, `sqbuild.c`, `sqvtab.c`, `sqstubs.c`; 4.8k lines in total with the shim).

```sql
.load build/sift
CREATE VIRTUAL TABLE docs_fts USING sift(body);                -- options: stemmer=none|english|..., positions=0|1, k1, b, ...
INSERT INTO docs_fts(rowid, body) VALUES (1, 'a quick brown fox');
SELECT rowid, rank FROM docs_fts WHERE docs_fts MATCH 'quick AND "brown fox"' ORDER BY rank LIMIT 10;   -- rank = -BM25
SELECT sift_count('docs_fts', 'fox OR dog');                   -- count without scoring
INSERT INTO docs_fts(docs_fts) VALUES('merge');                -- also 'flush', 'rebuild'
```

Queries are SiftQL (`../docs/USAGE.md`): AND/OR/NOT, phrases (positions), `pre*`, `te?t`, `/regex/`, `word~1`, `^boost`,
`{a b c}`, `field:term` with several columns. `MATCH` without `ORDER BY rank` returns every match in rowid order;
`ORDER BY rank LIMIT k [OFFSET n]` is pushed into the scan (xBestIndex: hidden rank column, orderByConsumed, LIMIT).
Regex uses the system's POSIX ERE (glibc `regcomp`), not PostgreSQL's engine: the syntax is the same for the
common subset, the semantics of back-references and some bracket classes differ.

## Run

```
./test.sh                                                  # build + 23 unit tests + FTS5 differential (synthetic corpus), ~10 s
SIFT_ARTICLES=articles.db SIFT_PG_EXPECTED=expected.tsv ./test.sh   # + real corpus differential, + comparison with pg_sift
```

Needs `cc`, `make`, python3 with `sqlite3` loadable extensions and FTS5. Nothing is installed, no root.
`bench/` has `build.py` (index build time and size), `bench.py` (latency/qps per query class), `cold.py` (first touch and RSS),
`dml.py` (insert/update/delete/rollback/merge/reopen at corpus scale), `compare_pg.py`, `gen_trace.py`, `load_base.py`.

## Design

- **Key space.** rowid r maps to the codec key (block `r>>5`, offset `(r&31)+1`), so 32 rowids share a codec block.
  Valid rowids are 0 .. 2^37-1 (137,438,953,471); others are rejected with an error. Dense rowids use the structure
  best (the benchmark's ids are dense).
- **Storage.** Segments are 8 KiB page rows in `<t>_pages(blk, data)`; a manifest `<t>_segs` lists live segments;
  `<t>_meta` holds the version/high-water mark; `<t>_content` keeps the text (the engine rechecks non-exact offsets
  and phrases against it). The pages are read lazily into an in-process cache on first use (`SELECT` of the page row;
  `sift_read_mode(1)` uses `sqlite3_blob_open` instead, measured equal, see below). Pages are never evicted.
- **Writes.** One writer (SQLite's). INSERT/UPDATE/DELETE go to an in-memory inverted buffer; commit (or xSync, or 64M
  postings, or a savepoint boundary) flushes it as one immutable segment and publishes it in the manifest in the same
  SQLite transaction, so rollback, crash and WAL readers are consistent by construction. Delete clears the liveness bit
  of the document in its segment (in place, in the same transaction). Update is delete + insert. `merge` rewrites all
  segments into one; it also runs automatically at 16 segments (8 at a time).
- **Readers.** Each statement checks the table version (`<t>_meta`) and reloads the manifest when another connection
  committed; cursors keep their segments alive until they close.
- **PostgreSQL shim.** Memory contexts over malloc (bump chunks), `ereport` as `siglongjmp` (`SQ_TRY`), StringInfo,
  List, dynahash, UTF-8 multibyte helpers, BufFile on `tmpfile()`, `Page/Buffer/Relation` emulated over SQLite rows,
  generic-WAL calls become dirty-page writes. Errors raised in the engine surface as SQLite errors.

## Correctness

- **pg_sift's evaluator** (same corpus, same 3,044 queries: the benchmark's 1000 AND / 1000 OR / 1000 phrase queries
  plus 44 hand-written ones with NOT, groups, phrases, prefix, wildcard, regex, fuzzy, boosts): the PostgreSQL
  side exported `count(*)` and the top-100 ids with `sift.score()`; `bench/compare_pg.py` runs the same SiftQL here.
  Result: match counts equal (also through `MATCH` without rank), the top-100 ids equal in order, and all
  136,323 scores bit-equal as float32 (single merged segment, `stemmer=none`, the PostgreSQL index default).
- **Brute force oracle** (`tests/test_sift.py`): an independent BM25 over random documents compares match sets and scores,
  across inserts, updates, deletes, merges and reopen.
- **FTS5 differential** (`tests/diff_fts5.py`, no stemmer on either side: sift `stemmer=none` vs FTS5 `unicode61
  remove_diacritics 2`): complete rowid sets of AND, OR, phrase, prefix and AND NOT queries. Identical on the synthetic corpus
  and on the ASCII documents of the Wikipedia corpus (500 queries per class). On all 218k articles, phrase matches
  are identical; 3 percent of the AND, 7 of the OR, 6 of the NOT and 26 of the prefix queries differ by a handful of documents, all through the tokenizer:
  sift ends tokens at a script change (`2002年` is `2002`; unicode61 keeps it as one token), folds stroke
  letters (`Đ` as `d`), and joins words across soft hyphens, modifier letters and combining marks (IPA
  `faˈdʊt͡s` is one word; unicode61 indexes `s`). The stemmed benchmark uses Snowball English (Porter2) against
  FTS5's `porter` (Porter 1), another expected source of different result sets there.

## Results

Simple English Wikipedia, 218,058 articles (about 300 MB of text), one thread pinned to an otherwise idle core, warm cache,
1000 queries per class from the benchmark's trace, `stemmer=english` for sift against `porter unicode61
remove_diacritics 2` for FTS5, top 10 by BM25.
Median latency per query; qps is queries over total time.

|  | sift | FTS5 | ratio |
|---|---|---|---|
| index, with positions | 134.7 MB (1 segment) | 125.1 MB | 1.08x larger |
| index, no positions | 48.4 MB | 35.0 MB (`detail=none`) | 1.38x larger |
| build (insert 218k docs) | 7.2 s (+0.02 merge) | 7.2 s (+0.8 optimize) | 1.0x |
| AND top 10 | 0.26 ms, 2266 qps | 9.57 ms, 86 qps | 26x qps, 36x median |
| OR top 10 | 1.93 ms, 511 qps | 182 ms, 5.6 qps | 91x qps, 94x median |
| phrase top 10 | 0.26 ms, 1642 qps | 1.19 ms, 360 qps | 4.6x qps, 4.6x median |
| count AND | 0.053 ms, 15.7k qps | 0.61 ms, 878 qps | 18x |
| count OR | 0.164 ms, 5.8k qps | 18.2 ms, 59 qps | 99x |
| count phrase | 0.091 ms, 1.3k qps | 0.63 ms, 796 qps | 1.6x |

Without positions sift answers the AND/OR trace at the same speed (2296 and 517 qps; subset of 999 alphanumeric
queries). FTS5 `detail=none` is not a useful baseline for ranked queries: AND ran at 23 qps and the OR class did not
finish in 30 minutes, so the ratios above are against FTS5 with positions. Sizes come from `bench/build.py`; timings were taken

**Kill criterion (at least about 10x FTS5 per core on ranked AND/OR at a similar size): met.** 26x and 91x at +8 percent
size with positions.
only indicative) gave 3115 qps (AND), 609 (OR), 2536 (phrase): the SQLite extension runs at 73, 84 and 65 percent of that.

Build index sizes include SQLite's own page overhead (8 KiB rows spill into overflow pages). sift also stores the text
(`<t>_content`, about 340 MB here) because the engine rechecks against it; FTS5 here is `content='articles'` (external) and
stores none. The text is not counted in the sizes above.

**Blob I/O vs SELECT.** First-touch cost of the 182 pages one cold mixed query set needs (OS cache warm): 3.1 ms with
`sqlite3_blob_open`, 3.2 to 3.5 ms with `SELECT`, i.e. indistinguishable, about 17 microseconds per page including
decoding. Both copy the page into the engine's cache (no zero-copy), which is then used for the rest of the process:
the second pass over 3000 queries is as fast as the first (2.9 s both; the cold pass costs under 3 percent).

**Write path at scale** (`bench/dml.py` on the built index): 1000 inserts, 1000 updates, 1000 deletes each commit in
2 to 7 ms for small documents; rollback leaves no trace; a full merge of the 218k-document index takes 3.7 s;
reopening is immediate (the manifest is read, pages load lazily).

## Limits (what is not done, and what is slower than it could be)

- **Memory.** Segment pages are cached for the life of the process and never evicted: 121 MiB RSS after 3000 mixed queries on
  the 135 MB index, up to the index size plus the doc map in the worst case. An index larger than RAM needs an
  eviction policy or SQLite's page cache instead. Building buffers up to 64M postings in RAM (peak RSS on the full
  corpus: 1.5 GiB, which includes the 1 GiB SQLite page cache the script configures, from `bench/build.py`).
- **Copies.** Every page is copied out of SQLite (`SELECT` or blob read) into the cache; there is no zero-copy path
  (SQLite offers none for BLOBs; `mmap` of the database file is not usable for rows that span overflow pages).
  The cost is visible only on cold start.
- **Merge** is a full rewrite through an in-memory buffer (postings of every merged segment held in RAM), not a streaming
  tiered merge, and `merge` takes all segments. Segments hold 8 KiB rows; row overhead and overflow chains add 6 percent.
- **Concurrency.** One writer (SQLite). Readers in other connections are coherent through the version check, but each
  connection loads its own copy of the pages. Not tested with more than two connections. **Not thread-safe:** the shim
  keeps process-global state (current memory context, the error frame), so engine calls from several threads at once
  would corrupt it; use one thread, or serialize the connections.
- **Content.** The text is duplicated in `<t>_content`; an external-content mode (reading the text from the user's table)
  is not implemented. The `content=` option is rejected.
- **Not implemented:** facets, parallel scans, filter pushdown of other columns, snippet/highlight (FTS5 has them),
  `phrase_index=pairs`, the PostgreSQL heap-tid machinery. Deleted documents stay counted in N, avgdl and df (so scores
  do not move) until a `merge` drops them; as in pg_sift, which counts them until vacuum.
- **SIMD.** The AVX2/AVX-512 kernels are dispatched at run time on x86-64 (`sift_isa()` reports which); the build is
  `-march=x86-64` so the extension loads everywhere. Other architectures compile the scalar paths (the SIMD files are
  guarded by `__x86_64__`); that build was not run here. Scores are bit-identical across paths (`-ffp-contract=off`).
- **Portability.** Linux/glibc, GCC or Clang. `strlcpy` is provided by the shim (glibc older than 2.38).
- **Analyzers.** Snowball stemmers and the folding tables of pg_sift, not unicode61 or ICU; see the differential above.

## Effort to a usable product

Roughly 3 to 5 person-weeks: page-cache eviction and an external-content mode (1 week), a streaming tiered merge with
background-friendly steps and an automatic policy (1 week), multi-connection and multi-thread hardening plus a fuzz/crash
test harness (1 to 2 weeks), `snippet`/`highlight`, an FTS5-style `bm25()` auxiliary function and column weights (1 week),
packaging and an ARM/macOS build with tests (a few days).
