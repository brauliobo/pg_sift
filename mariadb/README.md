# sift for MariaDB

BM25 full-text search for MariaDB tables: ranked top-k, exact counts, phrases, prefix / wildcard / fuzzy / regular
expression terms, AND / OR / NOT, boosts, fields, filters and facets, as a user defined function library
(`libsift_udf.so`) and two SQL files. It is the [pg_sift](../README.md) engine (the sources of `src/`, compiled
unmodified) behind a small PostgreSQL-backend shim, so the page format, the analyzer, the ranking and the query language are
those of pg_sift; for the same text and the same options the counts, the top-10 order and the scores are bit identical.

It adds no storage engine and patches nothing: a trigger-fed queue table keeps the index in step with an InnoDB table, the
index lives in plain files next to the data directory, and a query is a function call. Use it when `MATCH ... AGAINST`
is too slow or too limited (InnoDB FULLTEXT takes 0.06 to 1.2 s for a ranked top-10 on 218,058 articles, sift 0.2 to 1.3 ms), you
want BM25 and phrases that behave the same on every engine of the product, or you want filters and facets inside the ranked
scan. Do not use it if you need `MATCH ... AGAINST` syntax (it cannot be added to a UDF, see "MATCH ... AGAINST") or a
MySQL server (MariaDB only).

