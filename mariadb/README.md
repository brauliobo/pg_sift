# sift for MariaDB

BM25 full-text search for MariaDB tables: ranked top-k, exact counts, phrases, prefix / wildcard / fuzzy / regular
expression terms, AND / OR / NOT, boosts, fields, filters and facets, as a user defined function library
(`libsift_udf.so`) and two SQL files. It is the portable core of [pg_sift](../README.md) (the files of `src/portable.list`, compiled
unmodified, the same ones the PostgreSQL and SQLite extensions use) over a small host in `mariadb/host/`, so the page format, the
analyzer, the ranking and the query language are those of pg_sift; for the same text and the same options the counts, the top-10
order and the scores are bit identical.

It adds no storage engine and patches nothing: a trigger-fed queue table keeps the index in step with an InnoDB table, the
index lives in plain files next to the data directory, and a query is a function call. Use it when `MATCH ... AGAINST`
is too slow or too limited (InnoDB FULLTEXT takes 0.09 to 1.0 s for a ranked top-10 on 218,058 articles, sift 0.4 to 1.3 ms), you
want BM25 and phrases that behave the same on every engine of the product, or you want filters and facets inside the ranked
scan. Do not use it if you need `MATCH ... AGAINST` syntax (it cannot be added to a UDF, see "MATCH ... AGAINST") or a
MySQL server (MariaDB only).

