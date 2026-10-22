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
- `scripts/pgtest.sh install|start|psql|regress [test]|tap [test]|destroy` is the throwaway cluster; `scripts/ci.sh` runs every suite.
- `scripts/coverage.sh --gen` writes `docs/COVERAGE.md` and the README coverage block from `docs/coverage.tsv`; `--check` proves them.
- Benchmark tables are generated: `bench/bench all --docs` (`bench/steps/readme.py`, `tables.py`, `store.py`).
- `scripts/check-portable.sh` guards the portable core; `sqlite/test.sh` and `mariadb/test.sh` test the ports.

## Conventions

- Commit subject `context: subcontext: change` (the module or feature, never the project name); stage files individually.
- LF line endings only. Public docs use neutral wording: no private paths, hosts or internal names.
- Generated blocks (the `bench:` blocks of `docs/BENCHMARKS.md` and README.md, the coverage block, `docs/COVERAGE.md`) are never edited by hand: change the data or the generator.
- Fix root causes; a bug gets a failing test first. Comments only for non-obvious code.

## On-disk formats

Any change to what is stored needs a reader for the old and the new format, an upgrade test (`test/t/extension_lifecycle.sh`, `test/t/compat_2x_fixture.sh`)
and the format number in `src/core/sift.h` (BM25: major 1, minor 2) or `src/col/col.h` (column: major 1, minor 3), in DESIGN.md "Versioning" or docs/COL.md section 2,
and in CHANGELOG.md.

## Documentation

- Update docs only for essential changes: user-visible behaviour, SQL surface, on-disk formats, limits, feature status, and performance
  figures backed by stored logs (`docs/results/`, `bench/`). Do not rewrite or lengthen docs for internal refactors.
- CHANGELOG.md `## Unreleased` is the one place for small notes. Operational notes for agents belong here, not in other files.
- A perf figure carries its date, version, hardware and CPUs, or is marked as a historical baseline.