Contents: [install](#install) - [quick start](#quick-start) - [queries](#queries) - [keeping the index current](#keeping-the-index-current) -
[visibility](#what-a-query-sees) - [filters and facets](#filters-and-facets) - [options](#index-options) - [keys](#keys) -
[replication and backup](#replication-and-backup) - [operations](#operations) - [limits](#limits) - [MATCH AGAINST](#match--against) -
[versions](#versions-abi-and-release-artifacts) - [numbers](#numbers-against-innodb-fulltext) - [tests](#tests) - [layout](#layout-and-shared-core)

## Install

Needs a MariaDB server 10.5 to 13.x (built and tested on 13.0.2), its development headers (`/usr/include/mysql/server`),
the PostgreSQL server headers and the static `libpgcommon_shlib.a` / `libpgport_shlib.a` of the same PostgreSQL (types, macros,
UTF-8 tables and `qsort`; no PostgreSQL code runs), a C compiler and `make` (or CMake). `libmariadb` is only needed for the
test tools.

```sh
nice make -C mariadb -j4 OUT=/tmp/sift-build            # or: cmake -S mariadb -B /tmp/sift-build && nice cmake --build /tmp/sift-build -j4
mariadb -e "SHOW VARIABLES LIKE 'plugin_dir'"           # where the server loads UDF libraries from
cp /tmp/sift-build/libsift_udf.so /usr/lib/mysql/plugin/   # that directory (needs write access to it)
mariadb mydb < mariadb/sql/functions.sql                # registers the functions in the server (mysql.func), once per server
mariadb mydb < mariadb/sql/sift.sql                     # the procedures and the index table, once per database that has indexed tables
```

`make` finds the MariaDB headers with `mariadb_config` (or `MDB_INC=<dir with mysql_version.h>`). The library exports only the
`sift_*` functions. Indexes are directories under `<datadir>/.sift/<index name>` (`SIFT_DIR` in the server's environment
moves them); index names are server wide, 1 to 63 letters, digits or underscores. Uninstall: `CALL sift_detach(...)` for each
index, `DROP FUNCTION sift_...` (the list is `sql/functions.sql`), remove the library.

## Quick start

Every statement below was run against a private MariaDB 13.0.2; the output is what it printed.

```sql
-- setup (once): the functions of the server, the procedures of the database
SOURCE mariadb/sql/functions.sql;   -- once per server (see Install)
SOURCE mariadb/sql/sift.sql;        -- once per database
```

```sql
CREATE TABLE articles (id BIGINT PRIMARY KEY, title VARCHAR(200), body MEDIUMTEXT, category INT, likes INT);
INSERT INTO articles VALUES
  (1, 'Solar power', 'Solar power plants turn sunlight into electricity.', 1, 10),
  (2, 'Wind farms', 'A wind farm is a power plant made of many turbines.', 1, 5),
  (3, 'Coal', 'A coal power plant burns coal to boil water.', 2, 1),
  (4, 'Pasta', 'How to cook pasta in salted water.', 3, 7);
```

```sql
-- index title and body of articles; the key is the primary key id; 32 documents per page
CALL sift_attach('news', 'articles', 'id', "CONCAT_WS(' ', t.title, t.body)", 32);
```

```sql
-- ranked top 10: the UDF in a derived table, JSON_TABLE over it, a join with the table
SELECT a.id, a.title, ROUND(j.s, 3) AS score
FROM (SELECT sift_topk('news', 'solar OR "power plant"', 10) AS r) x,
     JSON_TABLE(x.r, '$[*]' COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j
JOIN articles a ON a.id = j.id ORDER BY j.n;
```
```text
+----+-------------+-------+
| id | title       | score |
+----+-------------+-------+
|  1 | Solar power | 1.703 |
|  3 | Coal        | 1.050 |
|  2 | Wind farms  | 0.935 |
+----+-------------+-------+
```

```sql
SELECT sift_count('news', 'power AND plant') AS plants, sift_matches('news', 'water') AS water;
```
```text
+--------+-------+
| plants | water |
+--------+-------+
|      2 | [3,4] |
+--------+-------+
```

```sql
-- changes are visible at once, before any drain
INSERT INTO articles VALUES (5, 'Hydro power', 'Hydroelectric power plants use water.', 1, 3);
UPDATE articles SET body = 'Pasta needs no power plant.' WHERE id = 4;
DELETE FROM articles WHERE id = 3;
SELECT sift_matches('news', '"power plant"') AS power_plant, sift_matches('news', 'water') AS water;
```
```text
+-------------+-------+
| power_plant | water |
+-------------+-------+
| [2,4]       | [5]   |
+-------------+-------+
```

```sql
SELECT sift_stats('news') AS stats;
```
```text
+------------------------------------------------------------------------------------------------------------------------+
| stats                                                                                                                  |
+------------------------------------------------------------------------------------------------------------------------+
| {"segments":0,"docs":0,"dead":0,"live":0,"drained":0,"pending":7,"bytes":0,"dpp":32,"docbase":0,"merges":0,"format":2} |
+------------------------------------------------------------------------------------------------------------------------+
```

```sql
-- write the queue into a segment (an EVENT can do this every second: CALL sift_schedule('news', 1))
CALL sift_sync('news');
SELECT JSON_VALUE(sift_stats('news'), '$.pending') AS pending, sift_count('news', '"power plant"') AS power_plant;
```
```text
+--------------+
| rows_applied |
+--------------+
|            7 |
+--------------+
+---------+-------------+
| pending | power_plant |
+---------+-------------+
| 0       |           2 |
+---------+-------------+
```

```sql
-- a filter inside the ranked scan, and facets
CALL sift_search('news', 'power', 10, 't.category = 1');
```
```text
+----+-------------+-----------------------------------------------------+----------+-------+--------------+
| id | title       | body                                                | category | likes | score        |
+----+-------------+-----------------------------------------------------+----------+-------+--------------+
|  5 | Hydro power | Hydroelectric power plants use water.               |        1 |     3 |  0.124776244 |
|  1 | Solar power | Solar power plants turn sunlight into electricity.  |        1 |    10 |  0.116445482 |
|  2 | Wind farms  | A wind farm is a power plant made of many turbines. |        1 |     5 | 0.0702005848 |
+----+-------------+-----------------------------------------------------+----------+-------+--------------+
```

```sql
CALL sift_facets('news', 'power', 't.category', NULL);
```
```text
+-------+---+
| value | n |
+-------+---+
|     1 | 3 |
|     3 | 1 |
+-------+---+
```

```sql
SELECT sift_topk('news', 'power', 10, (SELECT sift_idset(id) FROM articles WHERE likes > 4)) AS likes_over_4;
```
```text
+--------------------------------------------------------------------------------+
| likes_over_4                                                                   |
+--------------------------------------------------------------------------------+
| [{"id":1,"s":0.116445482},{"id":4,"s":0.0977388024},{"id":2,"s":0.0702005848}] |
+--------------------------------------------------------------------------------+
```

```sql
SELECT sift_tokens('news', 'Solar-powered PLANTS') AS tokens;
```
```text
+----------------------+
| tokens               |
+----------------------+
| solar powered plants |
+----------------------+
```

```sql
SELECT JSON_VALUE(sift_merge('news', 'full'), '$.segments') AS segments;
```
```text
+----------+
| segments |
+----------+
| 1        |
+----------+
```


## Queries

The ranked top-k has one documented shape: the UDF in a derived table (so MariaDB evaluates it once), `JSON_TABLE` over its
result, a join with the table. Written directly in the `FROM` clause of `JSON_TABLE` the argument is evaluated twice.

```sql
SELECT a.*, j.s AS score
FROM (SELECT sift_topk('news', 'solar AND "power plant"', 10) AS r) x,
     JSON_TABLE(x.r, '$[*]' COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j
JOIN articles a ON a.id = j.id ORDER BY j.n;
```

| function | returns |
| --- | --- |
| `sift_topk(index, query, k [, filter])` | JSON `[{"id": key, "s": score}]`, best first (BM25, ties in document order) |
| `sift_count(index, query [, filter])` | the number of matching rows |
| `sift_matches(index, query [, filter])` | JSON array of every matching key |
| `sift_tokens(index, text)` | the terms the analyzer makes of a text (what the index stores) |
| `sift_stats(index)` | JSON: `segments docs dead live drained pending bytes dpp docbase merges format` |
| `sift_idset(key)` | aggregate: a compact set of keys, the `filter` argument of the functions above |
| `sift_merge(index [, 'full'])` | merges segments now (`'full'`: all of them, dropping the deleted documents) |
| `sift_watermark(index)`, `sift_exists(index)`, `sift_version()`, `sift_create`, `sift_drop` | housekeeping used by the procedures |
| `sift_drain`, `sift_load` | aggregates that feed the index (used by `sift_sync`, `sift_rebuild`) |
| `sift_ping(k)` | k fake hits, to measure the cost of a UDF call and its JSON |

The query language is SiftQL, that of pg_sift: terms (analyzed like the indexed text), `"phrases"`, `AND` / `OR` / `NOT`
and parentheses, `{a b c}` (any of), `term^2.5` boosts, `pre*`, `w?ldcard`, `fuzzy~1`, `/regular expressions/` (an ARE
subset, matched against terms), and `field:term` for indexes with fields. A syntax error is an SQL error with the position.

## Keeping the index current

`sift_attach` creates the index, a queue table `sift_q_<index>` and three triggers (insert, update, delete) on the table.
A trigger writes `(op, key)` into the queue inside the writer's transaction: a rolled back write leaves nothing. The queue is a
log; a drain (`CALL sift_sync('news')`) reads the rows above the watermark of this node, fetches the current text of each
row, and writes one new immutable segment atomically (temporary file, `fsync`, `rename` of the manifest). An interrupted drain
is repeated harmlessly. Small drains make small segments; more than `merge_fanin` (6) of a size merge in the background.
`CALL sift_schedule('news', 1)` creates an `EVENT` that drains every second (`SET GLOBAL event_scheduler = ON`) and prunes
drained queue rows older than an hour (`CALL sift_prune('news', seconds)`).

You do not have to wait for the drain to see a change: see the next section.

## What a query sees

By default (`fresh`) every query reads the queue rows above the watermark through a local connection of the server, builds a
small in-memory live segment from them (cached while the queue does not change), and merges it with the segments on disk.
The consequences, all covered by `tests/fresh.sh`:

* An inserted or updated row is found by the next statement; a deleted row is gone; an updated row matches its new text only.
  Counts equal `SELECT COUNT(*) ... LIKE` over the table.
* A `LIMIT` is never short: documents that were deleted or replaced since the last drain are excluded inside the ranked scan,
  and the best candidates are checked against the table (`recheck`) so rows removed without a trigger (`TRUNCATE`, foreign key
  cascades) cannot fill the page.
* Isolation. The index reads the queue and the rows on its own local connection of the server, not in the caller's
  transaction. It never returns uncommitted data, the caller's own uncommitted writes included (they are found after the
  commit). With autocommit and in `READ COMMITTED` the next statement sees every row committed before it started. In
  `REPEATABLE READ` the local connection may or may not share the transaction's snapshot: a transaction that opened
  `START TRANSACTION WITH CONSISTENT SNAPSHOT` before its first call read the queue as of that snapshot in our tests, a plain
  `START TRANSACTION` read the latest committed state, and a drain by another session makes rows committed after the snapshot part
  of the segments. Joining with the table, as in the query above, applies the snapshot of the caller to the rows, so a row the
  transaction cannot see is dropped from the page; the index never invents a row. That is what an index outside the storage
  engine can promise: committed rows, never uncommitted ones, a snapshot-consistent final result when you join.
* A long writing transaction does not stall readers: the drain asks for a barrier row with a one second lock timeout and, if
  a writer is in the way, uses the barrier of the previous round and tries again on the next call.
* `fresh = false` indexes the committed state of the last drain only (the older, cheaper mode); `recheck = false` skips the
  existence check. Both are index options.

## Filters and facets

A filter restricts the ranked scan itself, so a selective filter still fills the page. It is a set of keys: the aggregate
`sift_idset(key)` over any SQL subquery (a bitmap), or a JSON array of keys.

```sql
SELECT sift_topk('news', 'power', 10, (SELECT sift_idset(id) FROM articles WHERE category = 1));
SELECT sift_matches('news', 'power', '[1, 2, 7]');
CALL sift_search('news', 'power', 10, 't.category = 1 AND t.likes > 3');   -- rows with their score, predicate over alias t
CALL sift_facets('news', 'power', 't.category', NULL);                     -- value, n: the same as GROUP BY over the matches
CALL sift_facets('news', 'power', 't.likes DIV 10', 't.category = 1');
```

`tests/filters.sh` compares both with the SQL baseline. `sift_filter(index, predicate)` leaves the bitmap in `@sift_f`.

## Index options

`CALL sift_attach_opts(name, table, key_columns, text_expression, options_json)`. The text expression is written over the alias `t`
of the table. All options are those of the pg_sift index of the same name, and give the same results (`tests/options.sh` checks
each against pg_sift):

| option | default | meaning |
| --- | --- | --- |
| `dpp` | 32 | documents per page of the index (a build parameter; 8 to 32 for small tables) |
| `k1`, `b` | 1.2, 0.75 | BM25 saturation and length normalization |
| `stemmer` | none | `"en"` (English) or none |
| `case_folding`, `accent_folding` | true | fold case / strip accents when analyzing |
| `positions` | true | store positions (phrases are decided by the index); false: phrases are rechecked on the text |
| `prefix_index` | 0 | index the first 1 to n characters of every word, so `pre*` is a lookup |
| `fields` | none | `[{"name":"title","expr":"t.title"},{"name":"body","expr":"t.body"}]`: queries can say `title:term`, `body:(a b)^2` |
| `analyzer` | `"text"` | `"keyword"`: the whole value is one term |
| `merge_fanin` | 6 | segments of a size merged together |
| `fresh`, `recheck` | true | see "What a query sees" |

## Keys

The key is the primary key of the table: an integer column (the fast path, the key is the document id) or any other
primary key, text, UUID or several columns. For those the triggers keep a dense mapping table `sift_k_<index>` (key to integer);
the functions return the integers, and `sift_search` / `sift_facets` join them back to the rows. `tests/keys.sh` covers text,
UUID and composite keys through insert, update, key change, delete, drain, merge, restart and rebuild.

## Replication and backup

The index is derived data, local to a node. Only the table and the queue (a table, with its barrier rows) travel through
the binlog (`binlog_format=ROW`); the triggers fire on the source only, and the queue rows replicate like any other.

* **Replica.** After the replica has the table and the queue, `CALL sift_rebuild('news')` builds its index (it creates the
  engine index if the node has none, and reads the table in one consistent snapshot together with the newest barrier). From
  then on a replica answers fresh queries from the replicated queue rows and `CALL sift_sync('news')` drains them with the
  watermark of its own node; a replica (`read_only`) never writes a barrier. Run `sift_sync` on each node: an `EVENT` created on the
  primary replicates as disabled on the replica, which should run a drain-only event
  (`CREATE EVENT ... DO CALL sift_sync('news')`), not `sift_schedule`, whose `sift_prune` deletes queue rows.
* **Pruning.** `CALL sift_prune('news', seconds)` deletes drained queue rows older than `seconds`; a node that is further behind
  than that gets "the queue was pruned past the watermark of this node" and runs `sift_rebuild`.
* **Backup.** `mariadb-dump --single-transaction --routines --triggers db` contains the table, the queue, the triggers and the
  index catalogue, not the index files (they are not InnoDB files: treat them as derived data, whatever physical backup you use).
  After a restore on a fresh server: register the functions (`functions.sql`), then `CALL sift_rebuild('news')` for each index.
* `tests/repl.sh` runs a private primary and replica with binlog replication and a third server for the restore, and checks
  equal counts and top-10 after catch-up, after rebuilds, after pruning, and after a dump restore.

## Operations

* `SELECT sift_stats('news')`: `segments`, live and `dead` documents, `pending` (queue rows not yet in a segment), bytes.
* Crash safety. A segment, the event log and the manifest are written to temporary files, `fsync`ed and renamed; the manifest
  rename is the only commit point; files the manifest does not name are removed at open. `tests/functional.sh` kills the
  server after a drain, `tests/live.sh` kills it at three points of a merge: the answers after the restart are those of the last
  published state.
* Merge. The background merge keeps the number of segments within `merge_fanin`; `sift_merge('news', 'full')` merges all of
  them and drops deleted documents. The postings are streamed through temporary runs; what stays in memory is the document
  map (about 70 bytes per document at the peak: 36 MB for 540,000 documents).
* Concurrency. Each index has a reader / writer lock; queries of different connections run in parallel, a drain or a merge
  publishes under a short write lock. `tests/stress.sh` runs 8 reader threads, 4 writers, a draining and a merging thread
  at once and then checks the index against the table.
* Memory and files. Segments are mapped from the page cache; they cost no heap. `<datadir>/.sift` is outside InnoDB.

## Limits

* MariaDB only. One text expression per index (a `CONCAT_WS` or `fields`), over one table; no partition awareness; no
  `ALTER TABLE` story for the indexed table (detach, alter, attach).
* Triggers are the change feed: `TRUNCATE`, bulk loads with `SET unique_checks`... that skip triggers, and foreign key cascades
  do not reach the queue. A cascaded or truncated row is never returned (the existence check), but a `TRUNCATE` leaves the
  documents in the index until `CALL sift_rebuild`; rows added by a trigger-less load need the same.
* A query reads the pending queue rows: about 0.3 ms per 1,000 of them. Drain on an `EVENT` (see "Numbers"). The live segment
  holds the text of the pending rows in memory while a query uses it.
* `sift_idset` is limited by `max_allowed_packet` and the server's UDF result size (256 MB, about 32 million keys).
* The UDF library is loaded into `mariadbd` (GPLv2) and built against its headers; pg_sift is under the PostgreSQL license,
  which is GPL compatible. Whether linking a UDF is a derivative work is a question for a lawyer.
* Not done: `MATCH ... AGAINST`, a storage engine, visibility inside the caller's own uncommitted transaction, highlighting.

## MATCH ... AGAINST

Assessment: not feasible as a UDF, and no proof is shipped. `MATCH(col) AGAINST(...)` is parsed by the server into a full-text
function that is bound to a `FULLTEXT` key of the table's storage engine: the server calls the handler's `ft_init_ext`,
`ft_read` and `ft_end`. A UDF library cannot register a handler method, and a full-text *parser* plugin (`MYSQL_FTPARSER_PLUGIN`)
only changes how text is split into words for an index that the engine (InnoDB, MyISAM, Aria) still builds, stores and ranks
itself, so it cannot route queries to sift. The only ways to the syntax are a storage engine that owns the `FULLTEXT` key (the
model of Mroonga's wrapper mode: it wraps InnoDB for the rows and answers `ft_init_ext` from its own index) or a patch of the
server. Both are C++ code against the server's private handler interface, rebuilt for every MariaDB series, with the full handler
surface (about 100 virtual methods) to wrap. The smallest next step is a read-only storage engine in the style of SphinxSE:
`CREATE TABLE hits (id BIGINT, score FLOAT, query VARCHAR(1024) NOT NULL, KEY (query)) ENGINE=SIFT CONNECTION='news'` so that
`SELECT ... FROM hits WHERE query = 'solar AND wind' LIMIT 10` plans as an index lookup and joins with the table; it needs the
engine skeleton (`rnd_init`, `index_read_map`, `ha_*` registration) and nothing of sift changes. Wrapper mode with real
`MATCH AGAINST` comes after that. Until then use the derived-table query above.

## Versions, ABI and release artifacts

* Built and tested on MariaDB 13.0.2 with PostgreSQL 18.6 headers. The sources compile against the headers of any MariaDB
  from 10.5 to 13.x and refuse other versions at compile time (a `#error` on `MYSQL_VERSION_ID`, checked by `tests/version.sh`);
  the UDF structures are `static_assert`ed. At load time the library compares its series with the running server's
  (`server_version`) and every function fails with "rebuild it against the headers of this server" on a mismatch. Only 13.0 was
  run; the other series are unverified. MySQL 8.4 / 9.x are not supported (the UDF interface and the server-side SQL service differ)
  and no MySQL headers were available to check.
* `sift_version()` reports the format of the engine, the page format and the server it was built for. An index written by
  another format is refused with "rebuild it with sift_rebuild", and `sift_rebuild` replaces it (`tests/version.sh` damages the
  manifest in four ways).
* A release artifact is a tarball per MariaDB series and CPU architecture: `libsift_udf.so` built against that series' headers
  (x86-64 baseline, dynamic glibc), `sql/functions.sql`, `sql/sift.sql`, this document. The library depends on symbols of
  `mariadbd` of that series, so a tarball for 13.0 does not load on 11.8. The index files do not depend on the server version:
  they depend on the page format (`sift_version()`), which the library checks when it opens an index.

## Numbers against InnoDB FULLTEXT

Corpus: the 218,058 articles of Simple English Wikipedia in an InnoDB table (`docs(id, body MEDIUMTEXT)`, 402 MB), a
FULLTEXT index and a sift index on `body`, InnoDB buffer pool 3 GB, one client over the Unix socket, warm cache, the host as it is. Each cell is the median over 200 evenly spaced queries of the 1,000 benchmark queries of its class (AND, OR and
phrase of the terms of an article substring), second pass, in microseconds. InnoDB: `ORDER BY MATCH(body) AGAINST(...) DESC LIMIT 10`
(`+a +b` for AND, natural language and boolean for OR, `"a b"` for phrase; `COUNT(*)` in boolean mode for counts), statements
longer than 10 s are aborted by the server and counted as timeouts. sift: the query shape above, joined with `docs`; "UDF
only" is `SELECT sift_topk(...)`; "plain" is the same query on an index with `fresh=false, recheck=false`. One run of
`mariadb/test.sh`, the last run of the final code (every correctness check above passed in it); load average of the host 20.1 at the
start of the benchmark and 11.4 at the end, `SELECT 1` at 5.9 us.

| class | sift, joined | sift, UDF only | sift, plain index | InnoDB FULLTEXT | InnoDB / sift |
| --- | --- | --- | --- | --- | --- |
| AND top-10 | 304 | 270 | 281 | 62,305 | 205x |
| OR top-10 | 1,253 | 1,157 | 1,130 | 1,232,091 natural, 908,285 boolean | 984x / 725x |
| phrase top-10 | 242 | 156 | 186 | 175,430 | 726x |
| count AND | 103 | | 69 | 65,318 | 632x |
| count OR | 269 | | 255 | 916,283 | 3,411x |
| count phrase | 201 | | 127 | 167,613 | 832x |

Build and size: sift 7.0 s to drain 218,058 documents into one segment plus 0.4 s to attach (create the queue, the triggers, queue
the rows); InnoDB `ADD FULLTEXT` 44.9 s (6.4x). An earlier run of the same code, before main was merged and with the host a little
quieter at that moment, gave 3.6 s and 21.2 s (5.9x). Index size: sift 135.9 MB (a 130.6 MB segment with positions, a 5.2 MB event
log), InnoDB 232.0 MB (`FTS_*.ibd`): 1.7x smaller.

Cost of a UDF call, 1,000 statements, median of the third pass: `SELECT 1` 5.9 us; `SELECT sift_ping(10)` 7.9 us; with
`JSON_TABLE` 16.8 us; with the join with `docs` and `ORDER BY` (the application shape) 30.4 us.

The same run before the merge of main (load average 14 to 21, `SELECT 1` at 6.9 us) read AND 351, OR 1,495, phrase 544 us joined and
count AND 160 us: the spread between two runs of identical code is as large as the differences between the columns above.

**Against the first proof of concept, and what freshness costs.** The first version of this library (same engine, no queue
read: results as of the last drain) measured AND 192, OR 730, phrase 173 us, count AND 52 us, build 4.3 s, 135.9 MB in a
quieter session (load average 7 to 12, `SELECT 1` at 4.9 us). The table above is slower in absolute terms for two reasons. The
host was busier (every statement of the table costs 1.4 to 2x what it did then: InnoDB's AND went from 93 ms to 149 ms
on the same data), and the default index now does more work per call. To separate them the original code and the new code ran
back to back, three alternating rounds in one session (load average 13), same corpus, same 200 statements per cell, median of the
three rounds' medians:

| statement (us) | first version | now, default (`fresh`, `recheck`) | now, plain index |
| --- | --- | --- | --- |
| AND top-10, joined | 267 | 441 | 307 |
| AND top-10, UDF only | 163 | 296 | 181 |
| OR top-10, joined | 1,189 | 1,341 | 983 |
| phrase top-10, joined | 342 | 397 | 286 |
| count AND | 77 | 118 | 88 |
| count OR | 234 | 288 | 225 |
| count phrase | 140 | 131 | 159 |

The plain index is the first version's speed, within the noise of the host (the rounds of one cell differ by up to 50%); the
engine did not regress. The default costs 20 to 170 us per top-10 and 15 to 75 us per count (the interleaved rounds and the plain column of the table, whose noise is of the same size): the queue read and the existence
check are statements on a local connection of the server (about 50 us each under this load), and they are what makes a
write visible to the next query and a page never short. The price is paid per call and does not grow with the index; it does grow
with the rows waiting for a drain, because the live segment is built from them: measured on the same corpus, 1,000 pending rows
cost about 0.3 ms per query, 10,000 about 3.8 ms, 100,000 about 32 ms. Keep `sift_sync` on an `EVENT` (every second by
default in `sift_schedule`) and the queue stays in the hundreds. Use `fresh=false, recheck=false` for a read-mostly table you drain
on a schedule and want the lowest latency; build time and size did not change (3.6 to 7.0 s against 4.3 s, 135.9 MB against 135.9 MB).

Honest losses. sift's OR is its slowest class (1.1 to 1.5 ms: OR queries over common terms match most of the corpus);
the joined shape adds 15 to 50 us over the bare function and `JSON_TABLE` evaluates the function once only through the derived table;
anything under a few thousand rows does not need an external index at all (not measured; InnoDB FULLTEXT is fast enough there);
the absolute numbers move by about 50% from session to session on this shared host, the ratios (200x to 3,700x) do not; InnoDB
timeouts are counted at the limit, which understates the ratio.


## Tests

`mariadb/test.sh` builds, starts a private PostgreSQL (the pg_sift reference) and a private MariaDB under `/tmp`, loads the
218,058-article Simple English Wikipedia corpus, runs everything below and the benchmark, and deletes both servers (`STEPS`,
`KEEP`, `WORK`, `SIFT_BENCH_DATA`, `BENCH_QUERIES`: see the header of the script). `mariadb/server.sh` is the private server on
its own (`start|stop|sql|destroy`). No sudo; the benchmark runs on the host as it is and the load average of the host is printed at the start and the end.

| suite | what it checks |
| --- | --- |
| `test.sh` pgref | 5,098 queries (and / or / phrase traces, prefix, wildcard, fuzzy, NOT, boosts, groups, nesting, phrases under OR/NOT including CJK, regular expressions): count, top-10 order and float32 scores equal to pg_sift |
| `test.sh` inno | match sets against InnoDB FULLTEXT (boolean mode), with the analyzer differences listed below |
| `tests/functional.sh` | 63 checks: queries, DML visible at once, rollback, update / delete, drain, merge, restart, kill -9, concurrent writers, EVENT |
| `tests/fresh.sh` | freshness without sync, LIMIT never short, rollback / savepoints, `REPEATABLE READ` / `READ COMMITTED`, TRUNCATE, `fresh=false` |
| `tests/keys.sh` | text, UUID, composite keys, the integer fast path |
| `tests/filters.sh` | bitmap, id list and predicate filters, facets against GROUP BY |
| `tests/options.sh` | every index option against pg_sift with the same options (needs the private PostgreSQL) |
| `tests/version.sh` | version strings, refused manifests, the compile-time window, the CMake build |
| `tests/live.sh` | small segments, background merge, bounded merge memory, kill -9 during a merge, log compaction |
| `tests/stress.sh` | reader, writer, drain and merge threads at once, then index equals table |
| `tests/regex_check.sh` | the regular expression engine against PostgreSQL's, 227,040 pattern / word pairs |
| `tests/repl.sh` | primary, replica, rebuild, prune, dump restore |

Against InnoDB FULLTEXT the match sets differ only where the analyzers differ: InnoDB keeps `_` and an apostrophe between letters
inside a word and splits at U+00AD; sift (like the PostgreSQL parser the trace comes from) splits at `_` and `'` and keeps U+00AD
inside the token. On a copy of the text with those characters turned into spaces, 97 to 100 of 100 queries per class are equal; the rest
are off by 1 or 2 documents of tens of thousands.

## Layout and shared core

| path | what |
| --- | --- |
| `udf.c`, `sql/` | MariaDB-specific: the UDFs (arguments, JSON, the local SQL connection) and the procedures / triggers |
| `engine.c`, `engine.h`, `ix.h`, `query.c`, `feed.c`, `merge.c` | the index: manifest, event log, live segment, queries, drains, merge, locks |
| `store.c` | segment files mapped as the buffer pool; write targets; the visibility filter |
| `shim.c`, `shim.h`, `regex.c` | the PostgreSQL-backend shim: memory contexts, `ereport` as `longjmp`, hash tables, strings, UTF-8, temporary files, buffer stubs, and the `pg_reg*` regular expression API |
| `cli.c` | `sift_cli`: the engine without a server |
| `Makefile`, `CMakeLists.txt` | the build |
| `tests/`, `bench/`, `tools/`, `test.sh`, `server.sh` | tests, the benchmark, the comparison tools, the private server |

Compiled from `src/` unmodified: `sift_bitmap*` (with its seek limit made per thread by `sed`), `sift_kernels*`, `sift_analyze*`,
`sift_query`, `sift_dict`, `sift_posting`, `sift_segment`, `sift_prefix`, `sift_eval`, and `sift_build.c` up to its `ambuild` marker
(cut with `sed`). Not compiled: the access method, the planner, parallel scans, the SQL-callable test code.

**Shared core.** The MariaDB-specific files are `udf.c`, `cli.c`, `sql/`, `engine.c` / `query.c` / `feed.c` (the manifest and queue
protocol), and `store.c`; the shim (`shim.c`, `shim.h`, `regex.c`, and the thread-local memory-context macros of the Makefile) is
a separate layer under them. When the common core (`sift_host.h` and the shared algorithms of the `refactor/dry-core` branch)
lands, these duplicate what it will provide and should be replaced by it:

* `merge.c`: the tier choice (`tier_of`, `pick`) and the driver around `sift_runs_write` (merge policy, run merging);
* `engine.c`: `apply_opts`, `check_opts`, `set_meta` and the `cfg` hash (option parsing, validation, the analyzer-options hash);
* `store.c`: `sift_filter_from_docids` and the `sift_filter_*` helpers (copies of `src/sift_filter.c` over docids), the segment
  write target (`sift_wt_*`);
* `sql/sift.sql`: `sift_facets` (facet logic as SQL over the matches);
* `regex.c`: the `pg_reg*` implementation, which any host without PostgreSQL's regex library needs.

What stays MariaDB-specific after that: the local-connection host callbacks and UDF glue of `udf.c`, the queue / trigger / barrier
protocol of `sql/` and `feed.c`, the `MADV_POPULATE_READ` mapping and thread-local contexts.
