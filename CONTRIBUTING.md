# Contributing

pg_sift needs PostgreSQL 17 or 18 (19 is experimental) with the server headers (`pg_config` on `PATH`) and gcc or clang.

```sh
make                                # builds pg_sift.so; PG_CONFIG=/path/to/pg_config picks another installation
make installcheck                   # regression tests against a running server that can load pg_sift
make check                          # quick tier: every suite in throwaway clusters, about 1.5 minutes on 8 cores, 2.5 on 4
scripts/ci.sh affected              # the same, only the jobs your diff against origin/main (with the working tree) can break (scripts/areas.tsv); use it before a push
make check-full                     # full tier: ASan build, long soak and fuzz runs, all mutants
```

`scripts/pgtest.sh install && scripts/pgtest.sh start` gives a throwaway cluster (ports 56100-56299, nothing
installed system-wide); `scripts/pgtest.sh psql` connects to it and `scripts/pgtest.sh destroy` removes it.
The gate runs the PostgreSQL of `pg_config` on `PATH` only, and GitHub CI also runs 17: PostgreSQL 17 installs sift into the server's own directories (`SIFT_SUDO` when they are not writable), so run it on a private copy of the installation
(`cp -a /opt/pgsql-17 /tmp/pg17`, the `postgresql-old-upgrade` package of Arch) with `PATH=/tmp/pg17/bin:$PATH SIFT_TMP=/tmp/sift-pg17 SIFT_PORT=56180 scripts/ci.sh affected`, or one job of it with `SIFT_CI_JOBS`. That build has no Perl TAP modules: the `tap-*.pl` jobs need a server built with `--enable-tap-tests`.
`make check` cuts the suites into jobs that one lane per core (at most 8, `SIFT_LANES=N`) takes from a queue; `SIFT_CI_JOBS=REGEX`
runs just the jobs whose name matches (`SIFT_CI_JOBS='installcheck-facets|tap-001' make check`).
`scripts/coldiff.sh` is the differential test of the aggregate pushdown, plain SQL aggregates with `sift.enable_agg` off and on, the results compared,
with fixtures, scenarios and a self-test (also part of `make check`, docs/AGG.md).
The scratch directory (`$TMPDIR/sift-ci-PID`, `SIFT_CI_TMP` picks another) peaks at about 1.1 GB (the same with `SANITIZE=1`; `CI_BUDGET_MB=900` brings it to 1 GB, at the price of about a quarter more time, as the two big WAL tests then run one after the other): a lane starts a job when
the estimated scratch of the running ones plus its own fits `CI_BUDGET_MB` (`EST_MB` in `scripts/ci.sh`), and a job's data goes when it passes.
`SIFT_CI_STATS=1 make check` prints the peak by component (WAL, tables, temp files), lane and job, and the memory of the servers; run it when
you add a heavy test and give the job an estimate. The clusters are small (1 MB WAL segments, 48 MB `max_wal_size`, 32-64 MB buffers); the
crash, recovery and replication tests (`WAL_CONF` in `test/t/lib.bash`, `tap.sh` for the Perl ones) keep the default WAL segments and
settings, since the WAL is what they test.
`scripts/replication.sh` (also part of `make check`) tests physical and logical replication on temporary clusters;
`scripts/replication.sh --container` runs it with every node in its own docker container (`PG_IMAGE`, default `postgres:18`).
`SIFT_TESTING=1` (the default for `pgtest.sh` and `make check`) also builds the test-only `pg_sift_test` extension and the
`sift._test_*` functions; a normal `make install` never installs them. Switching it needs `make clean`.
The regression tests listed in `TESTING_REGRESS` (Makefile) need those functions and are skipped by a release build; every other one
(but the slow ones of `SLOW_REGRESS` in `scripts/ci.sh`) runs twice in `make check`, against the testing build and against the release library (`SIFT_TESTING` unset, built in a copy of the tree,
job `release-library-regress`, `scripts/release-library-check.sh`). Tests of limits, privileges and the planner belong in the second group (do not use
`sift._test_*` or the `sift.test_*` settings in them), so that they run against the library users get. `SIFT_TESTING= scripts/pgtest.sh install`
builds it by hand.

