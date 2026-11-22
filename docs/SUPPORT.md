# Support and Compatibility

Supported PostgreSQL versions, platforms, versioning policy, and on-disk format guarantees for `pg_sift` 6.0. For step-by-step upgrade procedures, see [UPGRADING.md](UPGRADING.md).

## Compatibility

| PostgreSQL | Status | Notes |
|---|---|---|
| 17 | Supported | Minimum version (`PG_MIN_MAJOR`); no `EXPLAIN (SIFT)` (use `sift.explain_why`) or CNPG image volumes |
| 18 | Supported | Primary benchmark and CI reference version |
| 19 | Supported (GA) | Tested continuously against `REL_19_STABLE` and the 19.x releases; `.deb`/`.rpm` built against PGDG 19 packages; Homebrew `postgresql@19` |
| 16 and older | Unsupported | Build stops with an error |

| Platform / Component | Support |
|---|---|
| Linux `x86-64` | PG 17, 18, 19; full test, fuzz, sanitizer, and memory-slope suite; `.deb`, `.rpm`, AUR |
| Linux `arm64` (`aarch64`) | PG 17, 18, 19 `.deb` and `.rpm` packages; NEON SIMD kernels; CI tested on PG 18 |
| macOS `arm64` | PG 17, 18, and 19 via [Homebrew formula](../Formula/pg_sift.rb) (`postgresql@17`, `@18`, `@19`) |
| Containers | Docker, Compose, and CloudNativePG recipes for PG 17–19 ([CONTAINERS.md](CONTAINERS.md)) |
| Standalone search ports | [SQLite](../sqlite/README.md) (3.38+, Linux `x86-64`) and [MariaDB](../mariadb/README.md) (10.5–13.x) |
| Server requirements | `BLCKSZ = 8192`, `UTF8` or `SQL_ASCII` encoding, heap and `sift_table` relations |

## Versioning Policy

- Releases use `MAJOR.MINOR.PATCH`. The PostgreSQL extension version (`pg_sift.control`) is `MAJOR.MINOR`; patch releases update only the shared library (`pg_sift.so`) with zero SQL or format changes.
- Bug and security fixes ship as patch releases on the newest minor line. Older lines are not backported — upgrade to the current line first.
- Security vulnerabilities are handled privately per [SECURITY.md](../SECURITY.md).
- The SQLite and MariaDB search ports share the same release tags and maintain their own `VERSION` and `CHANGELOG.md`.

## On-Disk Format & Downgrade Rules

- **`sift_col` and `sift_table` (Format Major 1 and 2):** `pg_sift` 6.0 writes new relations and every `REINDEX` in Format Major 2 (`2.0`) and keeps full read and write support for Format Major 1 relations created by 5.x, so upgrading the extension never forces a rebuild. Move a 5.x relation to Format 2.0 online with `REINDEX INDEX CONCURRENTLY` (`sift_col`) or `VACUUM FULL` / `sift.upgrade_table()` (`sift_table`). A 5.x binary cannot read a Format Major 2 relation: downgrading one means converting `sift_table` to `heap` (or restoring from backup) and rebuilding `sift_col` indexes under 5.x.
- **`sift` BM25 (Format `1.7`):** Unchanged from 5.x; existing BM25 indexes work across 5.1 → 6.0 without `REINDEX`.
- **Standbys:** Always upgrade physical standbys before the primary so standbys understand every WAL record written by the primary.

## License and Warranty

`pg_sift` is licensed under the [PostgreSQL License](../LICENSE) and provided without warranty or commercial SLA. File questions and bug reports on the GitHub issue tracker with `SELECT version(), sift.version()` and your `CREATE INDEX` / `CREATE TABLE` definition.