Contents: [install](#install) - [quick start](#quick-start) - [queries](#queries) - [keeping the index current](#keeping-the-index-current) -
[visibility](#what-a-query-sees) - [filters and facets](#filters-and-facets) - [options](#index-options) - [keys](#keys) -
[replication and backup](#replication-and-backup) - [operations](#operations) - [limits](#limits) - [MATCH AGAINST](#match--against) -
[versions](#versions-abi-and-release-artifacts) - [numbers](#numbers-against-innodb-fulltext) - [tests](#tests) - [layout](#layout-and-shared-core)

## Install

**From a release.** `sift-mariadb-<version>-linux-x86_64.tar.gz` holds one library per MariaDB series (`lib/<series>/libsift_udf.so`,
`SELECT VERSION()` tells yours; `BUILD-INFO` lists the server version and glibc each was built for), `sql/functions.sql`,
`sql/sift.sql` and this document. A library loads only into the series it was built for. Copy the one of your series into the
server's plugin directory and run the two SQL files as below.

**From source.** Needs a MariaDB server 10.5 to 13.x (built and tested on the series listed under "Versions"), its development
headers (`libmariadbd-dev`, `/usr/include/mysql/server`), a C compiler and `make` (or CMake). Nothing else: the build needs no
PostgreSQL. `libmariadb` is only needed for the test tools.

```sh
nice make -C mariadb -j4 OUT=/tmp/sift-build            # or: cmake -S mariadb -B /tmp/sift-build && nice cmake --build /tmp/sift-build -j4
mariadb -e "SHOW VARIABLES LIKE 'plugin_dir'"           # where the server loads UDF libraries from
cp /tmp/sift-build/libsift_udf.so /usr/lib/mysql/plugin/   # that directory (needs write access to it); a release: lib/<series>/libsift_udf.so
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
  them and drops deleted documents. The postings are streamed through temporary runs whose sort buffers are bounded by
  `maintenance_work_mem` (64 MB, set in `shim.c`); what else stays in memory is the document map (about 70 bytes per document at
  the peak: 36 MB for 540,000 documents).
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

* Tested on MariaDB 13.0.2 (the whole suite below, on the host's server) and, with `tests/smoke.sh` (12 checks: version string, ranked
  top-10, count, matches, DML visible at once, prefix / wildcard / fuzzy / regular expression, sync and merge, restart, detach), on the
  stock Docker images `mariadb:10.11` (10.11.19), `11.4` (11.4.13), `11.8` (11.8.9), `12.1` (12.1.2) and `13.0` (13.0.2), each loading the
  library built against the headers of that very image. The sources compile against the headers of any MariaDB from 10.5 to 13.x and
  refuse other versions at compile time (a `#error` on `MYSQL_VERSION_ID`, checked by `tests/version.sh`); the UDF structures are
  `static_assert`ed. 10.5 to 10.10, 11.0 to 11.3, 11.5 to 11.7 and 12.0 have no release library and were not run. MySQL 8.4 / 9.x are not
  supported (the UDF interface and the server-side SQL service differ).
* ABI. The library calls symbols of `mariadbd` itself, so it works only with the series whose headers it was built against; at load time
  it compares its series with the running server's (`server_version`) and every function fails with "rebuild it against the headers of
  this server" on a mismatch. It needs libc and libm
  only (no libstdc++, no libmariadb), x86-64 baseline (no POPCNT, no vector instruction outside the runtime-dispatched kernels:
  `make isa-check`), glibc 2.34 (Ubuntu 22.04 image, series 10.11) or 2.38 (the 24.04 and 26.04 images, series 11.4 to 13.0).
* `sift_version()` reports the release, the format of the engine, the page format and the server it was built for. An index written by
  another format is refused with "rebuild it with sift_rebuild", and `sift_rebuild` replaces it (`tests/version.sh` damages the
  manifest in four ways). The index files do not depend on the server version: they depend on the page format, which the library
  checks when it opens an index.
* Release artifact: `sift-mariadb-<version>-linux-x86_64.tar.gz`, built by `scripts/package-mariadb.sh` (and the `mariadb` job of
  `.github/workflows/release.yml`): `lib/<series>/libsift_udf.so` for each series above, `sql/functions.sql`, `sql/sift.sql`, LICENSE,
  NOTICE, this document and `BUILD-INFO` (server version and glibc floor of each library). Every library is built in the stock
  `mariadb:<series>` image against `libmariadbd-dev` from MariaDB's own repository (which pins the headers to that server's exact
  version) and loaded into a server of the same image by the smoke test before the tarball is accepted; the tarball is reproducible
  (sorted names, commit time) and goes into the single `SHA256SUMS` of the release. The version is the tag (`v1.4.0` is 1.4.0) and
  must equal `mariadb/VERSION`; [CHANGELOG.md](CHANGELOG.md) lists the changes. A distribution with an older glibc, or another
  architecture, builds from source (`make`).

## Numbers against InnoDB FULLTEXT

Corpus: the 218,058 articles of Simple English Wikipedia in an InnoDB table (`docs(id, body MEDIUMTEXT)`, 402 MB), a FULLTEXT index and
a sift index on `body`, InnoDB buffer pool 3 GB, one client over the Unix socket, warm cache, the host as it is. Each cell is the median
over 200 evenly spaced queries of the 1,000 benchmark queries of its class (AND, OR and phrase of the terms of an article substring),
second pass, in microseconds. InnoDB: `ORDER BY MATCH(body) AGAINST(...) DESC LIMIT 10` (`+a +b` for AND, natural language and
boolean for OR, `"a b"` for phrase; `COUNT(*)` in boolean mode for counts); a statement over 10 s is aborted (none was). sift: the query
shape above, joined with `docs`; "UDF only" is `SELECT sift_topk(...)`; "plain" is the same query on an index with
`fresh=false, recheck=false`. The sift columns are the mean of six interleaved rounds (see below); the InnoDB column is one `mariadb/test.sh`
run of the final code in the same session. The host was shared and busy throughout: load average 21 to 32 (`SELECT 1` takes 7.2 us),
so absolute values are about 1.5x those of a quiet host; the ratios hold.

| class | sift, joined | sift, UDF only | sift, plain index | InnoDB FULLTEXT | InnoDB / sift |
| --- | --- | --- | --- | --- | --- |
| AND top-10 | 476 | 245 | 298 | 87,191 | 183x |
| OR top-10 | 1,301 | 1,196 | 1,228 | 943,532 natural, 1,032,812 boolean | 725x / 794x |
| phrase top-10 | 424 | 250 | 338 | 213,772 | 504x |
| count AND | 110 | | 83 | 112,650 | 1,025x |
| count OR | 269 | | 247 | 1,506,334 | 5,608x |
| count phrase | 172 | | 144 | 233,629 | 1,357x |

Build and size: `sift_rebuild` of the 218,058 documents takes 6.6 s (mean of six rounds, standard error 0.9 s); the first attach (create,
queue the table, triggers) took 0.5 s and the drain 10.9 s under a load of 25; InnoDB `ADD FULLTEXT` took 58.9 s in that session
(8.9x the rebuild, 5.4x the first drain). Index size: sift 135.9 MB (a 130.6 MB segment with positions, a 5.2 MB event log), InnoDB 232.0 MB (`FTS_*.ibd`): 1.7x smaller.

Cost of a UDF call, 1,000 statements, median of the third pass: `SELECT 1` 7.2 us; `SELECT sift_ping(10)` 11.1 us; with `JSON_TABLE`
29.5 us; with the join with `docs` and `ORDER BY` (the application shape) 51.0 us, at that load.

**Against the build before the shared core.** The engine moved onto the portable core shared with pg_sift and the SQLite extension (see
"Layout and shared core"). `mariadb/bench/ab.sh` measures two builds of the library on the same server and corpus, interleaved: six
rounds, each restarting the server with the build that goes first in that round (the order alternates), each build on indexes it made
itself, 200 queries per cell, two passes, median of the second. Mean and standard error over the rounds, in microseconds, and the
ratio after / before of the paired rounds (below 1 is faster); load average 22 to 32:

| statement | before | after | after / before |
| --- | --- | --- | --- |
| AND top-10, joined | 466 +- 56 | 476 +- 81 | 1.05 +- 0.15 |
| AND top-10, UDF only | 286 +- 36 | 245 +- 36 | 0.89 +- 0.12 |
| AND top-10, plain index | 373 +- 41 | 298 +- 34 | 0.82 +- 0.09 |
| OR top-10, joined | 1,488 +- 120 | 1,301 +- 152 | 0.90 +- 0.13 |
| OR top-10, plain index | 1,361 +- 149 | 1,228 +- 168 | 0.94 +- 0.15 |
| phrase top-10, joined | 513 +- 113 | 424 +- 52 | 0.94 +- 0.18 |
| phrase top-10, plain index | 293 +- 41 | 338 +- 51 | 1.20 +- 0.18 |
| count AND | 139 +- 10 | 110 +- 11 | 0.82 +- 0.12 |
| count OR | 307 +- 35 | 269 +- 20 | 0.93 +- 0.14 |
| count phrase | 179 +- 21 | 172 +- 21 | 0.98 +- 0.11 |
| build (s) | 7.3 +- 0.8 | 6.6 +- 0.9 | 0.93 +- 0.09 |
| index size (MB) | 135.9 | 135.9 | 1.00 |

No cell is slower beyond its standard error (the only one more than two standard errors from 1 is faster: AND on the plain index; the
mean ratio over the 15 cells is 0.97); the host's noise, 10 to 15% per cell at this load, is the limit of what the comparison can resolve. The index files are byte for byte the same size, and the 5,098-query
comparison with pg_sift still reports 0 differences with bit-identical scores.

**What freshness costs.** The default index reads the queue and checks existence on every call (statements on a local connection of the
server, about 50 us each under this load), which makes a write visible to the next query and a page never short. In the table above
that is 70 to 180 us per top-10 and 20 to 30 us per count over the plain index. The price is paid per call and does not grow with the
index; it grows with the rows waiting for a drain, because the live segment is built from them: 1,000 pending rows cost about 0.3 ms per
query, 10,000 about 3.8 ms, 100,000 about 32 ms. Keep `sift_sync` on an `EVENT` (every second by default in `sift_schedule`) and
the queue stays in the hundreds. Use `fresh=false, recheck=false` for a read-mostly table you drain on a schedule and want the lowest
latency; build time and size do not change.

Honest losses. sift's OR is its slowest class (1.2 to 1.3 ms: OR queries over common terms match most of the corpus); the joined shape
adds 100 to 230 us over the bare function; anything under a few thousand rows does not need an external index at all (not measured;
InnoDB FULLTEXT is fast enough there); absolute numbers move by about 50% from session to session on this shared host, the ratios
(180x to 5,600x) do not.

## Tests

`mariadb/test.sh` builds, starts a private PostgreSQL (the pg_sift reference) and a private MariaDB under `/tmp`, loads the
218,058-article Simple English Wikipedia corpus, runs everything below and the benchmark, and deletes both servers (`STEPS`,
`KEEP`, `WORK`, `SIFT_BENCH_DATA`, `BENCH_QUERIES`: see the header of the script). `mariadb/server.sh` is the private server on
its own (`start|stop|sql|destroy`). No sudo; the benchmark runs on the host as it is and the load average of the host is printed at the start and the end.

| suite | what it checks |
| --- | --- |
| `test.sh` pgref | 5,098 queries (and / or / phrase traces, prefix, wildcard, fuzzy, NOT, boosts, groups, nesting, phrases under OR/NOT including CJK, regular expressions): count, top-10 order and float32 scores equal to pg_sift |
| `test.sh` inno | match sets against InnoDB FULLTEXT (boolean mode), with the analyzer differences listed below |
| `tests/functional.sh` | 67 checks: queries, DML visible at once, rollback, update / delete, drain, merge, restart, kill -9, concurrent writers, EVENT |
| `tests/fresh.sh` | freshness without sync, LIMIT never short, rollback / savepoints, `REPEATABLE READ` / `READ COMMITTED`, TRUNCATE, `fresh=false` |
| `tests/keys.sh` | text, UUID, composite keys, the integer fast path |
| `tests/filters.sh` | bitmap, id list and predicate filters, facets against GROUP BY |
| `tests/options.sh` | every index option against pg_sift with the same options (needs the private PostgreSQL) |
| `tests/version.sh` | version strings, refused manifests, the compile-time window, the CMake build |
| `tests/live.sh` | small segments, background merge, bounded merge memory, kill -9 during a merge, log compaction |
| `tests/stress.sh` | reader, writer, drain and merge threads at once, then index equals table |
| `tests/regex_check.sh` | the regular expression engine against PostgreSQL's, 227,040 pattern / word pairs |
| `tests/repl.sh` | primary, replica, rebuild, prune, dump restore |
| `tests/smoke.sh` | a release library in a stock MariaDB image (`scripts/package-mariadb.sh`): version, queries, DML, merge, restart, detach |

Against InnoDB FULLTEXT the match sets differ only where the analyzers differ: InnoDB keeps `_` and an apostrophe between letters
inside a word and splits at U+00AD; sift (like the PostgreSQL parser the trace comes from) splits at `_` and `'` and keeps U+00AD
inside the token. On a copy of the text with those characters turned into spaces, 97 to 100 of 100 queries per class are equal; the rest
are off by 1 or 2 documents of tens of thousands.

## Layout and shared core

| path | what |
| --- | --- |
| `udf.c`, `sql/` | MariaDB-specific: the UDFs (arguments, JSON, the local SQL connection) and the procedures / triggers |
| `engine.c`, `engine.h`, `ix.h`, `query.c`, `feed.c`, `merge.c` | the index: manifest, event log, live segment, queries, drains, merge driver, locks |
| `host/host.h` | the host interface of the portable core (`SIFT_HOST_MARIADB` in `src/sift_host.h`): the plain-C vocabulary of `sqlite/host/host.h`, whose `host.c` (memory contexts, `ereport` as `longjmp`, hash tables, strings, UTF-8, temporary files, heaps) is compiled as it is, with `host_stubs.c` (no mutable segments, no kept docmap directories) |
| `host/store.c`, `host/store.h` | `src/sift_store.h` over segment files mapped as the buffer pool: the stream reader and writer, write targets |
| `host/stubs.c` | the engine's own visibility, and the filter of a list of documents as the TID bitmap the shared filter builder reads |
| `host/host_regex.c` | `pg_regcomp` / `pg_regexec` / `pg_regprefix`: a matcher for PostgreSQL's advanced regular expressions |
| `cli.c` | `sift_cli`: the engine without a server |
| `Makefile`, `CMakeLists.txt` | the build (it reads `src/portable.list`) |
| `tests/`, `bench/`, `tools/`, `test.sh`, `server.sh` | tests, the benchmark, the comparison tools, the private server |

Compiled from `src/` unmodified: every `.c` file of `src/portable.list`, except that `sift_bitmap.c` is compiled from a copy whose seek limit is
per thread (`sed`, checked by the build). No PostgreSQL header, library or code is involved: `scripts/check-portable.sh` guarantees that the
files of the list reach their host only through `sift_host.h`.

**What is shared, and what is not.** Taken from the core and not copied: the merge policy (`sift_merge_pick`, tiers from 1 MiB), the merge of
segments through sorted runs (`sift_run_from_segment`, `sift_runs_write`), the inverted buffer of a drain (`SiftInvBuf`), the deletion of a
document from a segment (`sift_seg_delete_key`: the merge deletes the engine's dead documents from its private copy of the pages, then
rewrites them), the filter builder (`sift_filter_build`), the extent bookkeeping and padding of byte streams (`sift_stream.c`), the CPU
dispatch, the analyzer options. MariaDB's own: the manifest, the event log and the queue protocol (`engine.c`, `feed.c`, `sql/`), the
local-connection host callbacks and the UDF glue (`udf.c`), the mapped store with its `MADV_POPULATE_READ`, the background merge thread, the
live segment of a query. One implementation is kept apart on purpose: the regular expression matcher. The C library's POSIX matcher (what
the SQLite host uses, in `sqlite/host/host_regex.c`) has no `\d`, `\w`, lookaround, back-references or lazy quantifiers and matches
bytes, not characters; on the 5,098 comparison queries that alone made two differ from pg_sift (a `.` against a Greek letter),
and `tests/regex_check.sh` compares this matcher with PostgreSQL's on 227,040 pattern and word pairs.

