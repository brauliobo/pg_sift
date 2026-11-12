# Upgrading

What to run, in order, to move a database to a newer pg_sift, to a newer PostgreSQL major, or back to an older pg_sift. On-disk formats and the
first release that reads each: [FORMATS.md](FORMATS.md).

| Change | What happens to the files | Steps |
|---|---|---|
| a newer pg_sift (patch or minor) | the new library reads the files of the older ones; it writes its own format from the next write on | [preflight](#1-preflight), [install and update](#2-install-and-update) |
| 3.2.1 to 3.2.2 | library only: the extension SQL stays at 3.2, no format changes | install the files and restart the server (an open session keeps the old library, and pg_sift in `shared_preload_libraries` is read at start only); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back to 3.2.1 needs nothing ([notes](releases/v3.2.2.md)) |
| 4.0.2 to 4.0.3 | the library and two functions, `sift.index_choices` and `sift.help`: the extension SQL goes from 4.0 to 4.0.3, no format changes | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX`; going back: [an older release](#4-an-older-release) ([notes](releases/v4.0.3.md)) |
| 4.0.3 to 4.0.4 | the library only; the extension SQL stays at 4.0.3. The log header of a `sift_col` index with `track_changes` gets one more flag (additive: an older library reads it as an incomplete log and checks the heap) | install the files, restart the server (a standby too); no `ALTER EXTENSION UPDATE`, no `REINDEX`: an index that 4.0.0 to 4.0.3 left with wrong counts is re-baselined by the next `VACUUM` of its table, by the first read, or by the maintenance worker (`sift.col_tracking`: `complete` true, `heap_free` false until then); going back: [an older release](#4-an-older-release) |
| 4.0.4 to 4.0.5 | library only: the extension SQL stays at 4.0.3, no format changes; two wrong answers of the aggregate scan are fixed | install the files and restart the server (a standby too; an open session keeps the old library); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back needs nothing first ([notes](releases/v4.0.5.md)) |
| 4.0.3 to 4.1.0 | the library and the planner (new scans and costs; four new settings, `sift.agg_probe_rewrite`, `sift.agg_probe_direct`, `sift.col_build_key_ranges`, `sift.col_auto_merge_pass_mb`): the extension SQL goes from 4.0.3 to 4.1 with no change of its objects, no format changes | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX` (a `REINDEX` of a `key_cut` index builds it by key ranges); from 4.0 to 4.0.2 the update goes through 4.0.3 by itself; going back: [an older release](#4-an-older-release) ([notes](releases/v4.1.0.md)) |
| 4.1.0 to 4.1.1 | library only: the extension SQL stays at 4.1, no format changes; one new value of an option, `track_changes = 'ctid'` (never chosen by `auto`) | install the files and restart the server (a standby too; an open session keeps the old library); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back to 4.1.0: first `ALTER INDEX <index> SET (track_changes = 'auto')` on every index you set to `'ctid'` (4.1.0 does not know its row trigger and fails `UPDATE` and `DELETE` of the table with `sift.col_track_old_rows is the statement trigger of an index option`), then nothing else ([notes](releases/v4.1.1.md)) |
| 4.1.1 to 4.1.2 | library only: the extension SQL stays at 4.1, no format changes; the log of a `sift_col` index with `track_changes` that an earlier release wrote is re-checked and re-baselined on its own | install the files and restart the server (a standby too; an open session keeps the old library); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`, no `ALTER INDEX` (after `pg_upgrade`, once for each index: see the section below); going back to 4.1.1 needs nothing first ([notes](releases/v4.1.2.md)) |
| 4.1.2 to 4.1.3 | library only: the extension SQL stays at 4.1, no format changes; `REINDEX INDEX CONCURRENTLY` of a `sift_col` index with `track_changes` no longer takes ACCESS EXCLUSIVE on the table | install the files and restart the server (a standby too; an open session keeps the old library); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back to 4.1.2 needs nothing first ([notes](releases/v4.1.3.md)) |
| 4.1.3 to 4.1.4 | library only: the extension SQL stays at 4.1, no format changes; two wrong answers of the aggregate scan are fixed | install the files and restart the server (a standby too; an open session keeps the old library); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back needs nothing first ([notes](releases/v4.1.4.md)) |
| a newer pg_sift from 4.1 to 4.2 | seven changes of files, plans, a setting and one new SQL function | [what 4.2 changes](#what-42-changes) |
| 4.2.0 to 4.2.1 | the library and one function, `sift.col_track_convert()`: the extension SQL goes from 4.2 to 4.2.1, no format changes; `DROP INDEX CONCURRENTLY` of a `sift_col` index with `track_changes` works (the index depends on its two triggers, where they depended on it) | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database: it converts the indexes of the database (it takes `SHARE UPDATE EXCLUSIVE` on each table that has a tracked index, which waits for a `DROP INDEX`, `REINDEX` or `VACUUM` of the table that is running and lets the writers go on), no DDL on a tracked `sift_col` index needs to wait for it (a `CREATE INDEX`, `REINDEX`, `ALTER INDEX` or `DROP INDEX` before the update behaves as after it), only the `DROP INDEX CONCURRENTLY` of an index made by 4.2.0 or earlier does, and until it runs the concurrent drop of an index made by 4.2.0 or earlier still fails (its triggers are found, so reads, writes, `REINDEX`, `ALTER INDEX` and a plain `DROP INDEX` work as before; an index made before the update is dropped with its triggers); no `REINDEX` is needed to drop; going back to the 4.2.0 library after the update leaves such an index with triggers that library does not find (`REINDEX` there makes them again), and `ALTER EXTENSION pg_sift UPDATE TO '4.2'` does not undo the conversion (its script is empty: the index keeps depending on its triggers, and a plain `DROP INDEX` with the 4.2.0 library then leaves the triggers behind, which fail the writes of the table: drop them with `DROP TRIGGER` after the index) |
| 4.2.1 to 4.2.2 | the library and one function, `sift.table_find_stale_versions()`: the extension SQL goes from 4.2.1 to 4.2.2, no format changes; a `VACUUM` of a `sift_table` that waited for the maintenance lock no longer makes old versions of updated rows, or deleted rows, visible again (4.0.0 to 4.2.1) | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX`; the update does not repair rows that already came back: run `sift.table_find_stale_versions('<table>')` on each updated `sift_table` and follow the notes; a database at 4.2 goes through 4.2.1 by itself; going back to 4.2.1: install its files, restart, `ALTER EXTENSION pg_sift UPDATE TO '4.2.1'` (the function stays, the cause returns) ([notes](releases/v4.2.2.md)) |
| 4.2 to 4.3 (also from 4.2.1 and 4.2.2) | a new minor: the library, the planner, connected indexes, a write buffer of references, seekable order chunks (column format minor 44, which 4.2 and older refuse), three deprecated options and new functions: the extension SQL goes from 4.2 to 4.3 | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX` is needed; update every node that reads an index (the standbys first) before the primary writes one; going back needs a setting, `REINDEX` and the 4.2 files: [4.2 to 4.3](#42-to-43) ([notes](releases/v4.3.0.md)) |
| 4.3 to 4.3.1 | the library and one statement of the extension SQL (`sift.table_option_values` gets its replica identity: with a `FOR ALL TABLES` publication, 4.2.0 to 4.3.0 failed every `DROP INDEX` and `DROP TABLE` of the database, `ALTER EXTENSION pg_sift UPDATE` repairs it), no new object, no format changes; a read inside a statement that deletes rows (an `AFTER ROW` trigger, an `AFTER STATEMENT` trigger that fires before the index's own, a function the statement calls) no longer counts the rows the statement deleted, through a `sift_col` index with `track_changes` (4.0.0 to 4.3.0) | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX`, nothing stored is wrong; until then `ALTER INDEX <index> SET (track_changes = 'off')` on the indexes read that way; going back to 4.3: install its files, restart, `ALTER EXTENSION pg_sift UPDATE TO '4.3'` (the cause returns) ([notes](releases/v4.3.1.md)) |
| 4.3.1 to 4.3.2 | the library only: the extension SQL goes from 4.3.1 to 4.3.2 with no new object, no format changes; a correlated sub-select over a join with a derived table that PostgreSQL does not pull up (`GROUP BY`, `DISTINCT`, `UNION`, `HAVING`) no longer returns the answer of an earlier outer row (4.0.5, 4.1.4, 4.2.2, 4.3.0 and 4.3.1) | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX`, nothing stored is wrong; until then `SET sift.enable_agg = off` for such a query; going back to 4.3: install its files, restart, `ALTER EXTENSION pg_sift UPDATE TO '4.3'` (the cause returns) ([notes](releases/v4.3.2.md)) |
| 4.3 to 5.0 (also from 4.3.1 and 4.3.2) | a new minor: the scope index and scope copy (experimental), named projections, a shared store for strings, reads under writers without the heap, a rolling rewrite; the extension SQL goes from 4.3 to 5.0 (two event triggers, new functions and tables); new column format minors 45, 46 and 49 (47 is unassigned and 48 reserved; nothing writes either) written only by the features that need them (4.3 and older refuse them); three options ignored and one setting removed | install the files, restart the server (a standby too), then `ALTER EXTENSION pg_sift UPDATE` in each database; no `REINDEX` is needed; reset the ignored options and remove the setting ([what 4.3 deprecates and 5.0 ignores](#what-43-deprecates-and-50-ignores), [what 5.0 removes](#what-50-removes)); going back: [an older release](#4-an-older-release) ([notes](releases/v5.0.0.md)) |
| a newer PostgreSQL major | `pg_upgrade` copies (or links) the files as they are | [pg_upgrade](#3-a-newer-postgresql-major) |
| an older pg_sift (a downgrade, or a standby that stays on an older release) | an older library refuses what it cannot read | [preflight with `--to`](#4-an-older-release) |

## 1. Preflight

`scripts/upgrade-check.sh` runs `sift.upgrade_check()` (`scripts/upgrade_check.sql`) in each database. It reads the catalogs and metapages and changes nothing
(the function is loaded in a transaction that is rolled back, so a database with an older extension can be checked). It needs a superuser on the primary
and the files of the new release installed:

```
scripts/upgrade-check.sh --all                 # every database that accepts connections
scripts/upgrade-check.sh app reporting         # these databases (or connection strings)
scripts/upgrade-check.sh --strict app          # a warning also ends with exit status 1
```

One row per finding: `STATUS` (`ok`, `warn`, `blocker`), `OBJECT`, `KIND`, `DETECTED` and `ADVICE`. The exit status is 0 when nothing blocks, 1 when a
database has a blocker (or a warning with `--strict`) and 2 when a database could not be checked.

| Kind | What it looks at | Status and advice |
|---|---|---|
| `version` | `pg_extension` against `sift.version()` | `warn`: the library is newer than the extension (`ALTER EXTENSION pg_sift UPDATE`) or the new release is available; `blocker`: the library is older than the extension scripts |
| `index`, `table` | the metapage format and the column formats of every `sift` index, `sift_col` index and `sift_table` | `blocker`: this library cannot open it (the error and its hint are the advice); with `--to`, the first release that reads it is newer than the target |
| `option` | options that only units written afterwards follow | `warn`: `cube_minmax`, `rollup_columns`, `key_columns`, `order_columns`, `point_columns`, `point_tid_columns`, `key_cut` or `hll_columns` on an index with older units, JSON `paths` on segments before column format 6 (the advice is the command from `sift.upgrade_commands()`, which an extension older than 3.3 has after its update; the three removed options below are `reset` rows of it, `ALTER INDEX .. RESET`; a `sift_table` below the format of its schema is a `table` row with `VACUUM FULL`), the deprecated `mutable_limit` |
| `setting` | `sift.*` in the server configuration, the session, `ALTER DATABASE` and `ALTER ROLE` | `warn`: a setting the library does not define (removed or renamed), a feature switched off (a boolean setting that defaults on), `sift.col_format_minor` kept below the default |
| `trigger` | with `--to` a release before 3.3: the event triggers of 3.3; before 5.0: `sift_col_build_lock` and `sift_col_value_store_sql_drop` | `blocker`: the older library has not their function and would fail every `ALTER TABLE` and `CREATE TRIGGER` (3.3), every `CREATE INDEX` and every `DROP INDEX` (5.0); `ALTER EVENT TRIGGER x DISABLE`, `ENABLE` when the newer library runs again |
| `test` | test-only settings and the extension `pg_sift_test` | `warn`: remove them; they need the test build of the library |

Run it before and after the update: the second run must have no `warn` the first one explained.

## 2. Install and update

```

# 1. install the files of the new release (package, or from source)
sudo apt install ./postgresql-N-pg-sift_X.Y.Z-1_amd64.deb
make USE_PGXS=1 && sudo make USE_PGXS=1 install

# 2. preflight (section 1), then in every database that has the extension
psql -d app -c "ALTER EXTENSION pg_sift UPDATE"

#    library keeps it until its connection ends, so after every library install restart the server or reconnect every pool and worker)
sudo systemctl restart postgresql

# 4. check the update
psql -d app -c "SELECT extversion, sift.version() FROM pg_extension WHERE extname = 'pg_sift'"
psql -d app -c "SELECT sift.verify('docs_idx', true)"            # a sift index
psql -d app -c "SELECT sift.col_verify('facts_col', true)"       # a sift_col index
scripts/upgrade-check.sh app
```

`ALTER EXTENSION pg_sift UPDATE` runs the scripts from the installed version to the default one of the control file, through every release in between. Index and table
files are not rewritten: an older index stays in its format until a merge, `VACUUM` or `REINDEX` writes it again, and the preflight and the update tell which
`REINDEX` is worth it. A `sift_table` cannot be reindexed; it follows `sift.col_format_minor` on its next write.

**The extension version of a patch release.** From 4.2 a patch release `X.Y.N` has the extension version `X.Y.N`, and `sift.version()` (the loaded library) returns the same string, so
`ALTER EXTENSION pg_sift UPDATE` after installing the files moves the database to it and the two queries of step 4 agree. The step from one patch release to the next changes no object. A database at
any `X.Y.N` reaches the next minor with the one script of that minor, because every patch release also ships an empty script back to `X.Y`. The releases before 4.2 kept the extension version of
their minor (4.1.1 to 4.1.4 are 4.1, 4.0.4 and 4.0.5 are 4.0.3), so on them `ALTER EXTENSION pg_sift UPDATE` has nothing to do and PostgreSQL prints its own notice, `version "4.1" of extension "pg_sift" is
already installed`: it is harmless, the library files are the ones that changed. `sift.version()` of those releases is the minor too (`4.1`); the release that runs is in
`SELECT version FROM pg_get_loaded_modules() WHERE module_name = 'pg_sift'` (PostgreSQL 18 and later: `4.1.4+` and the commit), after a restart or a reconnect, as an open session keeps the library it loaded.
If `sift.version()` is older than `extversion`, the library files are older than the extension scripts: install the release or a newer one
(`scripts/upgrade-check.sh` reports it as a blocker).

The last step of an update to 3.3 (from any older version) prints what is left to do, as warnings with the commands (the first 20 of each, then the count of the rest), or a
notice that nothing is (PostgreSQL shows only warnings from an extension script). `SELECT * FROM sift.upgrade_commands()` lists the same later, for the indexes and tables the role owns or may maintain (every one for a superuser):
one row per command (`kind` `reindex` or `rewrite`, the `object`, the `reason`, the `command`), the cheapest first.

| Warning | Why | Command |
|---|---|---|
| a `sift` (text) index needs a `REINDEX` | **queries miss rows**: since 3.3 a word over 255 bytes is stored as its first 236 bytes and a hash; an older index holds it cut at 255 bytes, so a query for it finds nothing. The index does not record whether it holds such a word, so every text index except `analyzer = keyword` is listed, also after its `REINDEX` | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_col` index would gain from a `REINDEX` | an option set after the units were written (`cube_minmax`, JSON `paths`, `rollup_columns`, `key_columns`, `order_columns`, `point_columns`, `point_tid_columns`, `key_cut`, `hll_columns`): older units stay readable and are scanned (for `order_columns`, read whole by an `ORDER BY .. LIMIT`) until then | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_table` would gain from a rewrite | segments below the column format of its schema: no calendar cells for its date and timestamp columns (format 8), JSON kinds that may be incomplete for declared paths (format 6). A `sift_table` cannot be reindexed | `VACUUM FULL schema.table;` |

Only the first warning means wrong results without the command. `VACUUM FULL` takes an `ACCESS EXCLUSIVE` lock and rewrites the whole table in the format of `sift.col_format_minor`, so
schedule it; once it ran the table leaves the list. `REINDEX CONCURRENTLY` cannot run in a transaction block: run the commands one at a time. The check reads the catalogs and
metapages and cannot fail the update; when it cannot answer (a relation locked by another session) it says so, and `sift.upgrade_commands()` can be run after reconnecting.

## What 4.2 changes

Ten changes of 4.2 touch what an index holds, what an older library does with it, a plan you may have measured, or an object you may have made. Only 7, 8 and 9 ask for a step (remove a setting that is no longer accepted, before the update); `ALTER EXTENSION pg_sift UPDATE`
runs `pg_sift--4.1--4.2.sql` and the new library reads the indexes of 4.1 and older as they are. Each entry says what changes, what an older library does with the result, whether a `REINDEX` is needed, and the switch that turns it off.

1. **The index keeps a presence bit for the `jsonb` columns that may be NULL.** `CREATE INDEX` and `REINDEX` of a `sift_col` index write one boolean leaf for each `jsonb` column that has `paths` and no `NOT NULL`
   constraint on the column of the table (an index expression counts as nullable; a `CHECK` or a domain that forbids NULL is not seen), so `NOT (doc @> x)`, `(doc @> x) IS FALSE`, `NOT (doc ? 'k')` and `doc IS [NOT] NULL`, also in a `FILTER` or a `CASE`, are answered by the index where they read the table.
   There is no option and no NOTICE: the layout of an index is the one it was built with (`ALTER TABLE .. SET NOT NULL` or `DROP NOT NULL` changes nothing; `REINDEX` decides again). No column format minor changes. Measured on 150,000 documents, 1% to 50% of them without a declared member: `count(*), sum(id) WHERE NOT (doc @> x)` 84.9 M to 7.4 M cycles at 1% and 85.0 M to 9.9 M at 50%, `count(*) WHERE doc IS NULL` 34.9 M to 1.9 M at 5%.
   The price, on 1,000,000 rows: `CREATE INDEX` +3.2% instructions, an insert +0.4%, a promotion +4.6%, a merge +5.0%, the index +2.0% in size after a merge (up to +13% right after the build of a wide index, which a merge removes).
   - *Existing indexes* are not touched: they have no leaf, those forms keep reading the table, and nothing is wrong; `REINDEX` writes the leaf.
   - *An older library* (3.2, 4.0 and 4.1, tried with 3.2.3, 4.0.5 and 4.1.4 on the files of 4.2) refuses an index that has the leaf: a read, an aggregate included, fails with `sift_col index "x" does not match its column layout` (hint
     `REINDEX the index.`), a write with `index "x" was built with other column definitions` (hint `Use REINDEX to apply the new options.`), and `sift.col_verify` reports `The metapage does not describe this column layout.`
     The way back is to `REINDEX INDEX x` with the older release, which writes the layout it reads; there is no switch. `scripts/upgrade-check.sh --to 4.1` lists the indexes with the leaf as blockers, with that advice.

2. **The write buffer of a BM25 (`sift`) index is format 6.** The full pages of the buffer have a bloom of term hashes of 8,192 bits where format 0 has 256, so a search for a term that none of the buffered documents has no longer reads every page
   of the buffer (220,000 documents and a buffer of 2,000: 1.96 M to 1.23 M cycles). Upgrading takes no step: the buffer that is open at the update keeps format 0 and is read as it is; the next one is format 6, and the first of them
   raises the metapage of the index to minor 1.6 and nothing lowers it ([FORMATS.md](FORMATS.md)).
   - *Existing indexes* need nothing; `sift_col` indexes and `sift_table` tables have no such buffer.
   - *An older library* (4.1 and older) refuses a BM25 index of 1.6 on a read, a `sift.verify` and an insert: `sift index "x" has on-disk format 1.6, this build (pg_sift 4.1) reads formats 1.0 to 1.5`, hint `Install the pg_sift
     release that wrote the index on every node that reads it, or REINDEX it.` A standby or a node that stays on the older release is such a node. `scripts/upgrade-check.sh --to 4.1` lists the index as a blocker.
   - *To go back:* install the older release and `REINDEX` the index there (it is built again from the table). *Off:* none; an index that has taken no insert since the update is still format 0.

3. **The aggregate scan is chosen more often for a grouped aggregate of one table.** For a grouped aggregate with no lookup join, no search and no top-N whose normal plan is a sequential scan and a hash of the groups, the scan's own work per row
   is now priced in the units of the plan it is compared with (PostgreSQL's estimate of that plan runs at about 210 cost units a millisecond, the scan's price at 833), so a scan that was up to 4 times faster is no longer refused as `cost` when its rows are not few:
   `max(ends - starts)` by tenant over 150,000 rows 18.6 to 7.1 ms, a `COALESCE` measure over 20,000 events 3.4 to 2.1 ms. A statement the scan does not win stays on the normal plan. This changes plans, not files.
   - *An older library* and the indexes: nothing; a statement is planned by the library that runs. *`REINDEX`:* no.
   - *Off:* no switch separates this choice; `SET sift.enable_agg = off` (a session, a role or a database) plans the statement as PostgreSQL does without pg_sift, and `sift.explain_why(query)` says why a statement did or did not take the scan.

4. **One rule for `HAVING column IN (SELECT ..)` and `EXISTS (SELECT ..)`, and sub-selects of the groups.** In a plain `GROUP BY`, such a `HAVING` condition is always answered as a filter of the rows, as a semi join the scan runs, where
   PostgreSQL keeps it above the aggregate and the sub-select ran for each group (`GROUP BY kind_id HAVING kind_id IN (SELECT id FROM kinds WHERE enabled)`: 300,000 rows, 1,000 keys, 40.1 to 0.58 ms); the rows form was the cheaper in all 40 cells measured,
   at 0.09 to 0.78 of the cycles. A sub-select in the select list or the `HAVING` that reads the group keys or the aggregates is evaluated by the scan once for each group (36.8 to 1.85 ms for 300,000 rows and 1,000 groups). `NOT IN`, `NOT EXISTS`,
   an expression or a cast of the key, a correlated `IN`, grouping sets and a `HAVING` without `GROUP BY` keep their `HAVING`. The rows are the same (`agg_having_where`: 46 statements equal to the rows with the scan off).
   - *An older library*, *`REINDEX`:* nothing, no file changes. *Off:* no switch for this rule alone; `sift.enable_agg = off` as above.

5. **A `sift_col` index with `track_changes` repairs its own log in more cases.** The log of an index whose triggers do not vouch for it (after `pg_upgrade`, which makes the triggers again; `pg_restore --disable-triggers`; `ALTER TABLE .. DISABLE TRIGGER ALL` then
   `ENABLE TRIGGER ALL`; `CREATE INDEX CONCURRENTLY`; `REINDEX CONCURRENTLY`; `DETACH PARTITION`) was checked against the heap on every read until `REINDEX` or `ALTER INDEX`. Now the next `VACUUM`, the first read that checks the heap or the
   maintenance worker that a write asks for re-baselines it (`ShareUpdateExclusiveLock` only). Triggers enabled for the origin only (what `ENABLE TRIGGER ALL` leaves, `tgenabled = 'O'`) are enabled for all sessions (`'A'`) by the worker, which asks
   for the lock of `ENABLE TRIGGER` without waiting; triggers you disabled (`'D'`) stay disabled and the reads keep checking the heap. 300,000 rows churned and not vacuumed: the second statement of a grouped count costs 38.1 M instructions and 918 blocks
   while the log is not vouched, 29.7 M and 467 when it is; after the repair it is 33.9 M and 585 (the repaired log is a little larger, a `VACUUM` or a merge shrinks it).
   - *Format:* none, no minor changes; answers are right before and after. *An older library:* tried with 4.1.1 on an index that 4.2 repaired (`ALTER TABLE .. DISABLE TRIGGER ALL`, `ENABLE TRIGGER ALL`, a `VACUUM`, the
     worker on: `sift.col_tracking` true, true, true, true and the triggers `'A'` again): it reads and writes the index without an error, its aggregates equal the heap's before and after its own writes and
     `sift.col_verify` is clean, but it does not take the log for complete (`sift.col_tracking`: `triggers`, `complete` and `heap_free` false), so its reads check the heap as they did before the repair; the repair
     itself is the re-baseline of 4.1.2 and later. *`REINDEX`:* not needed. *Off:* none for the repair; `ALTER INDEX x SET (track_changes = 'off')` drops the log and its triggers for an index that should not have them.

6. **`sift.refusal_actions()` returns what to change for each refusal slug** (the statement, the index or a setting, the hint of the notice of `sift.explain_refusals`): `sift.explain_why(q) w JOIN sift.refusal_actions() a USING (slug)`.
   It is a new function; `sift.refusal_slugs()` keeps its three columns, and `pg_sift--4.1--4.2.sql` drops or replaces no function, so a view, a default or a function of yours that reads the objects of 4.1 does not
   make `ALTER EXTENSION pg_sift UPDATE` fail (`test/t/extension_lifecycle.sh` updates a database that has a view over `sift.refusal_slugs()`). *Files, `REINDEX`, off:* not applicable.

7. **`sift.eval_methods` no longer takes `latest_hint`, `snapshot_masks`, `unit_rewrite` and `skip_trigger_lookup`** (nor the `-name` form of them); `penalty_maintenance` is the only name left. `skip_trigger_lookup` is now always on (the statement trigger of
   `track_changes` leaves out the look at an UPDATE that kept the key), `latest_hint` and `snapshot_masks` had nothing behind them, and the writer of `unit_rewrite` is gone (segments an earlier release wrote with it stay readable).
   What happens to a setting that names one (tried on this tree): with the library loaded, a `SET`, `ALTER ROLE .. SET`, `ALTER DATABASE .. SET` and `ALTER SYSTEM SET` fail with `ERROR: invalid value for parameter "sift.eval_methods": "NAME"` and
   `DETAIL: Unknown evaluation method: NAME`, so a script that runs one breaks; a value already stored (`postgresql.conf`, `postgresql.auto.conf`, a role or database setting) does not stop the server or a login, but every session that loads the library
   and the log of the server say the same text as a WARNING, and the setting is ignored. Remove them before updating:
   ```sql
   SELECT coalesce(r.rolname, '(all roles)') AS role, coalesce(d.datname, '(all databases)') AS database, c AS setting
   FROM pg_db_role_setting s LEFT JOIN pg_roles r ON r.oid = s.setrole LEFT JOIN pg_database d ON d.oid = s.setdatabase, unnest(s.setconfig) c
   WHERE c LIKE 'sift.eval_methods=%';                                        -- ALTER ROLE r [IN DATABASE d] RESET sift.eval_methods; or ALTER DATABASE d RESET sift.eval_methods
   SELECT name, setting, sourcefile, sourceline FROM pg_file_settings WHERE name = 'sift.eval_methods';   -- superuser; remove the line, or ALTER SYSTEM RESET sift.eval_methods, then SELECT pg_reload_conf()
   ```
   `scripts/upgrade-check.sh` lists each of them as a blocker with the command (any release of the library, so it can run before the update). *Files, `REINDEX`:* no. *Off:* the names are gone; leave the setting empty.

8. **`sift.enable_col` is removed.** It was declared in 3.0 and no code ever read it (`sift.enable_col_scan` and `sift.enable_agg` are the switches of the planner), so setting it changed nothing, whatever its documentation said.
   What happens to a setting that names it (tried on this tree): with the library loaded, a `SET`, `ALTER ROLE .. SET`, `ALTER DATABASE .. SET` and `ALTER SYSTEM SET` fail with `ERROR: invalid configuration parameter name "sift.enable_col"` and
   `DETAIL: "sift" is a reserved prefix.`, so a script that runs one breaks; a value already stored (`postgresql.conf`, `postgresql.auto.conf`, a role or database setting) does not stop the server or a login, but every session that loads the library
   and the log of the server say `WARNING: invalid configuration parameter name "sift.enable_col", removing it` with `DETAIL: "sift" is now a reserved prefix.`, and `SHOW sift.enable_col` is `unrecognized configuration parameter`. Remove it before updating:
   ```sql
   SELECT coalesce(r.rolname, '(all roles)') AS role, coalesce(d.datname, '(all databases)') AS database, c AS setting
   FROM pg_db_role_setting s LEFT JOIN pg_roles r ON r.oid = s.setrole LEFT JOIN pg_database d ON d.oid = s.setdatabase, unnest(s.setconfig) c
   WHERE c LIKE 'sift.enable_col=%';                                          -- ALTER ROLE r [IN DATABASE d] RESET sift.enable_col; or ALTER DATABASE d RESET sift.enable_col
   SELECT name, setting, sourcefile, sourceline FROM pg_file_settings WHERE name = 'sift.enable_col';    -- superuser; remove the line, or ALTER SYSTEM RESET sift.enable_col, then SELECT pg_reload_conf()
   ```
   `scripts/upgrade-check.sh` lists each of them as a blocker with the command (any release of the library, so it can run before the update). *Files, `REINDEX`:* no. *Off:* not applicable; to compare a plan with the normal one, `SET sift.enable_agg = off` and `SET sift.enable_col_scan = off`.

9. **`sift.table_auto_defrag_dead_pct` is removed.** It was the trigger of the background pass of a `sift_table` in 3.3 (a percent of tombstones); since 4.0 the pass is asked for by the reads of the table and the setting changed nothing. It is refused and dropped
   exactly as `sift.enable_col` of item 8 (`ERROR: invalid configuration parameter name "sift.table_auto_defrag_dead_pct"`, `DETAIL: "sift" is a reserved prefix.`; a stored value: `WARNING: invalid configuration parameter name "sift.table_auto_defrag_dead_pct", removing it`),
   and the queries and commands of item 8 with that name find and clear it; `scripts/upgrade-check.sh` lists each as a blocker. *Files, `REINDEX`:* no. *Off:* not applicable (`sift.table_auto_defrag = off` turns the pass off).

10. **Two reloptions of `sift_col` have clearer names: `rollup` is `rollup_leaves` (the array elements and map values a unit groups by) and `rollup_dims` is `rollup_columns` (the scalar columns of the rollup of the rows).** The older names stay accepted through 4.x:
    `CREATE INDEX` and `ALTER INDEX .. SET` with one say `NOTICE: reloption "rollup" is the older name of "rollup_leaves"` (`"rollup_dims"` and `"rollup_columns"` for the other) and read it as the new one; when both are given the new one wins; `ALTER INDEX .. SET` and `RESET` of the old name work;
    an index that carries the old name keeps it in `pg_class.reloptions`, `pg_dump` writes it as it is, and `pg_upgrade` and `ALTER EXTENSION UPDATE` need nothing. The build writes the new name where it decides a value itself (`rollup_columns = 'auto: a, b'`). The messages, the
    choices of `sift.index_choices` and the errors name the new spelling (`invalid rollup_columns option: ..`). `sift.upgrade_commands()` matches both spellings, so an index with either gets the same advice; a script or a view of yours that matches the text of `pg_class.reloptions` for `rollup_dims`
    should match `rollup_columns` too. `sift.index_choices` reports the choice as `rollup_columns`. The limit ids of `sift.stats()` keep their names (`limit.rollup_dims`). *Files, `REINDEX`:* no (the files hold no option names). *Off:* not applicable; the old names go away in 5.0.

## 4.2 to 4.3

`ALTER EXTENSION pg_sift UPDATE` runs `pg_sift--4.2--4.3.sql` (4.3 is `default_version = '4.3'`); a database at 4.2.1 or 4.2.2 first runs the empty script back to 4.2 that every patch release ships, then that one. The script adds the functions and the view of connected indexes (`sift.col_connection_state()`, `sift.col_connection_list()`, `sift.col_connection_fill()`, `sift.col_connections`) and `sift.col_arena_status()`, and replaces `sift.workers`, `sift.col_index_status` and `sift.upgrade_commands()` (a view or a function of yours that reads them keeps working); it drops nothing. The new library reads every index and table that 4.2 and older wrote, and nothing asks for a `REINDEX`. Four things are worth reading before the update.

1. **Column format minor 44, which a library before 4.3 refuses.** A segment is stamped with the lowest minor that describes what it holds. A 4.3 build writes minor 44 for each of the following, and the defaults of 4.3 turn each of them on:

   | Written when | By default | Switch |
   |---|---|---|
   | an order chunk that is seekable, a positional chunk of the second order by a group, or the packed table of groups: every index with order arrays chosen by the build | yes, under `sift.col_format_minor` 15 or more | `SET sift.col_format_minor = 14` before the build |
   | the tombstone log of a parent index that has a connected child (the key journal), and the segments of a connected child (a found flag for each connection) | when an index is connected (below) | `sift.col_auto_connect = off`, `connect = 'off'` |
   | a write buffer that holds a record that is a reference to a heap tuple (`sift.col_wbuf_ref`) | yes, for a heap table whose index tracks changes | `sift.col_wbuf_ref = off` (records already written stay references until the buffer is promoted) |

   A 4.2 library that reads such a segment fails with `sift_col index "x" has a segment of column format 1.44, this build reads 1.3 to 1.43` and the hint to `REINDEX` (a scan, `VACUUM`, `sift.col_verify` and `sift.col_segments`); it refuses the merge and the promotion of a connected index, and appends tombstones to the log of a parent without the journal records, so a database must not be written by 4.2 once a connected child exists. No answer is wrong: the tests found no wrong row. `scripts/upgrade-check.sh --to 4.2` lists each such index as a blocker. An index written by a pre-release build of 4.3 (which stamped minors 45 to 47) is refused by the release with `this build reads 1.3 to 1.44` and needs a `REINDEX`; no released version is concerned.

2. **What happens to an index that exists.** Nothing, until a write or a rebuild: a merge makes the choices of the build again for the segments it writes, and `REINDEX` for all of them, and the old and the new segments answer the same queries meanwhile. So an index of 4.2 gets, from its next merge or `REINDEX`: a serial id as an order array and no point directory (5,000,000 rows: 63.3 MB to 49.1 MB), the second order by a group column chosen from a b-tree of the table or from a foreign key, seekable order chunks (minor 44), and, at `REINDEX`, the automatic connection. The choices are named in the `NOTICE` of `CREATE INDEX` and `REINDEX` and listed by `sift.index_choices('index')`: `sift_col index "x" is column format minor 44, which a library before 4.3 cannot read`.

3. **`sift.col_format_minor` is kept.** It is the switch for a rollback: `SET sift.col_format_minor = 14` before a build writes a format that an older library reads (the hint of that `NOTICE`), and the build then says `does not track changes: sift.col_format_minor is below 15` for an index that would track them. It is a superuser setting, and the background worker that rewrites an index follows the server and database settings only, so use `ALTER SYSTEM` or `ALTER DATABASE` when no write may go beyond 4.2 (the same rule as for 3.2 below).

4. **A new write path and two new plans.** (a) `sift.col_wbuf_ref` (on): the rows of a write buffer of a heap table with a tracked index are 16-byte references and their values live in a shared arena of up to 128 MB for the server; the first read of a buffer after a restart, on a standby or on a promoted standby reads the heap once (about 11,900 instructions a row). (b) `CREATE INDEX` and `REINDEX` of a `sift_col` index connect it to the `sift_col` index of the table that each validated single-column integer foreign key references (`sift.col_auto_connect`, on), and say so in a `NOTICE`; a `REINDEX` of an index of 4.2 can therefore connect it, and the first child of a parent holds the writers of the parent table out from the end of its scan to the end of the transaction. (c) The planner takes the child-first plan and the found-flag answer where they cost less (`sift.agg_child_first`, `sift.agg_connect_found`; `sift.enable_agg = off` plans as PostgreSQL does). Only (a) and (b) change files: they are the minor above; (c) changes plans only.

**What to do.**

- Update as in section 2: install, restart (a standby too), `ALTER EXTENSION pg_sift UPDATE` in each database, then `scripts/upgrade-check.sh` (it warns for the three ignored options below; nothing blocks). **Update the standbys and every node that reads the index before the primary**, or set `sift.col_wbuf_ref = off` and `sift.col_auto_connect = off` (and `sift.col_format_minor = 14` for the other sections of minor 44) on the primary until they are updated.
- `REINDEX INDEX CONCURRENTLY` is worth it for an index with a serial id, for an index whose table is read by `WHERE col = x ORDER BY ts LIMIT n` through a b-tree you want to drop, and for a child table whose foreign key you want connected; not for the rest. Reset the three deprecated options first (below). A `REINDEX` after `ALTER INDEX x SET (connect = 'off')` leaves an index unconnected.
- The options `order_columns`, `point_columns` and `key_cut_rows` are accepted and ignored or replaced by 4.3 and ignored by 5.0 (the names stay accepted, with a `NOTICE`): [What 4.3 deprecates and 5.0 ignores](#what-43-deprecates-and-50-ignores). `key_order = 'col'` of one column no longer builds the point directory: set `point_tid_columns` on the key for lists of keys.

### Going back to 4.2

Run this on the 4.3 database while the 4.3 library still runs. There is no script from 4.3 to 4.2: `ALTER EXTENSION pg_sift UPDATE TO '4.2'` fails with `extension "pg_sift" has no update path from version "4.3" to version "4.2"`, the catalog stays at 4.3, and what 4.3 added to it (the functions of connected indexes and `sift.col_arena_status()`) is unusable with the 4.2 library, as in the section "After the downgrade" below.

```
# 1. what 4.2 cannot read: one row for each index, with the advice
scripts/upgrade-check.sh --to 4.2 app

# 2. every write from now on is readable by 4.2 (superuser; new sessions pick it up)
psql -d postgres -c "ALTER SYSTEM SET sift.col_wbuf_ref = off" -c "ALTER SYSTEM SET sift.col_auto_connect = off" \
     -c "ALTER SYSTEM SET sift.col_format_minor = 14" -c "SELECT pg_reload_conf()"

# 3. connected indexes: switch the connection off (child and parent), then rebuild every index of step 1
psql -d app -c "ALTER INDEX public.events_col SET (connect = 'off')"
psql -d app -c "REINDEX INDEX CONCURRENTLY public.events_col"
psql -d app -c "REINDEX INDEX CONCURRENTLY public.dim_a_col"

# 4. nothing blocks any more (exit status 0; the warnings that the settings are below the default are the point)
scripts/upgrade-check.sh --to 4.2 app

# 5. stop the server, install the 4.2 files, start it; then check
psql -d app -c "SELECT sift.col_verify('events_col', true)"
```

- **An index with a `scope` (5.0) cannot go back to a release before 5.0:** it refuses the option and the scope segments, and `sift.col_format_minor = 14` cannot write them (the `REINDEX` is refused, naming the reason). The preflight (`sift.upgrade_check('4.3')`) advises `ALTER INDEX x RESET (scope)`, then `SET sift.col_format_minor = 14` and `REINDEX INDEX x`, which writes the entity segments the older release reads; the reads of the scope go through the join again (the scope copy of `sift.scope_create` is a table and needs nothing).
- The `REINDEX` under `sift.col_format_minor = 14` writes no seekable chunk and no section of minors 15 and above (so no minor 44), and an index built so does not track changes (its `NOTICE` says it), so its reads check the heap. After the 4.2 files run, `ALTER SYSTEM RESET sift.col_format_minor` and `REINDEX` the index again to log its changes again; keep `sift.col_format_minor = 14` while any 4.3 library still writes.
- `ALTER INDEX x SET (connect = 'off')` drops the dependency on the parent index, and the parent keeps its journal (minor 44) until its own `REINDEX`, which is why step 3 rebuilds the parent too. `sift.col_wbuf_ref = off` does not turn a buffer that holds references into one that does not: the `REINDEX` of step 3 does, or its promotion.
- A standby that stays on 4.2 needs steps 2 and 3 on its primary before anything is written, and `sift.col_format_minor` set there for every later write.
- Settings the 4.3 library accepts and 4.2 does not know (`sift.col_wbuf_ref`, `sift.col_auto_connect`, `sift.agg_child_first`, `sift.agg_connect_found`, `sift.agg_dimmap_kb`, `sift.col_connect_refresh_pct`): remove them from `postgresql.conf` and from role and database settings when the 4.2 files are installed, as for any setting a release does not define.

## What 4.3 deprecates and 5.0 ignores

Three `sift_col` options are accepted and ignored by 4.3 and still ignored by 5.0, which keeps accepting them so that nothing breaks: an index that stores one opens, takes `REINDEX` and `ALTER INDEX` of another option, and a dump that names one restores. A `CREATE INDEX` or `ALTER INDEX` that names one says so in a `NOTICE` (`reloption-removed`; so does the restore of a dump), `REINDEX` of an index that stores one is silent, and `scripts/upgrade-check.sh` lists each index that carries one as a warning with the `ALTER INDEX .. RESET` that clears it. Indexes built before keep reading as they are; `REINDEX` writes the layout the defaults choose.

| Option | What 4.3 and 5.0 do | What to do |
|---|---|---|
| `order_columns` | writes no sorted run; the order arrays (`order_array_columns`, chosen by the build) answer a top-N, a range and an `OFFSET` | `ALTER INDEX .. RESET (order_columns)`; keep `order_array_columns` if you set it |
| `point_columns` | lists its columns as heap addresses, as `point_tid_columns` does | set `point_tid_columns` to the same columns and reset `point_columns` |
| `key_cut_rows` | the units aim at the rows of an average superblock | `ALTER INDEX .. RESET (key_cut_rows)` |

`order_layout` and `sift.col_format_minor` stay.

## What 5.0 writes that 4.3 does not read

Three features write column formats that 4.3 and older refuse (`sift.upgrade_check('4.3')` lists each index with its command; [FORMATS](FORMATS.md)):

| what | column format | to go back to 4.3 | to go back to 4.2 or older |
|---|---|---|---|
| a `scope` option | 45 (49 for a scope of two paths) | `ALTER INDEX x RESET (scope)`, `SET sift.col_format_minor = 14` and `REINDEX`, as the preflight advises (the reads of the scope go through the join again) | the same |
| strings held by a shared value store | 46 | `SET sift.col_value_store = off` and `REINDEX` the members: the strings are written in the segments, change tracking and the structures of minors 15 to 44 stay | `sift.col_format_minor = 14` and `REINDEX` |

`sift.col_value_store = off` is the narrow switch: it stops only the store. `sift.col_format_minor = 14` stops the store too, and with it change tracking (a build says so in a `NOTICE` and its reads check the heap) and every structure of minors 15 and above, so it is for a release before 4.3 only.

## What 5.0 removes

**`sift.col_auto_merge_dirty_pct` is removed.** It was the percent of the units of a `sift_col` index holding a dead row above which the automatic rewrite ran (30). Under churn every unit holds one, so the rule never told a rewrite that pays from one that does not; a unit with a dead row is now the reason and the pace is the one that stayed: `sift.col_auto_merge_wal_pct` (which the reads that have cost half of the rewrite skip) and the snapshots that hold blocks. An index with dead rows in under 30% of its units, which waited for those reads, is rewritten when the budget allows, as one with more was. The index WAL of the write benchmark does not change.
It is refused and dropped exactly as `sift.enable_col` of item 8 of [what 4.2 changes](#what-42-changes) (`ERROR: invalid configuration parameter name "sift.col_auto_merge_dirty_pct"`, `DETAIL: "sift" is a reserved prefix.`; a stored value in `postgresql.conf`, `postgresql.auto.conf` or a role or database setting does not stop the server or a login and is dropped with a `WARNING` in every session), and the queries and commands of item 8 with that name find and clear it. A `pg_dumpall` of a cluster that stored it as a database or role setting restores with that one `ERROR` for the `ALTER DATABASE .. SET` or `ALTER ROLE .. SET` line (psql goes on); `scripts/upgrade-check.sh` lists each as a blocker, before the update. *Files, `REINDEX`:* no, it is not a reloption and no segment records it. *Off:* not applicable.

## 3. A newer PostgreSQL major

`pg_upgrade` copies the relation files and restores the catalogs. pg_sift needs only its library for the new major next to the server (the same release as
before: `pg_upgrade` does not change the extension version), and the same data checksum setting on both clusters (PostgreSQL 18 enables checksums by default; a cluster
without them is initialised with `initdb --no-data-checksums`).

```

# the library of the same pg_sift release for the new major
sudo apt install ./postgresql-N-pg-sift_X.Y.Z-1_amd64.deb
make USE_PGXS=1 PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config && sudo make USE_PGXS=1 PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config install   # from source

# the new cluster, then the check (it starts both clusters and loads pg_sift in each), then the upgrade
/usr/lib/postgresql/18/bin/initdb -D /var/lib/postgresql/18/main
/usr/lib/postgresql/18/bin/pg_upgrade -b /usr/lib/postgresql/17/bin -B /usr/lib/postgresql/18/bin \
    -d /var/lib/postgresql/17/main -D /var/lib/postgresql/18/main --check
/usr/lib/postgresql/18/bin/pg_upgrade -b /usr/lib/postgresql/17/bin -B /usr/lib/postgresql/18/bin \
    -d /var/lib/postgresql/17/main -D /var/lib/postgresql/18/main
```

Afterwards: copy the `sift.*` lines of the old `postgresql.conf` (`pg_upgrade` carries settings stored in the catalogs, `ALTER DATABASE` and `ALTER ROLE`, not the file), start the new
cluster, run `vacuumdb --all --analyze-in-stages`, and check as in section 2 (`sift.verify`, `sift.col_verify`, `scripts/upgrade-check.sh --all`). BM25 results and scores, column aggregates and `sift_table` contents
are the same as before, and writes, `VACUUM` and merges work on what the old cluster wrote
(`test/t/pg_upgrade.sh`: PostgreSQL 17 to 18 when both are installed, otherwise 18 to 18). The triggers of a `sift_col` index that tracks its changes are made again by the restore, so until the
log is re-baselined (the first `VACUUM` of the table, which the step above runs, or the maintenance worker that a read or a write of the table asks for) its reads check the heap: the answers are right, and the
instructions of a statement over rows that were not vacuumed rise (`sift.col_tracking(index)` shows `triggers` false). Nothing to run by hand
(`test/t/col_track_revouch.sh`: a cluster written by 4.1.0, PostgreSQL 17 to 18).

## 4. An older release

**A BM25 (`sift`) index written by 4.2 is not readable by 4.1 or older once it has taken an insert.** 4.2 opens the write buffer of a BM25 index in buffer format 6 (a bloom of 8,192 bits for each full page, [FORMATS.md](FORMATS.md)); the first such buffer raises the metapage of the index to minor 1.6 and nothing lowers it. A 4.1 or older library refuses the index with the format error and a `REINDEX` hint (`reads formats 1.0 to 1.5`). Upgrading takes no step: the buffer that is open at the upgrade keeps format 0 and is read as it is. To go back, install the older release and `REINDEX` the index there (the old library does not read the index, but `REINDEX` builds a new one from the table); a `sift_col` index or a `sift_table` is not affected.

**A database written by the 3.3 library with its default settings can no longer be read by a 3.2 library.** An older library refuses a metapage minor or a column format it does not know, with a `REINDEX` hint
(`old_lib_refusal`), and 3.3 writes at `sift.col_format_minor = 15` by default: a `sift_col` index created or rewritten after the update carries sections 3.2 does not know, and so does a `sift_table` with a date or timestamp
column. Only what was written with the settings below stays at minor 4 or 6, the highest a 3.2 library reads (a segment is stamped with the lowest minor that describes its content):

| Minor | Written when | Keeps the 3.2 bytes |
|---|---|---|
| 7 | a `sift_col` index with `cube_minmax` (on by default) | `WITH (cube_minmax = false)`, or `sift.col_format_minor` 6 or less |
| 8 | the calendar cells of a date, timestamp or timestamptz column: a `sift_col` index (`calendar_columns` defaults to all of them), a `sift_table` | `WITH (calendar_columns = 'off')`; a `sift_table`: `sift.col_format_minor` 7 or less |
| 9 | `rollup_columns` (option, off) | do not set it, or `sift.col_format_minor` 8 or less |
| 10 | `key_columns` naming a column that keeps a summary of its keys: text or uuid, or an int2, int4, int8 or date one whose key list or bitmap would not fit (option; `auto` never picks one) | do not name it, or `sift.col_format_minor` 9 or less |
| 11 | a `sift_table` write buffer that took a row over the buffer (`sift.table_wide_rows`, on by default; a 3.2 library reads, writes beside and promotes it, but the preflight lists it until promoted) | `SET sift.table_wide_rows = off` (a row over the buffer is refused again) |
| 12 | a `sift_col` index with `superunit` (16 by default) | `WITH (superunit = 0)`, or `sift.col_format_minor` 11 or less |
| 13 | `order_columns` (option, off) | do not set it, or `sift.col_format_minor` 12 or less |
| 14 | a string dictionary of 1,024 strings or more (text, varchar, uuid) stored in blocks to be read in place, where that adds at most 3% to the segment (ids, hashes, uuids, long strings; short repeating strings stay compact), with the string order for a column in `key_columns`, `point_columns` or `return_columns`, any column with `dict_lookup = on`, or in a `sift_table` with `sift.table_dict_lookup = on`; `point_columns` (option, off) | `sift.col_format_minor` 13 or less |
| 15 | `key_columns`: a key list or bitmap of an int2, int4, int8 or date column (the foreign key and `_id` columns the default `auto` picks, or the ones named) | `WITH (key_columns = 'off')`, or `sift.col_format_minor` 14 or less |
| 16 | `point_tid_columns` (option, off): the point directory lists heap addresses | `sift.col_format_minor` 14 or less (then it lists rows, minor 14) |
| 17 | `key_cut` (option, automatic from 3.4 for an index that benefits, or named): the units are cut by the range of a key and list heap addresses | `ALTER INDEX x SET (key_cut = 'off')` and `REINDEX`; `sift.col_format_minor` 14 or less also writes superblock units |
| 18 | `order_columns` (option, off): the sorted runs also hold their heads (the best keys at each end of a unit with their heap addresses) | `ALTER INDEX x RESET (order_columns)` and `REINDEX`, or `sift.col_format_minor` 14 or less (the runs are written without heads, minor 13) and `REINDEX` |
| 19 | a leaf NULL in nearly every row of a unit (a soft-delete stamp): the unit also keeps the pre-aggregates of the non-NULL rows, so `leaf IS NULL` is answered from them | `REINDEX` (a merge writes them too), or `sift.col_format_minor` 14 or less (written without) and `REINDEX` |
| 20 | `hll_columns` (option, off): a HyperLogLog sketch of a column in every unit, for `sift.approx_count_distinct` | `ALTER INDEX x RESET (hll_columns)` and `REINDEX` (the setting does not apply: the option stamps the minor) |
| 21 | `order_array_columns` (option, `auto` from 3.5: the build chooses the scattered columns of a table of 100,000 rows or more), `order_array_group` (option, off): a global order array of the keys of a segment | `ALTER INDEX x SET (order_array_columns = 'off')` (and `RESET (order_array_group)`) and `REINDEX`; the setting does not apply: the option stamps the minor |
| 22 | `key_table` (option, on) of an index, `sift.table_key_table` (setting, on) of a `sift_table`: the key table of a leaf of `key_columns`, or of a foreign key or `*_id` column of a table, in a segment | `ALTER INDEX x SET (key_table = off)` and `REINDEX`; for a table `SET sift.table_key_table = off` and `VACUUM FULL t`; before the older files are installed (a setting below 15 also suppresses key tables) |
| 23 | `key_columns` lists of runs of consecutive keys (on when it pays) | `ALTER INDEX x SET (key_columns = 'off')` and `REINDEX`, before the older files are installed (a 23 segment makes 3.4 refuse the index) |
| 24 | `count_columns` (option, on for a text leaf read in place): the rows of each string of a text leaf's dictionary | `ALTER INDEX x SET (count_columns = 'off')` and `REINDEX`, before the older files are installed (a 24 segment may also hold key tables: `key_table = off` too, for 3.4) |

So `WITH (cube_minmax = false, superunit = 0, calendar_columns = 'off')` at creation, or `sift.col_format_minor = 6` for the server (`ALTER SYSTEM`, as in "Going back to 3.2") or the database, writes what 3.2 reads (4 for a schema
without JSON paths, 6 with them; 4 for 3.1, 3 for 3.0 with the setting at 4 or 3). An index with wide leaves (column format 5) is written for 3.2 and later at every setting.

A session setting (`SET`), or one on an application role, is not enough while `sift.col_auto_merge` is on (the default since 3.3): the background worker that rewrites a `sift_col` index runs in its own process, as the bootstrap superuser, and follows only the server and database settings, so it would write the default 15 into an index that 3.2 must read. Use the server setting when no 3.3 write may go beyond 3.2, or `sift.col_auto_merge = off` while the session setting is the one in force.

### Going back to 3.2

Run this sequence on the 3.3 database while the 3.3 library still runs (`test/t/compat_matrix.sh`, "3.2 downgrade", runs it end to end on a database holding an index of every minor above, a `sift_table` with calendar cells,
one with a 5 MB row and a BM25 index, then starts the 3.2.0 library on it):

```

# 1. what the target cannot read: one blocker row per object, with the advice
scripts/upgrade-check.sh --to 3.2 app

# 2. every write from now on is readable by 3.2 (superuser; new sessions pick it up)
psql -d postgres -c "ALTER SYSTEM SET sift.col_format_minor = 6" -c "SELECT pg_reload_conf()"

#    with DISABLE (the 3.2 library has not the function of the two event triggers of 3.3, and would fail every ALTER TABLE and CREATE TRIGGER of the database)
psql -d app -c "REINDEX INDEX CONCURRENTLY public.facts_col"
psql -d app -c "VACUUM FULL public.orders"
psql -d app -c "ALTER EVENT TRIGGER sift_table_ddl_check DISABLE" -c "ALTER EVENT TRIGGER sift_table_repack_guard DISABLE"

# 4. nothing blocks any more (exit status 0; the warning that the setting is below the default is the point)
scripts/upgrade-check.sh --to 3.2 app

# 5. stop the server, install the 3.2 files, start it; then check
psql -d app -c "SELECT sift.col_verify('facts_col', true)"       # each sift_col index
psql -d app -c "SELECT sift.verify('docs_idx', true)"            # each sift index
```

- Keep `sift.col_format_minor = 6` while any 3.3 library still writes: a write at the default after step 4 would put a section 3.2 cannot read back into the database.
- `VACUUM FULL` takes an `ACCESS EXCLUSIVE` lock and rewrites the table in the format of the setting, whatever the row size (tested with a 5 MB row). `ALTER TABLE t SET ACCESS METHOD heap` and
  back rewrites it too, but fails with `row is too big` on a row that does not fit a heap page, so the preflight does not name it.
- A `sift` (BM25) index built by 3.7 is at segment format 4 (metapage minor 1.4), which 3.2 refuses with the format error and the hint to `REINDEX` (no wrong answer; `scripts/upgrade-check.sh --to 3.2` lists it as a blocker). A 3.3 to 3.6 index is at minor 1.3, the one 3.1 and 3.2 write (tested: built with 3.3, verified and answered the same by 3.2). There is no setting that makes 3.7 write the older format, so the way back is the `REINDEX` of the index by the older library after the downgrade (tested: it verifies and answers as before). A word over 255 bytes is
  stored as a hash since 3.3 and cut at 255 bytes by 3.2 (section 2): `REINDEX` such an index with 3.2 if a query for the long word must find it.
- The catalog keeps the extension at 3.3 (below). The access methods, `sift.col_verify`, `sift.verify`, the other functions 3.2 has, and writes, promotion, merge and `REINDEX` by the 3.2 library
  work on the downgraded database (tested).
- A standby that stays on 3.2 needs steps 2 and 3 on its primary, and `sift.col_format_minor` set there for every later write.
- **`point_tid_columns` (minor 16).** 3.2, 3.1 and 3.0 refuse minor 16 segments with a format error and the `REINDEX` hint (tested; no answer is ever wrong). Their planner falls back to other plans
  only for an index it can open, so a refused index fails the query until rewritten. The setting of step 2 (6, 4 or 3) writes no point directory, so the `REINDEX` of step 3 drops the heap addresses.
  The index keeps the option in `pg_class`: a 3.2 library ignores it when reading (tested), but a restore into 3.2 reports `unrecognized parameter`. Roll back: `ALTER INDEX x RESET (point_tid_columns)`
  (before the dump, if any), then the `REINDEX` of step 3. Go forward again: `ALTER INDEX x SET (point_tid_columns = 'col')` and `REINDEX INDEX x` with the setting reset (the option does not apply to
  segments written before it). `REINDEX`, `sift.col_refresh` and 3.3 merges keep the option and write minor 16 again (tested).
- **`key_cut` (minor 17).** Same refusal by 3.2, 3.1 and 3.0 (tested; no answer is ever wrong). The setting of step 2 (14 or less, 6, 4 or 3) writes superblock units again, so the `REINDEX` of step 3 drops
  the cut; the option stays in `pg_class` with the same restore message. Roll back: `ALTER INDEX x SET (key_cut = 'off')` (a `RESET` lets the next `REINDEX` decide again) and `RESET (key_cut_rows)`, before the dump, then the `REINDEX`. Go forward again:
  `ALTER INDEX x SET (key_cut = 'col')` and `REINDEX INDEX x` with the setting reset. `REINDEX`, `sift.col_refresh` and merges keep the option and write minor 17 again (tested).
- **`hll_columns` (minor 20).** 3.3 and older refuse minor 20 segments with a format error and the `REINDEX` hint. `sift.upgrade_check('3.3')` lists the index as a blocker with
  `ALTER INDEX x RESET (hll_columns)` and the `REINDEX`; `sift.approx_count_distinct` exists only from 3.4, so a query that calls it fails on an older library whatever the index. It and
  `count(DISTINCT)` are different aggregates: no setting turns one into the other.
- **`order_array_columns` (minor 21).** 3.3 and older refuse minor 21 segments with a format error and the `REINDEX` hint. `sift.upgrade_check('3.3')` lists the index as a blocker with
  `ALTER INDEX x SET (order_array_columns = 'off')` (and `RESET (order_array_group)`) and the `REINDEX`; from 3.5 a `CREATE INDEX` or `REINDEX` of a table of 100,000 rows or more may choose columns (`auto`), so a rollback to 3.3 sets it to `off` before the `REINDEX`.
- **`key_table` (minor 22, on by default).** A 3.4 library refuses a segment of minor 22 with the format error (`index "x" has a segment of column format 1.22, this build reads 1.3 to 1.21`) and the `REINDEX` hint, in a scan, `VACUUM`, `sift.col_verify` and `sift.col_segments`; an `INSERT` into the table fails with `invalid directory update: unknown segment format`; nothing crashes (checked with the 3.4.2 library on a database written by 3.5). The error is SQLSTATE 55000 (`HINT:  Use a newer pg_sift`) and comes from opening the index in the planner, so every statement on the table fails, even a plain `select count(*)`, not only the scans of sift; the server stays up. `DROP INDEX x` makes the table usable again (tested with the 3.4.2 library). A `REINDEX` on the 3.4 library rebuilds the index without key tables (it does not know the option), so it fixes it too; an index written with `key_table = off` is read by 3.4.2 without error. `sift.upgrade_check('3.4')` lists the index as a blocker with
  `ALTER INDEX x SET (key_table = off)` and the `REINDEX` (for a `sift_table`: `SET sift.table_key_table = off` and `VACUUM FULL t`, which rewrites the segments without tables); run both on the 3.5 library, before the files of the older release are installed (an older library does not know the option, but ignores it in `pg_class`, as it ignores the others in this list). Plan the way back before the update: an index whose segments got key tables cannot be read by 3.4 again (segments too small for one stay at minor 15). The setting does not apply: `sift.col_format_minor` is 15 at most, and a table is written under 15 only. A segment written under a lower setting has none.
- **`count_columns` (minor 24, on for a text leaf read in place).** A 3.4 library refuses a segment of minor 24 as it does minor 22: the format error (`index "x" has a segment of column format 1.24, this build reads 1.3 to 1.21`) with the `REINDEX` hint, in a scan, `VACUUM`, `sift.col_verify` and `sift.col_segments`, and an `INSERT` fails with `invalid directory update: unknown segment format`. `sift.upgrade_check('3.4')` lists the index as a blocker with the advice `ALTER INDEX x SET (count_columns = 'off', key_columns = 'off', key_table = off)` and `REINDEX`. An index has such a segment when a text leaf with 1,024 strings or more is read in place (`point_columns`, `point_tid_columns`, `key_columns`, `return_columns` or `dict_lookup = on`) and `count_columns` is not `off`.
- **The hash of the dictionaries (minor 25, on for a text or uuid leaf read in place).** A 3.5 library refuses a segment of minor 25 with the format error (`index "x" has a segment of column format 1.25, this build reads 1.3 to 1.24`) and the `REINDEX` hint, as it refuses minor 24 in a 3.4 library; `sift.upgrade_check('3.5')` lists the index as a blocker with the advice `SET sift.col_format_minor = 14`, then `REINDEX INDEX x`. There is no option of its own: every dictionary of 1,024 strings or more written in the lookup form (`point_columns`, `point_tid_columns`, `key_columns`, `return_columns` or `dict_lookup = on`) has the hash, so a rollback sets the setting for the `REINDEX` (it also leaves out the sections of minors 15 to 24) and an update to 3.6 leaves the older segments as they are until a merge or `REINDEX` writes them again.
- For 3.1 use 4 and for 3.0 use 3 in step 2, and `--to 3.1` or `--to 3.0` in steps 1 and 4 (the matrix test checks that those releases read what the settings 4 and 3 write, not the full sequence).

### After the downgrade: the catalog, pg_dump and the way forward

- **The catalog stays at 3.3.** There is no script from 3.3 to 3.2: `ALTER EXTENSION pg_sift UPDATE TO '3.2'` fails with `extension "pg_sift" has no update path from version "3.3" to version "3.2"`,
  and `DROP EXTENSION .. CASCADE` would drop every index and `sift_table` that uses it. What 3.3 added to the SQL stays and is unusable with the 3.2 library: its functions (`sift.upgrade_commands`,
  `sift.stats`, `sift.explain_why`, `sift.worker_state`, `sift.table_space`, ...) and the two event triggers fail with `could not find function "..." in file ".../pg_sift.so"`. Hence the `DISABLE`
  of step 3: enabled, they make every `ALTER TABLE` and `CREATE TRIGGER` of the database fail, and the preflight lists them as blockers for a target before 3.3. The tables and indexes,
  `sift.version()`, `sift.col_verify` and `sift.verify` work (tested).
- **pg_dump and a server that has the 3.2 files only.** The dump holds `CREATE EXTENSION pg_sift` without a version, so the restore creates the extension at the version of the files it finds (3.2); the
  tables are restored with their rows and the 3.2 library builds every index again in its own format (no setting is needed, no 3.3 or 3.4 section is left). A `CREATE INDEX` with an option that 3.3 or
  3.4 added (`cube_minmax`, `superunit`, `calendar_columns`, `dict_lookup`, `rollup_columns`, `key_columns`, `order_columns`, `point_columns`, `point_tid_columns`, `key_cut`, `key_cut_rows`) fails with
  `unrecognized parameter`, and the restore goes on without that index. A `sift_table` with a row too large for a write buffer page (3.3 stores it, 3.2 refuses it with
  `row of sift_table "t" is too large`) restores no row at all: its `COPY` fails as a whole. These are the only errors (tested); a dump without them restores clean, and an index without an option is built at the 3.2 defaults. Reset the option before the dump
  (`ALTER INDEX x RESET (option)`) or edit the statement, and keep such a table out of a dump for 3.2.
- **pg_dump of a catalog at 4.2 under an older library.** 4.2 added `sift.table_option_values`, the per-table options of a `sift_table`, whose key type `sift.tblref` has its input and output functions in the 4.2 library only:
  `pg_dump` fails with `could not find function "sift_tblref_out"` while it copies that table out (even when it is empty). Leave its data out, `pg_dump --exclude-table-data=sift.table_option_values`, and set the options again
  with 4.2 (`sift.table_options()` lists them before the downgrade); the options are the only thing a downgrade to a release before 4.2 loses.
- **Going forward again.** Install the 3.3 files and restart. A database downgraded in place is at 3.3 in the catalog already: `ALTER EXTENSION pg_sift UPDATE` has nothing to do. Run
  `ALTER EVENT TRIGGER sift_table_ddl_check ENABLE` and `ALTER EVENT TRIGGER sift_table_repack_guard ENABLE`, then `ALTER SYSTEM RESET sift.col_format_minor` and `SELECT pg_reload_conf()` so writes carry
  the 3.3 sections again; what was rewritten at minor 6 stays at 6 until a `REINDEX` or `VACUUM FULL` (the preflight without `--to` and `sift.upgrade_commands()` list them). A database restored at 3.2
  takes the update of section 2. The matrix test runs both.

Every `blocker` row of the preflight says how to write what the release reads:

| Object | Advice |
|---|---|
| `sift_col` index | `SET sift.col_format_minor = N` (3 for 3.0, 4 for 3.1, 6 for 3.2; 3.3 reads every minor up to 16, 3.4 up to 21, 3.5 up to 24, 3.6 up to 32, 3.7 up to 39 and 4.0 up to 40; the default setting is 15, and minors 16, 17, 18, 20 and 21 are written only for an index with `point_tid_columns`, `key_cut` or `order_columns`, `hll_columns` or `order_array_columns`, minor 22 for an index with `key_columns` leaves unless `key_table = off` and for a `sift_table` with a foreign key or `*_id` column unless `sift.table_key_table = off`, minor 24 for an index with `count_columns`, minor 25 for a lookup dictionary of 1,024 strings or more, and 19 only for a leaf NULL in nearly every row) and `REINDEX INDEX x`; an index with wide leaves (column format 5) is written for 3.2 and later at every setting: drop it for 3.1 |
| `sift_table` | with `sift.col_format_minor` set, `VACUUM FULL t` rewrites it in that format (`ALTER TABLE t SET ACCESS METHOD heap` and back fails with `row is too big` on a row over the heap page limit) |
| `sift` (BM25) index | the segment format is the library's (3.7 writes format 4, which 3.6 and older refuse with the format error): drop the index and create it again with the older release, or keep the newer library |

## Defaults changed in 3.3

An existing index or table keeps what it holds; the new default applies to units written after the update (promotion, merge, `REINDEX`), and `sift.upgrade_commands()` lists the `REINDEX` that gives older units the new sections. To keep the 3.2 behavior set the value in the last column. `sift.table_auto_defrag` is read from the library, not the extension script: it is on for every `sift_table` of the cluster as soon as the 3.3 library runs (after the restart), whatever the extension version, and the preflight of section 1 warns about the `off` that keeps 3.2, as for any feature switched off.

| Setting | 3.2 | 3.3 | Measured (200,000 rows, 53 units) | To keep the 3.2 behavior |
|---|---|---|---|---|
| `sift_col` option `cube_minmax` | off | on | `GROUP BY` with `min`/`max` of an integer 5.5 -> 0.34 ms; index +0.17% | `WITH (cube_minmax = false)` |
| `sift_col` option `superunit` | 0 | 16 | whole-table aggregates 4 to 7 times faster; index +1.0% | `WITH (superunit = 0)` |
| `sift.table_auto_defrag` (server setting, with `sift.async_maintenance`, on in 3.2 too) | off | on | four clients, update/insert/delete mix on 200,000 rows: 7,536 against 5,092 transactions a second, range read p99 16.8 against 34.2 ms; a table that only appends starts no pass | `sift.table_auto_defrag = off` |
| `sift.col_auto_merge` (server setting, with `sift.async_maintenance`, on in 3.2 too) | off | on | 1M rows, 3% of the rows changed per round: the units answerable from their pre-aggregates 32% to 51% (wide rows), 33% to 83% (mixed workload); total WAL +0.9% to +1.9% (full-page images on, a checkpoint per round), +0.0% with them off (the rewrite then waits for the WAL: none in 20 rounds); applies to the existing `sift_col` indexes as soon as the new library is loaded, and the CREATE INDEX of a new one says so in a NOTICE | `sift.col_auto_merge = off` |
| `sift_col` option `key_columns` | none | `auto`: the foreign key and `_id` columns of type int2, int4, int8 or date that are not unique and not a cube dimension, where their key lists or bitmaps fit the 3% of a unit; CREATE INDEX and REINDEX say which in a NOTICE | `count(DISTINCT fk)` 2.0 to 8.6 times faster on the foreign keys of three 200,000-row tables; index +1.1% to +1.4% | `WITH (key_columns = 'off')`, or `sift.col_format_minor = 14` |
| `sift_col` option `calendar_columns` | none | all date, timestamp and timestamptz columns | `date_trunc` / `extract` group-bys of a date 3.7 to 4.5 times faster; index +1.8% for two columns | `WITH (calendar_columns = 'off')` |
| `sift.table_wide_rows` | off | on | a row over the write buffer is accepted (up to 1 GB) instead of refused; no cost for rows that fit | `SET sift.table_wide_rows = off` |
| `sift.table_bulk_groups` | off | on | `COPY` of 200,000 rows: CPU -25%, WAL 44 -> 1 MB | `SET sift.table_bulk_groups = off` |

## 5. First release that reads each format

| Format | First release |
|---|---|
| BM25 metapage minor 1.2 (segment formats 1 and 2) | 3.0 |
| BM25 metapage minor 1.3 (segment format 3) | 3.1 |
| BM25 metapage minor 1.4 (segment format 4, the position stream in blocks) | 3.7 |
| column format 3 | 3.0 |
| column format 4 (tags and kinds, version section of a `sift_table`) | 3.1 |
| column format 5 (wide leaves), 6 (complete JSON kinds) | 3.2 |
| column format 7 (`cube_minmax`), 8 (the calendar cells of a date or timestamp in the cube of a `sift_table`, or of a `sift_col` index with `calendar_columns`), 9 (`rollup_columns`), 10 (`key_columns`, a summary of the keys), 11 (the write buffer of a `sift_table` with a row on tail pages), 12 (`superunit`), 13 (`order_columns`), 14 (a string dictionary of 1,024 strings or more, stored in blocks to be read in place), 15 (`key_columns`, a key list or bitmap) | 3.3 |
| column format 16 (`point_tid_columns`) | 3.3 |
| column format 17 (`key_cut`), 18 (heads of the sorted runs), 19 (the exceptions of a NULL-heavy leaf), 20 (`hll_columns`), 21 (`order_array_columns`) | 3.4 |
| column format 22 (`key_table`, the key table of a segment) | 3.5 |
| column format 24 (`count_columns`, the row counts of a text dictionary) | 3.5 |
| column format 25 to 32 (the hash of the lookup dictionaries, the containment lanes of range, inet and cidr columns, the text signatures, the finer global order of `order_array_columns` for dates and text) | 3.6 |
| column format 33 to 39 (the point directories at `ceil(keys * 4 / 3)` slots, the chunk table before the directory, the rollup lengths, the directory records at the end of the dictionary section, the packed order of a dictionary searched in place) | 3.7; 3.6 and older refuse them |
| column format 40 | 4.0; 3.7 and older refuse the segments; a list in `order_array_columns` keeps complete arrays |
| column format 41 (a compact chunk in an order array; `order_layout` auto, the default, writes it for spread keys such as a timestamp) | 4.0; 3.7 and older refuse the segments; `order_layout = 'whole'` and a REINDEX, or `sift.col_format_minor` 14, write what they read |
| column format 42 (the order array of a text column names its strings by dictionary code) | 4.0; 3.7 and older refuse the segments; `sift.col_format_minor` 14 and a REINDEX write what they read |
| column format 44 (the key journal of a connected parent index and the found flags of a connected child, a write buffer of references to heap tuples, seekable and positional order chunks and the packed table of groups of the second order) | 4.3; 4.2 and older refuse the segments; `sift.col_format_minor` 14 and a REINDEX write what they read, with `connect = 'off'` and `sift.col_wbuf_ref = off` for the connection and the references |
| column format 45 (a scope index: the derived key of a scope and its scope segments) and 49 (a scope of two paths: three stamps in a segment) | 5.0; 4.3 and older refuse the segments; `ALTER INDEX x RESET (scope)` and `REINDEX` write the entities they read |
| column format 46 (strings held by a shared value store) | 5.0; 4.3 and older refuse the segments and the store; `SET sift.col_value_store = off` and `REINDEX` write the strings in the segments (minor 44 at the most), `sift.col_format_minor` 14 does it for a release before 4.3 |

`scripts/upgrade_check.sql` holds this table; a release that adds a format adds its row there.

## Schema `sift` is open to PUBLIC since 3.3

Up to 3.2 a role needed `GRANT USAGE ON SCHEMA sift` to call any `sift.*` function or read a `sift.*` view (`permission denied for schema sift`). The install and `ALTER EXTENSION pg_sift UPDATE` to 3.3 grant it to `PUBLIC`: existing grants stay and are no longer needed, and nothing else is granted (the functions check the table or index they read; `sift.table_paths` is not readable). A database that does not want it runs `REVOKE USAGE ON SCHEMA sift FROM PUBLIC` after the update; the next update grants it again. PostgreSQL creates the schema named in the control file but does not make it a member of the extension (`ALTER EXTENSION pg_sift ADD SCHEMA sift` is refused), so `DROP EXTENSION` leaves it empty, as in 3.2, with the privileges it had; `CREATE EXTENSION` then gives the objects and privileges of a fresh install again, and `DROP SCHEMA sift` removes it.

## Known issue of 3.2.0, 3.2.1 and 3.2.2: a role cannot drop anything

In 3.2.0, 3.2.1 and 3.2.2 the event trigger function `sift.table_paths_drop()` is not `SECURITY DEFINER`, so a `DROP` by a role that is neither a superuser nor the extension owner fails with `permission denied for schema sift` (any table, index or schema, also one without pg_sift). `ALTER EXTENSION pg_sift UPDATE` to 3.3 fixes it. Until then, run once in each database with the extension, as a superuser (and again after a restore from a dump, which creates the extension from the 3.2 script):

```sql
ALTER FUNCTION sift.table_paths_drop() SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
```

## Known issue of 4.0.0 to 4.1.0: aggregates could count old versions of rows

Fixed in 4.1.1 (install the files and restart the server; nothing to update in the database). In 4.0.0, 4.0.1, 4.0.2, 4.0.3 and 4.1.0 an aggregate over a `sift_col` index with `track_changes` (the default for a table with a primary key) could count the old versions of rows that a transaction had updated more than once: the first statement of a new session that needed a table the backends share (the dictionary of a text or varchar key, the statistics of the index, the choices of the planner) was a write (`UPDATE`, `INSERT ... ON CONFLICT DO UPDATE`, `DELETE`, `COPY`), the shared memory was attached inside a subtransaction, and the command id advanced under that statement. Reproduced on 4.0.0, 4.0.2, 4.0.3 and 4.1.0 (4.0.1 shares the code) with a text key; the cases with an integer key did not show it. Before 4.0.0 a read checks the heap, so 3.x does not show it.

A second form: an `UPDATE`, `MERGE`, `INSERT ... ON CONFLICT DO UPDATE` or a cursor `UPDATE ... WHERE CURRENT OF` that calls a function that runs SQL inside the statement (a plpgsql function with `PERFORM` or `EXECUTE`, a trigger that updates the table) advanced the command id inside the statement, and the entry of each version the statement ended was not removed after the first call, so the rows were counted twice. A third: a bulk `UPDATE` of a table that also has a `sift` (BM25) index, which seals its segments in a subtransaction of the statement. Confirmed on 4.1.0 and expected on every release since 4.0.0.

If you cannot update yet: `ALTER INDEX <index> SET (track_changes = 'off')` on each `sift_col` index of a table with a primary key or another tracked key. It takes a `ShareUpdateExclusiveLock`, rewrites nothing, needs no `REINDEX` (35 ms on 600,000 rows, and about 0.1 s for the `SET (track_changes = 'id')` that turns it back on), avoids all three routes, and repairs an index that was already wrong because every row is then checked against the heap; `REINDEX` alone repairs it only until the next such statement. The price: on a table that has churned and was not vacuumed, the statements after an update check more rows in the heap (the instructions of the next statement rose by 27% to 53% in the cases measured), nothing after a `VACUUM`; writes get cheaper; the first statement of a session is the same. After the update, `ALTER INDEX <index> SET (track_changes = 'id')` turns the tracking back on. For the first route only, `ALTER SYSTEM SET sift.col_dict_shared_cache_kb = 0` (with `sift.col_leaf_cache_kb = 0` and `sift.verdict_cache_kb = 0`), then `SELECT pg_reload_conf()` (or the same in `postgresql.conf`, or `ALTER DATABASE .. SET`; superuser settings, no restart) also avoids it: each backend then parses its own dictionaries and keeps its own statistics (what [docs/OPTIONS.md](OPTIONS.md) gives for these settings). Putting pg_sift in `shared_preload_libraries` does not avoid it. See also [Reading without the heap (track_changes)](USAGE.md).

### The wrong counts that 4.0.0 to 4.1.0 left in an index, and 4.1.2

Installing 4.1.1 stopped new wrong counts; it did not take away the old versions that an earlier release had left in the log of an index, so such an index went on counting them after the update (`ALTER EXTENSION UPDATE` changes nothing there; reproduced from 4.0.3 and 4.1.0 to 4.1.1 and 4.2 with the three routes above), until a `REINDEX`, `ALTER INDEX .. SET (track_changes = 'off')` and back, or a `VACUUM` that vacuumed the indexes (PostgreSQL skips that when fewer than 2% of the pages hold a dead row: 30 stale entries of a 300,000-row table stayed through a plain `VACUUM`). 4.1.2 repairs them by itself, with the files installed and the server restarted and no other step:

- The log of a `sift_col` index with `track_changes` that an earlier release wrote (flags 1 in its header) is not taken for complete any more: its reads check the heap, so their answers are the heap's at once (`sift.col_tracking` shows `complete` true and `heap_free` false). The cost is that of an index with `track_changes = 'off'` on a table that has churned and was not vacuumed (600,000 rows, 30,000 each updated, deleted and inserted, the second statement of a session, millions of instructions, heap-free against checked: one entity 1.5 against 2.3, a set of 75 entities 5.6 against 7.2, the full group 57.6 against 73.4), nothing on a vacuumed table.
- It is re-baselined by the first of: the next `VACUUM` of the table (also one that skips the indexes), the first read that checks the heap for it (the maintenance worker, `sift.col_auto_merge` on, so a table nobody writes or vacuums is repaired too), and the maintenance worker that a write of the table asks for. The step writes a tombstone for every heap address that no version is live for, marks the log (flags 3), and the heap-free reads resume. 600,000 rows, 30,000 of them updated, deleted and inserted since the last VACUUM: 111 to 119 ms and 7 to 14 MB of WAL; after a VACUUM 71 ms and 3 kB. It holds a `ShareUpdateExclusiveLock` on the table (writers go on, a VACUUM waits or skips). The re-baseline of an index that was fine changes nothing in its answers.
- A standby cannot write the log: its reads check the heap until the re-baseline of the primary reaches it by replication.
- An earlier library that reads a log with flags 3 takes it for incomplete and checks the heap, which is right.
- An index created, reindexed or rebuilt by `ALTER INDEX .. SET (track_changes = ..)` by 4.1.2 or later is marked at once.
- **After `pg_upgrade`, a dump restore with triggers disabled, or `REINDEX CONCURRENTLY`, nothing is repaired by itself.** The restore recreates the triggers with a new xmin, so the witnesses of the log no longer match: every tracked `sift_col` index has flags 1, `triggers` false and `heap_free` false (damaged or not; since 4.0.0), the worker, `VACUUM` and writes change nothing, and the answers are right, taken from the heap. Run `REINDEX INDEX <index>` or `ALTER INDEX <index> SET (track_changes = '<value>')` (the value the index had: its key column, `'auto'` or `'id'`) once for every tracked `sift_col` index, one by one; the indexes that are still to do:

```sql
SELECT c.oid::regclass AS index FROM pg_class c JOIN pg_am a ON a.oid = c.relam, LATERAL sift.col_tracking(c.oid) t WHERE a.amname = 'sift_col' AND t.track_changes AND NOT t.heap_free;
```

From 4.2 on the maintenance worker, `VACUUM` or a read that has to check the heap repairs it by itself (see the pg_upgrade section above): nothing to run.

## Known issue of 4.0.0 to 4.3.2: a concurrent writer at a promotion left reads on the heap

Fixed in 5.0; no patch release is planned. In 4.0.0 to 4.3.2 a `sift_col` index with `track_changes` that promoted its write buffer while a transaction that had inserted some of the rows was still open wrote the segment without its stamp, and a merge that took such a segment in wrote its output without one too. A read of an unstamped segment decides its rows from the heap, not from the tombstone log, so the reads of an index under concurrent writers cost more (the heap pages of its rows) until a rewrite of the index (`REINDEX`, or a rewrite of those segments). **Answers were never wrong**: an unstamped segment is the safe side, and the three reads (heap-free, heap-checked, no sift plan) agree in every state of an inserter (open, committed, rolled back, subtransactions, prepared).

5.0 keeps the wait in the directory entry of the segment (no new format minor; an older library reads it as an unstamped segment) and turns it into the stamp at the next maintenance step or merge once the transactions it waited for have committed, so install the files and restart; nothing to update in the database. A segment promoted by an older release stays unstamped until its next rewrite (`REINDEX`).

A limit that stays: a transaction that **rolled back** after the promotion that waited for it leaves its segment unstamped until a rewrite of the segment, and so do transactions in doubt more than 63 transaction ids apart or more than 64 distinct ones. The rows a read returns are the heap's in every case.

## What is tested

| What | Test |
|---|---|
| the preflight on a clean database, with `--to`, with options, settings and test objects left behind, on an older extension and on an index the library refuses | `test/t/upgrade_check.sh` |
| `pg_upgrade --check` and the upgrade of `sift`, `sift_col` and `sift_table` objects (tablespace, wide leaf types, JSON paths, open write buffers), answers, `sift.verify`, `sift.col_verify`, writes and merges afterwards | `test/t/pg_upgrade.sh` |
| a same-major `pg_upgrade` of the BM25 indexes, `ALTER EXTENSION UPDATE` along every release | `test/t/extension_lifecycle.sh` |
| the previous releases' libraries write, this tree reads (and the other way) | `test/t/old_lib_refusal.sh`, `test/t/compat_31_column.sh`, and `scripts/release.sh` |
