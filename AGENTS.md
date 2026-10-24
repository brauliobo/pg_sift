# AGENTS.md

pg_sift: a PostgreSQL extension (17, 18 and 19) with an exact BM25 full-text index access method (`sift`), a column index access method (`sift_col`),
a table access method (`sift_table`) and the aggregate pushdown (`Custom Scan (SiftAgg)`). The SQLite and MariaDB ports share the search core.

## Layout

- `src/core` shared shell and page store, `src/bm25` search engine, `src/col` column engine, `src/agg` aggregate pushdown, `src/table` `sift_table`
  (`src/portable.list` names the files every host compiles).
- `sql/` extension install and upgrade scripts; control files stay at the root. `test/sql/` and `bench/sql/` hold test and benchmark queries.
- `docs/` user and design documents (`USAGE.md`, `DESIGN.md`, `COL*.md`); `docs/results/` stored benchmark data; `docs/releases/` release notes.
- `bench/` benchmark harness and generators; `scripts/` build, CI, packaging; `test/` (`sql`, `isolation`, `t`, `col`, `fuzz`); `tools/` standalone harnesses.
- `sqlite/`, `mariadb/` the ports (each has its own `test.sh` and CHANGELOG); `packaging/` Debian, RPM, AUR.

## Build and test

- `make`, `make check` (quick tier), `make check-full` (ASan, soak, fuzz); `nice make -j4` on a shared host. Before a push run `scripts/ci.sh affected`, not `make check`: GitHub CI picks jobs by the same map (`scripts/areas.tsv`; a new test or file goes into it, `scripts/ci.sh --selftest` fails otherwise). Release: `scripts/release.sh X.Y.Z --dry-run`, no local gate (docs/RELEASE.md). Details: CONTRIBUTING.md.
- GitHub CI runs for pushes to `main`, dispatches, the nightly and pull requests from `integration/*` branches only; label any other pull request `ci` to opt it in (every other job skips, `gate` passes). The push run of `main` is the safety net for what a pull request did not run.
- `scripts/pgtest.sh install|start|psql|regress [test]|tap [test]|destroy` is the throwaway cluster; `scripts/ci.sh` runs every suite.
- Docs numbers are generated, never typed: `scripts/gen-docs.py` rewrites every generated block of README.md and docs/ (benchmark tables from the committed `docs/results/`,
  coverage from `docs/coverage.tsv`, the settings table and the test counts from `src/` and `test/`); `--check` (the lint job; seconds, no server) fails when one is stale.
  A benchmark pass is `bench/bench ... --docs` (it writes `docs/results/` and runs the renderers of `bench/steps`), then `scripts/gen-docs.py`; a block whose results file is missing says so.
  `scripts/coverage.sh --check` still proves the coverage claims on a cluster.
- `scripts/check-portable.sh` guards the portable core; `sqlite/test.sh` and `mariadb/test.sh` test the ports.

## Conventions

- Commit subject `context: subcontext: change` (the module or feature, never the project name); stage files individually.
- LF line endings only. Public docs use neutral wording: no private paths, hosts or internal names.
- Generated blocks (the `bench:`, `coverage:` and `gen:` blocks of `docs/BENCHMARKS.md` and README.md, `docs/COVERAGE.md`, `docs/LIMITS.md`, `docs/REFERENCE.md`, the `sift-reference` and `sift-options` blocks of README.md and the `sift-settings` and `sift-reloptions` tables of `docs/USAGE.md`) are never edited by hand: change the data, the source or the generator (`scripts/gen-docs.py` for the `bench:`, `coverage:` and `gen:` kinds).
- A new or changed `sift.*` function or view needs a `COMMENT ON` in the 3.3+ install and upgrade scripts (never the released ones: `docs/reference-notes.tsv`), then `make reference` (`scripts/gen-reference.sh`, no server needed) and
  commit `docs/REFERENCE.md` and the README block. A new limit goes in the registry, then `make limits`. A new reloption or `sift.*` setting needs its description in the `DefineCustom*Variable` / `add_*_reloption` call and a row in `docs/options.tsv` (applies to, what it buys, what it costs, link), then `make options`; defaults and ranges come from the call, and a changed default is flagged against `scripts/options-released.tsv` by itself. `--check` of both runs in lint and `scripts/ci.sh`, and fails otherwise.
- Fix root causes; a bug gets a failing test first. Comments only for non-obvious code.
- A test that counts units answered from pre-aggregates VACUUMs with `FREEZE, DISABLE_PAGE_SKIPPING`: a plain VACUUM skips a page another backend has pinned (the
  checkpointer writing it) and leaves it not all-visible. A TAP that measures a background worker or reads the log from a position first waits until no `sift maintenance` backend runs (`quiet` in
  `test/t/col_auto_merge_partition.sh`): a worker's snapshot keeps VACUUM from removing the dead rows of a table changed after it started.

## On-disk formats

Any change to what is stored needs a reader for the old and the new format, an upgrade test (`test/t/extension_lifecycle.sh`, `test/t/compat_2x_fixture.sh`)
and the format number in `src/core/sift.h` (BM25: major 1, minor 2) or `src/col/col.h` (column: major 1, minor 3), in DESIGN.md "Versioning" or docs/COL.md section 2,
in CHANGELOG.md, and, for a column format, the table of the first release that reads it in `scripts/upgrade_check.sql`.

## Documentation

- Update docs only for essential changes: user-visible behaviour, SQL surface, on-disk formats, limits, feature status, and performance
  figures backed by stored logs (`docs/results/`, `bench/`). Do not rewrite or lengthen docs for internal refactors.
- CHANGELOG.md `## Unreleased` is the one place for small notes. Operational notes for agents belong here, not in other files.
- A perf figure carries its date, version, hardware and CPUs, or is marked as a historical baseline.
- A release run from a host without the AUR key shows `aur fail` in its manifest (`GIT_SSH_COMMAND=false`): expected, then publish the PKGBUILD from the key host (docs/RELEASE.md, "The AUR from the host that has the key").