Every suite runs under one leak gate, `scripts/leakcheck.sh`, without any test referring to it: the server logs of every
cluster and node are scanned for what PostgreSQL logs when a transaction or backend leaks (buffer, relcache, TupleDesc,
snapshot, lock and file references, `you don't own a lock`), for assertion failures, crashes and sanitizer reports; a
`SANITIZE=1` server also reports malloc leaks as each process exits (LeakSanitizer, `scripts/lsan.supp`); every cluster is
checked when it stops (no session left in a transaction or holding locks, no prepared transaction, no temporary file left,
no long-lived process grown); the soak fails on a backend whose memory keeps growing. `make check` ends with the scan of
everything it ran. A test that crashes or leaks on purpose declares it with `scripts/leakcheck.sh expect COUNT REGEX`.

The SQLite extension in `sqlite/` is tested on its own and needs no PostgreSQL: `sqlite/test.sh` (about 40 s; `SIFT_SANITIZE=1 sqlite/test.sh` adds
ASan/UBSan and the TSan stress test) needs `cc`, `make`, the SQLite development files (3.38 or newer) and python3 with loadable `sqlite3`
extensions and FTS5; `make -C sqlite isa-check` proves the baseline objects use no POPCNT or vector instruction, and `scripts/package-sqlite.sh`
builds and tests the release tarball in containers. It compiles the portable core of `src/` (the files of `src/portable.list`)
unmodified against the host implementation in `sqlite/host/`.

**Portable files include only the host header.** A file of `src/portable.list` includes the C library, compiler intrinsics, other files of the list and
`src/core/sift_host.h`, never a PostgreSQL, SQLite or other host header: new code there uses the vocabulary of `sift_host.h` (documented at the top of that file) and, when
it needs something the host must provide, adds it to that vocabulary and to every host (`sqlite/host/host.h`), not a `#include`. `scripts/check-portable.sh` fails
a file that breaks the rule; it is a stage of `scripts/ci.sh` (so of `make check`) and the first step of `sqlite/test.sh` (the SQLite job). A change to a portable file
that uses another host API or a global mutable variable needs the host or the Makefile to follow, so run `sqlite/test.sh` as well as `make check` when you touch
them. Code shared by two hosts goes into the portable core, not into both. `make check` and `scripts/ci.sh` never
build or run `sqlite/`. Its changes are listed in [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md) and its version is `sqlite/VERSION`, which must equal the
release tag.

`src/` has one folder per area. A source includes another by its bare name, and a new file goes to the folder its name selects (`scripts/restructure-src.sh --check` fails for a name no rule places: add the pattern to its MAP).

`scripts/check-neutral.sh` fails a tracked file or a commit subject of the branch that holds a product-specific name: data, tests and docs use generic names (fact table, entities, `dim_a_id`, `group_members`). `scripts/ci.sh affected` runs it first, whatever the diff touches.

Before sending a change:

- run `make check` and add a regression test (`test/sql/NAME.sql` plus `test/expected/NAME.out`) or a TAP test
  (`test/t/`) for a fix or a new behavior. For a bug, write the test first and see it fail before you fix it;
- keep the build free of compiler warnings;
- start the commit subject with the module or feature it changes: `query: parse fuzzy terms with a distance above 2`.

Contributions are licensed under the [PostgreSQL License](LICENSE) like the rest of the project.

## Releasing

Write `docs/releases/vX.Y.Z.md` from `docs/releases/TEMPLATE.md`, keep the notes of the release under `## Unreleased` of the CHANGELOGs, push `main`, then
`scripts/release.sh X.Y.Z --dry-run` and `scripts/release.sh X.Y.Z`: it takes the green full CI run of the commit, checks the upgrade from the previous release and
`AUR_SSH_KEY`).
