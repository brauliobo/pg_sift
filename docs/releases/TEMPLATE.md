<!--
Template of docs/releases/<tag>.md, the text of the GitHub release of a tag (.github/workflows/release.yml publishes it). Copy it to
docs/releases/vX.Y.Z.md before tagging and delete this comment. Keep the parts apart: the PostgreSQL extension (pg_sift) first, then
the SQLite extension (`sift` for SQLite, artifact `sift-sqlite`), then the MariaDB extension (`sift` for MariaDB, artifact `sift-mariadb`),
each with its own highlights, compatibility and install lines, and its own changelog (CHANGELOG.md, sqlite/CHANGELOG.md,
mariadb/CHANGELOG.md). Set sqlite/VERSION and mariadb/VERSION to X.Y.Z before tagging: the release builds refuse a tag that differs.
A release without the SQLite or the MariaDB tarball (an old tag, or one that does not ship it) leaves its "## SQLite extension" or
"## MariaDB extension" part out; the publish job requires the section whenever the tarball is attached. The title of the release is
generated ("pg_sift X.Y.Z").
-->
pg_sift X.Y.Z: <one line: what changed for PostgreSQL users and how to upgrade (`ALTER EXTENSION pg_sift UPDATE`, `REINDEX`, nothing)>.

## Highlights

- **<feature>:** <what it does, with the measured effect>.

## Compatibility

- PostgreSQL <versions>; <on-disk format, upgrade notes>.

## Packages

x86-64 packages for PostgreSQL 18 are attached below; other versions build from source with `make USE_PGXS=1`.

- Debian 13, Ubuntu 24.04 (PGDG `postgresql-18`): `sudo apt install ./postgresql-18-pg-sift_X.Y.Z-1_amd64.deb`
- RHEL, Rocky, Alma 9 and 10 (PGDG `postgresql18-server`): `sudo dnf install ./pg_sift_18-X.Y.Z-1.el9.x86_64.rpm`
- Arch Linux: AUR `pg_sift`.

Benchmarks: [docs/BENCHMARKS.md](https://github.com/brauliobo/pg_sift/blob/vX.Y.Z/docs/BENCHMARKS.md). Every change: [CHANGELOG.md](https://github.com/brauliobo/pg_sift/blob/vX.Y.Z/CHANGELOG.md).

## SQLite extension

pg_sift for SQLite X.Y.Z (`sift`, a loadable extension; a separate component from the PostgreSQL extension above). <one line: what is new in it>.

- **<feature>:** <what it does, with the measured effect against FTS5 where it applies>.
- Needs SQLite 3.38 or newer; Linux x86-64, glibc 2.34 or newer. Built on Debian 12, loaded into the SQLite of Debian 12, Ubuntu 24.04 and AlmaLinux 10 before release.
- Storage format <n>: <upgrade note: tables of an older format are refused and must be rebuilt, or nothing>.

Download `sift-sqlite-X.Y.Z-linux-x86_64.tar.gz` (below, covered by `SHA256SUMS` with the packages), unpack it and `.load ./sift` from the `sqlite3`
shell (`sha256sum --ignore-missing -c SHA256SUMS`). Documentation: [sqlite/README.md](https://github.com/brauliobo/pg_sift/blob/vX.Y.Z/sqlite/README.md).
Every change: [sqlite/CHANGELOG.md](https://github.com/brauliobo/pg_sift/blob/vX.Y.Z/sqlite/CHANGELOG.md).

## MariaDB extension

pg_sift for MariaDB X.Y.Z (`sift`, a UDF library and two SQL files; a separate component from the extensions above). <one line: what is new in it>.

- **<feature>:** <what it does, with the measured effect against InnoDB FULLTEXT where it applies>.
- One library per MariaDB series, because it calls the server's own symbols and refuses to load into another series: <the series of this release, for example 10.11, 11.4, 11.8, 12.1 and 13.0>; Linux x86-64. Each was built in the stock `mariadb:<series>` image against that server's headers (<the server versions, from BUILD-INFO>) and loaded into a server of the same image before release. `BUILD-INFO` in the tarball lists the server version and the glibc each library needs (<the floors>); a server of another distribution or of a series not listed is built from source (`make -C mariadb`).
- Index format <n>: <upgrade note: an index of another format is refused and rebuilt with `sift_rebuild`, or nothing>.

Download `sift-mariadb-X.Y.Z-linux-x86_64.tar.gz` (below, covered by `SHA256SUMS` with the packages), unpack it, copy `lib/<series>/libsift_udf.so` of your server's series
(`SELECT VERSION()`) into its `plugin_dir`, then `SOURCE sql/functions.sql` once per server and `sql/sift.sql` once per database (`sha256sum --ignore-missing -c SHA256SUMS`).
Documentation: [mariadb/README.md](https://github.com/brauliobo/pg_sift/blob/vX.Y.Z/mariadb/README.md).
Every change: [mariadb/CHANGELOG.md](https://github.com/brauliobo/pg_sift/blob/vX.Y.Z/mariadb/CHANGELOG.md).
