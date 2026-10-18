# Contributing

pg_sift needs PostgreSQL 17 or 18 (19 is experimental) with the server headers (`pg_config` on `PATH`) and gcc or clang.

```sh
make                                # builds pg_sift.so; PG_CONFIG=/path/to/pg_config picks another installation
make installcheck                   # regression tests against a running server that can load pg_sift
make check                          # quick tier: every suite in throwaway clusters, about 1.5 minutes on 8 cores, 2.5 on 4
make check-full                     # full tier: ASan build, long soak and fuzz runs, all mutants
```

`scripts/pgtest.sh install && scripts/pgtest.sh start` gives a throwaway cluster (ports 56100-56299, nothing
installed system-wide); `scripts/pgtest.sh psql` connects to it and `scripts/pgtest.sh destroy` removes it.
`make check` cuts the suites into jobs that one lane per core (at most 8, `SIFT_LANES=N`) takes from a queue; `SIFT_CI_JOBS=REGEX`
runs just the jobs whose name matches (`SIFT_CI_JOBS='installcheck-facets|tap-001' make check`).
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
`src/sift_host.h`, never a PostgreSQL, SQLite or other host header: new code there uses the vocabulary of `sift_host.h` (documented at the top of that file) and, when
it needs something the host must provide, adds it to that vocabulary and to every host (`sqlite/host/host.h`), not a `#include`. `scripts/check-portable.sh` fails
a file that breaks the rule; it is a stage of `scripts/ci.sh` (so of `make check`) and the first step of `sqlite/test.sh` (the SQLite job). A change to a portable file
that uses another host API or a global mutable variable needs the host or the Makefile to follow, so run `sqlite/test.sh` as well as `make check` when you touch
them. Code shared by two hosts goes into the portable core, not into both. `make check` and `scripts/ci.sh` never
build or run `sqlite/`. Its changes are listed in [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md) and its version is `sqlite/VERSION`, which must equal the
release tag.

Before sending a change:

- run `make check` and add a regression test (`test/sql/NAME.sql` plus `test/expected/NAME.out`) or a TAP test
  (`test/t/`) for a fix or a new behavior;
- keep the build free of compiler warnings;
- start the commit subject with the module or feature it changes: `query: parse fuzzy terms with a distance above 2`.

Contributions are licensed under the [PostgreSQL License](LICENSE) like the rest of the project.

## Releasing

1. Write `docs/releases/vX.Y.Z.md` from `docs/releases/TEMPLATE.md`, retitle `## Unreleased` in `CHANGELOG.md` and `sqlite/CHANGELOG.md`, and
   set `pkgver` in `packaging/PKGBUILD`, the install lines of the README and `sqlite/VERSION`.
2. `scripts/ci.sh quick` and `SANITIZE=1 scripts/ci.sh quick` pass; push `main` and wait for CI.
3. Tag `vX.Y.Z` and push the tag: the `release` workflow builds the `.deb`, `.rpm` and SQLite tarball and publishes them with the notes.
   `AUR_SSH_KEY`). Every release goes to the AUR.
