# Support

What is supported, for how long, and how to move between releases. The steps of an upgrade are in [UPGRADING.md](UPGRADING.md), the numbers written to disk in
[FORMATS.md](FORMATS.md).

## PostgreSQL versions

| PostgreSQL | pg_sift | Notes |
|---|---|---|
| 17 | supported | the oldest the build accepts (`PG_MIN_MAJOR` in the Makefile); it has no `extension_control_path`, so an image-volume extension ([CONTAINERS.md](CONTAINERS.md)) needs 18 |
| 18 | supported | the reference version of the benchmarks and of the packages |
| 19 | supported | the CI builds PostgreSQL 19 from its git branch and runs the same jobs as for 17 and 18; the official `postgres:19` image tag did not exist on 2026-10-22 (`19rc1` did) and PGDG packaged `postgresql-19` as `19~rc1`; the release `.deb` and `.rpm` are built for 18 only, so on 19 build from source |
| 16 and older | not supported | the build stops with an error |

## pg_sift versions

- A release is `MAJOR.MINOR.PATCH`. The extension version (`pg_sift.control`, `SELECT extversion FROM pg_extension`) is `MAJOR.MINOR`; a patch release changes the
  library only.
- The two newest minor lines get fixes (with 3.3 the newest, 3.3.x and 3.2.x); a fix is a patch release of the line. Older lines get none: upgrade first.
- Security problems are reported as [SECURITY.md](../SECURITY.md) says.
- The SQLite and MariaDB ports have their own `CHANGELOG.md` and `VERSION` and are released with the same tag.

## Upgrade paths

- `ALTER EXTENSION pg_sift UPDATE` runs the scripts `sql/pg_sift--A--B.sql` from the installed version to the default one of the control file, through every release
  (1.0 to the current one). The scripts of a released version never change.
- The index and table files are not rewritten by an update: the new library reads what the older ones wrote, and writes its own format from the next write.
  `scripts/upgrade-check.sh` reports what a release can read before you install it.
- A PostgreSQL major upgrade with `pg_upgrade` keeps the files as they are and needs the same pg_sift release installed for the new major.
- A container image carries one pg_sift release: pull the new image, start it on the same data directory, then `ALTER EXTENSION pg_sift UPDATE` in each database.

## Format minors and the downgrade rule

Each index or table file records the format it needs (the BM25 metapage minor, the column format). A library reads every format of its own release and of the
older ones, and refuses a newer one with a `REINDEX` hint instead of guessing. Going back to an older pg_sift is therefore not automatic:

- run `scripts/upgrade-check.sh --to X.Y`; every object the target release cannot read is a `blocker` row with its fix;
- `sift_col`: `SET sift.col_format_minor` to the minor the target release reads, then `REINDEX INDEX`; `sift_table`: the same setting, then `VACUUM FULL t`; the two event triggers of 3.3 `DISABLE`d for a 3.2 library; the sequence, tested end to end for 3.2, is in [UPGRADING.md](UPGRADING.md#4-an-older-release);
- `sift` (BM25) index: drop it and create it again with the older release, or keep the newer library;
- a standby that stays on the older release needs the formats the older library reads while its primary writes;
- 3.3 writes at `sift.col_format_minor = 15` by default: a database written with the defaults can no longer be read by a 3.2 library (new indexes carry `cube_minmax` and `superunit`, and the `sift_table` tables with a date or timestamp column hold calendar cells), unless the setting is lowered (6 for 3.2) before the writes.

The first release that reads each format is the table in [UPGRADING.md](UPGRADING.md#5-first-release-that-reads-each-format), and the release notes of a release
say which formats it writes by default.

## Licence and warranty

pg_sift is under the [PostgreSQL License](../LICENSE). The licence disclaims warranties and any obligation to provide maintenance or support, and the project has
no support contract or service level. Questions and bug reports go to the GitHub issue tracker; include `SELECT version(), sift.version()` and the `CREATE INDEX` statement.
