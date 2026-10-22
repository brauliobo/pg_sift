# Upgrading

What to run, in order, to move a database to a newer pg_sift, to a newer PostgreSQL major, or back to an older pg_sift. The on-disk formats and the
first release that reads each are in [FORMATS.md](FORMATS.md).

| Change | What happens to the files | Steps |
|---|---|---|
| a newer pg_sift (patch or minor) | the new library reads the files of the older ones; it writes its own format from the next write on | [preflight](#1-preflight), [install and update](#2-install-and-update) |
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
| `option` | options that only units written afterwards follow | `warn`: `cube_minmax`, `rollup_dims` or `set_columns` on an index with older units, JSON `paths` on segments before column format 6 (the advice is the command from `sift.upgrade_commands()`, which an extension older than 3.3 has after its update; a `sift_table` below the format of its schema is a `table` row with `VACUUM FULL`), the deprecated `mutable_limit` |
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
notice that nothing is. PostgreSQL shows only warnings from an extension script, which is why they are warnings. `SELECT * FROM sift.upgrade_commands()` lists the same later:
one row per command (`kind` `reindex` or `rewrite`, the `object`, the `reason`, the `command`), the cheapest first.

| Warning | Why | Command |
|---|---|---|
| a `sift` (text) index needs a `REINDEX` | **queries miss rows**: a word over 255 bytes is stored as its first 236 bytes and a hash since 3.3, and an index built before holds it cut at 255 bytes, so a query for it finds nothing. The index does not say whether it holds such a word, so every text index except `analyzer = keyword` is listed, and stays listed after its `REINDEX` | `REINDEX INDEX CONCURRENTLY schema.index;` |
| a `sift_col` index would gain from a `REINDEX` | an option set after the units were written (`cube_minmax`, JSON `paths`, `rollup_dims`, `set_columns`): the older units stay readable and are scanned until it is done | `REINDEX INDEX CONCURRENTLY schema.index;` |
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

An older library refuses a metapage minor or a column format it does not know, with a `REINDEX` hint (`old_lib_refusal`). 3.3 writes at `sift.col_format_minor = 12` by default, so a 3.2 library cannot read a `sift_table` with a date or timestamp column, or an index with `cube_minmax`, `rollup_dims`, `set_columns` or `superunit`, written after the update, unless the setting is lowered to 6 before they are written. Ask the preflight what the target release can read:

```
scripts/upgrade-check.sh --to 3.1 app
```

Every `blocker` row says how to write what that release reads:

| Object | Advice |
|---|---|
| `sift_col` index | `SET sift.col_format_minor = N` (3 for 3.0, 4 for 3.1, 6 for 3.2; 3.3 reads every minor up to 12, its default) and `REINDEX INDEX x`; an index with wide leaves (column format 5) is written for 3.2 and later at every setting: drop it |
| `sift_table` | with `sift.col_format_minor` set, `ALTER TABLE t SET ACCESS METHOD heap` and back to `sift_table` rewrites it in that format |
| `sift` (BM25) index | the segment format is the library's: drop the index and create it again with the older release, or keep the newer library |

A standby that stays on the older release needs the same: its primary writes the formats the standby reads while `sift.col_format_minor` is set.

## 5. First release that reads each format

| Format | First release |
|---|---|
| BM25 metapage minor 1.2 (segment formats 1 and 2) | 3.0 |
| BM25 metapage minor 1.3 (segment format 3) | 3.1 |
| column format 3 | 3.0 |
| column format 4 (tags and kinds, version section of a `sift_table`) | 3.1 |
| column format 5 (wide leaves), 6 (complete JSON kinds) | 3.2 |
| column format 7 (`cube_minmax`), 8 (the calendar cells of a date or timestamp in the cube of a `sift_table`, or of a `sift_col` index with `calendar_columns`), 9 (`rollup_dims`), 10 (`set_columns`), 11 (the write buffer of a `sift_table` with a row on tail pages), 12 (`superunit`) | 3.3 |

`scripts/upgrade_check.sql` holds this table; a release that adds a format adds its row there (AGENTS.md, "On-disk formats").

## Schema `sift` is open to PUBLIC since 3.3

Up to 3.2 a role needed `GRANT USAGE ON SCHEMA sift` before it could call any `sift.*` function or read a `sift.*` view (`permission denied for schema sift`). The install and `ALTER EXTENSION pg_sift UPDATE` to 3.3 grant it to `PUBLIC`: the grants of a DBA stay and are no longer needed, and nothing else is granted (the functions check the table or index they read, `sift.table_paths` is not readable). A database that does not want it runs `REVOKE USAGE ON SCHEMA sift FROM PUBLIC` after the update; the next update grants it again, and the schema keeps its privileges across `DROP EXTENSION` and `CREATE EXTENSION`.

## Known issue of 3.2.0 and 3.2.1: a role cannot drop anything

In 3.2.0 and 3.2.1 the event trigger function `sift.table_paths_drop()` is not `SECURITY DEFINER`, so a `DROP` by a role that is neither a superuser nor the owner of the extension fails with `permission denied for schema sift` (any table, index or schema, also one without pg_sift). `ALTER EXTENSION pg_sift UPDATE` to 3.3 fixes it. Until then, run once in each database with the extension, as a superuser (and again after a restore from a dump, which creates the extension from the 3.2 script):

```sql
ALTER FUNCTION sift.table_paths_drop() SECURITY DEFINER SET search_path = pg_catalog, pg_temp;
```

## What is tested

| What | Test |
|---|---|
| the preflight on a clean database, with `--to`, with options, settings and test objects left behind, on an older extension and on an index the library refuses | `test/t/upgrade_check.sh` |
| `pg_upgrade --check` and the upgrade of `sift`, `sift_col` and `sift_table` objects (tablespace, wide leaf types, JSON paths, open write buffers), answers, `sift.verify`, `sift.col_verify`, writes and merges afterwards | `test/t/pg_upgrade.sh` |
| a same-major `pg_upgrade` of the BM25 indexes, `ALTER EXTENSION UPDATE` along every release | `test/t/extension_lifecycle.sh` |
| the previous releases' libraries write, this tree reads (and the other way) | `test/t/old_lib_refusal.sh`, `test/t/compat_31_column.sh`, and `scripts/release.sh` (`upgrade` step, [RELEASE.md](RELEASE.md)) |
