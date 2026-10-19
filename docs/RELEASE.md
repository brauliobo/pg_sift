# Release gate

`scripts/ci.sh release` is the check a release head passes, once. It does not repeat the whole test matrix: the full CI, the full
differential test, the long soaks and the long fuzz runs belong to development. The gate picks the jobs by the risk of what changed since the
last head that was gated in full, and a tree that already has a stamp is not gated again.

```
scripts/ci.sh release                 run the gate on HEAD (stamp written when everything passes)
scripts/ci.sh release --list          the key of the tree, whether it has a stamp, and the jobs in order (nothing runs)
scripts/ci.sh release --force         run although the tree has a stamp
scripts/ci.sh release --only REGEX    some jobs (no stamp)
scripts/ci.sh release --keep          keep the scratch directory of every job
```

## What is gated

| Tier | Job | Proves |
|---|---|---|
| 1 | `build` | both flavors (production, `SIFT_TESTING`) build with no warning |
| 1 | `werror-gcc`, `werror-clang` | warnings are errors |
| 1 | `portable` | the portable core includes only the host header |
| 1 | `ci-quick` | `scripts/ci.sh quick`, the shrunk suites, on the normal build |
| 1 | `coverage` | every row of the coverage table holds (`scripts/coverage.sh --check`) |
| 2 | `ci-sanitize` | `scripts/ci.sh quick` on the ASan+UBSan build |
| 2 | `pg17` | `scripts/ci.sh quick` on PostgreSQL 17, in a container |
| 2 | `race` | `vacuum_count_stress`, `vacuum_count_dead`, `vacuum_count_snapshot`, `vacuum_agg_stress`, `col_vacuum_scan`, `topn_vacuum_scan` |
| 2 | `gate-bm25` | user instructions per BM25 operation against `RELEASE_BM25_BASE` (`bench/steps/cpu_gate.sh`); a cell above 1.02 fails unless it is in `RELEASE_BM25_ACCEPT` (`session`, `vacuum_nofree`, `idle1_rank`, documented in the README) |
| 2 | `gate-col` | user instructions per question of the store (`bench/cpu-query.sh`, 100 000 rows) on a `sift_col` index against `RELEASE_COL_REF` (`baab1a2` for 3.0); above `RELEASE_COL_MAX` (1.15) fails, between 1.02 and 1.15 is printed as `accepted: size work`. The `sift_table` ratios are printed, not judged (the grouped scan on `title` is 1.209 on purpose) |
| 2 | `fuzz` | 60 s each of the column chunk and BM25 decoders (`scripts/fuzz.sh colchunk bm25`) |
| 2 | `coldiff` | the aggregate pushdown against PostgreSQL's own plan: the distinct, pre-aggregate, top-N, search, set and table fixtures with every scenario |
| 2 | `sqlite`, `mariadb` | the shared core still builds and answers in the two hosts (smoke only; MariaDB one series) |
| 3 | `upgrade` | 2.0 files in a cluster with a BM25 index are replaced by 3.0, `ALTER EXTENSION UPDATE TO '3.0'`, the answers are the same, `sift.verify` and REINDEX pass, a `sift_col` index is created; then a column index written by the last head with column format 2 (`RELEASE_MINOR2`) answers with a REINDEX hint, and after REINDEX with the same result |
| 3 | `package` | `make install DESTDIR=...`, then `CREATE EXTENSION` and both index types in a fresh cluster that sees only the installed files |

After every job the logs it left are searched for a server crash (`terminated by signal` 4, 6, 7, 8 or 11, and `TRAP:`); signals 9 and 15 are what
the crash tests send on purpose. A crash fails the job.

## Why these jobs

The base of 3.0 was gated in full. What changed since is the aggregate source and planner (top-N, pre-aggregates, distinct bitmaps, filters in
the source), the BM25 visibility paths, the column format (minor 3), the build layout (rename, folders) and the test and bench scripts. The
jobs are the ones that fail when one of these breaks: the quick suites on three builds, the races, the two instruction gates (which do not
depend on the load of the host), the decoders whose format moved, the differential test of what was pushed down, the upgrade of what is on disk.

## Why the column gate allows 1.15

Compressed dictionaries and the column format minor 3 trade some CPU for size, and the user accepted that for CPU-bound questions. A `sift_col`
question up to 1.15 of the reference is therefore printed, not failed; anything above needs a cause. The BM25 gate keeps 1.02: the BM25
format did not change.

## Failures

A failed job runs once more alone (a CI job: only the parts that failed). It passes the gate only if it then passes and every failed part has a
line in `scripts/release.flakes` (regex, tab, cause); a second failure stops the run and no stamp is written. A flake is fixed with a test that
fails first, or explained in that file; it is not skipped.

## The stamp

The key is the hash of the committed `src/` (all of it, whatever its layout), `test/`, `scripts/`, `Makefile`, `pg_sift.control` and the
`pg_sift--*.sql` scripts. A head with a stamp (`$RELEASE_STATE`, default `~/.local/state/pg_sift-release/KEY`) is reported as gated. A commit
that touches only docs, bench or packaging keeps the key; any change under the paths above gives a new key and a new gate. Uncommitted changes
in those paths stop the run (the gate runs on a clone of HEAD).

## Cost and limits

The jobs run one at a time in a scope of 2 cores and 12 GB (`systemd-run`, `CPUQuota=200%`), at most 100 000 rows, with `nice`. The scratch is
under `$RELEASE_TMP` (default `/tmp`) and removed at the end except the logs, which are in `sift-release.*/logs` (`NAME.log`, and
`NAME.first.log` for a job that ran twice, and `NAME.scratch.N/` with the logs, diffs and findings of every failed attempt). Not in the gate: the full CI, the full differential test, PG18 full, the long soaks and fuzz
runs, repeated runs of flaky tests, the full SQLite and MariaDB suites, and the timed benchmarks (`bench/analytics --final`).

Environment: `RELEASE_BM25_BASE` (544ddf0, the main the 3.0 branch started from), `RELEASE_BM25_ACCEPT`, `RELEASE_COL_REF` (a gated ref;
default the head of the newest stamp), `RELEASE_COL_MAX` (1.15), `RELEASE_COL_NOTE` (1.02), `RELEASE_OLD` (v2.0.3), `RELEASE_MINOR2` (46008df), `RELEASE_MARIADB_SERIES` (11.8),
`RELEASE_TMP`, `SIFT_PORT` (first port, 56130).
