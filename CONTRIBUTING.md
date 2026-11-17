# Contributing

pg_sift needs PostgreSQL 17, 18 or 19 with the server headers (`pg_config` on `PATH`) and gcc or clang.

```sh
make                                # builds pg_sift.so; PG_CONFIG=/path/to/pg_config picks another installation
make installcheck                   # regression tests against a running server that can load pg_sift
make check                          # quick tier: every suite in throwaway clusters, about 1.5 minutes on 8 cores, 2.5 on 4
scripts/ci.sh affected              # the same, only the jobs your diff against origin/main (with the working tree) can break (scripts/areas.tsv); use it before a push
scripts/ci.sh affected --focus      # while developing: of the changed functions of src/, the regression tests that execute them (scripts/focus/); the full set is the push
make check-full                     # full tier: ASan build, long soak and fuzz runs, all mutants
```

`make check` and its scripts need bash 4 or newer: on macOS `brew install bash` and put it first on `PATH` (the stock bash 3.2 stops with `needs bash 4+`). The memory checks of the leak gate (`/proc`) and the CPU column of `scripts/ci.sh --times` are Linux only. A host that cannot run a stage says so and skips it: the Perl TAP tests (Homebrew's PostgreSQL has no `src/test/perl`), the compatibility tests that build a release older than 3.2.0 (x86-64 only) and the benchmark generators (Python 3.12).

A pull request that changes a source that starts parallel workers or handles snapshots (the files `ASSERT_MARK` of `scripts/ci/affected.sh` matches, e.g. `src/agg/par/worker.c`) runs the regression tests that run those paths on a server built with assertions (`ASSERT_JOBS`, about 27 tests, a few minutes) beside its other checks; the label `assertions` runs the same for any pull request. Say in the pull request text that the family passed on an assertions build (that job or `make pg-assert` locally).

`scripts/pgtest.sh install && scripts/pgtest.sh start` gives a throwaway cluster (ports 56100-56299, nothing installed system-wide); `scripts/pgtest.sh psql` connects to it and `scripts/pgtest.sh destroy` removes it.
The local gate runs only the PostgreSQL of `pg_config` on `PATH`; GitHub CI also runs 17. PostgreSQL 17 installs sift into the server's own directories (`SIFT_SUDO` when they are not writable), so run it on a private copy of the installation
(`cp -a /opt/pgsql-17 /tmp/pg17`, or the `postgresql-old-upgrade` package of Arch) with `PATH=/tmp/pg17/bin:$PATH SIFT_TMP=/tmp/sift-pg17 SIFT_PORT=56180 scripts/ci.sh affected`, or one job of it with `SIFT_CI_JOBS`. That build has no Perl TAP modules: the `tap-*.pl` jobs need a server built with `--enable-tap-tests`.
`make check` cuts the suites into jobs that one lane per core (at most 8, `SIFT_LANES=N`) takes from a queue; `SIFT_CI_JOBS=REGEX`
runs only the jobs whose name matches (`SIFT_CI_JOBS='installcheck-facets|tap-001' make check`).
`scripts/coldiff.sh` is the differential test of the aggregate pushdown: plain SQL aggregates with `sift.enable_agg` off and on, results compared,
with fixtures, scenarios and a self-test (also part of `make check`, docs/AGG.md).
The scratch directory (`$TMPDIR/sift-ci-PID`, `SIFT_CI_TMP` picks another) peaks at about 1.1 GB (the same with `SANITIZE=1`; `CI_BUDGET_MB=900` brings it to 1 GB at the price of about a quarter more time, as the two big WAL tests then run one after the other). A lane starts a job when
the estimated scratch of the running ones plus its own fits `CI_BUDGET_MB` (`EST_MB` in `scripts/ci/jobs.sh`), and a job's data goes when it passes.
`SIFT_CI_STATS=1 make check` prints the peak by component (WAL, tables, temp files), lane and job, and the memory of the servers; run it when
you add a heavy test, and give the job an estimate. The clusters are small (1 MB WAL segments, 48 MB `max_wal_size`, 32-64 MB buffers); the
crash, recovery and replication tests (`WAL_CONF` in `test/t/lib.bash`, `tap.sh` for the Perl ones) keep the default WAL segments and
settings, since the WAL is what they test.
`scripts/replication.sh` (also part of `make check`) tests physical and logical replication on temporary clusters;
`scripts/replication.sh --container` runs it with every node in its own docker container (`PG_IMAGE`, default `postgres:18`).
`SIFT_TESTING=1` (the default for `pgtest.sh` and `make check`) also builds the test-only `pg_sift_test` extension and the `sift._test_*` functions; a normal `make install` never installs them. Switching it needs `make clean`.
The regression tests in `TESTING_REGRESS` (Makefile; one name for each line, `TESTING_REGRESS += name`, in byte order inside its block: add a new test in its place, never at the end, so that branches adding tests merge without a conflict, `scripts/check-testing-regress.sh` checks it) need those functions and are skipped by a release build; every other one
(except the slow ones of `SLOW_REGRESS` in `scripts/ci/plan.sh`) runs against the testing build, and the deep tier runs it against the release library too (`SIFT_TESTING` unset, built in a copy of the tree,
job `release-library-regress`, `scripts/release-library-check.sh`; the quick tier runs a smoke of it and the tests that refer to a test-only name behind a guard). Tests of limits, privileges and the planner belong in the second group (do not use
`sift._test_*` or the `sift.test_*` settings in them), so they run against the library users get. `SIFT_TESTING= scripts/pgtest.sh install`
builds it by hand.

Every suite runs under one leak gate, `scripts/leakcheck.sh`, which no test refers to: the server logs of every
cluster and node are scanned for what PostgreSQL logs when a transaction or backend leaks (buffer, relcache, TupleDesc,
snapshot, lock and file references, `you don't own a lock`), for assertion failures, crashes and sanitizer reports; a
`SANITIZE=1` server also reports malloc leaks as each process exits (LeakSanitizer, `scripts/lsan.supp`); every cluster is
checked when it stops (no session left in a transaction or holding locks, no prepared transaction, no temporary file left,
no long-lived process grown); the soak fails on a backend whose memory keeps growing. `make check` ends with the scan of
everything it ran. A test that crashes or leaks on purpose declares it with `scripts/leakcheck.sh expect COUNT REGEX`.

The SQLite extension in `sqlite/` is tested on its own, without PostgreSQL: `sqlite/test.sh` (about 40 s; `SIFT_SANITIZE=1 sqlite/test.sh` adds ASan/UBSan and the TSan stress test) needs
`cc`, `make`, the SQLite development files (3.38 or newer) and python3 with loadable `sqlite3` extensions and FTS5. `make -C sqlite isa-check` proves the baseline objects use no POPCNT or
vector instruction; `scripts/package-sqlite.sh` builds and tests the release tarball in containers. It compiles the portable core of `src/` (the files of `src/portable.list`) unmodified
against the host implementation in `sqlite/host/`.

**Errors say what to do.** An ERROR with SQLSTATE 54000 (limit), 0A000 (unsupported) or 55000 (wrong state) carries an `errhint` or `errdetail`; no ERROR is XX000. A condition the code
rules out is `SIFT_INVARIANT(cond, "text")` (`core/host.h`, an `Assert` in an assert build); anything a user or the data can reach is an `ereport` with its own errcode (XX001/XX002 for
damaged data, 40001 for a retry). `scripts/check-errors.sh` enforces it in the lint job and `scripts/ci.sh`; a test-only hook outside a `*_test.c` file says `check-errors: allow` in a
comment on the call.

**An expected output holds no failed verdict.** A test that compares two plans or two engines prints the verdict in a column (`agree`, `same`, `equal`, `match`, `ok`, `identical`, `equivalent`, `passed`, or a name made of them); `scripts/check-expected-oracle.py` (the `expected-oracle` step of `scripts/lint.sh`) fails an `f` or an empty cell (a NULL: the comparison did not say yes) in such a column of `test/expected/*.out` and the word `FAIL`, `MISMATCH` or `DIFFERENT` in a result cell, because a file accepted with one pins the disagreement as the right answer. An `f` or NULL that is the answer (a function named `match` asked a question it must refuse, a case with nothing to compare) is listed in `scripts/expected-oracle.allow` with its reason and its exact number of cells.

**Messages have a row.** Every NOTICE, WARNING and INFO of `src/` and `sql/` is one row of `docs/messages.tsv` (the exact text, where and when, the decision, its benefit and trade-off with numbers quoted from `docs/options.tsv` or a cited file, how to switch it off), and `docs/MESSAGES.md` is generated from the rows. `scripts/gen-messages.sh --skeleton` prints the row of a message that has none, `scripts/gen-messages.sh` writes the page, and `scripts/gen-messages.sh --check` (lint) fails on a message without a row, a row without a message, a changed text, DETAIL or HINT, and a number that is in no cited measurement.

**No source file passes 1000 lines.** A longer C file, header, script, SQL or Python file is split into a directory of context files (`src/<area>/<context>/*.c` and a small private header), a pure move first, then the change. `scripts/check-file-size.sh` enforces it in the lint job; documents, expected outputs and result tables are not counted, and the full install scripts of released minors (`sql/pg_sift--X.Y.sql`, frozen) are the one allowance.

**SQL changes of the release in progress go in `sql/<version>/NN-<context>.sql`** (for example `sql/5.0/`, one file for each feature area, a two-digit number that is unique in the directory), never in the generated `sql/pg_sift--<from>--<version>.sql`: the Makefile builds it from the parts in `LC_ALL=C` order (`scripts/sql-parts.sh`, run by `make`, `make install`, `scripts/release.sh` and `scripts/gen-reference.sh`), and git ignores it. A release freezes the generated script as a tracked file and removes its parts.

**Portable files include only the host header.** A file of `src/portable.list` includes the C library, compiler intrinsics, other files of the list and
`src/core/host.h`, never a PostgreSQL, SQLite or other host header: new code there uses the vocabulary of `core/host.h` (documented at the top of that file) and, when
it needs something from the host, adds it to that vocabulary and to every host (`sqlite/host/host.h`), not a `#include`. `scripts/check-portable.sh` fails
a file that breaks the rule; it is a stage of `scripts/ci.sh` (so of `make check`) and the first step of `sqlite/test.sh` (the SQLite job). A change to a portable file
that uses another host API or a global mutable variable needs the host or the Makefile to follow, so run `sqlite/test.sh` as well as `make check` when you touch
them. Code shared by two hosts goes into the portable core, not into both. Every file of `src/` is in `src/portable.list`, `src/portable-col.list` or `src/pg-only.list` (`scripts/check-classified.sh`, in lint): a new file is classified when it is added. `make check` and `scripts/ci.sh` never
build or run `sqlite/`. Its changes are listed in [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md); its version is `sqlite/VERSION`, which must equal the
release tag.

`src/` has one folder per area. Sources include each other by path from `src/` (`#include "bm25/query.h"`); a file is named for what it holds, without the `sift_` prefix or its folder's name, and goes to the folder of its area (`scripts/restructure-src.sh --check` fails for a file outside them or a stale path).

`scripts/check-neutral.sh` keeps the repository generic. It fails a tracked file (name or text), a commit of the branch (subject or body) or the title or body of the pull request that holds a name of the private vocabulary (product, machine and application-domain names; it is read from `NEUTRAL_VOCAB_FILE` or the `NEUTRAL_VOCAB` secret, and without it only the structural rules run), a name that the naming vocabulary of `test/README.md` replaced, an address of a private range, a local domain name or the home directory of a person. Data, tests, benchmarks and docs use generic names (orders, events, docs, facts, `dim_a_id`, `tenant_id`). A generic use of a listed word goes into the private vocabulary with a path, a text pattern and the reason; the inet and cidr test data may use `10.0.0.x` and `192.168.x.x` (`scripts/neutral.allow`). `scripts/ci.sh affected` runs the guard first, whatever the diff touches; `.github/workflows/neutral.yml` runs it on every pull request, also when only the title or body is edited (they reach the script through the environment).

Before sending a change:

- run `make check` and add a regression test (`test/sql/NAME.sql` plus `test/expected/NAME.out`) or a TAP test
  (`test/t/`) for a fix or a new behavior. For a bug, write the test first and see it fail before you fix it;
- keep the build free of compiler warnings;
- start the commit subject with the module or feature it changes: `query: parse fuzzy terms with a distance above 2`.

Contributions are licensed under the [PostgreSQL License](LICENSE) like the rest of the project.

## Writing the catalog

sift never writes a catalog row, and never takes a transaction id that it stamps into a structure other sessions read with MVCC rules, where it runs restricted: in a lazy `VACUUM` (manual or autovacuum's) or in a parallel operation. A `VACUUM` runs with `PROC_IN_VACUUM`: `GetSnapshotData()` leaves its transaction id out of every snapshot, so a session that reads a row the `VACUUM` wrote takes it for an aborted writer and hints the old version as not updated (the row is there twice, or its update is lost); a parallel operation cannot assign an id at all. A build (`CREATE INDEX` with or without `CONCURRENTLY`, `REINDEX`) and the maintenance workers are not restricted and write the catalog as they need.

The writers are the functions of `src/core/restrict.c` (`sift_cat_update`, `sift_cat_insert`, `sift_cat_delete`, `sift_depend`, `sift_undepend`, `sift_create_trigger`, `sift_drop_object`, `sift_create_index`, `sift_create_matview`, `sift_change_owner`), and each calls `sift_catalog_write_guard` first: an error in a release build, an assertion with `cassert`. `scripts/check-catalog-writes.sh` (a lint check) fails a file of `src/` that calls the PostgreSQL function itself. To add a writer, add a function to `restrict.c` that guards and calls it. A decision that a `VACUUM` reaches and that ends in a catalog write asks `sift_catalog_write_ok()` and leaves the choice to the maintenance worker (`maint_decide_wanted`, docs/TUNING.md). The one write a `VACUUM` may do is `vac_update_relstats`, which updates in place and takes no id. `sift._test_catalog_write` (testing build) runs each writer as a `VACUUM` would (`test/t/col_catalog_write_restricted.sh`); `test/t/col_vacuum_catalog_xid.sh` stops a real `VACUUM` at its cleanup gate and looks at `pg_class`.

Every lock `src/` takes (`LockRelation*`, `LockPage`, `LockTuple`, `LockDatabaseObject`, `LockAcquire`, a relation opened above `RowExclusiveLock`, an LWLock, a wait for a transaction) has a row in `scripts/locks.tsv`: call site, mode, object, scope, longest hold, the lock PostgreSQL takes for the same kind of work, and the verdict. `scripts/check-locks.sh` (a lint check) fails a lock without a row, a row without a lock, a release whose mode is the result of a call, and an allocation, a report, a wait or a lock between `SpinLockAcquire` and `SpinLockRelease`; `scripts/check-locks.sh --list` prints the acquisitions to copy a new row from. A lock that keeps INSERT, UPDATE, DELETE or SELECT out for longer than a page update is a defect to fix, not a row to add. `test/t/lock_modes.sh` stops each maintenance operation inside its work at a test gate and asserts from `pg_locks` that it holds no lock that conflicts with `RowExclusiveLock` on the table or its indexes, that the writers and readers do not wait, and that no log says `you don't own a lock`; add an operation as a row of `CASES`.

## The relation extension lock

PostgreSQL forbids every other heavyweight lock while the relation extension lock is held (an assertion with `cassert` since 19), and any `ereport` under it runs the error context of the index and the damage record, which look names up. sift takes the lock, and extends a relation under it, only through `sift_extension_lock` and `sift_extend_zeroed_to` in `src/core/extlock.c`, which resolve the lines of the error contexts into their scopes first (`sift_errctx_freeze`: a lookup under the lock misses whenever the cache does, and a `CATCACHE_FORCE_RELEASE` build misses every time); `scripts/check-extlock.sh` (a lint check) fails a file of `src/` that calls the PostgreSQL function itself. A new report that looks something up (a name, an access method) belongs in that resolution, not in a guard at the call site. Only the append of a value store holds the lock across a write (the one lock that parallel build members do not share); every other write extends through `sift_extend_zeroed_to`. `sift._test_raise_extending` raises each class of `core/damage.h`, and what is no damage, under the lock with a cold cache, with and without an extension under it (`test/sql/extlock_errors.sql`); the `assert` lane (`scripts/assert.sh`) runs it and the value store tests.

## Removing a deprecated option

The options that a release deprecates (`order_columns`, `point_columns` and `key_cut_rows`, deprecated in 4.3 and ignored since 5.0) go in one commit each: the setting that honors them in tests, the regression tests that name them (`test/sql/`, `grep -l`), the bench cases that name them (`bench/cases/`), the advice rows of `docs/messages.tsv` and the preflight rows of `sql/5.0/27-upgrade-check.sql` (`sift.upgrade_check`) and the option rows of `sift.upgrade_commands()` in `sql/`. The name stays registered, accepted and ignored with the NOTICE `reloption-ignored`, so that a stored option, a dump and an `ALTER INDEX` of another option keep working; `test/sql/col_removed_options.sql` covers it. A test that checks a layout only the removed option wrote is deleted with it; a test that only passed the option drops the argument and keeps its answers.

## Memory analysis

Every suite can write a census of the memory each statement keeps (`SIFT_MEMCENSUS=on`, test builds), the valgrind lane takes `SIFT_VALGRIND_LEAKS=1`, and `scripts/valgrind.sh --massif` names the allocation
sites that grow with the rows of the slope cases; `scripts/mem-report.py` ranks the artifacts. A memory slope that fails comes with the `growth_CASE.csv` of its case: fix the site at the top, then turn its
row of `test/mem-slope.tsv` (the kinds leakx, cap, evict and pinned: `test/mem-slope-legacy.tsv`; the memory of a merge against the rows: `test/t/col_merge_exact_memory.sh`) to `clean`.

## Memory budgets

A main path declares the memory it may hold, and every test that runs it asserts it (test build only; `docs/SIZING.md` "Memory budgets"). The path gets its own named context, `SIFT_MEM_SCOPE(name, context)` at its entry and `SIFT_MEM_CHECKPOINT()` at each batch boundary (a run, a unit, a group, a merge pass, a statement end), both from `core/membudget.h`. The budget is a row of `test/mem-budgets.tsv`: a formula of settings (`maintenance_work_mem`, `work_mem`, `hash_mem`, `sift.*` limits) plus a constant, never a number of rows. `scripts/gen-membudget.py` makes `src/core/membudget_table.h` and the table of `docs/SIZING.md` from it, and `scripts/check-membudget.sh` (in `scripts/lint.sh`) fails on a scope without a row, a row without a scope, and a required path of `test/mem-required-scopes.tsv` in state `scoped` without its scope.

To add a scope: put it in the path, add its rows (budget table, required list), take the peak from the `SIFTMEM_SCOPE name=.. peak=.. budget=..` line of the server log (or `sift._test_mem_highwater()`) of the tests that run the path, and set the constant to that peak plus 25% for the fixed overhead. A path that holds more than its intended formula today is an `open` row: no formula, the constant is the ceiling measured today, `intended` names the formula it flips to, and `scripts/gen-membudget.py --open` lists it in the CI report. When the lane that fixes it lands, the integration step replaces the row by `closed` with the formula of `intended` and a constant from the new peak, and runs `test/t/mem_budget.sh` and the tests of the path. `sift.test_mem_budget_scale` (0.0001 to 1000) multiplies every budget at a checkpoint: the TAP test `mem_budget` proves each class fails at a lowered budget.

## Merging

`scripts/land.sh PR` lands one pull request: it merges the tip of the PR's base into the branch (conflicts only in lists both sides add to, in the CHANGELOG and in generated files are resolved; any other conflict is exit 2, `SOURCE CONFLICT`: merge the tip yourself), runs `scripts/lint.sh`, the privacy gate and the check that a new regression test has a file `scripts/focus/NAME.tsv`, pushes only when needed, waits for the required checks (`gate` and `neutral`, or the branch protection list) and merges by rebase, or by squash when the branch holds a merge commit. Exit codes: 0 merged, 1 required checks red, 2 source conflict, 3 merge commit failed, 4 a gate is red, 5 the PR deletes files (`ALLOW_DELETE=1` after checking they are its own), 6 closed, 75 try again (checks pending, the tip or the branch moved). `--dry-run` prints the plan and pushes and merges nothing. Landings take turns on one kernel lock (`land.lock`), which goes away with its holder however it ends; nothing matches process names. State and settings are outside git, in `$XDG_STATE_HOME/pg_sift/land` (`LAND_DIR`): the lock, `MERGED.md`, and `privacy.deny`, the private list of names the privacy gate refuses (one regular expression per line; `LAND_DENY` names another file; without it the gate fails closed). `scripts/land.sh --selftest` is part of `scripts/lint.sh`.

## Releasing

Write `docs/releases/vX.Y.Z.md` with what is about the release (the sections up to Compatibility, from the notes of the previous release), run `scripts/release.sh X.Y.Z --notes` to append the Packages, SQLite extension and MariaDB extension sections that the release workflow checks (`scripts/release-components.tmpl`: the same text in every patch release, the version filled in; edit the file after for a release that changes them), keep the release notes under `## Unreleased` of the CHANGELOGs, push `main`, then
`scripts/release.sh X.Y.Z --dry-run` and `scripts/release.sh X.Y.Z`. It takes the green full CI run of the commit, checks the upgrade from the previous release and
`AUR_SSH_KEY`).
