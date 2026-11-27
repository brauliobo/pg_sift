# Contributing

`pg_sift` targets PostgreSQL 17, 18, and 19 with server headers (`pg_config` on `PATH`) and GCC or Clang.

## Build and Test

```sh
make                            # build pg_sift.so (override with PG_CONFIG=/path/to/pg_config)
make installcheck               # regression tests against a running server
make check                      # quick tier in throwaway clusters (~1.5 min on 8 cores)
scripts/ci.sh affected          # only jobs your diff against origin/main can affect (scripts/areas.tsv)
scripts/ci.sh affected --focus  # regression tests covering the changed C functions (scripts/focus/)
```

- `make check` and CI scripts require bash 4+ (on macOS, `brew install bash` and place it first on `PATH`).
- Use `scripts/pgtest.sh install && scripts/pgtest.sh start` for an isolated throwaway cluster (`pgtest.sh psql` to connect, `pgtest.sh destroy` to remove).
- Filter jobs with `SIFT_CI_JOBS=REGEX make check` and control parallelism with `SIFT_LANES=N`.
- `scripts/coldiff.sh` runs differential tests comparing `sift.enable_agg = on` against `off` across fixtures and randomized shapes (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)).
- `scripts/replication.sh` verifies physical and logical replication (`--container` runs each node in Docker).
- `SIFT_TESTING=1` (default in `make check` and `pgtest.sh`) builds test-only `sift._test_*` helpers and `pg_sift_test`; normal `make install` never ships them. Switching modes requires `make clean`.
- Add test-only regression files to `TESTING_REGRESS` in `scripts/testing-regress.mk` in byte order so parallel branches merge cleanly (`scripts/check-testing-regress.sh`). Keep limit, privilege, and planner tests free of `sift._test_*` so they also run against the release library (`release-library-regress`).

## Standalone Search Ports

- **SQLite (`sqlite/`):** Test standalone with `sqlite/test.sh` (or `SIFT_SANITIZE=1 sqlite/test.sh` for ASan/UBSan/TSan). `make -C sqlite isa-check` verifies the baseline build uses no POPCNT or SIMD instructions.
- **MariaDB (`mariadb/`):** Built and tested via `scripts/package-mariadb.sh`.
- Both ports compile `src/portable.list` unmodified against their host shims (`src/core/host.h`). Run `sqlite/test.sh` whenever touching portable files.

## Engineering Rules (Enforced by `scripts/lint.sh`)

- **Strict bounded memory:** All allocations obey PostgreSQL's `work_mem` / `maintenance_work_mem` and the unified cache caps (`sift.backend_cache_kb`, `sift.shared_cache_kb`) via `SiftMemTracker`. `scripts/check-memory.sh` and `scripts/check-mem-growth.sh` ban unbounded allocations, `MemoryContextAllocHuge` in operational paths, and per-row growth. Every test cluster runs under `scripts/leakcheck.sh`.
- **Errors and invariants:** Every `ERROR` with SQLSTATE `54000`, `0A000`, or `55000` must include `errhint` or `errdetail`; never emit `XX000`. Use `SIFT_INVARIANT(cond, "msg")` for impossible internal states and `ereport` (`XX001`/`XX002`, `40001`) for data/runtime faults (`scripts/check-errors.sh`).
- **Expected test outputs:** Never accept `f`, `NULL`, `FAIL`, or `MISMATCH` in oracle verdict columns of `test/expected/*.out` (`scripts/check-expected-oracle.py`). Pin deterministic codecs (`compression = 'none'` or encoding-only specs) in tests that print byte sizes (`scripts/check-codec-bytes.py`).
- **File size and layout:** Keep C, SQL, shell, and Python files under 1,000 lines (`scripts/check-file-size.sh`). Place sources in their area folder under `src/` (`scripts/restructure-src.sh --check`) and classify every file in `src/portable.list`, `src/portable-col.list`, or `src/pg-only.list` (`scripts/check-classified.sh`).
- **Portable core:** Files in `src/portable.list` include only standard C headers, other portable files, and `src/core/host.h` — never PostgreSQL or SQLite headers (`scripts/check-portable.sh`).
- **SQL migration parts:** Add release SQL changes in `sql/<version>/NN-<context>.sql`, never directly to generated `sql/pg_sift--*.sql`. `scripts/sql-parts.sh build` assembles them deterministically.
- **Documentation sync:** Options (`docs/options.tsv` → `docs/OPTIONS.md`), SQL reference (`sql/` → `docs/REFERENCE.md`), limits (`src/core/limits.h` → `docs/LIMITS.md`), refusals (`src/agg/refuse.h` → `docs/ARCHITECTURE.md`), and messages (`docs/messages.tsv` → `docs/MESSAGES.md`) are generated and drift-checked by `scripts/lint.sh`. Tables in `README.md` are checked by `scripts/check-readme-tables.py` (max 5 columns, max 140 chars/cell).
- **Neutral vocabulary:** `scripts/check-neutral.sh` enforces generic names (`orders`, `events`, `docs`, `customers`, `tenants`, `members`, `items`) and rejects private hosts, IPs, or local paths in files, commits, and PR metadata.

## Catalog Writes and Locking

- Never write catalog rows or assign XIDs inside `VACUUM` (`PROC_IN_VACUUM`) or parallel workers. Route all catalog mutations through `src/core/restrict.c` (`scripts/check-catalog-writes.sh`); let background maintenance workers perform deferred catalog writes.
- Register every heavyweight lock and LWLock acquisition in `scripts/locks.tsv` (`scripts/check-locks.sh`). Maintenance operations must never conflict with `RowExclusiveLock` on user tables (`test/t/core/lock_modes.sh`).
- Acquire relation extension locks exclusively through `sift_extension_lock` and `sift_extend_zeroed_to` in `src/core/extlock.c` (`scripts/check-extlock.sh`), which freeze error context names before locking.
- PRs touching parallel workers or snapshot paths must pass on a `--enable-cassert` build (`make pg-assert`).

## Submitting Changes and Releasing

- Add a regression (`test/sql/`) or TAP (`test/t/`) test reproducing the bug before fixing it.
- Keep builds free of compiler warnings and pass `make check` and `scripts/lint.sh`.
- Prefix commit subjects with the subsystem: `col: handle empty granule in range prune`.
- Land PRs via `scripts/land.sh PR` and cut releases via `scripts/release.sh X.Y.Z` after updating `docs/releases/vX.Y.Z.md` and `CHANGELOG.md`.

Contributions are licensed under the [PostgreSQL License](LICENSE).
