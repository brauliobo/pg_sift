# Upgrading

What to run, in order, to move a database to a newer pg_sift, to a newer PostgreSQL major, or back to an older pg_sift. The on-disk formats and the
first release that reads each are in [FORMATS.md](FORMATS.md).

| Change | What happens to the files | Steps |
|---|---|---|
| a newer pg_sift (patch or minor) | the new library reads the files of the older ones; it writes its own format from the next write on | [preflight](#1-preflight), [install and update](#2-install-and-update) |
| 3.2.1 to 3.2.2 | library only: the extension SQL stays at 3.2, no format changes | install the files and restart the server (an open session keeps the old library, and pg_sift in `shared_preload_libraries` is read at start only); no `ALTER EXTENSION pg_sift UPDATE`, no `REINDEX`; going back to 3.2.1 needs nothing ([notes](releases/v3.2.2.md)) |
| a newer PostgreSQL major | `pg_upgrade` copies (or links) the files as they are | [pg_upgrade](#3-a-newer-postgresql-major) |
| an older pg_sift (a downgrade, or a standby that stays on an older release) | an older library refuses what it cannot read | [preflight with `--to`](#4-an-older-release) |

## 1. Preflight

`scripts/upgrade-check.sh` runs `sift.upgrade_check()` (`scripts/upgrade_check.sql`) in each database. It reads the catalogs and the metapages, changes nothing
(the function is loaded in a transaction that is rolled back, so a database whose extension is older can be checked) and needs a superuser on the primary,
with the files of the new release installed:

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
| `option` | options that only units written afterwards follow | `warn`: `cube_minmax`, `rollup_dims`, `set_columns`, `order_columns` or `key_presence` on an index with older units, JSON `paths` on segments before column format 6 (the advice is the command from `sift.upgrade_commands()`, which an extension older than 3.3 has after its update; a `sift_table` below the format of its schema is a `table` row with `VACUUM FULL`), the deprecated `mutable_limit` |
| `setting` | `sift.*` in the server configuration, the session, `ALTER DATABASE` and `ALTER ROLE` | `warn`: a setting the library does not define (removed or renamed), a feature switched off (a boolean setting that defaults on), `sift.col_format_minor` kept below the default |
| `test` | test-only settings and the extension `pg_sift_test` | `warn`: remove them; they need the test build of the library |

Run it before the update and again after it: the second run must have no `warn` that the first one explained.

## 2. Install and update

```

# 1. install the files of the new release (package, or from source)
sudo apt install ./postgresql-18-pg-sift_X.Y.Z-1_amd64.deb
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

`ALTER EXTENSION pg_sift UPDATE` runs the scripts from the installed version to the default one of the control file, through every release in between. The files of
the indexes and tables are not rewritten: an older index stays in its format until a merge, `VACUUM` or `REINDEX` writes it again, and the preflight and the update tell which
`REINDEX` is worth it. A `sift_table` cannot be reindexed; it follows `sift.col_format_minor` on its next write.

The last step of an update to 3.3 (from any older version) prints what is left to do, as warnings with the commands (the first 20 of each, then the count of the rest), or a
notice that nothing is. PostgreSQL shows only warnings from an extension script, which is why they are warnings. `SELECT * FROM sift.upgrade_commands()` lists the same later, for the indexes and tables the role owns or may maintain (every one for a superuser):
one row per command (`kind` `reindex` or `rewrite`, the `object`, the `reason`, the `command`), the cheapest first.

| Warning | Why | Command |
|---|---|---|
| a `sift` (text) index needs a `REINDEX` | **queries miss rows**: a word over 255 bytes is stored as its first 236 bytes and a hash since 3.3, and an index built before holds it cut at 255 bytes, so a query for it finds nothing. The index does not say whether it holds such a word, so every text index except `analyzer = keyword` is listed, and stays listed after its `REINDEX` | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_col` index would gain from a `REINDEX` | an option set after the units were written (`cube_minmax`, JSON `paths`, `rollup_dims`, `set_columns`, `order_columns`, `key_presence`): the older units stay readable and are scanned (or, for `order_columns`, read whole by an `ORDER BY .. LIMIT`) until it is done | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_table` would gain from a rewrite | segments below the column format of its schema: no calendar cells for its date and timestamp columns (format 8), JSON kinds that may be incomplete for declared paths (format 6). A `sift_table` cannot be reindexed | `VACUUM FULL schema.table;` |

Nothing but the first is wrong without the command. `VACUUM FULL` takes an `ACCESS EXCLUSIVE` lock and rewrites the whole table in the format of `sift.col_format_minor`, so
schedule it; once it ran the table leaves the list. `REINDEX CONCURRENTLY` cannot run in a transaction block: run the commands one at a time. The check reads the catalogs and the
metapages and cannot fail the update; when it cannot answer (a relation locked by another session) it says so and leaves `sift.upgrade_commands()` to run after reconnecting.

## 3. A newer PostgreSQL major

`pg_upgrade` copies the relation files and restores the catalogs. pg_sift needs only its library for the new major next to the server (the same release as
before: the extension version is not changed by `pg_upgrade`), and the same data checksum setting on both clusters (PostgreSQL 18 enables checksums by default; a cluster
without them is initialised with `initdb --no-data-checksums`).

```

# the library of the same pg_sift release for the new major
sudo apt install ./postgresql-18-pg-sift_X.Y.Z-1_amd64.deb
make USE_PGXS=1 PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config && sudo make USE_PGXS=1 PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config install   # from source

# the new cluster, then the check (it starts both clusters and loads pg_sift in each), then the upgrade
/usr/lib/postgresql/18/bin/initdb -D /var/lib/postgresql/18/main
/usr/lib/postgresql/18/bin/pg_upgrade -b /usr/lib/postgresql/17/bin -B /usr/lib/postgresql/18/bin \
    -d /var/lib/postgresql/17/main -D /var/lib/postgresql/18/main --check
/usr/lib/postgresql/18/bin/pg_upgrade -b /usr/lib/postgresql/17/bin -B /usr/lib/postgresql/18/bin \
    -d /var/lib/postgresql/17/main -D /var/lib/postgresql/18/main
```

After it: copy the `sift.*` lines of the old `postgresql.conf` (`pg_upgrade` carries the settings stored in the catalogs, `ALTER DATABASE` and `ALTER ROLE`, not the file), start the new
cluster, run `vacuumdb --all --analyze-in-stages`, and check as in section 2 (`sift.verify`, `sift.col_verify`, `scripts/upgrade-check.sh --all`). The
BM25 results and scores, the column aggregates and the `sift_table` contents are the same as before the upgrade, and writes, `VACUUM` and merges work on what the old cluster wrote
(`test/t/pg_upgrade.sh`: PostgreSQL 17 to 18 when both are installed, otherwise 18 to 18).

## 4. An older release

An older library refuses a metapage minor or a column format it does not know, with a `REINDEX` hint (`old_lib_refusal`). 3.3 writes at `sift.col_format_minor = 12` by default, so a 3.2 library cannot read a `sift_table` with a date or timestamp column, or a `sift_col` index (`cube_minmax` and `superunit` are on by default; `rollup_dims` and `set_columns` are options), written after the update, unless the setting is lowered to 6 before they are written (`SET sift.col_format_minor = 6` makes `cube_minmax` and `superunit` no-ops). Ask the preflight what the target release can read:

```
scripts/upgrade-check.sh --to 3.1 app
```

Every `blocker` row says how to write what that release reads:

| Object | Advice |
|---|---|
| `sift_col` index | `SET sift.col_format_minor = N` (3 for 3.0, 4 for 3.1, 6 for 3.2; 3.3 reads every minor up to 15, its default) and `REINDEX INDEX x`; an index with wide leaves (column format 5) is written for 3.2 and later at every setting: drop it |
| `sift_table` | with `sift.col_format_minor` set, `ALTER TABLE t SET ACCESS METHOD heap` and back to `sift_table` rewrites it in that format |
| `sift` (BM25) index | the segment format is the library's: drop the index and create it again with the older release, or keep the newer library |

A standby that stays on the older release needs the same: its primary writes the formats the standby reads while `sift.col_format_minor` is set.

## Defaults changed in 3.3

An existing index or table keeps what it holds; the new default applies to the units written after the update (promotion, merge, `REINDEX`), and `sift.upgrade_commands()` lists the `REINDEX` that gives older units the new sections. To keep the 3.2 behavior set the value named in the last column. `sift.table_auto_defrag` is read from the library, not from the extension script: it is on for every `sift_table` of the cluster as soon as the 3.3 library runs (after the restart), whatever the extension version, and the preflight of section 1 warns about the `off` that keeps 3.2, as it does for any feature switched off.

| Setting | 3.2 | 3.3 | Measured (200,000 rows, 53 units) | To keep the 3.2 behavior |
|---|---|---|---|---|
| `sift_col` option `cube_minmax` | off | on | `GROUP BY` with `min`/`max` of an integer 5.5 -> 0.34 ms; index +0.17% | `WITH (cube_minmax = false)` |
| `sift_col` option `superunit` | 0 | 16 | whole-table aggregates 4 to 7 times faster; index +1.0% | `WITH (superunit = 0)` |
| `sift.table_auto_defrag` (server setting, with `sift.async_maintenance`, on in 3.2 too) | off | on | four clients of an update, insert and delete mix on 200,000 rows: 7,536 against 5,092 transactions a second, range read p99 16.8 against 34.2 ms; a table that only appends starts no pass | `sift.table_auto_defrag = off` |
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
| column format 7 (`cube_minmax`), 8 (the calendar cells of a date or timestamp in the cube of a `sift_table`, or of a `sift_col` index with `calendar_columns`), 9 (`rollup_dims`), 10 (`set_columns`), 11 (the write buffer of a `sift_table` with a row on tail pages), 12 (`superunit`), 13 (`order_columns`), 14 (a string dictionary of 1,024 strings or more, stored for a lookup in place), 15 (`key_presence`) | 3.3 |

`scripts/upgrade_check.sql` holds this table; a release that adds a format adds its row there.

## Schema `sift` is open to PUBLIC since 3.3

Up to 3.2 a role needed `GRANT USAGE ON SCHEMA sift` before it could call any `sift.*` function or read a `sift.*` view (`permission denied for schema sift`). The install and `ALTER EXTENSION pg_sift UPDATE` to 3.3 grant it to `PUBLIC`: the grants of a DBA stay and are no longer needed, and nothing else is granted (the functions check the table or index they read, `sift.table_paths` is not readable). A database that does not want it runs `REVOKE USAGE ON SCHEMA sift FROM PUBLIC` after the update; the next update grants it again. PostgreSQL creates the schema named in the control file but does not make it a member of the extension (`ALTER EXTENSION pg_sift ADD SCHEMA sift` is refused), so `DROP EXTENSION` leaves it empty, as in 3.2, with the privileges it had; `CREATE EXTENSION` then gives the objects and privileges of a fresh install again, and `DROP SCHEMA sift` removes it.

## Known issue of 3.2.0, 3.2.1 and 3.2.2: a role cannot drop anything

In 3.2.0, 3.2.1 and 3.2.2 the event trigger function `sift.table_paths_drop()` is not `SECURITY DEFINER`, so a `DROP` by a role that is neither a superuser nor the owner of the extension fails with `permission denied for schema sift` (any table, index or schema, also one without pg_sift). `ALTER EXTENSION pg_sift UPDATE` to 3.3 fixes it. Until then, run once in each database with the extension, as a superuser (and again after a restore from a dump, which creates the extension from the 3.2 script):

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
