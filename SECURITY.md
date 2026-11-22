# Security policy

pg_sift is C code that runs inside the PostgreSQL server and parses user-supplied documents and queries. A crash,
memory error, wrong-result or index-corruption bug reachable from SQL is in scope, in particular anything that a
crafted query string, a crafted document, concurrent DML, or recovery after a crash can trigger.

Please report such problems privately with GitHub's "Report a vulnerability" button (Security tab of the repository)
instead of opening a public issue, and include the PostgreSQL version, the `CREATE INDEX` statement and a reproduction
if you have one. Fixes ship as patch releases of the newest minor line (6.0.x) and land on the `main` branch; older lines are not
backported ([docs/SUPPORT.md](docs/SUPPORT.md)). The PostgreSQL versions in scope are 17, 18 and 19. Reports are handled privately until a
fixed release is available.

The PostgreSQL surface in scope covers the three access methods (`sift` BM25 indexes, `sift_col` indexes and `sift_table` tables), the `SiftAgg`
and `SiftHybridScan` custom scans, the background maintenance workers, and the SQL functions in schema `sift`. The boundaries are:

- **Row-level security and security barriers.** `SiftAgg`, `SiftHybridScan`, the Top-N, window and stream executors, scope cubes and rings, and
  joined dimension caches read column data directly, so each one refuses a relation with row-level security (including `FORCE ROW LEVEL SECURITY`),
  security quals, or a `security_barrier` view, and PostgreSQL's own plan runs instead (`sift.explain_why()` names the slug). Cardinality
  estimates ignore such relations too. BM25 ranked scans under row-level security stay off unless a superuser sets `sift.rls_scores`
  ([docs/SEARCH.md](docs/SEARCH.md)). A row, count, score or estimate that crosses a policy is in scope.
- **Functions.** The SQL functions are `SECURITY INVOKER` with `search_path = pg_catalog, sift`, and the internal helpers live in `sift_internal`,
  revoked from `PUBLIC`. The few `SECURITY DEFINER` helpers (scope pair maintenance, the projection registry) pin `search_path = pg_catalog, pg_temp`
  and check the calling role. `sift.upgrade_table(regclass)` rewrites a table and needs the `MAINTAIN` privilege on it (`ACL_MAINTAIN`, as
  `VACUUM FULL` does).
- **Shared memory.** The five shared pools (`SiftShmLedger`: `manifest`, `agg_lookup`, `agg_dimmap`, `col_dict`, `col_leaf`) and the maintenance
  scheduler state (`SiftMaintShmem`) are fixed-size and capped by `sift.shared_cache_kb`, which only a superuser sets; a full pool refuses or evicts
  and never grows. `sift.shared_memory_pools()` reports use per pool. A cross-role leak through a pool entry, or a backend that crashes while
  holding a pool slot, is in scope.
- **Vulkan backend.** The opt-in device path (`make VULKAN=1`, [docs/GPU.md](docs/GPU.md)) opens `libvulkan` with `dlopen` in the backend on first use, never in the postmaster, and only a superuser sets `sift.gpu`, `sift.gpu_device` and `sift.gpu_cache_mb`. A crash, wrong result or memory error reachable through a GPU-eligible query is in scope; the packages build without it.
- **Damaged pages.** Every Format Major 2 page is CRC32C-verified; a failed check quarantines the index (`sift.on_corruption = 'quarantine'`)
  instead of returning rows from it, and a crash, wrong result or silent acceptance of crafted page content is in scope.

The SQLite extension (`sift` for SQLite, in `sqlite/`) is C code that runs inside the process of the application that loads it and
parses documents and queries the application supplies; the same classes of problem are in scope (a crash, memory error, data race, wrong
result or index corruption reachable from SQL), reported the same way with the SQLite version, the `CREATE VIRTUAL TABLE` statement and a
reproduction. Loading an extension is code execution by design, so only load the `sift` file from a source you trust (check it against the
release's `SHA256SUMS`). The shadow tables (`<table>_chunks`, `_segs`, `_meta`, `_config`) are written by the extension: a database whose shadow
tables were edited by hand is not a supported input, though a crash on one is still welcome as a report.

## Supported versions

| Version | Fixes |
|---|---|
| 6.0.x | yes |
| older | no |

The table names the newest minor line and is kept in step with the release by `scripts/check-release-sync.sh`.

The MariaDB extension (`sift` for MariaDB, in `mariadb/`) is a UDF library (`libsift_udf.so`) loaded into the `mariadbd` server process, so a
crash, memory error, data race, wrong result or index corruption reachable from the SQL functions and procedures it registers is in scope, reported
the same way with the MariaDB series (`SELECT VERSION()`), the statements and a reproduction. Registering a UDF is code execution inside the server
by design: copy `libsift_udf.so` into the plugin directory only from a source you trust (check it against the release's `SHA256SUMS`) and run
`mariadb/sql/functions.sql` as a user allowed to do so. The index table is written by the extension: a table edited by hand is not a supported
input, though a crash on one is still welcome as a report.
