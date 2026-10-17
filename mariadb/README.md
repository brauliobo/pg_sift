# sift for MariaDB (proof of concept)

The pg_sift search engine (BM25 top-k, count, phrase, prefix, AND/OR/NOT, boosts, SiftQL) as a MariaDB user defined
function library, over a queue table that triggers fill. No server patch, no storage engine: `libsift_udf.so` plus two SQL
files. The engine sources of `src/` are compiled unmodified; the PostgreSQL backend they call is replaced by a small shim.

```sql
CALL sift_attach('wiki', 'docs', 'id', 't.body', 32);   -- index docs.body (integer primary key id), queue + triggers
CALL sift_sync('wiki');                                  -- drain the queue into a new segment
SELECT d.id, j.s FROM JSON_TABLE(sift_topk('wiki', 'solar AND "power plant"', 10), '$[*]'
       COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j
JOIN docs d ON d.id = j.id ORDER BY j.n;
SELECT sift_count('wiki', 'solar OR wind');
```

Functions: `sift_topk(index, query, k)` (JSON `[{"id","s"}]`), `sift_count`, `sift_matches`, `sift_merge`, `sift_stats`,
`sift_tokens`, `sift_create`, `sift_drop`, `sift_ping(k)` (fake hits, to measure the call and JSON cost), and the aggregate
`sift_drain` that `sift_sync` uses. Procedures: `sift_attach`, `sift_detach`, `sift_sync`.

## Run

`mariadb/test.sh` builds, starts a private PostgreSQL (the pg_sift reference) and a private MariaDB under `/tmp`, loads the
218,058-article Simple English Wikipedia corpus, runs every check and the benchmark, and deletes both servers (`STEPS`, `KEEP`,
`WORK`, `SIFT_BENCH_DATA`: see the header of the script). It needs the PostgreSQL server headers, the MariaDB server headers,
libmariadb and no sudo. `mariadb/server.sh` is the private server on its own (`start|stop|sql|destroy`).

## Layout

| path | lines | what |
| --- | --- | --- |
| `shim.c`, `shim.h` | 1130 | memory contexts, `ereport` as `longjmp`, hash tables, strings, lists, UTF-8, temp files, buffer and WAL stubs |
| `store.c` | 680 | segment files mapped as the buffer pool, segment writer and reader, visibility filter |
| `engine.c`, `engine.h` | 1260 | indexes, manifest, event log, queries, feed, merge |
| `udf.c` | 530 | the SQL functions |
| `cli.c` | 280 | `sift_cli`: the engine without a server |
| `sql/` | 100 | function registration; queue, triggers and drain procedures |
| `bench/`, `tools/`, `tests/`, `test.sh`, `server.sh` | 900 | statement runner, comparison and benchmark scripts, 50 functional checks |

Compiled from `src/` unmodified: `sift_bitmap*`, `sift_kernels*`, `sift_analyze`, `sift_analyze_stem`, `sift_query`,
`sift_dict`, `sift_posting`, `sift_segment`, `sift_prefix`, `sift_eval`. `sift_build.c` is compiled up to its `ambuild`
marker (the in-memory inverted buffer, runs and merge), cut with `sed` at build time. Not compiled: the access method, the
planner, the mutable segment, parallel scans, filter pushdown, facets, the SQL-callable test code.

## Design

* **Segments are the pg_sift page format**, 8 KiB pages in one file per segment, written once. The engine's readers address
  pages by block number; each file is mapped `MAP_PRIVATE` into a reserved range at `slot << 19 | local`, so a "buffer" is a
  pointer into the page cache and the eval, ranking and merge code run on it without a copy. Reusing the format is what
  makes the scores and the on-disk size those of pg_sift and avoids a second codec; merge edits pages privately (copy on write).
* **Documents.** A queue row gets `seq` from an AUTO_INCREMENT column and the document id is that `seq`; the row key is the
  integer primary key. `events-N.log` records `{seq, pk, op}`; at open it is replayed into the `pk -> live docid` map and a
  sorted array of dead docids, which filter the ranker (`verify_cb`) and the count and match scans (page filter).
* **Commit.** A drain writes a new segment file, appends its events, and replaces `MANIFEST` by write-to-temp, fsync, rename:
  the only commit point. Files not named by the manifest (a crash between steps) are deleted at open. Merge is a full merge of
  every segment that drops dead documents and compacts the log; more than 16 segments merge automatically.
* **DML path.** Triggers only insert `(op, pk)` into `sift_q_<index>` inside the writer's transaction, so a rolled-back write
  leaves nothing. `sift_sync` takes `LOCK TABLES` on the queue for an instant (waits for writers in flight), reads
  `MAX(seq)`, unlocks, and feeds every queue row up to it joined to the base table: the text is that of the committed row
  now, a vanished row becomes a delete. The segment is committed, then the drained queue rows are deleted. A crash between
  the two repeats the drain, which is idempotent through the watermark in the manifest (rows at or below it are skipped).
  `GET_LOCK` keeps one drain at a time. An `EVENT` can call `sift_sync` every second.
