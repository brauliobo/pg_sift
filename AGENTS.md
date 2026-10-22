# AGENTS.md

pg_sift: a PostgreSQL extension (17 and 18) with an exact BM25 full-text index access method (`sift`), a column index access method (`sift_col`),
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
- Generated blocks (`bench:`, `coverage:` and `gen:` markers in README.md and docs/, `docs/COVERAGE.md`) are never edited by hand: change the data, the source or the generator.
- Fix root causes; a bug gets a failing test first. Comments only for non-obvious code.

## Tables in the docs

GitHub scrolls a wide table sideways, which is unreadable on a phone. Every table of README.md and every generated table of docs/ stays within 6 columns, 100 characters a source row and 40 a cell
(`scripts/gen-docs.py --check` fails otherwise; `--tables [--all]` lists the offenders). Renderers call `bench/steps/mdtable.py` (`table`, `details`), never a private `md_table`. To fit:

- Put the unit in the header or the row label (`build s`, `size (MiB)`), not in each cell; round to 3 significant digits (`readme.absolute`).
- Rows are the many things (metrics, queries, classes), columns the few (engines, versions). More than 5 engines: `mdtable.table` splits the value columns into several tables and repeats the first `keys` columns, so pass `keys=2` to keep a baseline engine in every part.
- Short labels and a legend: `Q03` and `sift_col`, with a list under the table that says what they are. Code, SQL and long names go to the legend or a note, not into cells; prose (settings, coverage rows, how-to) is a list, not a table.
- A comparison shows a compact ratio (`sift faster by`) next to the two values; the full table with every engine, error bar and absolute ms goes into `mdtable.details(...)` below it. Charts (`docs/img/*.svg`, `bench/steps/charts.py`) carry the headline when one exists.
- Hand-written tables (the comparison tables of README.md) follow the same budget; run `scripts/gen-docs.py --tables` after editing one.

## On-disk formats

Any change to what is stored needs a reader for the old and the new format, an upgrade test (`test/t/extension_lifecycle.sh`, `test/t/compat_2x_fixture.sh`)
and the format number in `src/core/sift.h` (BM25: major 1, minor 2) or `src/col/col.h` (column: major 1, minor 3), in DESIGN.md "Versioning" or docs/COL.md section 2,
and in CHANGELOG.md.

## Documentation

- Update docs only for essential changes: user-visible behaviour, SQL surface, on-disk formats, limits, feature status, and performance
  figures backed by stored logs (`docs/results/`, `bench/`). Do not rewrite or lengthen docs for internal refactors.
- CHANGELOG.md `## Unreleased` is the one place for small notes. Operational notes for agents belong here, not in other files.
- A perf figure carries its date, version, hardware and CPUs, or is marked as a historical baseline.
