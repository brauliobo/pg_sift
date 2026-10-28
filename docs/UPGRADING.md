# Upgrading

What to run, in order, to move a database to a newer pg_sift, to a newer PostgreSQL major, or back to an older pg_sift. On-disk formats and the
first release that reads each: [FORMATS.md](FORMATS.md).

| Change | What happens to the files | Steps |
|---|---|---|
| a newer pg_sift (patch or minor) | the new library reads the files of the older ones; it writes its own format from the next write on | [preflight](#1-preflight), [install and update](#2-install-and-update) |
| 3.2.1 to 3.2.2 | library only: the extension SQL stays at 3.2, no format changes | install the files and restart the server (an open session keeps the old library, and pg_sift in `shared_preload_libraries` is read at start only); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back to 3.2.1 needs nothing ([notes](releases/v3.2.2.md)) |
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
| `option` | options that only units written afterwards follow | `warn`: `cube_minmax`, `rollup_dims`, `key_columns`, `order_columns`, `point_columns`, `point_tid_columns`, `key_cut` or `hll_columns` on an index with older units, JSON `paths` on segments before column format 6 (the advice is the command from `sift.upgrade_commands()`, which an extension older than 3.3 has after its update; a `sift_table` below the format of its schema is a `table` row with `VACUUM FULL`), the deprecated `mutable_limit` |
| `setting` | `sift.*` in the server configuration, the session, `ALTER DATABASE` and `ALTER ROLE` | `warn`: a setting the library does not define (removed or renamed), a feature switched off (a boolean setting that defaults on), `sift.col_format_minor` kept below the default |
| `trigger` | with `--to` a release before 3.3: the event triggers of 3.3 | `blocker`: the older library has not their function and would fail every `ALTER TABLE` and `CREATE TRIGGER`; `ALTER EVENT TRIGGER x DISABLE` |
| `test` | test-only settings and the extension `pg_sift_test` | `warn`: remove them; they need the test build of the library |

Run it before and after the update: the second run must have no `warn` the first one explained.

## 2. Install and update

```

# 1. install the files of the new release (package, or from source)
sudo apt install ./postgresql-N-pg-sift_X.Y.Z-1_amd64.deb
make USE_PGXS=1 && sudo make USE_PGXS=1 install

# 2. preflight (section 1), then in every database that has the extension
psql -d app -c "ALTER EXTENSION pg_sift UPDATE"

# 3. restart the server, so that every process runs one library (new sessions load the new file by themselves)
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

The last step of an update to 3.3 (from any older version) prints what is left to do, as warnings with the commands (the first 20 of each, then the count of the rest), or a
notice that nothing is (PostgreSQL shows only warnings from an extension script). `SELECT * FROM sift.upgrade_commands()` lists the same later, for the indexes and tables the role owns or may maintain (every one for a superuser):
one row per command (`kind` `reindex` or `rewrite`, the `object`, the `reason`, the `command`), the cheapest first.

| Warning | Why | Command |
|---|---|---|
| a `sift` (text) index needs a `REINDEX` | **queries miss rows**: since 3.3 a word over 255 bytes is stored as its first 236 bytes and a hash; an older index holds it cut at 255 bytes, so a query for it finds nothing. The index does not record whether it holds such a word, so every text index except `analyzer = keyword` is listed, also after its `REINDEX` | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_col` index would gain from a `REINDEX` | an option set after the units were written (`cube_minmax`, JSON `paths`, `rollup_dims`, `key_columns`, `order_columns`, `point_columns`, `point_tid_columns`, `key_cut`, `hll_columns`): older units stay readable and are scanned (for `order_columns`, read whole by an `ORDER BY .. LIMIT`) until then | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_table` would gain from a rewrite | segments below the column format of its schema: no calendar cells for its date and timestamp columns (format 8), JSON kinds that may be incomplete for declared paths (format 6). A `sift_table` cannot be reindexed | `VACUUM FULL schema.table;` |

Only the first warning means wrong results without the command. `VACUUM FULL` takes an `ACCESS EXCLUSIVE` lock and rewrites the whole table in the format of `sift.col_format_minor`, so
schedule it; once it ran the table leaves the list. `REINDEX CONCURRENTLY` cannot run in a transaction block: run the commands one at a time. The check reads the catalogs and
metapages and cannot fail the update; when it cannot answer (a relation locked by another session) it says so, and `sift.upgrade_commands()` can be run after reconnecting.

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
(`test/t/pg_upgrade.sh`: PostgreSQL 17 to 18 when both are installed, otherwise 18 to 18).

## 4. An older release

**A database written by the 3.3 library with its default settings can no longer be read by a 3.2 library.** An older library refuses a metapage minor or a column format it does not know, with a `REINDEX` hint
(`old_lib_refusal`), and 3.3 writes at `sift.col_format_minor = 15` by default: a `sift_col` index created or rewritten after the update carries sections 3.2 does not know, and so does a `sift_table` with a date or timestamp
column. Only what was written with the settings below stays at minor 4 or 6, the highest a 3.2 library reads (a segment is stamped with the lowest minor that describes its content):

| Minor | Written when | Keeps the 3.2 bytes |
|---|---|---|
| 7 | a `sift_col` index with `cube_minmax` (on by default) | `WITH (cube_minmax = false)`, or `sift.col_format_minor` 6 or less |
| 8 | the calendar cells of a date, timestamp or timestamptz column: a `sift_col` index (`calendar_columns` defaults to all of them), a `sift_table` | `WITH (calendar_columns = 'off')`; a `sift_table`: `sift.col_format_minor` 7 or less |
| 9 | `rollup_dims` (option, off) | do not set it, or `sift.col_format_minor` 8 or less |
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
- A `sift` (BM25) index needs nothing for 3.2: its metapage minor (1.3) is the one 3.1 and 3.2 write (tested: built with 3.3, verified and answered the same by 3.2). A word over 255 bytes is
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
- **`key_table` (minor 22, on by default).** A 3.4 library refuses a segment of minor 22 with the format error (`index "x" has a segment of column format 1.22, this build reads 1.3 to 1.21`) and the `REINDEX` hint, in a scan, `VACUUM`, `sift.col_verify` and `sift.col_segments`; an `INSERT` into the table fails with `invalid directory update: unknown segment format`; nothing crashes (checked with the 3.4.2 library on a database written by 3.5). A `REINDEX` on the 3.4 library rebuilds the index without key tables (it does not know the option), so it fixes it too; an index written with `key_table = off` is read by 3.4.2 without error. `sift.upgrade_check('3.4')` lists the index as a blocker with
  `ALTER INDEX x SET (key_table = off)` and the `REINDEX` (for a `sift_table`: `SET sift.table_key_table = off` and `VACUUM FULL t`, which rewrites the segments without tables); run both on the 3.5 library, before the files of the older release are installed (an older library does not know the option, but ignores it in `pg_class`, as it ignores the others in this list). The setting does not apply: `sift.col_format_minor` is 15 at most, and a table is written under 15 only. A segment written under a lower setting has none.
- **`count_columns` (minor 24, on for a text leaf read in place).** A 3.4 library refuses a segment of minor 24 as it does minor 22: the format error (`index "x" has a segment of column format 1.24, this build reads 1.3 to 1.21`) with the `REINDEX` hint, in a scan, `VACUUM`, `sift.col_verify` and `sift.col_segments`, and an `INSERT` fails with `invalid directory update: unknown segment format`. `sift.upgrade_check('3.4')` lists the index as a blocker with the advice `ALTER INDEX x SET (count_columns = 'off', key_columns = 'off', key_table = off)` and `REINDEX`. An index has such a segment when a text leaf with 1,024 strings or more is read in place (`point_columns`, `point_tid_columns`, `key_columns`, `return_columns` or `dict_lookup = on`) and `count_columns` is not `off`.
- For 3.1 use 4 and for 3.0 use 3 in step 2, and `--to 3.1` or `--to 3.0` in steps 1 and 4 (the matrix test checks that those releases read what the settings 4 and 3 write, not the full sequence).

### After the downgrade: the catalog, pg_dump and the way forward

- **The catalog stays at 3.3.** There is no script from 3.3 to 3.2: `ALTER EXTENSION pg_sift UPDATE TO '3.2'` fails with `extension "pg_sift" has no update path from version "3.3" to version "3.2"`,
  and `DROP EXTENSION .. CASCADE` would drop every index and `sift_table` that uses it. What 3.3 added to the SQL stays and is unusable with the 3.2 library: its functions (`sift.upgrade_commands`,
  `sift.stats`, `sift.explain_why`, `sift.worker_state`, `sift.table_space`, ...) and the two event triggers fail with `could not find function "..." in file ".../pg_sift.so"`. Hence the `DISABLE`
  of step 3: enabled, they make every `ALTER TABLE` and `CREATE TRIGGER` of the database fail, and the preflight lists them as blockers for a target before 3.3. The tables and indexes,
  `sift.version()`, `sift.col_verify` and `sift.verify` work (tested).
- **pg_dump and a server that has the 3.2 files only.** The dump holds `CREATE EXTENSION pg_sift` without a version, so the restore creates the extension at the version of the files it finds (3.2); the
  tables are restored with their rows and the 3.2 library builds every index again in its own format (no setting is needed, no 3.3 or 3.4 section is left). A `CREATE INDEX` with an option that 3.3 or
  3.4 added (`cube_minmax`, `superunit`, `calendar_columns`, `dict_lookup`, `rollup_dims`, `key_columns`, `order_columns`, `point_columns`, `point_tid_columns`, `key_cut`, `key_cut_rows`) fails with
  `unrecognized parameter`, and the restore goes on without that index. A `sift_table` with a row too large for a write buffer page (3.3 stores it, 3.2 refuses it with
  `row of sift_table "t" is too large`) restores no row at all: its `COPY` fails as a whole. These are the only errors (tested); a dump without them restores clean, and an index without an option is built at the 3.2 defaults. Reset the option before the dump
  (`ALTER INDEX x RESET (option)`) or edit the statement, and keep such a table out of a dump for 3.2.
- **Going forward again.** Install the 3.3 files and restart. A database downgraded in place is at 3.3 in the catalog already: `ALTER EXTENSION pg_sift UPDATE` has nothing to do. Run
  `ALTER EVENT TRIGGER sift_table_ddl_check ENABLE` and `ALTER EVENT TRIGGER sift_table_repack_guard ENABLE`, then `ALTER SYSTEM RESET sift.col_format_minor` and `SELECT pg_reload_conf()` so writes carry
  the 3.3 sections again; what was rewritten at minor 6 stays at 6 until a `REINDEX` or `VACUUM FULL` (the preflight without `--to` and `sift.upgrade_commands()` list them). A database restored at 3.2
  takes the update of section 2. The matrix test runs both.

Every `blocker` row of the preflight says how to write what the release reads:

| Object | Advice |
|---|---|
| `sift_col` index | `SET sift.col_format_minor = N` (3 for 3.0, 4 for 3.1, 6 for 3.2; 3.3 reads every minor up to 16, 3.4 up to 21 and 3.5 up to 24; the default setting is 15, and minors 16, 17, 18, 20 and 21 are written only for an index with `point_tid_columns`, `key_cut` or `order_columns`, `hll_columns` or `order_array_columns`, minor 22 for an index with `key_columns` leaves unless `key_table = off` and for a `sift_table` with a foreign key or `*_id` column unless `sift.table_key_table = off`, minor 24 for an index with `count_columns`, and 19 only for a leaf NULL in nearly every row) and `REINDEX INDEX x`; an index with wide leaves (column format 5) is written for 3.2 and later at every setting: drop it for 3.1 |
| `sift_table` | with `sift.col_format_minor` set, `VACUUM FULL t` rewrites it in that format (`ALTER TABLE t SET ACCESS METHOD heap` and back fails with `row is too big` on a row over the heap page limit) |
| `sift` (BM25) index | the segment format is the library's: drop the index and create it again with the older release, or keep the newer library |

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
| column format 3 | 3.0 |
| column format 4 (tags and kinds, version section of a `sift_table`) | 3.1 |
| column format 5 (wide leaves), 6 (complete JSON kinds) | 3.2 |
| column format 7 (`cube_minmax`), 8 (the calendar cells of a date or timestamp in the cube of a `sift_table`, or of a `sift_col` index with `calendar_columns`), 9 (`rollup_dims`), 10 (`key_columns`, a summary of the keys), 11 (the write buffer of a `sift_table` with a row on tail pages), 12 (`superunit`), 13 (`order_columns`), 14 (a string dictionary of 1,024 strings or more, stored in blocks to be read in place), 15 (`key_columns`, a key list or bitmap) | 3.3 |
| column format 16 (`point_tid_columns`) | 3.3 |
| column format 17 (`key_cut`), 18 (heads of the sorted runs), 19 (the exceptions of a NULL-heavy leaf), 20 (`hll_columns`), 21 (`order_array_columns`) | 3.4 |
| column format 22 (`key_table`, the key table of a segment) | 3.5 |
| column format 24 (`count_columns`, the row counts of a text dictionary) | 3.5 |

`scripts/upgrade_check.sql` holds this table; a release that adds a format adds its row there.

## Schema `sift` is open to PUBLIC since 3.3

Up to 3.2 a role needed `GRANT USAGE ON SCHEMA sift` to call any `sift.*` function or read a `sift.*` view (`permission denied for schema sift`). The install and `ALTER EXTENSION pg_sift UPDATE` to 3.3 grant it to `PUBLIC`: existing grants stay and are no longer needed, and nothing else is granted (the functions check the table or index they read; `sift.table_paths` is not readable). A database that does not want it runs `REVOKE USAGE ON SCHEMA sift FROM PUBLIC` after the update; the next update grants it again. PostgreSQL creates the schema named in the control file but does not make it a member of the extension (`ALTER EXTENSION pg_sift ADD SCHEMA sift` is refused), so `DROP EXTENSION` leaves it empty, as in 3.2, with the privileges it had; `CREATE EXTENSION` then gives the objects and privileges of a fresh install again, and `DROP SCHEMA sift` removes it.

## Known issue of 3.2.0, 3.2.1 and 3.2.2: a role cannot drop anything

In 3.2.0, 3.2.1 and 3.2.2 the event trigger function `sift.table_paths_drop()` is not `SECURITY DEFINER`, so a `DROP` by a role that is neither a superuser nor the extension owner fails with `permission denied for schema sift` (any table, index or schema, also one without pg_sift). `ALTER EXTENSION pg_sift UPDATE` to 3.3 fixes it. Until then, run once in each database with the extension, as a superuser (and again after a restore from a dump, which creates the extension from the 3.2 script):

```sql
ALTER FUNCTION sift.table_paths_drop() SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
```

## What is tested

| What | Test |
|---|---|
| the preflight on a clean database, with `--to`, with options, settings and test objects left behind, on an older extension and on an index the library refuses | `test/t/upgrade_check.sh` |
| `pg_upgrade --check` and the upgrade of `sift`, `sift_col` and `sift_table` objects (tablespace, wide leaf types, JSON paths, open write buffers), answers, `sift.verify`, `sift.col_verify`, writes and merges afterwards | `test/t/pg_upgrade.sh` |
| a same-major `pg_upgrade` of the BM25 indexes, `ALTER EXTENSION UPDATE` along every release | `test/t/extension_lifecycle.sh` |
| the previous releases' libraries write, this tree reads (and the other way) | `test/t/old_lib_refusal.sh`, `test/t/compat_31_column.sh`, and `scripts/release.sh` |