* **Concurrency.** One mutex serializes engine calls (the engine's error state and memory context are process-global).
  Queries are sub-millisecond, so the lock is not the limit here; concurrent clients are not measured.

## Correctness

Method. Both sides index the same 218,058 articles with the same analyzer settings (the `sift` defaults). The query set is the
3,000 trace queries (1,000 AND, 1,000 OR, 1,000 phrase; the benchmark trace) plus 1,437 derived queries on the same terms:
prefix, two-prefix AND, wildcard, `?`, fuzzy, NOT, boosts, groups, nesting, phrase with a term, phrase with NOT. For each query
`tools/pg_ref.sh` runs `count(*)` and the top 10 by `sift.score(ctid)` in PostgreSQL, ties broken by id; `tools/compare.py`
checks the count, the order of the ten ids and the scores as float32, bit for bit.

Result: 4,437 queries, 4,435 equal: same count, same ten documents in the same order, scores bit identical as float32. The
two others are OR queries with a CJK word that the analyzer turns into a phrase: pg_sift answers them by rechecking the heap
text, the PoC has no heap text and returns an error (see Limits). The 26 queries that are errors in both (regular
expressions, malformed syntax) agree as errors.

Against InnoDB FULLTEXT (`tools/diff_inno.py`): the match set of each trace query, boolean mode, `innodb_ft_min_token_size=1`,
stopwords off. The analyzers differ and the engines are not expected to agree everywhere:

* InnoDB keeps `_` and an apostrophe between letters inside a word (`CONFIRMED_PLANET`, `man's` are one word each); sift
  splits them (`confirmed`, `planet`; `man`, `s`). The trace comes from the PostgreSQL parser, which splits like sift.
* InnoDB splits at the soft hyphen U+00AD (`Bun\u00adhill` indexes `hill`); sift keeps it inside the token.
* Both fold case and accents. Other scripts and punctuation classes are tokenized differently and were not analysed further.

Queries whose match sets are equal, of 1,000 per class (all 3,000 trace queries):

|  | and | or | phrase |
| --- | --- | --- | --- |
| InnoDB on the stored text | 537 (sift superset in 462) | 271 (sift superset in 722) | 556 (sift superset in 444) |
| InnoDB on a copy with `_`, `'`, U+2019 and U+00AD replaced by spaces | 997 | 978 | 1000 |

On the copy the 25 queries that still differ are off by 1 to 9 documents of tens of thousands (the Unicode cases above:
U+02BC, soft hyphens kept by sift). Without the copy sift returns more documents than InnoDB, never fewer except in 8 of the 3,000 queries. The differences that were inspected (about a dozen documents) were all
tokenization rules; the remaining 25 were not individually attributed. The InnoDB side of the count comparison is the match set of the same query in boolean mode (`+a +b`, `a b`, `"a b"`).

## Benchmark

One client, warm cache, 218,058 documents, InnoDB buffer pool 3 GB. InnoDB: `ORDER BY MATCH(body) AGAINST(...) DESC LIMIT 10`
(`+a +b` in boolean mode for AND, natural language and boolean for OR, `"a b"` boolean for phrase). sift: the UDF in a derived
table, then `JSON_TABLE` joined to `docs` (what an application writes); "UDF only" is `SELECT sift_topk(...)`. Server and client
absolute numbers move by about 30% between runs; the ratios do not.

Medians in microseconds over the 1,000 trace queries of each class, second pass (qps in parentheses). sift is from the final
run; InnoDB is from an earlier session on the same data (other CPUs), 1,000 queries, two passes except where noted:

| class | sift, joined with `docs` | sift, UDF only | InnoDB FULLTEXT | InnoDB / sift joined |
| --- | --- | --- | --- | --- |
| AND top-10 | 192 (4,171 qps) | 103 (6,514) | 93,378 boolean (9) | 486x |
| OR top-10 | 730 (1,329) | 642 (1,523) | 752,671 natural (1); 640,066 boolean (2) | 1,031x / 877x |
| phrase top-10 | 173 (3,653) | 111 (4,783) | 141,680 boolean (2) | 819x |
| count AND | 52 (15,968) |  | 96,043 (8) | 1,861x |
| count OR | 162 (5,993) |  | 622,261 (2), one pass | 3,841x |
| count phrase | 74 (1,456) |  | 143,917 (2) | 1,955x |

Check in one session on the first 100 queries of each class (sift and InnoDB on the same CPUs and load): AND 212 vs 58,878
(277x); OR 777 vs 658,866 natural / 832,953 boolean (848x / 1,072x); phrase 221 vs 149,708 (677x); count AND 52 vs 57,209
(1,100x); count OR 277 vs 894,922 (3,233x). The InnoDB phrase count of this session was not finished.

The same top-10 with `JSON_TABLE(sift_topk(...), ...)` written directly in the `FROM` clause is about twice as slow (OR,
200 queries: 1,369 us against 780 us with the derived table, 642 us for the UDF alone): MariaDB evaluates the table function's
argument twice. Use the derived table.

2 of the 1,000 OR queries fail in sift (the CJK phrase of Limits).

Build (server pinned to one CPU, so the sort threads of InnoDB do not help it): sift 4.3 s (0.3 s to create the queue and queue the
rows, 4.0 s to drain 218,058 documents into one segment); InnoDB `ADD FULLTEXT` 38 to 41 s (9x). Size: sift 135.9 MB (a
130.6 MB segment with positions plus a 5.2 MB event log); InnoDB 232.0 MB (`FTS_*.ibd`; the table `docs.ibd` is 403 MB for
both): 1.7x smaller.

UDF and JSON per call, 1,000 statements, median of the third pass:

| statement | median |
| --- | --- |
| `SELECT 1` (protocol floor) | 4.9 us |
| `SELECT sift_ping(10)` (UDF returning 10 hits as JSON) | 8.1 us |
| ... `JSON_TABLE` over it | 16.2 us |
| ... `JOIN docs ORDER BY` (the application shape) | 28.9 us |

The call and its JSON cost 3 us; with `JSON_TABLE` and the join, 24 us.

Kill criteria. sift at least 2x InnoDB FULLTEXT on ranked top-k: 277x to 1,072x depending on the class and the run, passed.
UDF with JSON round trip under 0.3 ms: 8 us, 29 us with `JSON_TABLE` and the join, passed.

## Limits

* **Not implemented**: regular expressions (SiftQL `/re/`), a phrase under OR or NOT (an error, as in pg_sift without the
  heap text; 2 of the 1,000 OR trace queries contain such a phrase: a CJK word), live (mutable) segments, filter pushdown
  and facets, partitioning, parallel scans, composite or non-integer primary keys, several indexed columns beyond an SQL
  expression (`CONCAT_WS(...)`), index options other than `dpp` (the pg_sift defaults are used), a bulk loader without the
  queue, an `ALTER TABLE` story for the base table, `mariabackup`/`mysqldump` of the index (the files are outside InnoDB).
* **Consistency lag.** Results are those of the last drain. An inserted row is invisible, an updated row still matches its
  old text, and a deleted row is still returned until the next `sift_sync`. Joining with the base table hides the deleted and
  rolled-back rows at once and gives updated rows their current text but cannot add new matches. Commit time visibility is
  impossible without a storage engine hook; the lag is the `EVENT` period.
* **Top-k over-fetch.** Documents deleted or replaced before the last drain never come back (the engine filters them). Those
  deleted after it do, and the join drops them, so a `LIMIT 10` can return fewer rows: ask for `k` plus the number of
  undrained deletes (the queue length bounds it) or accept short pages.
* **Replication.** Not tested. Row-based replication ships the queue rows (the triggers fire on the source only) but not the
  `.sift` files: the engine index is node-local. A replica needs its own `sift_attach` and drain, and the replicated
  `DELETE FROM sift_q_*` of the source's drain races the replica's own. Galera and backups have the same blind spot. The
  design needs the queue consumption to be per node (a per-node watermark instead of deleting rows), which this PoC does not do.
* **Engine concurrency.** One global lock; a drain holds it for its segment write.
* **Licensing.** The UDF library is loaded into `mariadbd` (GPLv2, with the FOSS exception for clients only) and built
  against its headers. pg_sift is under the PostgreSQL license, which is GPL compatible, so a combined binary distributed
  under GPLv2 terms is possible; whether linking a UDF counts as a derivative work is a question for a lawyer, not a statement
  of this file. The shim links only `libpgcommon`/`libpgport` (PostgreSQL license).
* **ABI and version coupling.** The UDF interface and `my_global.h` change between MariaDB majors (built and tested on 13.0.2;
  `my_bool` and the `private` headers are used), the engine pages are those of the pg_sift revision the library is built from,
  and the page format must match to reopen existing indexes. The PostgreSQL headers are a build dependency only for types
  and macros. The library must be rebuilt per MariaDB version; the on-disk files do not depend on it.

## Effort to a usable product

Roughly three to four engineer-months for a UDF-based product that is safe to run: replication and backup story (per-node
watermark instead of deleting queue rows, rebuild from the table on a replica or after a restore; 3 to 4 weeks), phrases
under OR/NOT without the heap text (1 week), several columns, non-integer keys and the analyzer options (2 weeks), a lock
per index instead of the global one and a bounded memory budget (2 weeks), packaging and a test matrix over MariaDB
versions (1 week), operations (`sift_sync` scheduling, monitoring, merge policy, crash tests under load; 3 weeks).
Visibility at commit time and `MATCH ... AGAINST` integration need a storage engine or full-text parser plugin: a separate
project of several months; the on-disk format and the engine would carry over unchanged.
