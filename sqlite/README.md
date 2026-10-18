# pg_sift for SQLite

Full-text search with BM25 ranking as a SQLite loadable extension: a virtual table `sift` that answers boolean, phrase, prefix,
wildcard, regex and fuzzy queries, ranks with exact BM25 (block-max pruning for top-k), counts matches from the index,
highlights, snippets and facets. It is the search engine of [pg_sift](https://github.com/brauliobo/pg_sift) (the PostgreSQL
extension, a separate component with its own documentation) running on SQLite's storage; the two share their codec,
analyzers and query evaluator, and nothing else. The extension is called `sift` in SQLite and its release artifact is `sift-sqlite`.

- **Fast:** 10x to 242x the throughput of FTS5 on ranked top-10 queries over 218,000 Wikipedia articles ([numbers](#numbers-against-fts5)).
- **Bounded memory:** a page cache of your choosing (`cache_size`), streaming merges, no copy of the index in RAM.
- **Plain SQLite:** transactions, rollback, savepoints and WAL readers behave as for any table; one database file; no server.
- **Your table or its own:** keep the text in the virtual table, or index a table you already have (`content='tbl'`), or keep only the index.

## Install

Needs Linux (glibc 2.34 or newer, x86-64 for the released binary) and SQLite 3.38 or newer with extension loading enabled
(the `sqlite3` shell always allows `.load`; Python needs `enable_load_extension(True)`). Debian 12, Ubuntu 24.04 and RHEL 10
ship such a SQLite; RHEL 9 and Ubuntu 22.04 do not.

**From a release.** Download `sift-sqlite-<version>-linux-x86_64.tar.gz` and `SHA256SUMS` from the
[latest release](https://github.com/brauliobo/pg_sift/releases/latest):

```sh
V=2.0.0
curl -LO https://github.com/brauliobo/pg_sift/releases/download/v$V/sift-sqlite-$V-linux-x86_64.tar.gz
curl -LO https://github.com/brauliobo/pg_sift/releases/download/v$V/SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS && tar xzf sift-sqlite-$V-linux-x86_64.tar.gz
```

The archive holds `sift.so`, `LICENSE`, `NOTICE` and this file. Nothing is installed: load the file from where you keep it.

**From source.** Needs `cc`, `make` and the SQLite development files (`libsqlite3-dev`, `sqlite-devel`; 3.38 or newer):

```sh
git clone https://github.com/brauliobo/pg_sift && make -C pg_sift/sqlite      # builds pg_sift/sqlite/build/sift.so
```

The build uses the baseline x86-64 instruction set and picks AVX2 or AVX-512 kernels at load time from the CPU (`SELECT sift_isa()`);
other architectures build with scalar kernels only (compiled, but not tested on hardware). `sift_version()` reports the version.

## Quick start

```sql
.load ./sift
CREATE VIRTUAL TABLE docs USING sift(title, body);
INSERT INTO docs(rowid, title, body) VALUES
  (1, 'Venue note', 'Breakfast was great and the staff were friendly. The pool was noisy.'),
  (2, 'Cafe', 'Friendly staff, no breakfast, but a quiet pool and spa.'),
  (3, 'Resume tips', 'A good résumé is short. Cafés near the office help.'),
  (4, 'Running', 'Runners were running; she runs every day.'),
  (5, 'Noisy venue', 'The venue was noisy at night, noise everywhere. Friendly staff though.');

SELECT rowid FROM docs WHERE docs MATCH 'breakfast friendly';                   -- all terms: 1, 2
SELECT rowid, rank FROM docs WHERE docs MATCH 'pool OR spa' ORDER BY rank LIMIT 10;   -- BM25 top 10, best first
SELECT sift_count('docs', 'friendly');                                          -- 3, from the index, without scoring
SELECT sift_highlight(docs, 1, '<b>', '</b>') FROM docs WHERE docs MATCH 'friendly' ORDER BY rank LIMIT 3;
```

From Python (`python3 -I` works too): `db.enable_load_extension(True); db.load_extension('./sift')`. In C,
`sqlite3_load_extension(db, "./sift", 0, &err)`; the entry point `sqlite3_sift_init` is found from the file name `sift`, and
`sqlite3_extension_init` from any other name. A second load into one connection does nothing.

## Queries

The text after `MATCH` is SiftQL. Words are `AND`ed; operators are case sensitive:

| syntax | meaning |
|---|---|
| `a b`, `a AND b`, `a OR b`, `NOT a` | boolean; precedence `NOT` > `AND` > `OR`, parentheses group; a `NOT` needs a positive term in its group |
| `"quick brown"` | phrase (word positions are indexed unless the table has `positions=0`) |
| `pre*`, `te?t` | prefix and wildcard (`?` one character) |
| `/regex/` | regular expression over the indexed terms (the system's POSIX ERE, not PostgreSQL's engine) |
| `word~1`, `word~2` | fuzzy: edit distance 1 or 2 |
| `a^3` | boost the score of a term |
| `{a b c}` | any of a set of words or patterns |
| `title:venue` | restrict a term to a column of the table |

```sql
SELECT rowid FROM docs WHERE docs MATCH 'breakfast NOT noisy';               -- 2
SELECT rowid FROM docs WHERE docs MATCH '"friendly staff"';                  -- phrase: 2, 5
SELECT rowid FROM docs WHERE docs MATCH '(pool OR spa) AND friendly^3';      -- grouping and boost
SELECT rowid FROM docs WHERE docs MATCH 'title:venue';                       -- 1, 5
```

Text is split into words, case folded, accent folded (`résumé` matches `resume`) and, by default, stemmed with the Snowball English
stemmer (`running` matches `run`). Prefix, wildcard, regex and fuzzy patterns are matched against the *indexed* terms, so with the
default stemmer `n?isy` does not match `noisy` (indexed as `noisi`): create the table with `stemmer=none` when you search by pattern.
There is no stopword list.

## Ranking

`rank` is a hidden column holding the negated BM25 score, so `ORDER BY rank` is best first and `sift_bm25(t)` returns the same value.
`ORDER BY rank LIMIT k [OFFSET n]` is pushed into the scan, which skips the blocks that cannot reach the top k. A `MATCH` without
`ORDER BY rank` returns every match in rowid order and computes no score. `k1` and `b` (defaults 1.2 and 0.75) tune the formula per table.
Equal scores come out in rowid order.

```sql
SELECT rowid, rank FROM docs WHERE docs MATCH 'friendly' ORDER BY rank LIMIT 2 OFFSET 1;
SELECT rowid, sift_bm25(docs) FROM docs WHERE docs MATCH 'pool' ORDER BY rank;
```

Deleted documents stay in the collection statistics (document count, average length, term frequencies) until a merge purges them, so scores
of a table with deletes move when it merges; the order of the matches does not depend on anything else.

## Highlights, snippets, facets

```sql
SELECT sift_highlight(docs, 1, '<b>', '</b>')               FROM docs WHERE docs MATCH 'noisy';   -- column 1 (0-based) with the matches wrapped
SELECT sift_snippet(docs, 1, '[', ']', '…', 6)              FROM docs WHERE docs MATCH 'noisy';   -- the 6-token window with most matches
SELECT sift_highlight_text('docs', 'The quick fox', 'fox', '<b>', '</b>');                         -- any text, with the analyzer of a table
SELECT sift_snippet_text('docs', 'one two three venue four five six', 'venue', '[', ']', '…', 3);

-- matches per value of another sift table with the same rowids (one value per rowid, analyzer=keyword)
CREATE VIRTUAL TABLE docs_category USING sift(category, analyzer=keyword, stemmer=none);
INSERT INTO docs_category(rowid, category) VALUES (1, 'travel'), (2, 'travel'), (5, 'food');
SELECT value, n FROM sift_facets('docs', 'docs_category', 'friendly');       -- travel 2, food 1
```

`sift_facets(t, facet_table, query [, limit [, nulls]])` returns the values ordered by count, then value; `nulls=1` adds a NULL row for matches without a value. Highlights and snippets re-analyze the stored text of
the row, so they need the text (`internal` or `external` content mode).

## Content modes

| `content` | where the text lives | notes |
|---|---|---|
| (default) | in the virtual table (`<t>_content`) | `INSERT`, `UPDATE`, `DELETE`, `REPLACE` as for any table |
| `'tbl'` (with `content_rowid='col'`, default `rowid`) | in your own table `tbl` | only the index is stored: **73 percent less space** on the benchmark corpus |
| `''` | nowhere | only the index; columns read as NULL; `UPDATE`, `DELETE`, `count(*)` and `rebuild` are errors; needs `positions=1` (the default) |

The last two are maintained by you, as with FTS5's external content, but the index needs only the rowid to remove a document:

```sql
INSERT INTO t(rowid, body) VALUES (7, 'new text');          -- index a document; its rowid must not be indexed already
INSERT INTO t(t, rowid) VALUES ('delete', 7);               -- remove it from the index (before or after changing your table)
-- an update is a delete, the change to your table, and an insert again
```

Indexing a rowid that is already indexed, without deleting it first, indexes the document twice. Valid rowids are 0 to 2^37-1.

## Options

`CREATE VIRTUAL TABLE t USING sift(col, ..., option=value, ...)`

| option | default | meaning |
|---|---|---|
| `stemmer`, `positions`, `k1`, `b`, `case_fold`, `accent_fold`, `prefix_index`, `analyzer` | `english`, 1, 1.2, 0.75, ... | the engine's index options; `analyzer=keyword` indexes the whole value as one term (needs `stemmer=none`) |
| `content` | internal | see [Content modes](#content-modes) |
| `chunk` | 16384 | bytes per row of the shadow table `<t>_chunks`, fixed at `CREATE` (powers of two from 4096 to 65536) |
| `cache_size` | 65536 | page cache of this connection, KiB |
| `automerge` | 8 | segments of a tier that start a merge after a flush; 0 never (persistent) |
| `pending_postings` | 64000000 | the in-memory buffer becomes a segment above this many postings (persistent) |
| `parallel` | 0 | threads of a scan; below 2 the scan is serial |

## Maintenance

Commands are `INSERT`s into the hidden column named like the table (the argument goes into `rank`), as in FTS5:

```sql
INSERT INTO t(t) VALUES('optimize');                 -- merge every segment into one
INSERT INTO t(t, rank) VALUES('merge', 4);           -- the tiered policy, at most 4 merges
INSERT INTO t(t, rank) VALUES('integrity-check', 1); -- checks the index (1: also compares it with the stored text)
```

| command | effect |
|---|---|
| `optimize` | all segments into one |
| `merge` [N] | the tiered merge policy, at most N merges |
| `flush` | write the in-memory buffer as a segment now |
| `rebuild` | re-read the text and rebuild the index (not in contentless mode) |
| `delete-all` | remove every document |
| `delete` | remove one document (`rowid=`), for the `external` and contentless modes |
| `integrity-check` | verify the index; `rank=1` also compares with the text |
| `cache-size` N, `parallel` N, `automerge` N, `pending-postings` N | change a setting for this connection or, where marked persistent above, the table |

Functions: `sift_count(t, q)`, `sift_stat(t)` (segments, documents, bytes, cache hits, misses and evictions), `sift_drop_cache(t)`,
`sift_isa()`, `sift_simd(level)` (`scalar`, `avx2`, `avx512`, `auto`; the environment variable `SIFT_SIMD` caps it at load),
`sift_version()`, `sift_bm25`, `sift_highlight`, `sift_snippet`, `sift_highlight_text`, `sift_snippet_text`, `sift_facets`.

Writes go to an in-memory buffer; a commit, the posting threshold or a savepoint boundary stores it as an immutable segment in the same
SQLite transaction, so rollback, a crash and WAL readers are consistent. After a flush, segments of the same size tier are merged (`automerge`) with
memory proportional to the live documents of the inputs, not to the index. `ALTER TABLE ... RENAME` and `DROP TABLE` work and handle the
shadow tables (`<t>_chunks`, `_segs`, `_meta`, `_config`, `_content`). The storage format is recorded in `<t>_config`; a table of another
format is refused with an error.

## Numbers against FTS5

Measured on Simple English Wikipedia (218,058 articles), top 10 by BM25, warm cache, the same queries for both systems. The numbers are those of a 3.0 development build; the port code did not change in 3.1 and later, and the shared core did, so sizes and speeds may differ.

|  | sift, AVX-512 | sift, scalar forced | FTS5 | sift vs FTS5 |
|---|---|---|---|---|
| index, with positions | 130.8 MB | 130.8 MB | 125.1 MB | 1.05x larger |
| index, no positions | 46.9 MB |  | 35.0 MB (`detail=none`) | 1.34x larger |
| build (218k docs, one segment) | 12.0 s + 0.07 merge | same | 11.2 s + 1.2 optimize | equal |
| AND top 10 | **5746 qps**, 0.142 ms | 2086 qps | 64.6 qps, 13.2 ms | 89x |
| OR top 10 | **1209 qps**, 0.836 ms | 451 qps | 5.0 qps, 193 ms | 242x |
| phrase top 10 | **4019 qps**, 0.160 ms | 1563 qps | 400 qps, 1.13 ms | 10x |
| count AND | 15.3k qps | 16.2k qps | 795 qps | 19x |
| count OR | 10.2k qps | 5.3k qps | 53.9 qps | 189x |
| count phrase | 2.6k qps | 3.0k qps | 769 qps | 3.4x |

## Limits

- **One writer**, SQLite's. Another connection's commit is seen at its next statement. Each connection keeps its own page cache, so N connections
  cost N caches (`cache_size`).
- **Parallel scans** (`parallel=N`, off by default) need the whole index resident in the cache and no document text, and pay only on heavy ranked
  `OR` queries and phrase counts (1.2x to 3x); an `AND`, a phrase top 10 or a bitmap count is faster serial.
- **Scores and deletes.** Deleted documents stay in the statistics until a merge purges them, so ranked scores can change at the merge.
- **Duplicates.** The `external` and contentless modes trust you not to index a rowid twice.
- **Snippets** need the text; matching is token based, so a regex or fuzzy match is highlighted as the whole token and a phrase only where the stored
  occurrence is exact.
- **Regular expressions** use the C library's POSIX ERE (glibc `regcomp`): the same syntax as PostgreSQL's for the common subset, with
  different back-references and some bracket classes.
- **Analyzers** are the Snowball English stemmer and pg_sift's folding tables, not `unicode61` or ICU; on real text the matches differ from
  FTS5's by a few documents in 3 to 26 percent of queries, all through tokenization (script changes, stroke letters, soft hyphens, combining marks).
- **Not available:** `phrase_index=pairs`, filter pushdown of other columns (SQLite applies them after the match), SQLite older than 3.38.
- **Platforms:** the release is Linux x86-64 glibc. Other Linux architectures build but were compiled only (aarch64 passes `clang --target=aarch64-linux-gnu
  -fsyntax-only`, nothing was run); macOS and Windows were not tried.

## Tests

```sh
sqlite/test.sh                 # build, smoke test, 59 unit tests, FTS5 differential, the same with scalar kernels forced and with a vector-free build; about 40 s
SIFT_SANITIZE=1 sqlite/test.sh # also ASan/UBSan runs and the thread stress test under TSan and ASan; a few minutes
SIFT_ARTICLES=articles.db SIFT_PG_EXPECTED=expected.tsv sqlite/test.sh   # also the 218k-article differential and the comparison with pg_sift
```

Needs `cc`, `make` and python3 with loadable `sqlite3` extensions and FTS5. The real-corpus differential and the comparison with the PostgreSQL
extension need data and a PostgreSQL run that are not in the repository; they are skipped, with a message, when their variables are unset.
CI runs the first two on every change. `make -C sqlite asan | tsan-run | cross-aarch64 | isa-check` build the individual variants.
`scripts/package-sqlite.sh` builds and tests the release tarball in containers.

## License

[PostgreSQL License](https://github.com/brauliobo/pg_sift/blob/main/LICENSE) like the rest of the repository; third-party notices are in
[NOTICE](https://github.com/brauliobo/pg_sift/blob/main/NOTICE). Report crashes, wrong results and index corruption privately as described in
[SECURITY.md](https://github.com/brauliobo/pg_sift/blob/main/SECURITY.md).
